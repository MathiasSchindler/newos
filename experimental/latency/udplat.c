#include "../../src/platform/linux/common.h"

#if !defined(LINUX_SYS_SOCKET) && defined(__aarch64__)
#include "../../src/arch/aarch64/linux/syscall.h"
#elif !defined(LINUX_SYS_SOCKET)
#include "../../src/arch/x86_64/linux/syscall.h"
#endif

#define UDPLAT_AF_INET 2
#define UDPLAT_SOCK_DGRAM 2
#define UDPLAT_IPPROTO_UDP 17
#define UDPLAT_SOL_SOCKET 1
#define UDPLAT_SO_REUSEADDR 2
#define UDPLAT_SO_BUSY_POLL 46
#define UDPLAT_DEFAULT_PORT 17777U
#define UDPLAT_DEFAULT_COUNT 1000U
#define UDPLAT_DEFAULT_TIMEOUT_MS 1000U
#define UDPLAT_PROTOCOL_HEADER_SIZE 40U
#define UDPLAT_DEFAULT_PAYLOAD_SIZE 46U
#define UDPLAT_MAX_PAYLOAD_SIZE 1400U
#define UDPLAT_MAX_SAMPLES 100000U
#define UDPLAT_OP_PING 1U
#define UDPLAT_OP_PONG 2U
#define UDPLAT_POLLIN 0x0001

typedef struct {
    unsigned char bytes[4];
} UdplatInAddr;

typedef struct {
    unsigned short sin_family;
    unsigned short sin_port;
    UdplatInAddr sin_addr;
    unsigned char sin_zero[8];
} UdplatSockaddrIn;

typedef struct {
    int fd;
    short events;
    short revents;
} UdplatPollfd;

typedef struct {
    const char *mode;
    int has_host;
    UdplatInAddr host;
    UdplatInAddr bind_addr;
    unsigned int port;
    unsigned int count;
    unsigned int payload_size;
    unsigned int timeout_ms;
    unsigned int interval_us;
    unsigned int busy_poll_us;
    int samples_tsv;
    int quiet;
} UdplatOptions;

static unsigned long long latency_samples[UDPLAT_MAX_SAMPLES];

static unsigned short udplat_bswap16(unsigned short value) {
    return (unsigned short)(((value & 0x00ffU) << 8U) | ((value & 0xff00U) >> 8U));
}

static unsigned short udplat_htons(unsigned int value) {
    return udplat_bswap16((unsigned short)value);
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

static void write_usage(void) {
    rt_write_line(1, "usage: udplat listen [--bind IPv4] [--port PORT] [--count N] [--busy-poll-us USEC]");
    rt_write_line(1, "       udplat ping IPv4 [--port PORT] [--count N] [--size BYTES] [--timeout-ms MS] [--samples]");
    rt_write_line(1, "       udplat ping IPv4 [--interval-us USEC] [--busy-poll-us USEC]");
}

static void write_error(const char *message) {
    rt_write_cstr(2, "udplat: ");
    rt_write_line(2, message);
}

static void write_error2(const char *message, const char *value) {
    rt_write_cstr(2, "udplat: ");
    rt_write_cstr(2, message);
    rt_write_line(2, value);
}

static void write_error_uint(const char *message, unsigned long long value) {
    rt_write_cstr(2, "udplat: ");
    rt_write_cstr(2, message);
    rt_write_uint(2, value);
    rt_write_char(2, '\n');
}

static int parse_uint_option(const char *text, unsigned int *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value > 0xffffffffULL) return -1;
    *value_out = (unsigned int)value;
    return 0;
}

static int parse_ipv4(const char *text, UdplatInAddr *addr_out) {
    unsigned char bytes[4];
    size_t byte_index = 0U;
    size_t i = 0U;

    while (byte_index < 4U) {
        unsigned int value = 0U;
        unsigned int digits = 0U;

        while (text[i] >= '0' && text[i] <= '9') {
            value = value * 10U + (unsigned int)(text[i] - '0');
            if (value > 255U) return -1;
            digits += 1U;
            i += 1U;
        }
        if (digits == 0U) return -1;
        bytes[byte_index++] = (unsigned char)value;
        if (byte_index == 4U) break;
        if (text[i] != '.') return -1;
        i += 1U;
    }
    if (text[i] != '\0') return -1;
    memcpy(addr_out->bytes, bytes, sizeof(bytes));
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

static int parse_options(int argc, char **argv, UdplatOptions *options) {
    int i;

    memset(options, 0, sizeof(*options));
    options->port = UDPLAT_DEFAULT_PORT;
    options->count = UDPLAT_DEFAULT_COUNT;
    options->payload_size = UDPLAT_DEFAULT_PAYLOAD_SIZE;
    options->timeout_ms = UDPLAT_DEFAULT_TIMEOUT_MS;

    if (argc < 2 || rt_strcmp(argv[1], "--help") == 0 || rt_strcmp(argv[1], "-h") == 0) {
        write_usage();
        return 1;
    }
    if (rt_strcmp(argv[1], "ping") != 0 && rt_strcmp(argv[1], "listen") != 0) {
        write_usage();
        return -1;
    }
    options->mode = argv[1];
    if (rt_strcmp(options->mode, "listen") == 0) options->count = 0U;

    for (i = 2; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = 0;

        if (rt_strcmp(arg, "--bind") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_ipv4(value, &options->bind_addr) != 0) return -1;
        } else if ((value = inline_option_value(arg, "--bind=")) != 0) {
            if (parse_ipv4(value, &options->bind_addr) != 0) return -1;
        } else if (rt_strcmp(arg, "--host") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_ipv4(value, &options->host) != 0) return -1;
            options->has_host = 1;
        } else if ((value = inline_option_value(arg, "--host=")) != 0) {
            if (parse_ipv4(value, &options->host) != 0) return -1;
            options->has_host = 1;
        } else if (rt_strcmp(arg, "--port") == 0 || rt_strcmp(arg, "-p") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_uint_option(value, &options->port) != 0 || options->port == 0U || options->port > 65535U) return -1;
        } else if ((value = inline_option_value(arg, "--port=")) != 0) {
            if (parse_uint_option(value, &options->port) != 0 || options->port == 0U || options->port > 65535U) return -1;
        } else if (rt_strcmp(arg, "--count") == 0 || rt_strcmp(arg, "-c") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_uint_option(value, &options->count) != 0 || options->count > UDPLAT_MAX_SAMPLES) return -1;
        } else if ((value = inline_option_value(arg, "--count=")) != 0) {
            if (parse_uint_option(value, &options->count) != 0 || options->count > UDPLAT_MAX_SAMPLES) return -1;
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
        } else if (arg[0] != '-' && rt_strcmp(options->mode, "ping") == 0 && !options->has_host) {
            if (parse_ipv4(arg, &options->host) != 0) return -1;
            options->has_host = 1;
        } else {
            write_error2("unknown option ", arg);
            return -1;
        }
    }

    if (rt_strcmp(options->mode, "ping") == 0 && !options->has_host) {
        write_error("ping requires an IPv4 host");
        return -1;
    }
    if (options->payload_size < UDPLAT_PROTOCOL_HEADER_SIZE) options->payload_size = UDPLAT_PROTOCOL_HEADER_SIZE;
    if (options->payload_size > UDPLAT_MAX_PAYLOAD_SIZE) {
        write_error("payload size exceeds UDP benchmark cap of 1400 bytes");
        return -1;
    }
    return 0;
}

static void prepare_sockaddr(UdplatSockaddrIn *address, const UdplatInAddr *ip, unsigned int port) {
    memset(address, 0, sizeof(*address));
    address->sin_family = UDPLAT_AF_INET;
    address->sin_port = udplat_htons(port);
    if (ip != 0) address->sin_addr = *ip;
}

static int open_udp_socket(unsigned int busy_poll_us) {
    long fd = linux_syscall3(LINUX_SYS_SOCKET, UDPLAT_AF_INET, UDPLAT_SOCK_DGRAM | LINUX_SOCK_CLOEXEC, UDPLAT_IPPROTO_UDP);
    int enabled = 1;

    if (fd == -LINUX_EINVAL) fd = linux_syscall3(LINUX_SYS_SOCKET, UDPLAT_AF_INET, UDPLAT_SOCK_DGRAM, UDPLAT_IPPROTO_UDP);
    if (fd < 0) return -1;
    (void)linux_syscall5(LINUX_SYS_SETSOCKOPT, fd, UDPLAT_SOL_SOCKET, UDPLAT_SO_REUSEADDR, (long)&enabled, sizeof(enabled));
    if (busy_poll_us != 0U) {
        int value = (int)busy_poll_us;
        if (linux_syscall5(LINUX_SYS_SETSOCKOPT, fd, UDPLAT_SOL_SOCKET, UDPLAT_SO_BUSY_POLL, (long)&value, sizeof(value)) < 0) {
            write_error_uint("cannot set SO_BUSY_POLL to ", busy_poll_us);
            linux_syscall1(LINUX_SYS_CLOSE, fd);
            return -1;
        }
    }
    return (int)fd;
}

static int bind_udp_socket(int fd, const UdplatInAddr *bind_addr, unsigned int port) {
    UdplatSockaddrIn address;

    prepare_sockaddr(&address, bind_addr, port);
    return linux_syscall3(LINUX_SYS_BIND, fd, (long)&address, sizeof(address)) < 0 ? -1 : 0;
}

static void build_payload(unsigned char *payload, unsigned int op, unsigned long long seq, unsigned long long send_ns, unsigned long long peer_recv_ns, unsigned int payload_size, unsigned int nonce) {
    unsigned int i;

    payload[0] = 'U';
    payload[1] = 'L';
    payload[2] = 'A';
    payload[3] = 'T';
    payload[4] = 1U;
    payload[5] = (unsigned char)op;
    store_u16_be(payload + 6U, UDPLAT_PROTOCOL_HEADER_SIZE);
    store_u64_be(payload + 8U, seq);
    store_u64_be(payload + 16U, send_ns);
    store_u64_be(payload + 24U, peer_recv_ns);
    store_u32_be(payload + 32U, payload_size);
    store_u32_be(payload + 36U, nonce);
    for (i = UDPLAT_PROTOCOL_HEADER_SIZE; i < payload_size; ++i) payload[i] = (unsigned char)(seq + (unsigned long long)i);
}

static int parse_payload(const unsigned char *payload, size_t length, unsigned int *op_out, unsigned long long *seq_out, unsigned long long *send_ns_out, unsigned long long *peer_recv_ns_out, unsigned int *payload_size_out, unsigned int *nonce_out) {
    unsigned int header_size;
    unsigned int payload_size;

    if (length < UDPLAT_PROTOCOL_HEADER_SIZE || payload[0] != 'U' || payload[1] != 'L' || payload[2] != 'A' || payload[3] != 'T' || payload[4] != 1U) return -1;
    header_size = load_u16_be(payload + 6U);
    payload_size = load_u32_be(payload + 32U);
    if (header_size < UDPLAT_PROTOCOL_HEADER_SIZE || payload_size > length) return -1;
    *op_out = payload[5];
    *seq_out = load_u64_be(payload + 8U);
    *send_ns_out = load_u64_be(payload + 16U);
    *peer_recv_ns_out = load_u64_be(payload + 24U);
    *payload_size_out = payload_size;
    *nonce_out = load_u32_be(payload + 36U);
    return 0;
}

static int udplat_poll(int fd, unsigned int timeout_ms) {
    UdplatPollfd pollfd;
    struct linux_timespec timeout;
    long result;

    pollfd.fd = fd;
    pollfd.events = UDPLAT_POLLIN;
    pollfd.revents = 0;
    timeout.tv_sec = (long)(timeout_ms / 1000U);
    timeout.tv_nsec = (long)((timeout_ms % 1000U) * 1000000U);
    result = linux_syscall5(LINUX_SYS_PPOLL, (long)&pollfd, 1, (long)&timeout, 0, 0);
    if (result <= 0) return (int)result;
    return (pollfd.revents & UDPLAT_POLLIN) != 0 ? 1 : -1;
}

static unsigned int remaining_timeout_ms(unsigned long long deadline_ns) {
    unsigned long long now = platform_get_monotonic_time_ns();
    unsigned long long remaining;

    if (now >= deadline_ns) return 0U;
    remaining = (deadline_ns - now + 999999ULL) / 1000000ULL;
    return remaining > 0xffffffffULL ? 0xffffffffU : (unsigned int)remaining;
}

static int latency_compare(const void *left, const void *right) {
    const unsigned long long lhs = *(const unsigned long long *)left;
    const unsigned long long rhs = *(const unsigned long long *)right;

    return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
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

static int run_ping(const UdplatOptions *options) {
    int fd = open_udp_socket(options->busy_poll_us);
    UdplatSockaddrIn peer;
    unsigned char buffer[UDPLAT_MAX_PAYLOAD_SIZE];
    unsigned char reply[UDPLAT_MAX_PAYLOAD_SIZE];
    unsigned int received = 0U;
    unsigned int seq;
    unsigned int nonce;

    if (fd < 0) {
        write_error("cannot open UDP socket");
        return 1;
    }
    prepare_sockaddr(&peer, &options->host, options->port);
    nonce = (unsigned int)platform_get_monotonic_time_ns();
    if (options->samples_tsv) rt_write_line(1, "kind\tseq\tpayload_bytes\trtt_ns");
    for (seq = 0U; seq < options->count; ++seq) {
        unsigned long long send_ns = platform_get_monotonic_time_ns();
        unsigned long long deadline = send_ns + (unsigned long long)options->timeout_ms * 1000000ULL;
        int got_reply = 0;

        build_payload(buffer, UDPLAT_OP_PING, seq, send_ns, 0ULL, options->payload_size, nonce);
        if (linux_syscall6(LINUX_SYS_SENDTO, fd, (long)buffer, options->payload_size, 0, (long)&peer, sizeof(peer)) < 0) {
            write_error("sendto failed");
            linux_syscall1(LINUX_SYS_CLOSE, fd);
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

            if (timeout_ms == 0U || udplat_poll(fd, timeout_ms) <= 0) break;
            bytes = linux_syscall6(LINUX_SYS_RECVFROM, fd, (long)reply, sizeof(reply), 0, 0, 0);
            if (bytes < (long)UDPLAT_PROTOCOL_HEADER_SIZE) continue;
            if (parse_payload(reply, (size_t)bytes, &op, &reply_seq, &original_send_ns, &peer_recv_ns, &payload_size, &reply_nonce) != 0) continue;
            (void)peer_recv_ns;
            if (op != UDPLAT_OP_PONG || reply_seq != seq || reply_nonce != nonce || original_send_ns != send_ns) continue;
            latency_samples[received] = platform_get_monotonic_time_ns() - send_ns;
            received += 1U;
            got_reply = 1;
            if (options->samples_tsv) write_sample(seq, payload_size, latency_samples[received - 1U]);
        }
        if (options->interval_us != 0U) platform_sleep_milliseconds((options->interval_us + 999U) / 1000U);
    }
    write_summary(options->count, received, options->payload_size);
    linux_syscall1(LINUX_SYS_CLOSE, fd);
    return received == options->count ? 0 : 1;
}

static int run_listen(const UdplatOptions *options) {
    int fd = open_udp_socket(options->busy_poll_us);
    unsigned char buffer[UDPLAT_MAX_PAYLOAD_SIZE];
    unsigned char reply[UDPLAT_MAX_PAYLOAD_SIZE];
    unsigned int replies = 0U;

    if (fd < 0 || bind_udp_socket(fd, &options->bind_addr, options->port) != 0) {
        write_error("cannot bind UDP socket");
        if (fd >= 0) linux_syscall1(LINUX_SYS_CLOSE, fd);
        return 1;
    }
    while (options->count == 0U || replies < options->count) {
        UdplatSockaddrIn peer;
        unsigned int peer_len = sizeof(peer);
        long bytes = linux_syscall6(LINUX_SYS_RECVFROM, fd, (long)buffer, sizeof(buffer), 0, (long)&peer, (long)&peer_len);
        unsigned int op;
        unsigned long long seq;
        unsigned long long send_ns;
        unsigned long long peer_recv_ns;
        unsigned int payload_size;
        unsigned int nonce;

        if (bytes < (long)UDPLAT_PROTOCOL_HEADER_SIZE) continue;
        if (parse_payload(buffer, (size_t)bytes, &op, &seq, &send_ns, &peer_recv_ns, &payload_size, &nonce) != 0) continue;
        (void)peer_recv_ns;
        if (op != UDPLAT_OP_PING || payload_size > UDPLAT_MAX_PAYLOAD_SIZE) continue;
        build_payload(reply, UDPLAT_OP_PONG, seq, send_ns, platform_get_monotonic_time_ns(), payload_size, nonce);
        if (linux_syscall6(LINUX_SYS_SENDTO, fd, (long)reply, payload_size, 0, (long)&peer, peer_len) >= 0) replies += 1U;
    }
    linux_syscall1(LINUX_SYS_CLOSE, fd);
    return 0;
}

int main(int argc, char **argv) {
    UdplatOptions options;
    int parse_result = parse_options(argc, argv, &options);

    if (parse_result > 0) return 0;
    if (parse_result < 0) return 2;
    if (rt_strcmp(options.mode, "ping") == 0) return run_ping(&options);
    return run_listen(&options);
}