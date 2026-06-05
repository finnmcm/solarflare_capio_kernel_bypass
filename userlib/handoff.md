# Session Handoff — MMIO-read-through-slice FIXED + new cleanup-leak bug (2026-06-04)

> Branch: **`debug-mmio-read`** (cut from `main` this session). All changes below
> are **uncommitted** on that branch unless noted. `main` and other branches do
> NOT have the capio memattr fix.

## TL;DR

1. **RESOLVED — the HW_REV_ID-via-slice "reads 0" bug.** Root cause was a memory
   attribute: `capio.c` mapped `is_physical` regions `VM_MEMATTR_UNCACHEABLE`,
   which on ARM64/Morello is *Normal* non-cacheable, not Device memory. The
   SFC9120 BIU RAZ's a Normal read of its register window. Fix = map them
   `VM_MEMATTR_DEVICE` (Device-nGnRE). **Confirmed on hardware:** `HW_REV_ID via
   slice cap = 0xeb14face (ok)` on both PFs, and a **full userspace TX→RX
   round-trip works (4/4 frames PF0→PF1, correct payload markers).**
2. **NEW BUG (open) — CAPIO region `mapped` flag leaks; re-running on a PF needs
   a module reload.** The cdev_pager dtor that clears `smem.mapped` never fires,
   so the 2nd map of any region fails `MODMAPIOC_MAP` with EINVAL. A pragmatic
   fix is identified (have `revoke_cap_token` clear the flag itself) but NOT yet
   applied; the deeper root cause (an object reference that never drops to 0) is
   still being dug into at the end of this session.

## (1) The HW_REV_ID memattr fix — what changed and why

**Symptom:** reading `HW_REV_ID` (BAR2 offset 0) through the userspace CHERI
slice capability returned `0x00000000` on both PFs, with NO CHERI trap. The
kernel reads the same register `0xeb14face` via `bus_space`.

**How it was cornered (elimination ledger):**
- Kernel read path fine — added a LIVE/DEAD print to `sfc7120_dump_regs`
  (`sfc7120_mmio.c`); prints `HW_REV_ID = eb14face (LIVE)` at attach.
- CHERI cap valid (correct 4-byte bounds, load perm, no trap).
- modmap slice math fine (offset 0).
- **Physical address correct** — added a `rman_get_start` vs
  `pmap_kextract(mem_bsh)` print in `sfc7120_fbsd_attach` (`sfc7120.c`); prints
  **MATCH**, so capio maps the same PA the kernel reads.
- General "Normal-NC reads broken" theory disproven — the e1000 reference driver
  (closed research result) reads live MMIO through the identical Normal-NC capio
  path.

**Conclusion (device-specific):** the SFC9120 register window requires
Device-attribute access; the original `VM_MEMATTR_UNCACHEABLE` was an x86-ism
(on x86 UC == device-ordered; on ARM64 it decodes to weaker Normal-NC). sfxge
corroborates: EF10 maps its VI-register BAR region as UC
(`ef10_nic.c` `ena_uc_mem_map`), reserving write-combining only for the separate
piobuf region.

**Fix (capio.c, both `is_physical` sites — guarded for x86):**
```c
#if defined(VM_MEMATTR_DEVICE)
    mem_attribute = VM_MEMATTR_DEVICE;       /* ARM64: Device-nGnRE */
#else
    mem_attribute = VM_MEMATTR_UNCACHEABLE;  /* x86: UC is already device-ordered */
#endif
```
- `capio_mmap_single_extra` (~line 383, object `obj->memattr` default)
- `capio_pager_fault` (~line 500 — **the operative one**; this is the page the
  CPU's load walks). Its existing "Mapping physical addr" debug print now also
  shows `memattr=%d` (Device=4, old UNCACHEABLE=1) so you can confirm the
  attribute took.

x86 has no `VM_MEMATTR_DEVICE`; the `#ifdef` keeps the shared/in-tree copy
building. Device is the stricter/more-correct MMIO attribute, so e1000/mlx5 are
unaffected (and it's what Phase C's doorbell writes want anyway — nGnRE
ordering, no gathering).

## (2) The open cleanup-leak bug

**Symptom:** first run on a freshly-loaded module works end-to-end. The *second*
run on the same PF (tx or rx, packets or not) fails at init:
```
sfc7120: MODMAPIOC_MAP failed: Invalid argument
sfc7120_init: map TX buffer: Invalid argument
```

**Mechanism:** the CAPIO region `mapped` flag (`shared_mem_region_t.mapped`) is
set in `capio_pager_ctor` and only cleared in `capio_pager_dtor`. `MODMAPIOC_MAP`
refuses an already-mapped region: `capio.c` `if(smem.mapped) return EINVAL`. So
once a run leaves a region flagged mapped, every later map of it EINVALs until a
module reload (the flag lives in the softc, reset on attach).

**What's been ruled in/out (dmesg-confirmed):**
- A `CAPIO_GOODBYE` call was added to `sfc7120_destroy`
  (`userlib/sfc7120_user.c`, top of the function, before closing fd). It DOES
  run — proof: the 2nd run gets *past* `CAPIO_ATTACH` (which refuses to attach
  while a prior token is set), so `revoke_cap_token` cleared the token.
- `revoke_cap_token` → `delete_mapping_from_user` → `vm_map_remove` runs and
  **succeeds** (dmesg shows `Deleting tx mapping for user` with NO
  `vm_map_remove failed` / `vm_map_lookup failed` after it), and the region
  offset is zeroed.
- **BUT `mapped` stays `true`** — i.e. `capio_pager_dtor` never fired, even
  though the map entry was removed. So the cdev_pager VM object's refcount is
  NOT reaching 0 on unmap → it's never deallocated → dtor never runs. This same
  root cause is why plain process-exit also fails to clear the flag.
- After the first revoke zeroes the offsets, later runs show `No offset for
  user` for every region (offset already 0, and the failed 2nd map never records
  a new one) — consistent with the above, not a separate bug.

Note: the userspace test **never calls `munmap`** (`sfc7120_destroy` only frees
the slice array + closes fds), so teardown relies entirely on `revoke` /
process-exit — both of which depend on the dtor firing, which it doesn't.

Also note a latent issue found while reading `modmap.c`: for a **sliced** region,
`MODMAPIOC_MAP` reassigns `kern_req_user->addr = slice_definitions[0].addr`
*before* calling `notify_dev_vaddr` (modmap.c ~line 412), so the offset recorded
for the MMIO region is `slice[0]`'s address (base+0x200), not the region base —
so `revoke`'s `vm_map_remove` for the MMIO region uses a slightly-wrong
start/len. Unsliced regions (the packet buffers / rings) are unaffected; the
TX_BUFFER failure is purely the dtor/refcount issue above.

**Proposed pragmatic fix (NOT yet applied):** make `revoke_cap_token`
authoritatively clear `shared_mem_regs[i].mapped = false` (+ offset/len) for each
region in its loop, and `sc->mapped = false` after, instead of depending on the
dtor. CAPIO is single-owner (one sealed token at a time), so on revoke it's
correct to mark all regions unmapped. Unblocks repeated cooperative runs with no
reload. **Tradeoffs:** (a) the object still isn't deallocated, so each run leaks
the cdev_pager object + fictitious pages (minor; reclaim with an occasional
reload); (b) a SIGKILL'd test still leaks the flag (no `GOODBYE`) — a device
last-close handler would close that gap but hits the same dtor problem.

**ROOT CAUSE FOUND (deeper dig, 2026-06-04):** the reference drivers
(`~/E1000Lwip/netif/e1000.c`, `mlx5_driver.c`) **explicitly `munmap` every
region in their destroy path — 12 `munmap` calls each, and ZERO
`CAPIO_GOODBYE`.** `sfc7120_destroy` `munmap`s *nothing* (it only frees the
slice array + closes fds). That is the entire difference.

Why `munmap` works but `GOODBYE`/`revoke` doesn't, even though *both* end up in
`vm_map_remove_locked` (kern_munmap calls it directly at `vm_mmap.c:1133`;
`vm_map_remove_locked` does delete the CHERI reservation once the range is fully
unmapped, `vm_map.c:~2367`): it's the **arguments**, not the path.
- `munmap` passes the *exact* mmap capability + length the process holds.
- `revoke_cap_token` passes `smem.offset`/`smem.len` recorded by
  `capio_vaddr_callback`. `vm_map_remove` returns `KERN_SUCCESS` even when it
  removes nothing, so if that recorded range doesn't exactly cover the real
  mapping, the object is never deallocated, ref never hits 0, dtor never fires,
  `mapped` leaks — and the dmesg looks "successful" (no error line). This matches
  exactly what we saw: `Deleting tx mapping for user` + no error, yet `mapped`
  stuck.
- For the **sliced MMIO region** the recorded offset is *definitely* wrong:
  modmap reassigns `kern_req_user->addr` to `slice_definitions[0].addr` before
  `notify_dev_vaddr`, and sfc7120's `slice[0]` is `MC_DOORBELL` at **0x200**, so
  the recorded offset is `base+0x200`, not the region base. (e1000's slice[0] is
  at offset 0, i.e. == base, which is why e1000 can even `munmap(main_mmio_addr)`
  using slice[0]'s address.)

**RECOMMENDED FIX (match the working reference drivers, userspace-only, no leak):**
make `sfc7120_destroy` `munmap` each region, like e1000 does.
- The 5 unsliced regions (TX/RX packet buffers, TX/RX desc rings, EVQ ring) are
  trivial: `munmap(sfc->tx_buffer, SFC7120_TX_BUFFER_SIZE*SFC7120_NUM_TX_DESC)`,
  etc. — sizes are compile-time constants. This alone fixes the TX_BUFFER
  blocker and the rings.
- The **sliced MMIO region is the catch**: `map_region` returns `slice[0]`
  (base+0x200), so the region's base cap is not retained and can't be
  `munmap`'d. Fix options: (a) also map MMIO *unsliced* and keep that base cap to
  munmap (e1000 does exactly this — `main_mmio_addr_unsliced`); or (b) have
  `map_region` capture/return the base cap before modmap reassigns it; or (c)
  fix modmap to not clobber `kern_req_user->addr` for sliced maps.

**ALTERNATIVE (kernel-side, simpler, uniform, but leaks):** have
`revoke_cap_token` set `shared_mem_regs[i].mapped = false` (+offset/len) directly
instead of depending on the dtor. Handles all regions including MMIO in one
place; downside is the cdev_pager objects still aren't deallocated (the objects
are *already* leaking today since the dtor isn't firing — this just lets re-runs
proceed). A `SIGKILL` still leaks the flag (no GOODBYE) either way.

Runtime probe to confirm the "wrong range → no dealloc" theory before fixing:
print `obj->ref_count` in `delete_mapping_from_user` after `vm_map_remove`, and
add a `device_printf` to `capio_pager_dtor` (it currently has none) to see
whether/when it fires for each region type.

## Current branch state (`debug-mmio-read`, uncommitted)

| File | Change | Type |
|---|---|---|
| `capio.c` | `is_physical` → `VM_MEMATTR_DEVICE` (guarded), 2 sites; `memattr=%d` in fault print | kernel, the FIX |
| `sfc7120_mmio.c` | HW_REV_ID `LIVE/DEAD` verdict in `sfc7120_dump_regs` | kernel, diag (committed 415c4a0) |
| `sfc7120.c` | BAR2 `rman_get_start` vs `pmap_kextract(mem_bsh)` MATCH/MISMATCH print | kernel, diag |
| `userlib/sfc7120_user.c` | `CAPIO_GOODBYE` ioctl at top of `sfc7120_destroy` | userspace |

To reproduce the working path: reload `sfc7120pol.ko`, then `./sfctest tx` then
`./sfctest rx` (tx first — RX events persist in the EVQ until consumed; a
consumer started before any producer just times out). Re-running on the same PF
needs a reload until the cleanup-leak bug is fixed.

---

# Session Handoff — Phase C Prerequisite: Userspace Mapping Wiring (2026-06-03)

## What was achieved

Wired the Phase A/B kernel exposures into the userspace library. The kernel
module was **not** functionally modified (header + comment additions only —
the loaded `.ko` did not need a rebuild).

### Code changes

| File | Change |
|---|---|
| `../sfc7120_uapi.h` | Added `sfc7120_mmio_slice_idx_t` enum — slice array indices matching `sfc7120_reg_slices[]` order (`0=MC_DOORBELL, 1=DATA_EVQ_RPTR_DBL, 2=RX_DESC_DBL, 3=TX_DESC_DBL, 4=HW_REV_ID`). Index is the only usable lookup key: the `name` field copied out by modmap is a kernel pointer. |
| `../sfc7120_tables.c` | Comment only: "ORDER IS ABI" warning tying the manifest order to the new enum. |
| `sfc7120_user.h` | `sfc7120_if_t` extended: `tx_desc_ring` / `rx_desc_ring` / `evq_ring`, `mmio_slices` + `mmio_slices_len`, `vi_info` (full `sfc7120_vi_info_req_t`). |
| `sfc7120_user.c` | Old `map_buffer` replaced by a single unified `map_region(sfc, map_type, out_slices, out_slice_len)` — handles sliced and unsliced regions in one helper (per user feedback: do **not** split into map_buffer/map_sliced like mlx5 does). Always queries `MODMAPIOC_GET_SLICES` first so the kernel's `get_buffer_size` is the single source of truth for region lengths; callers no longer compute `NUM × SIZE`. Zero-inits `user_map_req_t` (the old code passed stack garbage in the slice fields through `copyincap`). `sfc7120_init` now maps all six regions (TX/RX packet buffers, TX/RX desc rings, data-EVQ ring, sliced MMIO) and calls `SFC7120_GET_VI_INFO`. `sfc7120_destroy` frees the slice array. |
| `test.c` | New `dump_vi_state()` runs in both tx/rx modes: prints `vi_info`, mapped ring addresses, slice count, and a HW_REV_ID read through its slice capability. |

### Hardware verification (Morello board, 2026-06-03)

All verified by the user on real hardware:

- Both PFs init with all six regions mapped; `slices=5` as expected.
- `GET_VI_INFO` returns sane geometry: `vi_base=1024` (PF1) / `1` (PF0),
  `evq=1 rxq=0 txq=0`, all counts 512, plausible DRAM bus addresses for
  `tx/rx_buffer_paddr`.
- **Ioctl-path regression green**: `sfctest tx` 4/4 ok, `sfctest rx` 4/4 ok
  with correct payload markers. The new mappings do not disturb Phase 1.
- Orchestration note: the kernel RX ioctl wait is a ~100k-iteration busy-spin
  (sub-second). Run `tx` first, then `rx` — RX descriptors are pre-posted and
  events persist in the EVQ until consumed, so this works; a consumer started
  long before the producer times out instead.

## Open bug — HW_REV_ID reads 0 through the userspace slice capability

**Symptom:** `*(volatile uint32_t * __capability)mmio_slices[SFC7120_SLICE_HW_REV_ID].addr`
returns `0x00000000` on **both** PFs. No CHERI trap — the capability is valid,
correctly bounded, and the load completes. The kernel reads the same register
(BAR2 offset `0x0`) non-zero via `SFC7120_READ_REG` / `bus_space`.

**What was ruled out** (dmesg from the test run):
- The modmap/capio fault path fired correctly: fault at offset 0 →
  `Mapping physical addr: 0x60000000` (PF0) / `0x60800000` (PF1) — the right
  BAR2 bases. Fictitious page created, `valid=0xff`.
- Slice construction is correct: `modmap_make_slices` derives
  `mmap VA + manifest offset`, sets 4-byte bounds, masks STORE for RO. The
  read not trapping confirms index/bounds are right.

**Leading hypothesis (UNCONFIRMED — MORE RESEARCH NEEDS TO BE DONE):**
the memory attribute. `capio.c` maps `is_physical` regions with
`VM_MEMATTR_UNCACHEABLE` (dmesg shows `memattr=1`). On arm64 CheriBSD
(`sys/arm64/include/vm.h`):

```
VM_MEMATTR_UNCACHEABLE  = 1   → Normal Non-cacheable
VM_MEMATTR_DEVICE_nGnRE = 4   → Device memory (= VM_MEMATTR_DEVICE)
```

`UNCACHEABLE` is the correct MMIO attribute on x86 but on arm64 it is *Normal*
memory, not Device memory. The kernel's working reads go through `bus_space`,
which maps the BAR Device-nGnRE. Reads of PCIe BAR space through Normal-NC
mappings on ARM are not architecturally guaranteed to behave, which would
explain load-returns-zero with no fault. It would also explain why mlx5 never
hit this: its userspace MMIO use was doorbell *writes* (posted writes mostly
survive Normal-NC), not register reads.

**Candidate fix (if the hypothesis holds):** use `VM_MEMATTR_DEVICE` for
`is_physical` regions in `capio.c` — two sites: object creation (~line 368-383)
and the fault handler (~line 500). This is a kernel module change (rebuild +
reload `sfc7120pol.ko`; the userlib binary is unaffected). If it proves out,
sync to `~/CheriBsdE1000/capio.c` per the local-copy convention (note x86
doesn't define `VM_MEMATTR_DEVICE`, so a shared copy would need an `#ifdef`).

**But before changing anything, validate the diagnosis:**
- Confirm the kernel still reads HW_REV_ID non-zero *today* on this load
  (e.g. `sfc7120_mcdi_log_mc_state`, `sfc7120_dump_regs`, or a one-off attach
  print) — i.e. rule out a stale expectation.
- Check what attribute the resulting PTE actually carries for the userspace
  mapping (the fictitious-page memattr is requested, but verify pmap honors it
  end-to-end on Morello).
- Check whether mlx5's userspace `init_seg` *reads* (`mlx5_read_init_seg_dword`)
  ever returned real data on Morello — if they did, Normal-NC reads work there
  and the hypothesis is wrong or incomplete.
- Consider whether Morello/CHERI capability loads interact differently with
  Normal-NC vs Device mappings.

**Why this matters beyond a cosmetic print:** Phase C's doorbell writes want
Device-nGnRE ordering semantics anyway (Normal-NC writes can be gathered or
merged — bad for an EVQ RPTR doorbell), and Normal mappings permit
*speculative* reads into register space with read side effects. The attribute
question must be settled before the direct data path is trusted.

## What still needs to be worked on

1. **Diagnose + fix the MMIO-read-through-slice-cap bug above.** Gate for
   Phase C's verify step ("read HW_REV_ID through its slice cap").
2. **Phase C — direct EVQ polling** (`sfc7120_poll`): poll the mapped
   `evq_ring` (empty = EV_CODE nibble `0xF`, ring init'd all-`0xFF`), decode
   RX_EV/TX_EV, advance read pointer, `dsb sy`, ack via the
   `SFC7120_SLICE_DATA_EVQ_RPTR_DBL` cap (offset `0x2400`). Everything it
   needs is now in `sfc7120_if_t`.
3. **Phases D–G** per `CLAUDE.md` roadmap: direct RX post, direct TX,
   cutover, latency tuning + CHERI security validation.
4. **Doc updates** (deferred per convention until this session's hardware
   results were in — now done, so next session can apply them): mark the
   mapping/`GET_VI_INFO` wiring as hardware-verified, and fix the stale
   `userlib/CLAUDE.md` claim that the MMIO region was already mapped in
   Phase 1 (it wasn't until this session).
