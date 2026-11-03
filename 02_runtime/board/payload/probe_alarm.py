"""One fixed11-frame NPU-to-PL alarm regression, never an accuracy test."""
from pathlib import Path
import datetime,fcntl,json,os,sys,time,traceback
import cv2
from worker import Hardware,NPU
from policy import boxes_q16,hit
from motion_tracker_m355 import MotionTracker
from run_camera import ROOT,RUNTIME,verify_payload,save

def main():
 cv2.setNumThreads(1)
 plan=json.loads((ROOT/'qualification/ALARM_PLAN.json').read_text())
 verify_payload()
 assert Path('/proc/sys/kernel/random/boot_id').read_text().strip()==plan['expected_boot']
 assert not (ROOT/'alarm_execution').exists()
 fd=os.open('/root/fpga_gaming/ps_session_m53.lock',os.O_RDWR|os.O_NOFOLLOW);fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
 assert os.fstat(fd).st_ino==943514
 out=ROOT/'alarm_execution';out.mkdir();hw=None;npu=None;closed=False;rows=[];error=None;start=time.monotonic()
 try:
  hw=Hardware(fd);initial=hw.snapshot()
  assert not(initial['32']&1),'Do not clear an inherited alarm'
  npu=NPU.__new__(NPU);NPU.__init__(npu,fd,dict(runtime_env=RUNTIME,boot=plan['expected_boot'],max_forwards=11),out)
  fid=(initial['36']+2)&0xffffffff;tracker=MotionTracker()
  with (out/'FRAMES.jsonl').open('x') as f:
   for i,name in enumerate(plan['sequence']):
    assert time.monotonic()-start<90
    im=cv2.imread(str(ROOT/'qualification/input'/name));assert im.shape==(640,640,3)
    ds,stages=npu.infer(im);boxes=boxes_q16(ds,640,640)
    assert hit(boxes)==(i<3),'Actual detector did not produce the planned integration stimulus'
    commit=hw.commit(fid,boxes);fid=(fid+1)&0xffffffff
    tracks=tracker.update(ds)
    row=dict(index=i,input=name,detections=ds,tracks=tracks,PL=commit,stages_ms=stages)
    f.write(json.dumps(row,allow_nan=False)+'\n');f.flush();rows.append(row)
    if i==2:assert commit['CPU']['alarm'],'Third positive failed to set PL alarm'
    if i==10:assert not commit['CPU']['alarm'],'Eighth negative failed to clear PL alarm'
  after=hw.snapshot();assert after['40']==initial['40']+11 and after['44']==after['48']==0
  closed=npu.close();assert closed
 except Exception:error=traceback.format_exc()
 finally:
  if npu and not closed:
   try:closed=npu.close()
   except Exception:pass
  if npu is None:closed=True
  result=dict(at=datetime.datetime.now().astimezone().isoformat(),completed=error is None,frames=len(rows),
              NPU_calls=npu.calls if npu else 0,PL_commits=len(rows),device_closed=closed,error=error,
              elapsed_s=time.monotonic()-start,independent_accuracy=False,physical_LED_verified=False,
              SDK_device_reset_per_forward=True,explicit_business_reset_writes=0)
  save(out/'RESULT.json',result);print(json.dumps(result),flush=True)
  if not closed:
   save(ROOT/'UNKNOWN.json',dict(pid=os.getpid(),boot=plan['expected_boot'],child=npu.p.pid if npu and npu.p else None))
   while True:time.sleep(1)
  if hw:hw.lib.switch_close()
  os.close(fd)
 return 0 if error is None else 1
if __name__=='__main__':sys.exit(main())
