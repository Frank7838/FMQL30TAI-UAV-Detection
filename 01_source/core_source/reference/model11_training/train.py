"""读取训练计划，运行Ultralytics训练并保存权重。"""

from common import *
import argparse, sys, time, traceback, msvcrt, importlib.metadata, base64


# 读取计划和样本清单，创建训练器并保存进度。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", required=True)
    parser.add_argument("--trial", required=True)
    args = parser.parse_args()
    planpath = Path(args.plan)
    p = read(planpath)
    trial = Path(args.trial)
    assert trial.is_relative_to(W / "runs")
    assert now() >= datetime.datetime.fromisoformat(
        p["start_not_before"]
    ) and now() < datetime.datetime.fromisoformat(p["handoff_deadline"])
    lock = (W / "gpu_owner.lock").open("a+b")
    lock.seek(0)
    lock.write(b"1")
    lock.flush()
    lock.seek(0)
    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
    try:
        assert (
            sha(p["initial"]["path"]) == p["initial"]["SHA256"]
            and sha(p["data"]) == p["dataset_yaml_SHA256"]
        )
        manifest_path = Path(p.get("data_manifest_path", W / "DATA_MANIFEST.json"))
        from data_role_guard import assert_development_manifest

        assert_development_manifest(manifest_path, p)
        assert sha(manifest_path) == p["data_manifest_SHA256"]
        for name, h in p["code_SHA256"].items():
            assert sha(name) == h
        for r in read(manifest_path)["records"]:
            assert sha(r["image"]) == r["image_SHA256"] and sha(r["label"]) == r["label_SHA256"]
        for filename, digest in p.get("data_file_SHA256", {}).items():
            assert sha(filename) == digest
        import torch, cv2
        from ultralytics import YOLO
        from ultralytics.utils.loss import v8DetectionLoss

        assert torch.cuda.is_available()
        torch.set_num_threads(4)
        cv2.setNumThreads(1)
        # 核对训练环境中Ultralytics核心文件与安装记录。
        dist = importlib.metadata.distribution("ultralytics")
        targets = {
            "ultralytics/utils/loss.py",
            "ultralytics/engine/trainer.py",
            "ultralytics/nn/tasks.py",
            "ultralytics/data/dataset.py",
        }
        checks = {}
        for f in dist.files:
            if str(f).replace("\\", "/") in targets:
                digest = (
                    base64.urlsafe_b64encode(
                        hashlib.sha256(dist.locate_file(f).read_bytes()).digest()
                    )
                    .decode()
                    .rstrip("=")
                )
                assert f.hash and digest == f.hash.value
                checks[str(f)] = digest
        assert len(checks) == 4
        write_new(
            trial / "STARTED.json",
            dict(
                at=stamp(),
                pid=os.getpid(),
                plan_SHA256=sha(planpath),
                torch=torch.__version__,
                ultralytics=dist.version,
                GPU=torch.cuda.get_device_name(0),
                package_RECORD_checks=checks,
            ),
        )
        start = time.monotonic()
        counts = {"batches": 0, "optimizer_updates": 0}
        deadline = datetime.datetime.fromisoformat(p["handoff_deadline"])
        model = YOLO(p["initial"]["path"])

        # 记录损失函数和参与训练的参数。
        def on_start(t):
            assert t.model.nc == 1 and not hasattr(t.model, "teacher")
            # 使用Ultralytics的标准检测损失函数。
            criterion = t.model.init_criterion()
            assert type(criterion) is v8DetectionLoss
            t.model.criterion = criterion
            trainable = [name for name, value in t.model.named_parameters() if value.requires_grad]
            assert (
                any(name.startswith("model.0.") for name in trainable)
                and any("cv2." in name for name in trainable)
                and any("cv3." in name for name in trainable)
            )
            write_new(
                trial / "STANDARD_LOSS_PROOF.json",
                dict(
                    at=stamp(),
                    criterion_module=type(criterion).__module__,
                    criterion_class=type(criterion).__name__,
                    trainable_parameter_names=trainable,
                    no_custom_background_term=True,
                    no_teacher=True,
                ),
            )

        # 累计批次，并按计划时限结束训练。
        def on_batch(t):
            counts["batches"] += 1
            if now() >= deadline or time.monotonic() - start >= p["max_wall_seconds"]:
                t.stop = True

        # 保存当前轮次的指标和优化器更新数。
        def on_epoch(t):
            steps = [int(s["step"]) for s in t.optimizer.state.values() if "step" in s]
            counts["optimizer_updates"] = max(steps) if steps else 0
            update(
                trial / "PROGRESS.json",
                dict(
                    at=stamp(),
                    pid=os.getpid(),
                    epoch_zero_based=t.epoch,
                    epochs_requested=t.epochs,
                    counts=counts,
                    elapsed_s=time.monotonic() - start,
                    metrics={str(k): float(v) for k, v in (t.metrics or {}).items()},
                    stop_requested=bool(t.stop),
                ),
            )

        for event, callback in [
            ("on_train_start", on_start),
            ("on_train_batch_end", on_batch),
            ("on_fit_epoch_end", on_epoch),
        ]:
            model.add_callback(event, callback)
        model.train(data=p["data"], project=str(trial), name="train", exist_ok=False, **p["params"])
        weights = []
        for name in ["best.pt", "last.pt"]:
            path = trial / "train/weights" / name
            if path.exists():
                weights.append(dict(path=str(path), SHA256=sha(path), bytes=path.stat().st_size))
        assert weights
        write_new(
            trial / "TRAINING_RESULT.json",
            dict(
                at=stamp(),
                counts=counts,
                elapsed_s=time.monotonic() - start,
                weights=weights,
                product_replaced=False,
                board_commands=0,
                next="Evaluate core P/R/F1 on fixedDEV,analyze failures and freeze next meaningful experiment within overnightwindow",
            ),
        )
        update(
            W / "WORK_STATE.json",
            dict(
                at=stamp(),
                status="BASELINE_TRAINING_DONE_CONTINUE_WINDOW",
                active=None,
                last_trial=str(trial),
                next="Do not end overnight task: assessDEV,then standard continuation or justified data expansion; final report09:00",
            ),
        )
    finally:
        lock.seek(0)
        msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
        lock.close()


if __name__ == "__main__":
    try:
        main()
    except BaseException:
        error = traceback.format_exc()
        print(error, file=sys.stderr)
        try:
            args = sys.argv
            trial = Path(args[args.index("--trial") + 1])
            write_new(
                trial / "FAILURE.json",
                dict(at=stamp(), error=error, automatic_training_retry=False),
            )
            update(
                W / "WORK_STATE.json",
                dict(
                    at=stamp(),
                    status="TRIAL_FAILED_NEEDS_AGENT_ADAPTATION",
                    active=None,
                    last_trial=str(trial),
                    next="Inspect preserved failure,fix specific issue or revise hypothesis; do not stop overallwindow",
                ),
            )
        except Exception:
            pass
        raise
