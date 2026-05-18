# Handoff — debugging `MC_CMD_VADAPTOR_ALLOC` EINVAL on PF1

## Current status

We are stuck on `MC_CMD_VADAPTOR_ALLOC` (opcode `0x98`) returning
`MC_CMD_ERR=22` (EINVAL) on the SFN7322F-R2, PF1.

Everything else in `sfc7120_hw_init` works:

```
sfc7120pol0: MC GET_PORT_ASSIGNMENT: port=1
sfc7120pol0: MC GET_FUNCTION_INFO: pf=1 vf=0xffff (PF)
sfc7120pol0: MC GET_CAPABILITIES: flags1=0x1be42f68 rxdp_fw=0x0001 txdp_fw=0x0001 hw_caps=0x00000000 lic_caps=0x00000003
sfc7120pol0: MC: allocated 32 VIs starting at base 1024
sfc7120pol0: MC VADAPTOR_FREE (preemptive): rc=0
sfc7120pol0: MCDI VADAPTOR_ALLOC failed: 22
sfc7120pol0: MC state @after VADAPTOR_ALLOC fail: HW_REV_ID=0xeb14face MC_SFT_STATUS=0xb0070005
```

`HW_REV_ID=0xeb14face` after the failure ⇒ MC is alive, not rebooting. So
the firmware is *explicitly rejecting our arguments*, not crashing.

`flags1=0x1be42f68` — bit 11
(`VADAPTOR_PERMIT_SET_MAC_WHEN_FILTERS_INSTALLED`) **is** set (mask 0x800
hits), so the firmware advertises that flag. Sending `flags=0x2` should
be legal, but it gets EINVAL anyway.

`VADAPTOR_FREE` returning rc=0 confirms there was a stale vAdaptor from a
prior load — but freeing it didn't unblock `ALLOC`.

## What's already been ruled out

| Hypothesis | Status |
|---|---|
| MCDI v1/v2 framing bug | **Ruled out.** `ALLOC_VIS (0x8b)`, `GET_CAPABILITIES (0xbe)`, etc., all work — the v2 escape path is correct. |
| Stale vAdaptor from prior load (need preemptive FREE) | **Ruled out as the *sole* cause.** Preemptive `VADAPTOR_FREE` now runs and returns rc=0, but `ALLOC` still EINVALs. |
| `PERMIT_SET_MAC_WHEN_FILTERS_INSTALLED` not advertised | **Ruled out.** `flags1=0x1be42f68 & (1<<11)` is non-zero. |
| MCDI doorbell / mailbox endianness wrong | **Ruled out.** Other commands at the same code path succeed. |
| `EVB_PORT_ID_ASSIGNED` (0x01000000) is wrong sentinel | **Ruled out.** `VADAPTOR_FREE` used the same port-id and succeeded. |

## What's in the tree right now

`sfc7120_mcdi.c::sfc7120_mcdi_vadaptor_alloc` (around line 1395+) was
restructured to call a helper `sfc7120_mcdi_vadaptor_alloc_try()` that
**dumps the full 30-byte payload** before sending, then tries four
variants in sequence:

- **A:** cap-gated flags (i.e. flags=0x2 here since bit 11 is advertised),
  AUTO_MAC (all-zero MACADDR)
- **B:** flags=0, AUTO_MAC
- **C:** cap-gated flags, explicit MAC from `sc->mac_addr` (which is
  populated by an earlier `GET_MAC_ADDRESSES`, value `00:0f:53:28:54:91`
  ish)
- **D:** flags=0, explicit MAC

The probe code is **built and ready** (`sfc7120pol.ko` is up to date as
of the last build) but **has not yet been run on the Morello board**.

## Immediate next action

1. `scp sfc7120pol.ko cheri:/root/KernelMods/`
2. On the board: `kldunload sfc7120pol; kldload /root/KernelMods/sfc7120pol.ko; dmesg | tail -40`
3. Look for four `VADAPTOR_ALLOC[A:..D:]` lines.

Expected outcome categories:

- One variant returns `rc=0` ⇒ we've found the arg the MC wants. Lock it
  in, drop the probe, document in CLAUDE.md as a quirk of the LOW_LATENCY
  DPCPU 1001.7.2.6 firmware.
- All four EINVAL but with **identical** failures ⇒ argument format isn't
  the issue. Likely candidates remaining:
  - PF1 lacks privilege for VADAPTOR_ALLOC on this card. func_flags=0x6
    (LINKCTRL+TRUSTED, no PRIMARY). Try with stock `sfxge` attached to
    PF0 first, then load our driver on PF1 (sfxge may set up the EVB
    state that PF1 needs).
  - The MCDI v2 ext-header `ACTUAL_LEN` field is being misread by this
    firmware variant. Try padding `IN_LEN` to 32 (next dword multiple)
    and see if behavior changes — though sfxge sends raw 30 and it works
    for them, so this would be surprising.
  - Need an intermediate `EVB_PORT_ASSIGN (0x9a)` before `VADAPTOR_ALLOC`.
    Sfxge does **not** do this, but worth a try if everything else fails.
- Mixed results ⇒ read the bytes-on-the-wire dumps carefully against
  `ref_efx_regs_mcdi.h` lines 11707-11744.

## Key files / line numbers

| File | What's there |
|---|---|
| `sfc7120_mcdi.c:1395+` | `sfc7120_mcdi_vadaptor_alloc` — fallback probe |
| `sfc7120_mcdi.c:1359+` | `sfc7120_mcdi_vadaptor_alloc_try` — single-attempt helper with byte dump |
| `sfc7120_mcdi.c:203-210` | MC_CMD_VADAPTOR_ALLOC opcode + field offsets (including `_MACADDR_OFST = 24` that I added) |
| `sfc7120_mcdi.c:828-901` | `sfc7120_mcdi_dump_func_info` — now caches `flags1` into `sc->mcdi_cap_flags1` |
| `sfc7120.c:365-369` | `hw_init` now calls `dump_func_info` before `alloc_vis` (was previously never called despite CLAUDE.md saying it was) |
| `sfc7120.h:222-228` | New softc fields `mcdi_cap_flags1` / `mcdi_caps_valid` |
| `ref_efx_regs_mcdi.h:11707-11744` | VADAPTOR_ALLOC wire format reference |
| `~/cheri/cheribsd/sys/dev/sfxge/common/ef10_nic.c:255-291` | sfxge's `efx_mcdi_vadaptor_alloc` for comparison |
| `~/cheri/cheribsd/sys/dev/sfxge/common/ef10_nic.c:2299-2322` | sfxge's call site (with VF retry loop on NO_EVB_PORT) |

## Build / deploy loop

```bash
# On Linux:
cd ~/CheriBsdSolarflare7120
./build.sh build /home/khacker/cheri/cheribsd
scp sfc7120pol.ko cheri:/root/KernelMods/

# On Morello (`ssh cheri`):
kldunload sfc7120pol 2>/dev/null
kldload /root/KernelMods/sfc7120pol.ko
dmesg | tail -40
```

`modmap.ko` must already be loaded. Stock `sfxge` must be **un**loaded
unless you specifically want to test the "let PF0 do EVB setup, attach
to PF1" theory above.

## Things to NOT do

- **Don't add a `pcie_flr()` call** — bricks this card's BIU clock
  domain. See CLAUDE.md "Bringup notes (2026-05-07)". `sfc7120_pcie_flr`
  helper exists but is `#if 0`'d at the call site for a reason.
- **Don't truncate MCDI opcodes to 7 bits.** All our commands use v2
  framing. See CLAUDE.md "MCDI v1/v2 framing trap".
- **Don't trust `MC_DB_LWRD` (0x200) for liveness probes** — it's
  write-only and reads as zero. Use `HW_REV_ID` (0x0).

## Open questions for the next session

1. Does the variant probe actually identify the accepted args, or do all
   four still EINVAL?
2. If all EINVAL: is PF1's privilege the blocker? Test by loading stock
   sfxge first, then our driver on PF1 (would need a way to coexist or
   target only PF1 at the PCI matcher level).
3. Is `MC_SFT_STATUS=0xb0070005` (constant after every failure) telling
   us anything? `0xb007` looks like a "BOOT_OK" status code from
   firmware, but the low byte (0x05) varies by command — worth decoding
   if we get desperate.
