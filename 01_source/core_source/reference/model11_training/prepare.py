"""从来源目录整理基础图像、标签与训练清单。"""
from common import *
import collections, subprocess, shutil, math, re


def main():
    import cv2, numpy as np

    catalog = []
    listed = subprocess.run(
        ["rg", "--files", "pc_runs", "weights", "-g", "*.pt"],
        cwd=R,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    paths = [R / p for p in listed] + [R / "yolo11n.pt"]
    seen = set()
    for p in paths:
        key = str(p.resolve())
        if key in seen:
            continue
        seen.add(key)
        role = "historical_or_research_checkpoint_not_current_product"
        evidence = None
        if p.resolve() == (R / "yolo11n.pt").resolve():
            role = "generic_pretrained_rebuild_initializer"
        elif p.resolve() == (R / "pc_runs/m370_day_candidate/weights/best.pt").resolve():
            role = "FORMAL_RGB_M370_PRESERVE"
        elif "/rgb_rebuild_20261022_v1/" in p.as_posix():
            role = "new_rebuild_candidate_PC_only"
        elif "IR" in p.as_posix() or "ir_" in p.as_posix().lower():
            role = "IR_research_or_preserved_artifact_not_RGB_initializer"
        for parent in p.parents:
            if parent == R:
                break
            q = parent / "ROUND_CLOSED.json"
            if q.exists():
                d = read(q)
                evidence = dict(
                    path=str(q),
                    PC_pass=d.get("PC_pass"),
                    product_promoted=d.get("product_promoted"),
                )
                break
        catalog.append(
            dict(
                path=str(p),
                bytes=p.stat().st_size,
                SHA256=sha(p),
                role=role,
                decision_evidence=evidence,
            )
        )
    assert (
        sha(R / "yolo11n.pt") == "0ebbc80d4a7680d14987a577cd21342b65ecfd94632bd9a8da63ae6417644ee1"
    )
    write_new(
        W / "MODEL_CATALOG.json",
        dict(
            at=stamp(),
            records=catalog,
            policy="Index only,no file moves or deletions; role unknown remains historical/research,not falsely marked obsolete",
        ),
    )
    md = [
        "# 模型文件索引",
        "",
        f"共{len(catalog)}个权重；历史路径保持。当前正式RGB仅M370，重建从通用预训练权重开始。",
        "",
        "|角色|路径|MiB|SHA256前12位|",
        "|---|---|---:|---|",
    ]
    for r in sorted(catalog, key=lambda r: (r["role"], r["path"])):
        md.append(
            f"|{r['role']}|[{Path(r['path']).name}]({Path(r['path']).as_posix()})|{r['bytes']/1048576:.2f}|{r['SHA256'][:12]}|"
        )
    (W / "MODEL_CATALOG.md").write_text("\n".join(md), encoding="utf-8")
    prior = R / "pc_runs/model_round13_RGB_neck_20261020_v1"
    train = [
        dict(r, rebuild_role="TRAIN")
        for r in read(prior / "TRAIN_ROWS.json")
        if r["category"] in ["replay", "LI_replay"]
    ]
    dev = [
        dict(r, rebuild_role="DEV")
        for r in read(prior / "TRAIN_CHECK_ROWS.json")
        if r["category"] in ["held_day", "held_LI"]
    ]
    extra = read(R / "pc_runs/m328_detfly_candidate_v1/manifest.json")["records"]
    train.extend(
        dict(r, rebuild_role="TRAIN")
        for r in extra
        if r.get("category") == "DetFly_positive" and r["split"] == "train"
    )
    train_groups = {r["group"] for r in train}
    dev_groups = {r["group"] for r in dev}
    assert not train_groups & dev_groups
    assert not {r["image_sha256"] for r in train} & {r["image_sha256"] for r in dev}
    forbidden = [
        "V_DRONE_002",
        "V_DRONE_008",
        "V_DRONE_004",
        "V_DRONE_005",
        "V_DRONE_010",
        "V_DRONE_104",
        "UAV_CB",
        "reserved",
    ]
    assert not any(token in str(r["image"]) for r in train + dev for token in forbidden)
    data = W / "data"
    data.mkdir(exist_ok=False)
    manifest = []
    pixel_roles = {}
    seen_images = set()
    counts = collections.Counter()
    for split, rows in [("train", train), ("val", dev)]:
        images = data / "images" / split
        labels = data / "labels" / split
        images.mkdir(parents=True)
        labels.mkdir(parents=True)
        for i, row in enumerate(rows):
            src = Path(row["image"])
            lab = Path(row["label"])
            assert src.exists() and lab.exists()
            image_sha = sha(src)
            label_sha = sha(lab)
            assert image_sha == row["image_sha256"] and label_sha == row["label_sha256"]
            # Remove exact repeated training records only; never move across roles.
            identity = (split, image_sha, label_sha)
            if identity in seen_images:
                continue
            seen_images.add(identity)
            im = cv2.imread(str(src))
            assert im is not None and im.shape == (640, 640, 3) and im.dtype == np.uint8
            pixel_sha = hashlib.sha256(im.tobytes()).hexdigest()
            previous = pixel_roles.setdefault(pixel_sha, split)
            assert previous == split, "Cross-role decoded-pixel collision"
            truth = []
            for line in lab.read_text(encoding="utf-8-sig").splitlines():
                if not line.strip():
                    continue
                values = list(map(float, line.split()))
                assert len(values) == 5 and np.isfinite(values).all()
                k, x, y, w, h = values
                assert k == 0 and 0 <= x <= 1 and 0 <= y <= 1 and 0 < w <= 1 and 0 < h <= 1
                assert (
                    x - w / 2 >= -1e-6
                    and y - h / 2 >= -1e-6
                    and x + w / 2 <= 1 + 1e-6
                    and y + h / 2 <= 1 + 1e-6
                )
                truth.append(values)
            if row.get("present") is not None:
                assert bool(truth) == bool(row["present"])
            stem = f"{row['category']}__{image_sha[:16]}"
            dst = images / (stem + src.suffix.lower())
            target = labels / (stem + ".txt")
            assert not dst.exists()
            shutil.copy2(src, dst)
            shutil.copy2(lab, target)
            assert sha(dst) == image_sha and sha(target) == label_sha
            record = dict(
                role="TRAIN" if split == "train" else "DEV",
                image=str(dst),
                label=str(target),
                source_image=str(src),
                source_label=str(lab),
                image_SHA256=image_sha,
                label_SHA256=label_sha,
                decoded_pixels_SHA256=pixel_sha,
                group=row["group"],
                sequence=row.get("sequence"),
                frame_id=row.get("frame_id"),
                category=row["category"],
                GT_boxes=len(truth),
                known_truth=True,
            )
            manifest.append(record)
            counts[split + "_images"] += 1
            counts[split + "_GT"] += len(truth)
            counts[split + "_positive" if truth else split + "_negative"] += 1
        print("DATA_AUDITED", split, counts[split + "_images"], flush=True)
    yaml = f"path: {data.as_posix()}\ntrain: images/train\nval: images/val\nnames:\n  0: drone\n"
    (data / "dataset.yaml").write_text(yaml, encoding="utf-8")
    write_new(
        W / "DATA_MANIFEST.json",
        dict(
            at=stamp(),
            records=manifest,
            counts=dict(counts),
            TRAIN_groups=sorted(train_groups),
            DEV_groups=sorted(dev_groups),
            group_disjoint=True,
            decoded_pixel_disjoint=True,
            role="Rebuilt TRAIN and exposed held-group DEV,not independent TEST",
            source_manifests={
                str(p): sha(p)
                for p in [
                    prior / "TRAIN_ROWS.json",
                    prior / "TRAIN_CHECK_ROWS.json",
                    R / "pc_runs/m328_detfly_candidate_v1/manifest.json",
                ]
            },
            confuser_specialist_training_excluded=True,
            natural_known_empty_RGB_frames_retained=True,
            unknown_as_negative=False,
            source_files_moved_or_deleted=0,
        ),
    )
    write_new(
        W / "DATA_AUDIT.json",
        dict(
            at=stamp(),
            counts=dict(counts),
            image_shape_UINT8_640_all=True,
            all_source_copy_SHA256_exact=True,
            all_labels_singleclass_finite_normalized=True,
            TRAIN_DEV_groups_disjoint=True,
            TRAIN_DEV_decoded_pixels_disjoint=True,
            DATA_MANIFEST_SHA256=sha(W / "DATA_MANIFEST.json"),
            dataset_yaml_SHA256=sha(data / "dataset.yaml"),
            limitations=[
                "DEV has previous project development exposure; new generic initializer removes oldtask-training chain,not unknown generic-pretraining exposure.",
                "DetFly capture groups unknown,whole DetFly includedTRAINonly; no independent familygeneralization claim.",
                "This is bootstrap data,not final dataset; expand source diversity if error analysis warrants.",
            ],
        ),
    )
    plan = dict(
        at=stamp(),
        trial="baseline_standard_01",
        initial=dict(path=str(R / "yolo11n.pt"), SHA256=sha(R / "yolo11n.pt")),
        data=str(data / "dataset.yaml"),
        data_manifest_SHA256=sha(W / "DATA_MANIFEST.json"),
        dataset_yaml_SHA256=sha(data / "dataset.yaml"),
        start_not_before="2026-10-22T01:20:00+08:00",
        handoff_deadline="2026-10-22T09:00:00+08:00",
        latest_new_run="2026-10-22T08:20:00+08:00",
        hypothesis="Full detector learning from generic pretrained initialization with standard detection loss can restore core drone confidence/localization without legacy suppression losses.",
        params=dict(
            epochs=12,
            imgsz=640,
            batch=16,
            device=0,
            workers=0,
            seed=20261022,
            deterministic=True,
            optimizer="AdamW",
            lr0=0.003,
            lrf=0.05,
            weight_decay=0.0005,
            warmup_epochs=3.0,
            mosaic=0.5,
            close_mosaic=3,
            scale=0.3,
            translate=0.05,
            hsv_h=0.015,
            hsv_s=0.4,
            hsv_v=0.25,
            fliplr=0.5,
            amp=False,
            cache=False,
            patience=12,
            val=True,
            save=True,
            plots=False,
            verbose=False,
            freeze=None,
            quantize=32,
        ),
        standard_loss_only=True,
        max_wall_seconds=3600,
        selection="Standard framework best DEV mAP50-95 within this single run; report fixed .25/.7/.5 P/R/F1 separately; no independent-test checkpoint search.",
        next_stage="Do not stop overall window after this run. Analyze DEV and trainhealth,then freeze a new meaningful continuation/data-adjustment plan; generic-start baseline remains the control.",
        code_SHA256={str(p): sha(p) for p in [W / "train.py", W / "launch.py", W / "common.py"]},
        product_replacement=False,
        board_calls=0,
        external_test_for_selection=False,
    )
    write_new(W / "BASELINE_PLAN.json", plan)
    update(
        W / "WORK_STATE.json",
        dict(
            at=stamp(),
            status="READY_WAITING_20261022_0120",
            model_catalog_records=len(catalog),
            data_counts=dict(counts),
            baseline_plan=str(W / "BASELINE_PLAN.json"),
            active=None,
            new_training_updates=0,
            heartbeat="rgb-01-20-09-00",
            next="At01:20 launch baseline once,monitor and continue improvement until09:00; do not train earlier",
        ),
    )
    print("PREPARATION_READY", len(catalog), dict(counts), flush=True)


if __name__ == "__main__":
    main()
