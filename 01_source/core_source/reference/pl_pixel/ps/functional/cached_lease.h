// 定义图像缓冲区、缓存交接和回收接口。
#ifndef OPUS_P1_PLC_LEASE_H
#define OPUS_P1_PLC_LEASE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PLC_LEASE_BYTES (UINT64_C(16) << 20)
#define PLC_SLOT_BYTES (UINT64_C(8) << 20)
#define PLC_SRC_OFFSET UINT64_C(0)
#define PLC_DST_OFFSET PLC_SLOT_BYTES
#define PLC_REAL_LOCK_INODE UINT64_C(943514)
#define PLC_P3_SIGNATURE UINT32_C(0xa5080001)
#define PLC_LEASE_ABI_VERSION UINT32_C(4)
typedef struct plc_lease plc_lease;
/* Trusted upper-layer callback must READ actual signed DMA registers after
 * inhibiting all future starts while the same process holds the real lock.
 * Values must not be inferred from elapsed time or supplied as constants. */
typedef struct {
    uint32_t signature, idle, starts_inhibited, release_ack;
    uint32_t busy, command_pending, quarantine;
    uint32_t read_outstanding, write_outstanding;
    uint64_t completion_epoch;
} plc_idle_evidence;
typedef struct {
    uint32_t expected_signature;
    uint64_t expected_epoch;
    int (*read_idle)(void *context, plc_idle_evidence *evidence);
    void *context;
} plc_idle_guard;
/* ABI3: release_ack is the Opus246/247 P1-own predicate, not raw STATUS bit7.
 * read/write_outstanding represent bits8/9 only. Native bits10/11 and full
 * hardware fence bit7 are diagnostics, never evidence of private ownership.
 * Both independent samples must equal the guard's current expected_epoch.
 * A previous operation's guard must not be reused after a local MMIO write. */
typedef struct {
    char name[64];
    uint64_t physical_base, bytes, src_physical, dst_physical;
    uint64_t src_bytes, dst_bytes, device_inode;
    int created, identity_verified, cpu_owned, mapped, ever_handed_off;
} plc_lease_info;
typedef struct {
    uint64_t elapsed_ns, packed_command;
} plc_sync_timing;
/* Pure functions: safe for host controls; no file/device calls. */
int plc_validate_layout(uint64_t base, uint64_t size, uint64_t sdk_base, uint64_t sdk_size);
int plc_validate_name(const char *name);
int plc_format_manager(char *out, size_t capacity, const char *verb, const char *name,
                       uint64_t size);
int plc_pack_sync(uint64_t offset, uint64_t length, unsigned direction, uint64_t *command);
/* Validates the two live reads; pure host checks cannot establish that the
 * caller actually read MMIO. Trusted callback must map every field directly
 * from the hardware contract, including active-high quarantine and busy. */
int plc_validate_idle_evidence(uint32_t expected_signature, const plc_idle_evidence *first,
                               const plc_idle_evidence *second);
int plc_validate_idle_epoch(uint32_t expected_signature, uint64_t expected_epoch,
                            const plc_idle_evidence *first, const plc_idle_evidence *second);
/* P1 ctypes same-process contract only; NOT qualified for P3 M53 child workers.
 * Borrowed lockfd is NEVER unlocked, duplicated or closed by this library.
 * /proc/locks must identify getpid() as its exclusive FLOCK owner. An inherited
 * FD whose parent owns the lock is intentionally rejected. Call from the actual
 * lock-owner process (e.g. ctypes), not a new SDK/device probe process.
 * If create might have reached the manager, *out remains nonnull on failure:
 * preserve it and inspect the error; release still requires real idle proof. */
int plc_lease_create(plc_lease **out, int lockfd, uint64_t expected_inode);
/* Snapshot may describe a rejected lease for diagnosis; never submit DMA from
 * this snapshot. Only successful handoff grants upper-layer submission. */
int plc_lease_get_info(plc_lease *lease, plc_lease_info *out);
const char *plc_lease_error(const plc_lease *lease);
int plc_lease_prepare(plc_lease *lease, uint64_t src_bytes, uint64_t dst_bytes,
                      const plc_idle_guard *guard);
int plc_lease_write_source(plc_lease *lease, const void *data, size_t bytes);
/* CPU ownership only, exactly the prepared payload length; guards untouched. */
int plc_lease_fill_destination(plc_lease *lease, unsigned char value, size_t bytes);
int plc_lease_read_source(plc_lease *lease, void *data, size_t bytes);
/* Handoff before upper-layer DMA start; reclaim only with fresh hardware idle
 * proof. No SDK, register mapping, start command, or DMA is performed here. */
int plc_lease_handoff(plc_lease *lease, plc_lease_info *info);
int plc_lease_reclaim(plc_lease *lease, const plc_idle_guard *guard);
int plc_lease_read_destination(plc_lease *lease, void *data, size_t bytes);
/* Optional explicit sync touches ONLY this lease's private buffer. This ABI4
 * lease is cached, sync_mode1, with mandatory handoff/reclaim maintenance.
 * direction:0 bidirectional,1 to device,2 from device. */
int plc_lease_sync(plc_lease *lease, uint64_t offset, uint64_t bytes, unsigned direction,
                   int for_cpu, plc_sync_timing *timing);
/* Unknown/not-idle/error/guard corruption: retains mapping, FD and buffer.
 * Never auto-cleans up a possibly active DMA target. Successful release closes
 * only owned FDs/mapping and deletes exactly its recorded unique name. */
int plc_lease_release(plc_lease **lease, const plc_idle_guard *guard);
/* ABI4 prototype: cached map, mandatory range sync, serialized same-process
 * owner only. Caller must retain each token through the final SDK alias AND
 * actual inference completion. An alias release alone is not a device fence.
 * No automatic unmap/delete after failure. Not yet physically qualified. */
int plc_lease_borrow_source(plc_lease *, void **, size_t *, uint64_t *);
int plc_lease_borrow_destination(plc_lease *, void **, size_t *, uint64_t *);
int plc_lease_return_view(plc_lease *, uint64_t);
int plc_lease_check_full_batch(plc_lease *, const plc_idle_guard *);
#ifdef __cplusplus
}
#endif
#endif
