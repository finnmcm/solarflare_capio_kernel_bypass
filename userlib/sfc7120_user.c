#include "sfc7120_user.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int
sfc7120_init(sfc7120_if_t *sfc)
{
    sfc->fd        = -1;
    sfc->modmap_fd = -1;
    sfc->tx_buffer = NULL;
    sfc->rx_buffer = NULL;

    sfc->fd = open(DEVSFC7120, O_RDWR);
    if (sfc->fd < 0) {
        perror("sfc7120_init: open " DEVSFC7120);
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

    /* TODO: mmap tx_buffer */
    /* TODO: mmap rx_buffer */

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

void
sfc7120_destroy(sfc7120_if_t *sfc)
{
    if (sfc->cap_token != NULL) {
        free(sfc->cap_token);
        sfc->cap_token = NULL;
    }
    if (sfc->modmap_fd >= 0)
        close(sfc->modmap_fd);
    if (sfc->fd >= 0)
        close(sfc->fd);
}
