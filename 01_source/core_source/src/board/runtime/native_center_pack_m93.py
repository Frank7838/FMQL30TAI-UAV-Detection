"""调用C库，对1920×1080图像进行3倍中心取样。"""
import ctypes
from pathlib import Path
import numpy as np

_library = None


# 首次使用时加载纯CPU采样库并固定ABI类型；不在import阶段打开设备。
def library():
    global _library
    if _library is None:
        loaded = ctypes.CDLL(str(Path(__file__).with_name("libcenter_pack_m93.so")))
        loaded.fpga_center_pack_m93.argtypes = [
            np.ctypeslib.ndpointer(
                dtype=np.uint8, ndim=3, shape=(1080, 1920, 3), flags=("C_CONTIGUOUS", "ALIGNED")
            ),
            ctypes.c_size_t,
            np.ctypeslib.ndpointer(
                dtype=np.uint8,
                ndim=3,
                shape=(360, 640, 3),
                flags=("C_CONTIGUOUS", "ALIGNED", "WRITEABLE"),
            ),
            ctypes.c_size_t,
        ]
        loaded.fpga_center_pack_m93.restype = ctypes.c_int
        _library = loaded
    return _library


# 仅接受1920×1080连续BGR，返回独立640×360缓冲。
# 它只用于紧凑传输分支，当前640×480相机输入不会调用。
def pack(bgr):
    if (
        not isinstance(bgr, np.ndarray)
        or bgr.dtype != np.uint8
        or bgr.shape != (1080, 1920, 3)
        or not bgr.flags.c_contiguous
    ):
        raise ValueError("Native sampler requires contiguous1920x1080 BGR UINT8")
    centers = np.empty((360, 640, 3), dtype=np.uint8)
    code = library().fpga_center_pack_m93(bgr, bgr.nbytes, centers, centers.nbytes)
    if code != 0:
        raise RuntimeError("Native center sampler rejected ndarray buffers: " + str(code))
    return centers
