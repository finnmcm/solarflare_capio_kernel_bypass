#ifndef SFC7120_MMIO_HEADER
#define SFC7120_MMIO_HEADER

#include "capio.h"
#include "sfc7120.h"

/*
 * Solarflare EF10 (Huntington / SFN7xxx) BAR0 register offsets.
 *
 * EF10 is fundamentally different from e1000 — it talks to firmware (the
 * Management Controller, "MC") through MCDI: a doorbell + DMA-resident
 * mailbox protocol, similar in spirit to mlx5pol's command queue. The
 * register set userspace cares about is small:
 *
 *   - MC doorbell   — kick MCDI requests
 *   - EVQ doorbells — write read pointer to ack events
 *   - RX/TX doorbells — push descriptor producer pointers
 *
 * The MC doorbell, RX_DESC_UPD, and TX_DESC_UPD offsets below are verified
 * working on the SFN7322F-R2 via the kernel MCDI/TX/RX paths. The data-EVQ
 * RPTR offset (0x2400) is derived from the same per-VI window formula
 * sfxge uses (EFX_BAR_VI_WRITED, step 8192) — pending hardware
 * verification.
 */

/* MCDI request doorbell. EF10 splits this into a low-word (0x0200) and
 * high-word (0x0204) register; for MCDI you only need to write the low word.
 * Verified against ER_DZ_MC_DB_LWRD_REG_OFST in efx_regs_ef10.h. */
#define SFC7120_REG_MCDB            0x0200    /* MC doorbell low word */
/* #define SFC7120_REG_MC_EVENT — does not exist as an MMIO register on EF10.
 * MC events are delivered through the EVQ ring (DMA buffer), not via a
 * dedicated BAR address. No slice entry should reference this. */

/* Per-VI doorbell window addressing.
 * Each function-local VI owns an 8 KB doorbell window in the BAR:
 *   reg_offset + (vi_local_index << SFC7120_VI_WINDOW_SHIFT)
 * Huntington uses EFX_VI_WINDOW_SHIFT_8K (shift=13, stride=8192); see
 * sfxge's EFX_BAR_VI_WRITED / ER_DZ_*_REG_STEP = 8192.
 * Confirmed from efx_regs_ef10.h: ER_DZ_EVQ_RPTR_REG_OFST,
 * ER_DZ_RX_DESC_UPD_REG_OFST, ER_DZ_TX_DESC_UPD_REG_OFST. */
#define SFC7120_VI_WINDOW_SHIFT     13
#define SFC7120_VI_WINDOW(inst)     ((uint64_t)(inst) << SFC7120_VI_WINDOW_SHIFT)

/* Function-relative data-path instance numbers. MUST match the
 * init_evq/init_rxq/init_txq calls in sfc7120_hw_init (and the values
 * SFC7120_GET_VI_INFO reports to userspace): data EVQ = 1 (EVQ 0 is the
 * kernel-owned control queue), RXQ = 0, TXQ = 0. */
#define SFC7120_DATA_EVQ_INSTANCE   1
#define SFC7120_RXQ_INSTANCE        0
#define SFC7120_TXQ_INSTANCE        0

/* Per-window (VI-relative) doorbell offsets. */
#define SFC7120_REG_EVQ_RPTR_DBL    0x0400  /* ERF_DZ_EVQ_RPTR  LBN=0 WIDTH=15 */
#define SFC7120_REG_RX_DESC_DBL     0x0830  /* ERF_DZ_RX_DESC_WPTR LBN=0 WIDTH=12; align to 8 */
#define SFC7120_REG_TX_DESC_DBL     0x0a10  /* 128-bit TX_DESC_UPD base (full push) */
/* TX write-pointer-only doorbell: EFX_BAR_VI_WRITED2 adds 8 bytes (2 dwords)
 * to the base, landing on dword[2] which holds ERF_DZ_TX_DESC_WPTR (LBN=64,
 * WIDTH=12 of the 128-bit register). See ef10_tx.c:ef10_tx_qpush. */
#define SFC7120_REG_TX_WPTR_DBL     (SFC7120_REG_TX_DESC_DBL + 8)  /* 0x0a18 */

/* Absolute BAR offsets of the DATA-PATH doorbells: per-window offset plus
 * the owning VI's window. These are what the kernel TX/RX handlers write
 * and what the CAPIO slice manifest (sfc7120_tables.c) exposes to
 * userspace. The data EVQ is instance 1, so its RPTR doorbell is at
 * 0x2400 — NOT 0x0400, which is the control EVQ 0's (kernel-owned). */
#define SFC7120_REG_DATA_EVQ_RPTR_DBL \
    (SFC7120_VI_WINDOW(SFC7120_DATA_EVQ_INSTANCE) + SFC7120_REG_EVQ_RPTR_DBL) /* 0x2400 */
#define SFC7120_REG_DATA_RX_DESC_DBL \
    (SFC7120_VI_WINDOW(SFC7120_RXQ_INSTANCE) + SFC7120_REG_RX_DESC_DBL)       /* 0x0830 */
#define SFC7120_REG_DATA_TX_DESC_DBL \
    (SFC7120_VI_WINDOW(SFC7120_TXQ_INSTANCE) + SFC7120_REG_TX_DESC_DBL)       /* 0x0a10 */
#define SFC7120_REG_DATA_TX_WPTR_DBL \
    (SFC7120_REG_DATA_TX_DESC_DBL + 8)                                        /* 0x0a18 */

/* EVQ_RPTR doorbell: bits 0..14 are the read-pointer (mod 2^15). */
#define SFC7120_EVQ_RPTR_MASK       0x7fffu

/* Boot/identity registers.
 * BIU_HW_REV_ID verified against ER_DZ_BIU_HW_REV_ID_REG_OFST in efx_regs_ef10.h. */
#define SFC7120_REG_BIU_HW_REV_ID   0x0000
/* #define SFC7120_REG_MC_STATUS — does not exist as an MMIO register on EF10.
 * "MC_STATUS" in sfxge is a magic value (0xb007b007 / 0xdeaddead) written by
 * firmware into the MCDI *response buffer* in DMA memory, not a BAR offset. */

/* Userspace BAR window size — full BAR0. Caller must populate via
 * rman_get_size(). The slice manifest below is what bounds individual
 * userspace capabilities. */

/* ----------------------------------------------------------------------
 * R/W helpers — same shape as e1000's macros so existing code patterns
 * port cleanly. The `debug_reg_ops` flag in the softc, when true, logs
 * every access via device_printf for bringup debugging.
 * ---------------------------------------------------------------------- */

static __inline uint32_t
SFC7120_READ_REG(sfc7120_softc_t *sc, bus_size_t off)
{
    uint32_t v = bus_space_read_4(sc->mem_bus_tag, sc->mem_bsh, off);
    if (sc->debug_reg_ops)
        device_printf(sc->dev, "R [%04lx] = %08x\n",
                      (unsigned long)off, v);
    return v;
}

static __inline void
SFC7120_WRITE_REG(sfc7120_softc_t *sc, bus_size_t off, uint32_t v)
{
    if (sc->debug_reg_ops)
        device_printf(sc->dev, "W [%04lx] = %08x\n",
                      (unsigned long)off, v);
    bus_space_write_4(sc->mem_bus_tag, sc->mem_bsh, off, v);
}

/* Slice manifest — populated in sfc7120_tables.c. */
extern slice_def_t sfc7120_reg_slices[];
extern const size_t SFC7120_MMIO_SLICE_COUNT;

/* Diagnostic dump (optional; mirrors e1000e_dump_regs). */
void sfc7120_dump_regs(sfc7120_softc_t *sc);

#endif /* SFC7120_MMIO_HEADER */
