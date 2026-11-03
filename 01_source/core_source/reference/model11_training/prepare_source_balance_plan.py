"""生成来源平衡阶段的训练计划。"""

from common import *
import shutil


def main():
    parent = read(W / "MULTISOURCE_PLAN.json")
    trial = W / "runs/standard_multisource_03"
    assert read(trial / "POSTPROCESS_STATUS.json")["state"] == "COMPLETED"
    diagnosis = read(trial / "REMAINING_DIAGNOSIS.json")
    assert diagnosis["actual_forwards"] == 36
    selected = [
        row
        for row in diagnosis["summaries"]
        if row["model"] == trial.name and row["sequence"] == "V_DRONE_104"
    ]
    assert len(selected) == 6 and all(
        row["best_matching_score"] is not None and row["best_matching_score"] < 0.25
        for row in selected
    )
    source = W / "multisource_data_v1"
    manifest = read(source / "DATA_MANIFEST.json")
    destination = W / "source_balance_v1"
    destination.mkdir(exist_ok=False)
    exposures = []
    source_count = 0
    for record in manifest["records"]:
        if record["role"] != "TRAIN":
            continue
        repeats = 10 if record["category"] == "Halmstad_drone_TRAIN" else 1
        exposures.extend([record["image"]] * repeats)
        source_count += record["category"] == "Halmstad_drone_TRAIN"
    assert len(set(exposures)) == 6734 and len(exposures) == 10091 and source_count == 373
    (destination / "train.txt").write_text(
        "\n".join(Path(path).as_posix() for path in exposures) + "\n", encoding="utf-8"
    )
    shutil.copyfile(source / "val.txt", destination / "val.txt")
    assert sha(source / "val.txt") == sha(destination / "val.txt")
    (destination / "dataset.yaml").write_text(
        f"path: {destination.as_posix()}\ntrain: train.txt\nval: val.txt\nnames:\n  0: drone\n",
        encoding="utf-8",
    )
    manifest.update(
        at=stamp(),
        train_exposures=10091,
        source_exposure_repetitions=10,
        parent_manifest_SHA256=sha(source / "DATA_MANIFEST.json"),
        dataset_change="Same unique images, labels and roles; Halmstad TRAIN exposure increases from 6 to 10. DEV unchanged.",
    )
    write_new(destination / "DATA_MANIFEST.json", manifest)
    result = read(trial / "TRAINING_RESULT.json")
    weight = next(item for item in result["weights"] if Path(item["path"]).name == "best.pt")
    assert sha(weight["path"]) == weight["SHA256"]
    current = now()
    plan = dict(parent)
    plan.update(
        at=stamp(),
        trial="standard_source_balance_04",
        initial=weight,
        data=str(destination / "dataset.yaml"),
        data_manifest_path=str(destination / "DATA_MANIFEST.json"),
        evaluation_manifest_path=str(destination / "DATA_MANIFEST.json"),
        data_manifest_SHA256=sha(destination / "DATA_MANIFEST.json"),
        dataset_yaml_SHA256=sha(destination / "dataset.yaml"),
        data_file_SHA256={
            str(destination / name): sha(destination / name) for name in ["train.txt", "val.txt"]
        },
        train_exposures=10091,
        start_not_before=current.isoformat(),
        latest_new_run=(current + datetime.timedelta(minutes=10)).isoformat(),
        handoff_deadline=(current + datetime.timedelta(minutes=75)).isoformat(),
        max_wall_seconds=3300,
        hypothesis="Multisource training reduced false boxes but attenuated matching scores in six sampled 104 misses and left localization errors. Restore the previously useful drone-video exposure share from 26% to 37%, retain all DUT data, and use low-rate standard full-detector refinement without mosaic.",
        observed_errors=str(trial / "REMAINING_DIAGNOSIS.json"),
        comparison_limit="Same 1629 DEV and same 1209 exposed diagnostic frames permit matched-set comparison; exposure ratio, lower learning rate, augmentation and further training change together, so no single-factor causal claim.",
        selection="Best fixed 1629-image exposed DEV mAP50-95; final best only on original 1163 subset and four exposed clips. Product confidence .25/NMS .7/IoU .5 unchanged.",
        next_stage="Compare recall retention, precision and localization to 03 on the same denominators; if no useful gain, keep 03 and diagnose rather than repeating the same configuration.",
    )
    plan["params"] = dict(parent["params"])
    plan["params"].update(
        epochs=12, lr0=0.00012, lrf=0.2, warmup_epochs=1.0, mosaic=0.0, close_mosaic=0, patience=12
    )
    plan["code_SHA256"] = {
        str(W / name): sha(W / name) for name in ["train.py", "launch.py", "common.py"]
    }
    plan["evaluation_code_SHA256"] = {
        filename: sha(filename) for filename in parent["evaluation_code_SHA256"]
    }
    write_new(W / "SOURCE_BALANCE_PLAN.json", plan)
    print("SOURCE_BALANCE_PLAN_FROZEN", plan["trial"], plan["train_exposures"], flush=True)


if __name__ == "__main__":
    main()
