"""向检测会话提交正常停止请求。"""
import subprocess, json

subprocess.run(["systemctl", "stop", "--no-block", "model11-detect.service"], check=True)
print(
    json.dumps(
        dict(
            requested=True, message="Wait for STOPPED/device_closed; unknown owners remain retained"
        )
    )
)
