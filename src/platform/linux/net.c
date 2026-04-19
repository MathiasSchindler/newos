#include "platform.h"
#include "runtime.h"

int platform_ping_host(const char *host, const PlatformPingOptions *options) {
    (void)host;
    (void)options;
    rt_write_line(2, "ping: ICMP is not yet supported on this platform");
    return 1;
}

int platform_netcat(const char *host, unsigned int port, const PlatformNetcatOptions *options) {
    (void)host;
    (void)port;
    (void)options;
    return -1;
}

int platform_netcat_tcp(const char *host, unsigned int port, int listen_mode) {
    PlatformNetcatOptions options;

    options.listen_mode = listen_mode;
    options.use_udp = 0;
    options.scan_mode = 0;
    options.timeout_milliseconds = 0;
    return platform_netcat(host, port, &options);
}

int platform_connect_tcp(const char *host, unsigned int port, int *socket_fd_out) {
    (void)host;
    (void)port;
    (void)socket_fd_out;
    return -1;
}

int platform_list_network_links(PlatformNetworkLink *entries_out, size_t entry_capacity, size_t *count_out) {
    (void)entries_out;
    (void)entry_capacity;
    if (count_out != 0) {
        *count_out = 0U;
    }
    return -1;
}

int platform_list_network_addresses(
    PlatformNetworkAddress *entries_out,
    size_t entry_capacity,
    size_t *count_out,
    int family_filter,
    const char *ifname_filter
) {
    (void)entries_out;
    (void)entry_capacity;
    (void)family_filter;
    (void)ifname_filter;
    if (count_out != 0) {
        *count_out = 0U;
    }
    return -1;
}

int platform_list_network_routes(
    PlatformRouteEntry *entries_out,
    size_t entry_capacity,
    size_t *count_out,
    int family_filter,
    const char *ifname_filter
) {
    (void)entries_out;
    (void)entry_capacity;
    (void)family_filter;
    (void)ifname_filter;
    if (count_out != 0) {
        *count_out = 0U;
    }
    return -1;
}

int platform_network_link_set(const char *ifname, int want_up, unsigned int mtu_value, int set_mtu) {
    (void)ifname;
    (void)want_up;
    (void)mtu_value;
    (void)set_mtu;
    return -1;
}

int platform_network_address_change(const char *ifname, const char *cidr, int add) {
    (void)ifname;
    (void)cidr;
    (void)add;
    return -1;
}

int platform_network_route_change(const char *destination, const char *gateway, const char *ifname, int add) {
    (void)destination;
    (void)gateway;
    (void)ifname;
    (void)add;
    return -1;
}

int platform_poll_fds(const int *fds, size_t fd_count, size_t *ready_index_out, int timeout_milliseconds) {
    (void)fds;
    (void)fd_count;
    (void)ready_index_out;
    (void)timeout_milliseconds;
    return -1;
}
