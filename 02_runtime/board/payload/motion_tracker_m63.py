"""Past-observation velocity for association only; reported stale boxes stay stale."""
from board_video import box_iou


class MotionTracker:
    def __init__(self):
        self.tracks=[]
        self.next_id=1
        self.velocity={}

    def update(self,detections):
        used=set()
        for track in self.tracks:
            box=track['xyxy'];gap=track['missed']+1
            vx,vy=self.velocity[track['track_id']]
            predicted=[box[0]+gap*vx,box[1]+gap*vy,box[2]+gap*vx,box[3]+gap*vy]
            candidates=[(max(box_iou(box,d['xyxy']),box_iou(predicted,d['xyxy'])),index)
                for index,d in enumerate(detections) if index not in used and d['class_id']==track['class_id']]
            score,index=max(candidates,default=(0,-1))
            if score>=.3:
                new=detections[index]['xyxy']
                self.velocity[track['track_id']]=((new[0]+new[2]-box[0]-box[2])/(2*gap),
                    (new[1]+new[3]-box[1]-box[3])/(2*gap))
                track.update(detections[index]);track['missed']=0;used.add(index)
            else:track['missed']+=1
        for index,detection in enumerate(detections):
            if index not in used:
                self.tracks.append(dict(detection,track_id=self.next_id,missed=0))
                self.velocity[self.next_id]=(0.,0.);self.next_id+=1
        self.tracks=[track for track in self.tracks if track['missed']<=8]
        live={track['track_id'] for track in self.tracks}
        self.velocity={identity:value for identity,value in self.velocity.items() if identity in live}
        return [dict(track) for track in self.tracks]
