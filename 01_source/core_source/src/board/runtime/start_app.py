"""通过systemd启动检测服务。"""
import subprocess, json
from run_camera import ROOT

if (ROOT / "UNKNOWN.json").exists():
    raise RuntimeError("Device owner retained; inspect UNKNOWN.json")
subprocess.run(["systemctl", "start", "model11-detect.service"], check=True)
print(json.dumps(dict(requested=True, service="model11-detect.service", continuous=True)))
