"""按IoU匹配检测框与标注，统计精确率、召回率和F1。"""

import math


# 计算两个xyxy框的交并比。
def iou(a, b):
    intersection = max(0.0, min(a[2], b[2]) - max(a[0], b[0])) * max(
        0.0, min(a[3], b[3]) - max(a[1], b[1])
    )
    area_a = max(0.0, a[2] - a[0]) * max(0.0, a[3] - a[1])
    area_b = max(0.0, b[2] - b[0]) * max(0.0, b[3] - b[1])
    union = area_a + area_b - intersection
    return intersection / union if union else 0.0


# 由TP、FP、FN计算精确率、召回率和F1。
def rates(tp, fp, fn):
    return dict(
        TP=tp,
        FP=fp,
        FN=fn,
        P=tp / (tp + fp) if tp + fp else None,
        R=tp / (tp + fn) if tp + fn else None,
        F1=2 * tp / (2 * tp + fp + fn) if 2 * tp + fp + fn else None,
    )


# 按固定IoU阈值匹配检测与标注并汇总计数。
def score(rows):
    """Rows contain already thresholded/NMS outputs; do not refilter cached data."""
    tp = fp = fn = known = unknown = negative = false_frames = 0
    presence_tp = presence_fp = presence_fn = 0
    for row in rows:
        if not row["valid_truth"]:
            unknown += 1
            continue
        known += 1
        gt, ds = row["truth"], row["detections"]
        for box in gt + ds:
            if len(box["xyxy"]) != 4 or not all(math.isfinite(x) for x in box["xyxy"]):
                raise ValueError("Non-finite or malformed box")
        remaining = list(range(len(ds)))
        hits = 0
        for g in gt:
            choices = [j for j in remaining if ds[j]["class"] == g["class"]]
            if choices:
                j = max(choices, key=lambda j: iou(g["xyxy"], ds[j]["xyxy"]))
                if iou(g["xyxy"], ds[j]["xyxy"]) >= 0.5:
                    remaining.remove(j)
                    hits += 1
        tp += hits
        fp += len(ds) - hits
        fn += len(gt) - hits
        negative += not gt
        false_frames += not gt and bool(ds)
        presence_tp += bool(gt) and bool(ds)
        presence_fp += not gt and bool(ds)
        presence_fn += bool(gt) and not ds
    return dict(
        **rates(tp, fp, fn),
        frames=len(rows),
        known_frames=known,
        unknown_frames=unknown,
        GT=tp + fn,
        negative_frames=negative,
        negative_false_alarm_frames=false_frames,
        negative_false_alarm_rate=false_frames / negative if negative else None,
        presence=rates(presence_tp, presence_fp, presence_fn)
    )
