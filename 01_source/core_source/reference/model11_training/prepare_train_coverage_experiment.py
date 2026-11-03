"""准备来源覆盖与重复采样的训练输入。"""

from common import *
import copy, collections
from data_role_guard import assert_development_manifest


def main():
    audit = W / "train_hardcase_audit_v1"
    assert read(audit / "RESULT.json")["forward_samples"] == 1986
    review = read(audit / "visual_review/RESULT.json")
    assert len(review["pages"]) == 4 and review["scenes"] == 15
    parent = W / "icg_outdoor_data_v1/DATA_MANIFEST.json"
    manifest = copy.deepcopy(read(parent))
    assert_development_manifest(parent)
    dest = W / "train_coverage_balance_v1"
    dest.mkdir(exist_ok=False)
    repeats = {
        "replay": 1,
        "LI_replay": 1,
        "DUT_drone": 2,
        "DetFly_positive": 4,
        "Halmstad_drone_TRAIN": 4,
        "ICG_outdoor_drone_TRAIN": 6,
    }
    train = []
    dev = []
    for r in manifest["records"]:
        if r["role"] == "TRAIN":
            train.extend([r["image"]] * repeats[r["category"]])
        else:
            dev.append(r["image"])
    assert len(set(train)) == 6892 and len(train) == 11680 and len(dev) == 1629
    manifest.update(
        at=stamp(),
        train_exposures=len(train),
        parent_manifest_SHA256=sha(parent),
        source_exposure_repetitions=repeats,
        dataset_change="No image/label/role changes versus06. TRAIN-only audit motivates finite source-frequency rebalance. Six HalmstadTRAIN clips remainTRAIN, diagnostic clips excluded.",
        exposures_by_category=dict(
            collections.Counter(
                {
                    cat: sum(
                        r["role"] == "TRAIN" and r["category"] == cat for r in manifest["records"]
                    )
                    * rep
                    for cat, rep in repeats.items()
                }
            )
        ),
    )
    manifest["new_real_repeats"] = 6
    write_new(dest / "DATA_MANIFEST.json", manifest)
    for name, rows in [("train", train), ("val", dev)]:
        (dest / (name + ".txt")).write_text(
            "\n".join(Path(p).as_posix() for p in rows) + "\n", encoding="utf-8"
        )
    (dest / "dataset.yaml").write_text(
        f"path: {dest.as_posix()}\ntrain: train.txt\nval: val.txt\nnames:\n  0: drone\n",
        encoding="utf-8",
    )
    assert_development_manifest(dest / "DATA_MANIFEST.json")
    p = copy.deepcopy(read(W / "REAL_SOURCE_PLAN.json"))
    current = now()
    p.update(
        at=stamp(),
        trial="standard_train_coverage_balance_10",
        data=str(dest / "dataset.yaml"),
        data_manifest_path=str(dest / "DATA_MANIFEST.json"),
        evaluation_manifest_path=str(dest / "DATA_MANIFEST.json"),
        data_manifest_SHA256=sha(dest / "DATA_MANIFEST.json"),
        dataset_yaml_SHA256=sha(dest / "dataset.yaml"),
        data_file_SHA256={str(dest / n): sha(dest / n) for n in ["train.txt", "val.txt"]},
        start_not_before=current.isoformat(),
        latest_new_run=(current + datetime.timedelta(minutes=12)).isoformat(),
        handoff_deadline=(current + datetime.timedelta(minutes=75)).isoformat(),
        max_wall_seconds=2400,
        train_unique=6892,
        train_exposures=11680,
        dev_unique=1629,
        hypothesis="TRAIN-only audit shows04 learns Halmstad128/128GT but only92/131DUT and57/128DetFly; ICG not previously seen by04. Rebalance legal positive-source exposures instead of continuing failed11s lineage or suppression/threshold sweeps. Preserve full-detector11n and same06 training parameters.",
        data_roles="Identical6892TRAIN and1629exposedDEV as06; unchanged full image/label SHA and roles. Source repetitions Halmstad10to4,DUT1to2,DetFly1to4,ICG6 unchanged,replay/LI1. No new source or negative labels.",
        comparison_limit="Compared with06 same04initializer,data,standard parameters; source-frequency change also changes optimizer update budget. TRAIN audit is biased diagnostic, not generalization or independent test.",
        selection="Best by unchanged1629DEV mAP50-95 only. Once final best, fixed1163subset,1209Halmstad,395USC exposed diagnostics. No threshold/checkpoint sweep or final holdout access.",
        next_stage="If no useful same-set consolidation keep04 and seek new real source coverage/label QA; do not repeat this same sampler indefinitely.",
        evidence_SHA256={
            str(f): sha(f)
            for f in [
                audit / "PLAN.json",
                audit / "RESULT.json",
                audit / "visual_review/PLAN.json",
                W / "TRAIN_COVERAGE_AUDIT_RESULT.md",
                W / "FP_GEOMETRY_PROFILE_04_08_09.json",
            ]
        },
    )
    assert p["params"]["epochs"] == 8 and p["params"]["lr0"] == 0.0002
    p["code_SHA256"] = {
        str(W / n): sha(W / n) for n in ["train.py", "launch.py", "common.py", "data_role_guard.py"]
    }
    p["evaluation_code_SHA256"] = {
        str(W / n): sha(W / n)
        for n in [
            "continue_after_training.py",
            "evaluate_dev.py",
            "evaluate_exposed.py",
            "evaluate_usc_exposed_v2.py",
            "common.py",
            "data_role_guard.py",
        ]
    }
    scoring = R / "pc_runs/model_capability_freeze_20261021_v1/scoring.py"
    p["evaluation_code_SHA256"][str(scoring)] = sha(scoring)
    p["data_role_registry_SHA256"] = sha(W / "DATA_ROLE_REGISTRY.json")
    write_new(W / "TRAIN_COVERAGE_BALANCE_PLAN.json", p)
    print("TRAIN_COVERAGE_FROZEN", 6892, 11680, 1629, "8epochs", "04initializer")


if __name__ == "__main__":
    main()
