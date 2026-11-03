"""轮转文本日志并读取子进程的输出。"""
from pathlib import Path
import os, threading


# 保持当前文件与有限轮转副本；每个实例由单个写入线程持有。
# 按UTF-8字节数轮转，默认当前4MiB加两份备份；此类本身不加线程锁，由写入者独占。
class RollingText:
    # 以追加方式打开现存日志；CSV表头仅在新空文件或轮转后的文件写入。
    def __init__(self, path, max_bytes=4 * 1024 * 1024, backups=2, header=""):
        self.path = Path(path)
        self.limit = max_bytes
        self.backups = backups
        self.header = header
        self.f = self.path.open("a", encoding="utf8", newline="")
        self.size = self.path.stat().st_size
        if not self.size and header:
            self.write(header)

    # 超限前先把备份从大号向小号移动，再新建当前文件；单条写入不会被切成两个文件。
    def write(self, text):
        size = len(text.encode("utf8"))
        if self.size + size > self.limit:
            self.f.close()
            for i in range(self.backups, 0, -1):
                source = self.path if i == 1 else Path(str(self.path) + "." + str(i - 1))
                if source.exists():
                    os.replace(source, Path(str(self.path) + "." + str(i)))
            self.f = self.path.open("w", encoding="utf8", newline="")
            self.size = 0
            if self.header:
                self.f.write(self.header)
                self.size = len(self.header.encode("utf8"))
        self.f.write(text)
        self.size += size

    # 刷新Python缓冲，便于查看服务读到近期数据；不等同于每帧fsync落盘。
    def flush(self):
        self.f.flush()

    # 关闭当前文本句柄；上下文退出同样调用此方法。
    def close(self):
        self.f.close()

    # with语句返回同一个写入实例，不另开文件。
    def __enter__(self):
        return self

    # 正常或异常离开with都关闭当前文件。
    def __exit__(self, *_):
        self.close()


# 后台排空子进程stdout/stderr到滚动日志，防止管道塞满阻塞推理。
# 每条子进程管道由一个线程持续排空，防止stdout/stderr满后反向阻塞推理。
class PipeLog:
    # 每份管道日志默认1MiB加两份备份，保存线程异常供关闭时检查。
    def __init__(self, pipe, path):
        self.pipe = pipe
        self.path = path
        self.error = None
        self.t = threading.Thread(target=self.copy, daemon=True)
        self.t.start()

    # 分块读取字节并写滚动文本；无法解码的诊断字节替换显示，不改二进制图像通道。
    def copy(self):
        try:
            with RollingText(self.path, max_bytes=1024 * 1024) as out:
                while True:
                    chunk = self.pipe.read(8192)
                    if not chunk:
                        break
                    out.write(chunk.decode("utf8", errors="replace"))
                    out.flush()
        except Exception as e:
            self.error = e
        finally:
            self.pipe.close()

    # 等待管道EOF；超时或读取异常都上报，不直接终止持有设备的子进程。
    def close(self):
        self.t.join(timeout=5)
        if self.t.is_alive():
            raise RuntimeError("Subprocess log did not close")
        if self.error:
            raise RuntimeError("Subprocess log failed: " + str(self.error))
