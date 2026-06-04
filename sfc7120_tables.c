#include "sfc7120_mmio.h"

/*
 * CAPIO MMIO slice manifest for the Solarflare 7120 (Huntington / EF10).
 *
 * Each entry defines a sub-capability that userspace can derive into the
 * BAR0 capability. modmap enforces these bounds in hardware: registers
 * not listed cannot be accessed by userspace, even if the offsets are
 * known. Read-only registers get CHERI_PERM_LOAD only.
 *
 * This is the SHORT initial set covering the registers a CAPIO
 * userspace driver minimally needs:
 *
 *   - MC doorbell (RW)               — push MCDI requests (low word only)
 *   - EVQ read-pointer doorbell (RW) — ack events
 *   - RX/TX descriptor doorbells (RW) — push producer pointer
 *   - HW_REV_ID (RO)                 — sanity check at attach
 *
 * MC_EVENT and MC_STATUS are omitted: neither is a real BAR register on EF10.
 * MC events arrive via the EVQ DMA ring; MC status is a magic value in the
 * MCDI response buffer.
 *
 * Extend this table when adding multi-queue support. Mirror the per-queue
 * pattern from mlx5pol: one slice per channel, named with the channel
 * index suffix.
 *
 * ORDER IS ABI: userspace indexes the copied-out slice array by position
 * (sfc7120_mmio_slice_idx_t in sfc7120_uapi.h). Any reorder/insert here
 * must update that enum in lockstep.
 */
slice_def_t sfc7120_reg_slices[] = {
    { SFC7120_REG_MCDB,              "MC_DOORBELL",       false, 4 },
    // { SFC7120_REG_MC_EVENT, "MC_EVENT", true, 4 },
    // Not a BAR register — MC events come via the EVQ DMA ring, not MMIO.
    /*
     * Data-path doorbells (Phase B). Each is the per-window offset plus the
     * owning VI's 8 KB window (instance << 13), matching the instances the
     * kernel programmed via INIT_EVQ/INIT_RXQ/INIT_TXQ: data EVQ = 1,
     * RXQ = 0, TXQ = 0. The data-EVQ RPTR therefore sits at 0x2400.
     *
     * The control EVQ 0's RPTR doorbell (0x0400) is deliberately NOT
     * exposed — EVQ 0 is kernel-owned (link/MCDI/error events, serviced by
     * the ISR); a userspace write there would corrupt the kernel's event
     * stream. With slicing, userspace cannot even derive a capability to it.
     */
    { SFC7120_REG_DATA_EVQ_RPTR_DBL, "DATA_EVQ_RPTR_DBL", false, 4 },  /* 0x2400 */
    { SFC7120_REG_DATA_RX_DESC_DBL,  "RX_DESC_DBL",       false, 4 },  /* 0x0830 */
    /* TX_DESC_UPD is a 128-bit (16-byte) register. The userspace driver needs
     * two dwords: base+0 (descriptor LWORD) and base+8 (wptr-only push, dword[2]
     * = ERF_DZ_TX_DESC_WPTR). 12 bytes covers both without exposing dword[3]. */
    { SFC7120_REG_DATA_TX_DESC_DBL,  "TX_DESC_DBL",       false, 12 }, /* 0x0a10–0x0a1b */
    { SFC7120_REG_BIU_HW_REV_ID,     "HW_REV_ID",         true,  4 },
    // { SFC7120_REG_MC_STATUS, "MC_STATUS", true, 4 },
    // Not a BAR register — MC_STATUS is a magic value in the MCDI DMA response buffer.
};

const size_t SFC7120_MMIO_SLICE_COUNT =
    sizeof(sfc7120_reg_slices) / sizeof(sfc7120_reg_slices[0]);
