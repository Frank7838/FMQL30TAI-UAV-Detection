"""Bounded application records. Rotation applies only to this new runtime."""
from pathlib import Path
import os,threading
class RollingText:
 def __init__(self,path,max_bytes=4*1024*1024,backups=2,header=''):
  self.path=Path(path);self.limit=max_bytes;self.backups=backups;self.header=header
  self.f=self.path.open('a',encoding='utf8',newline='');self.size=self.path.stat().st_size
  if not self.size and header:self.write(header)
 def write(self,text):
  size=len(text.encode('utf8'))
  if self.size+size>self.limit:
   self.f.close()
   for i in range(self.backups,0,-1):
    source=self.path if i==1 else Path(str(self.path)+'.'+str(i-1))
    if source.exists():os.replace(source,Path(str(self.path)+'.'+str(i)))
   self.f=self.path.open('w',encoding='utf8',newline='');self.size=0
   if self.header:self.f.write(self.header);self.size=len(self.header.encode('utf8'))
  self.f.write(text);self.size+=size
 def flush(self):self.f.flush()
 def close(self):self.f.close()
 def __enter__(self):return self
 def __exit__(self,*_):self.close()
class PipeLog:
 def __init__(self,pipe,path):
  self.pipe=pipe;self.path=path;self.error=None
  self.t=threading.Thread(target=self.copy,daemon=True);self.t.start()
 def copy(self):
  try:
   with RollingText(self.path,max_bytes=1024*1024) as out:
    while True:
     chunk=self.pipe.read(8192)
     if not chunk:break
     out.write(chunk.decode('utf8',errors='replace'));out.flush()
  except Exception as e:self.error=e
  finally:self.pipe.close()
 def close(self):
  self.t.join(timeout=5)
  if self.t.is_alive():raise RuntimeError('Subprocess log did not close')
  if self.error:raise RuntimeError('Subprocess log failed: '+str(self.error))
