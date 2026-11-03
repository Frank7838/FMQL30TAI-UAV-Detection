// 用内存寄存器检查等待状态采样逻辑。
/* PC memory-backed control of the actual fast-sample implementation.
 * No /dev/mem, lease, FPGA or AXI behavior is exercised or claimed. */
#include "cached_mmio.c"
static unsigned forbidden_calls, checks;
FILE *__wrap_fopen(const char *a, const char *b) {
    (void)a;
    (void)b;
    ++forbidden_calls;
    errno = EPERM;
    return NULL;
}
int __wrap_fstat(int a, struct stat *b) {
    (void)a;
    (void)b;
    ++forbidden_calls;
    errno = EPERM;
    return -1;
}
int __wrap_lstat(const char *a, struct stat *b) {
    (void)a;
    (void)b;
    ++forbidden_calls;
    errno = EPERM;
    return -1;
}
int __wrap_open(const char *a, int b, ...) {
    (void)a;
    (void)b;
    ++forbidden_calls;
    errno = EPERM;
    return -1;
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "CONTROL_ERROR line=%d\n", __LINE__);                                  \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(void) {
    volatile uint32_t regs[1024] = {0};
    regs[0] = PLC_MMIO_SIGNATURE;
    regs[1] = PLC_MMIO_VERSION;
    regs[0x2c / 4] = 9;
    regs[0x30 / 4] = 1;
    plc_mmio p = {0};
    p.owner_pid = getpid();
    p.memfd = 123;
    p.regs = regs;
    p.expected_epoch = 9;
    p.epoch_pinned = 1;
    p.wait_armed = 1;
    plc_mmio_snapshot out = {0};
    regs[3] = 0x11;
    CHECK(plc_mmio_get_wait_snapshot(&p, &out) == 0 && out.status == 0x11);
    regs[3] = 0x402;
    CHECK(plc_mmio_get_wait_snapshot(&p, &out) == 0 &&
          out.status == 0x402); /* native pending+done */
    for (unsigned bit = 4; bit <= 32; bit *= 2) {
        if (bit == 16)
            continue;
        p.wait_failed = 0;
        regs[3] = bit;
        CHECK(plc_mmio_get_wait_snapshot(&p, &out) == -EIO);
        CHECK(p.wait_failed == 1);
        regs[3] = 2;
        CHECK(plc_mmio_get_wait_snapshot(&p, &out) == -EPERM); /* no retry after unknown */
    }
    p.wait_failed = 0;
    regs[3] = 2;
    regs[0x2c / 4] = 10;
    CHECK(plc_mmio_get_wait_snapshot(&p, &out) == -ESTALE);
    p.wait_failed = 0;
    regs[0x2c / 4] = 9;
    regs[0] = 0;
    CHECK(plc_mmio_get_wait_snapshot(&p, &out) == -EPROTO);
    p.wait_failed = 0;
    regs[0] = PLC_MMIO_SIGNATURE;
    regs[1] = 0x30000;
    CHECK(plc_mmio_get_wait_snapshot(&p, &out) == -EPROTO);
    p.wait_failed = 0;
    regs[1] = PLC_MMIO_VERSION;
    regs[0x30 / 4] = 0;
    regs[3] = 0x42;
    CHECK(plc_mmio_get_wait_snapshot(&p, &out) == -EPROTO);
    p.wait_failed = 0;
    regs[0x30 / 4] = 1;
    regs[3] = 16;
    CHECK(plc_mmio_get_wait_snapshot(&p, &out) == -EPROTO); /* pending without busy */
    p.wait_failed = 0;
    regs[3] = 2;
    p.wait_armed = 0;
    CHECK(plc_mmio_get_wait_snapshot(&p, &out) == -EPERM);
    p.wait_armed = 1;
    p.epoch_pinned = 0;
    CHECK(plc_mmio_get_wait_snapshot(&p, &out) == -EPERM);
    p.epoch_pinned = 1;
    p.owner_pid = getpid() + 1;
    CHECK(plc_mmio_get_wait_snapshot(&p, &out) == -EPERM);
    p.owner_pid = getpid();
    p.regs = NULL;
    CHECK(plc_mmio_get_wait_snapshot(&p, &out) == -EPERM);
    CHECK(forbidden_calls == 0); /* Link-wrapped open/fstat/lstat/fopen cannot pass unnoticed. */
    printf("SESSION_HOTPATH_PC_CONTROLS_PASS checks=%u forbidden_file_calls=%u hardware_calls=0\n",
           checks, forbidden_calls);
    return 0;
}
