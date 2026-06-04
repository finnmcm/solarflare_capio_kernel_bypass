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
