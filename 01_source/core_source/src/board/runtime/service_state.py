"""汇总当前启动、检测会话和运行状态。"""
from pathlib import Path
import json, time

ROOT = Path(__file__).resolve().parent


# 仅用于展示文件，读取失败返回空状态；硬件运行阶段仍由snapshot明确判定。
def read(p):
    try:
        return json.loads(p.read_text())
    except (OSError, ValueError):
        return {}


# 只接受camera_会话名，避免把任意路径用作导出根目录。
# 会话名只允许camera_前缀和字母数字下划线；返回活动记录及受限会话路径。
def current():
    x = read(ROOT / "ACTIVE_RUN.json")
    name = x.get("session", "")
    if not name.startswith("camera_") or not all(c.isalnum() or c == "_" for c in name):
        return {}, None
    return x, ROOT / "sessions" / name


# 汇合服务阶段与会话状态后，再检查boot和更新时间。历史JPEG可以继续展示，
# 但只有同boot、RUNNING且3秒内更新的状态标为live，避免把停止画面当实时结果。
def snapshot():
    boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    now = time.monotonic()
    active, out = current()
    app = read(ROOT / "APP_STATE.json")
    state = read(out / "state.json") if out else {}
    age = max(0, now - state.get("updated_monotonic_s", 0)) if state.get("boot") == boot else None
    phase = state.get("phase", "IDLE")
    if app.get("boot") != boot:
        phase = "NOT_STARTED_THIS_BOOT"
    elif app.get("phase") != "ACTIVE":
        phase = app.get("phase", "IDLE")
    elif active.get("boot") != boot or state.get("boot") != boot:
        phase = "STARTING"
    if (ROOT / "UNKNOWN.json").exists():
        phase = "UNKNOWN_RETAINED"
    live = phase == "RUNNING" and age is not None and age < 3
    if phase == "RUNNING" and not live:
        phase = "STALE_INPUT"
    result = dict(
        model="11",
        boot=boot,
        board_uptime_s=now,
        active=active,
        app=app,
        state=state,
        phase=phase,
        live=live,
        state_age_s=age,
        history_only=not live,
        record_retention="Rolling recent records; up to five completed/current sessions. Export before overwrite.",
    )
    if out and (out / "latest.jpg").is_file():
        result["preview_available"] = True
    return result
