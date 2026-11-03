"""通过IoU关联相邻帧的检测框。"""
from board_video import box_iou


# 用上一观测的速度帮助IoU关联；预测框只参与关联评分，不变成模型新检测。
class MotionTracker:
    # 为当前会话初始化轨迹ID、速度和丢失计数；不跨boot复用轨迹。
    def __init__(self):
        self.tracks = []
        self.next_id = 1
        self.velocity = {}

    # 同类别候选按原框/速度预测框的最大IoU匹配，一次检测只分配给一个轨迹。
    # 连续缺失超过8次才删轨迹；返回missed字段供界面识别保留框。
    def update(self, detections):
        used = set()
        for track in self.tracks:
            box = track["xyxy"]
            gap = track["missed"] + 1
            vx, vy = self.velocity[track["track_id"]]
            predicted = [box[0] + gap * vx, box[1] + gap * vy, box[2] + gap * vx, box[3] + gap * vy]
            candidates = [
                (max(box_iou(box, d["xyxy"]), box_iou(predicted, d["xyxy"])), index)
                for index, d in enumerate(detections)
                if index not in used and d["class_id"] == track["class_id"]
            ]
            score, index = max(candidates, default=(0, -1))
            if score >= 0.3:
                new = detections[index]["xyxy"]
                self.velocity[track["track_id"]] = (
                    (new[0] + new[2] - box[0] - box[2]) / (2 * gap),
                    (new[1] + new[3] - box[1] - box[3]) / (2 * gap),
                )
                track.update(detections[index])
                track["missed"] = 0
                used.add(index)
            else:
                track["missed"] += 1
        for index, detection in enumerate(detections):
            if index not in used:
                self.tracks.append(dict(detection, track_id=self.next_id, missed=0))
                self.velocity[self.next_id] = (0.0, 0.0)
                self.next_id += 1
        self.tracks = [track for track in self.tracks if track["missed"] <= 8]
        live = {track["track_id"] for track in self.tracks}
        self.velocity = {
            identity: value for identity, value in self.velocity.items() if identity in live
        }
        return [dict(track) for track in self.tracks]
