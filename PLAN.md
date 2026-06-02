# Plan — Port Finn's ISR onto arthur as the Control-EVQ Handler

**Base branch:** `arthur` (the verified-working TX/RX baseline; `main` does not build).
**Goal:** Add a kernel interrupt handler that owns EVQ 0 and processes *control* events
(link up/down, MCDI async, errors) only — while the TX/RX data path moves to EVQ 1.
Keep the `sfctest` TX/RX smoke test passing at every checkpoint.

---

## Starting facts (why the plan is shaped this way)

**arthur today**
- EVQ 0 is created *interrupting* (`flags = 0x39`, `sfc7120_mcdi.c:1128`), `IRQ_NUM=0`,
  but **no MSI-X vector is wired**. `sfc7120_interrupt_handler` is an empty stub
  (`sfc7120.c:1097`).
- TX/RX work because the ioctls **poll EVQ 0 inline** (`sfc7120.c:941`, `:1002`).
- softc already has `irq_resource / irq_res_id / irq_handle`, `rx_task / rx_taskqueue`,
  `dying`, `evq_initialized`. It **lacks** `msix_bar_resource`, `msix_nvec`,
  `intr_initialized`.

**finn's ISR (the thing being ported)**
- Walks the EVQ ring, decodes MCDI/LINKCHANGE/DRIVER **and** RX_EV/TX_EV, advances the
  read pointer, writes the RPTR doorbell, kicks an RX taskqueue.
- `intr_setup` allocates BAR4 + 1 MSI-X vector + `bus_setup_intr`.

## The one hard constraint (dictates the order)

**One owner per EVQ.** arthur's inline poll currently owns EVQ 0. Attaching the ISR to
EVQ 0 as well would race over the read pointer (the exact race finn patched in
`16de4b9`). And **LINKCHANGE is firmware-pinned to EVQ 0** (verified in sfxge:
`efx_mcdi.c:1786`, `ef10_ev.c:55`). So the control handler must live on EVQ 0, which
means **the data path has to move off EVQ 0 first.** The data-EVQ split is a
prerequisite, not optional.

## Concepts

- **ISR** — kernel function the CPU runs on the NIC's MSI-X interrupt; drains the EVQ
  briefly and returns.
- **MSI-X** — PCIe interrupt mechanism; table lives in BAR4 on this card. Allocate the
  BAR, request 1 vector, bind the ISR.
- **Control EVQ (0)** — interrupting, kernel ISR, link/MCDI events.
  **Data EVQ (1)** — non-interrupting, TX/RX completions, drained by the inline poll
  (later by userspace).
- **Taskqueue** — finn used it to wake RX consumers from the ISR. **Deleted here** —
  RX no longer goes through the ISR.

---

## Phases (each ends with the TX/RX test passing)

### Phase 1 — prerequisite: move the data path to EVQ 1
(Technically "data EVQ" work, but it must land before the ISR can own EVQ 0.)
- Add a second `INIT_EVQ` for instance 1, **non-interrupting** (drop the `1<<0` bit;
  keep cut-thru/merge).
- Retarget `INIT_RXQ` / `INIT_TXQ` `TARGET_EVQ` from 0 → 1.
- Point the inline TX/RX poll loops at EVQ 1's ring + EVQ 1's RPTR doorbell.
- EVQ 0 stays created & interrupting, now carrying only control events.
- ✅ **Verify:** `sfctest` TX/RX still passes (now over EVQ 1).

### Phase 2 — port the MSI-X plumbing with a skeleton handler
- Add softc fields: `msix_bar_resource`, `msix_bar_rid`, `msix_nvec`, `intr_initialized`.
- Port `sfc7120_intr_setup` / `sfc7120_intr_teardown` from finn — **minus** the
  taskqueue. Wire `intr_setup` into attach **before** `INIT_EVQ`; `intr_teardown` into
  detach (before freeing EVQ DMA).
- Replace the stub handler with a **minimal** version: guard on `dying` /
  `evq_initialized`, walk EVQ 0, presence-check (0xff / 0x0), advance read ptr, write
  RPTR, and just `device_printf` the event code. No decode yet.
- ✅ **Verify:** module loads, `MSI-X: allocated 1 vector` prints, no interrupt storm,
  TX/RX still green.

### Phase 3 — flesh out the control decode (the actual port)
- Port finn's `MCDI_EV` / `LINKCHANGE` decode and the `DRIVER_EV` case; update
  `sc->link_up / link_speed_mbps / full_duplex / link_fcntl` live.
- **Delete** finn's `RX_EV` / `TX_EV` cases and the `wake_rx_task` /
  `taskqueue_enqueue` tail — those belong to the data EVQ.
- ✅ **Verify:** unplug/replug the SFP+ → `EVQ LINKCHANGE: link=DOWN/UP …` in dmesg,
  `sc->link_up` tracks it, TX/RX keeps working.

## The port is mostly deletion
Finn's ISR is large because it does control + data + taskqueue. As a control-only
handler it shrinks to: ring-walk + presence check + LINKCHANGE/MCDI/DRIVER cases +
RPTR doorbell. Port the small half; drop the rest.

## Gotchas
1. EVQ 0 (interrupting) before EVQ 1 (non-interrupting) at init — the non-interrupting
   queue must reference EVQ 0's IRQ (sfxge constraint, verified). Teardown reverses.
2. `intr_setup` before `INIT_EVQ` in attach (finn `sfc7120.c:643`) — a spurious early
   interrupt is a safe no-op while `evq_initialized` is false.
3. The control handler must still advance the read pointer + write RPTR on EVQ 0, or
   control events back up.
4. BAR4 is not allocated on arthur today — Phase 2 adds that.

## Implementation style
Skeleton-first, piece by piece (struct fields → setup/teardown → handler skeleton →
decode), explaining each piece before writing the `.c`.
