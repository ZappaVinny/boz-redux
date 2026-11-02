#ifndef CODBOZ_ZEROCONF_PLATFORM_H
#define CODBOZ_ZEROCONF_PLATFORM_H

#include "socket_compat.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

/* [network] interface (BOZ_NETWORK_INTERFACE): the IPv4 address of the adapter co-op discovery
 * uses, for PCs with a LAN VPN (Radmin) next to the home network. Returns 0 when unset. */
static inline int zeroconf_platform_wanted_ipv4(struct in_addr *wanted) {
    const char *value = getenv("BOZ_NETWORK_INTERFACE");
    if (!value || !value[0]) {
        return 0;
    }
    if (inet_pton(AF_INET, value, wanted) != 1) {
        fprintf(stderr, "[net] BOZ_NETWORK_INTERFACE: %s is not an IPv4 address\n", value);
        return 0;
    }
    return 1;
}

uint64_t zeroconf_platform_now_ms(void);
/* Selection and send return boolean success; open returns a descriptor or -1. */
int zeroconf_platform_select_ipv4(struct in_addr *selected);
int zeroconf_platform_open_socket(struct in_addr *selected);
int zeroconf_platform_send(int socket_fd, const uint8_t *packet, size_t packet_size,
                           const struct sockaddr_in *destination);
/* Receive returns a byte count or -1 with errno set. */
ssize_t zeroconf_platform_receive(int socket_fd, uint8_t *packet, size_t capacity,
                                  struct sockaddr_in *source);
void zeroconf_platform_close_socket(int socket_fd);

#endif
