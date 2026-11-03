"""Prioritize recent observations for identity, with original display retention.

Past velocity, .3 IoU association and eight-miss expiry stay unchanged. Sorting
applies only to the identity association; it never removes/adds model detections
or changes retained box pixels, confidence, ordering or missed ages.
"""
from motion_tracker_m63 import MotionTracker as OriginalAssociation
from motion_tracker_m243 import MotionTracker as OriginalRetention


class RecentAssociation(OriginalAssociation):
    def update(self, detections):
        self.tracks.sort(key=lambda row: (row['missed'], row['track_id']))
        return super().update(detections)


class MotionTracker(OriginalRetention):
    def __init__(self):
        super().__init__()
        self.association = RecentAssociation()
