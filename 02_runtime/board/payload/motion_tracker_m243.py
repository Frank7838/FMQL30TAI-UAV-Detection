"""Associate fresh observations independently of the accepted display retention slots.

All box geometry, confidence, ordering and missed ages come from the original
tracker. Velocity changes identity association only, never produces a detection.
An old retained slot may need a new display identity when its previous identity
is observed in another slot. Those stale identities are not model observations.
"""
from collections import defaultdict
from board_video import Tracker
from motion_tracker_m63 import MotionTracker as Association


def key(row):
    return (row['class_id'],row['confidence'],*row['xyxy'])


class MotionTracker:
    def __init__(self):
        self.retention=Tracker()
        self.association=Association()
        self.identity={}
        self.slot_identity={}
        self.next_id=1

    def allocate(self):
        identity=self.next_id
        self.next_id+=1
        return identity

    def update(self,detections):
        slots=self.retention.update(detections)
        associations=self.association.update(detections)
        fresh=defaultdict(list)
        for row in associations:
            if row['missed']==0:
                if row['track_id'] not in self.identity:
                    self.identity[row['track_id']]=self.allocate()
                fresh[key(row)].append(self.identity[row['track_id']])
        used=set()
        for slot in slots:
            if slot['missed']==0:
                identity=fresh[key(slot)].pop(0)
                self.slot_identity[slot['track_id']]=identity
                used.add(identity)
        for slot in slots:
            slot_id=slot['track_id']
            identity=self.slot_identity[slot_id]
            if slot['missed'] and identity in used:
                identity=self.allocate()
                self.slot_identity[slot_id]=identity
            used.add(identity)
            slot['track_id']=identity
        live={row['track_id'] for row in associations}
        self.identity={k:v for k,v in self.identity.items() if k in live}
        live_slots={row['track_id'] for row in self.retention.tracks}
        self.slot_identity={k:v for k,v in self.slot_identity.items() if k in live_slots}
        assert len({row['track_id'] for row in slots})==len(slots)
        return slots
