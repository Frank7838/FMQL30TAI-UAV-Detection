"""通过SSH执行启动、停止和状态读取命令。"""
from pathlib import Path
import argparse, base64, hashlib, json, re, socket, sys, time
import paramiko

DEFAULT_PIN = ""
APP = "/root/fpga_gaming/model11_standalone_20261025_v1"


# 主机指纹确认连接的是已授权板卡；它与证明用户身份的SSH私钥是两项不同检查。
class Pinned(paramiko.MissingHostKeyPolicy):
    # expected来自本机连接配置，不能用网络上刚读到的未知指纹自动替换。
    def __init__(self, expected):
        self.expected = expected

    # 使用保存的SHA256指纹验证板卡身份，拒绝未知替换设备。
    # 对服务端公钥算SHA256；不匹配直接拒绝，不采用自动信任首次连接。
    def missing_host_key(self, client, hostname, key):
        actual = "SHA256:" + base64.b64encode(
            hashlib.sha256(key.asbytes()).digest()
        ).decode().rstrip("=")
        if actual != self.expected:
            raise paramiko.SSHException("板卡身份与已保存的主机指纹不一致，已拒绝连接。")


# 检查地址、用户、端口、指纹和私钥文件位置；配置只存路径，不把私钥内容写入JSON。
def load(path):
    c = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,253}", c["host"]):
        raise ValueError("板卡地址无效")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", c.get("username", "root")):
        raise ValueError("用户名无效")
    if not re.fullmatch(r"SHA256:[A-Za-z0-9+/]{43}", c.get("host_fingerprint", "")):
        raise ValueError("请配置已核实的主机SHA256指纹")
    if not 1 <= int(c.get("ssh_port", 22)) <= 65535:
        raise ValueError("SSH端口无效")
    if not c.get("key_file") or not Path(c["key_file"]).is_file():
        raise ValueError("请在连接设置中选择本机已授权的SSH私钥。迁移包不附带私钥。")
    return c


# 只执行固定控制命令，有界响应和超时；超时不代表板端已停止。
# 仅允许start/stop/check固定命令。禁用SSH agent及自动搜索其他密钥，避免迁移时误选身份。
# stdout/stderr合计最多32KiB，超时只表示控制请求未确认，不能推断板端已经停止。
def execute(c, action):
    commands = {
        "start": "cd " + APP + " && python3 -B start_app.py",
        "stop": "cd " + APP + " && python3 -B stop_app.py",
        "check": "cat /proc/sys/kernel/random/boot_id; systemctl show model11-detect.service -p ActiveState -p SubState -p MainPID",
    }
    command = commands[action]
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(6)
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(Pinned(c["host_fingerprint"]))
    try:
        if c.get("bind_address"):
            sock.bind((c["bind_address"], 0))
        sock.connect((c["host"], int(c.get("ssh_port", 22))))
        client.connect(
            c["host"],
            port=int(c.get("ssh_port", 22)),
            username=c.get("username", "root"),
            key_filename=c["key_file"],
            sock=sock,
            allow_agent=False,
            look_for_keys=False,
            timeout=6,
            banner_timeout=6,
            auth_timeout=6,
        )
        channel = client.get_transport().open_session(timeout=6)
        channel.exec_command(command)
        channel.shutdown_write()
        out = bytearray()
        err = bytearray()
        end = time.monotonic() + 12
        while True:
            if channel.recv_ready():
                out.extend(channel.recv(4096))
            if channel.recv_stderr_ready():
                err.extend(channel.recv_stderr(4096))
            if len(out) + len(err) > 32768:
                raise RuntimeError("控制响应超过预期大小，请查看板端状态。")
            if (
                channel.exit_status_ready()
                and not channel.recv_ready()
                and not channel.recv_stderr_ready()
            ):
                break
            if time.monotonic() > end:
                raise TimeoutError("响应超时，板端推理未被强杀；请查看当前状态后再操作。")
            time.sleep(0.02)
        rc = channel.recv_exit_status()
        if rc:
            raise RuntimeError(
                err.decode(errors="replace")[-1500:]
                or out.decode(errors="replace")[-1500:]
                or "板端请求失败"
            )
        return dict(ok=True, action=action, response=out.decode(errors="replace"))
    finally:
        client.close()
        sock.close()


# 每次调用输出一条JSON供C#读取；self-test只查组件环境，不连接板卡。
def main():
    p = argparse.ArgumentParser()
    p.add_argument("action", choices=["start", "stop", "check", "self-test"])
    p.add_argument("--config")
    a = p.parse_args()
    try:
        if a.action == "self-test":
            result = dict(
                ok=True,
                app="UAVDetectionSystemAgent",
                paramiko=paramiko.__version__,
                bundled=bool(getattr(sys, "frozen", False)),
                browser_required=False,
                system_ssh_required=False,
            )
        else:
            result = execute(load(a.config), a.action)
        print(json.dumps(result, ensure_ascii=True))
        return 0
    except Exception as e:
        print(json.dumps(dict(ok=False, error=str(e)), ensure_ascii=True))
        return 1


if __name__ == "__main__":
    sys.exit(main())

