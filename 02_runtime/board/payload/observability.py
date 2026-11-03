"""通过当前设备会话读取四类硬件事件时间和帧号。"""
MAGIC = 0x4F425331
EVENT_NAMES = ('alarm_assert', 'alarm_clear', 'stream_fault', 'stream_recover')


# read32来自主循环已经持有的设备会话；此函数不另开设备，不写寄存器。
# 事件序号变化或64位时钟跨32位边界时，整组重读，最多尝试max_attempts次。
# supported=False表示旧硬件无此扩展；ready=False表示本次未取得一致快照。
def read_observability(read32, max_attempts=3):
    if read32(0x40) != MAGIC:
        return {'supported': False, 'reason': 'OBS1 extension absent'}
    hz = read32(0x44)
    if hz <= 0:
        return {'supported': True, 'ready': False, 'reason': 'Invalid hardware clock metadata'}
    for _ in range(max_attempts):
        seq0 = read32(0x50)
        hi0 = read32(0x4C)
        lo = read32(0x48)
        hi1 = read32(0x4C)
        valid = read32(0x54) & 15
        last_mask = read32(0x58) & 15
        records = {}
        for i, name in enumerate(EVENT_NAMES):
            if valid & (1 << i):
                low = read32(0x60 + i * 8)
                high = read32(0x64 + i * 8)
                frame = read32(0x80 + i * 4)
                ticks = (high << 32) | low
                records[name] = {'cycles': ticks, 'frame': frame}
        seq1 = read32(0x50)
        if hi0 == hi1 and seq0 == seq1:
            return {'supported': True, 'ready': True, 'clock_hz': hz,
                    'now_cycles': (hi1 << 32) | lo, 'event_seq': seq1,
                    'valid_mask': valid, 'last_mask': last_mask,
                    'events': records, 'epoch': 'hardware business_reset; not UTC',
                    'record_scope': 'latest occurrence per type; not a complete event log'}
    return {'supported': True, 'ready': False, 'reason': 'Hardware changed during bounded read'}
