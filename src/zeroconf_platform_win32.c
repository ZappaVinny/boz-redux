#include "s3e_host_internal.h"

#include "zeroconf_platform.h"

#include <iphlpapi.h>

enum {
    MDNS_PORT = 5353,
    MDNS_MULTICAST_ADDRESS = 0xe00000fbu,
};

uint64_t zeroconf_platform_now_ms(void) {
    return monotonic_ms();
}

int zeroconf_platform_select_ipv4(struct in_addr *selected) {
    if (!selected) {
        return 0;
    }
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *adapters = NULL;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        free(adapters);
        adapters = malloc(size);
        if (!adapters) {
            return 0;
        }
        result = GetAdaptersAddresses(AF_INET,
                                      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER,
                                      NULL, adapters, &size);
    }
    int found = 0;
    if (result == NO_ERROR) {
        for (const IP_ADAPTER_ADDRESSES *adapter = adapters; adapter && !found;
             adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp ||
                adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
                (adapter->Flags & IP_ADAPTER_NO_MULTICAST)) {
                continue;
            }
            for (const IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress; unicast;
                 unicast = unicast->Next) {
                const struct sockaddr *address = unicast->Address.lpSockaddr;
                if (!address || address->sa_family != AF_INET) {
                    continue;
                }
                const struct sockaddr_in *candidate = (const struct sockaddr_in *)address;
                if (candidate->sin_addr.s_addr != htonl(INADDR_ANY)) {
                    *selected = candidate->sin_addr;
                    found = 1;
                    break;
                }
            }
        }
    }
    free(adapters);
    return found;
}

static int configure_socket(int socket_fd, struct in_addr interface_address) {
    int enabled = 1;
    int ttl = 255;
    int multicast_ttl = 255;
    int loop = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(MDNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    struct ip_mreq multicast = {
        .imr_multiaddr.s_addr = htonl(MDNS_MULTICAST_ADDRESS),
        .imr_interface = interface_address,
    };

    (void)setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    return setsockopt(socket_fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) == 0 &&
           setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_TTL, &multicast_ttl,
                      sizeof(multicast_ttl)) == 0 &&
           setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) == 0 &&
           setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_IF, &interface_address,
                      sizeof(interface_address)) == 0 &&
           socket_set_nonblocking(socket_fd, 1) == 0 &&
           bind((SOCKET)socket_fd, (const struct sockaddr *)&address, sizeof(address)) == 0 &&
           (setsockopt(socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &multicast, sizeof(multicast)) ==
                0 ||
            socket_errno() == EADDRINUSE);
}

int zeroconf_platform_open_socket(struct in_addr *selected) {
    struct in_addr interface_address;
    if (!zeroconf_platform_select_ipv4(&interface_address)) {
        return -1;
    }
    SOCKET handle = socket(AF_INET, SOCK_DGRAM, 0);
    if (handle == INVALID_SOCKET) {
        return -1;
    }
    int socket_fd = (int)handle;
    if (!configure_socket(socket_fd, interface_address)) {
        socket_close(socket_fd);
        return -1;
    }
    if (selected) {
        *selected = interface_address;
    }
    return socket_fd;
}

int zeroconf_platform_send(int socket_fd, const uint8_t *packet, size_t packet_size,
                           const struct sockaddr_in *destination) {
    if (socket_fd < 0 || !packet || !packet_size) {
        return 0;
    }
    struct sockaddr_in multicast = {
        .sin_family = AF_INET,
        .sin_port = htons(MDNS_PORT),
        .sin_addr.s_addr = htonl(MDNS_MULTICAST_ADDRESS),
    };
    const struct sockaddr_in *target = destination ? destination : &multicast;
    return sendto(socket_fd, packet, packet_size, 0, (const struct sockaddr *)target,
                  sizeof(*target)) == (int)packet_size;
}

ssize_t zeroconf_platform_receive(int socket_fd, uint8_t *packet, size_t capacity,
                                  struct sockaddr_in *source) {
    if (socket_fd < 0 || !packet || !capacity || !source) {
        return -1;
    }
    socklen_t source_length = sizeof(*source);
    int received = recvfrom(socket_fd, packet, capacity, 0, (struct sockaddr *)source,
                            &source_length);
    if (received < 0) {
        errno = socket_errno();
        return -1;
    }
    return received;
}

void zeroconf_platform_close_socket(int socket_fd) {
    if (socket_fd >= 0) {
        socket_close(socket_fd);
    }
}
