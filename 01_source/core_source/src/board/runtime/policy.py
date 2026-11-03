"""计算Q16检测框、ROI命中和告警参考状态。"""
import math

ROI = (8192, 4096, 57344, 57344)


# 将原始像素框规范化为Q16；超16框、越界或无效数值显式报错，不截断隐藏。
# 输入原图像素xyxy和置信度，输出Q16整数四元组。
# 四舍五入后夹到0..65535；无效框、归一化塌缩或超过16框都报错，不能悄悄删框。
def boxes_q16(boxes, w, h):
    if not (w > 0 and h > 0):
        raise ValueError("Invalid frame geometry")
    if len(boxes) > 16:
        raise ValueError("More than16 raw boxes: explicit FAULT, no silent truncation")
    out = []
    for d in boxes:
        b = d["xyxy"]
        s = d["confidence"]
        if (
            len(b) != 4
            or not all(math.isfinite(x) for x in list(b) + [s])
            or not 0 <= s <= 1
            or d.get("class_id", 0) != 0
        ):
            raise ValueError("Malformed raw box")
        x, y, u, v = b
        # 框经float32的x、宽分别传回后再相加，边缘可能多出约0.00002像素。
        # 仅在Q16转换时收回这点舍入误差；原始检测框和分数仍保留。
        eps_x, eps_y = w * 2.0**-22, h * 2.0**-22
        if not (-eps_x <= x < u <= w + eps_x and -eps_y <= y < v <= h + eps_y):
            raise ValueError("Box outside native content")
        b = [max(0.0, x), max(0.0, y), min(float(w), u), min(float(h), v)]
        q = [
            max(0, min(65535, math.floor(t * 65536.0 / size + 0.5)))
            for t, size in zip(b, [w, h, w, h])
        ]
        if not (q[0] < q[2] and q[1] < q[3]):
            raise ValueError("Collapsed normalized box")
        out.append(q)
    return out


# 判断至少一个框中心位于半开ROI内，边界与RTL一致。
# 用两个端点之和比较两倍ROI边界，不先做整数除法，边界与RTL半开区间一致。
def hit(boxes):
    return any(
        2 * ROI[0] <= b[0] + b[2] < 2 * ROI[2] and 2 * ROI[1] <= b[1] + b[3] < 2 * ROI[3]
        for b in boxes
    )


# 计算下次成功提交后的参考状态；它用于校验，不直接驱动LED。
# before的键是寄存器字节偏移的十进制字符串。
# 帧号按32位模差判断新旧；跳号重建连续性，断流后的首帧同时记录恢复事件。
def expected(before, fid, boxes):
    status = before["32"]
    fault = bool(status & 2)
    last = before["36"]
    delta = (fid - last) & 0xFFFFFFFF
    if before["40"] and not (0 < delta < 0x80000000):
        raise ValueError("Nonfresh hardware frame id")
    consecutive = bool(status & 4) and delta == 1
    p = 0 if fault else (status >> 4) & 15
    n = 0 if fault else (status >> 8) & 15
    a = bool(status & 1)
    old = a
    positive = hit(boxes)
    if positive:
        n = 0
        p = min(3, p + 1) if consecutive else 1
        if consecutive and p >= 3:
            a = True
    else:
        p = 0
        n = min(8, n + 1) if consecutive else 1
        if consecutive and n >= 8:
            a = False
    return dict(
        status=int(a) | 4 | (p << 4) | (n << 8) | (int(positive) << 12),
        events=(8 if fault else 0) | ((1 if a else 2) if a != old else 0),
        alarm=a,
        positive=positive,
    )


# 继承已置位告警时，先建立本会话观测所有权，避免新会话直接清掉旧警。
# 防止新会话在尚无自己的有效正观测时，直接用空帧解除继承下来的告警。
class Ownership:
    # 从现有alarm位建立初始所有权；未继承告警时允许正常提交负观测。
    def __init__(self, initial):
        self.inherited_alarm = bool(initial["32"] & 1)
        self.own_positive = 0
        self.clear_allowed = not self.inherited_alarm

    # 命中可提交；继承告警时，先积累本会话三次正观测，之后才允许负观测清警。
    def allow(self, positive):
        if positive:
            self.own_positive += 1
            if self.own_positive >= 3:
                self.clear_allowed = True
            return True
        self.own_positive = 0
        return self.clear_allowed

    # 未知观测打断本会话的正观测积累；不把未知输入算成一次空帧。
    def unknown(self):
        self.own_positive = 0


# 相机序号必须递增，接收时间到当前的年龄须在0到500ms内。
# 相机序号严格递增，且接收到当前的帧龄在0..500ms内；时间戳均为单调时钟。
def fresh(sequence, last_sequence, received_ns, now_ns, max_age_ns=500000000):
    return sequence > last_sequence and 0 <= now_ns - received_ns <= max_age_ns
