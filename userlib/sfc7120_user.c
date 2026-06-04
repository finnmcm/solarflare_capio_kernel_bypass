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
 * (../sfc7120_tables.c); index with sfc7120_mmio_slice_idx_t. There is no
 * whole-region capability for a sliced region — the returned value is just
 * slices[0].addr, the slice array is what matters.
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

void
sfc7120_destroy(sfc7120_if_t *sfc)
{
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
