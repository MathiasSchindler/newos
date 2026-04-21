#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/if_packet.h>
#endif

#ifdef __APPLE__
#include <net/if_dl.h>
#endif

#ifndef IP_TTL
#define IP_TTL 4
#endif
#ifndef IPPROTO_ICMPV6
#define IPPROTO_ICMPV6 58
#endif
#ifndef IPV6_UNICAST_HOPS
#define IPV6_UNICAST_HOPS 16
#endif

#define POSIX_ICMP_ECHO 8
#define POSIX_ICMP_REPLY 0
#define POSIX_ICMPV6_ECHO_REQUEST 128
#define POSIX_ICMPV6_ECHO_REPLY 129

typedef struct {
    unsigned char type;
    unsigned char code;
    unsigned short checksum;
    unsigned short identifier;
    unsigned short sequence;
} PosixIcmpPacket;

typedef struct {
    unsigned char type;
    unsigned char code;
    unsigned short checksum;
    unsigned short identifier;
    unsigned short sequence;
} PosixIcmpv6Packet;

#ifdef __APPLE__
static const unsigned char *posix_sockaddr_dl_addr(const struct sockaddr_dl *sdl) {
    if (sdl == NULL || sdl->sdl_alen <= 0 || sdl->sdl_nlen < 0) {
        return NULL;
    }
    return (const unsigned char *)(sdl->sdl_data + sdl->sdl_nlen);
}
#endif

static int posix_mark_fd_cloexec(int fd) {
    int flags;

    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }

    flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return -1;
    }
    if ((flags & FD_CLOEXEC) != 0) {
        return 0;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int posix_socket_finalize(int sock) {
#ifdef SO_NOSIGPIPE
    int enabled = 1;

    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        return -1;
    }
#endif
    return posix_mark_fd_cloexec(sock);
}

static int posix_socket_open(int family, int type, int protocol) {
    int sock;

#ifdef SOCK_CLOEXEC
    sock = socket(family, type | SOCK_CLOEXEC, protocol);
    if (sock >= 0) {
#ifdef SO_NOSIGPIPE
        int enabled = 1;

        if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
            int saved_errno = errno;
            close(sock);
            errno = saved_errno;
            return -1;
        }
#endif
        return sock;
    }
    if (errno != EINVAL) {
        return -1;
    }
#endif

    sock = socket(family, type, protocol);
    if (sock < 0) {
        return -1;
    }
    if (posix_socket_finalize(sock) != 0) {
        int saved_errno = errno;
        close(sock);
        errno = saved_errno;
        return -1;
    }
    return sock;
}

static int posix_socket_accept(int sock) {
    int client;

    client = accept(sock, NULL, NULL);
    if (client < 0) {
        return -1;
    }
    if (posix_socket_finalize(client) != 0) {
        int saved_errno = errno;
        close(client);
        errno = saved_errno;
        return -1;
    }
    return client;
}

static int posix_ifname_is_valid(const char *ifname) {
    size_t i;

    if (ifname == NULL || ifname[0] == '\0') {
        return 0;
    }
    if (rt_strlen(ifname) >= IFNAMSIZ) {
        return 0;
    }
    for (i = 0U; ifname[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)ifname[i];
        if (ch <= ' ' || ch == '/' || ch == '\\') {
            return 0;
        }
    }
    return 1;
}

static unsigned short compute_icmp_checksum(const void *data, size_t length) {
    const unsigned short *words = (const unsigned short *)data;
    unsigned int sum = 0;
    size_t remaining = length;

    while (remaining > 1) {
        sum += *words++;
        remaining -= 2;
    }

    if (remaining == 1) {
        sum += *(const unsigned char *)words;
    }

    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffU) + (sum >> 16);
    }

    return (unsigned short)~sum;
}

static int resolve_ping_host(const char *host, struct sockaddr_in *addr_out, char *ip_out, size_t ip_out_size) {
    struct addrinfo hints;
    struct addrinfo *results = 0;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    rc = getaddrinfo(host, 0, &hints, &results);
    if (rc != 0 || results == 0) {
        return -1;
    }

    memcpy(addr_out, results->ai_addr, sizeof(*addr_out));
    if (inet_ntop(AF_INET, &addr_out->sin_addr, ip_out, (socklen_t)ip_out_size) == 0) {
        freeaddrinfo(results);
        return -1;
    }

    freeaddrinfo(results);
    return 0;
}

static int resolve_ping_host6(const char *host, struct sockaddr_in6 *addr_out, char *ip_out, size_t ip_out_size) {
    struct addrinfo hints;
    struct addrinfo *results = 0;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;

    rc = getaddrinfo(host, 0, &hints, &results);
    if (rc != 0 || results == 0) {
        return -1;
    }

    memcpy(addr_out, results->ai_addr, sizeof(*addr_out));
    if (inet_ntop(AF_INET6, &addr_out->sin6_addr, ip_out, (socklen_t)ip_out_size) == 0) {
        freeaddrinfo(results);
        return -1;
    }

    freeaddrinfo(results);
    return 0;
}

static double elapsed_milliseconds(const struct timeval *start, const struct timeval *end) {
    long seconds = (long)(end->tv_sec - start->tv_sec);
    long usec = (long)(end->tv_usec - start->tv_usec);
    return (double)(seconds * 1000L) + (double)usec / 1000.0;
}

static void write_milliseconds(double milliseconds) {
    unsigned long long whole_ms = (unsigned long long)milliseconds;
    unsigned long long frac_ms = (unsigned long long)((milliseconds - (double)whole_ms) * 1000.0);

    rt_write_uint(1, whole_ms);
    rt_write_char(1, '.');
    if (frac_ms < 100ULL) {
        rt_write_char(1, '0');
    }
    if (frac_ms < 10ULL) {
        rt_write_char(1, '0');
    }
    rt_write_uint(1, frac_ms);
}

static int write_socket_all(int fd, const char *buffer, size_t count) {
    size_t offset = 0;

    while (offset < count) {
        ssize_t written = send(fd, buffer + offset, count - offset,
#ifdef MSG_NOSIGNAL
                               MSG_NOSIGNAL
#else
                               0
#endif
        );
        if (written <= 0) {
            return -1;
        }
        offset += (size_t)written;
    }

    return 0;
}

static int stream_fd_to_socket(int input_fd, int sock) {
    char buffer[4096];
    long bytes_read;

    while ((bytes_read = platform_read(input_fd, buffer, sizeof(buffer))) > 0) {
        if (write_socket_all(sock, buffer, (size_t)bytes_read) != 0) {
            return -1;
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int stream_socket_to_stdout(int sock) {
    char buffer[4096];
    ssize_t bytes_read;

    while ((bytes_read = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        if (rt_write_all(1, buffer, (size_t)bytes_read) != 0) {
            return -1;
        }
    }

    if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }

    return bytes_read < 0 ? -1 : 0;
}

static int set_socket_timeout(int sock, unsigned int timeout_milliseconds) {
    struct timeval timeout;

    if (timeout_milliseconds == 0U) {
        return 0;
    }

    timeout.tv_sec = (time_t)(timeout_milliseconds / 1000U);
    timeout.tv_usec = (suseconds_t)((timeout_milliseconds % 1000U) * 1000U);
    if (timeout.tv_sec == 0 && timeout.tv_usec == 0) {
        timeout.tv_usec = 1000;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        return -1;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        return -1;
    }
    return 0;
}

static int posix_netcat_ai_family(int family_filter) {
    if (family_filter == PLATFORM_NETWORK_FAMILY_IPV4) {
        return AF_INET;
    }
    if (family_filter == PLATFORM_NETWORK_FAMILY_IPV6) {
        return AF_INET6;
    }
    return AF_UNSPEC;
}

static int bind_socket_local_endpoint(
    int sock,
    int family,
    int socktype,
    int protocol,
    const PlatformNetcatOptions *options
) {
    struct addrinfo hints;
    struct addrinfo *results = 0;
    struct addrinfo *current;
    char port_text[16];
    const char *host = 0;

    if (options == NULL) {
        return 0;
    }
    if (options->bind_host[0] == '\0' && options->bind_port == 0U) {
        return 0;
    }

    host = options->bind_host[0] != '\0' ? options->bind_host : 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    hints.ai_protocol = protocol;
    hints.ai_flags = AI_PASSIVE;
#ifdef AI_NUMERICHOST
    if (options->numeric_only && host != 0) {
        hints.ai_flags |= AI_NUMERICHOST;
    }
#endif
    (void)snprintf(port_text, sizeof(port_text), "%u", options->bind_port);

    if (getaddrinfo(host, port_text, &hints, &results) != 0) {
        errno = EINVAL;
        return -1;
    }

    for (current = results; current != 0; current = current->ai_next) {
        if (bind(sock, current->ai_addr, current->ai_addrlen) == 0) {
            freeaddrinfo(results);
            return 0;
        }
    }

    freeaddrinfo(results);
    return -1;
}

int platform_connect_tcp(const char *host, unsigned int port, int *socket_fd_out) {
    struct addrinfo hints;
    struct addrinfo *results = 0;
    struct addrinfo *current;
    char port_text[16];
    int sock = -1;

    if (host == NULL || socket_fd_out == NULL || port == 0U || port > 65535U) {
        errno = EINVAL;
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    (void)snprintf(port_text, sizeof(port_text), "%u", port);

    if (getaddrinfo(host, port_text, &hints, &results) != 0) {
        errno = EINVAL;
        return -1;
    }

    for (current = results; current != 0; current = current->ai_next) {
        sock = posix_socket_open(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (connect(sock, current->ai_addr, current->ai_addrlen) == 0) {
            break;
        }
        close(sock);
        sock = -1;
    }

    freeaddrinfo(results);
    if (sock < 0) {
        return -1;
    }

    *socket_fd_out = sock;
    return 0;
}

int platform_poll_fds(const int *fds, size_t fd_count, size_t *ready_index_out, int timeout_milliseconds) {
    struct pollfd stack_fds[16];
    struct pollfd *poll_fds = stack_fds;
    int has_valid_fd = 0;
    size_t i;
    int rc;

    if (fds == NULL || fd_count == 0U || ready_index_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (fd_count > sizeof(stack_fds) / sizeof(stack_fds[0])) {
        poll_fds = (struct pollfd *)malloc(fd_count * sizeof(struct pollfd));
        if (poll_fds == NULL) {
            errno = ENOMEM;
            return -1;
        }
    }

    for (i = 0; i < fd_count; ++i) {
        poll_fds[i].fd = fds[i];
        poll_fds[i].events = (short)(POLLIN | POLLERR | POLLHUP | POLLOUT);
        poll_fds[i].revents = 0;
        if (fds[i] >= 0) {
            has_valid_fd = 1;
        }
    }

    if (!has_valid_fd) {
        if (poll_fds != stack_fds) {
            free(poll_fds);
        }
        errno = EINVAL;
        return -1;
    }

    rc = poll(poll_fds, (nfds_t)fd_count, timeout_milliseconds);
    if (rc <= 0) {
        if (poll_fds != stack_fds) {
            free(poll_fds);
        }
        return rc;
    }

    for (i = 0; i < fd_count; ++i) {
        if (poll_fds[i].fd >= 0 && (poll_fds[i].revents & (POLLIN | POLLERR | POLLHUP | POLLOUT)) != 0) {
            *ready_index_out = i;
            if (poll_fds != stack_fds) {
                free(poll_fds);
            }
            return 1;
        }
    }

    if (poll_fds != stack_fds) {
        free(poll_fds);
    }
    return 0;
}

int platform_netcat(const char *host, unsigned int port, const PlatformNetcatOptions *options) {
    PlatformNetcatOptions effective_options;
    int sock = -1;
    int ai_family;

    if (options == NULL) {
        memset(&effective_options, 0, sizeof(effective_options));
        options = &effective_options;
    }

    ai_family = posix_netcat_ai_family(options->family);

    if (options->listen_mode) {
        struct addrinfo hints;
        struct addrinfo *results = 0;
        struct addrinfo *current;
        char port_text[16];
        unsigned int listen_port = options->bind_port != 0U ? options->bind_port : port;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = ai_family;
        hints.ai_socktype = options->use_udp ? SOCK_DGRAM : SOCK_STREAM;
        hints.ai_protocol = options->use_udp ? IPPROTO_UDP : IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE;
#ifdef AI_NUMERICHOST
        if (options->numeric_only && options->bind_host[0] != '\0') {
            hints.ai_flags |= AI_NUMERICHOST;
        }
#endif
        (void)snprintf(port_text, sizeof(port_text), "%u", listen_port);

        if (getaddrinfo(options->bind_host[0] != '\0' ? options->bind_host : 0, port_text, &hints, &results) != 0) {
            errno = EINVAL;
            return -1;
        }

        for (current = results; current != 0; current = current->ai_next) {
            int reuse = 1;

            sock = posix_socket_open(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (sock < 0) {
                continue;
            }
            (void)set_socket_timeout(sock, options->timeout_milliseconds);
            (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            if (bind(sock, current->ai_addr, current->ai_addrlen) == 0) {
                break;
            }
            close(sock);
            sock = -1;
        }
        freeaddrinfo(results);
        if (sock < 0) {
            return -1;
        }

        if (options->use_udp) {
            char buffer[4096];
            ssize_t received = recvfrom(sock, buffer, sizeof(buffer), 0, NULL, NULL);

            if (received < 0) {
                close(sock);
                return -1;
            }
            if (!options->scan_mode && received > 0 && rt_write_all(1, buffer, (size_t)received) != 0) {
                close(sock);
                return -1;
            }
            close(sock);
            return 0;
        }

        if (listen(sock, 1) != 0) {
            close(sock);
            return -1;
        }

        {
            size_t ready_index = 0U;

            if (options->timeout_milliseconds > 0U &&
                platform_poll_fds(&sock, 1U, &ready_index, (int)options->timeout_milliseconds) <= 0) {
                close(sock);
                return -1;
            }

            int client = posix_socket_accept(sock);
            close(sock);
            if (client < 0) {
                return -1;
            }
            (void)set_socket_timeout(client, options->timeout_milliseconds);
            if (!options->scan_mode && stream_socket_to_stdout(client) != 0) {
                close(client);
                return -1;
            }
            close(client);
            return 0;
        }
    }

    if (host == NULL) {
        errno = EINVAL;
        return -1;
    }

    {
        struct addrinfo hints;
        struct addrinfo *results = 0;
        struct addrinfo *current;
        char port_text[16];
        int socktype = options->use_udp ? SOCK_DGRAM : SOCK_STREAM;
        int protocol = options->use_udp ? IPPROTO_UDP : IPPROTO_TCP;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = ai_family;
        hints.ai_socktype = socktype;
        hints.ai_protocol = protocol;
#ifdef AI_NUMERICHOST
        if (options->numeric_only) {
            hints.ai_flags |= AI_NUMERICHOST;
        }
#endif
        (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned int)port);

        if (getaddrinfo(host, port_text, &hints, &results) != 0) {
            errno = EINVAL;
            return -1;
        }

        for (current = results; current != 0; current = current->ai_next) {
            sock = posix_socket_open(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (sock < 0) {
                continue;
            }
            (void)set_socket_timeout(sock, options->timeout_milliseconds);
            if (bind_socket_local_endpoint(sock, current->ai_family, current->ai_socktype, current->ai_protocol, options) != 0) {
                close(sock);
                sock = -1;
                continue;
            }
            if (connect(sock, current->ai_addr, current->ai_addrlen) == 0) {
                break;
            }
            close(sock);
            sock = -1;
        }

        freeaddrinfo(results);
    }

    if (sock < 0) {
        return -1;
    }

    if (options->scan_mode) {
        close(sock);
        return 0;
    }

    if (!platform_isatty(0)) {
        if (stream_fd_to_socket(0, sock) != 0) {
            close(sock);
            return -1;
        }
        if (!options->use_udp) {
            (void)shutdown(sock, SHUT_WR);
        } else {
            close(sock);
            return 0;
        }
    }

    if (stream_socket_to_stdout(sock) != 0) {
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}

int platform_netcat_tcp(const char *host, unsigned int port, int listen_mode) {
    PlatformNetcatOptions options;

    memset(&options, 0, sizeof(options));
    options.listen_mode = listen_mode;
    return platform_netcat(host, port, &options);
}

static int posix_network_family_matches(unsigned short family, int family_filter) {
    if (family_filter == PLATFORM_NETWORK_FAMILY_ANY) {
        return family == AF_INET || family == AF_INET6;
    }
    if (family_filter == PLATFORM_NETWORK_FAMILY_IPV4) {
        return family == AF_INET;
    }
    if (family_filter == PLATFORM_NETWORK_FAMILY_IPV6) {
        return family == AF_INET6;
    }
    return 0;
}

static int posix_network_family_code(unsigned short family) {
    if (family == AF_INET) {
        return PLATFORM_NETWORK_FAMILY_IPV4;
    }
    if (family == AF_INET6) {
        return PLATFORM_NETWORK_FAMILY_IPV6;
    }
    return PLATFORM_NETWORK_FAMILY_ANY;
}

static void posix_format_mac_address(const unsigned char *bytes, size_t count, char *buffer, size_t buffer_size) {
    size_t offset = 0;
    size_t i;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';

    for (i = 0; i < count; ++i) {
        int written = snprintf(buffer + offset,
                               buffer_size - offset,
                               (i == 0) ? "%02x" : ":%02x",
                               (unsigned int)bytes[i]);
        if (written < 0 || (size_t)written >= buffer_size - offset) {
            break;
        }
        offset += (size_t)written;
    }
}

static int posix_format_address_text(const struct sockaddr *address, char *buffer, size_t buffer_size) {
    if (address == NULL || buffer == NULL || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }

    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)address;
        return inet_ntop(AF_INET, &sin->sin_addr, buffer, (socklen_t)buffer_size) != NULL ? 0 : -1;
    }
    if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)address;
        return inet_ntop(AF_INET6, &sin6->sin6_addr, buffer, (socklen_t)buffer_size) != NULL ? 0 : -1;
    }

    errno = EAFNOSUPPORT;
    return -1;
}

static unsigned int posix_count_mask_bits_v4(const struct sockaddr *mask) {
    const struct sockaddr_in *sin = (const struct sockaddr_in *)mask;
    unsigned int value;
    unsigned int count = 0;

    if (mask == NULL || mask->sa_family != AF_INET) {
        return 0;
    }

    value = ntohl(sin->sin_addr.s_addr);
    while ((value & 0x80000000U) != 0U) {
        count += 1U;
        value <<= 1U;
    }
    return count;
}

static unsigned int posix_count_mask_bits_v6(const struct sockaddr *mask) {
    const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)mask;
    unsigned int count = 0;
    size_t i;

    if (mask == NULL || mask->sa_family != AF_INET6) {
        return 0;
    }

    for (i = 0; i < sizeof(sin6->sin6_addr.s6_addr); ++i) {
        unsigned char byte = sin6->sin6_addr.s6_addr[i];
        unsigned int bit;

        for (bit = 0; bit < 8U; ++bit) {
            if ((byte & 0x80U) == 0U) {
                return count;
            }
            count += 1U;
            byte <<= 1U;
        }
    }

    return count;
}

static const char *posix_address_scope_name(const char *ifname, const struct sockaddr *address) {
    if (ifname != NULL && ifname[0] == 'l' && ifname[1] == 'o') {
        return "host";
    }

    if (address != NULL && address->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)address;
        unsigned int host_order = ntohl(sin->sin_addr.s_addr);

        if ((host_order & 0xff000000U) == 0x7f000000U) {
            return "host";
        }
        if ((host_order & 0xffff0000U) == 0xa9fe0000U) {
            return "link";
        }
        return "global";
    }

    if (address != NULL && address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)address;
        const unsigned char *bytes = sin6->sin6_addr.s6_addr;
        size_t i;
        int all_zero = 1;

        for (i = 0; i < 15U; ++i) {
            if (bytes[i] != 0U) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero && bytes[15] == 1U) {
            return "host";
        }
        if (bytes[0] == 0xfeU && (bytes[1] & 0xc0U) == 0x80U) {
            return "link";
        }
        return "global";
    }

    return "global";
}

#if defined(__linux__)
static unsigned int posix_route_prefix_from_mask(unsigned long value) {
    unsigned int count = 0;

    while ((value & 1UL) != 0UL) {
        count += 1U;
        value >>= 1U;
    }
    return count;
}
#endif

#if !defined(__linux__)
static unsigned int posix_count_prefix_bits_from_text(const char *text, int family) {
    char *end = NULL;
    unsigned long prefix;

    if (text == NULL || text[0] == '\0' || rt_strcmp(text, "default") == 0) {
        return 0U;
    }

    prefix = strtoul(text, &end, 10);
    if (end != NULL && *end == '\0') {
        if (family == PLATFORM_NETWORK_FAMILY_IPV4 && prefix <= 32UL) {
            return (unsigned int)prefix;
        }
        if (family == PLATFORM_NETWORK_FAMILY_IPV6 && prefix <= 128UL) {
            return (unsigned int)prefix;
        }
    }

    if (family == PLATFORM_NETWORK_FAMILY_IPV4) {
        struct in_addr address;
        unsigned int value;
        unsigned int count = 0U;

        if (inet_pton(AF_INET, text, &address) != 1) {
            return 0U;
        }
        value = ntohl(address.s_addr);
        while ((value & 0x80000000U) != 0U) {
            count += 1U;
            value <<= 1U;
        }
        return count;
    }

    if (family == PLATFORM_NETWORK_FAMILY_IPV6) {
        struct in6_addr address6;
        unsigned int count = 0U;
        size_t i;

        if (inet_pton(AF_INET6, text, &address6) != 1) {
            return 0U;
        }
        for (i = 0U; i < sizeof(address6.s6_addr); ++i) {
            unsigned char byte = address6.s6_addr[i];
            unsigned int bit;

            for (bit = 0U; bit < 8U; ++bit) {
                if ((byte & 0x80U) == 0U) {
                    return count;
                }
                count += 1U;
                byte <<= 1U;
            }
        }
        return count;
    }

    return 0U;
}

static int posix_route_flags_has_host(const char *flags) {
    size_t i = 0U;

    if (flags == NULL) {
        return 0;
    }
    while (flags[i] != '\0') {
        if (flags[i] == 'H') {
            return 1;
        }
        i += 1U;
    }
    return 0;
}

static void posix_parse_route_destination(const char *token, int family, const char *flags, PlatformRouteEntry *entry) {
    char destination[PLATFORM_NETWORK_TEXT_CAPACITY];
    char *slash;

    rt_copy_string(destination, sizeof(destination), token != NULL ? token : "");
    if (rt_strcmp(destination, "default") == 0) {
        entry->is_default = 1;
        rt_copy_string(entry->destination, sizeof(entry->destination), "default");
        entry->prefix_length = 0U;
        return;
    }

    slash = strchr(destination, '/');
    if (slash != NULL) {
        *slash = '\0';
        entry->prefix_length = posix_count_prefix_bits_from_text(slash + 1, family);
    } else if (posix_route_flags_has_host(flags)) {
        entry->prefix_length = (family == PLATFORM_NETWORK_FAMILY_IPV6) ? 128U : 32U;
    } else {
        entry->prefix_length = 0U;
    }

    rt_copy_string(entry->destination, sizeof(entry->destination), destination);
}

static FILE *posix_open_netstat_pipe(const char *family) {
    const char *selected_family = "inet";
    char command[64];
    int written;

    if (family != NULL && rt_strcmp(family, "inet6") == 0) {
        selected_family = "inet6";
    }

    written = snprintf(command, sizeof(command), "netstat -rn -f %s 2>/dev/null", selected_family);
    if (written < 0 || (size_t)written >= sizeof(command)) {
        errno = EINVAL;
        return NULL;
    }

    return popen(command, "r");
}

static int posix_parse_netstat_routes(
    FILE *file,
    int family,
    const char *ifname_filter,
    PlatformRouteEntry *entries_out,
    size_t entry_capacity,
    size_t *count_io
) {
    char line[512];
    int table_ready = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        char destination[PLATFORM_NETWORK_TEXT_CAPACITY];
        char gateway[PLATFORM_NETWORK_TEXT_CAPACITY];
        char flags[64];
        char ifname[PLATFORM_NAME_CAPACITY];
        PlatformRouteEntry *entry;

        if (!table_ready) {
            if (strstr(line, "Destination") != NULL && strstr(line, "Gateway") != NULL) {
                table_ready = 1;
            }
            continue;
        }
        if (line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        if (sscanf(line, "%63s %63s %63s %255s", destination, gateway, flags, ifname) < 4) {
            continue;
        }
        if (ifname_filter != NULL && rt_strcmp(ifname_filter, ifname) != 0) {
            continue;
        }
        if (*count_io >= entry_capacity) {
            break;
        }

        entry = &entries_out[*count_io];
        memset(entry, 0, sizeof(*entry));
        entry->family = family;
        rt_copy_string(entry->ifname, sizeof(entry->ifname), ifname);
        posix_parse_route_destination(destination, family, flags, entry);

        if (gateway[0] != '\0' && gateway[0] != '-' && strncmp(gateway, "link#", 5U) != 0) {
            rt_copy_string(entry->gateway, sizeof(entry->gateway), gateway);
            entry->has_gateway = 1;
        }

        *count_io += 1U;
    }

    return 0;
}
#endif

static int posix_find_link_index(PlatformNetworkLink *entries_out, size_t count, const char *name) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (rt_strcmp(entries_out[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static unsigned int posix_map_link_flags(unsigned int ifa_flags) {
    unsigned int flags = 0U;

#ifdef IFF_UP
    if ((ifa_flags & IFF_UP) != 0U) {
        flags |= PLATFORM_NETWORK_FLAG_UP;
    }
#endif
#ifdef IFF_BROADCAST
    if ((ifa_flags & IFF_BROADCAST) != 0U) {
        flags |= PLATFORM_NETWORK_FLAG_BROADCAST;
    }
#endif
#ifdef IFF_LOOPBACK
    if ((ifa_flags & IFF_LOOPBACK) != 0U) {
        flags |= PLATFORM_NETWORK_FLAG_LOOPBACK;
    }
#endif
#ifdef IFF_RUNNING
    if ((ifa_flags & IFF_RUNNING) != 0U) {
        flags |= PLATFORM_NETWORK_FLAG_RUNNING;
    }
#endif
#ifdef IFF_MULTICAST
    if ((ifa_flags & IFF_MULTICAST) != 0U) {
        flags |= PLATFORM_NETWORK_FLAG_MULTICAST;
    }
#endif

    return flags;
}

int platform_list_network_links(PlatformNetworkLink *entries_out, size_t entry_capacity, size_t *count_out) {
    struct ifaddrs *entries = NULL;
    struct ifaddrs *current;
    int sock = -1;
    size_t count = 0;

    if (entries_out == NULL || count_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    *count_out = 0;
    if (getifaddrs(&entries) != 0) {
        return -1;
    }

    sock = posix_socket_open(AF_INET, SOCK_DGRAM, 0);
    for (current = entries; current != NULL; current = current->ifa_next) {
        int index;

        if (current->ifa_name == NULL || current->ifa_name[0] == '\0') {
            continue;
        }

        index = posix_find_link_index(entries_out, count, current->ifa_name);
        if (index < 0) {
            struct ifreq ifr;

            if (count >= entry_capacity) {
                break;
            }

            index = (int)count;
            memset(&entries_out[index], 0, sizeof(entries_out[index]));
            rt_copy_string(entries_out[index].name, sizeof(entries_out[index].name), current->ifa_name);
            entries_out[index].index = if_nametoindex(current->ifa_name);
            entries_out[index].flags = posix_map_link_flags((unsigned int)current->ifa_flags);
            entries_out[index].mtu = 1500U;

            if (sock >= 0) {
                memset(&ifr, 0, sizeof(ifr));
                rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), current->ifa_name);
                if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
                    entries_out[index].flags = posix_map_link_flags((unsigned int)ifr.ifr_flags);
                }
#ifdef SIOCGIFMTU
                if (ioctl(sock, SIOCGIFMTU, &ifr) == 0 && ifr.ifr_mtu > 0) {
                    entries_out[index].mtu = (unsigned int)ifr.ifr_mtu;
                }
#endif
            }
            count += 1U;
        }

#ifdef __linux__
        if (current->ifa_addr != NULL && current->ifa_addr->sa_family == AF_PACKET && !entries_out[index].has_mac) {
            const struct sockaddr_ll *sll = (const struct sockaddr_ll *)current->ifa_addr;
            if (sll->sll_halen > 0U) {
                posix_format_mac_address((const unsigned char *)sll->sll_addr,
                                         (size_t)sll->sll_halen,
                                         entries_out[index].mac,
                                         sizeof(entries_out[index].mac));
                entries_out[index].has_mac = 1;
            }
        }
#endif

#ifdef __APPLE__
        if (current->ifa_addr != NULL && current->ifa_addr->sa_family == AF_LINK && !entries_out[index].has_mac) {
            const struct sockaddr_dl *sdl = (const struct sockaddr_dl *)current->ifa_addr;
            if (sdl->sdl_alen > 0) {
                const unsigned char *addr = posix_sockaddr_dl_addr(sdl);
                if (addr != NULL) {
                    posix_format_mac_address(addr, (size_t)sdl->sdl_alen, entries_out[index].mac, sizeof(entries_out[index].mac));
                    entries_out[index].has_mac = 1;
                }
            }
        }
#endif
    }

    if (sock >= 0) {
        (void)close(sock);
    }
    freeifaddrs(entries);
    *count_out = count;
    return 0;
}

int platform_list_network_addresses(
    PlatformNetworkAddress *entries_out,
    size_t entry_capacity,
    size_t *count_out,
    int family_filter,
    const char *ifname_filter
) {
    struct ifaddrs *entries = NULL;
    struct ifaddrs *current;
    size_t count = 0;

    if (entries_out == NULL || count_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    *count_out = 0;
    if (getifaddrs(&entries) != 0) {
        return -1;
    }

    for (current = entries; current != NULL; current = current->ifa_next) {
        PlatformNetworkAddress *entry;

        if (current->ifa_name == NULL || current->ifa_addr == NULL) {
            continue;
        }
        if (ifname_filter != NULL && rt_strcmp(current->ifa_name, ifname_filter) != 0) {
            continue;
        }
        if (!posix_network_family_matches(current->ifa_addr->sa_family, family_filter)) {
            continue;
        }
        if (count >= entry_capacity) {
            break;
        }

        entry = &entries_out[count];
        memset(entry, 0, sizeof(*entry));
        rt_copy_string(entry->ifname, sizeof(entry->ifname), current->ifa_name);
        entry->family = posix_network_family_code(current->ifa_addr->sa_family);
        if (posix_format_address_text(current->ifa_addr, entry->address, sizeof(entry->address)) != 0) {
            continue;
        }

        if (current->ifa_addr->sa_family == AF_INET) {
            entry->prefix_length = posix_count_mask_bits_v4(current->ifa_netmask);
            if (current->ifa_broadaddr != NULL &&
#ifdef IFF_BROADCAST
                (current->ifa_flags & IFF_BROADCAST) != 0 &&
#endif
                posix_format_address_text(current->ifa_broadaddr, entry->broadcast, sizeof(entry->broadcast)) == 0) {
                entry->has_broadcast = 1;
            }
        } else {
            entry->prefix_length = posix_count_mask_bits_v6(current->ifa_netmask);
        }

        rt_copy_string(entry->scope, sizeof(entry->scope), posix_address_scope_name(current->ifa_name, current->ifa_addr));
        count += 1U;
    }

    freeifaddrs(entries);
    *count_out = count;
    return 0;
}

int platform_list_network_routes(
    PlatformRouteEntry *entries_out,
    size_t entry_capacity,
    size_t *count_out,
    int family_filter,
    const char *ifname_filter
) {
    if (entries_out == NULL || count_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    *count_out = 0;
#if !defined(__linux__)
    {
        size_t count = 0U;
        int any_success = 0;

        if (family_filter != PLATFORM_NETWORK_FAMILY_IPV6) {
            FILE *file = posix_open_netstat_pipe("inet");
            if (file != NULL) {
                any_success = 1;
                if (posix_parse_netstat_routes(file,
                                               PLATFORM_NETWORK_FAMILY_IPV4,
                                               ifname_filter,
                                               entries_out,
                                               entry_capacity,
                                               &count) != 0) {
                    (void)pclose(file);
                    return -1;
                }
                (void)pclose(file);
            }
        }

        if (family_filter != PLATFORM_NETWORK_FAMILY_IPV4 && count < entry_capacity) {
            FILE *file = posix_open_netstat_pipe("inet6");
            if (file != NULL) {
                any_success = 1;
                if (posix_parse_netstat_routes(file,
                                               PLATFORM_NETWORK_FAMILY_IPV6,
                                               ifname_filter,
                                               entries_out,
                                               entry_capacity,
                                               &count) != 0) {
                    (void)pclose(file);
                    return -1;
                }
                (void)pclose(file);
            }
        }

        if (!any_success) {
            if (family_filter == PLATFORM_NETWORK_FAMILY_IPV6) {
                *count_out = 0U;
                return 0;
            }
            errno = ENOTSUP;
            return -1;
        }

        *count_out = count;
        return 0;
    }
#else
    FILE *file;
    char line[512];
    int header_seen = 0;
    size_t count = 0;
    if (family_filter == PLATFORM_NETWORK_FAMILY_IPV6) {
        *count_out = 0U;
        return 0;
    }

    file = fopen("/proc/net/route", "r");
    if (file == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char ifname[PLATFORM_NAME_CAPACITY];
        unsigned long destination = 0UL;
        unsigned long gateway = 0UL;
        unsigned long flags = 0UL;
        unsigned long mask = 0UL;
        unsigned long metric = 0UL;
        PlatformRouteEntry *entry;
        struct in_addr address;

        if (!header_seen) {
            header_seen = 1;
            continue;
        }

        if (sscanf(line,
                   "%255s %lx %lx %lx %*u %*u %lu %lx",
                   ifname,
                   &destination,
                   &gateway,
                   &flags,
                   &metric,
                   &mask) < 6) {
            continue;
        }
        if ((flags & RTF_UP) == 0UL) {
            continue;
        }
        if (ifname_filter != NULL && rt_strcmp(ifname_filter, ifname) != 0) {
            continue;
        }
        if (count >= entry_capacity) {
            break;
        }

        entry = &entries_out[count];
        memset(entry, 0, sizeof(*entry));
        entry->family = PLATFORM_NETWORK_FAMILY_IPV4;
        entry->metric = (unsigned int)metric;
        rt_copy_string(entry->ifname, sizeof(entry->ifname), ifname);

        if (destination == 0UL && mask == 0UL) {
            entry->is_default = 1;
            rt_copy_string(entry->destination, sizeof(entry->destination), "default");
        } else {
            address.s_addr = (in_addr_t)destination;
            if (inet_ntop(AF_INET, &address, entry->destination, sizeof(entry->destination)) == NULL) {
                continue;
            }
        }
        entry->prefix_length = posix_route_prefix_from_mask(mask);

        if (gateway != 0UL) {
            address.s_addr = (in_addr_t)gateway;
            if (inet_ntop(AF_INET, &address, entry->gateway, sizeof(entry->gateway)) != NULL) {
                entry->has_gateway = 1;
            }
        }

        count += 1U;
    }

    (void)fclose(file);
    *count_out = count;
    return 0;
#endif
}

static int posix_parse_ipv4_cidr(const char *text, struct in_addr *address_out, struct in_addr *mask_out, unsigned int *prefix_out) {
    char copy[64];
    char *slash;
    unsigned long prefix = 32UL;
    unsigned int mask_host;

    if (text == NULL || address_out == NULL || mask_out == NULL || prefix_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (rt_strlen(text) >= sizeof(copy)) {
        errno = EINVAL;
        return -1;
    }

    rt_copy_string(copy, sizeof(copy), text);
    slash = strchr(copy, '/');
    if (slash != NULL) {
        *slash = '\0';
        if (*(slash + 1) == '\0') {
            errno = EINVAL;
            return -1;
        }
        prefix = strtoul(slash + 1, NULL, 10);
        if (prefix > 32UL) {
            errno = EINVAL;
            return -1;
        }
    }

    if (inet_pton(AF_INET, copy, address_out) != 1) {
        errno = EINVAL;
        return -1;
    }

    if (prefix == 0UL) {
        mask_host = 0U;
    } else {
        mask_host = 0xffffffffU << (32U - (unsigned int)prefix);
    }
    mask_out->s_addr = htonl(mask_host);
    *prefix_out = (unsigned int)prefix;
    return 0;
}

int platform_network_link_set(const char *ifname, int want_up, unsigned int mtu_value, int set_mtu) {
    int sock;
    struct ifreq ifr;

    if (!posix_ifname_is_valid(ifname)) {
        errno = EINVAL;
        return -1;
    }

    sock = posix_socket_open(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
        (void)close(sock);
        return -1;
    }

    if (want_up >= 0) {
#ifdef IFF_UP
        if (want_up != 0) {
            ifr.ifr_flags = (short)(ifr.ifr_flags | IFF_UP);
        } else {
            ifr.ifr_flags = (short)(ifr.ifr_flags & ~IFF_UP);
        }
#endif
        if (ioctl(sock, SIOCSIFFLAGS, &ifr) != 0) {
            (void)close(sock);
            return -1;
        }
    }

#ifdef SIOCSIFMTU
    if (set_mtu != 0) {
        memset(&ifr, 0, sizeof(ifr));
        rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
        ifr.ifr_mtu = (int)mtu_value;
        if (ioctl(sock, SIOCSIFMTU, &ifr) != 0) {
            (void)close(sock);
            return -1;
        }
    }
#else
    (void)mtu_value;
    (void)set_mtu;
#endif

    (void)close(sock);
    return 0;
}

int platform_network_address_change(const char *ifname, const char *cidr, int add) {
    struct in_addr address;
    struct in_addr mask;
    unsigned int prefix = 0U;

    if (!posix_ifname_is_valid(ifname) || cidr == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (posix_parse_ipv4_cidr(cidr, &address, &mask, &prefix) != 0) {
        return -1;
    }
    (void)prefix;

#if defined(__linux__)
    {
        int sock;
        struct ifreq ifr;
        struct in_addr zero_address;

        zero_address.s_addr = 0U;
        sock = posix_socket_open(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            return -1;
        }

        memset(&ifr, 0, sizeof(ifr));
        rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
        ((struct sockaddr_in *)&ifr.ifr_addr)->sin_family = AF_INET;
        ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr = add ? address : zero_address;
        if (ioctl(sock, SIOCSIFADDR, &ifr) != 0) {
            (void)close(sock);
            return -1;
        }

#ifdef SIOCSIFNETMASK
        memset(&ifr, 0, sizeof(ifr));
        rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
        ((struct sockaddr_in *)&ifr.ifr_netmask)->sin_family = AF_INET;
        ((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr = add ? mask : zero_address;
        if (ioctl(sock, SIOCSIFNETMASK, &ifr) != 0) {
            (void)close(sock);
            return -1;
        }
#endif

        (void)close(sock);
        return 0;
    }
#elif defined(SIOCAIFADDR) && defined(SIOCDIFADDR)
    {
        int sock;
        struct ifaliasreq ifra;
        struct sockaddr_in *addr_ptr;
        struct sockaddr_in *mask_ptr;

        sock = posix_socket_open(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            return -1;
        }

        memset(&ifra, 0, sizeof(ifra));
        rt_copy_string(ifra.ifra_name, sizeof(ifra.ifra_name), ifname);

        addr_ptr = (struct sockaddr_in *)&ifra.ifra_addr;
        addr_ptr->sin_family = AF_INET;
#ifdef __APPLE__
        addr_ptr->sin_len = sizeof(*addr_ptr);
#endif
        addr_ptr->sin_addr = address;

        mask_ptr = (struct sockaddr_in *)&ifra.ifra_mask;
        mask_ptr->sin_family = AF_INET;
#ifdef __APPLE__
        mask_ptr->sin_len = sizeof(*mask_ptr);
#endif
        mask_ptr->sin_addr = mask;

        if (ioctl(sock, add ? SIOCAIFADDR : SIOCDIFADDR, &ifra) != 0) {
            (void)close(sock);
            return -1;
        }

        (void)close(sock);
        return 0;
    }
#else
    (void)add;
    errno = ENOTSUP;
    return -1;
#endif
}

int platform_network_route_change(const char *destination, const char *gateway, const char *ifname, int add) {
#if defined(SIOCADDRT) && defined(SIOCDELRT)
    struct rtentry route_entry;
    struct sockaddr_in destination_addr;
    struct sockaddr_in gateway_addr;
    struct sockaddr_in mask_addr;
    struct in_addr parsed_destination;
    struct in_addr parsed_mask;
    unsigned int prefix = 0U;
    int sock;

    if (destination == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ifname != NULL && ifname[0] != '\0' && !posix_ifname_is_valid(ifname)) {
        errno = EINVAL;
        return -1;
    }

    memset(&destination_addr, 0, sizeof(destination_addr));
    memset(&gateway_addr, 0, sizeof(gateway_addr));
    memset(&mask_addr, 0, sizeof(mask_addr));
    destination_addr.sin_family = AF_INET;
    gateway_addr.sin_family = AF_INET;
    mask_addr.sin_family = AF_INET;

    if (rt_strcmp(destination, "default") == 0) {
        parsed_destination.s_addr = 0U;
        parsed_mask.s_addr = 0U;
    } else if (posix_parse_ipv4_cidr(destination, &parsed_destination, &parsed_mask, &prefix) != 0) {
        return -1;
    }

    destination_addr.sin_addr = parsed_destination;
    mask_addr.sin_addr = parsed_mask;

    if (gateway != NULL && inet_pton(AF_INET, gateway, &gateway_addr.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }

    memset(&route_entry, 0, sizeof(route_entry));
    memcpy(&route_entry.rt_dst, &destination_addr, sizeof(destination_addr));
    memcpy(&route_entry.rt_gateway, &gateway_addr, sizeof(gateway_addr));
    memcpy(&route_entry.rt_genmask, &mask_addr, sizeof(mask_addr));
    route_entry.rt_flags = RTF_UP;
    if (gateway != NULL) {
        route_entry.rt_flags |= RTF_GATEWAY;
    }
    if (prefix == 32U) {
        route_entry.rt_flags |= RTF_HOST;
    }
#ifdef __linux__
    route_entry.rt_dev = (char *)ifname;
#else
    (void)ifname;
#endif

    sock = posix_socket_open(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }
    if (ioctl(sock, add ? SIOCADDRT : SIOCDELRT, &route_entry) != 0) {
        (void)close(sock);
        return -1;
    }

    (void)close(sock);
    return 0;
#else
    (void)destination;
    (void)gateway;
    (void)ifname;
    (void)add;
    errno = ENOTSUP;
    return -1;
#endif
}

static int posix_add_dns_entry(
    PlatformDnsEntry *entries_out,
    size_t entry_capacity,
    size_t *count_io,
    const char *name,
    int family,
    const char *address
) {
    PlatformDnsEntry *entry;
    size_t i;

    if (entries_out == NULL || count_io == NULL || *count_io >= entry_capacity) {
        return -1;
    }
    for (i = 0U; i < *count_io; ++i) {
        if (entries_out[i].family == family && rt_strcmp(entries_out[i].address, address) == 0) {
            return 0;
        }
    }
    entry = &entries_out[*count_io];
    memset(entry, 0, sizeof(*entry));
    entry->family = family;
    rt_copy_string(entry->name, sizeof(entry->name), name);
    rt_copy_string(entry->address, sizeof(entry->address), address);
    *count_io += 1U;
    return 0;
}

int platform_dns_lookup(
    const char *server,
    unsigned int port,
    const char *name,
    int family_filter,
    PlatformDnsEntry *entries_out,
    size_t entry_capacity,
    size_t *count_out
) {
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *current;
    int family = AF_UNSPEC;
    size_t count = 0U;
    char port_text[16];

    if (name == NULL || entries_out == NULL || count_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *count_out = 0U;

    if (family_filter == PLATFORM_NETWORK_FAMILY_IPV4) {
        family = AF_INET;
    } else if (family_filter == PLATFORM_NETWORK_FAMILY_IPV6) {
        family = AF_INET6;
    }

    if (server != NULL && server[0] != '\0') {
        int sock;
        struct sockaddr_in server_addr;
        unsigned char query[512];
        unsigned char reply[512];
        long reply_length;
        size_t used = 12U;
        unsigned short query_id = (unsigned short)(((unsigned int)platform_get_process_id() & 0xffffU) ^ 0x5a5aU);
        size_t offset;
        unsigned int answer_count;
        unsigned int i;

        memset(query, 0, sizeof(query));
        query[0] = (unsigned char)(query_id >> 8);
        query[1] = (unsigned char)(query_id & 0xffU);
        query[2] = 0x01U;
        query[5] = 0x01U;

        {
            const char *label = name;
            while (*label != '\0') {
                const char *end = label;
                size_t length;
                while (*end != '\0' && *end != '.') {
                    end += 1;
                }
                length = (size_t)(end - label);
                if (length == 0U || length > 63U || used + length + 2U >= sizeof(query)) {
                    errno = EINVAL;
                    return -1;
                }
                query[used++] = (unsigned char)length;
                memcpy(query + used, label, length);
                used += length;
                label = (*end == '.') ? (end + 1) : end;
            }
            query[used++] = 0U;
            query[used++] = 0x00U;
            query[used++] = (unsigned char)(family_filter == PLATFORM_NETWORK_FAMILY_IPV6 ? 28U : 1U);
            query[used++] = 0x00U;
            query[used++] = 0x01U;
        }

        sock = posix_socket_open(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            return -1;
        }
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons((unsigned short)(port == 0U ? 53U : port));
        if (inet_pton(AF_INET, server, &server_addr.sin_addr) != 1) {
            close(sock);
            errno = EINVAL;
            return -1;
        }
        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0 || send(sock, query, used, 0) < 0) {
            close(sock);
            return -1;
        }
        reply_length = recv(sock, reply, sizeof(reply), 0);
        if (reply_length < 12) {
            close(sock);
            return -1;
        }
        close(sock);

        answer_count = ((unsigned int)reply[6] << 8) | (unsigned int)reply[7];
        offset = 12U;
        while (offset < (size_t)reply_length && reply[offset] != 0U) {
            if ((reply[offset] & 0xc0U) == 0xc0U) {
                offset += 2U;
                break;
            }
            offset += 1U + (size_t)reply[offset];
        }
        offset += 5U;

        for (i = 0U; i < answer_count && offset + 12U <= (size_t)reply_length; ++i) {
            unsigned short type;
            unsigned short rdlength;
            if ((reply[offset] & 0xc0U) == 0xc0U) {
                offset += 2U;
            } else {
                while (offset < (size_t)reply_length && reply[offset] != 0U) {
                    offset += 1U + (size_t)reply[offset];
                }
                offset += 1U;
            }
            if (offset + 10U > (size_t)reply_length) {
                break;
            }
            type = (unsigned short)(((unsigned int)reply[offset] << 8) | (unsigned int)reply[offset + 1U]);
            rdlength = (unsigned short)(((unsigned int)reply[offset + 8U] << 8) | (unsigned int)reply[offset + 9U]);
            offset += 10U;
            if (offset + rdlength > (size_t)reply_length) {
                break;
            }
            if (type == 1U && rdlength == 4U) {
                char address[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, reply + offset, address, sizeof(address)) != NULL) {
                    (void)posix_add_dns_entry(entries_out, entry_capacity, &count, name, PLATFORM_NETWORK_FAMILY_IPV4, address);
                }
            } else if (type == 28U && rdlength == 16U) {
                char address[INET6_ADDRSTRLEN];
                if (inet_ntop(AF_INET6, reply + offset, address, sizeof(address)) != NULL) {
                    (void)posix_add_dns_entry(entries_out, entry_capacity, &count, name, PLATFORM_NETWORK_FAMILY_IPV6, address);
                }
            }
            offset += rdlength;
        }

        *count_out = count;
        return count > 0U ? 0 : -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_DGRAM;
    (void)snprintf(port_text, sizeof(port_text), "%u", 0U);

    if (getaddrinfo(name, port_text, &hints, &results) != 0 || results == NULL) {
        errno = EINVAL;
        return -1;
    }

    for (current = results; current != NULL; current = current->ai_next) {
        char address[INET6_ADDRSTRLEN];
        int result_family = PLATFORM_NETWORK_FAMILY_ANY;
        const void *addr_ptr = NULL;

        if (current->ai_family == AF_INET) {
            const struct sockaddr_in *sin = (const struct sockaddr_in *)current->ai_addr;
            result_family = PLATFORM_NETWORK_FAMILY_IPV4;
            addr_ptr = &sin->sin_addr;
        } else if (current->ai_family == AF_INET6) {
            const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)current->ai_addr;
            result_family = PLATFORM_NETWORK_FAMILY_IPV6;
            addr_ptr = &sin6->sin6_addr;
        } else {
            continue;
        }

        if ((family_filter == PLATFORM_NETWORK_FAMILY_IPV4 && result_family != PLATFORM_NETWORK_FAMILY_IPV4) ||
            (family_filter == PLATFORM_NETWORK_FAMILY_IPV6 && result_family != PLATFORM_NETWORK_FAMILY_IPV6)) {
            continue;
        }

        if (inet_ntop(current->ai_family, addr_ptr, address, sizeof(address)) != NULL) {
            (void)posix_add_dns_entry(entries_out, entry_capacity, &count, name, result_family, address);
        }
    }

    freeaddrinfo(results);
    *count_out = count;
    return count > 0U ? 0 : -1;
}

static int posix_select_mac_address(const char *ifname, unsigned char mac_out[6]) {
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;

    if (mac_out == NULL) {
        return -1;
    }
    if (getifaddrs(&ifaddr) != 0) {
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
            continue;
        }
        if (ifname != NULL && ifname[0] != '\0' && rt_strcmp(ifname, ifa->ifa_name) != 0) {
            continue;
        }
#ifdef __linux__
        if (ifa->ifa_addr->sa_family == AF_PACKET) {
            const struct sockaddr_ll *sll = (const struct sockaddr_ll *)ifa->ifa_addr;
            if ((ifa->ifa_flags & IFF_LOOPBACK) != 0 || sll->sll_halen < 6) {
                continue;
            }
            memcpy(mac_out, sll->sll_addr, 6U);
            freeifaddrs(ifaddr);
            return 0;
        }
#endif
#ifdef __APPLE__
        if (ifa->ifa_addr->sa_family == AF_LINK) {
            const struct sockaddr_dl *sdl = (const struct sockaddr_dl *)ifa->ifa_addr;
            const unsigned char *addr = posix_sockaddr_dl_addr(sdl);
            if ((ifa->ifa_flags & IFF_LOOPBACK) != 0 || addr == NULL || sdl->sdl_alen < 6) {
                continue;
            }
            memcpy(mac_out, addr, 6U);
            freeifaddrs(ifaddr);
            return 0;
        }
#endif
    }

    freeifaddrs(ifaddr);
    return -1;
}

static void posix_store_be16(unsigned char *buffer, unsigned short value) {
    buffer[0] = (unsigned char)(value >> 8);
    buffer[1] = (unsigned char)(value & 0xffU);
}

static void posix_store_be32(unsigned char *buffer, unsigned int value) {
    buffer[0] = (unsigned char)(value >> 24);
    buffer[1] = (unsigned char)((value >> 16) & 0xffU);
    buffer[2] = (unsigned char)((value >> 8) & 0xffU);
    buffer[3] = (unsigned char)(value & 0xffU);
}

static unsigned int posix_load_be32(const unsigned char *buffer) {
    return ((unsigned int)buffer[0] << 24) |
           ((unsigned int)buffer[1] << 16) |
           ((unsigned int)buffer[2] << 8) |
           (unsigned int)buffer[3];
}

static int posix_build_dhcp_packet(
    unsigned char *packet,
    size_t packet_size,
    unsigned int xid,
    const unsigned char mac[6],
    unsigned char message_type,
    const unsigned char *requested_ip,
    const unsigned char *server_id
) {
    size_t offset = 240U;
    static const unsigned char param_request[] = { 1U, 3U, 6U, 15U, 51U, 54U };

    if (packet == NULL || packet_size < 300U || mac == NULL) {
        return -1;
    }
    memset(packet, 0, packet_size);
    packet[0] = 1U;
    packet[1] = 1U;
    packet[2] = 6U;
    posix_store_be32(packet + 4, xid);
    posix_store_be16(packet + 10, 0x8000U);
    memcpy(packet + 28, mac, 6U);
    posix_store_be32(packet + 236, 0x63825363U);

    packet[offset++] = 53U;
    packet[offset++] = 1U;
    packet[offset++] = message_type;

    packet[offset++] = 61U;
    packet[offset++] = 7U;
    packet[offset++] = 1U;
    memcpy(packet + offset, mac, 6U);
    offset += 6U;

    if (requested_ip != NULL) {
        packet[offset++] = 50U;
        packet[offset++] = 4U;
        memcpy(packet + offset, requested_ip, 4U);
        offset += 4U;
    }
    if (server_id != NULL) {
        packet[offset++] = 54U;
        packet[offset++] = 4U;
        memcpy(packet + offset, server_id, 4U);
        offset += 4U;
    }

    packet[offset++] = 55U;
    packet[offset++] = (unsigned char)sizeof(param_request);
    memcpy(packet + offset, param_request, sizeof(param_request));
    offset += sizeof(param_request);
    packet[offset++] = 255U;
    return (int)offset;
}

static int posix_mask_to_prefix(const unsigned char *mask) {
    int prefix = 0;
    int i;
    int bit;

    for (i = 0; i < 4; ++i) {
        for (bit = 7; bit >= 0; --bit) {
            if ((mask[i] & (1U << bit)) != 0U) {
                prefix += 1;
            } else {
                return prefix;
            }
        }
    }
    return prefix;
}

static int posix_parse_dhcp_reply(
    const unsigned char *packet,
    size_t packet_length,
    unsigned int xid,
    const unsigned char mac[6],
    unsigned char expected_message_type,
    PlatformDhcpLease *lease_out
) {
    size_t offset = 240U;
    unsigned char message_type = 0U;
    struct in_addr addr;

    if (packet == NULL || lease_out == NULL || packet_length < 240U || mac == NULL) {
        return -1;
    }
    if (packet[0] != 2U || posix_load_be32(packet + 4) != xid || memcmp(packet + 28, mac, 6U) != 0 ||
        posix_load_be32(packet + 236) != 0x63825363U) {
        return -1;
    }

    memcpy(&addr, packet + 16, 4U);
    if (inet_ntop(AF_INET, &addr, lease_out->address, sizeof(lease_out->address)) == NULL) {
        return -1;
    }

    while (offset < packet_length) {
        unsigned char option = packet[offset++];
        unsigned char length;

        if (option == 0U) {
            continue;
        }
        if (option == 255U) {
            break;
        }
        if (offset >= packet_length) {
            break;
        }
        length = packet[offset++];
        if (offset + length > packet_length) {
            break;
        }

        if (option == 53U && length >= 1U) {
            message_type = packet[offset];
        } else if (option == 1U && length == 4U) {
            lease_out->prefix_length = (unsigned int)posix_mask_to_prefix(packet + offset);
        } else if (option == 3U && length >= 4U) {
            memcpy(&addr, packet + offset, 4U);
            (void)inet_ntop(AF_INET, &addr, lease_out->router, sizeof(lease_out->router));
        } else if (option == 6U && length >= 4U) {
            memcpy(&addr, packet + offset, 4U);
            (void)inet_ntop(AF_INET, &addr, lease_out->dns1, sizeof(lease_out->dns1));
            if (length >= 8U) {
                memcpy(&addr, packet + offset + 4U, 4U);
                (void)inet_ntop(AF_INET, &addr, lease_out->dns2, sizeof(lease_out->dns2));
            }
        } else if (option == 51U && length == 4U) {
            lease_out->lease_seconds = posix_load_be32(packet + offset);
        } else if (option == 54U && length == 4U) {
            memcpy(&addr, packet + offset, 4U);
            (void)inet_ntop(AF_INET, &addr, lease_out->server, sizeof(lease_out->server));
        }

        offset += length;
    }

    if (message_type != expected_message_type) {
        return -1;
    }
    if (lease_out->prefix_length == 0U) {
        lease_out->prefix_length = 24U;
    }
    return 0;
}

int platform_dhcp_request(
    const char *ifname,
    const char *server,
    unsigned int server_port,
    unsigned int client_port,
    unsigned int timeout_milliseconds,
    PlatformDhcpLease *lease_out
) {
    unsigned char mac[6];
    unsigned char packet[512];
    unsigned char reply[512];
    struct sockaddr_in server_addr;
    struct sockaddr_in peer_addr;
    struct sockaddr_in bind_addr;
    socklen_t peer_len = sizeof(peer_addr);
    int sock;
    int packet_length;
    unsigned int xid;
    int broadcast = 1;
    struct timeval timeout;
    unsigned char requested_ip[4];
    unsigned char server_id[4];
    int have_server_id = 0;

    if (lease_out == NULL ||
        ((ifname != NULL && ifname[0] != '\0') && !posix_ifname_is_valid(ifname)) ||
        posix_select_mac_address(ifname, mac) != 0) {
        return -1;
    }
    memset(lease_out, 0, sizeof(*lease_out));

    sock = posix_socket_open(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -1;
    }

    timeout.tv_sec = (time_t)((timeout_milliseconds == 0U ? 3000U : timeout_milliseconds) / 1000U);
    timeout.tv_usec = (suseconds_t)(((timeout_milliseconds == 0U ? 3000U : timeout_milliseconds) % 1000U) * 1000U);
    if (timeout.tv_sec == 0 && timeout.tv_usec == 0) {
        timeout.tv_usec = 1000;
    }
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &broadcast, sizeof(broadcast));
    (void)setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons((unsigned short)(client_port == 0U ? 68U : client_port));
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        close(sock);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((unsigned short)(server_port == 0U ? 67U : server_port));
    if (server == NULL || server[0] == '\0') {
        server_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    } else if (inet_pton(AF_INET, server, &server_addr.sin_addr) != 1) {
        close(sock);
        errno = EINVAL;
        return -1;
    }

    xid = ((unsigned int)platform_get_process_id() & 0xffffU) ^ 0x44480000U;
    packet_length = posix_build_dhcp_packet(packet, sizeof(packet), xid, mac, 1U, NULL, NULL);
    if (packet_length < 0 || sendto(sock, packet, (size_t)packet_length, 0, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return -1;
    }

    {
        ssize_t offer_bytes = recvfrom(sock, reply, sizeof(reply), 0, (struct sockaddr *)&peer_addr, &peer_len);
        if (offer_bytes <= 0 || posix_parse_dhcp_reply(reply, (size_t)offer_bytes, xid, mac, 2U, lease_out) != 0) {
            close(sock);
            return -1;
        }
    }

    memcpy(requested_ip, reply + 16, 4U);
    if (lease_out->server[0] != '\0' && inet_pton(AF_INET, lease_out->server, server_id) == 1) {
        have_server_id = 1;
    }

    packet_length = posix_build_dhcp_packet(packet, sizeof(packet), xid, mac, 3U, requested_ip, have_server_id ? server_id : NULL);
    if (packet_length < 0 || sendto(sock, packet, (size_t)packet_length, 0, (struct sockaddr *)&peer_addr, sizeof(peer_addr)) < 0) {
        close(sock);
        return -1;
    }
    {
        ssize_t ack_bytes = recvfrom(sock, reply, sizeof(reply), 0, (struct sockaddr *)&peer_addr, &peer_len);
        if (ack_bytes <= 0 || posix_parse_dhcp_reply(reply, (size_t)ack_bytes, xid, mac, 5U, lease_out) != 0) {
            close(sock);
            return -1;
        }
    }

    close(sock);
    return 0;
}

int platform_ping_host(const char *host, const PlatformPingOptions *options) {
    struct sockaddr_in addr;
    char ip_text[INET_ADDRSTRLEN];
    struct timeval timeout;
    PlatformPingOptions effective_options;
    int sock;
    unsigned int seq;
    unsigned int transmitted = 0;
    unsigned int received_count = 0;
    unsigned short identifier;
    int process_id = platform_get_process_id();
    double min_ms = 0.0;
    double max_ms = 0.0;
    double total_ms = 0.0;
    struct timeval overall_start;
    int deadline_exceeded = 0;

    if (options == NULL) {
        effective_options.count = PLATFORM_PING_DEFAULT_COUNT;
        effective_options.interval_seconds = PLATFORM_PING_DEFAULT_INTERVAL_SECONDS;
        effective_options.timeout_seconds = PLATFORM_PING_DEFAULT_TIMEOUT_SECONDS;
        effective_options.payload_size = PLATFORM_PING_DEFAULT_PAYLOAD_SIZE;
        effective_options.ttl = 0;
        effective_options.deadline_seconds = 0;
        effective_options.quiet_output = 0;
        effective_options.family = PLATFORM_NETWORK_FAMILY_ANY;
        effective_options.numeric_only = 0;
        options = &effective_options;
    }

    if (host == NULL || options->count == 0 || options->timeout_seconds == 0 ||
        options->payload_size > PLATFORM_PING_MAX_PAYLOAD_SIZE || options->ttl > PLATFORM_PING_MAX_TTL) {
        errno = EINVAL;
        return 1;
    }

    if (options->family == PLATFORM_NETWORK_FAMILY_IPV6 ||
        (options->family != PLATFORM_NETWORK_FAMILY_IPV4 && strchr(host, ':') != NULL)) {
        struct sockaddr_in6 addr6;
        char ip_text6[INET6_ADDRSTRLEN];

        if (resolve_ping_host6(host, &addr6, ip_text6, sizeof(ip_text6)) != 0) {
            tool_write_error("ping", "unknown host ", host);
            return 1;
        }

        sock = posix_socket_open(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
        if (sock < 0) {
            sock = posix_socket_open(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
        }
        if (sock < 0) {
            tool_write_error("ping", "unable to create ICMPv6 socket", 0);
            return 1;
        }

        timeout.tv_sec = (time_t)options->timeout_seconds;
        timeout.tv_usec = 0;
        (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        if (options->ttl > 0U) {
            int hops_value = (int)options->ttl;
            (void)setsockopt(sock, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops_value, sizeof(hops_value));
        }
        if (connect(sock, (struct sockaddr *)&addr6, sizeof(addr6)) != 0) {
            close(sock);
            tool_write_error("ping", "cannot reach ", host);
            return 1;
        }

        identifier = (unsigned short)((process_id > 0) ? process_id : 0x1234);
        (void)gettimeofday(&overall_start, 0);

        rt_write_cstr(1, "PING ");
        rt_write_cstr(1, host);
        rt_write_cstr(1, " (");
        rt_write_cstr(1, ip_text6);
        rt_write_cstr(1, ") ");
        rt_write_uint(1, options->payload_size);
        rt_write_line(1, " data bytes");

        for (seq = 1U; seq <= options->count; ++seq) {
            unsigned char packet[sizeof(PosixIcmpv6Packet) + PLATFORM_PING_MAX_PAYLOAD_SIZE];
            unsigned char reply[sizeof(packet) + 128];
            PosixIcmpv6Packet *header = (PosixIcmpv6Packet *)packet;
            struct timeval start_time;
            struct timeval end_time;
            struct timeval current_time;
            ssize_t reply_size;
            size_t packet_size = sizeof(PosixIcmpv6Packet) + options->payload_size;
            int matched = 0;

            if (options->deadline_seconds > 0U) {
                (void)gettimeofday(&current_time, 0);
                if (elapsed_milliseconds(&overall_start, &current_time) >= (double)options->deadline_seconds * 1000.0) {
                    deadline_exceeded = 1;
                    break;
                }
            }

            memset(packet, 0, packet_size);
            header->type = POSIX_ICMPV6_ECHO_REQUEST;
            header->code = 0;
            header->identifier = htons(identifier);
            header->sequence = htons((unsigned short)seq);
            memset(packet + sizeof(PosixIcmpv6Packet), 0x42, options->payload_size);
            header->checksum = 0;

            (void)gettimeofday(&start_time, 0);
            if (send(sock, packet, packet_size, 0) < 0) {
                close(sock);
                tool_write_error("ping", "send failed to ", host);
                return 1;
            }
            transmitted += 1U;

            for (;;) {
                reply_size = recv(sock, reply, sizeof(reply), 0);
                if (reply_size < 0) {
                    break;
                }

                {
                    size_t offset = 0U;
                    const PosixIcmpv6Packet *reply_header;

                    if ((size_t)reply_size >= 40U && (reply[0] >> 4) == 6U) {
                        offset = 40U;
                    }
                    if ((size_t)reply_size < offset + sizeof(PosixIcmpv6Packet)) {
                        continue;
                    }

                    reply_header = (const PosixIcmpv6Packet *)(reply + offset);
                    if (reply_header->type == POSIX_ICMPV6_ECHO_REPLY &&
                        ntohs(reply_header->identifier) == identifier &&
                        ntohs(reply_header->sequence) == (unsigned short)seq) {
                        double rtt_ms = 0.0;
                        (void)gettimeofday(&end_time, 0);
                        rtt_ms = elapsed_milliseconds(&start_time, &end_time);
                        if (received_count == 0U || rtt_ms < min_ms) {
                            min_ms = rtt_ms;
                        }
                        if (received_count == 0U || rtt_ms > max_ms) {
                            max_ms = rtt_ms;
                        }
                        total_ms += rtt_ms;
                        received_count += 1U;

                        if (!options->quiet_output) {
                            rt_write_uint(1, (unsigned long long)((reply_size > (ssize_t)offset) ? (reply_size - (ssize_t)offset) : reply_size));
                            rt_write_cstr(1, " bytes from ");
                            rt_write_cstr(1, ip_text6);
                            rt_write_cstr(1, ": icmp_seq=");
                            rt_write_uint(1, seq);
                            rt_write_cstr(1, " time=");
                            write_milliseconds(rtt_ms);
                            rt_write_line(1, " ms");
                        }

                        matched = 1;
                        break;
                    }
                }
            }

            if (!matched && !options->quiet_output) {
                rt_write_cstr(1, "Request timeout for icmp_seq ");
                rt_write_uint(1, seq);
                rt_write_char(1, '\n');
            }

            if (seq < options->count && options->interval_seconds > 0U) {
                if (options->deadline_seconds > 0U) {
                    (void)gettimeofday(&current_time, 0);
                    if (elapsed_milliseconds(&overall_start, &current_time) >= (double)options->deadline_seconds * 1000.0) {
                        deadline_exceeded = 1;
                        break;
                    }
                }
                (void)platform_sleep_seconds(options->interval_seconds);
            }
        }

        close(sock);

        if (deadline_exceeded && !options->quiet_output) {
            rt_write_line(1, "ping: deadline reached");
        }

        rt_write_cstr(1, "--- ");
        rt_write_cstr(1, host);
        rt_write_line(1, " ping statistics ---");
        rt_write_uint(1, transmitted);
        rt_write_cstr(1, " packets transmitted, ");
        rt_write_uint(1, received_count);
        rt_write_cstr(1, " packets received, ");
        if (transmitted > 0U) {
            unsigned long long loss = ((unsigned long long)(transmitted - received_count) * 100ULL) / (unsigned long long)transmitted;
            rt_write_uint(1, loss);
        } else {
            rt_write_uint(1, 0ULL);
        }
        rt_write_line(1, "% packet loss");

        if (received_count > 0U) {
            rt_write_cstr(1, "round-trip min/avg/max = ");
            write_milliseconds(min_ms);
            rt_write_char(1, '/');
            write_milliseconds(total_ms / (double)received_count);
            rt_write_char(1, '/');
            write_milliseconds(max_ms);
            rt_write_line(1, " ms");
        }

        return received_count > 0U ? 0 : 1;
    }

    if (resolve_ping_host(host, &addr, ip_text, sizeof(ip_text)) != 0) {
        tool_write_error("ping", "unknown host ", host);
        return 1;
    }

    sock = posix_socket_open(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (sock < 0) {
        sock = posix_socket_open(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    }
    if (sock < 0) {
        tool_write_error("ping", "unable to create ICMP socket", 0);
        return 1;
    }

    timeout.tv_sec = (time_t)options->timeout_seconds;
    timeout.tv_usec = 0;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (options->ttl > 0U) {
        int ttl_value = (int)options->ttl;
        (void)setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl_value, sizeof(ttl_value));
    }

    identifier = (unsigned short)((process_id > 0) ? process_id : 0x1234);
    (void)gettimeofday(&overall_start, 0);

    rt_write_cstr(1, "PING ");
    rt_write_cstr(1, host);
    rt_write_cstr(1, " (");
    rt_write_cstr(1, ip_text);
    rt_write_cstr(1, ") ");
    rt_write_uint(1, options->payload_size);
    rt_write_line(1, " data bytes");

    for (seq = 1U; seq <= options->count; ++seq) {
        unsigned char packet[sizeof(PosixIcmpPacket) + PLATFORM_PING_MAX_PAYLOAD_SIZE];
        unsigned char reply[sizeof(packet) + 128];
        PosixIcmpPacket *header = (PosixIcmpPacket *)packet;
        struct timeval start_time;
        struct timeval end_time;
        struct timeval current_time;
        ssize_t reply_size;
        size_t packet_size = sizeof(PosixIcmpPacket) + options->payload_size;
        int matched = 0;

        if (options->deadline_seconds > 0U) {
            (void)gettimeofday(&current_time, 0);
            if (elapsed_milliseconds(&overall_start, &current_time) >= (double)options->deadline_seconds * 1000.0) {
                deadline_exceeded = 1;
                break;
            }
        }

        memset(packet, 0, packet_size);
        header->type = POSIX_ICMP_ECHO;
        header->code = 0;
        header->identifier = htons(identifier);
        header->sequence = htons((unsigned short)seq);
        memset(packet + sizeof(PosixIcmpPacket), 0x42, options->payload_size);
        header->checksum = compute_icmp_checksum(packet, packet_size);

        (void)gettimeofday(&start_time, 0);
        if (sendto(sock, packet, packet_size, 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(sock);
            tool_write_error("ping", "send failed to ", host);
            return 1;
        }
        transmitted += 1U;

        for (;;) {
            reply_size = recvfrom(sock, reply, sizeof(reply), 0, 0, 0);
            if (reply_size < 0) {
                break;
            }

            {
                size_t offset = 0;
                const PosixIcmpPacket *reply_header;

                if ((size_t)reply_size >= 20U && (reply[0] >> 4) == 4U) {
                    offset = (size_t)(reply[0] & 0x0fU) * 4U;
                }
                if ((size_t)reply_size < offset + sizeof(PosixIcmpPacket)) {
                    continue;
                }

                reply_header = (const PosixIcmpPacket *)(reply + offset);
                if (reply_header->type == POSIX_ICMP_REPLY &&
                    ntohs(reply_header->identifier) == identifier &&
                    ntohs(reply_header->sequence) == (unsigned short)seq) {
                        double rtt_ms = 0.0;
                        unsigned int ttl = (offset >= 20U && (size_t)reply_size >= 9U) ? reply[8] : 0U;

                        (void)gettimeofday(&end_time, 0);
                        rtt_ms = elapsed_milliseconds(&start_time, &end_time);
                    if (received_count == 0U || rtt_ms < min_ms) {
                        min_ms = rtt_ms;
                    }
                    if (received_count == 0U || rtt_ms > max_ms) {
                        max_ms = rtt_ms;
                    }
                    total_ms += rtt_ms;
                    received_count += 1U;

                    if (!options->quiet_output) {
                        rt_write_uint(1, (unsigned long long)((reply_size > (ssize_t)offset) ? (reply_size - (ssize_t)offset) : reply_size));
                        rt_write_cstr(1, " bytes from ");
                        rt_write_cstr(1, ip_text);
                        rt_write_cstr(1, ": icmp_seq=");
                        rt_write_uint(1, seq);
                        if (ttl != 0U) {
                            rt_write_cstr(1, " ttl=");
                            rt_write_uint(1, (unsigned long long)ttl);
                        }
                        rt_write_cstr(1, " time=");
                        write_milliseconds(rtt_ms);
                        rt_write_line(1, " ms");
                    }

                    matched = 1;
                    break;
                }
            }
        }

        if (!matched && !options->quiet_output) {
            rt_write_cstr(1, "Request timeout for icmp_seq ");
            rt_write_uint(1, seq);
            rt_write_char(1, '\n');
        }

        if (seq < options->count && options->interval_seconds > 0U) {
            if (options->deadline_seconds > 0U) {
                (void)gettimeofday(&current_time, 0);
                if (elapsed_milliseconds(&overall_start, &current_time) >= (double)options->deadline_seconds * 1000.0) {
                    deadline_exceeded = 1;
                    break;
                }
            }
            (void)platform_sleep_seconds(options->interval_seconds);
        }
    }

    close(sock);

    if (deadline_exceeded && !options->quiet_output) {
        rt_write_line(1, "ping: deadline reached");
    }

    rt_write_cstr(1, "--- ");
    rt_write_cstr(1, host);
    rt_write_line(1, " ping statistics ---");
    rt_write_uint(1, transmitted);
    rt_write_cstr(1, " packets transmitted, ");
    rt_write_uint(1, received_count);
    rt_write_cstr(1, " packets received, ");
    if (transmitted > 0U) {
        unsigned long long loss = ((unsigned long long)(transmitted - received_count) * 100ULL) / (unsigned long long)transmitted;
        rt_write_uint(1, loss);
    } else {
        rt_write_uint(1, 0ULL);
    }
    rt_write_line(1, "% packet loss");

    if (received_count > 0U) {
        rt_write_cstr(1, "round-trip min/avg/max = ");
        write_milliseconds(min_ms);
        rt_write_char(1, '/');
        write_milliseconds(total_ms / (double)received_count);
        rt_write_char(1, '/');
        write_milliseconds(max_ms);
        rt_write_line(1, " ms");
    }

    return received_count > 0U ? 0 : 1;
}
