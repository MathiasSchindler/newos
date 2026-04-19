#include "platform.h"
#include "runtime.h"
#include "common.h"
#include "syscall.h"

#define LINUX_AF_INET 2
#define LINUX_SOCK_STREAM 1
#define LINUX_SOCK_DGRAM 2
#define LINUX_SOCK_RAW 3
#define LINUX_IPPROTO_IP 0
#define LINUX_IPPROTO_ICMP 1
#define LINUX_IPPROTO_TCP 6
#define LINUX_IP_TTL 2
#define LINUX_SOL_SOCKET 1
#define LINUX_SO_REUSEADDR 2
#define LINUX_SHUT_WR 1
#define LINUX_POLLIN 0x0001
#define LINUX_POLLOUT 0x0004
#define LINUX_POLLERR 0x0008
#define LINUX_POLLHUP 0x0010
#define LINUX_IFNAMSIZ 16
#define LINUX_IFF_UP 0x0001U
#define LINUX_IFF_BROADCAST 0x0002U
#define LINUX_IFF_LOOPBACK 0x0008U
#define LINUX_IFF_RUNNING 0x0040U
#define LINUX_IFF_MULTICAST 0x1000U
#define LINUX_SIOCADDRT 0x890BU
#define LINUX_SIOCDELRT 0x890CU
#define LINUX_SIOCGIFFLAGS 0x8913U
#define LINUX_SIOCSIFFLAGS 0x8914U
#define LINUX_SIOCGIFADDR 0x8915U
#define LINUX_SIOCSIFADDR 0x8916U
#define LINUX_SIOCGIFBRDADDR 0x8919U
#define LINUX_SIOCGIFNETMASK 0x891BU
#define LINUX_SIOCSIFNETMASK 0x891CU
#define LINUX_SIOCGIFMTU 0x8921U
#define LINUX_SIOCSIFMTU 0x8922U
#define LINUX_SIOCGIFHWADDR 0x8927U
#define LINUX_SIOCGIFINDEX 0x8933U
#define LINUX_RTF_UP 0x0001U
#define LINUX_RTF_GATEWAY 0x0002U
#define LINUX_RTF_HOST 0x0004U
#define LINUX_ICMP_ECHO 8U
#define LINUX_ICMP_REPLY 0U
#define LINUX_CLOCK_MONOTONIC 1

typedef struct {
    unsigned char bytes[4];
} LinuxInAddr;

typedef struct {
    unsigned char type;
    unsigned char code;
    unsigned short checksum;
    unsigned short identifier;
    unsigned short sequence;
} LinuxIcmpPacket;

struct linux_sockaddr {
    unsigned short sa_family;
    unsigned char sa_data[14];
};

struct linux_sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;
    LinuxInAddr sin_addr;
    unsigned char sin_zero[8];
};

struct linux_ifreq {
    char ifr_name[LINUX_IFNAMSIZ];
    union {
        struct linux_sockaddr addr;
        struct linux_sockaddr netmask;
        struct linux_sockaddr broadaddr;
        struct linux_sockaddr hwaddr;
        short flags;
        int mtu;
        int ifindex;
    } data;
};

struct linux_rtentry {
    unsigned long rt_pad1;
    struct linux_sockaddr rt_dst;
    struct linux_sockaddr rt_gateway;
    struct linux_sockaddr rt_genmask;
    unsigned short rt_flags;
    short rt_pad2;
    unsigned long rt_pad3;
    void *rt_pad4;
    short rt_metric;
    char *rt_dev;
    unsigned long rt_mtu;
    unsigned long rt_window;
    unsigned short rt_irtt;
};

struct linux_pollfd {
    int fd;
    short events;
    short revents;
};

static unsigned short linux_byte_swap16(unsigned short value) {
    return (unsigned short)(((value & 0x00ffU) << 8) | ((value & 0xff00U) >> 8));
}

static unsigned short linux_host_to_net16(unsigned short value) {
    return linux_byte_swap16(value);
}

static unsigned short linux_net_to_host16(unsigned short value) {
    return linux_byte_swap16(value);
}

static int linux_is_space_char(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static void linux_skip_spaces(const char **cursor) {
    while (**cursor != '\0' && linux_is_space_char(**cursor)) {
        *cursor += 1;
    }
}

static int linux_copy_token(const char **cursor, char *buffer, size_t buffer_size) {
    size_t used = 0;
    const char *scan;

    if (cursor == 0 || *cursor == 0 || buffer == 0 || buffer_size == 0U) {
        return -1;
    }

    linux_skip_spaces(cursor);
    scan = *cursor;
    while (*scan != '\0' && !linux_is_space_char(*scan)) {
        if (used + 1U < buffer_size) {
            buffer[used++] = *scan;
        }
        scan += 1;
    }
    buffer[used] = '\0';
    *cursor = scan;
    return used == 0U ? -1 : 0;
}

static int linux_parse_decimal_text(const char *text, unsigned long long *value_out) {
    unsigned long long value = 0;
    size_t i = 0;

    if (text == 0 || value_out == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[i] != '\0') {
        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }
        value = value * 10ULL + (unsigned long long)(text[i] - '0');
        i += 1U;
    }

    *value_out = value;
    return 0;
}

static int linux_parse_hex_text(const char *text, unsigned long *value_out) {
    unsigned long value = 0;
    size_t i = 0;

    if (text == 0 || value_out == 0 || text[0] == '\0') {
        return -1;
    }

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        i = 2U;
    }

    for (; text[i] != '\0'; ++i) {
        unsigned int digit;

        if (text[i] >= '0' && text[i] <= '9') {
            digit = (unsigned int)(text[i] - '0');
        } else if (text[i] >= 'a' && text[i] <= 'f') {
            digit = (unsigned int)(text[i] - 'a' + 10);
        } else if (text[i] >= 'A' && text[i] <= 'F') {
            digit = (unsigned int)(text[i] - 'A' + 10);
        } else {
            return -1;
        }
        value = (value << 4) | (unsigned long)digit;
    }

    *value_out = value;
    return 0;
}

static int linux_parse_ipv4_text(const char *text, LinuxInAddr *address_out) {
    unsigned char bytes[4];
    int part = 0;
    unsigned int value = 0U;
    int saw_digit = 0;
    size_t i = 0;

    if (text == 0 || address_out == 0) {
        return -1;
    }

    while (1) {
        char ch = text[i];

        if (ch >= '0' && ch <= '9') {
            value = value * 10U + (unsigned int)(ch - '0');
            if (value > 255U) {
                return -1;
            }
            saw_digit = 1;
            i += 1U;
            continue;
        }

        if ((ch == '.' || ch == '\0') && saw_digit && part < 4) {
            bytes[part++] = (unsigned char)value;
            value = 0U;
            saw_digit = 0;
            if (ch == '\0') {
                break;
            }
            i += 1U;
            continue;
        }

        return -1;
    }

    if (part != 4) {
        return -1;
    }

    address_out->bytes[0] = bytes[0];
    address_out->bytes[1] = bytes[1];
    address_out->bytes[2] = bytes[2];
    address_out->bytes[3] = bytes[3];
    return 0;
}

static void linux_ipv4_to_text(const LinuxInAddr *address, char *buffer, size_t buffer_size) {
    char temp[4];
    size_t used = 0;
    size_t i;

    if (buffer == 0 || buffer_size == 0U || address == 0) {
        return;
    }

    buffer[0] = '\0';
    for (i = 0; i < 4U; ++i) {
        unsigned int value = address->bytes[i];
        size_t digits = 0U;
        size_t j;

        if (i > 0U && used + 1U < buffer_size) {
            buffer[used++] = '.';
        }

        if (value >= 100U) {
            temp[digits++] = (char)('0' + (value / 100U));
            value %= 100U;
            temp[digits++] = (char)('0' + (value / 10U));
            temp[digits++] = (char)('0' + (value % 10U));
        } else if (value >= 10U) {
            temp[digits++] = (char)('0' + (value / 10U));
            temp[digits++] = (char)('0' + (value % 10U));
        } else {
            temp[digits++] = (char)('0' + value);
        }

        for (j = 0; j < digits && used + 1U < buffer_size; ++j) {
            buffer[used++] = temp[j];
        }
    }
    buffer[used] = '\0';
}

static void linux_trim_line(char *text) {
    size_t length;

    if (text == 0) {
        return;
    }

    length = rt_strlen(text);
    while (length > 0U && (text[length - 1U] == '\n' || text[length - 1U] == '\r' || text[length - 1U] == ' ' || text[length - 1U] == '\t')) {
        text[length - 1U] = '\0';
        length -= 1U;
    }
}

static int linux_read_text_file(const char *path, char *buffer, size_t buffer_size) {
    int fd;
    long bytes;

    if (path == 0 || buffer == 0 || buffer_size == 0U) {
        return -1;
    }

    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }

    bytes = platform_read(fd, buffer, buffer_size - 1U);
    platform_close(fd);
    if (bytes < 0) {
        return -1;
    }

    buffer[bytes] = '\0';
    linux_trim_line(buffer);
    return 0;
}

static unsigned int linux_prefix_from_mask(const LinuxInAddr *mask) {
    unsigned int count = 0U;
    size_t i;

    if (mask == 0) {
        return 0U;
    }

    for (i = 0U; i < 4U; ++i) {
        unsigned char byte = mask->bytes[i];
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

static unsigned int linux_route_prefix_from_mask(unsigned long mask_value) {
    unsigned int count = 0U;

    while ((mask_value & 1UL) != 0UL) {
        count += 1U;
        mask_value >>= 1U;
    }
    return count;
}

static unsigned int linux_map_link_flags(unsigned int raw_flags) {
    unsigned int flags = 0U;

    if ((raw_flags & LINUX_IFF_UP) != 0U) {
        flags |= PLATFORM_NETWORK_FLAG_UP;
    }
    if ((raw_flags & LINUX_IFF_BROADCAST) != 0U) {
        flags |= PLATFORM_NETWORK_FLAG_BROADCAST;
    }
    if ((raw_flags & LINUX_IFF_LOOPBACK) != 0U) {
        flags |= PLATFORM_NETWORK_FLAG_LOOPBACK;
    }
    if ((raw_flags & LINUX_IFF_RUNNING) != 0U) {
        flags |= PLATFORM_NETWORK_FLAG_RUNNING;
    }
    if ((raw_flags & LINUX_IFF_MULTICAST) != 0U) {
        flags |= PLATFORM_NETWORK_FLAG_MULTICAST;
    }

    return flags;
}

static void linux_format_mac_address(const unsigned char *bytes, size_t byte_count, char *buffer, size_t buffer_size) {
    static const char hex[] = "0123456789abcdef";
    size_t used = 0U;
    size_t i;

    if (buffer == 0 || buffer_size == 0U) {
        return;
    }

    buffer[0] = '\0';
    for (i = 0U; i < byte_count && used + 3U < buffer_size; ++i) {
        if (i > 0U) {
            buffer[used++] = ':';
        }
        buffer[used++] = hex[(bytes[i] >> 4) & 0x0fU];
        buffer[used++] = hex[bytes[i] & 0x0fU];
    }
    buffer[used] = '\0';
}

static int linux_collect_interface_names(char names[][PLATFORM_NAME_CAPACITY], size_t capacity, size_t *count_out) {
    int fd;
    char buffer[4096];
    long bytes;
    size_t count = 0U;
    const char *cursor;
    int line_index = 0;

    if (names == 0 || count_out == 0) {
        return -1;
    }

    *count_out = 0U;
    fd = platform_open_read("/proc/net/dev");
    if (fd < 0) {
        return -1;
    }

    bytes = platform_read(fd, buffer, sizeof(buffer) - 1U);
    platform_close(fd);
    if (bytes <= 0) {
        return -1;
    }
    buffer[bytes] = '\0';

    cursor = buffer;
    while (*cursor != '\0' && count < capacity) {
        const char *line_start = cursor;
        const char *line_end = cursor;
        const char *colon;
        size_t length;
        size_t i;
        int duplicate = 0;

        while (*line_end != '\0' && *line_end != '\n') {
            line_end += 1;
        }
        cursor = (*line_end == '\n') ? (line_end + 1) : line_end;
        line_index += 1;
        if (line_index <= 2) {
            continue;
        }

        colon = line_start;
        while (colon < line_end && *colon != ':') {
            colon += 1;
        }
        if (colon >= line_end) {
            continue;
        }

        while (line_start < colon && linux_is_space_char(*line_start)) {
            line_start += 1;
        }
        while (colon > line_start && linux_is_space_char(colon[-1])) {
            colon -= 1;
        }

        length = (size_t)(colon - line_start);
        if (length == 0U || length >= PLATFORM_NAME_CAPACITY) {
            continue;
        }

        for (i = 0U; i < count; ++i) {
            if (rt_strncmp(names[i], line_start, length) == 0 && names[i][length] == '\0') {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        memcpy(names[count], line_start, length);
        names[count][length] = '\0';
        count += 1U;
    }

    *count_out = count;
    return 0;
}

static int linux_open_inet_socket(int type, int protocol) {
    long fd = linux_syscall3(LINUX_SYS_SOCKET, LINUX_AF_INET, type, protocol);
    return fd < 0 ? -1 : (int)fd;
}

static void linux_prepare_sockaddr(struct linux_sockaddr_in *address, const LinuxInAddr *ip, unsigned int port) {
    memset(address, 0, sizeof(*address));
    address->sin_family = LINUX_AF_INET;
    address->sin_port = linux_host_to_net16((unsigned short)port);
    if (ip != 0) {
        address->sin_addr = *ip;
    } else {
        memset(&address->sin_addr, 0, sizeof(address->sin_addr));
    }
}

static int linux_connect_ipv4(int sock, const LinuxInAddr *ip, unsigned int port) {
    struct linux_sockaddr_in address;

    linux_prepare_sockaddr(&address, ip, port);
    return linux_syscall3(LINUX_SYS_CONNECT, sock, (long)&address, sizeof(address)) < 0 ? -1 : 0;
}

static int linux_bind_ipv4_any(int sock, unsigned int port) {
    struct linux_sockaddr_in address;

    linux_prepare_sockaddr(&address, 0, port);
    return linux_syscall3(LINUX_SYS_BIND, sock, (long)&address, sizeof(address)) < 0 ? -1 : 0;
}

static int linux_set_socket_int_option(int sock, int level, int option, int value) {
    return linux_syscall5(LINUX_SYS_SETSOCKOPT, sock, level, option, (long)&value, sizeof(value)) < 0 ? -1 : 0;
}

static int linux_stream_to_stdout(int sock) {
    char buffer[4096];

    for (;;) {
        long bytes = platform_read(sock, buffer, sizeof(buffer));
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            return 0;
        }
        if (rt_write_all(1, buffer, (size_t)bytes) != 0) {
            return -1;
        }
    }
}

static int linux_stream_stdin_to_socket(int sock) {
    char buffer[4096];

    for (;;) {
        long bytes = platform_read(0, buffer, sizeof(buffer));
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            return 0;
        }
        if (platform_write(sock, buffer, (size_t)bytes) != bytes) {
            return -1;
        }
    }
}

static int linux_resolve_ipv4_host(const char *host, LinuxInAddr *address_out) {
    char buffer[2048];
    const char *cursor;

    if (host == 0 || address_out == 0) {
        return -1;
    }

    if (linux_parse_ipv4_text(host, address_out) == 0) {
        return 0;
    }
    if (rt_strcmp(host, "localhost") == 0) {
        address_out->bytes[0] = 127U;
        address_out->bytes[1] = 0U;
        address_out->bytes[2] = 0U;
        address_out->bytes[3] = 1U;
        return 0;
    }
    if (linux_read_text_file("/etc/hosts", buffer, sizeof(buffer)) != 0) {
        return -1;
    }

    cursor = buffer;
    while (*cursor != '\0') {
        char ip_text[PLATFORM_NETWORK_TEXT_CAPACITY];
        char name_text[PLATFORM_NAME_CAPACITY];
        const char *line_end = cursor;

        while (*line_end != '\0' && *line_end != '\n') {
            line_end += 1;
        }
        if (*cursor == '#') {
            cursor = (*line_end == '\n') ? (line_end + 1) : line_end;
            continue;
        }
        if (linux_copy_token(&cursor, ip_text, sizeof(ip_text)) == 0) {
            while (linux_copy_token(&cursor, name_text, sizeof(name_text)) == 0) {
                if (rt_strcmp(name_text, host) == 0 && linux_parse_ipv4_text(ip_text, address_out) == 0) {
                    return 0;
                }
            }
        }
        cursor = (*line_end == '\n') ? (line_end + 1) : line_end;
    }

    return -1;
}

static unsigned long long linux_monotonic_milliseconds(void) {
    struct linux_timespec time_value;

    if (linux_syscall2(LINUX_SYS_CLOCK_GETTIME, LINUX_CLOCK_MONOTONIC, (long)&time_value) >= 0) {
        return (unsigned long long)time_value.tv_sec * 1000ULL + (unsigned long long)(time_value.tv_nsec / 1000000L);
    }
    return (unsigned long long)platform_get_epoch_time() * 1000ULL;
}

static unsigned short linux_compute_icmp_checksum(const void *data, size_t length) {
    const unsigned short *words = (const unsigned short *)data;
    unsigned int sum = 0U;
    size_t remaining = length;

    while (remaining > 1U) {
        sum += (unsigned int)(*words++);
        remaining -= 2U;
    }
    if (remaining == 1U) {
        sum += (unsigned int)(*(const unsigned char *)words);
    }
    while ((sum >> 16) != 0U) {
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return (unsigned short)~sum;
}

static void linux_write_milliseconds(unsigned long long elapsed_ms, unsigned long long sub_ms) {
    rt_write_uint(1, elapsed_ms);
    rt_write_char(1, '.');
    if (sub_ms < 100ULL) {
        rt_write_char(1, '0');
    }
    if (sub_ms < 10ULL) {
        rt_write_char(1, '0');
    }
    rt_write_uint(1, sub_ms);
}

int platform_connect_tcp(const char *host, unsigned int port, int *socket_fd_out) {
    LinuxInAddr address;
    int sock;

    if (host == 0 || socket_fd_out == 0 || port == 0U || port > 65535U) {
        return -1;
    }
    if (linux_resolve_ipv4_host(host, &address) != 0) {
        return -1;
    }

    sock = linux_open_inet_socket(LINUX_SOCK_STREAM, LINUX_IPPROTO_TCP);
    if (sock < 0) {
        return -1;
    }
    if (linux_connect_ipv4(sock, &address, port) != 0) {
        platform_close(sock);
        return -1;
    }

    *socket_fd_out = sock;
    return 0;
}

int platform_poll_fds(const int *fds, size_t fd_count, size_t *ready_index_out, int timeout_milliseconds) {
    struct linux_pollfd poll_fds[16];
    struct linux_timespec timeout;
    long result;
    size_t i;

    if (fds == 0 || fd_count == 0U || fd_count > sizeof(poll_fds) / sizeof(poll_fds[0]) || ready_index_out == 0) {
        return -1;
    }

    for (i = 0U; i < fd_count; ++i) {
        poll_fds[i].fd = fds[i];
        poll_fds[i].events = (short)(LINUX_POLLIN | LINUX_POLLERR | LINUX_POLLHUP);
        poll_fds[i].revents = 0;
    }

    if (timeout_milliseconds >= 0) {
        timeout.tv_sec = timeout_milliseconds / 1000;
        timeout.tv_nsec = (long)(timeout_milliseconds % 1000) * 1000000L;
        result = linux_syscall5(LINUX_SYS_PPOLL, (long)poll_fds, fd_count, (long)&timeout, 0, 0);
    } else {
        result = linux_syscall5(LINUX_SYS_PPOLL, (long)poll_fds, fd_count, 0, 0, 0);
    }

    if (result <= 0) {
        return (int)result;
    }

    for (i = 0U; i < fd_count; ++i) {
        if ((poll_fds[i].revents & (LINUX_POLLIN | LINUX_POLLERR | LINUX_POLLHUP | LINUX_POLLOUT)) != 0) {
            *ready_index_out = i;
            return 1;
        }
    }

    return 0;
}

int platform_netcat(const char *host, unsigned int port, const PlatformNetcatOptions *options) {
    PlatformNetcatOptions defaults;
    int sock = -1;

    if (options == 0) {
        rt_memset(&defaults, 0, sizeof(defaults));
        options = &defaults;
    }

    if (options->use_udp) {
        return -1;
    }

    if (options->listen_mode) {
        long accepted;
        int reuse = 1;

        sock = linux_open_inet_socket(LINUX_SOCK_STREAM, LINUX_IPPROTO_TCP);
        if (sock < 0) {
            return -1;
        }
        (void)linux_set_socket_int_option(sock, LINUX_SOL_SOCKET, LINUX_SO_REUSEADDR, reuse);
        if (linux_bind_ipv4_any(sock, port) != 0 || linux_syscall2(LINUX_SYS_LISTEN, sock, 1) < 0) {
            platform_close(sock);
            return -1;
        }

        accepted = linux_syscall3(LINUX_SYS_ACCEPT, sock, 0, 0);
        platform_close(sock);
        if (accepted < 0) {
            return -1;
        }
        sock = (int)accepted;
        if (options->scan_mode) {
            platform_close(sock);
            return 0;
        }
        if (linux_stream_to_stdout(sock) != 0) {
            platform_close(sock);
            return -1;
        }
        platform_close(sock);
        return 0;
    }

    if (host == 0) {
        return -1;
    }
    if (platform_connect_tcp(host, port, &sock) != 0) {
        return -1;
    }
    if (options->scan_mode) {
        platform_close(sock);
        return 0;
    }

    if (!platform_isatty(0)) {
        if (linux_stream_stdin_to_socket(sock) != 0) {
            platform_close(sock);
            return -1;
        }
        (void)linux_syscall2(LINUX_SYS_SHUTDOWN, sock, LINUX_SHUT_WR);
    }

    if (linux_stream_to_stdout(sock) != 0) {
        platform_close(sock);
        return -1;
    }

    platform_close(sock);
    return 0;
}

int platform_netcat_tcp(const char *host, unsigned int port, int listen_mode) {
    PlatformNetcatOptions options;

    rt_memset(&options, 0, sizeof(options));
    options.listen_mode = listen_mode;
    return platform_netcat(host, port, &options);
}

int platform_list_network_links(PlatformNetworkLink *entries_out, size_t entry_capacity, size_t *count_out) {
    char names[32][PLATFORM_NAME_CAPACITY];
    size_t name_count = 0U;
    int sock;
    size_t i;

    if (entries_out == 0 || count_out == 0) {
        return -1;
    }
    *count_out = 0U;

    if (linux_collect_interface_names(names, sizeof(names) / sizeof(names[0]), &name_count) != 0) {
        return -1;
    }

    sock = linux_open_inet_socket(LINUX_SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    for (i = 0U; i < name_count && i < entry_capacity; ++i) {
        struct linux_ifreq ifr;
        PlatformNetworkLink *entry = &entries_out[i];

        rt_memset(entry, 0, sizeof(*entry));
        rt_copy_string(entry->name, sizeof(entry->name), names[i]);
        entry->mtu = 1500U;

        rt_memset(&ifr, 0, sizeof(ifr));
        rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), names[i]);
        if (linux_syscall3(LINUX_SYS_IOCTL, sock, LINUX_SIOCGIFINDEX, (long)&ifr) >= 0) {
            entry->index = (unsigned int)ifr.data.ifindex;
        }
        rt_memset(&ifr, 0, sizeof(ifr));
        rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), names[i]);
        if (linux_syscall3(LINUX_SYS_IOCTL, sock, LINUX_SIOCGIFFLAGS, (long)&ifr) >= 0) {
            entry->flags = linux_map_link_flags((unsigned int)ifr.data.flags);
        }
        rt_memset(&ifr, 0, sizeof(ifr));
        rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), names[i]);
        if (linux_syscall3(LINUX_SYS_IOCTL, sock, LINUX_SIOCGIFMTU, (long)&ifr) >= 0 && ifr.data.mtu > 0) {
            entry->mtu = (unsigned int)ifr.data.mtu;
        }
        rt_memset(&ifr, 0, sizeof(ifr));
        rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), names[i]);
        if (linux_syscall3(LINUX_SYS_IOCTL, sock, LINUX_SIOCGIFHWADDR, (long)&ifr) >= 0) {
            linux_format_mac_address(ifr.data.hwaddr.sa_data, 6U, entry->mac, sizeof(entry->mac));
            entry->has_mac = entry->mac[0] != '\0';
        }

        *count_out = i + 1U;
    }

    platform_close(sock);
    return 0;
}

int platform_list_network_addresses(
    PlatformNetworkAddress *entries_out,
    size_t entry_capacity,
    size_t *count_out,
    int family_filter,
    const char *ifname_filter
) {
    char names[32][PLATFORM_NAME_CAPACITY];
    size_t name_count = 0U;
    size_t count = 0U;
    int sock;
    size_t i;

    if (entries_out == 0 || count_out == 0) {
        return -1;
    }
    *count_out = 0U;

    if (family_filter == PLATFORM_NETWORK_FAMILY_IPV6) {
        return 0;
    }
    if (linux_collect_interface_names(names, sizeof(names) / sizeof(names[0]), &name_count) != 0) {
        return -1;
    }

    sock = linux_open_inet_socket(LINUX_SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    for (i = 0U; i < name_count && count < entry_capacity; ++i) {
        struct linux_ifreq ifr;
        struct linux_ifreq mask_ifr;
        struct linux_ifreq broad_ifr;
        struct linux_sockaddr_in *addr_in;
        struct linux_sockaddr_in *mask_in;
        struct linux_sockaddr_in *broad_in;
        PlatformNetworkAddress *entry;

        if (ifname_filter != 0 && rt_strcmp(ifname_filter, names[i]) != 0) {
            continue;
        }

        rt_memset(&ifr, 0, sizeof(ifr));
        rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), names[i]);
        if (linux_syscall3(LINUX_SYS_IOCTL, sock, LINUX_SIOCGIFADDR, (long)&ifr) < 0) {
            continue;
        }

        entry = &entries_out[count];
        rt_memset(entry, 0, sizeof(*entry));
        rt_copy_string(entry->ifname, sizeof(entry->ifname), names[i]);
        entry->family = PLATFORM_NETWORK_FAMILY_IPV4;

        addr_in = (struct linux_sockaddr_in *)&ifr.data.addr;
        linux_ipv4_to_text(&addr_in->sin_addr, entry->address, sizeof(entry->address));
        rt_copy_string(entry->scope, sizeof(entry->scope), rt_strcmp(names[i], "lo") == 0 ? "host" : "global");

        rt_memset(&mask_ifr, 0, sizeof(mask_ifr));
        rt_copy_string(mask_ifr.ifr_name, sizeof(mask_ifr.ifr_name), names[i]);
        if (linux_syscall3(LINUX_SYS_IOCTL, sock, LINUX_SIOCGIFNETMASK, (long)&mask_ifr) >= 0) {
            mask_in = (struct linux_sockaddr_in *)&mask_ifr.data.netmask;
            entry->prefix_length = linux_prefix_from_mask(&mask_in->sin_addr);
        }

        rt_memset(&broad_ifr, 0, sizeof(broad_ifr));
        rt_copy_string(broad_ifr.ifr_name, sizeof(broad_ifr.ifr_name), names[i]);
        if (linux_syscall3(LINUX_SYS_IOCTL, sock, LINUX_SIOCGIFBRDADDR, (long)&broad_ifr) >= 0) {
            broad_in = (struct linux_sockaddr_in *)&broad_ifr.data.broadaddr;
            linux_ipv4_to_text(&broad_in->sin_addr, entry->broadcast, sizeof(entry->broadcast));
            entry->has_broadcast = entry->broadcast[0] != '\0';
        }

        count += 1U;
    }

    platform_close(sock);
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
    int fd;
    char buffer[4096];
    long bytes;
    const char *cursor;
    int line_index = 0;
    size_t count = 0U;

    if (entries_out == 0 || count_out == 0) {
        return -1;
    }
    *count_out = 0U;

    if (family_filter == PLATFORM_NETWORK_FAMILY_IPV6) {
        return 0;
    }

    fd = platform_open_read("/proc/net/route");
    if (fd < 0) {
        return -1;
    }
    bytes = platform_read(fd, buffer, sizeof(buffer) - 1U);
    platform_close(fd);
    if (bytes <= 0) {
        return -1;
    }
    buffer[bytes] = '\0';

    cursor = buffer;
    while (*cursor != '\0' && count < entry_capacity) {
        const char *line_end = cursor;
        char ifname[PLATFORM_NAME_CAPACITY];
        char destination_text[32];
        char gateway_text[32];
        char flags_text[32];
        char metric_text[32];
        char mask_text[32];
        unsigned long destination = 0UL;
        unsigned long gateway = 0UL;
        unsigned long flags = 0UL;
        unsigned long long metric_value = 0ULL;
        unsigned long mask = 0UL;
        PlatformRouteEntry *entry;

        while (*line_end != '\0' && *line_end != '\n') {
            line_end += 1;
        }
        line_index += 1;
        if (line_index <= 1) {
            cursor = (*line_end == '\n') ? (line_end + 1) : line_end;
            continue;
        }

        if (linux_copy_token(&cursor, ifname, sizeof(ifname)) != 0 ||
            linux_copy_token(&cursor, destination_text, sizeof(destination_text)) != 0 ||
            linux_copy_token(&cursor, gateway_text, sizeof(gateway_text)) != 0 ||
            linux_copy_token(&cursor, flags_text, sizeof(flags_text)) != 0) {
            cursor = (*line_end == '\n') ? (line_end + 1) : line_end;
            continue;
        }

        if (linux_copy_token(&cursor, metric_text, sizeof(metric_text)) != 0 ||
            linux_copy_token(&cursor, metric_text, sizeof(metric_text)) != 0 ||
            linux_copy_token(&cursor, metric_text, sizeof(metric_text)) != 0 ||
            linux_copy_token(&cursor, mask_text, sizeof(mask_text)) != 0) {
            cursor = (*line_end == '\n') ? (line_end + 1) : line_end;
            continue;
        }

        cursor = (*line_end == '\n') ? (line_end + 1) : line_end;

        if (ifname_filter != 0 && rt_strcmp(ifname_filter, ifname) != 0) {
            continue;
        }
        if (linux_parse_hex_text(destination_text, &destination) != 0 ||
            linux_parse_hex_text(gateway_text, &gateway) != 0 ||
            linux_parse_hex_text(flags_text, &flags) != 0 ||
            linux_parse_decimal_text(metric_text, &metric_value) != 0 ||
            linux_parse_hex_text(mask_text, &mask) != 0) {
            continue;
        }
        if ((flags & LINUX_RTF_UP) == 0U) {
            continue;
        }

        entry = &entries_out[count];
        rt_memset(entry, 0, sizeof(*entry));
        entry->family = PLATFORM_NETWORK_FAMILY_IPV4;
        entry->metric = (unsigned int)metric_value;
        rt_copy_string(entry->ifname, sizeof(entry->ifname), ifname);
        entry->prefix_length = linux_route_prefix_from_mask(mask);

        if (destination == 0UL && mask == 0UL) {
            entry->is_default = 1;
            rt_copy_string(entry->destination, sizeof(entry->destination), "default");
        } else {
            LinuxInAddr addr;
            addr.bytes[0] = (unsigned char)(destination & 0xffU);
            addr.bytes[1] = (unsigned char)((destination >> 8) & 0xffU);
            addr.bytes[2] = (unsigned char)((destination >> 16) & 0xffU);
            addr.bytes[3] = (unsigned char)((destination >> 24) & 0xffU);
            linux_ipv4_to_text(&addr, entry->destination, sizeof(entry->destination));
        }

        if (gateway != 0UL) {
            LinuxInAddr gw;
            gw.bytes[0] = (unsigned char)(gateway & 0xffU);
            gw.bytes[1] = (unsigned char)((gateway >> 8) & 0xffU);
            gw.bytes[2] = (unsigned char)((gateway >> 16) & 0xffU);
            gw.bytes[3] = (unsigned char)((gateway >> 24) & 0xffU);
            linux_ipv4_to_text(&gw, entry->gateway, sizeof(entry->gateway));
            entry->has_gateway = 1;
        }

        count += 1U;
    }

    *count_out = count;
    return 0;
}

int platform_network_link_set(const char *ifname, int want_up, unsigned int mtu_value, int set_mtu) {
    int sock;
    struct linux_ifreq ifr;

    if (ifname == 0) {
        return -1;
    }

    sock = linux_open_inet_socket(LINUX_SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    rt_memset(&ifr, 0, sizeof(ifr));
    rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
    if (linux_syscall3(LINUX_SYS_IOCTL, sock, LINUX_SIOCGIFFLAGS, (long)&ifr) < 0) {
        platform_close(sock);
        return -1;
    }

    if (want_up >= 0) {
        if (want_up != 0) {
            ifr.data.flags = (short)(ifr.data.flags | (short)LINUX_IFF_UP);
        } else {
            ifr.data.flags = (short)(ifr.data.flags & (short)~LINUX_IFF_UP);
        }
        if (linux_syscall3(LINUX_SYS_IOCTL, sock, LINUX_SIOCSIFFLAGS, (long)&ifr) < 0) {
            platform_close(sock);
            return -1;
        }
    }

    if (set_mtu != 0) {
        rt_memset(&ifr, 0, sizeof(ifr));
        rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
        ifr.data.mtu = (int)mtu_value;
        if (linux_syscall3(LINUX_SYS_IOCTL, sock, LINUX_SIOCSIFMTU, (long)&ifr) < 0) {
            platform_close(sock);
            return -1;
        }
    }

    platform_close(sock);
    return 0;
}

static int linux_parse_ipv4_cidr(const char *text, LinuxInAddr *address_out, LinuxInAddr *mask_out, unsigned int *prefix_out) {
    char copy[PLATFORM_NETWORK_TEXT_CAPACITY];
    char *slash = 0;
    unsigned long long prefix = 32ULL;
    unsigned int i;

    if (text == 0 || address_out == 0 || mask_out == 0 || prefix_out == 0 || rt_strlen(text) >= sizeof(copy)) {
        return -1;
    }

    rt_copy_string(copy, sizeof(copy), text);
    for (i = 0U; copy[i] != '\0'; ++i) {
        if (copy[i] == '/') {
            slash = &copy[i];
            break;
        }
    }

    if (slash != 0) {
        *slash = '\0';
        if (linux_parse_decimal_text(slash + 1, &prefix) != 0 || prefix > 32ULL) {
            return -1;
        }
    }
    if (linux_parse_ipv4_text(copy, address_out) != 0) {
        return -1;
    }

    if (prefix == 0ULL) {
        memset(mask_out->bytes, 0, sizeof(mask_out->bytes));
    } else {
        unsigned int remaining = (unsigned int)prefix;
        for (i = 0U; i < 4U; ++i) {
            if (remaining >= 8U) {
                mask_out->bytes[i] = 0xffU;
                remaining -= 8U;
            } else if (remaining > 0U) {
                mask_out->bytes[i] = (unsigned char)(0xffU << (8U - remaining));
                remaining = 0U;
            } else {
                mask_out->bytes[i] = 0U;
            }
        }
    }

    *prefix_out = (unsigned int)prefix;
    return 0;
}

int platform_network_address_change(const char *ifname, const char *cidr, int add) {
    int sock;
    struct linux_ifreq ifr;
    LinuxInAddr address;
    LinuxInAddr mask;
    unsigned int prefix = 0U;
    struct linux_sockaddr_in *sockaddr_in;

    if (ifname == 0 || cidr == 0 || linux_parse_ipv4_cidr(cidr, &address, &mask, &prefix) != 0) {
        return -1;
    }
    (void)prefix;

    sock = linux_open_inet_socket(LINUX_SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    rt_memset(&ifr, 0, sizeof(ifr));
    rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
    sockaddr_in = (struct linux_sockaddr_in *)&ifr.data.addr;
    linux_prepare_sockaddr(sockaddr_in, add ? &address : 0, 0U);
    if (linux_syscall3(LINUX_SYS_IOCTL, sock, LINUX_SIOCSIFADDR, (long)&ifr) < 0) {
        platform_close(sock);
        return -1;
    }

    rt_memset(&ifr, 0, sizeof(ifr));
    rt_copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
    sockaddr_in = (struct linux_sockaddr_in *)&ifr.data.netmask;
    linux_prepare_sockaddr(sockaddr_in, add ? &mask : 0, 0U);
    if (linux_syscall3(LINUX_SYS_IOCTL, sock, LINUX_SIOCSIFNETMASK, (long)&ifr) < 0) {
        platform_close(sock);
        return -1;
    }

    platform_close(sock);
    return 0;
}

int platform_network_route_change(const char *destination, const char *gateway, const char *ifname, int add) {
    int sock;
    struct linux_rtentry route;
    LinuxInAddr dst;
    LinuxInAddr mask;
    LinuxInAddr gw;
    unsigned int prefix = 0U;
    struct linux_sockaddr_in *sockaddr_in;

    if (destination == 0) {
        return -1;
    }

    rt_memset(&route, 0, sizeof(route));
    if (rt_strcmp(destination, "default") == 0) {
        rt_memset(&dst, 0, sizeof(dst));
        rt_memset(&mask, 0, sizeof(mask));
        prefix = 0U;
    } else if (linux_parse_ipv4_cidr(destination, &dst, &mask, &prefix) != 0) {
        return -1;
    }

    if (gateway != 0 && gateway[0] != '\0') {
        if (linux_parse_ipv4_text(gateway, &gw) != 0) {
            return -1;
        }
        route.rt_flags = (unsigned short)(route.rt_flags | LINUX_RTF_GATEWAY);
    } else {
        rt_memset(&gw, 0, sizeof(gw));
    }

    route.rt_flags = (unsigned short)(route.rt_flags | LINUX_RTF_UP);
    if (prefix == 32U) {
        route.rt_flags = (unsigned short)(route.rt_flags | LINUX_RTF_HOST);
    }
    route.rt_dev = (char *)ifname;

    sockaddr_in = (struct linux_sockaddr_in *)&route.rt_dst;
    linux_prepare_sockaddr(sockaddr_in, &dst, 0U);
    sockaddr_in = (struct linux_sockaddr_in *)&route.rt_gateway;
    linux_prepare_sockaddr(sockaddr_in, &gw, 0U);
    sockaddr_in = (struct linux_sockaddr_in *)&route.rt_genmask;
    linux_prepare_sockaddr(sockaddr_in, &mask, 0U);

    sock = linux_open_inet_socket(LINUX_SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    if (linux_syscall3(LINUX_SYS_IOCTL, sock, add ? LINUX_SIOCADDRT : LINUX_SIOCDELRT, (long)&route) < 0) {
        platform_close(sock);
        return -1;
    }

    platform_close(sock);
    return 0;
}

int platform_ping_host(const char *host, const PlatformPingOptions *options) {
    PlatformPingOptions defaults;
    LinuxInAddr address;
    char address_text[PLATFORM_NETWORK_TEXT_CAPACITY];
    unsigned int transmitted = 0U;
    unsigned int received = 0U;
    unsigned int count;
    unsigned int payload_size;
    unsigned int timeout_ms;
    unsigned int interval_seconds;
    unsigned short identifier;
    int sock;
    unsigned int sequence;

    if (host == 0) {
        return 1;
    }

    if (options == 0) {
        defaults.count = PLATFORM_PING_DEFAULT_COUNT;
        defaults.interval_seconds = PLATFORM_PING_DEFAULT_INTERVAL_SECONDS;
        defaults.timeout_seconds = PLATFORM_PING_DEFAULT_TIMEOUT_SECONDS;
        defaults.payload_size = PLATFORM_PING_DEFAULT_PAYLOAD_SIZE;
        defaults.ttl = 0U;
        options = &defaults;
    }

    count = options->count == 0U ? PLATFORM_PING_DEFAULT_COUNT : options->count;
    payload_size = options->payload_size > PLATFORM_PING_MAX_PAYLOAD_SIZE ? PLATFORM_PING_MAX_PAYLOAD_SIZE : options->payload_size;
    timeout_ms = (options->timeout_seconds == 0U ? PLATFORM_PING_DEFAULT_TIMEOUT_SECONDS : options->timeout_seconds) * 1000U;
    interval_seconds = options->interval_seconds;

    if (linux_resolve_ipv4_host(host, &address) != 0) {
        rt_write_cstr(2, "ping: cannot resolve ");
        rt_write_line(2, host);
        return 1;
    }

    sock = linux_open_inet_socket(LINUX_SOCK_RAW, LINUX_IPPROTO_ICMP);
    if (sock < 0 || linux_connect_ipv4(sock, &address, 0U) != 0) {
        if (sock >= 0) {
            platform_close(sock);
        }
        rt_write_line(2, "ping: ICMP is not yet supported on this platform");
        return 1;
    }

    if (options->ttl != 0U) {
        int ttl_value = (int)options->ttl;
        (void)linux_set_socket_int_option(sock, LINUX_IPPROTO_IP, LINUX_IP_TTL, ttl_value);
    }

    linux_ipv4_to_text(&address, address_text, sizeof(address_text));
    rt_write_cstr(1, "PING ");
    rt_write_cstr(1, host);
    rt_write_cstr(1, " (");
    rt_write_cstr(1, address_text);
    rt_write_cstr(1, "): ");
    rt_write_uint(1, (unsigned long long)payload_size);
    rt_write_line(1, " data bytes");

    identifier = (unsigned short)(platform_get_process_id() & 0xffff);
    for (sequence = 0U; sequence < count; ++sequence) {
        unsigned char packet[8 + PLATFORM_PING_MAX_PAYLOAD_SIZE];
        unsigned char reply[512];
        LinuxIcmpPacket *icmp = (LinuxIcmpPacket *)packet;
        unsigned long long start_ms;
        unsigned long long deadline;
        int matched = 0;
        size_t ready_index = 0U;
        unsigned int i;

        rt_memset(packet, 0, sizeof(packet));
        icmp->type = LINUX_ICMP_ECHO;
        icmp->code = 0U;
        icmp->identifier = linux_host_to_net16(identifier);
        icmp->sequence = linux_host_to_net16((unsigned short)sequence);
        for (i = 0U; i < payload_size; ++i) {
            packet[sizeof(LinuxIcmpPacket) + i] = (unsigned char)('A' + (i % 26U));
        }
        icmp->checksum = 0U;
        icmp->checksum = linux_compute_icmp_checksum(packet, sizeof(LinuxIcmpPacket) + payload_size);

        start_ms = linux_monotonic_milliseconds();
        if (platform_write(sock, packet, sizeof(LinuxIcmpPacket) + payload_size) < (long)(sizeof(LinuxIcmpPacket) + payload_size)) {
            rt_write_line(2, "ping: send failed");
            platform_close(sock);
            return 1;
        }
        transmitted += 1U;
        deadline = start_ms + timeout_ms;

        while (!matched) {
            int timeout_remaining = (int)(deadline > linux_monotonic_milliseconds() ? deadline - linux_monotonic_milliseconds() : 0ULL);
            long reply_bytes;
            size_t ip_header_length = 0U;
            LinuxIcmpPacket *reply_icmp;

            if (platform_poll_fds(&sock, 1U, &ready_index, timeout_remaining) <= 0) {
                break;
            }

            reply_bytes = platform_read(sock, reply, sizeof(reply));
            if (reply_bytes <= 0) {
                break;
            }

            if ((reply[0] >> 4) == 4U) {
                ip_header_length = (size_t)(reply[0] & 0x0fU) * 4U;
            }
            if (ip_header_length + sizeof(LinuxIcmpPacket) > (size_t)reply_bytes) {
                ip_header_length = 0U;
            }

            reply_icmp = (LinuxIcmpPacket *)(reply + ip_header_length);
            if (reply_icmp->type == LINUX_ICMP_REPLY &&
                linux_net_to_host16(reply_icmp->identifier) == identifier &&
                linux_net_to_host16(reply_icmp->sequence) == (unsigned short)sequence) {
                unsigned long long elapsed = linux_monotonic_milliseconds() - start_ms;
                unsigned int ttl = (ip_header_length >= 9U) ? reply[8] : 0U;
                rt_write_uint(1, (unsigned long long)(reply_bytes - (long)ip_header_length));
                rt_write_cstr(1, " bytes from ");
                rt_write_cstr(1, address_text);
                rt_write_cstr(1, ": icmp_seq=");
                rt_write_uint(1, (unsigned long long)(sequence + 1U));
                if (ttl != 0U) {
                    rt_write_cstr(1, " ttl=");
                    rt_write_uint(1, (unsigned long long)ttl);
                }
                rt_write_cstr(1, " time=");
                linux_write_milliseconds(elapsed, 0ULL);
                rt_write_line(1, " ms");
                received += 1U;
                matched = 1;
            }
        }

        if (!matched) {
            rt_write_cstr(1, "Request timeout for icmp_seq ");
            rt_write_uint(1, (unsigned long long)(sequence + 1U));
            rt_write_char(1, '\n');
        }

        if (sequence + 1U < count && interval_seconds > 0U) {
            (void)platform_sleep_seconds(interval_seconds);
        }
    }

    rt_write_cstr(1, "--- ");
    rt_write_cstr(1, host);
    rt_write_line(1, " ping statistics ---");
    rt_write_uint(1, (unsigned long long)transmitted);
    rt_write_cstr(1, " packets transmitted, ");
    rt_write_uint(1, (unsigned long long)received);
    rt_write_cstr(1, " packets received, ");
    rt_write_uint(1, transmitted == 0U ? 0ULL : (unsigned long long)(((transmitted - received) * 100U) / transmitted));
    rt_write_line(1, "% packet loss");

    platform_close(sock);
    return received == 0U ? 1 : 0;
}
