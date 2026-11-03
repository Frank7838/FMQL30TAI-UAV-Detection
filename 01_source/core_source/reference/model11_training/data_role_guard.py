"""核对样本角色、来源分组和训练清单。"""

from common import *


# 核对来源归档和已接纳的训练样本。
def assert_fred_source_boundary(row, admission):
    """Canonical source identifiers survive local renaming; TRAIN-only admission."""
    hold_path = W / "FRED_SOURCE_TRAINING_HOLD.json"
    if hold_path.exists():
        assert not read(hold_path).get(
            "blocked", True
        ), "FRED source use held pending annotation/visible-size contract review"
    assert (
        row["role"] == "TRAIN" and row.get("source_split") == "train"
    ), "FRED admission is TRAIN only"
    reservation_path = Path(admission["source_reservation_path"])
    assert (
        sha(reservation_path) == admission["source_reservation_SHA256"]
    ), "FRED reservation changed"
    reservation = read(reservation_path)
    assert (
        reservation["source_id"] == row["source_id"]
        and reservation["official_TEST_role"] == "RESERVED_NO_MEDIA_NO_PROBE"
    )
    train_path = Path(admission["official_TRAIN_tree_path"])
    assert (
        sha(train_path) == admission["official_TRAIN_tree_SHA256"]
    ), "FRED officialTRAIN roster changed"
    official_train = {r["path"] for r in read(train_path)}
    official_reserved = {r["path"] for r in reservation["official_TEST_files"]}
    archive = row.get("canonical_archive")
    assert (
        archive in official_train
        and archive not in official_reserved
        and archive in admission["allowed_archives"]
    ), "FRED non-TRAIN/archive alias forbidden"
    assert (
        row.get("source_revision") == admission["source_revision"] == reservation["dataset_commit"]
    ), "FRED source revision mismatch"
    fields = [
        "image_SHA256",
        "label_SHA256",
        "canonical_archive",
        "original_frame_member",
        "original_frame_SHA256",
        "original_label_SHA256",
    ]
    key = tuple(row.get(k) for k in fields)
    assert key in {
        tuple(r[k] for k in fields) for r in admission["approved_records"]
    }, "FRED frame/label not in frozen admission"


# 核对样本角色、来源组与计划中的清单。
def assert_development_manifest(manifest_path, plan=None):
    registry_path = W / "DATA_ROLE_REGISTRY.json"
    registry = read(registry_path)
    assert (
        sha(registry["reservation"]) == registry["reservation_SHA256"]
    ), "Holdout reservation changed"
    additional = {}
    for reservation_entry in registry.get("additional_reservations", []):
        assert (
            sha(reservation_entry["path"]) == reservation_entry["SHA256"]
        ), "Additional source reservation changed"
        source_reservation = read(reservation_entry["path"])
        additional[source_reservation["source_id"]] = source_reservation
    if plan is not None:
        assert plan.get("data_role_registry_SHA256") == sha(
            registry_path
        ), "New plan must freeze data-role registry SHA"
        assert plan["code_SHA256"].get(str(W / "data_role_guard.py")) == sha(
            __file__
        ), "Freeze role-guard code in new plan"
    approved = {
        (r["image_SHA256"], r["label_SHA256"], r["role"])
        for r in registry["approved_existing_records"]
    }
    old_images = {r["image_SHA256"] for r in registry["approved_existing_records"]}
    records = read(manifest_path)["records"]
    for row in records:
        assert row["role"] in ("TRAIN", "DEV"), "Final/unknown data forbidden in development"
        serialized = json.dumps(row, ensure_ascii=False).lower().replace("\\", "/")
        assert not any(
            token in serialized for token in registry["protected_tokens"]
        ), "Reserved final source forbidden in development"
        if row.get("source_id") in additional:
            source_reservation = additional[row["source_id"]]
            assert (
                source_reservation.get("role") != "FINAL_RESERVED_NO_MEDIA"
            ), "Entire source reserved for final validation"
            if row["source_id"] == "VisioDECT_author_8335400":
                member = row.get("canonical_member", "").replace("\\", "/")
                parts = member.split("/")
                assert (
                    len(parts) >= 4 and parts[0] == "VisioDECT Dataset Upload"
                ), "Missing canonical VisioDECT identity"
                assert (
                    source_reservation["group_roles"].get(parts[1]) == "TRAIN_CANDIDATE"
                    and row["role"] == "TRAIN"
                ), "VisioDECT reserved group forbidden"
                assert (
                    row.get("source_version") == source_reservation["version"]
                ), "VisioDECT version mismatch"
        key = (row.get("image_SHA256"), row.get("label_SHA256"), row["role"])
        if key not in approved:
            assert (
                row.get("image_SHA256") not in old_images
            ), "Existing role or label change requires a new audited admission, not silent reuse"
            assert all(
                row.get(field) for field in ("source_id", "capture_group", "provenance_document")
            ), "New data needs explicit source/group/provenance"
            provenance = Path(row["provenance_document"])
            assert provenance.is_file() and row.get("provenance_SHA256") == sha(
                provenance
            ), "Freeze source admission document"
            admission = read(provenance)
            assert (
                admission.get("approved_for_development") is True
                and admission.get("source_id") == row["source_id"]
            ), "Missing source admission"
            assert not any(
                t in json.dumps(admission, ensure_ascii=False).lower()
                for t in registry["protected_tokens"]
            ), "Reserved family in provenance"
            if row["source_id"] == "Florence_FRED_author_2025":
                assert_fred_source_boundary(row, admission)
            if row["source_id"] == "VisioDECT_author_8335400":
                assert (
                    row["role"] == admission["allowed_role"] == "TRAIN"
                ), "VisioDECT admission TRAIN only"
                assert (
                    sha(admission["reservation_path"]) == admission["reservation_SHA256"]
                ), "VisioDECT reservation changed"
                fields = [
                    "image_SHA256",
                    "label_SHA256",
                    "canonical_member",
                    "original_frame_SHA256",
                    "original_label_SHA256",
                    "source_version",
                ]
                assert tuple(row.get(k) for k in fields) in {
                    tuple(r[k] for k in fields) for r in admission["approved_records"]
                }, "VisioDECT member/label not in frozen admission"
    return dict(
        records=len(records), registry_SHA256=sha(registry_path), reserved_final_source_used=False
    )
