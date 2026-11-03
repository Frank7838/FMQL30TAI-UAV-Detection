"""按原标签提取已选视频帧并加入训练清单。"""

from common import *
import collections
import shutil


def main():
    import cv2
    import numpy as np

    root = W / "source_adaptation"
    root.mkdir(exist_ok=False)
    images = root / "images/train"
    labels = root / "labels/train"
    images.mkdir(parents=True)
    labels.mkdir(parents=True)
    base = read(W / "DATA_MANIFEST.json")
    records = list(base["records"])
    pixels = {record["decoded_pixels_SHA256"] for record in records}
    prior = read(R / "pc_runs/m429_no_OSD_video_day_v1/manifest.json")
    sources = read(R / "pc_runs/m428_no_OSD_video_sources_v1/parsed_clips.json")
    expected_names = {
        "V_DRONE_003",
        "V_DRONE_009",
        "V_DRONE_013",
        "V_DRONE_042",
        "V_DRONE_054",
        "V_DRONE_056",
    }
    assert {source["name"] for source in sources} == expected_names
    protected = {
        str(path): sha(path)
        for path in [
            R / "pc_runs/m429_no_OSD_video_day_v1/manifest.json",
            R / "pc_runs/m428_no_OSD_video_sources_v1/parsed_clips.json",
        ]
    }
    added = []
    cv2.setNumThreads(1)
    for source in sources:
        protected[source["video"]] = sha(source["video"])
        protected[source["labels"]] = sha(source["labels"])
        selected = {
            record["frame_id"]: record
            for record in prior["records"]
            if record["sequence"] == source["name"]
        }
        capture = cv2.VideoCapture(source["video"])
        for reference in source["records"]:
            success, frame = capture.read()
            assert (
                success and hashlib.sha256(frame.tobytes()).hexdigest() == reference["decoded_SHA"]
            )
            if reference["frame_id"] not in selected:
                continue
            old = selected[reference["frame_id"]]
            image = cv2.imread(old["image"])
            expected = cv2.copyMakeBorder(
                frame, 0, 128, 0, 0, cv2.BORDER_CONSTANT, value=(114, 114, 114)
            )
            assert image.shape == (640, 640, 3) and np.array_equal(image, expected)
            digest = hashlib.sha256(image.tobytes()).hexdigest()
            assert digest == old["prepared_decoded_SHA"] and digest not in pixels
            pixels.add(digest)
            truth = [
                list(map(float, line.split()))
                for line in Path(old["label"]).read_text().splitlines()
                if line.strip()
            ]
            assert len(truth) == len(reference["boxes"]) == 1
            label, center_x, center_y, width, height = truth[0]
            assert label == 0 and np.isfinite(truth).all() and width > 0 and height > 0
            actual_box = [
                (center_x - width / 2) * 640,
                (center_y - height / 2) * 640,
                (center_x + width / 2) * 640,
                (center_y + height / 2) * 640,
            ]
            assert np.max(np.abs(np.array(actual_box) - np.array(reference["boxes"][0]))) < 1e-5
            target_image = images / Path(old["image"]).name
            target_label = labels / Path(old["label"]).name
            shutil.copyfile(old["image"], target_image)
            shutil.copyfile(old["label"], target_label)
            record = dict(
                role="TRAIN",
                image=str(target_image),
                label=str(target_label),
                image_SHA256=sha(target_image),
                label_SHA256=sha(target_label),
                decoded_pixels_SHA256=digest,
                source_video=source["video"],
                source_decoded_SHA256=reference["decoded_SHA"],
                frame_id=reference["frame_id"],
                sequence=source["name"],
                group="Halmstad_existing_TRAIN_capture_unknown",
                category="Halmstad_drone_TRAIN",
                GT_boxes=1,
                known_truth=True,
            )
            records.append(record)
            added.append(record["image"])
        assert not capture.read()[0]
        capture.release()
    assert len(added) == 373
    train = [record["image"] for record in base["records"] if record["role"] == "TRAIN"] + added * 6
    dev = [record["image"] for record in base["records"] if record["role"] == "DEV"]
    assert not set(train) & set(dev)
    train_groups = {record["group"] for record in records if record["role"] == "TRAIN"}
    dev_groups = {record["group"] for record in records if record["role"] == "DEV"}
    assert not train_groups & dev_groups
    for name, entries in [("train", train), ("val", dev)]:
        (root / (name + ".txt")).write_text(
            "\n".join(Path(path).as_posix() for path in entries) + "\n", encoding="utf-8"
        )
    yaml = root / "dataset.yaml"
    yaml.write_text(
        f"path: {root.as_posix()}\ntrain: train.txt\nval: val.txt\nnames:\n  0: drone\n",
        encoding="utf-8",
    )
    manifest = root / "DATA_MANIFEST.json"
    write_new(
        manifest,
        dict(
            at=stamp(),
            records=records,
            train_unique=len(set(train)),
            train_exposures=len(train),
            dev_unique=len(dev),
            source_unique=len(added),
            source_exposure_repetitions=6,
            source_sequences=sorted(expected_names),
            protected_sources=protected,
            scope="Reuse previously admitted TRAIN source only; exact full frame/label rechecked; exposed evaluation videos excluded.",
            capture_independence="Unknown within Halmstad collection; all six source clips remain one TRAIN group.",
        ),
    )
    plan = read(W / "BASELINE_NOW_PLAN.json")
    plan.update(
        at=stamp(),
        trial="standard_source_adapt_02",
        data=str(yaml),
        data_manifest_path=str(manifest),
        data_manifest_SHA256=sha(manifest),
        dataset_yaml_SHA256=sha(yaml),
        data_file_SHA256={str(root / name): sha(root / name) for name in ["train.txt", "val.txt"]},
        start_not_before=stamp(),
        latest_new_run=(now() + datetime.timedelta(minutes=10)).isoformat(),
        handoff_deadline="2026-10-22T11:08:17+08:00",
        max_wall_seconds=3600,
        hypothesis="Local DEV is already saturated while cross-source diagnostic recall is poor; restore existing admitted drone-video TRAIN coverage with explicit source exposure and standard full-detector continuation.",
        comparison_limit="Data exposure and further training both change; this is an improvement experiment, not a source-only causal control.",
        selection="Best fixed 1163-image local DEV mAP50-95; one completed best checkpoint gets exposed-source diagnosis; no checkpoint search on exposed Halmstad clips.",
    )
    trained = read(W / "runs/baseline_standard_now_01/TRAINING_RESULT.json")
    plan["initial"] = next(
        item for item in trained["weights"] if Path(item["path"]).name == "best.pt"
    )
    plan["params"].update(
        epochs=24, lr0=0.0005, lrf=0.1, warmup_epochs=1.0, mosaic=0.25, close_mosaic=4, patience=24
    )
    plan["code_SHA256"] = {
        str(W / name): sha(W / name) for name in ["train.py", "launch.py", "common.py"]
    }
    write_new(W / "SOURCE_ADAPT_PLAN.json", plan)
    print("ADAPTATION_DATA_READY", len(set(train)), len(train), len(dev), flush=True)


if __name__ == "__main__":
    main()
