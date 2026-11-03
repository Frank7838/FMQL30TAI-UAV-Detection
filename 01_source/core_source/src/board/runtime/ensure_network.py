"""恢复板端网口的固定地址。"""
import json, subprocess

DEVICE = "eth0"
ADDRESS = "169.254.74.247"
q = subprocess.run(
    ["ip", "-j", "-4", "addr", "show", "dev", DEVICE], check=True, capture_output=True
)
interfaces = json.loads(q.stdout)
if not any(a.get("local") == ADDRESS for d in interfaces for a in d.get("addr_info", [])):
    subprocess.run(["ip", "addr", "add", ADDRESS + "/16", "dev", DEVICE], check=True)
print("MODEL11_WIRED_ALIAS_READY")
