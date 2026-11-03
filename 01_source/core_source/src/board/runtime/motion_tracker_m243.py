"""结合运动预测关联目标并保留短时轨迹。"""
from collections import defaultdict
from board_video import Tracker
from motion_tracker_m63 import MotionTracker as Association


# 以类别、置信度和原框几何定位同一原始观测，不改变框或重算模型分数。
def key(row):
    return (row["class_id"], row["confidence"], *row["xyxy"])


# 把短时显示保留与速度关联分开，合并输出时保持所有显示ID唯一。
class MotionTracker:
    # 分别建立显示槽、关联器及二者的ID映射。
    def __init__(self):
        self.retention = Tracker()
        self.association = Association()
        self.identity = {}
        self.slot_identity = {}
        self.next_id = 1

    # 只分配显示身份号；身份号不作为检测正确性的证据。
    def allocate(self):
        identity = self.next_id
        self.next_id += 1
        return identity

    # 新观测继承关联ID；旧保留槽若与新ID冲突，另配显示ID并保留missed状态。
    # 清除不再存活的映射，避免长期运行无限积累身份字典。
    def update(self, detections):
        slots = self.retention.update(detections)
        associations = self.association.update(detections)
        fresh = defaultdict(list)
        for row in associations:
            if row["missed"] == 0:
                if row["track_id"] not in self.identity:
                    self.identity[row["track_id"]] = self.allocate()
                fresh[key(row)].append(self.identity[row["track_id"]])
        used = set()
        for slot in slots:
            if slot["missed"] == 0:
                identity = fresh[key(slot)].pop(0)
                self.slot_identity[slot["track_id"]] = identity
                used.add(identity)
        for slot in slots:
            slot_id = slot["track_id"]
            identity = self.slot_identity[slot_id]
            if slot["missed"] and identity in used:
                identity = self.allocate()
                self.slot_identity[slot_id] = identity
            used.add(identity)
            slot["track_id"] = identity
        live = {row["track_id"] for row in associations}
        self.identity = {k: v for k, v in self.identity.items() if k in live}
        live_slots = {row["track_id"] for row in self.retention.tracks}
        self.slot_identity = {k: v for k, v in self.slot_identity.items() if k in live_slots}
        assert len({row["track_id"] for row in slots}) == len(slots)
        return slots
