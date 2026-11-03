"""逐张运行开发图像并保存NPU输出。"""
from pathlib import Path
import datetime, fcntl, hashlib, json, os, sys, time, traceback
import cv2
from worker import Hardware, NPU
from run_camera import ROOT, RUNTIME, verify_payload, save


# 读取预设输入，运行推理并保存状态。
def main():
    cv2.setNumThreads(1)
    plan = json.loads((ROOT / "qualification_dev1629/INPUT_MANIFEST.json").read_text())
    assert plan["role"] == "EXPOSED_DEV_NOT_TEST" and len(plan["rows"]) == 1629
    assert Path("/proc/sys/kernel/random/boot_id").read_text().strip() == plan["expected_boot"]
    verify_payload()
    assert not (ROOT / "dev1629_execution").exists()
    fd = os.open("/root/fpga_gaming/ps_session_m53.lock", os.O_RDWR | os.O_NOFOLLOW)
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    assert os.fstat(fd).st_ino == 943514
    out = ROOT / "dev1629_execution"
    out.mkdir()
    hw = None
    npu = None
    closed = False
    done = 0
    error = None
    start = time.monotonic()
    try:
        hw = Hardware(fd)
        before = hw.snapshot()
        npu = NPU.__new__(NPU)
        NPU.__init__(
            npu, fd, dict(runtime_env=RUNTIME, boot=plan["expected_boot"], max_forwards=1629), out
        )
        with (out / "PREDICTIONS.jsonl").open("x") as f:
            for row in plan["rows"]:
                assert time.monotonic() - start < 210, "Bounded DEV observation exceeded"
                path = ROOT / "qualification_dev1629/input" / row["name"]
                assert hashlib.sha256(path.read_bytes()).hexdigest() == row["image_SHA256"]
                im = cv2.imread(str(path))
                assert im.shape == (640, 640, 3)
                detections, stages = npu.infer(im)
                f.write(
                    json.dumps(
                        dict(
                            name=row["name"],
                            image_SHA256=row["image_SHA256"],
                            detections=detections,
                            stages_ms=stages,
                        ),
                        allow_nan=False,
                    )
                    + "\n"
                )
                f.flush()
                done += 1
                if done % 100 == 0:
                    print("DEV_NPU_FRAMES", done, flush=True)
        after = hw.snapshot()
        assert all(before[k] == after[k] for k in ["192", "36", "40", "44", "48"])
        assert (before["32"] & 1) == (after["32"] & 1)
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
            completed=error is None,
            frames=done,
            NPU_calls=npu.calls if npu else 0,
            device_closed=closed,
            error=error,
            elapsed_s=time.monotonic() - start,
            role="EXPOSED_DEV_NOT_TEST",
            TEST=0,
            training=0,
            PL_business_writes=0,
            SDK_device_reset_per_forward=True,
        )
        save(out / "RESULT.json", result)
        print(json.dumps(result), flush=True)
        if not closed:
            save(
                ROOT / "UNKNOWN.json",
                dict(
                    pid=os.getpid(),
                    boot=plan["expected_boot"],
                    child=npu.p.pid if npu and npu.p else None,
                ),
            )
            while True:
                time.sleep(1)
        if hw:
            hw.lib.switch_close()
        os.close(fd)
    return 0 if error is None else 1


if __name__ == "__main__":
    sys.exit(main())
