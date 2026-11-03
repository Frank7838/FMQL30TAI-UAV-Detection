"""生成多来源训练配置与重复采样计划。"""
from common import *


def main():
    source_trial = W / "runs/standard_source_adapt_02"
    summary = read(source_trial / "STAGE_SUMMARY.json")
    assert summary["exposed_development"]["R"] > summary["baseline_exposed"]["R"]
    assert summary["exposed_development"]["F1"] > summary["baseline_exposed"]["F1"]
    trained = read(source_trial / "TRAINING_RESULT.json")
    weight = next(item for item in trained["weights"] if Path(item["path"]).name == "best.pt")
    assert sha(weight["path"]) == weight["SHA256"]
    data = W / "multisource_data_v1"
    audit = read(data / "DATA_AUDIT.json")
    assert audit["train_unique"] == 6734 and audit["dev_unique"] == 1629
    parameters = read(W / "SOURCE_ADAPT_PLAN.json")["params"]
    parameters.update(
        epochs=20, lr0=0.00035, lrf=0.1, warmup_epochs=1.0, mosaic=0.25, close_mosaic=4, patience=20
    )
    current = now()
    plan = dict(
        at=stamp(),
        trial="standard_multisource_03",
        initial=weight,
        data=str(data / "dataset.yaml"),
        data_manifest_path=str(data / "DATA_MANIFEST.json"),
        data_manifest_SHA256=sha(data / "DATA_MANIFEST.json"),
        dataset_yaml_SHA256=sha(data / "dataset.yaml"),
        data_file_SHA256={str(data / name): sha(data / name) for name in ["train.txt", "val.txt"]},
        evaluation_manifest_path=str(data / "DATA_MANIFEST.json"),
        start_not_before=current.isoformat(),
        latest_new_run=(current + datetime.timedelta(minutes=10)).isoformat(),
        handoff_deadline=(current + datetime.timedelta(minutes=85)).isoformat(),
        max_wall_seconds=3600,
        params=parameters,
        standard_loss_only=True,
        authorization="User explicitly requested continued PC optimization; hardware remains off.",
        hypothesis="Source adaptation recovered drone recall, but 104 contains 550 false boxes and 004 still misses localization. Add existing admitted DUT positives with varied targets/backgrounds and a broader fixed DEV to learn drone identity and geometry beyond the current narrow source.",
        observed_errors=str(source_trial / "ERROR_PROFILE.json"),
        selection="Best mAP50-95 on the frozen 1629-image exposed DEV; one final best checkpoint evaluated on the original 1163-image subset and the four exposed diagnostic videos. No checkpoint sweep on Halmstad diagnostics.",
        comparison_limit="DEV expands from 1163 to 1629, so aggregate scores cannot be directly called old-set improvement. Report original 1163 subset and four-clip counts separately. This experiment changes data coverage, budget and further training; no single-factor causal claim.",
        data_roles="DUT existing TRAIN/DEV preserved with similarity-component isolation; unknown true capture identity. Halmstad TRAIN remains six original clips, evaluated clips never become TRAIN. All DEV already exposed; no independent TEST claim.",
        train_unique=6734,
        train_exposures=8599,
        dev_unique=1629,
        next_stage="Inspect complete fixed evaluations, source/scale errors and false outputs, then freeze a justified next finite action.",
        product_replacement=False,
        board_calls=0,
        external_test_for_selection=False,
    )
    plan["code_SHA256"] = {
        str(W / name): sha(W / name) for name in ["train.py", "launch.py", "common.py"]
    }
    evaluation_files = [
        W / name
        for name in ["continue_after_training.py", "evaluate_dev.py", "evaluate_exposed.py"]
    ]
    evaluation_files.append(R / "pc_runs/model_capability_freeze_20261021_v1/scoring.py")
    plan["evaluation_code_SHA256"] = {str(path): sha(path) for path in evaluation_files}
    write_new(W / "MULTISOURCE_PLAN.json", plan)
    print("MULTISOURCE_PLAN_FROZEN", plan["trial"], plan["handoff_deadline"], flush=True)


if __name__ == "__main__":
    main()
