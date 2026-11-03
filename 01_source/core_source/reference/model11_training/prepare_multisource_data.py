"""将来源清单整理成训练与开发输入。"""

from common import *
import collections
import shutil


def main():
    import cv2
    import numpy as np

    destination = W / "multisource_data_v1"
    destination.mkdir(exist_ok=False)
    source = R / "pc_runs/m27_dut_audit_v1/admitted_manifest.json"
    original = read(source)
    base = read(W / "source_adaptation/DATA_MANIFEST.json")
    records = list(base["records"])
    pixels = {record["decoded_pixels_SHA256"]: record["role"] for record in records}
    training_components = {row["component_proxy"] for row in original if row["split"] == "train"}
    development_components = {row["component_proxy"] for row in original if row["split"] == "dev"}
    assert not training_components & development_components
    added = collections.Counter()
    duplicates = []
    cv2.setNumThreads(1)
    for row in original:
        assert row["split"] in ["train", "dev"]
        assert row["official_split"] == ("train" if row["split"] == "train" else "val")
        role = "TRAIN" if row["split"] == "train" else "DEV"
        assert sha(row["image"]) == row["image_sha256"] and sha(row["label"]) == row["label_sha256"]
        image = cv2.imread(row["image"])
        assert image is not None and image.shape == (640, 640, 3) and image.dtype == np.uint8
        digest = hashlib.sha256(image.tobytes()).hexdigest()
        if digest in pixels:
            assert pixels[digest] == role, "Cross-role pixel collision requires review"
            duplicates.append(dict(image=row["image"], role=role))
            continue
        truth = [
            list(map(float, line.split()))
            for line in Path(row["label"]).read_text().splitlines()
            if line.strip()
        ]
        assert len(truth) == len(row["truth"]) and truth
        for values, expected in zip(truth, row["truth"]):
            label, center_x, center_y, width, height = values
            assert label == 0 and np.isfinite(values).all() and width > 0 and height > 0
            box = [
                (center_x - width / 2) * 640,
                (center_y - height / 2) * 640,
                (center_x + width / 2) * 640,
                (center_y + height / 2) * 640,
            ]
            assert min(box) >= -1e-5 and max(box) <= 640 + 1e-5
            assert np.max(abs(np.array(box) - np.array(expected["xyxy"]))) < 1e-5
        pixels[digest] = role
        folder = "train" if role == "TRAIN" else "val"
        target_image = destination / "images" / folder / Path(row["image"]).name
        target_label = destination / "labels" / folder / Path(row["label"]).name
        target_image.parent.mkdir(parents=True, exist_ok=True)
        target_label.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(row["image"], target_image)
        shutil.copyfile(row["label"], target_label)
        assert sha(target_image) == row["image_sha256"] and sha(target_label) == row["label_sha256"]
        records.append(
            dict(
                role=role,
                image=str(target_image),
                label=str(target_label),
                image_SHA256=row["image_sha256"],
                label_SHA256=row["label_sha256"],
                decoded_pixels_SHA256=digest,
                category="DUT_drone",
                group="DUT_similarity_component_" + str(row["component_proxy"]),
                GT_boxes=len(truth),
                known_truth=True,
                source_image=row["source_image"],
                official_split=row["official_split"],
                capture_session_id=None,
            )
        )
        added[role] += 1
    train = []
    development = []
    for record in records:
        if record["role"] == "TRAIN":
            repeats = 6 if record["category"] == "Halmstad_drone_TRAIN" else 1
            train.extend([record["image"]] * repeats)
        else:
            development.append(record["image"])
    assert not set(train) & set(development)
    for name, entries in [("train", train), ("val", development)]:
        (destination / (name + ".txt")).write_text(
            "\n".join(Path(path).as_posix() for path in entries) + "\n", encoding="utf-8"
        )
    (destination / "dataset.yaml").write_text(
        f"path: {destination.as_posix()}\ntrain: train.txt\nval: val.txt\nnames:\n  0: drone\n",
        encoding="utf-8",
    )
    summary = dict(
        at=stamp(),
        records=records,
        train_unique=len(set(train)),
        train_exposures=len(train),
        dev_unique=len(development),
        added_DUT=dict(added),
        skipped_same_role_duplicates=duplicates,
        original_admission_SHA256=sha(source),
        source_role="Existing admitted DUT TRAIN stays TRAIN, admitted DUT DEV stays DEV; no new test access.",
        limitations=[
            "DUT components are visual similarity proxies, not verified capture identities.",
            "All DEV has historical development exposure and is not independent TEST.",
            "Original partial visual admission reused; every copied image and label checked numerically.",
            "Preparation only; no model selection, training or evaluation has used this expanded dataset yet.",
        ],
    )
    write_new(destination / "DATA_MANIFEST.json", summary)
    write_new(
        destination / "DATA_AUDIT.json",
        {key: value for key, value in summary.items() if key != "records"},
    )
    print(
        "MULTISOURCE_DATA_READY",
        summary["train_unique"],
        summary["train_exposures"],
        summary["dev_unique"],
        dict(added),
        flush=True,
    )


if __name__ == "__main__":
    main()
