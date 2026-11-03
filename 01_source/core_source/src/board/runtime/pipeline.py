"""读取文件图像，并用有限队列安排画面编码。"""
import queue, threading
import cv2


# 文件解码生产者，有界2帧队列并传播异常。
# 保留的文件输入路径；两帧队列限制解码超前量，实际相机使用worker.Latest。
class FileFrames:
    # 后台打开文件并等待就绪，避免在主推理线程上连续读文件。
    def __init__(self, path):
        self.q = queue.Queue(2)
        self.stop = threading.Event()
        self.error = None
        self.opened = False
        self.ready = threading.Event()
        self.t = threading.Thread(target=self._read, args=(path,), daemon=True)
        self.t.start()
        if not self.ready.wait(10):
            raise RuntimeError("File open readiness timeout")

    # 短超时等待队列空间并检查停止标志，使退出不被满队列永久卡住。
    def _put(self, x):
        while not self.stop.is_set():
            try:
                self.q.put(x, timeout=0.1)
                return
            except queue.Full:
                pass

    # 生产完整解码帧，异常存入error，finally释放文件并发送结束哨兵。
    def _read(self, path):
        c = None
        try:
            c = cv2.VideoCapture(path)
            self.opened = c.isOpened()
            self.ready.set()
            if not self.opened:
                raise RuntimeError("File open failed")
            while not self.stop.is_set():
                ok, image = c.read()
                if not ok:
                    break
                self._put(image)
        except Exception as e:
            self.error = e
            self.ready.set()
        finally:
            if c:
                c.release()
            self._put(None)

    # 返回已完成的打开结果，不在这里重复打开同一个片源。
    def isOpened(self):
        return self.opened

    # 返回(ok,image)，结束哨兵对应ok=False；线程异常与正常片尾分开处理。
    def read(self):
        try:
            x = self.q.get(timeout=15)
        except queue.Empty:
            raise RuntimeError("File decoder timeout")
        if self.error:
            raise RuntimeError("File decoder failure: " + str(self.error))
        return x is not None, x

    # 请求解码线程退出并有限等待，未退出就明确失败。
    def release(self):
        self.stop.set()
        self.t.join(timeout=10)
        if self.t.is_alive():
            raise RuntimeError("File decoder failed to close")


# 单编码线程，有界2项队列，结束时发送哨兵并等待排空。
# 旧同步绘制入口的有界编码线程；当前产品优先使用media_fast.MediaSink。
class VideoSink:
    # 编码器由消费者线程创建并独占，构造时同步检查是否就绪。
    def __init__(self, factory):
        self.q = queue.Queue(2)
        self.error = None
        self.closed = False
        self.frames = 0
        self.ready = threading.Event()
        self.t = threading.Thread(target=self._write, args=(factory,), daemon=True)
        self.t.start()
        if not self.ready.wait(10):
            raise RuntimeError("Video encoder readiness timeout")
        self.check()

    # 传播编码线程错误，不把写入失败算成成功录像。
    def check(self):
        if self.error:
            raise RuntimeError("Video encoder failure: " + str(self.error))

    # 顺序消费直到哨兵，finally写完编码器尾部并释放。
    def _write(self, factory):
        writer = None
        try:
            writer = factory()
            self.ready.set()
            while True:
                image = self.q.get()
                if image is None:
                    break
                writer.write(image)
                self.frames += 1
        except Exception as e:
            self.error = e
            self.ready.set()
        finally:
            if writer:
                try:
                    writer.release()
                except Exception as e:
                    self.error = e

    # 入队前复制图像，避免采集缓冲被下一帧覆盖；队列等待有10秒上限。
    def write(self, image):
        if self.closed:
            raise RuntimeError("Video sink closed")
        self.check()
        try:
            self.q.put(image.copy(), timeout=10)
        except queue.Full:
            raise RuntimeError("Video encoder queue timeout")
        self.check()

    # 停止接收新任务、送哨兵并等待排空；重复调用安全返回。
    def release(self):
        if self.closed:
            return
        self.closed = True
        self.check()
        try:
            self.q.put(None, timeout=10)
        except queue.Full:
            raise RuntimeError("Video encoder drain timeout")
        self.t.join(timeout=15)
        if self.t.is_alive():
            raise RuntimeError("Video encoder failed to close")
        self.check()
