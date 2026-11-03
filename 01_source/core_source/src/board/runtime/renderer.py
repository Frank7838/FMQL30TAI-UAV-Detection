"""绘制检测框、目标编号、ROI和告警状态。"""
from pathlib import Path
import os, cv2, numpy as np


# 画原始检测框，跟踪仅标ID；PL状态来自寄存器快照。
# 绘制全部原始框，再叠加跟踪ID及stale标记；绘图不会增删用于PL判定的原始检测。
# 画面缩放到640×480只用于显示，PL的Q16坐标来自原始输入几何。
def draw(job, mode):
    im = job["image"]
    detections = job["detections"]
    tracks = job["tracks"]
    st = job["status"]
    frames = job["frames"]
    phase = job["phase"]
    view = cv2.resize(im, (640, 480))
    sx, sy = 640 / im.shape[1], 480 / im.shape[0]
    for d in detections:
        x, y, u, v = d["xyxy"]
        cv2.rectangle(
            view, (round(x * sx), round(y * sy)), (round(u * sx), round(v * sy)), (0, 255, 0), 2
        )
    for tr in tracks:
        x, y, u, v = tr["xyxy"]
        cv2.putText(
            view,
            "ID%d%s" % (tr["track_id"], " stale" if tr["missed"] else ""),
            (max(0, round(x * sx)), max(60, round(y * sy))),
            0,
            0.4,
            (255, 220, 0),
            1,
        )
    cv2.rectangle(view, (80, 30), (560, 420), (255, 160, 0), 1)
    cv2.rectangle(view, (0, 0), (640, 48), (20, 20, 20), -1)
    cv2.putText(
        view,
        "MODEL11 / PL ALARM / " + mode.upper() + " / " + phase,
        (8, 18),
        0,
        0.43,
        (255, 255, 255),
        1,
    )
    cv2.putText(
        view,
        "frame %d  ALARM %d  FAULT %d" % (frames, st["32"] & 1, bool(st["32"] & 2)),
        (8, 38),
        0,
        0.45,
        (0, 120, 255) if st["32"] & 1 else (100, 255, 100),
        1,
    )
    return np.ascontiguousarray(view)


# 按record开关写入有限录像，预览JPEG经临时文件原子替换。
# 保留的旧展示编码入口；当前持续运行由MediaSink进行独立限速。
class RenderWriter:
    # 仅创建CPU视频编码器，不持相机、NPU或MMIO资源。
    def __init__(self, out, mode):
        from mpeg4_writer_native_m388 import MPEG4Writer

        self.out = Path(out)
        self.mode = mode
        self.writer = MPEG4Writer(str(self.out / "preview.mp4"), 10.0, (640, 480), threads=1)

    # record只决定是否录制本张图，不能停止检测；旧预览策略每5个结果更新一次。
    def write(self, job):
        view = draw(job, self.mode)
        if job["record"]:
            self.writer.write(view)
        if job["frames"] % 5 == 0 or job["frames"] == 1:
            ok, jpg = cv2.imencode(".jpg", view, [cv2.IMWRITE_JPEG_QUALITY, 75])
            if not ok:
                raise RuntimeError("Preview encoder failure")
            (self.out / "latest.jpg.tmp").write_bytes(jpg.tobytes())
            os.replace(str(self.out / "latest.jpg.tmp"), str(self.out / "latest.jpg"))

    # 提交视频尾部并释放编码器；主会话另行确认SDK关闭。
    def release(self):
        self.writer.release()
