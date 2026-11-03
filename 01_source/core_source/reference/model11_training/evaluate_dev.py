"""按固定开发清单运行检测并统计结果。"""

from common import *
import argparse, sys, time, traceback, msvcrt, collections


# 运行固定开发图像，按来源汇总检测指标。
def main():
    import cv2, numpy as np, torch
    from ultralytics import YOLO

    parser = argparse.ArgumentParser()
    parser.add_argument("--trial", required=True)
    parser.add_argument("--manifest", default=str(W / "DATA_MANIFEST.json"))
    args = parser.parse_args()
    trial = Path(args.trial).resolve()
    assert trial.is_relative_to(W / "runs")
    trained = read(trial / "TRAINING_RESULT.json")
    weight = next(x for x in trained["weights"] if Path(x["path"]).name == "best.pt")
    assert sha(weight["path"]) == weight["SHA256"]
    assert not (trial / "DEV_EVALUATION_STARTED.json").exists()
    manifest_path = Path(args.manifest).resolve()
    assert manifest_path.is_relative_to(W)
    from data_role_guard import assert_development_manifest

    assert_development_manifest(manifest_path)
    records = [r for r in read(manifest_path)["records"] if r["role"] == "DEV"]
    assert records
    lock = (W / "gpu_owner.lock").open("a+b")
    lock.seek(0)
    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
    try:
        sys.path.insert(0, str(R / "pc_runs/model_capability_freeze_20261021_v1"))
        from scoring import score

        write_new(
            trial / "DEV_EVALUATION_STARTED.json",
            dict(
                at=stamp(),
                candidate=weight,
                data_manifest_path=str(manifest_path),
                data_manifest_SHA256=sha(manifest_path),
                rules=dict(conf=0.25, NMS=0.7, match_IoU=0.5, max_det=300),
                role="Exposed held-group DEV,no independent claim",
                candidate_selection="best.pt selected bystandardDEV mAP50-95,no checkpointscan",
            ),
        )
        torch.set_num_threads(4)
        cv2.setNumThreads(1)
        start = time.monotonic()
        results = {}
        forwards = {}
        for name, path in [
            ("M370", str(R / "pc_runs/m370_day_candidate/weights/best.pt")),
            ("candidate", weight["path"]),
        ]:
            model = YOLO(path)
            rows = []
            counts = []
            handle = model.model.register_forward_pre_hook(
                lambda m, a: counts.append(int(a[0].shape[0]))
            )
            with (trial / f"DEV_{name}_PREDICTIONS.jsonl").open("x", encoding="utf-8") as output:
                for offset in range(0, len(records), 16):
                    assert time.monotonic() - start < 900, "DEVwallbudget"
                    chunk = records[offset : offset + 16]
                    images = []
                    for row in chunk:
                        assert (
                            sha(row["image"]) == row["image_SHA256"]
                            and sha(row["label"]) == row["label_SHA256"]
                        )
                        im = cv2.imread(row["image"])
                        assert im.shape == (640, 640, 3)
                        images.append(im)
                    predicted = model.predict(
                        images,
                        imgsz=640,
                        rect=False,
                        conf=0.25,
                        iou=0.7,
                        max_det=300,
                        device=0,
                        quantize=32,
                        verbose=False,
                        save=False,
                        augment=False,
                    )
                    for source, prediction in zip(chunk, predicted):
                        gt = []
                        for line in Path(source["label"]).read_text().splitlines():
                            if not line.strip():
                                continue
                            k, x, y, w, h = map(float, line.split())
                            gt.append(
                                dict(
                                    xyxy=[
                                        (x - w / 2) * 640,
                                        (y - h / 2) * 640,
                                        (x + w / 2) * 640,
                                        (y + h / 2) * 640,
                                    ],
                                    **{"class": int(k)},
                                )
                            )
                        ds = [
                            dict(
                                xyxy=[float(x), float(y), float(u), float(v)],
                                confidence=float(c),
                                **{"class": int(k)},
                            )
                            for x, y, u, v, c, k in prediction.boxes.data.cpu().tolist()
                        ]
                        row = dict(
                            id=source["image"],
                            group=source["group"],
                            category=source["category"],
                            valid_truth=True,
                            truth=gt,
                            detections=ds,
                            image_SHA256=source["image_SHA256"],
                        )
                        rows.append(row)
                        output.write(json.dumps(row, allow_nan=False) + "\n")
            handle.remove()
            results[name] = dict(
                all=score(rows),
                groups={
                    g: score([r for r in rows if r["group"] == g])
                    for g in sorted({r["group"] for r in rows})
                },
                categories={
                    g: score([r for r in rows if r["category"] == g])
                    for g in sorted({r["category"] for r in rows})
                },
            )
            forwards[name] = sum(counts)
            del model
            torch.cuda.empty_cache()
            print("DEV_EVALUATED", name, results[name]["all"], flush=True)
        write_new(
            trial / "DEV_EVALUATION.json",
            dict(
                at=stamp(),
                metrics=results,
                instrumented_forward_samples=forwards,
                elapsed_s=time.monotonic() - start,
                role="Exposed heldgroupDEV",
                training_updates=0,
                product_replaced=False,
            ),
        )
        update(
            W / "WORK_STATE.json",
            dict(
                at=stamp(),
                status="BASELINE_DEV_EVALUATED_CONTINUE_WINDOW",
                active=None,
                last_trial=str(trial),
                last_evaluation=str(trial / "DEV_EVALUATION.json"),
                next="Analyze core recall/localization and source gaps under the current authorized plan; freeze next meaningful experiment.",
            ),
        )
    finally:
        lock.seek(0)
        msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
        lock.close()


if __name__ == "__main__":
    main()
