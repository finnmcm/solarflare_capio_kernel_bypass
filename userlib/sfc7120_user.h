#ifndef SFC7120_USER_H
#define SFC7120_USER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioccom.h>

#include "../sfc7120_uapi.h"
#include "modmap.h"
#include "capio.h"   /* CAPIO_ATTACH / CAPIO_GOODBYE */

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define DEVSFC7120 "/dev/sfc7120pol1" // our actual module in tree
#define DEVMODMAP  "/dev/modmap"

typedef struct sfc7120_if { // state struct, everything we need from kernel stub
    const char *dev_path;   /* device node to open; NULL → DEVSFC7120 default */

    int     fd;
    int     modmap_fd;

    cap_req_t cap_req;

    uint8_t mac_addr[6];

    void   *tx_buffer;
    void   *rx_buffer;

    /* Phase C: direct data-path resources */
    void   *tx_desc_ring;          /* 4 KB mapped TX descriptor ring */
    void   *rx_desc_ring;          /* 4 KB mapped RX descriptor ring */
    void   *evq_ring;              /* 4 KB mapped data-EVQ (instance 1) ring */

    user_slice_def_t *mmio_slices; /* bounded caps, indexed by sfc7120_mmio_slice_idx_t */
    size_t            mmio_slices_len;

    sfc7120_vi_info_req_t vi_info; /* paddrs, vi_base, instances, counts, heads */

    /* Per-region mapping record for munmap at destroy; indexed by
     * sfc7120_vm_map_type_t. For the sliced MMIO region, base is the
     * perm-stripped (no LOAD/STORE) full-region cap — a munmap token only. */
    struct {
        void   *base;
        size_t  len;
    } region_maps[SFC7120_REGION_COUNT];

    void   *cap_token;  /* malloc'd page — raw material for CAPIO_ATTACH seal */
} sfc7120_if_t;

int  sfc7120_init(sfc7120_if_t *sfc);
void sfc7120_destroy(sfc7120_if_t *sfc);
int  sfc7120_tx(sfc7120_if_t *sfc, const void *buf, size_t len);
int  sfc7120_rx(sfc7120_if_t *sfc, void *buf, size_t *len_out);

#endif /* SFC7120_USER_H */
