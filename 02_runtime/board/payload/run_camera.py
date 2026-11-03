"""Foreground service owner. Zero limits explicitly mean continuous operation."""
from pathlib import Path
import argparse,datetime,fcntl,hashlib,json,os,signal,subprocess,sys,time,shutil
from runtime_log import PipeLog
from session_retention import maintain_sessions,prepare_session
ROOT=Path(__file__).resolve().parent
RUNTIME={'ICRAFT_VERSION':'v3.39.0','LD_LIBRARY_PATH':'/root/fpga_gaming/m7_20261002_01/runtime_339/usr/lib/aarch64-linux-gnu:/root/fpga_gaming/m7_20261002_01/runtime_339/ModelzooDeps/aarch64/Shared/lib'}
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,data):
 t=p.with_suffix(p.suffix+'.tmp');t.write_text(json.dumps(data,ensure_ascii=False,indent=2));os.replace(t,p)
def verify_payload():
 manifest=json.loads((ROOT/'PAYLOAD_SHA256.json').read_text())
 for name,expected in manifest.items():
  if digest(ROOT/name)!=expected:raise RuntimeError('Payload differs: '+name)
 return manifest
def select_camera():
 choices=[]
 for p in sorted(Path('/sys/class/video4linux').glob('*')):
  if 'RealSense' not in (p/'name').read_text():continue
  q=subprocess.run(['v4l2-ctl','--list-formats-ext','-d','/dev/'+p.name],capture_output=True,timeout=5)
  if q.returncode==0 and "'YUYV'" in q.stdout.decode(errors='replace'):choices.append('/dev/'+p.name)
 if len(choices)>1:raise RuntimeError('Multiple D435i color interfaces: '+repr(choices))
 return choices[0] if choices else None
def trim_sessions():
 # Startup history is bounded by boot identity; a live device owner is retained.
 maintain_sessions(ROOT)
def main():
 a=argparse.ArgumentParser();a.add_argument('--camera',default='auto');a.add_argument('--seconds',type=int,default=0);a.add_argument('--frames',type=int,default=0);a.add_argument('--video-frames',type=int,default=1200);a.add_argument('--camera-wait',type=int,default=180);a.add_argument('--qualification-probe',action='store_true');a.add_argument('--gap-test',action='store_true');a.add_argument('--capture-fps',type=int,choices=[15,30],default=30);a.add_argument('--service-rate',type=float,default=0);args=a.parse_args()
 if not(0<=args.seconds<=86400 and 0<=args.frames<=2000000 and 0<=args.video_frames<=1200):a.error('Zero means continuous; finite bounds86400s/2000000frames; video0..1200')
 fd=os.open(str(ROOT/'launcher.lock'),os.O_CREAT|os.O_RDWR,0o600);fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
 boot=Path('/proc/sys/kernel/random/boot_id').read_text().strip();stop=False;out=None
 def onstop(*_):
  nonlocal stop;stop=True
 signal.signal(signal.SIGTERM,onstop);signal.signal(signal.SIGINT,onstop)
 def phase(value,**kw):save(ROOT/'APP_STATE.json',dict(phase=value,boot=boot,updated_monotonic_s=time.monotonic(),**kw))
 try:
  phase('STARTING');payload=verify_payload()
  if not args.qualification_probe and not json.loads((ROOT/'RELEASE_ACCEPTANCE.json').read_text()).get('accepted'):raise RuntimeError('Release not accepted')
  if (ROOT/'UNKNOWN.json').exists():raise RuntimeError('Prior device session retained; inspect UNKNOWN.json')
  trim_sessions();start=time.monotonic();camera=None
  while not stop:
   camera=select_camera() if args.camera=='auto' else args.camera
   if camera:break
   phase('WAITING_CAMERA',reason='Waiting for D435i YUYV color interface',waited_s=time.monotonic()-start)
   if time.monotonic()-start>=args.camera_wait:raise RuntimeError('Camera readiness deadline exceeded; reconnect then authenticated Start')
   time.sleep(1)
  if stop:phase('STOPPED',reason='Stopped before opening NPU');return 0
  name='camera_'+datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')+'_b'+boot.replace('-','');out=ROOT/'sessions'/name;prepare_session(out,boot)
  session=dict(mode='camera',max_forwards=args.frames,seconds=args.seconds,video_frames_max=args.video_frames,capture_fps=args.capture_fps,service_rate_hz=args.service_rate)
  if args.gap_test:session['gap_after_s']=10
  save(ROOT/'contract.json',dict(boot=boot,runtime_env=RUNTIME,protected_files={},payload_SHA=payload,camera_device=camera,sessions={name:session}))
  save(ROOT/'ACTIVE_RUN.json',dict(session=name,boot=boot,model='11',camera_device=camera,seconds=args.seconds,max_forwards=args.frames,continuous=args.seconds==args.frames==0,capture_fps=args.capture_fps,service_rate_hz=args.service_rate))
  worker=subprocess.Popen([sys.executable,'-B','-u',str(ROOT/'worker.py'),name],cwd=str(ROOT),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  logs=[PipeLog(worker.stdout,ROOT/'worker.stdout'),PipeLog(worker.stderr,ROOT/'worker.stderr')];phase('ACTIVE',session=name)
  while worker.poll() is None:
   if stop:
    phase('STOPPING',session=name,reason='Waiting for inference and SDK close')
    if out.is_dir():(out/'STOP').write_text('Cooperative service stop')
   time.sleep(.5)
  for log in logs:log.close()
  phase('STOPPED' if worker.returncode==0 else 'FAILED',session=name,exit_code=worker.returncode);return worker.returncode
 except Exception as e:phase('FAILED',reason=str(e));print(str(e),file=sys.stderr,flush=True);return 1
 finally:os.close(fd)
if __name__=='__main__':sys.exit(main())
