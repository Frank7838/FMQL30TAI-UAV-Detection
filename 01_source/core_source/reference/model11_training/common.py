"""提供实验目录、JSON读写、摘要计算和时间函数。"""

from pathlib import Path
import datetime, hashlib, json, os

W = Path(__file__).resolve().parent
R = W.parents[1]
TZ = datetime.timezone(datetime.timedelta(hours=8))


# 返回北京时间。
def now():
    return datetime.datetime.now(TZ)


# 将当前时间写成ISO格式。
def stamp():
    return now().isoformat()


# 读取JSON文件。
def read(p):
    return json.loads(Path(p).read_text(encoding="utf-8-sig"))


# 分块计算文件摘要。
def sha(p):
    h = hashlib.sha256()
    with Path(p).open("rb") as f:
        for b in iter(lambda: f.read(4 * 1024 * 1024), b""):
            h.update(b)
    return h.hexdigest()


# 创建新的JSON记录。
def write_new(p, x):
    with Path(p).open("x", encoding="utf-8") as f:
        json.dump(x, f, ensure_ascii=False, indent=2, allow_nan=False)


# 写入临时文件后替换现有JSON。
def update(p, x):
    p = Path(p)
    tmp = p.with_suffix(p.suffix + ".tmp")
    tmp.write_text(json.dumps(x, ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
    os.replace(tmp, p)
