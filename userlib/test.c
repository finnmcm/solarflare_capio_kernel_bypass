/*
 * test.c — dual-port sfc7120 userspace TX/RX smoke test (phase 1, ioctl path).
 *
 * PF0 and PF1 are cabled port-to-port via SFP+ DAC, so a frame TX'd on port 0
 * ingresses port 1. This test runs as two cooperating processes:
 *
 *   ./sfctest rx     consumer — opens /dev/sfc7120pol1 (PF1), blocks on RX
 *   ./sfctest tx     producer — opens /dev/sfc7120pol0 (PF0), sends frames
 *
 * Start the consumer first, then the producer. The producer addresses frames
 * to PF1's MAC (its own PF0 base MAC with the low octet +1, per the hardware
 * docs) so the kernel exact-DST_MAC RX filter on PF1 matches → RXQ 0.
 *
 * Not part of the kernel-bypass build — just a sanity check of the userlib API.
 */
#include "sfc7120_user.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* CHERI capability introspection via compiler builtins — avoids pulling in
 * <cheri/cheric.h>, which transitively includes machine/vmparam.h and errors
 * on the PAGE_SIZE define in sfc7120_user.h (include-order clash). */
#define cheri_getbase(c)   __builtin_cheri_base_get((const void * __capability)(c))
#define cheri_getlen(c)    __builtin_cheri_length_get((const void * __capability)(c))
#define cheri_getoffset(c) __builtin_cheri_offset_get((const void * __capability)(c))
#define cheri_getperm(c)   __builtin_cheri_perms_get((const void * __capability)(c))
#define cheri_gettag(c)    __builtin_cheri_tag_get((const void * __capability)(c))

#ifndef SIGPROT
#define SIGPROT 34
#endif

#define DEV_PF0       "/dev/sfc7120pol0"
#define DEV_PF1       "/dev/sfc7120pol1"

#define TEST_PACKETS  4
#define ETHERTYPE     0x88B5   /* IEEE 802 local experimental */
#define FRAME_LEN     64       /* min Ethernet frame (sans FCS) */

static void
build_frame(uint8_t *frame, const uint8_t dst[6], const uint8_t src[6], int seq)
{
    memset(frame, 0, FRAME_LEN);
    memcpy(&frame[0], dst, 6);
    memcpy(&frame[6], src, 6);
    frame[12] = (ETHERTYPE >> 8) & 0xff;
    frame[13] = ETHERTYPE & 0xff;
    frame[14] = (uint8_t)seq;             /* payload marker */
    memset(&frame[15], 0xA5, FRAME_LEN - 15);
}

/*
 * dump_vi_state — print the Phase C mappings so hardware verification is
 * observable: VI geometry from GET_VI_INFO, the mapped ring addresses, and
 * a liveness read of HW_REV_ID through its bounded slice capability.
 */
static void
dump_vi_state(const sfc7120_if_t *sfc)
{
    const sfc7120_vi_info_req_t *vi = &sfc->vi_info;

    printf("test: vi_info: tx_paddr=0x%016lx rx_paddr=0x%016lx\n",
           (unsigned long)vi->tx_buffer_paddr,
           (unsigned long)vi->rx_buffer_paddr);
    printf("test: vi_info: vi_base=%u evq=%u rxq=%u txq=%u "
           "ntx=%u nrx=%u nevq=%u tx_head=%u rx_head=%u evq_rptr=%u\n",
           vi->vi_base, vi->evq_instance, vi->rxq_instance, vi->txq_instance,
           vi->num_tx_desc, vi->num_rx_desc, vi->num_evq_entry,
           vi->tx_head, vi->rx_head, vi->evq_read_ptr);
    printf("test: rings: tx_desc=%p rx_desc=%p evq=%p (slices=%zu)\n",
           sfc->tx_desc_ring, sfc->rx_desc_ring, sfc->evq_ring,
           sfc->mmio_slices_len);
    /* The MMIO base is a munmap token: full-region bounds, but LOAD/STORE
     * stripped — %#p shows the perms so that's verifiable on hardware. */
    printf("test: mmio base cap (munmap token) = %#p\n",
           sfc->region_maps[SFC7120_MMIO_REGION].base);

    if (sfc->mmio_slices_len > SFC7120_SLICE_HW_REV_ID) {
        uint32_t rev = *(volatile uint32_t * __capability)
            sfc->mmio_slices[SFC7120_SLICE_HW_REV_ID].addr;
        printf("test: HW_REV_ID via slice cap = 0x%08x (%s)\n",
               rev, rev != 0 ? "ok" : "BAD — expected non-zero");
    }
}

static int
run_producer(void)
{
    sfc7120_if_t sfc = { .dev_path = DEV_PF0 };
    uint8_t      dst[6], frame[FRAME_LEN];

    if (sfc7120_init(&sfc) != 0) {
        fprintf(stderr, "test: producer init failed\n");
        return 1;
    }
    printf("test: producer up on %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           DEV_PF0, sfc.mac_addr[0], sfc.mac_addr[1], sfc.mac_addr[2],
           sfc.mac_addr[3], sfc.mac_addr[4], sfc.mac_addr[5]);
    dump_vi_state(&sfc);

    /* PF1 MAC = PF0 base MAC + 1 in the low octet. */
    memcpy(dst, sfc.mac_addr, 6);
    dst[5] += 1;

    for (int i = 0; i < TEST_PACKETS; i++) {
        build_frame(frame, dst, sfc.mac_addr, i);
        if (sfc7120_tx(&sfc, frame, FRAME_LEN) != 0) {
            fprintf(stderr, "test: TX %d failed\n", i);
            sfc7120_destroy(&sfc);
            return 1;
        }
        printf("test: TX %d ok (%d bytes)\n", i, FRAME_LEN);
    }

    sfc7120_destroy(&sfc);
    printf("test: producer done\n");
    return 0;
}

/*
 * run_producer_direct — Phase F producer: submits the batch via
 * sfc7120_tx_post (returns immediately, no per-packet wait), then harvests TX
 * completions through sfc7120_poll — the single EVQ reader. Kernel out of the
 * data path. Pair with `sfctest rxd` on PF1 for a full direct dual-port test.
 */
static int
run_producer_direct(void)
{
    sfc7120_if_t sfc = { .dev_path = DEV_PF0 };
    uint8_t      dst[6], frame[FRAME_LEN];

    if (sfc7120_init(&sfc) != 0) {
        fprintf(stderr, "test: direct producer init failed\n");
        return 1;
    }
    printf("test: direct producer up on %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           DEV_PF0, sfc.mac_addr[0], sfc.mac_addr[1], sfc.mac_addr[2],
           sfc.mac_addr[3], sfc.mac_addr[4], sfc.mac_addr[5]);
    dump_vi_state(&sfc);
    printf("test: direct TX from tx_head=%u\n", sfc.tx_head);

    /* PF1 MAC = PF0 base MAC + 1 in the low octet. */
    memcpy(dst, sfc.mac_addr, 6);
    dst[5] += 1;

    /* Submit the whole batch — tx_post returns immediately (no per-packet
     * wait). Each call copies the frame into its own TX slot, so reusing the
     * local frame buffer between iterations is safe. */
    for (int i = 0; i < TEST_PACKETS; i++) {
        build_frame(frame, dst, sfc.mac_addr, i);
        if (sfc7120_tx_post(&sfc, frame, FRAME_LEN) != 0) {
            fprintf(stderr, "test: direct TX post %d failed\n", i);
            sfc7120_destroy(&sfc);
            return 1;
        }
        printf("test: direct TX %d posted (%d bytes)\n", i, FRAME_LEN);
    }

    /* Harvest TX completions through poll — the sole EVQ reader. */
    sfc7120_ev_t evs[8];
    int  done = 0;
    long tries = 0;
    while (done < TEST_PACKETS && tries++ < 2000000000L) {
        int n = sfc7120_poll(&sfc, evs, 8);
        if (n < 0) {
            fprintf(stderr, "test: poll failed\n");
            break;
        }
        for (int j = 0; j < n; j++)
            if (evs[j].type == SFC7120_EV_TX)
                printf("test: TX complete (desc %u) %d/%d\n",
                       evs[j].tx_desc_idx, ++done, TEST_PACKETS);
    }

    sfc7120_destroy(&sfc);
    printf("test: direct producer done (%d/%d completed)\n", done, TEST_PACKETS);
    return done == TEST_PACKETS ? 0 : 1;
}

static int
run_consumer(void)
{
    sfc7120_if_t sfc = { .dev_path = DEV_PF1 };
    uint8_t      frame[SFC7120_RX_BUFFER_SIZE];

    if (sfc7120_init(&sfc) != 0) {
        fprintf(stderr, "test: consumer init failed\n");
        return 1;
    }
    printf("test: consumer up on %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           DEV_PF1, sfc.mac_addr[0], sfc.mac_addr[1], sfc.mac_addr[2],
           sfc.mac_addr[3], sfc.mac_addr[4], sfc.mac_addr[5]);
    dump_vi_state(&sfc);

    for (int i = 0; i < TEST_PACKETS; i++) {
        size_t len = 0;
        if (sfc7120_rx(&sfc, frame, &len) != 0) {
            fprintf(stderr, "test: RX %d failed\n", i);
            break;
        }
        printf("test: RX %d ok (%zu bytes, payload marker 0x%02x)\n",
               i, len, len > 14 ? frame[14] : 0);
    }

    sfc7120_destroy(&sfc);
    printf("test: consumer done\n");
    return 0;
}

/*
 * run_consumer_direct — Phase F consumer: poll is the sole EVQ reader; each
 * RX_EV it returns is handed to sfc7120_rx_recv to read + recycle the slot.
 * Kernel out of the data path. Run `sfctest tx` or `sfctest txd` on PF0.
 */
static int
run_consumer_direct(void)
{
    sfc7120_if_t sfc = { .dev_path = DEV_PF1 };
    uint8_t      frame[SFC7120_RX_BUFFER_SIZE];

    if (sfc7120_init(&sfc) != 0) {
        fprintf(stderr, "test: direct consumer init failed\n");
        return 1;
    }
    printf("test: direct consumer up on %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           DEV_PF1, sfc.mac_addr[0], sfc.mac_addr[1], sfc.mac_addr[2],
           sfc.mac_addr[3], sfc.mac_addr[4], sfc.mac_addr[5]);
    dump_vi_state(&sfc);
    printf("test: direct RX from rx_head=%u (start `sfctest tx` on PF0)\n",
           sfc.rx_head);

    /* poll is the only EVQ reader; dispatch each RX_EV to rx_recv (read +
     * recycle the slot). TX_EVs would be handled here too in a full-duplex
     * app; this consumer only expects RX. */
    sfc7120_ev_t evs[8];
    int  got   = 0;
    long tries = 0;
    while (got < TEST_PACKETS && tries++ < 2000000000L) {
        int n = sfc7120_poll(&sfc, evs, 8);
        if (n < 0) {
            fprintf(stderr, "test: poll failed\n");
            break;
        }
        for (int j = 0; j < n; j++) {
            if (evs[j].type != SFC7120_EV_RX)
                continue;
            size_t len = 0;
            if (sfc7120_rx_recv(&sfc, frame, &len, evs[j].rx_bytes) != 0) {
                fprintf(stderr, "test: rx_recv failed\n");
                tries = 2000000000L;   /* bail out of the wait loop */
                break;
            }
            printf("test: direct RX %d ok (%zu bytes, payload marker 0x%02x)\n",
                   got, len, len > 14 ? frame[14] : 0);
            got++;
        }
    }

    sfc7120_destroy(&sfc);
    printf("test: direct consumer done (%d/%d received)\n", got, TEST_PACKETS);
    return got == TEST_PACKETS ? 0 : 1;
}

/*
 * run_poller — Phase C consumer: drains the data EVQ directly via
 * sfc7120_poll (zero syscalls in the poll loop) instead of the SFC7120_RX
 * ioctl. The kernel still owns descriptor posting; we only observe + ack
 * events. Run `sfctest tx` on PF0 as the producer — each 64-byte frame
 * should appear here as one RX_EV with rx_bytes = 64 + 14 (EF10 prefix).
 */
static int
run_poller(void)
{
    sfc7120_if_t sfc = { .dev_path = DEV_PF1 };
    sfc7120_ev_t evs[8];
    int          rx_seen = 0;
    long         tries;

    if (sfc7120_init(&sfc) != 0) {
        fprintf(stderr, "test: poller init failed\n");
        return 1;
    }
    printf("test: poller up on %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           DEV_PF1, sfc.mac_addr[0], sfc.mac_addr[1], sfc.mac_addr[2],
           sfc.mac_addr[3], sfc.mac_addr[4], sfc.mac_addr[5]);
    dump_vi_state(&sfc);
    printf("test: polling data EVQ from read_ptr=%u (start `sfctest tx` "
           "on PF0)\n", sfc.evq_read_ptr);

    /* Generous budget — the oracle spins 100000 tries per packet; we wait
     * for a human to launch the producer in another window. */
    for (tries = 0; tries < 2000000000L && rx_seen < TEST_PACKETS; tries++) {
        int n = sfc7120_poll(&sfc, evs, 8);
        if (n < 0) {
            fprintf(stderr, "test: sfc7120_poll failed\n");
            break;
        }
        for (int i = 0; i < n; i++) {
            const char *name =
                evs[i].type == SFC7120_EV_RX ? "RX_EV" :
                evs[i].type == SFC7120_EV_TX ? "TX_EV" : "OTHER";
            printf("test: ev %s raw=0x%016lx rx_bytes=%u tx_idx=%u "
                   "read_ptr now %u\n",
                   name, (unsigned long)evs[i].raw, evs[i].rx_bytes,
                   evs[i].tx_desc_idx, sfc.evq_read_ptr);
            if (evs[i].type == SFC7120_EV_RX)
                rx_seen++;
        }
    }

    printf("test: poller saw %d/%d RX events (final read_ptr=%u)\n",
           rx_seen, TEST_PACKETS, sfc.evq_read_ptr);
    sfc7120_destroy(&sfc);
    printf("test: poller done\n");
    return rx_seen == TEST_PACKETS ? 0 : 1;
}

/*
 * ============================================================================
 * Phase G — CHERI security validation
 * ============================================================================
 *
 * `sfctest sec` deliberately commits three memory-safety violations against
 * the bounded capabilities the kernel handed us and checks that each one traps
 * in hardware (SIGPROT — CHERI capability fault) rather than corrupting the
 * NIC, another VI, or kernel state:
 *
 *   1. Out-of-bounds access on a doorbell slice capability. The TX_DESC_DBL
 *      slice is 12 bytes; we offset well past its bound and write. CHERI must
 *      trap before the store reaches MMIO.
 *   2. Buffer overflow past the TX packet-buffer capability. The buffer cap is
 *      bounded to 1 MB (512 * 2048); we walk one byte past the end and write.
 *   3. Ringing/advancing past the TX descriptor-ring capability. The ring cap
 *      is 4 KB (512 * 8); we index one descriptor past the end and write.
 *
 * Each violation runs in a forked child so the expected SIGPROT terminates
 * only the child; the parent reads the child's exit status and reports PASS
 * (child died on a capability fault — the bound held) or FAIL (the access was
 * allowed — the bound did NOT hold). A SIGPROT-handler path inside the child
 * is also kept for boards where the signal can be caught, so we can print the
 * faulting capability's metadata before exiting.
 */

static jmp_buf  sec_env;
static volatile int sec_sig;

static void
sec_fault_handler(int sig)
{
    sec_sig = sig;
    longjmp(sec_env, 1);
}

static void
print_cap(const char *what, void * __capability cap)
{
    printf("    %s: base=0x%012lx len=%zu off=%zu perms=0x%05lx tag=%d\n",
           what,
           (unsigned long)cheri_getbase(cap),
           cheri_getlen(cap),
           cheri_getoffset(cap),
           (unsigned long)cheri_getperm(cap),
           (int)cheri_gettag(cap));
}

/*
 * Each violation_fn performs ONE deliberately-illegal access. It runs in the
 * child; if CHERI lets the longjmp handler catch the fault we return here and
 * the child exits 0 (NO fault — a FAIL). If the access traps uncatchably the
 * child dies on the signal, which the parent sees as a PASS.
 */
typedef void (*violation_fn)(sfc7120_if_t *sfc);

static void
violate_doorbell_oob(sfc7120_if_t *sfc)
{
    volatile uint32_t * __capability db = (volatile uint32_t * __capability)
        sfc->mmio_slices[SFC7120_SLICE_TX_DESC_DBL].addr;
    print_cap("TX_DESC_DBL slice", (void * __capability)db);
    /* Slice is 12 bytes (3 dwords). Index [64] is offset 256 — far past the
     * bound. Writing it must trap before any MMIO store happens. */
    printf("    -> writing dword index [64] (offset 256, bound 12)\n");
    db[64] = 0xdeadbeef;
    printf("    -> write returned (NO TRAP)\n");
}

static void
violate_txbuf_overflow(sfc7120_if_t *sfc)
{
    volatile uint8_t * __capability buf =
        (volatile uint8_t * __capability)sfc->tx_buffer;
    size_t cap_len = cheri_getlen((void * __capability)buf);
    print_cap("TX buffer", (void * __capability)buf);
    /* Buffer is 1 MB; touching byte [cap_len] is one past the end. */
    printf("    -> writing byte index [%zu] (one past %zu-byte bound)\n",
           cap_len, cap_len);
    buf[cap_len] = 0xA5;
    printf("    -> write returned (NO TRAP)\n");
}

static void
violate_descring_oob(sfc7120_if_t *sfc)
{
    volatile uint64_t * __capability ring =
        (volatile uint64_t * __capability)sfc->tx_desc_ring;
    size_t cap_len = cheri_getlen((void * __capability)ring);
    size_t n_desc  = cap_len / SFC7120_TX_DESC_SIZE;
    print_cap("TX desc ring", (void * __capability)ring);
    /* Ring holds n_desc descriptors (indices 0..n_desc-1). Writing index
     * [n_desc] is one descriptor past the ring's capability bound. */
    printf("    -> writing descriptor index [%zu] (bound is %zu descriptors)\n",
           n_desc, n_desc);
    ring[n_desc] = 0xdeadbeefcafef00dULL;
    printf("    -> write returned (NO TRAP)\n");
}

/*
 * run_one_violation — fork, run the violation in the child, report from the
 * parent. Returns 1 if the bound held (PASS: child trapped), 0 otherwise.
 */
static int
run_one_violation(int n, const char *desc, sfc7120_if_t *sfc, violation_fn fn)
{
    printf("\n[sec %d] %s\n", n, desc);
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        perror("    fork");
        return 0;
    }
    if (pid == 0) {
        /* Child: try to catch the fault so we can report cap metadata, but a
         * truly uncatchable CHERI trap will just kill us — which is the PASS
         * signal the parent is looking for. */
        signal(SIGPROT, sec_fault_handler);
        signal(SIGSEGV, sec_fault_handler);
        signal(SIGBUS,  sec_fault_handler);
        if (setjmp(sec_env) == 0) {
            fn(sfc);
            /* Reached here = NO fault. The access was allowed. */
            _exit(42);          /* sentinel: bound did NOT hold */
        } else {
            printf("    [child] caught signal %d (capability fault) — bound held\n",
                   sec_sig);
            _exit(0);           /* caught the fault: PASS */
        }
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        int s = WTERMSIG(status);
        printf("    PASS — child killed by signal %d (%s); hardware trapped "
               "the access\n", s, strsignal(s));
        return 1;
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0) {
            printf("    PASS — child caught the capability fault and exited "
                   "cleanly\n");
            return 1;
        }
        if (code == 42) {
            printf("    FAIL — access was ALLOWED; the capability bound did "
                   "NOT trap!\n");
            return 0;
        }
        printf("    FAIL — child exited %d (unexpected)\n", code);
        return 0;
    }
    printf("    FAIL — child neither exited nor signalled cleanly\n");
    return 0;
}

static int
run_security_test(void)
{
    sfc7120_if_t sfc = { .dev_path = DEV_PF1 };

    if (sfc7120_init(&sfc) != 0) {
        fprintf(stderr, "test: security init failed\n");
        return 1;
    }
    printf("============================================================\n");
    printf("  CAPIO Security Test — Solarflare 7120 (sfc7120pol)\n");
    printf("  Each test commits a memory violation in a forked child;\n");
    printf("  PASS = hardware (CHERI) trapped it.\n");
    printf("============================================================\n");
    dump_vi_state(&sfc);

    int passes = 0, total = 3;
    passes += run_one_violation(1,
        "Out-of-bounds write on the TX doorbell slice capability",
        &sfc, violate_doorbell_oob);
    passes += run_one_violation(2,
        "Buffer overflow one byte past the TX packet-buffer capability",
        &sfc, violate_txbuf_overflow);
    passes += run_one_violation(3,
        "Index one descriptor past the TX descriptor-ring capability",
        &sfc, violate_descring_oob);

    printf("\n============================================================\n");
    printf("  Security test result: %d/%d bounds enforced in hardware\n",
           passes, total);
    printf("============================================================\n");

    sfc7120_destroy(&sfc);
    return passes == total ? 0 : 1;
}

int
main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s tx|txd|rx|rxd|poll|sec\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "tx") == 0)
        return run_producer();
    if (strcmp(argv[1], "txd") == 0)
        return run_producer_direct();
    if (strcmp(argv[1], "rx") == 0)
        return run_consumer();
    if (strcmp(argv[1], "rxd") == 0)
        return run_consumer_direct();
    if (strcmp(argv[1], "poll") == 0)
        return run_poller();
    if (strcmp(argv[1], "sec") == 0)
        return run_security_test();

    fprintf(stderr, "usage: %s tx|txd|rx|rxd|poll|sec\n", argv[0]);
    return 2;
}
