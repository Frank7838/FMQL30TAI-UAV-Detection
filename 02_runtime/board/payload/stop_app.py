"""Cooperative stop; never kill an NPU process."""
import subprocess,json
subprocess.run(['systemctl','stop','--no-block','model11-detect.service'],check=True)
print(json.dumps(dict(requested=True,message='Wait for STOPPED/device_closed; unknown owners remain retained')))
