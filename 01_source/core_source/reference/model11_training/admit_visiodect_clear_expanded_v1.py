"""接纳补充样本并生成模型11训练输入。"""

from common import *
import cv2

src = W / "visiodect_train_expanded_v1"
qa = read(src / "QA_RESULT.json")
# 使用全图与标签检查时确定的样本编号。
keep = {16, 17, 18, 19, 20, 22, 23, 24, 25, 30, 31, 32, 34, 46, 49, 55, 58, 59}
review = []
for i, r in enumerate(qa["records"]):
    reason = (
        "CLEAR_VISIBLE_FULL_TARGET"
        if i in keep
        else (
            "LABEL_FORMAT_DISAGREEMENT"
            if r["flags"]
            else ("APPARENT_DEGENERATE_BOX" if i == 21 else "SMALL_UNCLEAR_OR_EDGE_TARGET_DEFERRED")
        )
    )
    if i in keep:
        assert (
            not r["flags"] and min(r["short_sides_640"]) >= 16 and not r["exact_overlap_existing"]
        )
    review.append(dict(index=i, member=r["canonical_member"], decision=reason, selected=i in keep))
write_new(
    src / "VISUAL_REVIEW_COMPLETE.json",
    dict(
        at=stamp(),
        actual_full_scenes_viewed=60,
        pages=[dict(path=p, SHA256=sha(p)) for p in qa["full_scene_pages"]],
        decisions=review,
        model_forwards=0,
        qualification="Full-frame640 review; selection by visibility/annotation before model outputs. No claim of independent flights. Excluded tiny frames remain preserved, never negatives.",
    ),
)
out = W / "visiodect_clear_train_expanded_v1"
out.mkdir(exist_ok=False)
(out / "images").mkdir()
(out / "labels").mkdir()
records = []
for i, r in enumerate(qa["records"]):
    if i not in keep:
        continue
    p = Path(r["local"])
    assert sha(p) == r["SHA256"]
    im = cv2.imread(str(p))
    h, w = im.shape[:2]
    scale = 640 / max(h, w)
    sw, sh = round(w * scale), round(h * scale)
    prepared = cv2.copyMakeBorder(
        cv2.resize(im, (sw, sh)),
        0,
        640 - sh,
        0,
        640 - sw,
        cv2.BORDER_CONSTANT,
        value=(114, 114, 114),
    )
    assert hashlib.sha256(prepared.tobytes()).hexdigest() == r["prepared_BGR_SHA256"]
    image = out / "images" / (p.stem + ".png")
    label = out / "labels" / (p.stem + ".txt")
    assert cv2.imwrite(str(image), prepared)
    lines = []
    for b in r["txt_boxes"]:
        x1, y1, x2, y2 = b
        x1 *= sw / w
        x2 *= sw / w
        y1 *= sh / h
        y2 *= sh / h
        lines.append(
            "0 "
            + " ".join(
                f"{v:.10f}"
                for v in [(x1 + x2) / 1280, (y1 + y2) / 1280, (x2 - x1) / 640, (y2 - y1) / 640]
            )
        )
    label.write_text("\n".join(lines) + "\n", encoding="utf-8")
    records.append(
        dict(
            role="TRAIN",
            image=str(image),
            label=str(label),
            image_SHA256=sha(image),
            label_SHA256=sha(label),
            decoded_pixels_SHA256=r["prepared_BGR_SHA256"],
            GT_boxes=len(lines),
            known_truth=True,
            source_id="VisioDECT_author_8335400",
            source_version=1,
            canonical_member=r["canonical_member"],
            original_frame_SHA256=r["SHA256"],
            original_label_SHA256=sha(p.with_suffix(".txt")),
            source_image=str(p),
            source_label=str(p.with_suffix(".txt")),
            capture_group="VisioDECT_author_related_TRAIN",
            capture_proxy=r["group"],
            group="VisioDECT_author_related_TRAIN",
            sequence=r["group"],
            category="VisioDECT_clear_ground_TRAIN",
        )
    )
admission = out / "SOURCE_ADMISSION.json"
fields = [
    "image_SHA256",
    "label_SHA256",
    "canonical_member",
    "original_frame_SHA256",
    "original_label_SHA256",
    "source_version",
]
write_new(
    admission,
    dict(
        at=stamp(),
        source_id="VisioDECT_author_8335400",
        approved_for_development=True,
        allowed_role="TRAIN",
        reservation_path=str(W / "visiodect_index_v1/SOURCE_ROLE_RESERVATION.json"),
        reservation_SHA256=sha(W / "visiodect_index_v1/SOURCE_ROLE_RESERVATION.json"),
        review_path=str(src / "VISUAL_REVIEW_COMPLETE.json"),
        review_SHA256=sha(src / "VISUAL_REVIEW_COMPLETE.json"),
        approved_records=[{k: r[k] for k in fields} for r in records],
        admitted_images=len(records),
        input_contract="Complete RGB resized with aspect ratio and bottom/right114padding to640, all source drone classes mapped0. Original TXT geometry retained. No GT crop.",
        limitations="18 correlated selected TRAIN positives only. No guarantee of capture independence or generic pretraining non-overlap; 0 exact pixel overlap with8521 prepared images. No automatic justification for training. No negative sample claims.",
        license="Official pinned Kaggle version1 CC BY-NC-SA4.0, local research, no raw redistribution",
        model_forwards=0,
    ),
)
for r in records:
    r.update(provenance_document=str(admission), provenance_SHA256=sha(admission))
write_new(
    out / "DATA_MANIFEST.json",
    dict(
        at=stamp(),
        records=records,
        TRAIN_images=len(records),
        TRAIN_GT=sum(r["GT_boxes"] for r in records),
        DEV_images=0,
    ),
)
print("ADMITTED", len(records), "TRAIN positives; model calls0")
