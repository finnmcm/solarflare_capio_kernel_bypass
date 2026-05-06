#include "sfc7120_mmio.h"

void
sfc7120_dump_regs(sfc7120_softc_t *sc)
{
    device_printf(sc->dev, "Solarflare 7120 register dump\n");
    device_printf(sc->dev, "  HW_REV_ID  = %08x\n",
                  SFC7120_READ_REG(sc, SFC7120_REG_BIU_HW_REV_ID));
    device_printf(sc->dev, "  MC_STATUS  = %08x\n",
                  SFC7120_READ_REG(sc, SFC7120_REG_MC_STATUS));
    /* TODO: extend with per-channel EVQ/RXQ/TXQ state once bringup lands. */
}
