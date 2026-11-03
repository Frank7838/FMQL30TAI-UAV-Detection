"""读取Icraft配置并编译INT8部署模型。"""
from pathlib import Path
import json,hashlib,subprocess,datetime,time
P=Path(__file__).resolve().parent;f=json.loads((P/'COMPILE_FREEZE.json').read_text());sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(Path(f['config_path']))==f['config_SHA256'];assert sha(P/'CALIBRATION_MANIFEST.json')==f['calibration_manifest_SHA256']
with (P/'COMPILE_STARTED.json').open('x') as out:json.dump({'at':datetime.datetime.now().astimezone().isoformat(),'attempt':1,'board':0,'TEST':0,'SDK':'3.39.0'},out,indent=2)
start=time.monotonic()
with (P/'compile.stdout').open('xb') as out,(P/'compile.stderr').open('xb') as err:
 child=subprocess.Popen(['C:/Icraft/CLI/bin/icraft.exe','compile','config/ZG/drone_int8.toml'],cwd=P/'2_compile',stdout=out,stderr=err)
 (P/'COMPILE_PID.json').write_text(json.dumps({'PID':child.pid,'start_at':datetime.datetime.now().astimezone().isoformat(),'scope':'PConly compiler; no timeoutkill'}))
 rc=child.wait()
x={'at':datetime.datetime.now().astimezone().isoformat(),'native_exit':rc,'elapsed_s':time.monotonic()-start,'no_adoption':True,'board':0,'TEST':0}
(P/'COMPILE_EXECUTION.json').write_text(json.dumps(x,indent=2));print(json.dumps(x),flush=True)
