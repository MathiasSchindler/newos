#include "../../src/platform/linux/common.h"

#if !defined(LINUX_SYS_SOCKET) && defined(__aarch64__)
#include "../../src/arch/aarch64/linux/syscall.h"
#elif !defined(LINUX_SYS_SOCKET)
#include "../../src/arch/x86_64/linux/syscall.h"
#endif

#define ETHERLAT_AF_PACKET 17
#define ETHERLAT_SOCK_RAW 3
#define ETHERLAT_SOL_SOCKET 1
#define ETHERLAT_SO_BUSY_POLL 46
#define ETHERLAT_SIOCGIFHWADDR 0x8927U
#define ETHERLAT_SIOCGIFINDEX 0x8933U
#define ETHERLAT_ETH_ALEN 6U
#define ETHERLAT_ETH_HEADER_SIZE 14U
#define ETHERLAT_PROTOCOL_HEADER_SIZE 40U
#define ETHERLAT_MIN_PAYLOAD_SIZE 46U
#define ETHERLAT_MAX_PAYLOAD_SIZE 1500U
#define ETHERLAT_MAX_FRAME_SIZE (ETHERLAT_ETH_HEADER_SIZE + ETHERLAT_MAX_PAYLOAD_SIZE)
#define ETHERLAT_DEFAULT_ETHERTYPE 0x88b5U
#define ETHERLAT_DEFAULT_COUNT 1000U
#define ETHERLAT_DEFAULT_TIMEOUT_MS 1000U
#define ETHERLAT_MAX_SAMPLES 100000U
#define ETHERLAT_OP_PING 1U
#define ETHERLAT_OP_PONG 2U
#define ETHERLAT_OP_DISCOVER 3U
#define ETHERLAT_OP_INFO 4U
#define ETHERLAT_POLLIN 0x0001

typedef struct {
    unsigned short sa_family;
    unsigned char sa_data[14];
} EtherlatSockaddr;

typedef struct {
    char ifr_name[16];
    union {
        EtherlatSockaddr hwaddr;
        int ifindex;
        unsigned char pad[24];
    } data;
} EtherlatIfreq;

typedef struct {
    unsigned short sll_family;
    unsigned short sll_protocol;
    int sll_ifindex;
    unsigned short sll_hatype;
    unsigned char sll_pkttype;
    unsigned char sll_halen;
    unsigned char sll_addr[8];
} EtherlatSockaddrLl;

typedef struct {
    int fd;
    short events;
    short revents;
} EtherlatPollfd;

typedef struct {
    const char *mode;
    const char *ifname;
    unsigned char dst_mac[ETHERLAT_ETH_ALEN];
    int has_dst_mac;
    unsigned int ethertype;
    unsigned int count;
    unsigned int payload_size;
    unsigned int timeout_ms;
    unsigned int interval_us;
    unsigned int busy_poll_us;
    int samples_tsv;
    int quiet;
} EtherlatOptions;

typedef struct {
    int fd;
    int ifindex;
    unsigned char mac[ETHERLAT_ETH_ALEN];
    unsigned int ethertype;
} EtherlatSocket;

static unsigned long long latency_samples[ETHERLAT_MAX_SAMPLES];

static unsigned short etherlat_bswap16(unsigned short value) {
    return (unsigned short)(((value & 0x00ffU) << 8U) | ((value & 0xff00U) >> 8U));
}

static unsigned short etherlat_htons(unsigned int value) {
    return etherlat_bswap16((unsigned short)value);
}

static void store_u16_be(unsigned char *out, unsigned int value) {
    out[0] = (unsigned char)((value >> 8U) & 0xffU);
    out[1] = (unsigned char)(value & 0xffU);
}

static void store_u32_be(unsigned char *out, unsigned int value) {
    out[0] = (unsigned char)((value >> 24U) & 0xffU);
    out[1] = (unsigned char)((value >> 16U) & 0xffU);
    out[2] = (unsigned char)((value >> 8U) & 0xffU);
    out[3] = (unsigned char)(value & 0xffU);
}

static void store_u64_be(unsigned char *out, unsigned long long value) {
    store_u32_be(out, (unsigned int)(value >> 32U));
    store_u32_be(out + 4U, (unsigned int)(value & 0xffffffffULL));
}

static unsigned int load_u16_be(const unsigned char *in) {
    return ((unsigned int)in[0] << 8U) | (unsigned int)in[1];
}

static unsigned int load_u32_be(const unsigned char *in) {
    return ((unsigned int)in[0] << 24U) |
           ((unsigned int)in[1] << 16U) |
           ((unsigned int)in[2] << 8U) |
           (unsigned int)in[3];
}

static unsigned long long load_u64_be(const unsigned char *in) {
    return ((unsigned long long)load_u32_be(in) << 32U) | (unsigned long long)load_u32_be(in + 4U);
}

static int text_starts_with(const char *text, const char *prefix) {
    return rt_strncmp(text, prefix, rt_strlen(prefix)) == 0;
}

static void write_usage(void) {
    rt_write_line(1, "usage: etherlat listen -i IFACE [--count N] [--ethertype HEX] [--busy-poll-us USEC]");
    rt_write_line(1, "       etherlat discover -i IFACE [--timeout-ms MS] [--busy-poll-us USEC] [--quiet]");
    rt_write_line(1, "       etherlat ping -i IFACE --dst MAC [--count N] [--size BYTES] [--timeout-ms MS] [--samples]");
    rt_write_line(1, "       etherlat ping -i IFACE --dst MAC [--interval-us USEC] [--busy-poll-us USEC] [--ethertype HEX]");
}

static void write_error(const char *message) {
    rt_write_cstr(2, "etherlat: ");
    rt_write_line(2, message);
}

static void write_error2(const char *message, const char *value) {
    rt_write_cstr(2, "etherlat: ");
    rt_write_cstr(2, message);
    rt_write_line(2, value);
}

static void write_error_uint(const char *message, unsigned long long value) {
    rt_write_cstr(2, "etherlat: ");
    rt_write_cstr(2, message);
    rt_write_uint(2, value);
    rt_write_char(2, '\n');
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int parse_ethertype(const char *text, unsigned int *value_out) {
    unsigned int value = 0U;
    size_t i = 0U;
    unsigned int digits = 0U;

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) i = 2U;
    while (text[i] != '\0') {
        int digit = hex_value(text[i]);
        if (digit < 0 || digits >= 4U) return -1;
        value = (value << 4U) | (unsigned int)digit;
        digits += 1U;
        i += 1U;
    }
    if (digits == 0U || value == 0U || value > 0xffffU) return -1;
    *value_out = value;
    return 0;
}

static int parse_uint_option(const char *text, unsigned int *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value > 0xffffffffULL) return -1;
    *value_out = (unsigned int)value;
    return 0;
}

static int parse_mac(const char *text, unsigned char mac[ETHERLAT_ETH_ALEN]) {
    size_t i;

    for (i = 0U; i < ETHERLAT_ETH_ALEN; ++i) {
        int high = hex_value(text[i * 3U]);
        int low = hex_value(text[i * 3U + 1U]);
        if (high < 0 || low < 0) return -1;
        mac[i] = (unsigned char)((high << 4) | low);
        if (i + 1U < ETHERLAT_ETH_ALEN) {
            if (text[i * 3U + 2U] != ':') return -1;
        } else if (text[i * 3U + 2U] != '\0') {
            return -1;
        }
    }
    return 0;
}

static const char *next_arg_value(int argc, char **argv, int *index_io, const char *option) {
    int index = *index_io;

    if (index + 1 >= argc) {
        write_error2("missing value for ", option);
        return 0;
    }
    *index_io = index + 1;
    return argv[index + 1];
}

static const char *inline_option_value(const char *arg, const char *prefix) {
    size_t prefix_len = rt_strlen(prefix);

    return rt_strncmp(arg, prefix, prefix_len) == 0 ? arg + prefix_len : 0;
}

static int parse_options(int argc, char **argv, EtherlatOptions *options) {
    int i;

    memset(options, 0, sizeof(*options));
    options->ethertype = ETHERLAT_DEFAULT_ETHERTYPE;
    options->count = ETHERLAT_DEFAULT_COUNT;
    options->payload_size = ETHERLAT_MIN_PAYLOAD_SIZE;
    options->timeout_ms = ETHERLAT_DEFAULT_TIMEOUT_MS;

    if (argc < 2 || rt_strcmp(argv[1], "--help") == 0 || rt_strcmp(argv[1], "-h") == 0) {
        write_usage();
        return 1;
    }
    if (rt_strcmp(argv[1], "ping") != 0 && rt_strcmp(argv[1], "listen") != 0 && rt_strcmp(argv[1], "discover") != 0) {
        write_usage();
        return -1;
    }
    options->mode = argv[1];
    if (rt_strcmp(options->mode, "listen") == 0) options->count = 0U;

    for (i = 2; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = 0;

        if (rt_strcmp(arg, "-i") == 0 || rt_strcmp(arg, "--iface") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0) return -1;
            options->ifname = value;
        } else if ((value = inline_option_value(arg, "--iface=")) != 0) {
            options->ifname = value;
        } else if (rt_strcmp(arg, "--dst") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_mac(value, options->dst_mac) != 0) return -1;
            options->has_dst_mac = 1;
        } else if ((value = inline_option_value(arg, "--dst=")) != 0) {
            if (parse_mac(value, options->dst_mac) != 0) return -1;
            options->has_dst_mac = 1;
        } else if (rt_strcmp(arg, "--ethertype") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_ethertype(value, &options->ethertype) != 0) return -1;
        } else if ((value = inline_option_value(arg, "--ethertype=")) != 0) {
            if (parse_ethertype(value, &options->ethertype) != 0) return -1;
        } else if (rt_strcmp(arg, "--count") == 0 || rt_strcmp(arg, "-c") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_uint_option(value, &options->count) != 0 || options->count > ETHERLAT_MAX_SAMPLES) return -1;
        } else if ((value = inline_option_value(arg, "--count=")) != 0) {
            if (parse_uint_option(value, &options->count) != 0 || options->count > ETHERLAT_MAX_SAMPLES) return -1;
        } else if (rt_strcmp(arg, "--size") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_uint_option(value, &options->payload_size) != 0) return -1;
        } else if ((value = inline_option_value(arg, "--size=")) != 0) {
            if (parse_uint_option(value, &options->payload_size) != 0) return -1;
        } else if (rt_strcmp(arg, "--timeout-ms") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_uint_option(value, &options->timeout_ms) != 0) return -1;
        } else if ((value = inline_option_value(arg, "--timeout-ms=")) != 0) {
            if (parse_uint_option(value, &options->timeout_ms) != 0) return -1;
        } else if (rt_strcmp(arg, "--interval-us") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_uint_option(value, &options->interval_us) != 0) return -1;
        } else if ((value = inline_option_value(arg, "--interval-us=")) != 0) {
            if (parse_uint_option(value, &options->interval_us) != 0) return -1;
        } else if (rt_strcmp(arg, "--busy-poll-us") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_uint_option(value, &options->busy_poll_us) != 0) return -1;
        } else if ((value = inline_option_value(arg, "--busy-poll-us=")) != 0) {
            if (parse_uint_option(value, &options->busy_poll_us) != 0) return -1;
        } else if (rt_strcmp(arg, "--samples") == 0 || rt_strcmp(arg, "--tsv") == 0) {
            options->samples_tsv = 1;
        } else if (rt_strcmp(arg, "--quiet") == 0 || rt_strcmp(arg, "-q") == 0) {
            options->quiet = 1;
        } else {
            write_error2("unknown option ", arg);
            return -1;
        }
    }

    if (options->ifname == 0 || options->ifname[0] == '\0') {
        write_error("missing interface; use -i IFACE");
        return -1;
    }
    if (rt_strcmp(options->mode, "ping") == 0 && !options->has_dst_mac) {
        write_error("ping requires --dst MAC");
        return -1;
    }
    if (options->payload_size < ETHERLAT_PROTOCOL_HEADER_SIZE) options->payload_size = ETHERLAT_PROTOCOL_HEADER_SIZE;
    if (options->payload_size < ETHERLAT_MIN_PAYLOAD_SIZE) options->payload_size = ETHERLAT_MIN_PAYLOAD_SIZE;
    if (options->payload_size > ETHERLAT_MAX_PAYLOAD_SIZE) {
        write_error("payload size exceeds Ethernet MTU payload cap of 1500 bytes");
        return -1;
    }
    return 0;
}

static int etherlat_open_socket(EtherlatSocket *socket_out, const char *ifname, unsigned int ethertype, unsigned int busy_poll_us) {
    EtherlatIfreq ifreq;
    EtherlatSockaddrLl bind_addr;
    long fd;

    fd = linux_syscall3(LINUX_SYS_SOCKET, ETHERLAT_AF_PACKET, ETHERLAT_SOCK_RAW | LINUX_SOCK_CLOEXEC, (long)etherlat_htons(ethertype));
    if (fd == -LINUX_EINVAL) fd = linux_syscall3(LINUX_SYS_SOCKET, ETHERLAT_AF_PACKET, ETHERLAT_SOCK_RAW, (long)etherlat_htons(ethertype));
    if (fd < 0) return -1;

    memset(&ifreq, 0, sizeof(ifreq));
    rt_copy_string(ifreq.ifr_name, sizeof(ifreq.ifr_name), ifname);
    if (linux_syscall3(LINUX_SYS_IOCTL, fd, ETHERLAT_SIOCGIFINDEX, (long)&ifreq) < 0) goto fail;
    socket_out->ifindex = ifreq.data.ifindex;

    memset(&ifreq, 0, sizeof(ifreq));
    rt_copy_string(ifreq.ifr_name, sizeof(ifreq.ifr_name), ifname);
    if (linux_syscall3(LINUX_SYS_IOCTL, fd, ETHERLAT_SIOCGIFHWADDR, (long)&ifreq) < 0) goto fail;
    memcpy(socket_out->mac, ifreq.data.hwaddr.sa_data, ETHERLAT_ETH_ALEN);

    if (busy_poll_us != 0U) {
        int value = (int)busy_poll_us;
        if (linux_syscall5(LINUX_SYS_SETSOCKOPT, fd, ETHERLAT_SOL_SOCKET, ETHERLAT_SO_BUSY_POLL, (long)&value, sizeof(value)) < 0) {
            write_error_uint("cannot set SO_BUSY_POLL to ", busy_poll_us);
            goto fail;
        }
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sll_family = ETHERLAT_AF_PACKET;
    bind_addr.sll_protocol = etherlat_htons(ethertype);
    bind_addr.sll_ifindex = socket_out->ifindex;
    if (linux_syscall3(LINUX_SYS_BIND, fd, (long)&bind_addr, sizeof(bind_addr)) < 0) goto fail;

    socket_out->fd = (int)fd;
    socket_out->ethertype = ethertype;
    return 0;

fail:
    linux_syscall1(LINUX_SYS_CLOSE, fd);
    return -1;
}

static void build_payload(unsigned char *payload, unsigned int op, unsigned long long seq, unsigned long long send_ns, unsigned long long peer_recv_ns, unsigned int payload_size, unsigned int nonce) {
    unsigned int i;

    payload[0] = 'E';
    payload[1] = 'L';
    payload[2] = 'A';
    payload[3] = 'T';
    payload[4] = 1U;
    payload[5] = (unsigned char)op;
    store_u16_be(payload + 6U, ETHERLAT_PROTOCOL_HEADER_SIZE);
    store_u64_be(payload + 8U, seq);
    store_u64_be(payload + 16U, send_ns);
    store_u64_be(payload + 24U, peer_recv_ns);
    store_u32_be(payload + 32U, payload_size);
    store_u32_be(payload + 36U, nonce);
    for (i = ETHERLAT_PROTOCOL_HEADER_SIZE; i < payload_size; ++i) {
        payload[i] = (unsigned char)(seq + (unsigned long long)i);
    }
}

static int parse_payload(const unsigned char *payload, size_t length, unsigned int *op_out, unsigned long long *seq_out, unsigned long long *send_ns_out, unsigned long long *peer_recv_ns_out, unsigned int *payload_size_out, unsigned int *nonce_out) {
    unsigned int header_size;
    unsigned int payload_size;

    if (length < ETHERLAT_PROTOCOL_HEADER_SIZE || payload[0] != 'E' || payload[1] != 'L' || payload[2] != 'A' || payload[3] != 'T' || payload[4] != 1U) return -1;
    header_size = load_u16_be(payload + 6U);
    payload_size = load_u32_be(payload + 32U);
    if (header_size < ETHERLAT_PROTOCOL_HEADER_SIZE || payload_size > length) return -1;
    *op_out = payload[5];
    *seq_out = load_u64_be(payload + 8U);
    *send_ns_out = load_u64_be(payload + 16U);
    *peer_recv_ns_out = load_u64_be(payload + 24U);
    *payload_size_out = payload_size;
    *nonce_out = load_u32_be(payload + 36U);
    return 0;
}

static void build_frame(unsigned char *frame, const unsigned char dst[ETHERLAT_ETH_ALEN], const unsigned char src[ETHERLAT_ETH_ALEN], unsigned int ethertype, unsigned int op, unsigned long long seq, unsigned long long send_ns, unsigned long long peer_recv_ns, unsigned int payload_size, unsigned int nonce) {
    memcpy(frame, dst, ETHERLAT_ETH_ALEN);
    memcpy(frame + 6U, src, ETHERLAT_ETH_ALEN);
    store_u16_be(frame + 12U, ethertype);
    build_payload(frame + ETHERLAT_ETH_HEADER_SIZE, op, seq, send_ns, peer_recv_ns, payload_size, nonce);
}

static long send_frame(EtherlatSocket *socket, const unsigned char dst[ETHERLAT_ETH_ALEN], unsigned char *frame, size_t frame_size) {
    EtherlatSockaddrLl address;

    memset(&address, 0, sizeof(address));
    address.sll_family = ETHERLAT_AF_PACKET;
    address.sll_protocol = etherlat_htons(socket->ethertype);
    address.sll_ifindex = socket->ifindex;
    address.sll_halen = ETHERLAT_ETH_ALEN;
    memcpy(address.sll_addr, dst, ETHERLAT_ETH_ALEN);
    return linux_syscall6(LINUX_SYS_SENDTO, socket->fd, (long)frame, (long)frame_size, 0, (long)&address, sizeof(address));
}

static int etherlat_poll(int fd, unsigned int timeout_ms) {
    EtherlatPollfd pollfd;
    struct linux_timespec timeout;
    long result;

    pollfd.fd = fd;
    pollfd.events = ETHERLAT_POLLIN;
    pollfd.revents = 0;
    timeout.tv_sec = (long)(timeout_ms / 1000U);
    timeout.tv_nsec = (long)((timeout_ms % 1000U) * 1000000U);
    result = linux_syscall5(LINUX_SYS_PPOLL, (long)&pollfd, 1, (long)&timeout, 0, 0);
    if (result <= 0) return (int)result;
    return (pollfd.revents & ETHERLAT_POLLIN) != 0 ? 1 : -1;
}

static unsigned int remaining_timeout_ms(unsigned long long deadline_ns) {
    unsigned long long now = platform_get_monotonic_time_ns();
    unsigned long long remaining;

    if (now >= deadline_ns) return 0U;
    remaining = deadline_ns - now;
    remaining = (remaining + 999999ULL) / 1000000ULL;
    return remaining > 0xffffffffULL ? 0xffffffffU : (unsigned int)remaining;
}

static int mac_is_broadcast(const unsigned char mac[ETHERLAT_ETH_ALEN]) {
    size_t i;

    for (i = 0U; i < ETHERLAT_ETH_ALEN; ++i) {
        if (mac[i] != 0xffU) return 0;
    }
    return 1;
}

static int frame_is_for_us(const unsigned char *frame, const unsigned char mac[ETHERLAT_ETH_ALEN]) {
    return memcmp(frame, mac, ETHERLAT_ETH_ALEN) == 0 || mac_is_broadcast(frame);
}

static int latency_compare(const void *left, const void *right) {
    const unsigned long long lhs = *(const unsigned long long *)left;
    const unsigned long long rhs = *(const unsigned long long *)right;

    return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
}

static void write_mac(const unsigned char mac[ETHERLAT_ETH_ALEN]) {
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0U; i < ETHERLAT_ETH_ALEN; ++i) {
        if (i != 0U) rt_write_char(1, ':');
        rt_write_char(1, hex[(mac[i] >> 4U) & 0x0fU]);
        rt_write_char(1, hex[mac[i] & 0x0fU]);
    }
}

static void write_summary(unsigned int sent, unsigned int received, unsigned int payload_size) {
    unsigned long long total = 0ULL;
    unsigned int i;

    rt_write_cstr(1, "summary\tsent\t");
    rt_write_uint(1, sent);
    rt_write_cstr(1, "\treceived\t");
    rt_write_uint(1, received);
    rt_write_cstr(1, "\tloss\t");
    rt_write_uint(1, sent >= received ? sent - received : 0U);
    rt_write_cstr(1, "\tpayload_bytes\t");
    rt_write_uint(1, payload_size);
    if (received == 0U) {
        rt_write_char(1, '\n');
        return;
    }
    for (i = 0U; i < received; ++i) total += latency_samples[i];
    rt_sort(latency_samples, received, sizeof(latency_samples[0]), latency_compare);
    rt_write_cstr(1, "\tmin_ns\t");
    rt_write_uint(1, latency_samples[0]);
    rt_write_cstr(1, "\tavg_ns\t");
    rt_write_uint(1, total / received);
    rt_write_cstr(1, "\tp50_ns\t");
    rt_write_uint(1, latency_samples[(received - 1U) * 50U / 100U]);
    rt_write_cstr(1, "\tp95_ns\t");
    rt_write_uint(1, latency_samples[(received - 1U) * 95U / 100U]);
    rt_write_cstr(1, "\tp99_ns\t");
    rt_write_uint(1, latency_samples[(received - 1U) * 99U / 100U]);
    rt_write_cstr(1, "\tmax_ns\t");
    rt_write_uint(1, latency_samples[received - 1U]);
    rt_write_char(1, '\n');
}

static void write_sample(unsigned long long seq, unsigned int payload_size, unsigned long long rtt_ns) {
    rt_write_cstr(1, "sample\t");
    rt_write_uint(1, seq);
    rt_write_char(1, '\t');
    rt_write_uint(1, payload_size);
    rt_write_char(1, '\t');
    rt_write_uint(1, rtt_ns);
    rt_write_char(1, '\n');
}

static int run_ping(const EtherlatOptions *options) {
    EtherlatSocket socket;
    unsigned char frame[ETHERLAT_MAX_FRAME_SIZE];
    unsigned char receive_buffer[ETHERLAT_MAX_FRAME_SIZE];
    unsigned int received = 0U;
    unsigned int seq;
    unsigned int nonce;

    if (etherlat_open_socket(&socket, options->ifname, options->ethertype, options->busy_poll_us) != 0) {
        write_error("cannot open AF_PACKET socket; root or CAP_NET_RAW is usually required");
        return 1;
    }
    nonce = (unsigned int)(platform_get_monotonic_time_ns() ^ ((unsigned long long)socket.ifindex << 16U));
    if (!options->quiet) {
        rt_write_cstr(1, "iface\t");
        rt_write_cstr(1, options->ifname);
        rt_write_cstr(1, "\tsrc\t");
        write_mac(socket.mac);
        rt_write_cstr(1, "\tdst\t");
        write_mac(options->dst_mac);
        rt_write_char(1, '\n');
    }
    if (options->samples_tsv) rt_write_line(1, "kind\tseq\tpayload_bytes\trtt_ns");

    for (seq = 0U; seq < options->count; ++seq) {
        unsigned long long send_ns = platform_get_monotonic_time_ns();
        unsigned long long deadline = send_ns + (unsigned long long)options->timeout_ms * 1000000ULL;
        int got_reply = 0;

        build_frame(frame, options->dst_mac, socket.mac, options->ethertype, ETHERLAT_OP_PING, seq, send_ns, 0ULL, options->payload_size, nonce);
        if (send_frame(&socket, options->dst_mac, frame, ETHERLAT_ETH_HEADER_SIZE + options->payload_size) < 0) {
            write_error("sendto failed");
            linux_syscall1(LINUX_SYS_CLOSE, socket.fd);
            return 1;
        }

        while (!got_reply) {
            unsigned int timeout_ms = remaining_timeout_ms(deadline);
            long bytes;
            unsigned int op;
            unsigned long long reply_seq;
            unsigned long long original_send_ns;
            unsigned long long peer_recv_ns;
            unsigned int payload_size;
            unsigned int reply_nonce;

            if (timeout_ms == 0U || etherlat_poll(socket.fd, timeout_ms) <= 0) break;
            bytes = linux_syscall6(LINUX_SYS_RECVFROM, socket.fd, (long)receive_buffer, sizeof(receive_buffer), 0, 0, 0);
            if (bytes < (long)(ETHERLAT_ETH_HEADER_SIZE + ETHERLAT_PROTOCOL_HEADER_SIZE)) continue;
            if (load_u16_be(receive_buffer + 12U) != options->ethertype || !frame_is_for_us(receive_buffer, socket.mac)) continue;
            if (parse_payload(receive_buffer + ETHERLAT_ETH_HEADER_SIZE, (size_t)bytes - ETHERLAT_ETH_HEADER_SIZE, &op, &reply_seq, &original_send_ns, &peer_recv_ns, &payload_size, &reply_nonce) != 0) continue;
            (void)peer_recv_ns;
            if (op != ETHERLAT_OP_PONG || reply_seq != seq || reply_nonce != nonce || original_send_ns != send_ns) continue;

            latency_samples[received] = platform_get_monotonic_time_ns() - send_ns;
            received += 1U;
            got_reply = 1;
            if (options->samples_tsv) write_sample(seq, payload_size, latency_samples[received - 1U]);
        }
        if (options->interval_us != 0U) platform_sleep_milliseconds((options->interval_us + 999U) / 1000U);
    }

    write_summary(options->count, received, options->payload_size);
    linux_syscall1(LINUX_SYS_CLOSE, socket.fd);
    return received == options->count ? 0 : 1;
}

static int run_discover(const EtherlatOptions *options) {
    static const unsigned char broadcast[ETHERLAT_ETH_ALEN] = { 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU };
    EtherlatSocket socket;
    unsigned char frame[ETHERLAT_MAX_FRAME_SIZE];
    unsigned char receive_buffer[ETHERLAT_MAX_FRAME_SIZE];
    unsigned int nonce;
    unsigned long long send_ns;
    unsigned long long deadline;

    if (etherlat_open_socket(&socket, options->ifname, options->ethertype, options->busy_poll_us) != 0) {
        write_error("cannot open AF_PACKET socket; root or CAP_NET_RAW is usually required");
        return 1;
    }

    send_ns = platform_get_monotonic_time_ns();
    deadline = send_ns + (unsigned long long)options->timeout_ms * 1000000ULL;
    nonce = (unsigned int)(send_ns ^ ((unsigned long long)socket.ifindex << 16U));
    build_frame(frame, broadcast, socket.mac, options->ethertype, ETHERLAT_OP_DISCOVER, 0ULL, send_ns, 0ULL, options->payload_size, nonce);
    if (send_frame(&socket, broadcast, frame, ETHERLAT_ETH_HEADER_SIZE + options->payload_size) < 0) {
        write_error("sendto failed");
        linux_syscall1(LINUX_SYS_CLOSE, socket.fd);
        return 1;
    }

    while (1) {
        unsigned int timeout_ms = remaining_timeout_ms(deadline);
        long bytes;
        unsigned int op;
        unsigned long long reply_seq;
        unsigned long long original_send_ns;
        unsigned long long peer_recv_ns;
        unsigned int payload_size;
        unsigned int reply_nonce;

        if (timeout_ms == 0U || etherlat_poll(socket.fd, timeout_ms) <= 0) break;
        bytes = linux_syscall6(LINUX_SYS_RECVFROM, socket.fd, (long)receive_buffer, sizeof(receive_buffer), 0, 0, 0);
        if (bytes < (long)(ETHERLAT_ETH_HEADER_SIZE + ETHERLAT_PROTOCOL_HEADER_SIZE)) continue;
        if (load_u16_be(receive_buffer + 12U) != options->ethertype || !frame_is_for_us(receive_buffer, socket.mac)) continue;
        if (parse_payload(receive_buffer + ETHERLAT_ETH_HEADER_SIZE, (size_t)bytes - ETHERLAT_ETH_HEADER_SIZE, &op, &reply_seq, &original_send_ns, &peer_recv_ns, &payload_size, &reply_nonce) != 0) continue;
        (void)reply_seq;
        (void)peer_recv_ns;
        (void)payload_size;
        if (op != ETHERLAT_OP_INFO || reply_nonce != nonce || original_send_ns != send_ns) continue;
        if (!options->quiet) rt_write_cstr(1, "peer\tmac\t");
        write_mac(receive_buffer + 6U);
        rt_write_char(1, '\n');
        linux_syscall1(LINUX_SYS_CLOSE, socket.fd);
        return 0;
    }

    write_error("no etherlat responder discovered");
    linux_syscall1(LINUX_SYS_CLOSE, socket.fd);
    return 1;
}

static int run_listen(const EtherlatOptions *options) {
    EtherlatSocket socket;
    unsigned char receive_buffer[ETHERLAT_MAX_FRAME_SIZE];
    unsigned char reply[ETHERLAT_MAX_FRAME_SIZE];
    unsigned int replies = 0U;

    if (etherlat_open_socket(&socket, options->ifname, options->ethertype, options->busy_poll_us) != 0) {
        write_error("cannot open AF_PACKET socket; root or CAP_NET_RAW is usually required");
        return 1;
    }
    if (!options->quiet) {
        rt_write_cstr(1, "listening\tiface\t");
        rt_write_cstr(1, options->ifname);
        rt_write_cstr(1, "\tmac\t");
        write_mac(socket.mac);
        rt_write_char(1, '\n');
    }

    while (options->count == 0U || replies < options->count) {
        long bytes = linux_syscall6(LINUX_SYS_RECVFROM, socket.fd, (long)receive_buffer, sizeof(receive_buffer), 0, 0, 0);
        unsigned int op;
        unsigned long long seq;
        unsigned long long send_ns;
        unsigned long long peer_recv_ns;
        unsigned int payload_size;
        unsigned int nonce;
        unsigned long long now;

        if (bytes < (long)(ETHERLAT_ETH_HEADER_SIZE + ETHERLAT_PROTOCOL_HEADER_SIZE)) continue;
        if (load_u16_be(receive_buffer + 12U) != options->ethertype || !frame_is_for_us(receive_buffer, socket.mac)) continue;
        if (parse_payload(receive_buffer + ETHERLAT_ETH_HEADER_SIZE, (size_t)bytes - ETHERLAT_ETH_HEADER_SIZE, &op, &seq, &send_ns, &peer_recv_ns, &payload_size, &nonce) != 0) continue;
        (void)peer_recv_ns;
        if ((op != ETHERLAT_OP_PING && op != ETHERLAT_OP_DISCOVER) || payload_size > ETHERLAT_MAX_PAYLOAD_SIZE) continue;
        now = platform_get_monotonic_time_ns();
        build_frame(reply, receive_buffer + 6U, socket.mac, options->ethertype, op == ETHERLAT_OP_DISCOVER ? ETHERLAT_OP_INFO : ETHERLAT_OP_PONG, seq, send_ns, now, payload_size, nonce);
        if (send_frame(&socket, receive_buffer + 6U, reply, ETHERLAT_ETH_HEADER_SIZE + payload_size) >= 0) replies += 1U;
    }

    linux_syscall1(LINUX_SYS_CLOSE, socket.fd);
    return 0;
}

int main(int argc, char **argv) {
    EtherlatOptions options;
    int parse_result = parse_options(argc, argv, &options);

    if (parse_result > 0) return 0;
    if (parse_result < 0) return 2;
    if (text_starts_with(options.mode, "ping")) return run_ping(&options);
    if (rt_strcmp(options.mode, "discover") == 0) return run_discover(&options);
    return run_listen(&options);
}