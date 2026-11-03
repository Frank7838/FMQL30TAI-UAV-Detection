"""Strict native MPEG4 adapter; the synchronous call borrows real BGR pixels."""
import ctypes
from fractions import Fraction
from pathlib import Path
import math
import numpy as np
class MPEG4Writer:
    def __init__(self,path,fps,size,threads=2):
        self._ptr=None;self.frames=0
        if not math.isfinite(fps) or fps<=0:raise ValueError('Invalid FPS')
        self.width,self.height=size
        if type(self.width) is not int or type(self.height) is not int:raise ValueError('Integer geometry required')
        raw=str(path).encode('utf8')
        if b'\0' in raw:raise ValueError('Invalid output path')
        self._lib=ctypes.CDLL(str(Path(__file__).with_name('libmpeg4_writer_m388.so')))
        lib=self._lib;lib.m134_open.argtypes=[ctypes.c_char_p,*([ctypes.c_int]*5),ctypes.POINTER(ctypes.c_int)];lib.m134_open.restype=ctypes.c_void_p
        lib.m134_write.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int];lib.m134_write.restype=ctypes.c_int
        lib.m134_close.argtypes=[ctypes.c_void_p];lib.m134_close.restype=ctypes.c_int
        f=Fraction(fps).limit_denominator(10000);status=ctypes.c_int()
        self._ptr=lib.m134_open(raw,self.width,self.height,f.numerator,f.denominator,threads,ctypes.byref(status))
        if not self._ptr:raise RuntimeError('M134 encoder open failed {}'.format(status.value))
    def isOpened(self):return self._ptr is not None
    def write(self,image):
        if self._ptr is None:raise RuntimeError('Closed writer')
        if not isinstance(image,np.ndarray) or image.dtype!=np.uint8 or image.shape!=(self.height,self.width,3) or not image.flags.c_contiguous or image.strides!=(self.width*3,3,1):raise ValueError('Exact contiguous uint8 BGR storage required')
        # image stays alive through sws_scale + FFmpeg-owned AVFrame encoding/drain.
        status=self._lib.m134_write(self._ptr,image.ctypes.data,image.nbytes,image.strides[0])
        if status<0:raise RuntimeError('M134 encoder write failed {}'.format(status))
        self.frames+=1
    def release(self):
        ptr=self._ptr;self._ptr=None
        if ptr is not None:
            status=self._lib.m134_close(ptr)
            if status<0:raise RuntimeError('M134 encoder finalize failed {}'.format(status))
