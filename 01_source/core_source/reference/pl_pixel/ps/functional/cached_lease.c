// 分配和映射连续图像缓冲区，完成缓存交接与回收。
#define _GNU_SOURCE
#include "cached_lease.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define LOCK_PATH "/root/fpga_gaming/ps_session_m53.lock"
#define MANAGER "/dev/u-dma-buf-mgr"
#define CLASS "/sys/class/u-dma-buf/"
#define GUARD_BYTE 0xA7
struct plc_lease {
    int lockfd, managerfd, devicefd, created, identity, cpu_owned, ever_handed_off;
    unsigned borrow_mask;
    int quarantined, full_sync_pending;
    uint64_t view_nonce, view_token[2];
    struct stat lock_stat, manager_stat, device_stat, directory_stat;
    char name[64], device[128], sysdir[PATH_MAX], error[256];
    uint64_t physical, src_bytes, dst_bytes;
    unsigned char *mapping;
    long page;
};

static int fail(plc_lease *p, int error, const char *format, ...) {
    if (p) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(p->error, sizeof(p->error), format, args);
        va_end(args);
    }
    return -(error > 0 ? error : EIO);
}
static uint64_t ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value))
        return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}
static void fence(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("dmb osh" ::: "memory");
#else
    __sync_synchronize();
#endif
}
static int same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_rdev == b->st_rdev;
}
static int overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return a < b + bs && b < a + as;
}
int plc_validate_layout(uint64_t base, uint64_t size, uint64_t sdk, uint64_t sdk_size) {
    const uint64_t lower = UINT64_C(0x30000000), limit = UINT64_C(0x3e000000);
    if (size != PLC_LEASE_BYTES || base < lower || (base & 63) || base >= limit ||
        size > limit - base)
        return -ERANGE;
    if (!sdk_size || sdk < lower || ((sdk | sdk_size) & 63) || sdk >= limit ||
        sdk_size > limit - sdk)
        return -ERANGE;
    if (overlap(base, size, sdk, sdk_size) ||
        overlap(base, size, UINT64_C(0x3e000000), UINT64_C(0x02000000)))
        return -EADDRINUSE;
    return 0;
}
int plc_validate_name(const char *name) {
    if (!name || strnlen(name, 64) != 40 || strncmp(name, "opus_p1_", 8))
        return -EINVAL;
    for (size_t i = 8; i < 40; i++)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f')))
            return -EINVAL;
    return 0;
}
int plc_format_manager(char *out, size_t capacity, const char *verb, const char *name,
                       uint64_t size) {
    int count;
    if (!out || !capacity || !verb || plc_validate_name(name))
        return -EINVAL;
    if (!strcmp(verb, "create") && size == PLC_LEASE_BYTES)
        count = snprintf(out, capacity, "create %s %" PRIu64 "\n", name, size);
    else if (!strcmp(verb, "delete") && size == 0)
        count = snprintf(out, capacity, "delete %s\n", name);
    else
        return -EINVAL;
    if (count < 0 || (size_t)count >= capacity || count >= 256)
        return -ENOSPC;
    return count;
}
int plc_pack_sync(uint64_t offset, uint64_t length, unsigned direction, uint64_t *command) {
    if (!command || direction > 2 || !length || ((offset | length) & 63) ||
        offset >= PLC_LEASE_BYTES || length > PLC_LEASE_BYTES - offset)
        return -EINVAL;
    *command = (offset << 32) | length | ((uint64_t)direction << 2) | 1;
    return 0;
}
static int read_text(const char *path, char *out, size_t capacity) {
    if (capacity < 2)
        return -EINVAL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -errno;
    ssize_t count = read(fd, out, capacity - 1);
    int saved = errno;
    if (close(fd) && count >= 0)
        return -errno;
    if (count < 0)
        return -saved;
    if ((size_t)count == capacity - 1)
        return -EOVERFLOW;
    out[count] = '\0';
    return 0;
}
static int number(const char *path, uint64_t *out) {
    char text[128], *end;
    int status = read_text(path, text, sizeof(text));
    if (status)
        return status;
    if (text[0] == '-' || text[0] == '+')
        return -EINVAL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno || end == text)
        return -EINVAL;
    while (*end == '\n' || *end == ' ' || *end == '\t')
        end++;
    if (*end)
        return -EINVAL;
    *out = (uint64_t)value;
    return 0;
}
static int attr(plc_lease *p, const char *name, uint64_t *out) {
    char path[PATH_MAX + 64];
    int n = snprintf(path, sizeof(path), "%s/%s", p->sysdir, name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return -ENAMETOOLONG;
    return number(path, out);
}
static int lock_check(plc_lease *p) {
    struct stat fd_st, path_st;
    char line[512];
    if (p->lockfd < 0 || fstat(p->lockfd, &fd_st) || lstat(LOCK_PATH, &path_st))
        return fail(p, EBADF, "Borrowed real lock FD/path unavailable");
    if (!S_ISREG(fd_st.st_mode) || !S_ISREG(path_st.st_mode) ||
        fd_st.st_ino != PLC_REAL_LOCK_INODE || !same(&fd_st, &path_st) ||
        !same(&fd_st, &p->lock_stat))
        return fail(p, ESTALE, "Borrowed lock must match exact real lock inode943514");
    FILE *stream = fopen("/proc/locks", "re");
    if (!stream)
        return fail(p, errno, "Cannot verify exclusive flock ownership");
    int found = 0;
    while (fgets(line, sizeof(line), stream)) {
        unsigned long long id, ino;
        long owner;
        unsigned device_major, device_minor;
        char begin[32], end[32];
        if (sscanf(line, "%llu: FLOCK ADVISORY WRITE %ld %x:%x:%llu %31s %31s", &id, &owner,
                   &device_major, &device_minor, &ino, begin, end) == 7 &&
            owner == (long)getpid() && ino == (uint64_t)fd_st.st_ino &&
            device_major == major(fd_st.st_dev) && device_minor == minor(fd_st.st_dev) &&
            !strcmp(begin, "0") && !strcmp(end, "EOF")) {
            found = 1;
            break;
        }
    }
    int read_error = ferror(stream);
    (void)fclose(stream);
    return found && !read_error
               ? 0
               : fail(
                     p, EPERM,
                     "This process must own real exclusive FLOCK; inherited parent ownership rejected");
}
static int manager_check(plc_lease *p) {
    struct stat st, path_st;
    char dev[80];
    unsigned a, b;
    char extra;
    if (fstat(p->managerfd, &st) || lstat(MANAGER, &path_st) || !same(&st, &p->manager_stat) ||
        !same(&st, &path_st) || !S_ISCHR(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0777) != 0600)
        return fail(p, ESTALE, "Manager identity/owner/mode changed");
    if (read_text("/sys/class/misc/u-dma-buf-mgr/dev", dev, sizeof(dev)) ||
        sscanf(dev, "%u:%u %c", &a, &b, &extra) != 2 || makedev(a, b) != st.st_rdev)
        return fail(p, ENODEV, "Manager sysfs dev identity mismatch");
    return 0;
}
static int identity_check(plc_lease *p, int enforce_layout) {
    char alias[PATH_MAX + 64], resolved[PATH_MAX], devtext[80], version[80];
    struct stat directory, node;
    unsigned a, b;
    char extra;
    uint64_t phys, size, mode, coherent, sdk, sdk_size;
    if (!p->identity || !p->created)
        return fail(p, ENODEV, "Private identity not established; retain lease");
    if (lock_check(p) || manager_check(p))
        return -EPERM;
    (void)snprintf(alias, sizeof(alias), CLASS "%s", p->name);
    if (!realpath(alias, resolved) || strcmp(resolved, p->sysdir) || stat(p->sysdir, &directory) ||
        !same(&directory, &p->directory_stat))
        return fail(p, ESTALE, "Private sysfs identity changed");
    (void)snprintf(alias, sizeof(alias), "%s/dev", p->sysdir);
    if (read_text(alias, devtext, sizeof(devtext)) ||
        sscanf(devtext, "%u:%u %c", &a, &b, &extra) != 2)
        return fail(p, EPROTO, "Private sysfs device number invalid");
    if (lstat(p->device, &node) || !S_ISCHR(node.st_mode) || node.st_uid != 0 ||
        (node.st_mode & 0077) || !same(&node, &p->device_stat) || makedev(a, b) != node.st_rdev)
        return fail(p, ESTALE, "Private devnode identity/permissions changed");
    if (p->devicefd >= 0) {
        struct stat opened;
        if (fstat(p->devicefd, &opened) || !same(&node, &opened))
            return fail(p, ESTALE, "Private FD mismatch");
    }
    if (attr(p, "phys_addr", &phys) || attr(p, "size", &size) || attr(p, "sync_mode", &mode) ||
        attr(p, "dma_coherent", &coherent) || phys != p->physical || size != PLC_LEASE_BYTES ||
        mode != 1 || coherent != 0)
        return fail(p, ESTALE, "Private physical/size/cache contract changed");
    char version_path[PATH_MAX + 32];
    (void)snprintf(version_path, sizeof(version_path), "%s/driver_version", p->sysdir);
    if (read_text(version_path, version, sizeof(version)) || strcmp(version, "3.2.5\n"))
        return fail(p, EPROTONOSUPPORT, "Private driver_version not3.2.5");
    if (number(CLASS "udmabuf0/phys_addr", &sdk) || number(CLASS "udmabuf0/size", &sdk_size))
        return fail(p, ERANGE, "Protected SDK buffer extent unavailable");
    if (enforce_layout && plc_validate_layout(phys, size, sdk, sdk_size))
        return fail(p, ERANGE, "Private allocation overlaps protected or out-of-whitelist memory");
    return 0;
}
static int private_check(plc_lease *p) { return identity_check(p, 1); }
int plc_validate_idle_evidence(uint32_t expected_signature, const plc_idle_evidence *a,
                               const plc_idle_evidence *b) {
    if (expected_signature != PLC_P3_SIGNATURE || !a || !b)
        return -EINVAL;
    if (a->signature != PLC_P3_SIGNATURE || b->signature != PLC_P3_SIGNATURE || a->idle != 1 ||
        b->idle != 1 || a->starts_inhibited != 1 || b->starts_inhibited != 1 ||
        a->release_ack != 1 || b->release_ack != 1 || a->busy || b->busy || a->command_pending ||
        b->command_pending || a->quarantine || b->quarantine || a->read_outstanding ||
        b->read_outstanding || a->write_outstanding || b->write_outstanding ||
        a->completion_epoch != b->completion_epoch)
        return -EBUSY;
    return 0;
}
int plc_validate_idle_epoch(uint32_t signature, uint64_t epoch, const plc_idle_evidence *a,
                            const plc_idle_evidence *b) {
    int rc = plc_validate_idle_evidence(signature, a, b);
    if (rc)
        return rc;
    return a->completion_epoch == epoch && b->completion_epoch == epoch ? 0 : -EBUSY;
}
static int idle_check(plc_lease *p, const plc_idle_guard *guard) {
    plc_idle_evidence a = {0}, b = {0};
    struct timespec pause = {0, 1000000};
    if (!guard || !guard->read_idle || guard->expected_signature != PLC_P3_SIGNATURE)
        return fail(p, EINVAL, "Signed DMA idle probe required; M512 has no new DMA engine");
    if (lock_check(p))
        return -EPERM;
    fence();
    if (guard->read_idle(guard->context, &a))
        return fail(p, EBUSY, "DMA state unknown: buffer retained");
    if (nanosleep(&pause, NULL))
        return fail(p, EINTR, "Idle verification interrupted: buffer retained");
    fence();
    if (guard->read_idle(guard->context, &b))
        return fail(p, EBUSY, "DMA second state unknown: buffer retained");
    if (plc_validate_idle_epoch(guard->expected_signature, guard->expected_epoch, &a, &b))
        return fail(
            p, EBUSY,
            "DMA signature/idle/fence/busy/pending/quarantine/outstanding/epoch proof failed: retain buffer");
    fence();
    return lock_check(p);
}
static int command(plc_lease *p, const char *verb) {
    char text[128];
    int count = plc_format_manager(text, sizeof(text), verb, p->name,
                                   !strcmp(verb, "create") ? PLC_LEASE_BYTES : 0);
    if (count < 0)
        return fail(p, -count, "Invalid manager command");
    if (lock_check(p) || manager_check(p))
        return -EPERM;
    /* One write; never retry a partially/possibly executed manager command. */
    ssize_t written = write(p->managerfd, text, (size_t)count);
    if (written != count)
        return fail(p, written < 0 ? errno : EIO,
                    "Manager %s write incomplete; no retry, buffer may exist", verb);
    return 0;
}
int plc_lease_create(plc_lease **out, int lockfd, uint64_t expected_inode) {
    if (!out || *out || lockfd < 0 || expected_inode != PLC_REAL_LOCK_INODE)
        return -EINVAL;
    if (geteuid() != 0)
        return -EPERM;
    plc_lease *p = calloc(1, sizeof(*p));
    if (!p)
        return -ENOMEM;
    p->lockfd = lockfd;
    p->managerfd = -1;
    p->devicefd = -1;
    p->page = sysconf(_SC_PAGESIZE);
    if (p->page != 4096 || fstat(lockfd, &p->lock_stat) || lock_check(p)) {
        free(p);
        return -EPERM;
    }
    p->managerfd = open(MANAGER, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (p->managerfd < 0) {
        int e = errno;
        free(p);
        return -e;
    }
    if (fstat(p->managerfd, &p->manager_stat) || manager_check(p)) {
        (void)close(p->managerfd);
        free(p);
        return -EPERM;
    }
    unsigned char random[16];
    ssize_t random_count = getrandom(random, sizeof(random), 0);
    if (random_count != (ssize_t)sizeof(random)) {
        (void)close(p->managerfd);
        free(p);
        return -EIO;
    }
    memcpy(p->name, "opus_p1_", 8);
    for (unsigned i = 0; i < 16; i++)
        (void)snprintf(p->name + 8 + i * 2, 3, "%02x", random[i]);
    (void)snprintf(p->device, sizeof(p->device), "/dev/%s", p->name);
    char alias[160];
    (void)snprintf(alias, sizeof(alias), CLASS "%s", p->name);
    struct stat st;
    if (lstat(p->device, &st) == 0 || errno != ENOENT) {
        (void)close(p->managerfd);
        free(p);
        return -EEXIST;
    }
    if (lstat(alias, &st) == 0 || errno != ENOENT) {
        (void)close(p->managerfd);
        free(p);
        return -EEXIST;
    }
    *out = p;
    p->created = 1; /* Uncertain command outcomes are retained for recovery. */
    int status = command(p, "create");
    if (status)
        return status;
    uint64_t start = ns();
    if (!start)
        return fail(p, EIO, "Monotonic timer failed; retain created lease");
    uint64_t deadline = start + UINT64_C(2000000000);
    struct timespec pause = {0, 20000000};
    while (lstat(p->device, &st) || lstat(alias, &st)) {
        uint64_t current = ns();
        if (!current)
            return fail(p, EIO, "Monotonic timer failed; retain created lease");
        if (current >= deadline)
            return fail(p, ETIMEDOUT, "Created buffer identity did not appear; retain lease/name");
        if (nanosleep(&pause, NULL))
            return fail(p, EINTR, "Creation observation interrupted; retain lease");
    }
    if (!realpath(alias, p->sysdir) || strncmp(p->sysdir, "/sys/devices/platform/", 22) ||
        stat(p->sysdir, &p->directory_stat))
        return fail(p, EPROTO, "New sysfs path not expected platform driver");
    size_t pathlen = strlen(p->sysdir), namelen = strlen(p->name);
    if (pathlen <= namelen || strcmp(p->sysdir + pathlen - namelen, p->name) ||
        p->sysdir[pathlen - namelen - 1] != '/')
        return fail(p, EPROTO, "New sysfs terminal name mismatch");
    if (lstat(p->device, &p->device_stat) || attr(p, "phys_addr", &p->physical))
        return fail(p, EIO, "New identity read failed");
    p->identity = 1;
    if ((status = private_check(p)))
        return status;
    p->devicefd = open(p->device, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (p->devicefd < 0)
        return fail(p, errno, "Open private cached device failed; retain buffer");
    if ((status = private_check(p)))
        return status;
    void *mapping =
        mmap(NULL, (size_t)PLC_LEASE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, p->devicefd, 0);
    if (mapping == MAP_FAILED)
        return fail(p, errno, "Private mmap failed; retain buffer");
    p->mapping = mapping;
    p->cpu_owned = 1;
    fence();
    if ((status = private_check(p)))
        return status;
    return 0;
}
const char *plc_lease_error(const plc_lease *p) { return p ? p->error : "No lease"; }
int plc_lease_get_info(plc_lease *p, plc_lease_info *out) {
    if (!p || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    memcpy(out->name, p->name, sizeof(out->name));
    out->physical_base = p->physical;
    out->bytes = PLC_LEASE_BYTES;
    out->src_physical = p->physical;
    out->dst_physical = p->physical + PLC_DST_OFFSET;
    out->src_bytes = p->src_bytes;
    out->dst_bytes = p->dst_bytes;
    out->device_inode = p->device_stat.st_ino;
    out->created = p->created;
    out->identity_verified = p->identity;
    out->cpu_owned = p->cpu_owned;
    out->mapped = p->mapping != NULL;
    out->ever_handed_off = p->ever_handed_off;
    return 0;
}
static int private_sync(plc_lease *p, uint64_t offset, uint64_t bytes, unsigned direction,
                        int for_cpu, plc_sync_timing *timing) {
    uint64_t packed;
    char path[PATH_MAX + 40], text[40];
    if (!p || !timing || (for_cpu != 0 && for_cpu != 1) ||
        plc_pack_sync(offset, bytes, direction, &packed))
        return -EINVAL;
    int status = private_check(p);
    if (status)
        return status;
    (void)snprintf(path, sizeof(path), "%s/%s", p->sysdir,
                   for_cpu ? "sync_for_cpu" : "sync_for_device");
    int fd = open(path, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        p->quarantined = 1;
        return fail(p, errno, "Open private sync attribute failed");
    }
    int count = snprintf(text, sizeof(text), "0x%016" PRIx64 "\n", packed);
    uint64_t begin = ns();
    fence();
    ssize_t written = write(fd, text, (size_t)count);
    int error = errno;
    fence();
    uint64_t end = ns();
    int close_error = close(fd);
    timing->packed_command = packed;
    timing->elapsed_ns = end >= begin ? end - begin : 0;
    if (written != count || close_error) {
        p->quarantined = 1;
        return fail(p, written < 0 ? error : EIO, "Private sync write failed");
    }
    return 0;
}
static int guards_full(plc_lease *p, int initialize) {
    if (!p->src_bytes || !p->dst_bytes)
        return initialize ? -EINVAL : 0;
    uint64_t lengths[2] = {p->src_bytes, p->dst_bytes};
    for (unsigned slot = 0; slot < 2; slot++) {
        unsigned char *begin = p->mapping + slot * PLC_SLOT_BYTES + lengths[slot];
        size_t bytes = (size_t)(PLC_SLOT_BYTES - lengths[slot]);
        if (initialize)
            memset(begin, GUARD_BYTE, bytes);
        else
            for (size_t i = 0; i < bytes; i++)
                if (begin[i] != GUARD_BYTE) {
                    p->quarantined = 1;
                    return fail(p, EOVERFLOW, "DMA tail/page guard corrupted; retain buffer");
                }
    }
    fence();
    return 0;
}
/* Per-frame fixed 4KiB tails. Full private tails are initialized/verified at
 * shape boundaries and batch close; no whole-tail scan is hidden per API. */
static int guards_fast(plc_lease *p, int initialize) {
    if (!p->src_bytes || !p->dst_bytes)
        return initialize ? -EINVAL : 0;
    uint64_t lengths[2] = {p->src_bytes, p->dst_bytes};
    for (unsigned slot = 0; slot < 2; slot++) {
        unsigned char *begin = p->mapping + slot * PLC_SLOT_BYTES + lengths[slot];
        size_t bytes = 4096;
        if (initialize)
            memset(begin, GUARD_BYTE, bytes);
        else
            for (size_t i = 0; i < bytes; i++)
                if (begin[i] != GUARD_BYTE) {
                    p->quarantined = 1;
                    return fail(p, EOVERFLOW, "Fixed frame guard corrupted; retain buffer");
                }
    }
    fence();
    return 0;
}
static int full_reclaim_and_check(plc_lease *p) {
    plc_sync_timing t;
    int status;
    if (p->full_sync_pending) {
        if ((status = private_sync(p, 0, PLC_LEASE_BYTES, 0, 0, &t)))
            return status;
        p->full_sync_pending = 0;
    }
    if ((status = private_sync(p, 0, PLC_LEASE_BYTES, 0, 1, &t)))
        return status;
    return guards_full(p, 0);
}

int plc_lease_prepare(plc_lease *p, uint64_t src, uint64_t dst, const plc_idle_guard *guard) {
    if (p && p->quarantined)
        return fail(p, EIO, "Sticky cache/guard quarantine retains buffer");
    if (p && p->borrow_mask)
        return fail(p, EBUSY, "Live external view retains private mapping");
    if (!p || !p->mapping || !src || !dst || ((src | dst) & 63) || src > PLC_SLOT_BYTES - 4096 ||
        dst > PLC_SLOT_BYTES - 4096)
        return -EINVAL;
    int status = private_check(p);
    if (status)
        return status;
    if ((status = idle_check(p, guard)))
        return status;
    if (!p->cpu_owned)
        return fail(p, EBUSY, "Reclaim before prepare");
    if (p->src_bytes == src && p->dst_bytes == dst)
        return guards_fast(p, 0);
    if (p->src_bytes && (status = full_reclaim_and_check(p)))
        return status;
    p->src_bytes = src;
    p->dst_bytes = dst;
    status = guards_full(p, 1);
    if (!status)
        p->full_sync_pending = 1;
    return status;
}
int plc_lease_write_source(plc_lease *p, const void *data, size_t bytes) {
    if (p && p->quarantined)
        return fail(p, EIO, "Sticky cache/guard quarantine retains buffer");
    if (p && p->borrow_mask)
        return fail(p, EBUSY, "Live external view retains private mapping");
    if (!p || !p->mapping || !p->cpu_owned || !data || bytes != p->src_bytes || !bytes)
        return -EINVAL;
    int status = private_check(p);
    if (status)
        return status;
    if ((status = guards_fast(p, 0)))
        return status;
    memcpy(p->mapping, data, bytes);
    fence();
    return 0;
}
int plc_lease_handoff(plc_lease *p, plc_lease_info *out) {
    if (p && p->quarantined)
        return fail(p, EIO, "Sticky cache/guard quarantine retains buffer");
    if (p && p->borrow_mask)
        return fail(p, EBUSY, "Live external view retains private mapping");
    if (!p || !out || !p->mapping || !p->cpu_owned || !p->src_bytes || !p->dst_bytes)
        return -EINVAL;
    int status = private_check(p);
    if (status)
        return status;
    if ((status = guards_fast(p, 0)))
        return status;
    plc_sync_timing t;
    if (p->full_sync_pending) {
        if ((status = private_sync(p, 0, PLC_LEASE_BYTES, 0, 0, &t)))
            return status;
        p->full_sync_pending = 0;
    } else {
        if ((status = private_sync(p, PLC_SRC_OFFSET, p->src_bytes + 4096, 0, 0, &t)))
            return status;
        if ((status = private_sync(p, PLC_DST_OFFSET, p->dst_bytes + 4096, 0, 0, &t)))
            return status;
    }
    fence();
    p->cpu_owned = 0;
    p->ever_handed_off = 1;
    return plc_lease_get_info(p, out);
}
int plc_lease_fill_destination(plc_lease *p, unsigned char value, size_t bytes) {
    if (p && p->quarantined)
        return fail(p, EIO, "Sticky cache/guard quarantine retains buffer");
    if (p && p->borrow_mask)
        return fail(p, EBUSY, "Live external view retains private mapping");
    if (!p || !p->mapping || !p->cpu_owned || bytes != p->dst_bytes || !bytes)
        return -EINVAL;
    int status = private_check(p);
    if (status)
        return status;
    if ((status = guards_fast(p, 0)))
        return status;
    memset(p->mapping + PLC_DST_OFFSET, value, bytes);
    fence();
    return 0;
}
int plc_lease_read_source(plc_lease *p, void *data, size_t bytes) {
    if (p && p->quarantined)
        return fail(p, EIO, "Sticky cache/guard quarantine retains buffer");
    if (p && p->borrow_mask)
        return fail(p, EBUSY, "Live external view retains private mapping");
    if (!p || !p->mapping || !p->cpu_owned || !data || bytes != p->src_bytes || !bytes)
        return -EINVAL;
    int status = private_check(p);
    if (status)
        return status;
    if ((status = guards_fast(p, 0)))
        return status;
    fence();
    memcpy(data, p->mapping + PLC_SRC_OFFSET, bytes);
    return 0;
}
int plc_lease_reclaim(plc_lease *p, const plc_idle_guard *guard) {
    if (p && p->quarantined)
        return fail(p, EIO, "Sticky cache/guard quarantine retains buffer");
    if (p && p->borrow_mask)
        return fail(p, EBUSY, "Live external view retains private mapping");
    if (!p || !p->mapping)
        return -EINVAL;
    int status = private_check(p);
    if (status)
        return status;
    if (p->cpu_owned)
        return fail(p, EALREADY, "Already CPU owned; do not invalidate dirty CPU data");
    if ((status = idle_check(p, guard)))
        return status;
    plc_sync_timing t;
    if ((status = private_sync(p, PLC_SRC_OFFSET, p->src_bytes + 4096, 0, 1, &t)))
        return status;
    if ((status = private_sync(p, PLC_DST_OFFSET, p->dst_bytes + 4096, 0, 1, &t)))
        return status;
    if ((status = guards_fast(p, 0)))
        return status;
    p->cpu_owned = 1;
    return 0;
}
int plc_lease_read_destination(plc_lease *p, void *data, size_t bytes) {
    if (p && p->quarantined)
        return fail(p, EIO, "Sticky cache/guard quarantine retains buffer");
    if (p && p->borrow_mask)
        return fail(p, EBUSY, "Live external view retains private mapping");
    if (!p || !p->mapping || !p->cpu_owned || !data || bytes != p->dst_bytes || !bytes)
        return -EINVAL;
    int status = private_check(p);
    if (status)
        return status;
    if ((status = guards_fast(p, 0)))
        return status;
    fence();
    memcpy(data, p->mapping + PLC_DST_OFFSET, bytes);
    return 0;
}
int plc_lease_sync(plc_lease *p, uint64_t offset, uint64_t bytes, unsigned direction, int for_cpu,
                   plc_sync_timing *timing) {
    if (p && p->quarantined)
        return fail(p, EIO, "Sticky cache/guard quarantine retains buffer");
    if (!p || !p->cpu_owned || p->borrow_mask)
        return -EBUSY;
    return private_sync(p, offset, bytes, direction, for_cpu, timing);
}

int plc_lease_release(plc_lease **handle, const plc_idle_guard *guard) {
    if (handle && *handle && (*handle)->quarantined)
        return fail(*handle, EIO, "Sticky cache/guard quarantine retains buffer");
    if (handle && *handle && (*handle)->borrow_mask)
        return fail(*handle, EBUSY, "Live external view retains private mapping");
    if (!handle || !*handle)
        return -EINVAL;
    plc_lease *p = *handle;
    /* A newly created allocation rejected by the whitelist may be deleted only
     * if never handed to DMA; its exact dev/sysfs identity and fresh idle proof
     * are still mandatory. Once handed off, all layout guards remain strict. */
    int status = identity_check(p, p->ever_handed_off);
    if (status)
        return status;
    if ((status = idle_check(p, guard)))
        return status;
    if (p->mapping && (status = full_reclaim_and_check(p)))
        return status;
    /* DMA is proven idle with starts inhibited. Only now unmap/close our FD.
     * Delete is unsafe while driver retains live VMAs, so it follows unmap. */
    if (p->mapping) {
        if (munmap(p->mapping, (size_t)PLC_LEASE_BYTES))
            return fail(p, errno, "Safe-idle munmap failed; keep lease");
        p->mapping = NULL;
    }
    if (p->devicefd >= 0) {
        int fd = p->devicefd;
        p->devicefd = -1;
        if (close(fd))
            return fail(p, errno,
                        "Safe-idle close returned error; lease retained without retrying fd");
    }
    if ((status = command(p, "delete")))
        return status;
    char alias[160];
    struct stat st;
    (void)snprintf(alias, sizeof(alias), CLASS "%s", p->name);
    if (lstat(p->device, &st) == 0 || errno != ENOENT || lstat(alias, &st) == 0 || errno != ENOENT)
        return fail(p, EIO, "Delete identity still exists; retain lease record, do not recreate");
    p->created = 0;
    (void)close(p->managerfd);
    p->managerfd = -1;
    memset(p, 0, sizeof(*p));
    free(p);
    *handle = NULL;
    return 0;
}

static int borrow_region(plc_lease *p, unsigned which, void **ptr, size_t *bytes, uint64_t *token) {
    if (p && p->quarantined)
        return fail(p, EIO, "Sticky cache/guard quarantine retains buffer");
    if (!p || !ptr || !bytes || !token || which > 1 || !p->mapping || !p->cpu_owned ||
        !p->src_bytes || !p->dst_bytes)
        return -EINVAL;
    int status = private_check(p);
    if (status)
        return status;
    if ((status = guards_fast(p, 0)))
        return status;
    if (p->borrow_mask & (1u << which))
        return fail(p, EBUSY, "Region already pinned");
    if (p->view_nonce == UINT64_MAX)
        return fail(p, EOVERFLOW, "View nonce exhausted");
    *token = ++p->view_nonce;
    p->view_token[which] = *token;
    p->borrow_mask |= 1u << which;
    *ptr = p->mapping + (which ? PLC_DST_OFFSET : PLC_SRC_OFFSET);
    *bytes = (size_t)(which ? p->dst_bytes : p->src_bytes);
    return 0;
}
int plc_lease_borrow_source(plc_lease *p, void **ptr, size_t *bytes, uint64_t *token) {
    return borrow_region(p, 0, ptr, bytes, token);
}
int plc_lease_borrow_destination(plc_lease *p, void **ptr, size_t *bytes, uint64_t *token) {
    return borrow_region(p, 1, ptr, bytes, token);
}
int plc_lease_return_view(plc_lease *p, uint64_t token) {
    if (!p || !token)
        return -EINVAL;
    int status = private_check(p);
    if (status)
        return status;
    for (unsigned i = 0; i < 2; i++)
        if ((p->borrow_mask & (1u << i)) && p->view_token[i] == token) {
            p->borrow_mask &= ~(1u << i);
            p->view_token[i] = 0;
            return 0;
        }
    return fail(p, ESTALE, "Stale or duplicate external view token");
}
int plc_lease_check_full_batch(plc_lease *p, const plc_idle_guard *guard) {
    if (p && p->quarantined)
        return fail(p, EIO, "Sticky cache/guard quarantine retains buffer");
    if (!p || !p->mapping || !p->cpu_owned || p->borrow_mask)
        return -EBUSY;
    int status = private_check(p);
    if (status)
        return status;
    if ((status = idle_check(p, guard)))
        return status;
    return full_reclaim_and_check(p);
}
