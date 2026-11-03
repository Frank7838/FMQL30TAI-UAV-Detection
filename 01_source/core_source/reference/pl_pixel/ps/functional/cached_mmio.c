// 访问FPGA预处理控制寄存器并提交图像描述符。
#define _GNU_SOURCE
#include "cached_mmio.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define REAL_LOCK "/root/fpga_gaming/ps_session_m53.lock"
#define ST_BUSY UINT32_C(1)
#define ST_ERROR UINT32_C(4)
#define ST_QUARANTINE UINT32_C(8)
#define ST_PENDING UINT32_C(16)
#define ST_REJECTED UINT32_C(32)
#define ST_INHIBITED UINT32_C(64)
#define ST_FENCE UINT32_C(128)
#define ST_READ UINT32_C(0x100)
#define ST_WRITE UINT32_C(0x200)
#define ST_KNOWN UINT32_C(0xfff)
struct plc_mmio {
    int lockfd, memfd;
    struct stat lock_stat, mem_stat;
    volatile uint32_t *regs;
    uint32_t expected_epoch;
    int epoch_pinned, start_ever_issued;
    char error[256];
};
static void barrier(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("dmb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}
static int fail(plc_mmio *p, int number, const char *format, ...) {
    if (p) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(p->error, sizeof(p->error), format, args);
        va_end(args);
    }
    return -(number > 0 ? number : EIO);
}
int plc_mmio_read_offset_allowed(uint32_t offset) {
    return (offset <= UINT32_C(0x3c) && !(offset & 3)) ? 0 : -EINVAL;
}
/* Opus246/247: enable remains0, no pending command, own busy/ownership/fault
 * are zero in two independent >=1ms samples at the pinned current epoch.
 * HP status is registered before two-stage CDC but is not an atomic vector.
 * Native video cannot touch the private lease. P2-own idle gates descriptor/CLEAR; native/full
 * fence remains diagnostic. */
int plc_mmio_decode(const plc_mmio_snapshot *s, plc_idle_evidence *out) {
    if (!s || !out)
        return -EINVAL;
    if (s->signature != PLC_MMIO_SIGNATURE || s->version != PLC_MMIO_VERSION ||
        (s->status & ~ST_KNOWN) || s->enable > 1)
        return -EPROTO;
    uint32_t inhibited = (s->status & ST_INHIBITED) != 0;
    if (inhibited != (s->enable == 0) || ((s->status & ST_PENDING) && !(s->status & ST_BUSY)))
        return -EPROTO;
    memset(out, 0, sizeof(*out));
    out->signature = s->signature;
    out->busy = (s->status & ST_BUSY) != 0;
    out->command_pending = (s->status & ST_PENDING) != 0;
    out->idle = !(out->busy || out->command_pending);
    out->starts_inhibited = inhibited;
    out->release_ack = s->enable == 0 && (s->status & UINT32_C(0x359)) == UINT32_C(0x40);
    out->quarantine = (s->status & ST_QUARANTINE) != 0;
    out->read_outstanding = (s->status & ST_READ) != 0;
    out->write_outstanding = (s->status & ST_WRITE) != 0;
    out->completion_epoch = s->epoch;
    return 0;
}
int plc_mmio_write_allowed(uint32_t offset, uint32_t value, const plc_mmio_snapshot *s) {
    plc_idle_evidence e;
    int rc = plc_mmio_decode(s, &e);
    if (rc)
        return rc;
    if (offset == 0x08 && value == 2)
        return 0;
    if (offset == 0x30 && value == 0)
        return 0;
    if ((offset == 0x08 && (value != 1 && value != 4)) || (offset == 0x30 && value != 1))
        return -EINVAL;
    if (offset == 0x3c && value > 2)
        return -EINVAL;
    if (offset != 0x3c && offset != 0x08 && offset != 0x30 && offset != 0x10 && offset != 0x14 &&
        offset != 0x18 && offset != 0x1c && offset != 0x20)
        return -EINVAL;
    if (e.busy || e.command_pending || e.quarantine || e.read_outstanding || e.write_outstanding)
        return -EBUSY;
    if (offset == 0x08 && value == 1) {
        if (!s->enable || e.starts_inhibited)
            return -EBUSY;
        return (s->status & (ST_ERROR | ST_REJECTED)) ? -EBUSY : 0;
    }
    if (!e.starts_inhibited || !e.release_ack || s->enable)
        return -EBUSY;
    if (offset == 0x30)
        return (s->status & (ST_ERROR | ST_REJECTED)) ? -EBUSY : 0;
    /* New P2 RTL config gate is private-owner idle; full native fence diagnostic only. */
    return 0;
}
int plc_mmio_validate_descriptor(const plc_mmio_descriptor *d) {
    return plc_mmio_validate_mode_descriptor(0, d);
}
int plc_mmio_validate_mode_descriptor(uint32_t mode, const plc_mmio_descriptor *d) {
    if (!d || mode > 2)
        return -EINVAL;
    uint64_t base = d->base, limit = d->limit, src = d->source, dst = d->destination,
             len = d->length;
    uint64_t slot = base + PLC_SLOT_BYTES, read_bytes = (len + 7) & ~UINT64_C(7);
    uint64_t output = mode ? UINT64_C(1228800) : len;
    if ((mode == 1 && len != UINT64_C(921600)) || (mode == 2 && len != UINT64_C(6220800)))
        return -ERANGE;
    if (base < UINT64_C(0x30000000) || base + PLC_LEASE_BYTES > UINT64_C(0x3e000000) ||
        limit != base + PLC_LEASE_BYTES || ((base | src | dst) & 63) || !len ||
        len > PLC_SLOT_BYTES - 4096 || src < base || src + read_bytes + 4096 > slot || dst < slot ||
        dst + output + 4096 > limit || !(src + read_bytes <= dst || dst + output <= src))
        return -ERANGE;
    return 0;
}
static int same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_rdev == b->st_rdev;
}
static int lock_check(plc_mmio *p) {
    struct stat fd, path;
    if (!p || p->lockfd < 0 || fstat(p->lockfd, &fd) || lstat(REAL_LOCK, &path))
        return fail(p, EBADF, "Real borrowed lock FD/path unavailable");
    if (!S_ISREG(fd.st_mode) || !S_ISREG(path.st_mode) || fd.st_ino != PLC_REAL_LOCK_INODE ||
        !same(&fd, &path) || !same(&fd, &p->lock_stat))
        return fail(p, ESTALE, "Real lock inode943514 identity changed");
    FILE *stream = fopen("/proc/locks", "re");
    if (!stream)
        return fail(p, errno, "Cannot inspect FLOCK owner");
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), stream)) {
        unsigned long long id, inode;
        long owner;
        unsigned maj, min;
        char first[32], last[32];
        if (sscanf(line, "%llu: FLOCK ADVISORY WRITE %ld %x:%x:%llu %31s %31s", &id, &owner, &maj,
                   &min, &inode, first, last) == 7 &&
            owner == (long)getpid() && inode == (uint64_t)fd.st_ino && maj == major(fd.st_dev) &&
            min == minor(fd.st_dev) && !strcmp(first, "0") && !strcmp(last, "EOF")) {
            found = 1;
            break;
        }
    }
    int error = ferror(stream);
    (void)fclose(stream);
    return found && !error
               ? 0
               : fail(
                     p, EPERM,
                     "Same process must own exclusive real FLOCK; parent inherited owner rejected");
}
static uint32_t raw_read(plc_mmio *p, uint32_t offset) {
    barrier();
    uint32_t value = p->regs[offset / 4];
    barrier();
    return value;
}
static int identity_check(plc_mmio *p) {
    if (!p || !p->regs || p->memfd < 0)
        return fail(p, EINVAL, "MMIO context unavailable");
    int rc = lock_check(p);
    if (rc)
        return rc;
    struct stat fd, path;
    if (fstat(p->memfd, &fd) || lstat("/dev/mem", &path) || !same(&fd, &path) ||
        !same(&fd, &p->mem_stat) || !S_ISCHR(fd.st_mode) || fd.st_uid != 0 ||
        major(fd.st_rdev) != 1 || minor(fd.st_rdev) != 1)
        return fail(p, ESTALE, "Owned /dev/mem identity changed");
    if (raw_read(p, 0) != PLC_MMIO_SIGNATURE || raw_read(p, 4) != PLC_MMIO_VERSION)
        return fail(p, EPROTO, "Exact P1 signature/version mismatch; no writes permitted");
    return 0;
}
int plc_mmio_open(plc_mmio **out, int lockfd, uint64_t expected_inode) {
    if (!out || *out || lockfd < 0 || expected_inode != PLC_REAL_LOCK_INODE)
        return -EINVAL;
    if (geteuid() != 0 || sysconf(_SC_PAGESIZE) != 4096)
        return -EPERM;
    plc_mmio *p = calloc(1, sizeof(*p));
    if (!p)
        return -ENOMEM;
    p->lockfd = lockfd;
    p->memfd = -1;
    int rc;
    if (fstat(lockfd, &p->lock_stat) || (rc = lock_check(p))) {
        free(p);
        return -EPERM;
    }
    p->memfd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC | O_NOFOLLOW);
    if (p->memfd < 0) {
        rc = -errno;
        free(p);
        return rc;
    }
    if (fstat(p->memfd, &p->mem_stat)) {
        rc = -errno;
        (void)close(p->memfd);
        free(p);
        return rc;
    }
    if (!S_ISCHR(p->mem_stat.st_mode) || p->mem_stat.st_uid != 0 ||
        major(p->mem_stat.st_rdev) != 1 || minor(p->mem_stat.st_rdev) != 1) {
        (void)close(p->memfd);
        free(p);
        return -EPERM;
    }
    void *map = mmap(NULL, PLC_MMIO_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, p->memfd,
                     (off_t)PLC_MMIO_PHYSICAL);
    if (map == MAP_FAILED) {
        rc = -errno;
        (void)close(p->memfd);
        free(p);
        return rc;
    }
    p->regs = map;
    rc = identity_check(p);
    if (rc) {
        (void)munmap(map, PLC_MMIO_BYTES);
        (void)close(p->memfd);
        free(p);
        return rc;
    }
    *out = p;
    return 0;
}
int plc_mmio_read32(plc_mmio *p, uint32_t offset, uint32_t *value) {
    if (!value || plc_mmio_read_offset_allowed(offset))
        return -EINVAL;
    int rc = identity_check(p);
    if (rc)
        return rc;
    uint32_t data = raw_read(p, offset);
    if (raw_read(p, 0) != PLC_MMIO_SIGNATURE || raw_read(p, 4) != PLC_MMIO_VERSION)
        return fail(p, EPROTO, "Signature/version changed during read");
    *value = data;
    return 0;
}
int plc_mmio_get_snapshot(plc_mmio *p, plc_mmio_snapshot *out) {
    if (!out)
        return -EINVAL;
    int rc = identity_check(p);
    if (rc)
        return rc;
    plc_mmio_snapshot s;
    plc_idle_evidence e;
    s.signature = raw_read(p, 0);
    s.version = raw_read(p, 4);
    s.epoch = raw_read(p, 0x2c);
    s.status = raw_read(p, 0x0c);
    s.enable = raw_read(p, 0x30);
    if (raw_read(p, 0x2c) != s.epoch)
        return fail(p, EAGAIN, "Epoch changed during MMIO snapshot; no retry");
    if (raw_read(p, 0) != s.signature || raw_read(p, 4) != s.version)
        return fail(p, EPROTO, "Signature/version changed during snapshot");
    rc = plc_mmio_decode(&s, &e);
    if (rc)
        return fail(p, -rc, "Inconsistent MMIO status/enable snapshot");
    if (p->epoch_pinned && s.epoch != p->expected_epoch)
        return fail(p, ESTALE, "Unexpected current epoch; no retry");
    barrier();
    *out = s;
    return 0;
}
static int issue_store(plc_mmio *p, uint32_t offset, uint32_t value, uint32_t epoch) {
    if (!p->epoch_pinned || epoch != p->expected_epoch || raw_read(p, 0x2c) != epoch)
        return fail(p, ESTALE, "Current epoch not pinned before store");
    int rc = identity_check(p);
    if (rc)
        return rc;
    barrier();
    p->regs[offset / 4] = value;
    barrier();
    uint32_t after = raw_read(p, 0x2c);
    if (raw_read(p, 0) != PLC_MMIO_SIGNATURE || raw_read(p, 4) != PLC_MMIO_VERSION ||
        after != epoch + UINT32_C(1))
        return fail(p, EPROTO, "Unknown post-store signature/epoch; never retry");
    p->expected_epoch = after;
    if (offset == 8 && value == 1)
        p->start_ever_issued = 1;
    return 0;
}
int plc_mmio_expect_epoch(plc_mmio *p, uint32_t expected) {
    plc_mmio_snapshot s;
    int rc = plc_mmio_get_snapshot(p, &s);
    if (rc)
        return rc;
    if (s.epoch != expected)
        return fail(p, ESTALE, "Expected operation epoch differs");
    p->expected_epoch = expected;
    p->epoch_pinned = 1;
    return 0;
}
int plc_mmio_write32(plc_mmio *p, uint32_t offset, uint32_t value) {
    plc_mmio_snapshot state;
    int rc = plc_mmio_get_snapshot(p, &state);
    if (rc)
        return rc;
    if (p->start_ever_issued &&
        ((offset >= 0x10 && offset <= 0x20) || offset == 0x3c || (offset == 8 && value == 4)))
        return fail(p, EPERM, "Descriptor/CLEAR forbidden after first START");
    rc = plc_mmio_write_allowed(offset, value, &state);
    if (rc)
        return fail(p, -rc, "Write offset/value/state denied");
    if (offset == 0x08 && value == 1) {
        plc_mmio_descriptor d;
        d.base = raw_read(p, 0x10);
        d.limit = raw_read(p, 0x14);
        d.source = raw_read(p, 0x18);
        d.destination = raw_read(p, 0x1c);
        d.length = raw_read(p, 0x20);
        if (plc_mmio_validate_mode_descriptor(raw_read(p, 0x3c), &d))
            return fail(p, ERANGE,
                        "Full readback descriptor fails private 16MiB/two-slot/range contract");
    }
    rc = issue_store(p, offset, value, state.epoch);
    if (rc)
        return rc;
    if (((offset >= 0x10 && offset <= 0x20) || offset == 0x3c) && raw_read(p, offset) != value)
        return fail(p, EIO, "Configuration readback differs after one write; never retry");
    if (offset == 0x30 && raw_read(p, 0x30) != value)
        return fail(p, EIO, "Enable readback differs after one write; never retry");
    return 0;
}
int plc_mmio_config_once(plc_mmio *p, uint32_t offset, uint32_t value, plc_config_attempt *out) {
    if (!p || !out)
        return -EINVAL;
    if (p->start_ever_issued)
        return fail(p, EPERM, "Configuration retry forbidden after any START");
    if (!((offset >= 0x10 && offset <= 0x20 && !(offset & 3)) || (offset == 0x3c && value <= 2) ||
          (offset == 8 && value == 4)))
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    plc_mmio_snapshot s;
    int rc = plc_mmio_get_snapshot(p, &s);
    if (rc)
        return rc;
    if (!p->epoch_pinned)
        return fail(p, ESTALE, "Configuration requires expected epoch");
    rc = plc_mmio_write_allowed(offset, value, &s);
    if (rc == -EBUSY)
        return 0;
    if (rc)
        return rc;
    out->epoch_before = s.epoch;
    out->accepted_before = raw_read(p, 0x34);
    out->issued = 1;
    rc = issue_store(p, offset, value, s.epoch);
    if (rc)
        return rc;
    plc_mmio_snapshot after;
    rc = plc_mmio_get_snapshot(p, &after);
    if (rc)
        return rc;
    out->epoch_after = after.epoch;
    out->accepted_after = raw_read(p, 0x34);
    out->rejected_after = (after.status & ST_REJECTED) != 0;
    out->readback = offset == 8 ? 0 : raw_read(p, offset);
    if (offset == 8) {
        if (out->accepted_after != out->accepted_before &&
            out->accepted_after != out->accepted_before + UINT32_C(1))
            return fail(p, EPROTO, "Unknown CLEAR accepted sequence");
        if (out->accepted_after == out->accepted_before && !out->rejected_after)
            return fail(p, EPROTO, "CLEAR neither accepted nor known rejected");
    } else if (out->accepted_after != out->accepted_before)
        return fail(p, EPROTO, "Configuration unexpectedly issued a command");
    return 0;
}
int plc_mmio_idle_callback(void *context, plc_idle_evidence *out) {
    plc_mmio_snapshot s;
    int rc = plc_mmio_get_snapshot((plc_mmio *)context, &s);
    if (rc)
        return rc;
    return plc_mmio_decode(&s, out);
}
int plc_mmio_make_idle_guard(plc_mmio *p, plc_idle_guard *out) {
    if (!p || !out)
        return -EINVAL;
    int rc = identity_check(p);
    if (rc)
        return rc;
    if (!p->epoch_pinned)
        return fail(p, ESTALE, "Idle guard requires pinned current epoch");
    out->expected_signature = PLC_MMIO_SIGNATURE;
    out->expected_epoch = p->expected_epoch;
    out->read_idle = plc_mmio_idle_callback;
    out->context = p;
    return 0;
}
const char *plc_mmio_error(const plc_mmio *p) { return p ? p->error : "No MMIO context"; }
int plc_mmio_close(plc_mmio **handle) {
    if (!handle || !*handle)
        return -EINVAL;
    plc_mmio *p = *handle;
    if (p->regs && munmap((void *)p->regs, PLC_MMIO_BYTES))
        return fail(p, errno, "Control-window munmap failed; context retained");
    p->regs = NULL;
    if (p->memfd >= 0) {
        int fd = p->memfd;
        p->memfd = -1;
        if (close(fd))
            return fail(p, errno, "Owned /dev/mem close failed; no FD retry");
    }
    memset(p, 0, sizeof(*p));
    free(p);
    *handle = NULL;
    return 0;
}
