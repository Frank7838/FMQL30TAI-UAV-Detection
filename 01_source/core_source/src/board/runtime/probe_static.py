"""运行固定图像并核对板端推理结果。"""
from pathlib import Path
import datetime, fcntl, hashlib, json, os, sys, time, traceback
import cv2
from worker import Hardware, NPU
from run_camera import ROOT, RUNTIME, verify_payload, save


# 读取预设输入，运行推理并保存状态。
def main():
    cv2.setNumThreads(1)
    plan = json.loads((ROOT / "qualification/INPUT_MANIFEST.json").read_text())
    assert plan["role"] == "TRAIN" and len(plan["rows"]) == 40
    assert not (ROOT / "qualification_execution").exists(), "This probe is single-attempt"
    verify_payload()
    guard = os.open("/root/fpga_gaming/ps_session_m53.lock", os.O_RDWR | os.O_NOFOLLOW)
    fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
    assert os.fstat(guard).st_ino == 943514
    boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    assert boot == plan["expected_boot"], "Board rebooted since this concrete probe was frozen"
    out = ROOT / "qualification_execution"
    out.mkdir()
    hw = None
    npu = None
    closed = False
    rows = []
    error = None
    started = time.monotonic()
    try:
        hw = Hardware(guard)
        before = hw.snapshot()
        assert before["192"] == 0xA5050001 and before["44"] == 0 and before["48"] == 0
        save(
            out / "STARTED.json",
            dict(boot=boot, before_PL=before, max_forwards=40, training=0, TEST=0),
        )
        npu = NPU.__new__(NPU)
        NPU.__init__(npu, guard, dict(runtime_env=RUNTIME, boot=boot, max_forwards=40), out)
        with (out / "PREDICTIONS.jsonl").open("x") as f:
            for row in plan["rows"]:
                assert time.monotonic() - started < 180
                image_path = ROOT / "qualification/input" / row["name"]
                assert hashlib.sha256(image_path.read_bytes()).hexdigest() == row["image_SHA256"]
                image = cv2.imread(str(image_path))
                assert image.shape == (640, 640, 3)
                detections, stages = npu.infer(image)
                result = dict(
                    name=row["name"], role="TRAIN", detections=detections, stages_ms=stages
                )
                f.write(json.dumps(result, allow_nan=False) + "\n")
                f.flush()
                rows.append(result)
        after = hw.snapshot()
        assert all(before[k] == after[k] for k in ["192", "36", "40", "44", "48"])
        assert (before["32"] & 1) == (after["32"] & 1), "Inference modified latched PL alarm"
        closed = npu.close()
        assert closed
    except Exception:
        error = traceback.format_exc()
    finally:
        if npu and not closed:
            try:
                closed = npu.close()
            except Exception:
                pass
        if npu is None:
            closed = True
        result = dict(
            at=datetime.datetime.now().astimezone().isoformat(),
            boot=boot,
            completed=error is None,
            frames=len(rows),
            NPU_calls=npu.calls if npu else 0,
            device_closed=closed,
            error=error,
            elapsed_s=time.monotonic() - started,
            TEST=0,
            training=0,
            PL_business_write_calls=0,
            SDK_device_reset_per_forward=True,
            physical_full_drain_proven=False,
        )
        save(out / "RESULT.json", result)
        print(json.dumps(result), flush=True)
        if not closed:
            save(
                ROOT / "UNKNOWN.json",
                dict(pid=os.getpid(), boot=boot, child=npu.p.pid if npu and npu.p else None),
            )
            print("UNKNOWN_RETAINED_NO_RETRY", flush=True)
            while True:
                time.sleep(1)
        if hw:
            hw.lib.switch_close()
        os.close(guard)
    return 0 if error is None else 1


if __name__ == "__main__":
    sys.exit(main())
