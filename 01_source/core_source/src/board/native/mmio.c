// CPU到PL业务寄存器的C ABI，映射0x400C0000。只接受继承的真实独占锁。
// switch_snapshot保留7次有序读取及每次屏障，合并调用不能保证硬件原子快照。

#include <stdint.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
static int memfd = -1, held = -1;
static volatile uint32_t *regs;
// ARM外设访问使用dmb osh，编译器memory约束也保留；volatile本身不保证完整访问顺序。
static void barrier(void) { __asm__ __volatile__("dmb osh" ::: "memory"); }
// 只解除映射并关闭本库打开的/dev/mem；继承的设备锁fd仍由Python会话持有和关闭。
int switch_close(void) {
    if (regs) {
        munmap((void *)regs, 4096);
        regs = 0;
    }
    if (memfd >= 0) {
        close(memfd);
        memfd = -1;
    }
    held = -1;
    return 0;
}
// 验证继承fd仍指向同一个锁文件；不能通过删除重建锁绕过设备所有者。
static int valid(void) {
    struct stat a, b;
    return held >= 0 && fstat(held, &a) == 0 &&
           stat("/root/fpga_gaming/ps_session_m53.lock", &b) == 0 && a.st_ino == 943514 &&
           a.st_ino == b.st_ino && a.st_dev == b.st_dev && flock(held, LOCK_EX | LOCK_NB) == 0;
}
// 映射业务窗口并读签名0xA5050001；不打开NPU SDK或启动DMA。
int switch_open(int inherited) {
    if (regs)
        return -EBUSY;
    held = inherited;
    if (!valid())
        return -EBUSY;
    memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0)
        return -errno;
    void *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0x400C0000ULL);
    if (p == MAP_FAILED) {
        switch_close();
        return -ENOMEM;
    }
    regs = p;
    barrier();
    if (regs[0xC0 / 4] != 0xA5050001) {
        switch_close();
        return -EPROTO;
    }
    return 0;
}
// offset是字节偏移，必须4字节对齐且在4KiB内；负返回值表示失败，不能当正常寄存器值。
int switch_read(uint32_t offset, uint32_t *out) {
    if (!regs || !out || offset >= 4096 || (offset & 3) || !valid())
        return -EINVAL;
    barrier();
    *out = regs[offset / 4];
    barrier();
    return 0;
}
// 写白名单仅覆盖业务寄存器；Python产品层进一步禁止业务复位0x00。
// C接口包含业务复位地址；日常Python接口仅写帧事务和事件清除。
int switch_write(uint32_t offset, uint32_t value) {
    if (!regs || !(offset <= 0x10 || offset == 0x38) || (offset & 3) || !valid())
        return -EINVAL;
    barrier();
    if (regs[0xC0 / 4] != 0xA5050001)
        return -EPROTO;
    regs[offset / 4] = value;
    barrier();
    return 0;
}

// Ordered read group, not an atomic hardware snapshot. No business writes.
// The same inherited exclusive owner is checked at both boundaries. Keep
// every per-register MMIO barrier and publish nothing if ownership fails.
// 七项顺序为签名、状态、末帧、accept、reject、protocol_error、event。
// 逐项有屏障且末尾复查所有权；这只是有序读取组，不保证同一硬件周期的原子性。
int switch_snapshot(uint32_t *out) {
    static const uint32_t offsets[7] = {0xC0, 0x20, 0x24, 0x28, 0x2C, 0x30, 0x34};
    uint32_t values[7];
    if (!regs || !out || !valid())
        return -EINVAL;
    for (unsigned i = 0; i < 7; i++) {
        barrier();
        values[i] = regs[offsets[i] / 4];
        barrier();
    }
    if (!valid())
        return -EBUSY;
    for (unsigned i = 0; i < 7; i++)
        out[i] = values[i];
    return 0;
}
