"""通过V4L2采集YUYV彩色图像，输出最新相机帧。"""
import cv2, os, signal, struct, sys, time, json

run = True


# 相机进程只置退出标志，让read循环自然结束并执行release。
def stop(*_):
    global run
    run = False


signal.signal(signal.SIGTERM, stop)


# 循环写stdout直到整个二进制包发送完；调试文字只能写stderr，不能混入图像协议。
def send(b):
    p = memoryview(b).cast("B")
    while p:
        n = os.write(1, p)
        p = p[n:]


cv2.setNumThreads(1)
c = cv2.VideoCapture(sys.argv[1], cv2.CAP_V4L2)
try:
    if not c.isOpened():
        raise RuntimeError("D435i RGB open failed")
    c.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"YUYV"))
    c.set(3, 640)
    c.set(4, 480)
    c.set(5, int(sys.argv[2]) if len(sys.argv) > 2 else 15)
    c.set(cv2.CAP_PROP_BUFFERSIZE, 2)
    print(
        json.dumps(
            dict(
                type="capture_negotiated",
                requested_fps=int(sys.argv[2]) if len(sys.argv) > 2 else 15,
                reported_fps=c.get(5),
                reported_buffers=c.get(cv2.CAP_PROP_BUFFERSIZE),
                width=c.get(3),
                height=c.get(4),
            )
        ),
        file=sys.stderr,
        flush=True,
    )
    seq = 0
    while run:
        ok, im = c.read()
        ts = time.monotonic_ns()
        if not ok or im is None or im.shape != (480, 640, 3):
            raise RuntimeError("RGB capture missing/shape changed")
        raw = im.tobytes()
        send(struct.pack("<IQQIII", 0x43414D31, seq, ts, 640, 480, len(raw)))
        send(raw)
        seq += 1
finally:
    c.release()
