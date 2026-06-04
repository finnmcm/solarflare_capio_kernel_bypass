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

#include <stdio.h>
#include <string.h>

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

int
main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s tx|rx\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "tx") == 0)
        return run_producer();
    if (strcmp(argv[1], "rx") == 0)
        return run_consumer();

    fprintf(stderr, "usage: %s tx|rx\n", argv[0]);
    return 2;
}
