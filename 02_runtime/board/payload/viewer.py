"""Bounded, read-only LAN display. SSH remains the only control entry."""
from pathlib import Path
import argparse,json,mimetypes,threading,tempfile,zipfile,socket,shutil,time
from http.server import ThreadingHTTPServer,BaseHTTPRequestHandler
from service_state import ROOT,current,snapshot
EXPORT=threading.Lock()
class Server(ThreadingHTTPServer):
 daemon_threads=True
 def __init__(self,*a):self.slots=threading.BoundedSemaphore(8);super().__init__(*a)
 def process_request(self,request,address):
  if not self.slots.acquire(blocking=False):request.close();return
  request.settimeout(10)
  try:super().process_request(request,address)
  except Exception:self.slots.release();raise
 def process_request_thread(self,*args):
  try:super().process_request_thread(*args)
  finally:self.slots.release()
class Handler(BaseHTTPRequestHandler):
 def output(self,data,mime='application/json',status=200):
  self.send_response(status);self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store');self.end_headers()
  try:self.wfile.write(data)
  except (OSError,TimeoutError):pass
 def do_GET(self):
  path=self.path.split('?',1)[0]
  if path=='/api/state':return self.output(json.dumps(snapshot(),ensure_ascii=False).encode())
  active,out=current()
  if path=='/export.zip' and out:return self.export(out,active)
  allowed={'/preview.jpg':'latest.jpg','/events.csv':'events.csv','/result.json':'result.json','/resources.json':'resources.json'}
  if path in allowed and out:
   p=out/allowed[path]
   if p.is_file():return self.output(p.read_bytes(),mimetypes.guess_type(p.name)[0] or 'application/octet-stream')
  return self.output(b'Not available','text/plain',404)
 def export(self,out,active):
  if not EXPORT.acquire(blocking=False):return self.output(b'Export busy','text/plain',429)
  try:
   with tempfile.SpooledTemporaryFile(max_size=1024*1024) as data:
    with zipfile.ZipFile(data,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as z:
     z.writestr('EXPORT_INFO.json',json.dumps(dict(active=active,snapshot=snapshot(),note='Recent rolling records only; live export may straddle a frame/rotation. Not an independent accuracy evaluation.'),ensure_ascii=False))
     for p in sorted(out.iterdir()):
      if p.is_file() and p.name!='STOP' and not p.name.endswith('.tmp'):
       try:z.write(p,p.name)
       except FileNotFoundError:pass
    size=data.tell();data.seek(0)
    self.send_response(200);self.send_header('Content-Type','application/zip');self.send_header('Content-Disposition','attachment; filename="model11_'+active['session']+'.zip"');self.send_header('Content-Length',str(size));self.send_header('Cache-Control','no-store');self.end_headers()
    shutil.copyfileobj(data,self.wfile,64*1024)
  except (OSError,TimeoutError):pass
  finally:EXPORT.release()
 def log_message(self,*args):pass
if __name__=='__main__':
 a=argparse.ArgumentParser();a.add_argument('--bind',default='0.0.0.0');a.add_argument('--port',type=int,default=8080);v=a.parse_args();Server((v.bind,v.port),Handler).serve_forever()
