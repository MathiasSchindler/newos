#include "platform.h"
#include "runtime.h"

int platform_ping_host(const char *host, const PlatformPingOptions *options) {
    (void)host;
    (void)options;
    rt_write_line(2, "ping: ICMP is not yet supported on this platform");
    return 1;
}

int platform_netcat_tcp(const char *host, unsigned int port, int listen_mode) {
    (void)host;
    (void)port;
    (void)listen_mode;
    return -1;
}
