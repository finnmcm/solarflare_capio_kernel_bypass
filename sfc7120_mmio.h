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
 * The values below are PLACEHOLDERS keyed off public Solarflare MCDI/EF10
 * documentation patterns and the FreeBSD `sfxge` driver. Cross-check
 * against the EF10 register documentation for your specific SFN7xxx board
 * before depending on these.
 */

/* MCDI request doorbell + return doorbell. EF10 keeps the MCDI message
 * itself in a DMA buffer; only the doorbell is in MMIO. */
#define SFC7120_REG_MCDB            0x0e80    /* MC doorbell */
#define SFC7120_REG_MC_EVENT        0x0e84

/* Per-channel doorbells (channel 0; per-channel stride is hardware-defined). */
#define SFC7120_REG_EVQ_RPTR_DBL    0x0500
#define SFC7120_REG_RX_DESC_DBL     0x0510
#define SFC7120_REG_TX_DESC_DBL     0x0518

/* Boot/identity registers. */
#define SFC7120_REG_BIU_HW_REV_ID   0x0010
#define SFC7120_REG_MC_STATUS       0x0c7c

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
