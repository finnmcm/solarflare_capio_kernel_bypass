# CheriBsdSolarflare7120 — Solarflare 7120 CAPIO Kernel Driver Stub

## What This Directory Is

Out-of-tree workspace for the Solarflare SFN7000-series (Huntington / EF10)
CAPIO kernel driver stub. Modeled on `~/CheriBsdE1000/`: cross-compile via
bmake on Linux, native make on CheriBSD, output `sfc7120pol.ko`.

**Status: SKELETON.** The PCI driver shell, CAPIO wiring, IOCTL dispatch,
and slice manifest exist. The hardware bringup (MCDI handshake, EVQ/RXQ/TXQ
init, MAC config, link config) is intentionally stubbed with TODO blocks.
This directory is the iteration workspace; an in-tree copy at
`~/cheri/cheribsd/sys/modules/sfc7120pol/` should follow once the driver
stabilizes (matching the e1000 / mlx5pol layout).

There is currently no in-tree counterpart and no userspace driver. Add
both as the work progresses.

---

## Hardware Target

**Card under test:** Solarflare **SFN7322F-R2** — Flareon Ultra Dual-Port
10GbE PCIe 3.0 Server I/O Adapter (Precision Time edition).

- Controller: **SFC9120** (Huntington, EF10 family)
- Two physical 10GbE ports → two PFs (function 0 = port 0, function 1 = port 1)
- Vendor / device IDs: `0x1924` / `0x0903` (PF), `0x1924` / `0x1903` (VF)
- Subsystem: `0x1924:0x8007` (this is the "-R2 Precision Time" SKU)

`pciconf -lvb` for the unit our driver attaches against (Function 1):

```
class=0x020000 rev=0x01 hdr=0x00
vendor=0x1924 device=0x0903 subvendor=0x1924 subdevice=0x8007
    vendor     = 'AMD Solarflare'
    device     = 'SFC9120 10G Ethernet Controller'
    cap 01[40] = powerspec 3  supports D0 D1 D2 D3  current D0
    cap 05[50] = MSI supports 1 message, 64 bit
    cap 10[70] = PCI-Express 2 endpoint max data 128(2048) FLR RO NS
                 max read 512
                 link x8(x8) speed 8.0(8.0) ASPM disabled(L0s/L1)
    cap 11[b0] = MSI-X supports 32 messages
                 Table in map 0x20[0x0], PBA in map 0x20[0x2000]
    cap 03[d0] = VPD
    ecap 0001[100] = AER 2 0 fatal 1 non-fatal 1 corrected
    ecap 0003[140] = Serial 1 000f53ffff285490
    ecap 000e[150] = ARI 1
    ecap 0019[160] = PCIe Sec 1 lane errors 0
    ecap 0010[180] = SR-IOV 1 IOV disabled, Memory Space disabled, ARI disabled
                     0 VFs configured out of 0 supported
                     First VF RID Offset 0x0001, VF RID Stride 0x0001
                     VF Device ID 0x1903
                     Page Sizes: 4096 (enabled), 8192, 65536, 262144, 1048576, 4194304
    ecap 0017[1c0] = TPH Requester 1
```

Notable bits:
- **FLR is supported** (cap 10) — `pcie_flr()` should work; we use it before
  BAR alloc.
- **MSI-X table lives in BAR4** (`map 0x20[0x0]`), so BAR4 is *not* the
  function-control window. The 8 MB BAR (currently mapped via `PCIR_BAR(2)`)
  is the candidate for the MMIO/MCDI doorbell window — but see "Open
  question" below.
- **Link is x8 / 8.0 GT/s** — full PCIe 3.0 negotiated, no degraded link.
- **SR-IOV is present but disabled** — we attach as a regular PF.

### Bringup notes (2026-05-07)

**RESOLVED — FLR breaks this card.** Removing the `pcie_flr()` call at
the top of `sfc7120_fbsd_attach` makes MMIO live immediately, MCDI
responds, `MC_CMD_GET_VERSION` returns `1001.7.2.6`, `DRV_ATTACH`
succeeds with `func_flags=0x6` (LINKCTRL+TRUSTED), and
`GET_MAC_ADDRESSES` reads `00:0f:53:28:54:91`. Before the fix,
`HW_REV_ID` and the entire 8 MB BAR returned all-zeros indefinitely — a
host-driven FLR puts the SFN7322F-R2 into a state where the BIU clock
domain stays gated, even after multi-second waits. **Stock sfxge does
not FLR at attach; do not add it back without a different reset path
(MC_CMD_REBOOT via MCDI, or per-VI soft reset).** The FLR call is
currently `#if 0`'d out at the top of `sfc7120_fbsd_attach`.

A second consequence: without FLR, firmware state survives module
unload. If a previous load `DRV_ATTACH`'d and was unloaded without a
matching `FREE_VIS`, the firmware still believes the previous driver
owns those VIs, and `MC_CMD_ALLOC_VIS` returns `MC_CMD_ERR=95`
(`EOPNOTSUPP` / errno 45). Fix: unconditionally issue
`MC_CMD_FREE_VIS` before `MC_CMD_ALLOC_VIS` (sfxge does this — see
`ef10_nic.c:2218-2220`). Already wired in `sfc7120_mcdi_alloc_vis`.

Other small notes captured during bringup:
- **`MC_DB_LWRD` (0x200) is write-only.** Reads return zero regardless of
  what was written. The "LWRD probe" diagnostic in `sfc7120_mcdi_init`
  reports "MMIO path dead" when in fact the register is write-only —
  trust `HW_REV_ID` (0x000) for liveness, not LWRD.
- **PF1 MAC differs from the serial number's last byte by 1.** The
  pciconf serial reports the PF0 base MAC; PF1's MAC is base+1.

### Earlier (now-resolved) symptom log

Prior to disabling FLR, attach symptoms were:

- BAR layout from `pciconf -lbv` (PF0 shown; PF1 is identical type/size,
  shifted in address):

  | RID | Offset | Type | Size | PF0 base | PF1 base |
  |---|---|---|---|---|---|
  | `PCIR_BAR(0)` | `0x10` | I/O | 256 B | `0x3100` | `0x3000` |
  | `PCIR_BAR(2)` | `0x18` | mem64 | 8 MB | `0x60000000` | `0x60800000` |
  | `PCIR_BAR(4)` | `0x20` | mem64 | 16 KB | `0x61000000` | `0x61004000` |

  `PCIR_BAR(2)` (8 MB) is the function MMIO window — matches sfxge's
  `EFX_MEM_BAR_HUNTINGTON_PF = 2`. `PCIR_BAR(4)` (16 KB) holds the MSI-X
  table+PBA per the `cap 11` line of `pciconf`. So the BAR choice is
  correct; "wrong BAR" is ruled out.
- `PCIR_COMMAND` shows `MEMEN=1, BUSMASTER=1` both before and after the
  failed command (so the device hasn't disabled itself mid-command).
- Doorbell write order matches sfxge `ef10_mcdi_send_request` (high half
  to LWRD at `0x200`, then low half to HWRD at `0x204` as the trigger).
- The "doorbell-clear" init kick (HWRD ← 1) from `ef10_mcdi_init` is
  performed.

Remaining candidates:

1. **Device not finished coming out of reset.** `pcie_flr()` returns at
   1 s but Huntington's MC firmware can take several seconds to re-boot.
   Until the BIU clock domain is fully up, MMIO reads return `0`. Fix:
   after FLR, poll `HW_REV_ID` for up to ~5 s waiting for `0xeb14face`
   before issuing any MCDI command.
2. **CHERI `bus_space` capability bounds.** A scan over the first 4 KB
   would distinguish this from (1): all-zeros = hardware-not-up;
   `0xffffffff` past some offset = capability bounds issue.
3. **Per-function init order.** Some EF10 firmware variants gate PF1
   MMIO behind PF0 MCDI bringup. Worth ruling out by attaching to PF0
   instead, or leaving stock `sfxge` on PF0 while we drive PF1.

Next debug step is the BAR magic-value scan that's already wired into
`sfc7120_mcdi_init` (uncommitted local change as of 2026-05-07) plus a
post-FLR wait loop on `HW_REV_ID`.

---

## Reference Reading (Required)

Before modifying anything in this directory, read:

1. `~/CheriBsdE1000/CLAUDE.md` — first CAPIO driver, simpler reference
2. `~/cheri/cheribsd/sys/modules/mlx5pol/CLAUDE.md` — complex CAPIO driver
   with firmware command channel; the closest existing pattern for EF10's
   MCDI

The CAPIO conventions (mandatory softc layout, attach/detach ordering,
slice manifest semantics, IOCTL token validation) are documented there
and apply identically here.

---

## Role in the Larger System

```
Userspace (E1000Lwip/netif/sfc7120_driver.c — TO BE WRITTEN)
  │  open("/dev/sfc7120pol")
  │  ioctl(CAPIO_ATTACH)          ──────► Kernel (this module)
  │                                         seals userspace cap
  │◄── returns sealed CHERI capability ────
  │
  │  mmap(SFC7120_TX_BUFFER /     ──────► capio_mmap_single_extra()
  │       SFC7120_RX_BUFFER /                validates token, builds
  │       SFC7120_MMIO_REGION)                bounded VM object
  │◄── userspace VA backed by NIC DMA/MMIO
  │
  │  TX/RX: write descriptor ring + ring doorbell (no syscalls)
```

After `CAPIO_ATTACH` + mmap, the userspace driver pushes TX descriptors
and reads RX events directly through the CHERI capability. The kernel is
not in the data path; CHERI hardware enforces the bounds of every
capability.

---

## File Map

| File | Purpose |
|---|---|
| `sfc7120.c` | PCI driver: probe/attach/detach, vtable, IOCTL dispatch |
| `sfc7120.h` | Softc, region enum (`SFC7120_TX_BUFFER` / `_RX_BUFFER` / `_MMIO_REGION`), IOCTL request structs |
| `sfc7120_mmio.h` | BAR0 register offsets (placeholders — confirm against EF10 docs), `SFC7120_READ_REG` / `SFC7120_WRITE_REG` |
| `sfc7120_mmio.c` | `sfc7120_dump_regs()` — diagnostic register dump |
| `sfc7120_tables.c` | Slice manifest (`sfc7120_reg_slices[]`) for the MMIO region |
| `capio.c` | Local copy of CAPIO framework (mirrors `~/CheriBsdE1000/capio.c`) |
| `capio.h` | CAPIO types: `capio_softc_t`, `slice_def_t`, etc. |
| `Makefile` | FreeBSD kmod Makefile; cross-compile via `CROSS_COMPILE=1` |
| `build.sh` | Detects Linux/FreeBSD, checks deps, invokes bmake or make |
| `install.sh` | SCPs the built `.ko` to the Morello board |
| `.gitignore`, `README.md` | Standard housekeeping |

`capio.c`/`capio.h` are local copies (not VPATH-shared) because this is an
out-of-tree workspace. If you change them, sync to the in-tree version
once it exists, and to `~/CheriBsdE1000/` if the change is generic.

---

## The Mandatory CAPIO Softc Layout

Same constraint as the other CAPIO drivers — `capio.c` casts `void *sc`
straight to `capio_softc_header_t *`:

```c
typedef struct {
    device_t      dev;        /* offset 0 — MUST be first */
    capio_softc_t capio_sc;   /* immediately after — MUST be second */
} capio_softc_header_t;
```

`sfc7120_softc_t` (`sfc7120.h`) opens with exactly:

```c
typedef struct sfc7120_softc {
    device_t            dev;
    capio_softc_t       capio_sc;
    shared_mem_region_t smem[SFC7120_REGION_COUNT];
    /* ... hardware fields ... */
} sfc7120_softc_t;
```

Anything before `dev` or between `dev` and `capio_sc` will panic.

---

## Memory Regions (Initial Set)

Three regions, mirroring the e1000 stub. Multi-queue can be added later
following the mlx5pol per-queue pattern.

| Index | Enum | `is_physical` | Size | Sliced? | Description |
|---|---|---|---|---|---|
| 0 | `SFC7120_TX_BUFFER` | false | 64 × 2048 = 128 KB | No | DMA TX packet buffer (kernel VA) |
| 1 | `SFC7120_RX_BUFFER` | false | 64 × 2048 = 128 KB | No | DMA RX packet buffer (kernel VA) |
| 2 | `SFC7120_MMIO_REGION` | true | BAR0 size | Yes | NIC register space (BAR0 physical addr) |

The MMIO slice manifest (`sfc7120_reg_slices[]`) starts with the registers
a CAPIO userspace driver minimally needs:

| Register | Direction | Notes |
|---|---|---|
| `MC_DOORBELL` (`0x0e80`) | RW | Kicks MCDI requests |
| `MC_EVENT`   (`0x0e84`) | RO | MC status |
| `EVQ_RPTR_DBL` (`0x0500`) | RW | Ack events |
| `RX_DESC_DBL`  (`0x0510`) | RW | Push RX descriptor producer pointer |
| `TX_DESC_DBL`  (`0x0518`) | RW | Push TX descriptor producer pointer |
| `HW_REV_ID`    (`0x0010`) | RO | Sanity check at attach |
| `MC_STATUS`    (`0x0c7c`) | RO | Health |

**These offsets are placeholders.** Cross-check against the EF10 register
documentation for your specific SFN7xxx board (and against the FreeBSD
`sfxge` driver in `~/cheri/cheribsd/sys/dev/sfxge/`) before treating them
as authoritative.

---

## What Is and Isn't Implemented

| Component | State | Notes |
|---|---|---|
| PCI probe | Partial | Skeleton table with vendor 0x1924 + device IDs 0x0903 (PF) / 0x1903 (VF). Confirm with `pciconf -lv`. |
| Attach ordering | Done | BAR alloc → hw_init → DMA alloc → make_dev_capio → smem populate → init_capio_sc → vm_object_handle alloc |
| Detach ordering | Done | dying flag → free handles → modmap_unregister → destroy_dev → free DMA → hw_teardown → release BAR → capio_destroy → destroy locks |
| `capio_ops_t` vtable | Done | `ioctl`, `get_buffer_size`, `is_dying` all wired |
| Slice manifest | Skeleton | 7 entries; extend for multi-queue |
| Cdev (`/dev/sfc7120pol`) | Done | open/close/poll/ioctl wired |
| MCDI / firmware bringup | **TODO** | `sfc7120_hw_init` / `sfc7120_hw_teardown` are stubs |
| DMA allocation | **TODO** | `sfc7120_alloc_dma_resources` is a stub; mirror e1000_alloc_*_dma |
| EVQ / TXQ / RXQ init | **TODO** | Triggered through MCDI commands |
| Interrupt handler | **TODO** | Skeleton exists; needs to walk EVQ |
| TX/RX IOCTLs (`SFC7120_TX`, `SFC7120_RX`) | **TODO** | Return ENOSYS today |
| `SFC7120_GET_MAC` | Done shape | Returns `sc->mac_addr`; that field is unpopulated until MCDI lands |
| In-tree copy | Not yet | Add to `~/cheri/cheribsd/sys/modules/sfc7120pol/` once stable |
| Userspace counterpart | Not yet | Add to `~/E1000Lwip/netif/sfc7120_driver.c` |

---

## Recommended Implementation Order

1. **`sfc7120_alloc_dma_resources` / `sfc7120_free_dma_resources`** — start
   with a `mlx5_dmabuf_t`-style helper (single tag/map/load wrapper). EF10
   needs at minimum: EVQ ring, TX desc ring, RX desc ring, TX packet
   buffer, RX packet buffer, MCDI message buffer. That's ~6 buffers — too
   many for the e1000 per-buffer pattern.

2. **MCDI plumbing** — synchronous request/response over a DMA-resident
   message buffer + the MC doorbell. EF10's MCDI is structurally similar
   to mlx5pol's command queue: write request to mailbox, kick doorbell,
   poll for completion, read response. Use `~/cheri/cheribsd/sys/dev/sfxge/`
   as the reference.

3. **MCDI bringup commands** — at minimum: `MC_CMD_GET_VERSION`,
   `MC_CMD_GET_BOARD_CFG` (yields the MAC), `MC_CMD_INIT_EVQ`,
   `MC_CMD_INIT_RXQ`, `MC_CMD_INIT_TXQ`, `MC_CMD_MAC_RECONFIGURE`.
   Populate `sc->mac_addr` from `GET_BOARD_CFG`.

4. **Interrupt handler** — MSI-X allocation, EVQ event walking.

5. **TX/RX IOCTLs** — kernel-mediated fallback paths for testing before
   the userspace driver lands.

6. **Userspace driver** — `~/E1000Lwip/netif/sfc7120_driver.c`, registers
   as a lwIP `netif`. Mirror `~/E1000Lwip/netif/mlx5_driver.c`.

7. **In-tree copy** — once stable, copy under
   `~/cheri/cheribsd/sys/modules/sfc7120pol/` so it builds into the
   CheriBSD image. Use the mlx5pol-style 7-line Makefile with VPATH
   pointing at `../em_pol_test` for `capio.c`.

---

## Build

### Cross-compile (Linux → Morello):
```bash
cd ~/CheriBsdSolarflare7120
./build.sh build
mv sfc7120pol.ko ~/mod_out/    # match the convention used by other modules
```

### Native (on CheriBSD board):
```bash
./build.sh build            # CROSS_COMPILE=0
sudo ./build.sh install     # kldunload old, kldload new
```

### Compilation database (clangd):
```bash
./build.sh bear
```

### Dependencies:

| Module | Why |
|---|---|
| `modmap.ko` | `init_capio_sc` registers modmap callbacks; modmap must load first |
| FreeBSD `pci` KLD | Standard PCI bus methods |
| Stock `sfxge` | If loaded, will conflict for the same device. `kldunload sfxge` before `kldload sfc7120pol`. |

---

## Devices Supported (placeholder)

| Vendor | Device ID | Description |
|---|---|---|
| 0x1924 | 0x0903 | Solarflare SFC9120 10G Ethernet Controller (EF10) PF |
| 0x1924 | 0x1903 | Solarflare SFC9120 10G Ethernet Controller (EF10) VF |

Confirm against the actual board with `pciconf -lv` and adjust
`sfc7120_fbsd_devs[]` in `sfc7120.c`.

---

## Known Gaps

- All MCDI / hardware bringup is stubbed.
- Slice manifest offsets are placeholder values from EF10 documentation
  conventions; cross-check against `~/cheri/cheribsd/sys/dev/sfxge/`.
- No in-tree copy.
- No userspace driver.
- `vm_object_handle_t` allocations are not freed in detach? They are —
  the loop is in `sfc7120_fbsd_detach` before `destroy_dev`. Mention this
  because the e1000 out-of-tree copy is missing that loop.
