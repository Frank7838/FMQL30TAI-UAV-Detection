"""Camera-only producer; no model, MMIO or inherited device lock."""
import cv2,os,signal,struct,sys,time,json
run=True
def stop(*_):
 global run;run=False
signal.signal(signal.SIGTERM,stop)
def send(b):
 p=memoryview(b).cast('B')
 while p:
  n=os.write(1,p);p=p[n:]
cv2.setNumThreads(1)
c=cv2.VideoCapture(sys.argv[1],cv2.CAP_V4L2)
try:
 if not c.isOpened():raise RuntimeError('D435i RGB open failed')
 c.set(cv2.CAP_PROP_FOURCC,cv2.VideoWriter_fourcc(*'YUYV'));c.set(3,640);c.set(4,480);c.set(5,int(sys.argv[2]) if len(sys.argv)>2 else 15);c.set(cv2.CAP_PROP_BUFFERSIZE,2)
 print(json.dumps(dict(type='capture_negotiated',requested_fps=int(sys.argv[2]) if len(sys.argv)>2 else 15,reported_fps=c.get(5),reported_buffers=c.get(cv2.CAP_PROP_BUFFERSIZE),width=c.get(3),height=c.get(4))),file=sys.stderr,flush=True)
 seq=0
 while run:
  ok,im=c.read();ts=time.monotonic_ns()
  if not ok or im is None or im.shape!=(480,640,3):raise RuntimeError('RGB capture missing/shape changed')
  raw=im.tobytes();send(struct.pack('<IQQIII',0x43414d31,seq,ts,640,480,len(raw)));send(raw);seq+=1
finally:c.release()
