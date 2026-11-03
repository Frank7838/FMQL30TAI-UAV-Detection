// 用C实现同一告警规则，比较纯计算、进程往返和FPGA提交开销。
// 输入为Q16检测框和帧号，ROI、连续性判定及3/8规则与告警RTL对应。
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <dlfcn.h>

typedef struct {
    uint32_t x0, y0, x1, y1;
} Box;
typedef struct {
    uint32_t op, fid, fresh, count;
    Box boxes[16];
} Request;
typedef struct {
    uint32_t status, last, accepted, rejected, events, protocol, sequence, reserved;
    uint64_t compute_ns, now_ns, last_accept_ns, fault_ns, alarm_ns, clear_ns;
} Reply;
typedef struct {
    uint32_t alarm, fault, have, p, n, last, accepted, rejected, events, hit;
    uint64_t last_accept_ns, fault_ns, alarm_ns, clear_ns;
} State;

// 返回单调时间，单位纳秒。
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
// 一秒没有有效新提交时置fault、清连续计数，原alarm保持。
static void tick(State *s, uint64_t now) {
    if (!s->fault && now - s->last_accept_ns >= 1000000000ULL) {
        s->fault = 1;
        s->p = s->n = 0;
        s->events |= 4;
        s->fault_ns = now;
    }
}
// 接受新帧，判断框中心是否命中ROI，再更新3/8计数和事件。
// noinline保留函数调用，避免计时循环被折叠成常量。
__attribute__((noinline)) static void step(State *s, const Request *r, uint64_t now) {
    tick(s, now);
    uint32_t d = r->fid - s->last;
    if (!r->fresh || (s->have && (!d || (d & 0x80000000U)))) {
        ++s->rejected;
        return;
    }
    uint32_t hit = 0;
    for (uint32_t i = 0; i < r->count; ++i) {
        const Box *b = &r->boxes[i];
        uint32_t x = b->x0 + b->x1, y = b->y0 + b->y1;
        if (b->x0 < b->x1 && b->y0 < b->y1 && x >= 16384 && x < 114688 && y >= 8192 && y < 114688) {
            hit = 1;
            break;
        }
    }
    uint32_t consecutive = s->have && d == 1;
    s->events = s->fault ? 8 : 0;
    s->fault = 0;
    s->hit = hit;
    ++s->accepted;
    s->last = r->fid;
    s->have = 1;
    s->last_accept_ns = now;
    if (hit) {
        s->n = 0;
        s->p = consecutive ? (s->p < 3 ? s->p + 1 : 3) : 1;
        if (consecutive && s->p >= 3 && !s->alarm) {
            s->alarm = 1;
            s->events |= 1;
            s->alarm_ns = now;
        }
    } else {
        s->p = 0;
        s->n = consecutive ? (s->n < 8 ? s->n + 1 : 8) : 1;
        if (consecutive && s->n >= 8 && s->alarm) {
            s->alarm = 0;
            s->events |= 2;
            s->clear_ns = now;
        }
    }
}
// 按告警寄存器的位布局打包状态。
static uint32_t status(const State *s) {
    return s->alarm | (s->fault << 1) | (s->have << 2) | (s->p << 4) | (s->n << 8) | (s->hit << 12);
}
// 完整读写一次请求或回复，处理中断和管道的部分传输。
static int full_io(int fd, void *buf, size_t n, int write_it) {
    char *p = buf;
    while (n) {
        ssize_t k = write_it ? write(fd, p, n) : read(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}
// 独立进程接收请求；空闲时每毫秒检查一次断流。
static int daemon_loop(void) {
    State s = {0};
    s.last_accept_ns = now_ns();
    uint32_t sequence = 0;
    for (;;) {
        tick(&s, now_ns());
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        int ready = poll(&p, 1, 1); /* independent alarm process, 1ms watchdog */
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return 3;
        if (!ready)
            continue;
        Request r;
        if (full_io(STDIN_FILENO, &r, sizeof(r), 0))
            return 0;
        if (r.op == 9)
            return 0;
        if (r.count > 16 && r.op == 1)
            return 4;
        uint64_t begin = now_ns();
        tick(&s, begin);
        if (r.op == 1)
            step(&s, &r, begin);
        else if (r.op == 3) { /* software-only synchronisation to observed PL state */
            memset(&s, 0, sizeof(s));
            s.alarm = r.fresh & 1;
            s.fault = (r.fresh >> 1) & 1;
            s.have = (r.fresh >> 2) & 1;
            s.p = (r.fresh >> 4) & 15;
            s.n = (r.fresh >> 8) & 15;
            s.hit = (r.fresh >> 12) & 1;
            s.last = r.fid;
            s.accepted = r.count;
            s.rejected = r.boxes[0].x0;
            s.last_accept_ns = begin;
        }
        uint64_t end = now_ns();
        Reply a = {status(&s), s.last,    s.accepted,  s.rejected, s.events,         0,
                   ++sequence, 0,         end - begin, end,        s.last_accept_ns, s.fault_ns,
                   s.alarm_ns, s.clear_ns};
        if (full_io(STDOUT_FILENO, &a, sizeof(a), 1))
            return 5;
    }
}

static int (*drv_open)(int);
static int (*drv_close)(void);
static int (*drv_snapshot)(uint32_t *);
static int (*drv_write)(uint32_t, uint32_t);
static void *driver;
// 加载现有MMIO库，并传入调用方持有的设备锁句柄。
int paired_open(const char *path, int fd) {
    driver = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!driver)
        return -101;
    drv_open = dlsym(driver, "switch_open");
    drv_close = dlsym(driver, "switch_close");
    drv_snapshot = dlsym(driver, "switch_snapshot");
    drv_write = dlsym(driver, "switch_write");
    if (!drv_open || !drv_close || !drv_snapshot || !drv_write)
        return -102;
    return drv_open(fd);
}
int paired_snapshot(uint32_t *out) { return drv_snapshot(out); }
int paired_close(void) {
    int r = drv_close();
    dlclose(driver);
    return r;
}
// 计时覆盖事件确认、帧事务写入、驱动检查及状态读回。
int paired_submit(uint32_t fid, const Box *b, uint32_t n, uint32_t *out, uint64_t *elapsed) {
    if (n > 16)
        return -103;
    uint64_t begin = now_ns();
    uint32_t before[7];
    int rc = drv_snapshot(before);
    if (rc)
        return rc;
    if (before[6] && (rc = drv_write(0x38, before[6])))
        return rc;
    if ((rc = drv_write(4, fid)))
        return rc;
    for (uint32_t i = 0; i < n; ++i) {
        if ((rc = drv_write(8, b[i].x0 | (b[i].y0 << 16))))
            return rc;
        if ((rc = drv_write(12, b[i].x1 | (b[i].y1 << 16))))
            return rc;
    }
    if ((rc = drv_write(16, (n << 8) | 1)))
        return rc;
    if ((rc = drv_snapshot(out)))
        return rc;
    *elapsed = now_ns() - begin;
    if (out[2] != fid || out[3] != before[3] + 1 || out[4] != before[4] || out[5] != before[5])
        return -104;
    return 0;
}

// 批量重复规则计算，统计不含进程通信的开销。
static int micro(void) {
    /* Batch timing excludes process IPC; valid last-box hit is the worst scan. */
    static const unsigned counts[] = {0, 1, 4, 16};
    volatile uint64_t checksum = 0;
    puts("count,batch,operations,elapsed_ns,checksum");
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned k = 0; k < 100; ++k) {
            State s = {0};
            Request r = {0};
            r.fresh = 1;
            r.count = counts[c];
            for (unsigned i = 0; i < r.count; ++i)
                r.boxes[i] = (Box){100, 100, 500, 500};
            if (r.count)
                r.boxes[r.count - 1] = (Box){25000, 25000, 35000, 35000};
            uint64_t begin = now_ns();
            for (unsigned i = 0; i < 1000; ++i) {
                r.fid = i + 1;
                step(&s, &r, begin + i);
            }
            uint64_t elapsed = now_ns() - begin;
            checksum += status(&s) + s.accepted + s.last;
            printf("%u,%u,1000,%llu,%llu\n", counts[c], k, (unsigned long long)elapsed,
                   (unsigned long long)checksum);
        }
    return 0;
}
int main(int argc, char **argv) {
    if (sizeof(Request) != 272 || sizeof(Reply) != 80)
        return 6;
    return argc > 1 && !strcmp(argv[1], "--micro") ? micro() : daemon_loop();
}
