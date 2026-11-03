"""Decode a complete local video on the board and infer every frame on its NPU.

Python handles codecs, basic tracking, alert state and presentation. A persistent
C++ worker uses the already validated Icraft model and preprocessing. No camera,
GUI, PyTorch or additional packages are required on the board.
"""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import socket
import statistics
import struct
import subprocess
import time

import cv2
import numpy as np

REQUEST_MAGIC = 0x46504741
RESULT_MAGIC = 0x4E505552


def receive_exact(channel, size):
    result = bytearray()
    while len(result) < size:
        data = channel.recv(size - len(result))
        if not data:
            raise RuntimeError("NPU worker socket closed; inspect worker.log")
        result.extend(data)
    return result


def infer(channel, frame_id, bgr):
    height, width = bgr.shape[:2]
    channel.sendall(struct.pack("<4I", REQUEST_MAGIC, frame_id, width, height))
    channel.sendall(memoryview(np.ascontiguousarray(bgr)).cast("B"))
    magic, returned_id, count = struct.unpack("<3I", receive_exact(channel, 12))
    if magic != RESULT_MAGIC or returned_id != frame_id or count > 10000:
        raise RuntimeError("invalid NPU response header")
    times = struct.unpack("<3d", receive_exact(channel, 24))
    rows = [struct.unpack("<6f", receive_exact(channel, 24)) for _ in range(count)]
    detections = []
    for class_id, x, y, w, h, confidence in rows:
        if not all(math.isfinite(value) for value in (class_id, x, y, w, h, confidence)):
            raise RuntimeError("non-finite detection")
        if class_id != 0 or not 0 <= confidence <= 1 or w < 0 or h < 0:
            raise RuntimeError("invalid detection class, size or score")
        detections.append({"class_id": int(class_id), "confidence": confidence,
                           "xyxy": [x, y, x + w, y + h]})
    return detections, times


def box_iou(a, b):
    iw = max(0, min(a[2], b[2]) - max(a[0], b[0]))
    ih = max(0, min(a[3], b[3]) - max(a[1], b[1]))
    intersection = iw * ih
    union = ((a[2] - a[0]) * (a[3] - a[1]) +
             (b[2] - b[0]) * (b[3] - b[1]) - intersection)
    return intersection / union if union > 0 else 0


class Tracker:
    """Greedy IoU baseline. Retained boxes are explicitly marked as stale."""
    def __init__(self):
        self.tracks = []
        self.next_id = 1

    def update(self, detections):
        used = set()
        for track in self.tracks:
            candidates = [(box_iou(track["xyxy"], d["xyxy"]), index)
                          for index, d in enumerate(detections)
                          if index not in used and d["class_id"] == track["class_id"]]
            score, index = max(candidates, default=(0, -1))
            if score >= 0.3:
                track.update(detections[index])
                track["missed"] = 0
                used.add(index)
            else:
                track["missed"] += 1
        for index, detection in enumerate(detections):
            if index not in used:
                self.tracks.append(dict(detection, track_id=self.next_id, missed=0))
                self.next_id += 1
        self.tracks = [track for track in self.tracks if track["missed"] <= 8]
        return [dict(track) for track in self.tracks]


def rss_kib(pid):
    try:
        for line in Path("/proc/{}/status".format(pid)).read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except OSError:
        pass
    return None


def distribution(values):
    return {"median_ms": statistics.median(values),
            "p95_ms": float(np.percentile(values, 95)), "mean_ms": statistics.mean(values)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-frames", type=int, required=True)
    parser.add_argument("--limit", type=int, default=0, help="0 processes the entire video")
    args = parser.parse_args()
    if args.expected_frames <= 0 or args.limit < 0:
        parser.error("invalid frame count")
    args.output.mkdir(parents=True, exist_ok=False)
    cap = cv2.VideoCapture(str(args.source))
    if not cap.isOpened():
        raise RuntimeError("cannot open video: {}".format(args.source))
    source_fps = cap.get(cv2.CAP_PROP_FPS)
    width, height = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)), int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    if not math.isfinite(source_fps) or source_fps <= 0 or not width or not height:
        raise RuntimeError("invalid video metadata")
    preview_size = (960, max(2, int(height * 960 / width) // 2 * 2))
    writer = cv2.VideoWriter(str(args.output / "annotated.mp4"),
                            cv2.VideoWriter_fourcc(*"mp4v"), source_fps, preview_size)
    if not writer.isOpened():
        raise RuntimeError("cannot open MPEG4 result writer")
    parent, child = socket.socketpair()
    parent.settimeout(90)
    started = time.perf_counter()
    tracker = Tracker()
    frames = 0
    positive_streak = negative_streak = 0
    alert = False
    series = {key: [] for key in ("decode", "worker_pre", "worker_infer", "worker_post",
                                  "worker_roundtrip", "pipeline")}
    rss = []
    worker = None
    try:
        with (args.output / "worker.log").open("wb") as worker_log:
            worker = subprocess.Popen([str(args.binary.resolve()), str(args.config.resolve()),
                                       str(child.fileno())], pass_fds=(child.fileno(),),
                                      stdout=worker_log, stderr=subprocess.STDOUT)
            child.close()
            if bytes(receive_exact(parent, 4)) != b"RDY1":
                raise RuntimeError("NPU worker failed to initialize")
            init_seconds = time.perf_counter() - started
            loop_start = time.perf_counter()
            with (args.output / "frames.jsonl").open("w") as frame_file, \
                 (args.output / "tracks.csv").open("w", newline="") as track_file, \
                 (args.output / "events.jsonl").open("w") as event_file:
                tracks_csv = csv.writer(track_file)
                tracks_csv.writerow(["frame_id", "track_id", "class_id", "confidence",
                                     "x1", "y1", "x2", "y2", "missed"])
                while not args.limit or frames < args.limit:
                    frame_start = time.perf_counter()
                    ok, bgr = cap.read()
                    if not ok:
                        break
                    if bgr.shape[:2] != (height, width):
                        raise RuntimeError("video dimensions changed unexpectedly")
                    decode_end = time.perf_counter()
                    detections, worker_times = infer(parent, frames, bgr)
                    npu_end = time.perf_counter()
                    tracks = tracker.update(detections)
                    previous_alert = alert
                    if detections:
                        positive_streak += 1
                        negative_streak = 0
                        if positive_streak >= 3:
                            alert = True
                    else:
                        positive_streak = 0
                        negative_streak += 1
                        if negative_streak >= 8:
                            alert = False
                    if alert != previous_alert:
                        event_file.write(json.dumps({"frame_id": frames, "source_time_s": frames/source_fps,
                                                     "event": "ALERT_ON" if alert else "ALERT_OFF"}) + "\n")
                    preview = cv2.resize(bgr, preview_size)
                    sx, sy = preview_size[0]/width, preview_size[1]/height
                    for track in tracks:
                        box = track["xyxy"]
                        tracks_csv.writerow([frames, track["track_id"], track["class_id"],
                                             track["confidence"]] + box + [track["missed"]])
                        x1, y1, x2, y2 = [int(value * scale) for value, scale in
                                         zip(box, (sx, sy, sx, sy))]
                        color = (0, 255, 0) if not track["missed"] else (0, 180, 255)
                        cv2.rectangle(preview, (x1, y1), (x2, y2), color, 1)
                        cv2.putText(preview, "ID {} missed {}".format(track["track_id"], track["missed"]),
                                    (x1, max(34, y1-4)), cv2.FONT_HERSHEY_SIMPLEX, 0.4, color, 1)
                    cv2.putText(preview, "frame {} | NPU | {}".format(frames, "ALERT" if alert else "CLEAR"),
                                (12, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.6,
                                (0, 0, 255) if alert else (0, 255, 0), 2)
                    writer.write(preview)
                    elapsed = (time.perf_counter() - frame_start)*1000
                    record = {"frame_id": frames, "source_time_s": frames/source_fps,
                              "detections": detections, "tracks": tracks, "alert": alert,
                              "times_ms": {"decode": (decode_end-frame_start)*1000,
                                           "worker_pre": worker_times[0], "worker_infer": worker_times[1],
                                           "worker_post": worker_times[2],
                                           "worker_roundtrip": (npu_end-decode_end)*1000,
                                           "pipeline": elapsed}}
                    frame_file.write(json.dumps(record) + "\n")
                    for key in series:
                        series[key].append(record["times_ms"][key])
                    if frames % 100 == 0:
                        rss.append({"frame_id": frames, "python_kib": rss_kib(os.getpid()),
                                    "worker_kib": rss_kib(worker.pid)})
                        print("VIDEO_PROGRESS {} frames={}".format(args.source.name, frames+1), flush=True)
                    if frames in (0, 99, 299) or (detections and frames % 200 == 0):
                        cv2.imwrite(str(args.output / "preview_{:06d}.jpg".format(frames)), preview)
                    frames += 1
            loop_seconds = time.perf_counter() - loop_start
            parent.sendall(struct.pack("<4I", REQUEST_MAGIC, 0, 0, 0))
            parent.close()
            returncode = worker.wait(timeout=30)
            if returncode:
                raise RuntimeError("NPU worker exited {}".format(returncode))
        expected = min(args.expected_frames, args.limit) if args.limit else args.expected_frames
        if frames != expected:
            raise RuntimeError("frame count mismatch: {} != {}".format(frames, expected))
        summary = {"status": "passed", "source": str(args.source), "source_sha256":
                   hashlib.sha256(args.source.read_bytes()).hexdigest(), "source_fps": source_fps,
                   "source_width": width, "source_height": height, "expected_frames": expected,
                   "processed_frames": frames, "skipped_frames": 0, "preview_size": preview_size,
                   "scope": "offline file decode, real NPU, CPU tracking/alert, MPEG4 file output; no camera/HDMI/custom PL",
                   "worker_init_s": init_seconds, "loop_seconds": loop_seconds,
                   "wall_seconds": time.perf_counter()-started, "processing_fps": frames/loop_seconds,
                   "latency_scope": "pipeline includes decode, socket copy, preprocess, NPU, postprocess, tracking, draw and writer.write; JSON/CSV overhead included in loop FPS but not pipeline timing; excludes worker init and final writer release",
                   "times_ms": {key: distribution(values) for key, values in series.items()},
                   "memory_samples": rss, "track_ids_created": tracker.next_id-1,
                   "worker_exit_code": returncode, "opencv_version": cv2.__version__}
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2)+"\n")
        print("BOARD_VIDEO_PROCESS_PASS frames={} fps={:.3f}".format(frames, frames/loop_seconds), flush=True)
    finally:
        cap.release()
        writer.release()
        parent.close()
        child.close()
        if worker is not None and worker.poll() is None:
            worker.terminate()
            try:
                worker.wait(timeout=10)
            except subprocess.TimeoutExpired:
                worker.kill()
                worker.wait()


if __name__ == "__main__":
    main()
