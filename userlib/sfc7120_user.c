#include "sfc7120_user.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * map_region — map one CAPIO region, sliced or not.
 *
 * Always asks the kernel for the region's size and slice count first
 * (MODMAPIOC_GET_SLICES), so callers never compute lengths themselves.
 *
 * Unsliced (out_slices == NULL): returns a capability to the whole region.
 *
 * Sliced (out_slices != NULL): allocates the slice array — a real bounded
 * allocation, the kernel validates its length via cheri_getlen — and the
 * kernel fills each entry's addr with a per-register bounded capability.
 * Slices come back by position in sfc7120_reg_slices[] order
 * (../sfc7120_tables.c); index with sfc7120_mmio_slice_idx_t. The returned
 * value is the full-region capability with LOAD/STORE stripped — a munmap
 * token only (modmap keeps SW_VMEM + bounds so destroy can unmap the
 * region); all register access goes through the slice caps.
 *
 * Every successful map is recorded in sfc->region_maps[map_type] so
 * sfc7120_destroy can munmap it.
 */
static void *
map_region(sfc7120_if_t *sfc, sfc7120_vm_map_type_t map_type,
           user_slice_def_t **out_slices, size_t *out_slice_len)
{
    get_slice_length_t length_req;
    length_req.fd       = sfc->fd;
    length_req.map_type = (int)map_type;

    if (ioctl(sfc->modmap_fd, MODMAPIOC_GET_SLICES, &length_req) < 0) {
        perror("sfc7120: MODMAPIOC_GET_SLICES failed");
        return NULL;
    }

    size_t n_slices   = length_req.region_sizes.slice_def_length;
    size_t region_len = length_req.region_sizes.region_length;

    if (region_len == 0) {
        fprintf(stderr, "sfc7120: region %d has length 0 — kernel region "
                "state is stale (smem len wiped by a prior teardown?); "
                "reload sfc7120pol.ko\n", (int)map_type);
        return NULL;
    }

    user_slice_def_t *slices = NULL;
    if (out_slices != NULL) {
        slices = calloc(n_slices ? n_slices : 1, sizeof(user_slice_def_t));
        if (slices == NULL) {
            perror("sfc7120: calloc slices");
            return NULL;
        }
    } else if (n_slices != 0) {
        fprintf(stderr, "sfc7120: region %d is sliced (%zu slices) but no "
                "slice array was passed\n", (int)map_type, n_slices);
        return NULL;
    }

    /* Zero-init: the kernel copyincap's the whole struct, so the slice
     * fields must not be stack garbage even for unsliced regions. */
    user_map_req_t map_req = { 0 };
    map_req.user_cap          = sfc->cap_req.user_cap;
    map_req.sealed_cap        = sfc->cap_req.sealed_cap;
    map_req.map_type          = (int)map_type;
    map_req.slice_definitions = slices;

    mmap_req_user_t req;
    req.addr  = NULL;
    req.len   = region_len;
    req.prot  = PROT_READ | PROT_WRITE;
    req.flags = MAP_SHARED;
    req.fd    = sfc->fd;
    req.pos   = 0;
    req.extra = (void * __capability)(&map_req);

    if (ioctl(sfc->modmap_fd, MODMAPIOC_MAP, &req) < 0) {
        perror("sfc7120: MODMAPIOC_MAP failed");
        free(slices);
        return NULL;
    }

    if (out_slices != NULL) {
        *out_slices = slices;
        if (out_slice_len != NULL)
            *out_slice_len = n_slices;
    }

    sfc->region_maps[map_type].base = req.addr;
    sfc->region_maps[map_type].len  = region_len;
    return req.addr;
}

int
sfc7120_init(sfc7120_if_t *sfc)
{
    sfc->fd           = -1;
    sfc->modmap_fd    = -1;
    sfc->tx_buffer    = NULL;
    sfc->rx_buffer    = NULL;
    sfc->tx_desc_ring = NULL;
    sfc->rx_desc_ring = NULL;
    sfc->evq_ring     = NULL;
    sfc->mmio_slices  = NULL;
    sfc->mmio_slices_len = 0;
    sfc->vi_info_valid   = false;
    sfc->evq_read_ptr    = 0;
    sfc->used_poll       = false;
    memset(sfc->region_maps, 0, sizeof(sfc->region_maps));

    const char *dev = sfc->dev_path != NULL ? sfc->dev_path : DEVSFC7120;
    sfc->fd = open(dev, O_RDWR);
    if (sfc->fd < 0) {
        fprintf(stderr, "sfc7120_init: open %s: ", dev);
        perror(NULL);
        return -1;
    }

    sfc->modmap_fd = open(DEVMODMAP, O_RDWR);
    if (sfc->modmap_fd < 0) {
        perror("sfc7120_init: open " DEVMODMAP);
        goto fail;
    }

    sfc->cap_token = malloc(PAGE_SIZE); // random page malloced that we can use to verify using the kernel to see if we actually own this memory in the process
    if (sfc->cap_token == NULL) {
        perror("sfc7120_init: malloc cap_token");
        goto fail;
    }
    sfc->cap_req.user_cap = sfc->cap_token;

    if (ioctl(sfc->fd, CAPIO_ATTACH, &sfc->cap_req) < 0) { // the above random page is used as part of capio_attach 
        perror("sfc7120_init: CAPIO_ATTACH");
        goto fail;
    }

    sfc->tx_buffer = map_region(sfc, SFC7120_TX_BUFFER, NULL, NULL);
    if (sfc->tx_buffer == NULL) {
        perror("sfc7120_init: map TX buffer");
        goto fail;
    }

    sfc->rx_buffer = map_region(sfc, SFC7120_RX_BUFFER, NULL, NULL);
    if (sfc->rx_buffer == NULL) {
        perror("sfc7120_init: map RX buffer");
        goto fail;
    }

    /* Phase C: descriptor rings + data EVQ ring (4 KB each) */
    sfc->tx_desc_ring = map_region(sfc, SFC7120_TX_DESC_RING, NULL, NULL);
    if (sfc->tx_desc_ring == NULL) {
        perror("sfc7120_init: map TX desc ring");
        goto fail;
    }

    sfc->rx_desc_ring = map_region(sfc, SFC7120_RX_DESC_RING, NULL, NULL);
    if (sfc->rx_desc_ring == NULL) {
        perror("sfc7120_init: map RX desc ring");
        goto fail;
    }

    sfc->evq_ring = map_region(sfc, SFC7120_EVQ_RING, NULL, NULL);
    if (sfc->evq_ring == NULL) {
        perror("sfc7120_init: map EVQ ring");
        goto fail;
    }

    /* Phase C: per-register doorbell capabilities from the sliced BAR */
    if (map_region(sfc, SFC7120_MMIO_REGION,
                   &sfc->mmio_slices, &sfc->mmio_slices_len) == NULL) {
        perror("sfc7120_init: map MMIO slices");
        goto fail;
    }
    if (sfc->mmio_slices_len != SFC7120_SLICE_COUNT)
        fprintf(stderr, "sfc7120_init: warning: kernel reports %zu MMIO "
                "slices, expected %d — slice indices may be stale\n",
                sfc->mmio_slices_len, SFC7120_SLICE_COUNT);

    /* Phase C: VI geometry — DMA bus addresses, instances, head pointers */
    sfc->vi_info.user_cap   = sfc->cap_req.user_cap;
    sfc->vi_info.sealed_cap = sfc->cap_req.sealed_cap;
    if (ioctl(sfc->fd, SFC7120_GET_VI_INFO, &sfc->vi_info) < 0) {
        perror("sfc7120_init: SFC7120_GET_VI_INFO");
        goto fail;
    }
    sfc->vi_info_valid = true;
    /* Seed our data-EVQ read pointer from the kernel's bookkeeping; it is
     * synced back via SFC7120_SET_EVQ_RPTR at destroy. */
    sfc->evq_read_ptr = sfc->vi_info.evq_read_ptr;

    sfc7120_mac_req_t mac_req;
    mac_req.user_cap   = sfc->cap_req.user_cap;
    mac_req.sealed_cap = sfc->cap_req.sealed_cap;
    if (ioctl(sfc->fd, SFC7120_GET_MAC, &mac_req) < 0) {
        perror("sfc7120_init: SFC7120_GET_MAC");
        goto fail;
    }
    memcpy(sfc->mac_addr, mac_req.mac_addr, 6);

    return 0;

fail:
    sfc7120_destroy(sfc);
    return -1;
}

/*
 * sfc7120_tx — send one packet through the kernel ioctl path (phase 1).
 *
 * Packages buf/len into a sfc7120_tx_req_t and hands it to the kernel via
 * SFC7120_TX. The kernel copies the packet into the TX DMA buffer, writes
 * a descriptor, rings the doorbell, and polls the EVQ for the TX completion
 * event before returning. Blocking — returns 0 on success, -1 on failure.
 */
int
sfc7120_tx(sfc7120_if_t *sfc, const void *buf, size_t len)
{
    sfc7120_tx_req_t req;
    req.user_cap    = sfc->cap_req.user_cap;
    req.sealed_cap  = sfc->cap_req.sealed_cap;
    req.tx_buf_addr = (uint8_t * __capability)buf;
    req.length      = len;
    req.status      = 0;

    if (ioctl(sfc->fd, SFC7120_TX, &req) < 0) {
        perror("sfc7120_tx: SFC7120_TX");
        return -1;
    }
    return 0;
}

/*
 * sfc7120_rx — receive one packet through the kernel ioctl path (phase 1).
 *
 * Passes buf to the kernel via SFC7120_RX. The kernel polls the EVQ until
 * an RX event arrives, copies the packet from the DMA buffer into buf, and
 * writes the byte count into *len_out. Blocking — returns 0 on success,
 * -1 on failure (including ETIMEDOUT if no packet arrives in time).
 */
int
sfc7120_rx(sfc7120_if_t *sfc, void *buf, size_t *len_out)
{
    sfc7120_rx_req_t req;
    req.user_cap        = sfc->cap_req.user_cap;
    req.sealed_cap      = sfc->cap_req.sealed_cap;
    req.raw_buffer      = buf;
    req.length_received = 0;
    req.status          = 0;
    req.error           = 0;

    if (ioctl(sfc->fd, SFC7120_RX, &req) < 0) {
        perror("sfc7120_rx: SFC7120_RX");
        return -1;
    }

    if (len_out != NULL)
        *len_out = req.length_received;
    return 0;
}

/*
 * sfc7120_poll — drain pending data-EVQ events directly (phase C).
 *
 * Zero-syscall: reads the mmapped data-EVQ (instance 1) ring through its
 * CHERI capability, decodes up to max_evs events into evs[], and acks the
 * consumed slots on the EVQ's RPTR doorbell through the bounded MMIO slice
 * cap. Non-blocking — returns the number of events digested (0 = none
 * pending), or -1 on bad arguments / missing mappings.
 *
 * The event loop is a verbatim port of the kernel ioctl handlers' EVQ poll
 * (../sfc7120.c SFC7120_TX/SFC7120_RX) — the verified oracle for this card:
 *   - empty = all-ones (NIC never wrote) or zero (already consumed); the
 *     consumer zeroes slots rather than re-priming 0xff
 *   - EV_CODE nibble (bits 63:60): RX_EV=0 (RX_BYTES in bits 13:0, incl.
 *     the 14-byte prefix), TX_EV=2 (TX_DESCR_INDX in bits 15:0); anything
 *     else is consumed and reported as OTHER
 *   - ack only when >=1 event was consumed: dsb sy, then write the new
 *     read pointer to the instance-1 RPTR doorbell (0x2400 slice)
 */
int
sfc7120_poll(sfc7120_if_t *sfc, sfc7120_ev_t *evs, int max_evs)
{
    if (sfc == NULL || evs == NULL || max_evs <= 0)
        return -1;
    if (sfc->evq_ring == NULL || sfc->mmio_slices == NULL ||
        sfc->mmio_slices_len <= SFC7120_SLICE_DATA_EVQ_RPTR_DBL)
        return -1;

    /* This session owns the EVQ read pointer from here on — record it so
     * destroy syncs evq_read_ptr back to the kernel. Without a poll, the
     * kernel's own pointer is authoritative and must be left alone. */
    sfc->used_poll = true;

    volatile uint64_t *evq = (volatile uint64_t *)sfc->evq_ring;
    int n = 0;

    while (n < max_evs) {
        uint64_t ev = evq[sfc->evq_read_ptr];

        /* All-ones or all-zeros means no event written yet. */
        if (ev == 0xffffffffffffffffULL || ev == 0)
            break;

        uint32_t code = (uint32_t)(ev >> 60) & 0xf;
        if (code == 0) {            /* RX_EV */
            evs[n].type     = SFC7120_EV_RX;
            evs[n].rx_bytes = (uint16_t)(ev & 0x3fff);
            evs[n].tx_desc_idx = 0;
        } else if (code == 2) {     /* TX_EV */
            evs[n].type        = SFC7120_EV_TX;
            evs[n].tx_desc_idx = (uint16_t)(ev & 0xffff);
            evs[n].rx_bytes    = 0;
        } else {                    /* DRIVER_EV=5, MCDI_EV=12, ... */
            evs[n].type        = SFC7120_EV_OTHER;
            evs[n].tx_desc_idx = 0;
            evs[n].rx_bytes    = 0;
        }
        evs[n].raw = ev;
        n++;

        /* Consume the event — zero the slot and advance. */
        evq[sfc->evq_read_ptr] = 0;
        sfc->evq_read_ptr =
            (sfc->evq_read_ptr + 1) & (SFC7120_NUM_EVQ_ENTRY - 1);
    }

    /* Ack consumed events on the data EVQ's instance-1 RPTR doorbell. The
     * dsb sy orders the slot-zeroing stores before the doorbell write so
     * the NIC never sees an acked-but-stale slot. */
    if (n > 0) {
        __asm__ volatile("dsb sy" ::: "memory");
        *(volatile uint32_t * __capability)
            sfc->mmio_slices[SFC7120_SLICE_DATA_EVQ_RPTR_DBL].addr =
            sfc->evq_read_ptr & SFC7120_EVQ_RPTR_MASK;
    }

    return n;
}

void
sfc7120_destroy(sfc7120_if_t *sfc)
{
    /* munmap every mapped region FIRST. Each munmap drops the cdev_pager
     * object's last reference, firing capio_pager_dtor, which is the only
     * thing that clears the kernel's per-region `mapped` flag — without it
     * the next run on this PF fails the re-map with EINVAL (capio.c:
     * "if(smem.mapped) return EINVAL"). The kernel-side vm_map_remove in
     * revoke_cap_token does NOT fire the dtor, so it cannot replace this
     * (matches the e1000/mlx5 reference drivers, which munmap everything).
     * For the sliced MMIO region, base is the perm-stripped full-region cap. */
    for (int i = 0; i < SFC7120_REGION_COUNT; i++) {
        if (sfc->region_maps[i].base != NULL) {
            if (munmap(sfc->region_maps[i].base, sfc->region_maps[i].len) != 0) {
                fprintf(stderr, "sfc7120_destroy: munmap region %d: ", i);
                perror(NULL);
            }
            sfc->region_maps[i].base = NULL;
            sfc->region_maps[i].len  = 0;
        }
    }
    sfc->tx_buffer    = NULL;
    sfc->rx_buffer    = NULL;
    sfc->tx_desc_ring = NULL;
    sfc->rx_desc_ring = NULL;
    sfc->evq_ring     = NULL;

    /* Sync our data-EVQ read pointer back into the kernel's bookkeeping so
     * the next run's GET_VI_INFO (and any ioctl-path TX/RX on this load)
     * starts from the right slot — the direct poll path consumes events the
     * kernel never sees. Best-effort: a failure leaves the kernel stale
     * (kldload recovers), but teardown must still proceed.
     *
     * ONLY when this session actually polled. In the ioctl-only path the
     * kernel advances data_evq_read_ptr itself and our evq_read_ptr is just
     * the stale init seed; syncing it would clobber the kernel pointer back
     * to that seed. Because INIT_EVQ runs once per module load (not per
     * open), the NIC's write pointer persists across sessions — so a clobber
     * desyncs every TX/RX after the first re-open. Skipped if init never
     * reached GET_VI_INFO, or if no poll ran. */
    if (sfc->fd >= 0 && sfc->vi_info_valid && sfc->used_poll) {
        sfc7120_evq_sync_req_t sync_req;
        sync_req.user_cap     = sfc->cap_req.user_cap;
        sync_req.sealed_cap   = sfc->cap_req.sealed_cap;
        sync_req.evq_read_ptr = sfc->evq_read_ptr;
        if (ioctl(sfc->fd, SFC7120_SET_EVQ_RPTR, &sync_req) < 0)
            perror("sfc7120_destroy: SFC7120_SET_EVQ_RPTR");
    }

    /* THEN revoke the token so the next run's CAPIO_ATTACH succeeds. Its
     * vm_map_remove fallback finds the ranges already unmapped and no-ops.
     * Must run while sfc->fd is still open and before freeing cap_token. */
    if (sfc->fd >= 0)
        (void)ioctl(sfc->fd, CAPIO_GOODBYE, &sfc->cap_req);

    if (sfc->mmio_slices != NULL) {
        free(sfc->mmio_slices);
        sfc->mmio_slices = NULL;
        sfc->mmio_slices_len = 0;
    }
    if (sfc->cap_token != NULL) {
        free(sfc->cap_token);
        sfc->cap_token = NULL;
    }
    if (sfc->modmap_fd >= 0)
        close(sfc->modmap_fd);
    if (sfc->fd >= 0)
        close(sfc->fd);
}
