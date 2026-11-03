"""向编码库传入图像及实际采集时间。"""
import ctypes
from fractions import Fraction
from pathlib import Path
import math
import numpy as np


class TimestampedMPEG4Writer:
    # 绑定带毫秒PTS的编码ABI，首张实际编码帧建立时间零点。
    def __init__(self, path, fps, size, threads=2):
        self._ptr = None
        self.frames = 0
        self.origin_ns = None
        self.last_pts_ms = None
        if not math.isfinite(fps) or fps <= 0:
            raise ValueError("Invalid FPS")
        self.width, self.height = size
        if type(self.width) is not int or type(self.height) is not int:
            raise ValueError("Integer geometry required")
        raw = str(path).encode("utf8")
        if b"\0" in raw:
            raise ValueError("Invalid output path")
        self._lib = ctypes.CDLL(str(Path(__file__).with_name("libmpeg4_writer_timed.so")))
        lib = self._lib
        lib.m134_open.argtypes = [
            ctypes.c_char_p,
            *([ctypes.c_int] * 5),
            ctypes.POINTER(ctypes.c_int),
        ]
        lib.m134_open.restype = ctypes.c_void_p
        lib.m26_write_at_ms.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_int64,
        ]
        lib.m26_write_at_ms.restype = ctypes.c_int
        lib.m134_close.argtypes = [ctypes.c_void_p]
        lib.m134_close.restype = ctypes.c_int
        f = Fraction(fps).limit_denominator(10000)
        status = ctypes.c_int()
        self._ptr = lib.m134_open(
            raw, self.width, self.height, f.numerator, f.denominator, threads, ctypes.byref(status)
        )
        if not self._ptr:
            raise RuntimeError("M134 encoder open failed {}".format(status.value))

    # 只检查本地编码器是否持有可用句柄。
    def isOpened(self):
        return self._ptr is not None

    # source_ns使用单调源时间，四舍五入成毫秒PTS并严格递增。
    # 真实帧间隔写入MP4，不能靠固定封装FPS把丢采样区间压缩成连续检测。
    def write_at(self, image, source_ns):
        if self._ptr is None:
            raise RuntimeError("Closed writer")
        if (
            not isinstance(image, np.ndarray)
            or image.dtype != np.uint8
            or image.shape != (self.height, self.width, 3)
            or not image.flags.c_contiguous
            or image.strides != (self.width * 3, 3, 1)
        ):
            raise ValueError("Exact contiguous uint8 BGR storage required")
        # image stays alive through sws_scale + FFmpeg-owned AVFrame encoding/drain.
        if type(source_ns) is not int or source_ns < 0:
            raise ValueError("Integer nonnegative source timestamp required")
        origin = source_ns if self.origin_ns is None else self.origin_ns
        pts_ms = (source_ns - origin + 500000) // 1000000
        if pts_ms < 0 or (self.last_pts_ms is not None and pts_ms <= self.last_pts_ms):
            raise ValueError("Non-increasing video timestamp")
        status = self._lib.m26_write_at_ms(
            self._ptr, image.ctypes.data, image.nbytes, image.strides[0], pts_ms
        )
        if status < 0:
            raise RuntimeError("M134 encoder write failed {}".format(status))
        self.origin_ns = origin
        self.last_pts_ms = pts_ms
        self.frames += 1
        return pts_ms

    # 正常写出编码尾部后释放，出错抛给媒体线程和主会话。
    def release(self):
        ptr = self._ptr
        self._ptr = None
        if ptr is not None:
            status = self._lib.m134_close(ptr)
            if status < 0:
                raise RuntimeError("M134 encoder finalize failed {}".format(status))
