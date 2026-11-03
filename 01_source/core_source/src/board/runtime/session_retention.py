"""Bounded camera history, with boot identity instead of a directory-count gate.

Called by the single systemd launcher before it opens any camera or SDK.
Current-boot uncertain owners remain a fault. Interrupted older boots are history,
not live device owners. The existing kernel device lock must be free throughout GC.
"""

from contextlib import contextmanager
from pathlib import Path
import json
import os
import re
import shutil
import stat
import time
import uuid

DEVICE_LOCK = Path("/root/fpga_gaming/ps_session_m53.lock")
DEVICE_LOCK_INODE = 943514
KEEP_HISTORY = 4


def boot_id(value):
    try:
        return str(uuid.UUID(str(value)))
    except (ValueError, TypeError, AttributeError):
        return None


def read_json(path):
    if not path.exists():
        return {}
    data = json.loads(path.read_text())
    if not isinstance(data, dict):
        raise ValueError("Expected JSON object: " + str(path))
    return data


def atomic_json(path, value):
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2))
    os.replace(temp, path)


@contextmanager
def device_guard(path=DEVICE_LOCK, expected_inode=DEVICE_LOCK_INODE):
    # Never create/unlink/replace the real lock and never open a device or MMIO.
    import fcntl

    fd = os.open(str(path), os.O_RDWR | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_ino != expected_inode:
            raise RuntimeError("Device lock identity differs; inspect before starting")
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("Device owner still holds its lock; no history changed")
        yield
    finally:
        os.close(fd)


def classify(path, current_boot):
    """Only a confirmed SDK close or a different, consistent boot is reclaimable."""
    try:
        state = read_json(path / "state.json")
        result = read_json(path / "result.json")
        owner = read_json(path / "owner.json")
        raw_boots = [x["boot"] for x in (state, result, owner) if x.get("boot")]
        match = re.search(r"_b([0-9a-f]{32})$", path.name)
        if match:
            raw_boots.append(match.group(1))
        parsed = [boot_id(x) for x in raw_boots]
        if None in parsed or len(set(parsed)) > 1:
            raise ValueError("Conflicting or invalid boot identity")
        previous = parsed[0] if parsed else None
        if result.get("device_closed") is True:
            kind = "SDK_CLOSED"
        elif previous and previous != current_boot:
            kind = "INTERRUPTED_PREVIOUS_BOOT"
        else:
            kind = "UNRESOLVED_CURRENT_BOOT" if previous else "UNCLASSIFIED_LEGACY"
        return dict(
            name=path.name,
            kind=kind,
            boot=previous,
            frames=result.get("frames", state.get("frames")),
            last_phase=state.get("phase"),
            updated_monotonic_s=float(state.get("updated_monotonic_s") or 0),
            error=str(result.get("error", ""))[:500],
        )
    except (OSError, ValueError) as exc:
        return dict(name=path.name, kind="UNCLASSIFIED_LEGACY", error=str(exc)[:500])


def collect_history(root, current_boot, keep=KEEP_HISTORY):
    """Caller holds launcher and device guards; never count unrelated files."""
    root = Path(root).resolve()
    current_boot = boot_id(current_boot)
    if current_boot is None:
        raise RuntimeError("Current boot identity unavailable")
    if (root / "UNKNOWN.json").exists():
        raise RuntimeError("Prior device session retained; inspect UNKNOWN.json")
    sessions = root / "sessions"
    if sessions.is_symlink():
        raise RuntimeError("sessions must be a real directory")
    sessions.mkdir(exist_ok=True)
    candidates, ignored = [], 0
    for path in sessions.iterdir():
        if not path.name.startswith("camera_") or path.is_symlink() or not path.is_dir():
            ignored += 1
            continue
        candidates.append((path, classify(path, current_boot)))
    unresolved = [
        item
        for _, item in candidates
        if item["kind"] in ("UNRESOLVED_CURRENT_BOOT", "UNCLASSIFIED_LEGACY")
    ]
    if unresolved:
        names = ", ".join(item["name"] for item in unresolved[:4])
        raise RuntimeError("Unresolved session ownership; no history changed: " + names)
    active = read_json(root / "ACTIVE_RUN.json").get("session")
    # This board can return to an old calendar date at boot. Keep the last active
    # run first and order this boot's histories by monotonic time, not wall time.
    candidates.sort(
        key=lambda pair: (
            pair[0].name == active,
            pair[1]["boot"] == current_boot,
            pair[1]["updated_monotonic_s"] if pair[1]["boot"] == current_boot else 0,
            pair[0].stat().st_mtime_ns,
            pair[0].name,
        ),
        reverse=True,
    )
    retained, pruned = candidates[:keep], candidates[keep:]
    report = dict(
        boot=current_boot,
        checked_monotonic_s=time.monotonic(),
        policy="Keep four safe histories plus the next/current run; old boot is not a live owner",
        ignored_entries=ignored,
        retained=[item for _, item in retained],
        pruned_count=len(pruned),
        pruned=[item for _, item in pruned[:16]],
        pruned_list_truncated=len(pruned) > 16,
        applied=False,
    )
    # A single bounded report preserves the reason without relabelling an abrupt
    # power cut as device_closed=true or maintaining an unbounded archive.
    atomic_json(root / "SESSION_RETENTION.json", report)
    for path, _ in pruned:
        if path.resolve().parent != sessions or path.is_symlink():
            raise RuntimeError("Session path changed during history collection")
        shutil.rmtree(path)
    report["applied"] = True
    atomic_json(root / "SESSION_RETENTION.json", report)
    if shutil.disk_usage(root).free < 512 * 1024 * 1024:
        raise RuntimeError("Less than512MiB free; export records and free space")
    return report


def maintain_sessions(root):
    # The caller already owns launcher.lock. A worker left alive by a failed
    # launcher still owns DEVICE_LOCK and therefore blocks both GC and restart.
    current_boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    with device_guard():
        return collect_history(root, current_boot)


def prepare_session(path, current_boot):
    """Validate identity; the unchanged worker exclusively creates the directory.

    The boot suffix identifies a power cut even before state.json is written.
    Do not pre-create this directory: the accepted worker uses exist_ok=False.
    """
    current_boot = boot_id(current_boot)
    if not current_boot or not path.name.endswith("_b" + current_boot.replace("-", "")):
        raise RuntimeError("New session requires its complete boot identity")
    if path.exists():
        raise RuntimeError("Session already exists; refusing to reuse it")
