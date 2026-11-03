"""用固定图像检查FPGA告警的置位和解除。"""
from pathlib import Path
import argparse, ctypes, fcntl, json, os, subprocess, time, traceback

ROOT = Path("/root/fpga_gaming/model11_standalone_20261025_v1")


def need(value, message):
    if not value:
        raise RuntimeError(message)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--expected-boot", required=True)
    ap.add_argument("--run-qualification-once", action="store_true", required=True)
    a = ap.parse_args()
    boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    need(boot == a.expected_boot, "Boot differs; do not reuse prior PID/boot")
    out = ROOT / ("pl_qualification_" + boot)
    out.mkdir(exist_ok=False)
    records = []
    writes = 0
    soft_clears = 0
    fd = None
    lib = None
    opened = False
    start = time.monotonic()
    passed = False
    error = None

    def write(offset, value):
        nonlocal writes, soft_clears
        need(
            time.monotonic() - start < 15 and writes < 100,
            "Finite hardware qualification budget exceeded",
        )
        need(offset in [0, 4, 8, 12, 16], "Only alarm business transaction registers")
        need(lib.switch_write(offset, value) == 0, "Guarded MMIO write failed")
        writes += 1
        if offset == 0:
            soft_clears += 1

    def snapshot(label):
        values = {}
        for offset in [192, 32, 36, 40, 44, 48, 52]:
            v = ctypes.c_uint32()
            need(lib.switch_read(offset, ctypes.byref(v)) == 0, "Guarded read failed")
            values[str(offset)] = v.value
        need(values["192"] == 0xA5050001, "Business ABI signature mismatch")
        records.append(dict(label=label, at_s=time.monotonic() - start, registers=values))
        return values

    def check(label, alarm, fault, accepted, last, protocol=2, rejected=0):
        s = snapshot(label)
        need(
            bool(s["32"] & 1) == alarm
            and bool(s["32"] & 2) == fault
            and s["40"] == accepted
            and s["36"] == last
            and s["48"] == protocol
            and s["44"] == rejected,
            "Mismatch at " + label,
        )
        return s

    def frame(fid, positive):
        write(4, fid)
        if positive:
            write(8, 0x40004000)
            write(12, 0x80008000)
        write(16, 0x101 if positive else 1)

    try:
        need(not (ROOT / "UNKNOWN.json").exists(), "Unknown device work retained")
        need(
            json.loads((ROOT / "APP_STATE.json").read_text())["phase"] == "STOPPED",
            "Stop detector normally and wait for SDK close first",
        )
        service = subprocess.check_output(
            ["systemctl", "show", "model11-detect", "-p", "MainPID", "-p", "ActiveState"], text=True
        )
        need(
            "MainPID=0" in service and "ActiveState=inactive" in service,
            "Detector service still active",
        )
        fd = os.open("/root/fpga_gaming/ps_session_m53.lock", os.O_RDWR)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        lib = ctypes.CDLL(str(ROOT / "libswitch_mmio.so"))
        lib.switch_open.argtypes = [ctypes.c_int]
        lib.switch_read.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
        lib.switch_write.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        need(lib.switch_open(fd) == 0, "Exclusive MMIO open failed")
        opened = True
        initial = snapshot("initial")
        need(not (initial["32"] & 1), "Preexisting alarm: retain it; no synthetic clear permitted")
        need(
            initial["44"] == initial["48"] == 0,
            "Preexisting rejection/protocol fault: preserve evidence",
        )
        write(0, 1)
        check("clean_test_start", False, False, 0, 0, protocol=0)
        for i in [1, 2, 3]:
            frame(i, True)
        check("three_hits_alarm", True, False, 3, 3, protocol=0)
        write(4, 4)
        time.sleep(1.08)
        check("pre_fault_partial_timed_out", True, True, 3, 3, protocol=0)
        write(16, 1)
        check("stale_commit_before_fault_rejected", True, True, 3, 3, protocol=1)
        write(4, 4)
        time.sleep(1.08)
        write(16, 1)
        check("stale_commit_started_while_fault_rejected", True, True, 3, 3)
        frame(4, False)
        check("complete_fresh_result_recovers_fault", True, False, 4, 4)
        for i in range(5, 11):
            frame(i, False)
            check("empty_before_eighth_" + str(i), True, False, i, i)
        frame(11, False)
        check("eighth_empty_clears_alarm", False, False, 11, 11)
        for i in [12, 13, 14]:
            frame(i, True)
        check("synthetic_alarm_again", True, False, 14, 14)
        time.sleep(1.08)
        check("no_submit_fault_holds_alarm", True, True, 14, 14)
        frame(15, False)
        check("fresh_recovery_keeps_alarm", True, False, 15, 15)
        frame(15, False)
        check("duplicate_frame_rejected", True, False, 15, 15, rejected=1)
        # Only the alarm established by this known synthetic test is cleared here.
        write(0, 1)
        check("clean_test_end", False, False, 0, 0, protocol=0)
        need(soft_clears == 2, "Unexpected soft-clear count")
        passed = True
    except Exception as e:
        error = dict(message=str(e), traceback=traceback.format_exc())
        if opened:
            try:
                snapshot("failure_preserved_no_clear")
            except Exception:
                pass
    finally:
        if opened:
            lib.switch_close()
        if fd is not None:
            os.close(fd)
        result = dict(
            passed=passed,
            boot=boot,
            elapsed_s=time.monotonic() - start,
            register_writes=writes,
            business_soft_clear_writes=soft_clears,
            SDK_loads=0,
            DMA_starts=0,
            device_or_clock_resets=0,
            BOOT_writes=0,
            records=records,
            error=error,
            not_physical_LED_acceptance=True,
        )
        (out / "RESULT.json").write_text(json.dumps(result, indent=2))
        print(json.dumps(result))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
