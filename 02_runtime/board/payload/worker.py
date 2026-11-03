"""Continuous or finite RGB product session. Model11 candidate backend, no business reset.

Inference child is completion-acknowledged before PL commit. Unknown in-flight
device work retains its owner/lock; camera-only process may be stopped safely.
"""
import os,sys,json,math,time,hashlib,fcntl,ctypes,socket,struct,subprocess,threading,signal,csv,traceback
from pathlib import Path
import cv2,numpy as np
from collections import deque
from runtime_log import RollingText,PipeLog
from policy import boxes_q16,hit,expected,Ownership,fresh
from motion_tracker_m355 import MotionTracker
from mpeg4_writer_native_m388 import MPEG4Writer
from native_center_pack_m93 import pack
from pipeline import FileFrames,VideoSink
from renderer import RenderWriter
from media_fast import MediaSink
from observability import MAGIC,read_observability
ROOT=Path(__file__).resolve().parent
def need(x,s):
 if not x:raise RuntimeError(s)
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,x):
 temp=p.with_suffix(p.suffix+'.tmp');temp.write_text(json.dumps(x,ensure_ascii=False,allow_nan=False),encoding='utf-8');os.replace(str(temp),str(p))
def emit(x):print(json.dumps(x,ensure_ascii=False,allow_nan=False),flush=True)
def exact(ch,n):
 b=bytearray()
 while len(b)<n:
  d=ch.recv(n-len(b));need(bool(d),'NPU socket EOF');b.extend(d)
 return bytes(b)
class Hardware:
 def __init__(self,fd):
  self.lib=ctypes.CDLL(str(ROOT/'libswitch_mmio.so'));self.lib.switch_open.argtypes=[ctypes.c_int];self.lib.switch_read.argtypes=[ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32)];self.lib.switch_write.argtypes=[ctypes.c_uint32,ctypes.c_uint32]
  self.lib.switch_snapshot.argtypes=[ctypes.POINTER(ctypes.c_uint32)]
  self.lib.switch_snapshot.restype=ctypes.c_int
  need(self.lib.switch_open(fd)==0,'M512/lock mapping failed');self.writes=0
  self.observation_supported=self.read(0x40)==MAGIC
  self.observation={'supported':False,'reason':'OBS1 extension absent'}
  self.observation_at=float('-inf')
 def read(self,o):
  v=ctypes.c_uint32();need(self.lib.switch_read(o,ctypes.byref(v))==0,'MMIO read');return v.value
 def write(self,o,v):
  need(o in (4,8,12,16,0x38),'No reset/other write permitted');need(self.lib.switch_write(o,v)==0,'MMIO write');self.writes+=1
 def snapshot(self):
  values=(ctypes.c_uint32*7)()
  need(self.lib.switch_snapshot(values)==0,'Guarded MMIO snapshot')
  return dict(zip(('192','32','36','40','44','48','52'),values))
 def observer_state(self,force=False):
  # 事件出现或间隔1秒时读取；旧位流只探测一次。
  if not self.observation_supported:return self.observation
  now=time.monotonic()
  if force or now-self.observation_at>=1.0:
   self.observation=read_observability(self.read);self.observation_at=now
  return self.observation
 def commit(self,fid,boxes):
  before=self.snapshot();need(before['192']==0xa5050001,'Signature changed');need(before['44']==0 and before['48']==0,'Existing protocol rejection/error')
  e=expected(before,fid,boxes)
  if before['52']:self.write(0x38,before['52'])
  self.write(4,fid)
  for x,y,u,v in boxes:self.write(8,x|(y<<16));self.write(12,u|(v<<16))
  self.write(16,(len(boxes)<<8)|1);after=self.snapshot()
  need(after['36']==fid and after['40']==((before['40']+1)&0xffffffff) and after['44']==0 and after['48']==0,'PL commit acknowledgement mismatch')
  need(after['32']==e['status'] and after['52']==e['events'],'CPU/PL state mismatch')
  if after['52']:self.write(0x38,after['52'])
  return dict(before=before,after=after,CPU=e,exact=True,observability=self.observer_state(force=bool(after['52'])))
class NPU:
 def __init__(self,fd,c,out):
  self.journal=RollingText(out/'forward_journal.jsonl');self.calls=0;self.outstanding=False;self.p=None;self.ch=None;self.out=out
  parent,child=socket.socketpair();parent.settimeout(30);self.ch=parent
  env=dict(os.environ,**c['runtime_env']);env.update(M834_EXPECTED_BOOT=c['boot'],M834_GUARD_FD=str(fd),PRODUCT_MAX_FORWARDS=str(c['max_forwards']))
  self.logs=[]
  self.loading=True
  self.p=subprocess.Popen([str(ROOT/'product_stream'),str(ROOT/'int8.yaml'),str(child.fileno())],pass_fds=(fd,child.fileno()),stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env,cwd=str(ROOT));child.close();self.logs=[PipeLog(self.p.stdout,out/'npu.stdout'),PipeLog(self.p.stderr,out/'npu.stderr')]
  parent.settimeout(60);need(exact(parent,4)==b'RGB1','NPU protocol readiness');parent.settimeout(15);self.loading=False
 def infer(self,image):
  profile_begin=time.monotonic_ns()
  h,w=image.shape[:2];compact=image.shape==(1080,1920,3) and image.flags.c_contiguous
  raw=(pack(image) if compact else np.ascontiguousarray(image)).tobytes();index=self.calls
  # Record intent before any SDK work. Each immutable input still receives SHA256;
  # checksum computation overlaps the C++ forward instead of delaying its start.
  self.journal.write(json.dumps(dict(index=index,phase='PREPARED',width=w,height=h,input_bytes=len(raw)))+'\n');self.journal.flush()
  prepared=time.monotonic_ns()
  self.calls+=1;self.outstanding=True
  self.ch.sendall(struct.pack('<4I',0x33534750 if compact else 0x46504741,index & 0xffffffff,w,h)+raw)
  sent=time.monotonic_ns()
  input_sha=digest_bytes(raw)
  self.journal.write(json.dumps(dict(index=index,phase='INPUT_SHA256',input_SHA=input_sha,width=w,height=h))+'\n');self.journal.flush()
  hashed=time.monotonic_ns()
  magic,returned,n=struct.unpack('<3I',exact(self.ch,12));need(magic==0x50524c39 and returned==(index & 0xffffffff) and n<=300,'Invalid response header')
  times=struct.unpack('<11d',exact(self.ch,88));rows=[struct.unpack('<6f',exact(self.ch,24)) for _ in range(n)]
  self.outstanding=False
  detections=[dict(class_id=int(k),confidence=float(s),xyxy=[float(x),float(y),float(x)+float(u),float(y)+float(v)]) for k,x,y,u,v,s in rows]
  need(all(k==0 for k,x,y,u,v,s in rows),'Unknown class')
  self.profile=dict(prepare_hash_journal=(prepared-profile_begin)/1e6,hash_overlap=(hashed-sent)/1e6,send=(sent-prepared)/1e6,wait_decode=(time.monotonic_ns()-sent)/1e6,guard=times[3],receive_cpp=times[4],picture=times[5],tensor=times[6],dma=times[7],forward=times[8],reset=times[9],post=times[10])
  return detections,list(times[:3])
 def close(self):
  if self.outstanding or self.loading:return False
  if self.p is None:return True
  self.ch.sendall(struct.pack('<4I',0x46504741,0,0,0));self.ch.close();self.ch=None
  need(self.p.wait(timeout=20)==0,'NPU completion exit failed');[log.close() for log in self.logs];self.journal.close();return True
def digest_bytes(x):return hashlib.sha256(x).hexdigest()
class Latest:
 def __init__(self,device,out,fps):
  self.cv=threading.Condition();self.latest=None;self.error=None;self.count=0
  self.err=None
  self.p=subprocess.Popen([sys.executable,'-B','-u',str(ROOT/'capture.py'),device,str(fps)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,close_fds=True)
  self.err=PipeLog(self.p.stderr,out/'capture.stderr');self.t=threading.Thread(target=self.reader,daemon=True);self.t.start()
 def reader(self):
  def read(n):
   b=bytearray()
   while len(b)<n:
    x=self.p.stdout.read(n-len(b))
    if not x:raise RuntimeError('Camera producer stopped')
    b.extend(x)
   return bytes(b)
  try:
   while True:
    m,seq,ts,w,h,n=struct.unpack('<IQQIII',read(32));need(m==0x43414d31 and (w,h,n)==(640,480,921600),'Camera packet contract');raw=read(n)
    with self.cv:self.latest=(seq,ts,np.frombuffer(raw,np.uint8).reshape(h,w,3));self.count+=1;self.cv.notify_all()
  except Exception as e:
   with self.cv:self.error=str(e);self.cv.notify_all()
 def get(self,last,deadline):
  with self.cv:
   while self.latest is None or self.latest[0]<=last:
    if self.error:raise RuntimeError(self.error)
    t=deadline-time.monotonic()
    if t<=0:return None
    self.cv.wait(min(t,.2))
   return self.latest
 def close(self):
  if self.p.poll() is None:
   self.p.terminate()
   try:self.p.wait(timeout=3)
   except subprocess.TimeoutExpired:self.p.kill();self.p.wait(timeout=3) # camera only, no SDK/lock
  self.p.stdout.close();self.err.close()
def resource():
 def rss(pid):
  try:return int(next(x.split()[1] for x in Path('/proc/%d/status'%pid).read_text().splitlines() if x.startswith('VmRSS:')))
  except (OSError,StopIteration):return None
 return dict(parent_RSS_KiB=rss(os.getpid()),MemAvailable_KiB=int(next(x.split()[1] for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:'))))
def main():
 c=json.loads((ROOT/'contract.json').read_text());name=sys.argv[1];p=c['sessions'][name];out=ROOT/'sessions'/name;out.mkdir(parents=True,exist_ok=False)
 need(Path('/proc/sys/kernel/random/boot_id').read_text().strip()==c['boot'],'Boot changed')
 for n,h in c['payload_SHA'].items():need(digest(ROOT/n)==h,'Payload changed: '+n)
 need(not (ROOT/'UNKNOWN.json').exists(),'Retained previous device owner')
 fd=os.open('/root/fpga_gaming/ps_session_m53.lock',os.O_RDWR|os.O_NOFOLLOW);need(os.fstat(fd).st_ino==943514,'Real lock differs');fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
 loopstart=None;started=time.monotonic();hw=Hardware(fd);initial=hw.snapshot();owner=Ownership(initial);npu=None;capture=None;file=None;encoder=None;frames=commits=unknown=0;last=-1;fid=(initial['36']+2)&0xffffffff;gap=True;tracker=MotionTracker();err=None;closed=False;resources=deque(maxlen=1500);stop=False;recorded=0
 def onstop(*_):
  nonlocal stop;stop=True
 signal.signal(signal.SIGTERM,onstop)
 def publish(phase,**kw):
  state=dict(type='state',session=name,input_mode=p['mode'],phase=phase,updated_monotonic_s=time.monotonic(),elapsed_s=time.monotonic()-started,frames=frames,NPU_calls=npu.calls if npu else 0,commits=commits,unknown=unknown,boot=c['boot'],hardware_observability=hw.observer_state(),**kw)
  if loopstart is not None:state.update(inference_loop_seconds=time.monotonic()-loopstart,processing_fps=frames/max(time.monotonic()-loopstart,1e-9))
  save(out/'state.json',state);emit(state)
 publish('STARTING',initial_PL=initial,inherited_alarm=bool(initial['32']&1))
 try:
  for path,h in c['protected_files'].items():need(digest(path)==h,'Protected original changed')
  need(resource()['MemAvailable_KiB']>=131072,'Memory floor')
  # Open/load inference first, then obtain fresh camera frames; never infer a pre-load queue.
  npu=NPU.__new__(NPU);NPU.__init__(npu,fd,dict(c,max_forwards=p['max_forwards']),out)
  after_load=hw.snapshot();need(all(initial[k]==after_load[k] for k in ['192','36','40','44','48']) and (initial['32']&1)==(after_load['32']&1),'Model load changed retained alarm/counters')
  if p['mode']=='camera':capture=Latest(c['camera_device'],out,p.get('capture_fps',15))
  else:
   need(digest(c['file_source'])==c['file_SHA'],'Fixed file changed');file=FileFrames(c['file_source']);need(file.isOpened(),'File source failed')
  loopstart=time.monotonic();gap_done=False;next_due=loopstart;first_frame_at=None
  with RollingText(out/'frames.jsonl') as rawlog,RollingText(out/'events.csv',header='frame,monotonic_s,event_bits,alarm,fault,source\n') as ef:
   ew=csv.writer(ef)
   while not stop and (not p['max_forwards'] or frames<p['max_forwards']) and (not p['seconds'] or time.monotonic()-loopstart<p['seconds']):
    need(Path('/proc/sys/kernel/random/boot_id').read_text().strip()==c['boot'],'Boot changed')
    if (out/'STOP').exists():stop=True;break
    t=time.monotonic()
    rate=p.get('service_rate_hz',0)
    if capture and rate>0:
     if t<next_due:time.sleep(next_due-t)
     next_due=max(next_due+1./rate,time.monotonic())
    if p.get('gap_after_s') is not None and not gap_done and t-loopstart>=p['gap_after_s']:
     gap_done=True;before=hw.snapshot();publish('FAULT_INJECTION_NO_SUBMIT',PL=before);time.sleep(1.25);after=hw.snapshot();need(after['32']&2 and (after['32']&1)==(before['32']&1) and after['40']==before['40'],'No-submit fault/hold failed');gap=True;owner.unknown();tracker=MotionTracker();ew.writerow([frames,time.monotonic(),4,bool(after['32']&1),True,p['mode']]);ef.flush();publish('RECOVERING',PL=after)
    if capture:
     item=capture.get(last,time.monotonic()+.5)
     if item is None:
      if frames==0:
       need(time.monotonic()-loopstart<30,'Camera first-frame deadline30s exceeded')
       publish('WAITING_FIRST_FRAME',PL=hw.snapshot());continue
      unknown+=1;gap=True;owner.unknown();publish('INPUT_FAULT',PL=hw.snapshot());continue
     seq,received,im=item
     if not fresh(seq,last,received,time.monotonic_ns()):unknown+=1;gap=True;owner.unknown();publish('STALE_INPUT_FAULT',PL=hw.snapshot());continue
     last=seq
     if first_frame_at is None:first_frame_at=time.monotonic()-loopstart
    else:
     ok,im=file.read()
     if not ok:break
     received=time.monotonic_ns();seq=frames
    hw_start=time.monotonic();before=hw.snapshot();begin=time.monotonic();detections,stages=npu.infer(im);end=time.monotonic();frames+=1
    after=hw.snapshot();need(all(before[k]==after[k] for k in ['192','36','40','44','48']) and (before['32']&1)==(after['32']&1),'NPU forward changed retained business state')
    hw_checked=time.monotonic()
    reason=None;pl=None;ownership_hold=False
    try:
     if capture and time.monotonic_ns()-received>500000000:raise ValueError('Result frame-age exceeded500ms')
     q=boxes_q16(detections,im.shape[1],im.shape[0]);positive=hit(q)
     if not owner.allow(positive):ownership_hold=True
     if ownership_hold:raise ValueError('INHERITED_ALARM_HOLD')
     if gap:fid=(fid+2)&0xffffffff;tracker=MotionTracker();gap=False
     pl=hw.commit(fid,q);fid=(fid+1)&0xffffffff;commits+=1
    except (ValueError,AssertionError) as e:reason=str(e)
    if reason:
     if not ownership_hold:unknown+=1
     gap=True;owner.unknown()
    pl_done=time.monotonic()
    tracks=tracker.update(detections) if reason is None else []
    tracked=time.monotonic()
    row=dict(profile_ms=dict(npu.profile,hw_pre=(begin-hw_start)*1000,hw_post=(hw_checked-end)*1000,alarm=(pl_done-hw_checked)*1000,track=(tracked-pl_done)*1000),frame=frames-1,capture_sequence=seq,received_monotonic_ns=received,completed_monotonic_ns=time.monotonic_ns(),detections=detections,tracks=tracks,PL=pl,unknown_reason=reason,stages_ms=stages,request_response_ms=(end-begin)*1000,receive_to_result_ms=(time.monotonic_ns()-received)/1e6)
    rawlog.write(json.dumps(row,allow_nan=False)+'\n');rawlog.flush();pass # No per-frame in-memory history
    if pl and pl['CPU']['events']:ew.writerow([frames-1,time.monotonic(),pl['CPU']['events'],pl['CPU']['alarm'],False,p['mode']]);ef.flush()
    phase='INHERITED_ALARM_HOLD' if ownership_hold else ('OBSERVATION_FAULT' if reason else 'RUNNING');st=hw.snapshot()
    if encoder is None:encoder=MediaSink(out,p['mode'],record_limit=p['video_frames_max'])
    encoder.write(dict(image=im,detections=detections,tracks=tracks,status=st,frames=frames,phase=phase,record=p['video_frames_max']>0,capture_sequence=seq,received_monotonic_ns=received,media_source_ns=received))
    media_stats=encoder.stats();recorded=media_stats['record_written']
    if frames%5==0 or frames==1:
     if frames%30==0:resources.append(dict(at_s=time.monotonic()-loopstart,NPU_RSS_KiB=int(next(x.split()[1] for x in Path('/proc/%d/status'%npu.p.pid).read_text().splitlines() if x.startswith('VmRSS:'))),**resource()));save(out/'resources.json',list(resources))
     publish(phase,PL=st,capture_sequence=seq,receive_to_result_ms=row['receive_to_result_ms'],model='11',hardware='M512',recorded_video_frames=recorded,reason=reason,media=media_stats,input_verification_mode='full')
    if p['seconds']:need(time.monotonic()-started<p['seconds']+120,'Whole session wall budget')
   duration=time.monotonic()-loopstart
  closed=npu.close();need(closed,'Device completion unknown; owner retained')
  media_final=None
  if encoder:encoder.release();media_final=encoder.stats();recorded=media_final['record_written'];encoder=None
  duration=time.monotonic()-loopstart
  final=hw.snapshot()
  result=dict(hardware_observability=hw.observer_state(force=True),media=media_final,input_verification_mode='full',input_hash_samples=npu.calls,input_hash_skipped=0,completed=True,stopped=stop,mode=p['mode'],frames=frames,NPU_calls=npu.calls,commits=commits,unknown=unknown,loop_seconds=duration,processing_fps=frames/duration,initial_PL=initial,final_PL=final,explicit_business_reset_writes=0,SDK_device_reset_per_forward=True,business_write_calls=hw.writes,NPU_child_exit=npu.p.returncode,video_frames=recorded,source_received_frames=capture.count if capture else frames,capture_gap_test=gap_done,independent_accuracy=False,physical_LED_verified=False,camera_first_frame_wait_s=first_frame_at,service_rate_cap_hz=p.get('service_rate_hz',0) if capture else None,capture_requested_fps=p.get('capture_fps'))
 except Exception as e:
  err=str(e);result=dict(completed=False,error=err,traceback=traceback.format_exc(),frames=frames,NPU_calls=npu.calls if npu else 0,commits=commits,unknown=unknown,explicit_business_reset_writes=0,SDK_device_reset_per_forward=True)
 finally:
  if capture:capture.close()
  if file:file.release()
  if encoder:
   try:encoder.release()
   except Exception as e:result['encoder_close_error']=str(e)
  if npu and not closed:
   try:closed=npu.close()
   except Exception as e:result['NPU_close_error']=str(e)
  if npu is None:closed=True
  result['device_closed']=closed;result['boot']=c['boot'];save(out/'result.json',result);publish('COMPLETED' if result['completed'] else 'FAILED',result=result)
  if not closed:
   publish('UNKNOWN_RETAINED',result=result)
   save(ROOT/'UNKNOWN.json',dict(pid=os.getpid(),boot=c['boot'],session=name,NPU_child=npu.p.pid if npu and npu.p else None))
   emit(dict(type='UNKNOWN_RETAINED',PID=os.getpid(),error=err))
   while True:time.sleep(1)
  hw.lib.switch_close();os.close(fd)
 emit(dict(type='terminal',session=name,result=result));return 0 if result['completed'] else 1
if __name__=='__main__':cv2.setNumThreads(1);sys.exit(main())
