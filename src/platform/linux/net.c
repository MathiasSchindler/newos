#include "platform.h"

int platform_ping_host(const char *host, unsigned int count) {
    (void)host;
    (void)count;
    return -1;
}

int platform_netcat_tcp(const char *host, unsigned int port, int listen_mode) {
    (void)host;
    (void)port;
    (void)listen_mode;
    return -1;
}
