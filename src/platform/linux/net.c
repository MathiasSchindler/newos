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

int platform_poll_fds(const int *fds, size_t fd_count, size_t *ready_index_out, int timeout_milliseconds) {
    (void)fds;
    (void)fd_count;
    (void)ready_index_out;
    (void)timeout_milliseconds;
    return -1;
}
