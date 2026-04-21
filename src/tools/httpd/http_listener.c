#include "httpd_impl.h"

int httpd_open_listener(const HttpServerOptions *options, int *listen_fd_out) {
    if (options == 0 || listen_fd_out == 0) {
        return -1;
    }
    return platform_open_tcp_listener(options->bind_host, options->port, listen_fd_out);
}
