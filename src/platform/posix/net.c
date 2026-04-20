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
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
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

#define POSIX_ICMP_ECHO 8
#define POSIX_ICMP_REPLY 0

typedef struct {
    unsigned char type;
    unsigned char code;
    unsigned short checksum;
    unsigned short identifier;
    unsigned short sequence;
} PosixIcmpPacket;

static void posix_fd_zero(void *set_ptr) {
    memset(set_ptr, 0, 128U);
}

static void posix_fd_set_bit(void *set_ptr, int fd) {
    unsigned long *bits = (unsigned long *)set_ptr;
    unsigned int index;
    unsigned int bit;

    if (set_ptr == NULL || fd < 0 || fd >= 1024) {
        return;
    }

    index = (unsigned int)fd / 64U;
    bit = (unsigned int)fd % 64U;
    bits[index] |= (1UL << bit);
}

static int posix_fd_is_set_bit(const void *set_ptr, int fd) {
    const unsigned long *bits = (const unsigned long *)set_ptr;
    unsigned int index;
    unsigned int bit;

    if (set_ptr == NULL || fd < 0 || fd >= 1024) {
        return 0;
    }

    index = (unsigned int)fd / 64U;
    bit = (unsigned int)fd % 64U;
    return (bits[index] & (1UL << bit)) != 0 ? 1 : 0;
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
        ssize_t written = send(fd, buffer + offset, count - offset, 0);
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
        sock = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
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
    fd_set read_set;
    struct timeval timeout;
    struct timeval *timeout_ptr = NULL;
    int max_fd = -1;
    size_t i;
    int rc;

    if (fds == NULL || fd_count == 0U || ready_index_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    posix_fd_zero(&read_set);
    for (i = 0; i < fd_count; ++i) {
        if (fds[i] < 0) {
            continue;
        }
        posix_fd_set_bit(&read_set, fds[i]);
        if (fds[i] > max_fd) {
            max_fd = fds[i];
        }
    }

    if (max_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (timeout_milliseconds >= 0) {
        timeout.tv_sec = (time_t)(timeout_milliseconds / 1000);
        timeout.tv_usec = (suseconds_t)((timeout_milliseconds % 1000) * 1000);
        timeout_ptr = &timeout;
    }

    rc = select(max_fd + 1, &read_set, NULL, NULL, timeout_ptr);
    if (rc <= 0) {
        return rc;
    }

    for (i = 0; i < fd_count; ++i) {
        if (fds[i] >= 0 && posix_fd_is_set_bit(&read_set, fds[i])) {
            *ready_index_out = i;
            return 1;
        }
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

            sock = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
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

            int client = accept(sock, NULL, NULL);
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
            sock = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
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

    sock = socket(AF_INET, SOCK_DGRAM, 0);
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
                const unsigned char *addr = (const unsigned char *)LLADDR(sdl);
                posix_format_mac_address(addr, (size_t)sdl->sdl_alen, entries_out[index].mac, sizeof(entries_out[index].mac));
                entries_out[index].has_mac = 1;
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
            FILE *file = popen("netstat -rn -f inet 2>/dev/null", "r");
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
            FILE *file = popen("netstat -rn -f inet6 2>/dev/null", "r");
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
        errno = ENOTSUP;
        return -1;
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

    if (ifname == NULL) {
        errno = EINVAL;
        return -1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
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

    if (ifname == NULL || cidr == NULL) {
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
        sock = socket(AF_INET, SOCK_DGRAM, 0);
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

        sock = socket(AF_INET, SOCK_DGRAM, 0);
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

    sock = socket(AF_INET, SOCK_DGRAM, 0);
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
        options = &effective_options;
    }

    if (host == NULL || options->count == 0 || options->timeout_seconds == 0 ||
        options->payload_size > PLATFORM_PING_MAX_PAYLOAD_SIZE || options->ttl > PLATFORM_PING_MAX_TTL) {
        errno = EINVAL;
        return 1;
    }

    if (resolve_ping_host(host, &addr, ip_text, sizeof(ip_text)) != 0) {
        tool_write_error("ping", "unknown host ", host);
        return 1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (sock < 0) {
        sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
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
