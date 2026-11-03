"""Bounded CPU-only preview/record consumers; never own SDK or MMIO.

Inference is never discarded here. Preview keeps the latest completed result;
recording samples source time at10Hz and reports queue overload separately.
"""
from pathlib import Path
import json,os,queue,threading,time
import cv2
from renderer import draw
from runtime_log import RollingText


class MediaSink:
    def __init__(self,out,mode,record_limit=1200,record_hz=10,preview_hz=10,
                 writer_factory=None,draw_fn=None,clock=None):
        self.out=Path(out);self.mode=mode;self.record_limit=record_limit
        self.record_period_ns=int(1e9/record_hz);self.preview_period=1./preview_hz
        self.clock=clock or time.monotonic;self.draw=draw_fn or draw
        self.cv=threading.Condition();self.latest=None;self.q=queue.Queue(2)
        self.closed=False;self.errors=[];self.next_record_ns=None
        self.start=self.clock();self.record_ready=threading.Event()
        self.c=dict(submitted=0,preview_overwritten=0,preview_written=0,record_requested=0,
                    record_not_due=0,record_eligible=0,record_enqueued=0,record_queue_dropped=0,
                    record_limit_reached=0,record_written=0,record_encode_ns=0,preview_publish_ns=0)
        if writer_factory is None:
            def writer_factory():
                from mpeg4_writer_timed import TimestampedMPEG4Writer
                return TimestampedMPEG4Writer(str(self.out/'preview.mp4'),float(record_hz),(640,480),threads=1)
        self.writer_factory=writer_factory
        self.preview_thread=threading.Thread(target=self._preview,name='preview-latest',daemon=True)
        self.record_thread=threading.Thread(target=self._record,name='record-bounded',daemon=True)
        self.preview_thread.start();self.record_thread.start()
        if not self.record_ready.wait(10):raise RuntimeError('Record consumer readiness timeout')
        try:self.check()
        except Exception:
            self.release()
            raise

    def check(self):
        with self.cv:
            if self.errors:raise RuntimeError('Media consumer failure: '+self.errors[0])

    def _fail(self,exc):
        with self.cv:self.errors.append(str(exc));self.cv.notify_all()

    def write(self,job):
        self.check()
        # Source images and raw detections are immutable until consumed. Track
        # dictionaries are copied because the next tracker update may edit them.
        item=dict(job)
        item['tracks']=[dict(t,xyxy=list(t['xyxy'])) for t in job['tracks']]
        stamp=int(item.get('media_source_ns',item.get('received_monotonic_ns',time.monotonic_ns())))
        with self.cv:
            if self.closed:raise RuntimeError('Media sink already closed')
            self.c['submitted']+=1
            if self.latest is not None:self.c['preview_overwritten']+=1
            self.latest=item;self.cv.notify_all()
            if not item['record']:return
            self.c['record_requested']+=1
            if self.c['record_enqueued']>=self.record_limit:
                self.c['record_limit_reached']+=1;return
            if self.next_record_ns is not None and stamp<self.next_record_ns:
                self.c['record_not_due']+=1;return
            self.next_record_ns=stamp+self.record_period_ns
            self.c['record_eligible']+=1
            try:self.q.put_nowait(item);self.c['record_enqueued']+=1
            except queue.Full:self.c['record_queue_dropped']+=1

    def _preview(self):
        next_at=0.
        try:
            while True:
                with self.cv:
                    while self.latest is None and not self.closed:self.cv.wait()
                    if self.latest is None and self.closed:return
                    wait=next_at-self.clock()
                    if wait>0 and not self.closed:self.cv.wait(wait);continue
                    job=self.latest;self.latest=None
                begin=time.monotonic_ns();view=self.draw(job,self.mode)
                ok,jpg=cv2.imencode('.jpg',view,[cv2.IMWRITE_JPEG_QUALITY,75])
                if not ok:raise RuntimeError('Preview JPEG encoding failed')
                temp=self.out/'latest.jpg.tmp';temp.write_bytes(jpg.tobytes())
                os.replace(temp,self.out/'latest.jpg')
                published=time.monotonic_ns()
                # This is read-only evidence; no server/control route is added.
                meta=dict(result_frame=job['frames']-1,source_sequence=job.get('capture_sequence'),
                          received_monotonic_ns=job.get('received_monotonic_ns'),published_monotonic_ns=published)
                temp=self.out/'preview_meta.json.tmp';temp.write_text(json.dumps(meta));os.replace(temp,self.out/'preview_meta.json')
                with self.cv:self.c['preview_written']+=1;self.c['preview_publish_ns']+=published-begin
                next_at=self.clock()+self.preview_period
        except Exception as exc:self._fail(exc)

    def _record(self):
        writer=None;mapping=None
        try:
            if self.record_limit>0:
                writer=self.writer_factory()
                mapping=RollingText(self.out/'video_map.jsonl')
            self.record_ready.set()
            while True:
                job=self.q.get()
                if job is None:return
                begin=time.monotonic_ns();view=self.draw(job,self.mode)
                source_ns=int(job.get('media_source_ns',job['received_monotonic_ns']))
                pts_ms=writer.write_at(view,source_ns)
                with self.cv:
                    index=self.c['record_written'];self.c['record_written']+=1
                    self.c['record_encode_ns']+=time.monotonic_ns()-begin
                mapping.write(json.dumps(dict(encoded_frame=index,result_frame=job['frames']-1,
                    source_sequence=job.get('capture_sequence'),received_ns=job.get('received_monotonic_ns'),
                    media_source_ns=job.get('media_source_ns'),nominal_fps=10,time_base='1/1000',pts_ms=pts_ms,variable_frame_timestamps=True))+'\n');mapping.flush()
        except Exception as exc:self._fail(exc);self.record_ready.set()
        finally:
            if writer:
                try:writer.release()
                except Exception as exc:self._fail(exc)
            if mapping:mapping.close()

    def stats(self):
        with self.cv:
            elapsed=max(self.clock()-self.start,1e-9)
            return dict(self.c,elapsed_s=elapsed,preview_publish_fps=self.c['preview_written']/elapsed,
                        recorded_frames_per_wall_s=self.c['record_written']/elapsed,
                        preview_latest_capacity=1,record_queue_capacity=2,
                        record_target_hz=1e9/self.record_period_ns,nominal_fps=10,variable_frame_timestamps=True,
                        record_policy='source-time sampling; intentional not_due separate from overload drop',
                        video_time='VFR source timestamps; final sample duration100ms; video_map records actual encoded PTS')

    def release(self):
        with self.cv:
            if self.closed:return
            self.closed=True;self.cv.notify_all()
        if self.record_thread.is_alive():
            try:self.q.put(None,timeout=5)
            except queue.Full:raise RuntimeError('Record queue failed to drain')
        self.preview_thread.join(timeout=10);self.record_thread.join(timeout=15)
        if self.preview_thread.is_alive() or self.record_thread.is_alive():
            raise RuntimeError('Media consumer failed to close')
        self.check()
