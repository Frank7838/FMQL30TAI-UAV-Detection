"""组织当前检测框与近期轨迹的匹配。"""
from motion_tracker_m63 import MotionTracker as OriginalAssociation
from motion_tracker_m243 import MotionTracker as OriginalRetention


class RecentAssociation(OriginalAssociation):
    # 优先让近期轨迹竞争本帧观测，排序只影响身份关联，不删原始检测。
    def update(self, detections):
        self.tracks.sort(key=lambda row: (row["missed"], row["track_id"]))
        return super().update(detections)


class MotionTracker(OriginalRetention):
    # 复用既有显示保留规则，只替换为近期观测优先的关联器。
    def __init__(self):
        super().__init__()
        self.association = RecentAssociation()
