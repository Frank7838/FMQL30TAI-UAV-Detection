"""Bounded CPU-only overlap. Threads never own SDK devices or PL mappings."""
import queue,threading
import cv2
class FileFrames:
 def __init__(self,path):
  self.q=queue.Queue(2);self.stop=threading.Event();self.error=None;self.opened=False;self.ready=threading.Event()
  self.t=threading.Thread(target=self._read,args=(path,),daemon=True);self.t.start()
  if not self.ready.wait(10):raise RuntimeError('File open readiness timeout')
 def _put(self,x):
  while not self.stop.is_set():
   try:self.q.put(x,timeout=.1);return
   except queue.Full:pass
 def _read(self,path):
  c=None
  try:
   c=cv2.VideoCapture(path);self.opened=c.isOpened();self.ready.set()
   if not self.opened:raise RuntimeError('File open failed')
   while not self.stop.is_set():
    ok,image=c.read()
    if not ok:break
    self._put(image)
  except Exception as e:self.error=e;self.ready.set()
  finally:
   if c:c.release()
   self._put(None)
 def isOpened(self):return self.opened
 def read(self):
  try:x=self.q.get(timeout=15)
  except queue.Empty:raise RuntimeError('File decoder timeout')
  if self.error:raise RuntimeError('File decoder failure: '+str(self.error))
  return x is not None,x
 def release(self):
  self.stop.set();self.t.join(timeout=10)
  if self.t.is_alive():raise RuntimeError('File decoder failed to close')
class VideoSink:
 def __init__(self,factory):
  self.q=queue.Queue(2);self.error=None;self.closed=False;self.frames=0;self.ready=threading.Event()
  self.t=threading.Thread(target=self._write,args=(factory,),daemon=True);self.t.start()
  if not self.ready.wait(10):raise RuntimeError('Video encoder readiness timeout')
  self.check()
 def check(self):
  if self.error:raise RuntimeError('Video encoder failure: '+str(self.error))
 def _write(self,factory):
  writer=None
  try:
   writer=factory();self.ready.set()
   while True:
    image=self.q.get()
    if image is None:break
    writer.write(image);self.frames+=1
  except Exception as e:self.error=e;self.ready.set()
  finally:
   if writer:
    try:writer.release()
    except Exception as e:self.error=e
 def write(self,image):
  if self.closed:raise RuntimeError('Video sink closed')
  self.check()
  try:self.q.put(image.copy(),timeout=10)
  except queue.Full:raise RuntimeError('Video encoder queue timeout')
  self.check()
 def release(self):
  if self.closed:return
  self.closed=True;self.check()
  try:self.q.put(None,timeout=10)
  except queue.Full:raise RuntimeError('Video encoder drain timeout')
  self.t.join(timeout=15)
  if self.t.is_alive():raise RuntimeError('Video encoder failed to close')
  self.check()
