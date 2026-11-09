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
                                          GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS,
                                      NULL, adapters, &size);
    }
    /* Only Ethernet and Wi-Fi adapters carry mDNS multicast (LAN VPNs such as Radmin, Hamachi
     * and ZeroTier appear as Ethernet); tunnels such as Tailscale and WireGuard do not. The
     * adapter with the default gateway goes first, so the home network wins over virtual
     * adapters. Pass -1 looks only for the adapter [network] interface names. */
    struct in_addr wanted;
    int want = zeroconf_platform_wanted_ipv4(&wanted);
    int found = 0;
    for (int pass = want ? -1 : 0; result == NO_ERROR && pass < 2 && !found; ++pass) {
        for (const IP_ADAPTER_ADDRESSES *adapter = adapters; adapter && !found;
             adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp ||
                (adapter->IfType != IF_TYPE_ETHERNET_CSMACD &&
                 adapter->IfType != IF_TYPE_IEEE80211) ||
                (adapter->Flags & IP_ADAPTER_NO_MULTICAST) ||
                (pass == 0 && !adapter->FirstGatewayAddress)) {
                continue;
            }
            for (const IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress; unicast;
                 unicast = unicast->Next) {
                const struct sockaddr *address = unicast->Address.lpSockaddr;
                if (!address || address->sa_family != AF_INET) {
                    continue;
                }
                const struct sockaddr_in *candidate = (const struct sockaddr_in *)address;
                if (pass == -1 && candidate->sin_addr.s_addr != wanted.s_addr) {
                    continue;
                }
                if (candidate->sin_addr.s_addr != htonl(INADDR_ANY)) {
                    *selected = candidate->sin_addr;
                    found = 1;
                    break;
                }
            }
        }
        if (pass == -1 && !found) {
            fprintf(stderr, "[net] no multicast adapter has %s; picking one automatically\n",
                    getenv("BOZ_NETWORK_INTERFACE"));
        }
    }
    free(adapters);
    return found;
}

static IP_ADAPTER_ADDRESSES *adapter_list(void) {
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *adapters = NULL;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        free(adapters);
        adapters = malloc(size);
        if (!adapters) {
            return NULL;
        }
        result = GetAdaptersAddresses(AF_INET,
                                      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER,
                                      NULL, adapters, &size);
    }
    if (result != NO_ERROR) {
        free(adapters);
        return NULL;
    }
    return adapters;
}

int zeroconf_platform_list_ipv4(struct in_addr *addresses, struct in_addr *netmasks, int capacity) {
    IP_ADAPTER_ADDRESSES *adapters = capacity > 0 ? adapter_list() : NULL;
    if (!adapters) {
        return 0;
    }
    struct in_addr wanted;
    int want = zeroconf_platform_wanted_ipv4(&wanted);
    int count = 0;
    for (int pass = want ? 0 : 1; pass < 2 && count == 0; ++pass) {
        for (const IP_ADAPTER_ADDRESSES *adapter = adapters; adapter && count < capacity;
             adapter = adapter->Next) {
            /* Ethernet and Wi-Fi carry mDNS multicast; LAN VPNs (Radmin, ZeroTier, Hamachi)
             * appear as Ethernet. */
            if (adapter->OperStatus != IfOperStatusUp ||
                (adapter->IfType != IF_TYPE_ETHERNET_CSMACD &&
                 adapter->IfType != IF_TYPE_IEEE80211) ||
                (adapter->Flags & IP_ADAPTER_NO_MULTICAST)) {
                continue;
            }
            for (const IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress;
                 unicast && count < capacity; unicast = unicast->Next) {
                const struct sockaddr *address = unicast->Address.lpSockaddr;
                if (!address || address->sa_family != AF_INET) {
                    continue;
                }
                struct in_addr candidate = ((const struct sockaddr_in *)address)->sin_addr;
                uint32_t host = ntohl(candidate.s_addr);
                if (!host || (host >> 16) == 0xa9feu) {
                    continue; /* 169.254/16: no network behind it */
                }
                if (pass == 0 && candidate.s_addr != wanted.s_addr) {
                    continue;
                }
                addresses[count] = candidate;
                if (netmasks) {
                    unsigned prefix = unicast->OnLinkPrefixLength;
                    netmasks[count].s_addr =
                        htonl(prefix >= 32 ? 0xffffffffu : prefix ? ~(0xffffffffu >> prefix) : 0);
                }
                ++count;
            }
        }
    }
    free(adapters);
    return count;
}

int zeroconf_platform_set_multicast_interface(int socket_fd, struct in_addr interface_address) {
    return setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&interface_address,
                      sizeof(interface_address)) == 0;
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
    /* Also listen on the other adapters (configure_socket joined the first one). */
    struct in_addr others[16];
    int count = zeroconf_platform_list_ipv4(others, NULL, 16);
    for (int i = 0; i < count; ++i) {
        if (others[i].s_addr == interface_address.s_addr) {
            continue;
        }
        struct ip_mreq multicast = {
            .imr_multiaddr.s_addr = htonl(MDNS_MULTICAST_ADDRESS),
            .imr_interface = others[i],
        };
        (void)setsockopt(socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&multicast,
                         sizeof(multicast));
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
