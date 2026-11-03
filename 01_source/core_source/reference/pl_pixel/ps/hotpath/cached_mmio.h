// 定义预处理寄存器状态与帧提交接口。
#ifndef OPUS_PLC_MMIO_H
#define OPUS_PLC_MMIO_H
#include <stddef.h>
#include <stdint.h>
#include "cached_lease.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PLC_MMIO_ABI_VERSION UINT32_C(4)
#define PLC_MMIO_PHYSICAL UINT64_C(0x80080000)
#define PLC_MMIO_BYTES UINT32_C(4096)
#define PLC_MMIO_VERSION UINT32_C(0x00030002)
#define PLC_MMIO_SIGNATURE PLC_P3_SIGNATURE
typedef struct plc_mmio plc_mmio;
typedef struct {
    uint32_t signature, version, status, epoch, enable;
} plc_mmio_snapshot;
typedef struct {
    uint32_t base, limit, source, destination, length;
} plc_mmio_descriptor;
typedef struct {
    uint32_t issued, readback, epoch_before, epoch_after;
    uint32_t accepted_before, accepted_after, rejected_after;
} plc_config_attempt;
/* Pure functions: no MMIO, files, SDK, device, allocation or board calls. */
int plc_mmio_read_offset_allowed(uint32_t offset);
int plc_mmio_decode(const plc_mmio_snapshot *state, plc_idle_evidence *out);
int plc_mmio_write_allowed(uint32_t offset, uint32_t value, const plc_mmio_snapshot *state);
/* Full 16MiB lease, exclusive limit, two separate 8MiB slots. Addresses require
 * 64B alignment; arbitrary positive length retains a 4096B tail guard and the
 * rounded final 8B source read must remain in the source slot. Does not prove
 * the descriptor belongs to the caller's lease or excludes SDK allocations. */
int plc_mmio_validate_descriptor(const plc_mmio_descriptor *descriptor);
int plc_mmio_validate_mode_descriptor(uint32_t mode, const plc_mmio_descriptor *descriptor);
/* Fixed window only. Caller must own same-process real FLOCK inode943514.
 * Borrowed lock FD is never duplicated, unlocked or closed. Open performs
 * reads only; no Device::Open, manager, DMA memory or start side effects.
 * All functions return 0 or negative errno; failed writes are never retried. */
int plc_mmio_open(plc_mmio **out, int lockfd, uint64_t expected_inode);
int plc_mmio_read32(plc_mmio *context, uint32_t offset, uint32_t *value);
int plc_mmio_get_snapshot(plc_mmio *context, plc_mmio_snapshot *out);
/* STOP command2 / enable0 may inhibit active or quarantined state.
 * Config / CLEAR command4 require a P1/P2-own inhibited idle before any
 * START. Enable1 requires P1-own idle only (native video may remain active).
 * START command1 requires enabled, no busy/pending/quarantine/outstanding;
 * clear prior rejected/error state first. This is a register access contract;
 * caller must additionally validate the exact private lease address ranges. */
int plc_mmio_write32(plc_mmio *context, uint32_t offset, uint32_t value);
/* Callback for plc_idle_guard; reads actual STATUS/ENABLE/EPOCH and maps every
 * P1 ownership evidence. Guard expected_epoch must be refreshed after writes.
 * Native/full-fence diagnostics remain available in raw snapshot.status. */
int plc_mmio_idle_callback(void *context, plc_idle_evidence *out);
int plc_mmio_make_idle_guard(plc_mmio *context, plc_idle_guard *out);
/* Pin the externally tracked current epoch; a different observed epoch is an
 * error. A successful local store must advance this exact epoch by one. */
int plc_mmio_expect_epoch(plc_mmio *context, uint32_t expected);
/* Configuration phase only, never after START. P2-own idle is required; native activity does not
 * gate configuration. Returns0+issued0 if no window (no write), or0+issued1 with exact post-store
 * epoch/sequence/readback. Negative return is unknown, not retryable. Caller bounds descriptor
 * attempts20 and entire phase30s; CLEAR issued<=1. A known descriptor readback mismatch may be
 * retried only before DMA starts. */
int plc_mmio_config_once(plc_mmio *context, uint32_t offset, uint32_t value,
                         plc_config_attempt *out);
/* Serialized same-process borrowed-lock session only. Fast wait is armed only
 * by this context's successful START, and closed by inhibit/STOP. Never use as
 * lease-release proof. Caller must retain the lock FD without unlock/close or
 * other threads mutating it. Open/start and all lease boundaries keep full
 * identity checks. Forked processes are rejected; no fast-path sysfs/proc scan. */
int plc_mmio_get_wait_snapshot(plc_mmio *, plc_mmio_snapshot *);
int plc_mmio_validate_wait_sample(const plc_mmio_snapshot *, uint32_t epoch);
const char *plc_mmio_error(const plc_mmio *context);
/* Own /dev/mem mapping and FD only; no hardware write, wait, lease release or
 * claim of DMA idle. Caller must retain private buffer on unknown DMA state. */
int plc_mmio_close(plc_mmio **context);
#ifdef __cplusplus
}
#endif
#endif
