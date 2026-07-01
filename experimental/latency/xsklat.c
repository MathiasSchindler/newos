#include "../../src/platform/linux/common.h"

#if !defined(LINUX_SYS_SOCKET) && defined(__aarch64__)
#include "../../src/arch/aarch64/linux/syscall.h"
#elif !defined(LINUX_SYS_SOCKET)
#include "../../src/arch/x86_64/linux/syscall.h"
#endif

#if !defined(LINUX_SYS_GETSOCKOPT) && defined(__aarch64__)
#define LINUX_SYS_GETSOCKOPT 209
#elif !defined(LINUX_SYS_GETSOCKOPT)
#define LINUX_SYS_GETSOCKOPT 55
#endif

#define XSKLAT_AF_PACKET 17
#define XSKLAT_AF_XDP 44
#define XSKLAT_SOCK_RAW 3
#define XSKLAT_SOL_XDP 283
#define XSKLAT_XDP_COPY (1U << 1U)
#define XSKLAT_XDP_MMAP_OFFSETS 1
#define XSKLAT_XDP_TX_RING 3
#define XSKLAT_XDP_UMEM_REG 4
#define XSKLAT_XDP_UMEM_FILL_RING 5
#define XSKLAT_XDP_UMEM_COMPLETION_RING 6
#define XSKLAT_XDP_PGOFF_TX_RING 0x80000000ULL
#define XSKLAT_XDP_UMEM_PGOFF_FILL_RING 0x100000000ULL
#define XSKLAT_XDP_UMEM_PGOFF_COMPLETION_RING 0x180000000ULL
#define XSKLAT_SIOCGIFHWADDR 0x8927U
#define XSKLAT_SIOCGIFINDEX 0x8933U
#define XSKLAT_ETH_ALEN 6U
#define XSKLAT_ETH_HEADER_SIZE 14U
#define XSKLAT_PROTOCOL_HEADER_SIZE 40U
#define XSKLAT_MIN_PAYLOAD_SIZE 46U
#define XSKLAT_MAX_PAYLOAD_SIZE 1500U
#define XSKLAT_MAX_FRAME_SIZE (XSKLAT_ETH_HEADER_SIZE + XSKLAT_MAX_PAYLOAD_SIZE)
#define XSKLAT_DEFAULT_ETHERTYPE 0x88b5U
#define XSKLAT_DEFAULT_COUNT 1000U
#define XSKLAT_DEFAULT_TIMEOUT_MS 1000U
#define XSKLAT_MAX_SAMPLES 100000U
#define XSKLAT_OP_PING 1U
#define XSKLAT_OP_PONG 2U
#define XSKLAT_POLLIN 0x0001
#define XSKLAT_SOCK_CLOEXEC LINUX_SOCK_CLOEXEC
#define XSKLAT_PAGE_SIZE 4096U
#define XSKLAT_RING_ENTRIES 64U
#define XSKLAT_CHUNK_SIZE 2048U
#define XSKLAT_UMEM_SIZE (XSKLAT_RING_ENTRIES * XSKLAT_CHUNK_SIZE)
#define XSKLAT_MSG_DONTWAIT 0x40

typedef struct {
    unsigned short sa_family;
    unsigned char sa_data[14];
} XsklatSockaddr;

typedef struct {
    char ifr_name[16];
    union {
        XsklatSockaddr hwaddr;
        int ifindex;
        unsigned char pad[24];
    } data;
} XsklatIfreq;

typedef struct {
    unsigned short sll_family;
    unsigned short sll_protocol;
    int sll_ifindex;
    unsigned short sll_hatype;
    unsigned char sll_pkttype;
    unsigned char sll_halen;
    unsigned char sll_addr[8];
} XsklatSockaddrLl;

typedef struct {
    unsigned short sxdp_family;
    unsigned short sxdp_flags;
    unsigned int sxdp_ifindex;
    unsigned int sxdp_queue_id;
    unsigned int sxdp_shared_umem_fd;
} XsklatSockaddrXdp;

typedef struct {
    unsigned long long addr;
    unsigned long long len;
    unsigned int chunk_size;
    unsigned int headroom;
    unsigned int flags;
    unsigned int tx_metadata_len;
} XsklatUmemReg;

typedef struct {
    unsigned long long producer;
    unsigned long long consumer;
    unsigned long long desc;
    unsigned long long flags;
} XsklatRingOffset;

typedef struct {
    XsklatRingOffset rx;
    XsklatRingOffset tx;
    XsklatRingOffset fr;
    XsklatRingOffset cr;
} XsklatMmapOffsets;

typedef struct {
    unsigned long long addr;
    unsigned int len;
    unsigned int options;
} XsklatDesc;

typedef struct {
    int fd;
    short events;
    short revents;
} XsklatPollfd;

typedef struct {
    unsigned int *producer;
    unsigned int *consumer;
    unsigned int *flags;
    void *desc;
    unsigned int mask;
} XsklatRing;

typedef struct {
    int fd;
    void *umem;
    size_t umem_size;
    void *tx_map;
    size_t tx_map_size;
    void *fill_map;
    size_t fill_map_size;
    void *completion_map;
    size_t completion_map_size;
    XsklatRing tx;
    XsklatRing fill;
    XsklatRing completion;
    unsigned long long free_addrs[XSKLAT_RING_ENTRIES];
    unsigned int free_count;
    unsigned int inflight;
} XsklatXsk;

typedef struct {
    int fd;
    int ifindex;
    unsigned char mac[XSKLAT_ETH_ALEN];
    unsigned int ethertype;
} XsklatPacketSocket;

typedef struct {
    const char *mode;
    const char *ifname;
    unsigned char dst_mac[XSKLAT_ETH_ALEN];
    int has_dst_mac;
    unsigned int queue_id;
    unsigned int ethertype;
    unsigned int count;
    unsigned int payload_size;
    unsigned int timeout_ms;
    unsigned int interval_us;
    int samples_tsv;
    int quiet;
} XsklatOptions;

static unsigned long long latency_samples[XSKLAT_MAX_SAMPLES];
static const char *xsklat_last_setup_step;
static long xsklat_last_setup_errno;

static void compiler_barrier(void) {
    __asm__ volatile("" ::: "memory");
}

static unsigned short xsklat_bswap16(unsigned short value) {
    return (unsigned short)(((value & 0x00ffU) << 8U) | ((value & 0xff00U) >> 8U));
}

static unsigned short xsklat_htons(unsigned int value) {
    return xsklat_bswap16((unsigned short)value);
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
    rt_write_line(1, "usage: xsklat ping -i IFACE --dst MAC [--queue N] [--count N] [--size BYTES]");
    rt_write_line(1, "       xsklat ping -i IFACE --dst MAC [--timeout-ms MS] [--interval-us USEC] [--samples]");
    rt_write_line(1, "       xsklat sends with AF_XDP copy-mode TX and receives replies with AF_PACKET");
}

static void write_error(const char *message) {
    rt_write_cstr(2, "xsklat: ");
    rt_write_line(2, message);
}

static void write_error2(const char *message, const char *value) {
    rt_write_cstr(2, "xsklat: ");
    rt_write_cstr(2, message);
    rt_write_line(2, value);
}

static void write_error_errno(const char *message, const char *step, long code) {
    rt_write_cstr(2, "xsklat: ");
    rt_write_cstr(2, message);
    rt_write_cstr(2, step);
    rt_write_cstr(2, " errno ");
    rt_write_uint(2, code < 0 ? (unsigned long long)-code : (unsigned long long)code);
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

static int parse_mac(const char *text, unsigned char mac[XSKLAT_ETH_ALEN]) {
    size_t i;

    for (i = 0U; i < XSKLAT_ETH_ALEN; ++i) {
        int high = hex_value(text[i * 3U]);
        int low = hex_value(text[i * 3U + 1U]);
        if (high < 0 || low < 0) return -1;
        mac[i] = (unsigned char)((high << 4) | low);
        if (i + 1U < XSKLAT_ETH_ALEN) {
            if (text[i * 3U + 2U] != ':') return -1;
        } else if (text[i * 3U + 2U] != '\0') {
            return -1;
        }
    }
    return 0;
}

static const char *inline_option_value(const char *arg, const char *prefix) {
    size_t prefix_len = rt_strlen(prefix);

    return rt_strncmp(arg, prefix, prefix_len) == 0 ? arg + prefix_len : 0;
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

static int parse_options(int argc, char **argv, XsklatOptions *options) {
    int i;

    memset(options, 0, sizeof(*options));
    options->ethertype = XSKLAT_DEFAULT_ETHERTYPE;
    options->count = XSKLAT_DEFAULT_COUNT;
    options->payload_size = XSKLAT_MIN_PAYLOAD_SIZE;
    options->timeout_ms = XSKLAT_DEFAULT_TIMEOUT_MS;

    if (argc < 2 || rt_strcmp(argv[1], "--help") == 0 || rt_strcmp(argv[1], "-h") == 0) {
        write_usage();
        return 1;
    }
    if (rt_strcmp(argv[1], "ping") != 0) {
        write_usage();
        return -1;
    }
    options->mode = argv[1];

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
        } else if (rt_strcmp(arg, "--queue") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_uint_option(value, &options->queue_id) != 0) return -1;
        } else if ((value = inline_option_value(arg, "--queue=")) != 0) {
            if (parse_uint_option(value, &options->queue_id) != 0) return -1;
        } else if (rt_strcmp(arg, "--count") == 0 || rt_strcmp(arg, "-c") == 0) {
            value = next_arg_value(argc, argv, &i, arg);
            if (value == 0 || parse_uint_option(value, &options->count) != 0 || options->count > XSKLAT_MAX_SAMPLES) return -1;
        } else if ((value = inline_option_value(arg, "--count=")) != 0) {
            if (parse_uint_option(value, &options->count) != 0 || options->count > XSKLAT_MAX_SAMPLES) return -1;
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
    if (!options->has_dst_mac) {
        write_error("ping requires --dst MAC");
        return -1;
    }
    if (options->payload_size < XSKLAT_PROTOCOL_HEADER_SIZE) options->payload_size = XSKLAT_PROTOCOL_HEADER_SIZE;
    if (options->payload_size < XSKLAT_MIN_PAYLOAD_SIZE) options->payload_size = XSKLAT_MIN_PAYLOAD_SIZE;
    if (options->payload_size > XSKLAT_MAX_PAYLOAD_SIZE) {
        write_error("payload size exceeds Ethernet MTU payload cap of 1500 bytes");
        return -1;
    }
    return 0;
}

static void build_payload(unsigned char *payload, unsigned int op, unsigned long long seq, unsigned long long send_ns, unsigned long long peer_recv_ns, unsigned int payload_size, unsigned int nonce) {
    unsigned int i;

    payload[0] = 'E';
    payload[1] = 'L';
    payload[2] = 'A';
    payload[3] = 'T';
    payload[4] = 1U;
    payload[5] = (unsigned char)op;
    store_u16_be(payload + 6U, XSKLAT_PROTOCOL_HEADER_SIZE);
    store_u64_be(payload + 8U, seq);
    store_u64_be(payload + 16U, send_ns);
    store_u64_be(payload + 24U, peer_recv_ns);
    store_u32_be(payload + 32U, payload_size);
    store_u32_be(payload + 36U, nonce);
    for (i = XSKLAT_PROTOCOL_HEADER_SIZE; i < payload_size; ++i) payload[i] = (unsigned char)(seq + (unsigned long long)i);
}

static int parse_payload(const unsigned char *payload, size_t length, unsigned int *op_out, unsigned long long *seq_out, unsigned long long *send_ns_out, unsigned long long *peer_recv_ns_out, unsigned int *payload_size_out, unsigned int *nonce_out) {
    unsigned int header_size;
    unsigned int payload_size;

    if (length < XSKLAT_PROTOCOL_HEADER_SIZE || payload[0] != 'E' || payload[1] != 'L' || payload[2] != 'A' || payload[3] != 'T' || payload[4] != 1U) return -1;
    header_size = load_u16_be(payload + 6U);
    payload_size = load_u32_be(payload + 32U);
    if (header_size < XSKLAT_PROTOCOL_HEADER_SIZE || payload_size > length) return -1;
    *op_out = payload[5];
    *seq_out = load_u64_be(payload + 8U);
    *send_ns_out = load_u64_be(payload + 16U);
    *peer_recv_ns_out = load_u64_be(payload + 24U);
    *payload_size_out = payload_size;
    *nonce_out = load_u32_be(payload + 36U);
    return 0;
}

static void build_frame(unsigned char *frame, const unsigned char dst[XSKLAT_ETH_ALEN], const unsigned char src[XSKLAT_ETH_ALEN], unsigned int ethertype, unsigned int op, unsigned long long seq, unsigned long long send_ns, unsigned long long peer_recv_ns, unsigned int payload_size, unsigned int nonce) {
    memcpy(frame, dst, XSKLAT_ETH_ALEN);
    memcpy(frame + 6U, src, XSKLAT_ETH_ALEN);
    store_u16_be(frame + 12U, ethertype);
    build_payload(frame + XSKLAT_ETH_HEADER_SIZE, op, seq, send_ns, peer_recv_ns, payload_size, nonce);
}

static int packet_open(XsklatPacketSocket *socket_out, const char *ifname, unsigned int ethertype) {
    XsklatIfreq ifreq;
    XsklatSockaddrLl bind_addr;
    long fd;

    memset(socket_out, 0, sizeof(*socket_out));
    socket_out->fd = -1;
    fd = linux_syscall3(LINUX_SYS_SOCKET, XSKLAT_AF_PACKET, XSKLAT_SOCK_RAW | XSKLAT_SOCK_CLOEXEC, (long)xsklat_htons(ethertype));
    if (fd == -LINUX_EINVAL) fd = linux_syscall3(LINUX_SYS_SOCKET, XSKLAT_AF_PACKET, XSKLAT_SOCK_RAW, (long)xsklat_htons(ethertype));
    if (fd < 0) return -1;
    socket_out->fd = (int)fd;

    memset(&ifreq, 0, sizeof(ifreq));
    rt_copy_string(ifreq.ifr_name, sizeof(ifreq.ifr_name), ifname);
    if (linux_syscall3(LINUX_SYS_IOCTL, fd, XSKLAT_SIOCGIFINDEX, (long)&ifreq) < 0) goto fail;
    socket_out->ifindex = ifreq.data.ifindex;

    memset(&ifreq, 0, sizeof(ifreq));
    rt_copy_string(ifreq.ifr_name, sizeof(ifreq.ifr_name), ifname);
    if (linux_syscall3(LINUX_SYS_IOCTL, fd, XSKLAT_SIOCGIFHWADDR, (long)&ifreq) < 0) goto fail;
    memcpy(socket_out->mac, ifreq.data.hwaddr.sa_data, XSKLAT_ETH_ALEN);

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sll_family = XSKLAT_AF_PACKET;
    bind_addr.sll_protocol = xsklat_htons(ethertype);
    bind_addr.sll_ifindex = socket_out->ifindex;
    if (linux_syscall3(LINUX_SYS_BIND, fd, (long)&bind_addr, sizeof(bind_addr)) < 0) goto fail;
    socket_out->ethertype = ethertype;
    return 0;

fail:
    linux_syscall1(LINUX_SYS_CLOSE, fd);
    socket_out->fd = -1;
    return -1;
}

static void packet_close(XsklatPacketSocket *socket) {
    if (socket->fd >= 0) linux_syscall1(LINUX_SYS_CLOSE, socket->fd);
    socket->fd = -1;
}

static int xsk_ring_mmap(XsklatRing *ring, int fd, const XsklatRingOffset *offset, unsigned int entries, unsigned long long pgoff, size_t desc_size, void **map_out, size_t *map_size_out) {
    size_t map_size = (size_t)offset->desc + (size_t)entries * desc_size;
    long mapped;

    if (map_size < XSKLAT_PAGE_SIZE) map_size = XSKLAT_PAGE_SIZE;
    mapped = linux_syscall6(LINUX_SYS_MMAP, 0, (long)map_size, LINUX_PROT_READ | LINUX_PROT_WRITE, LINUX_MAP_SHARED, fd, (long)pgoff);
    if (mapped < 0) return -1;
    *map_out = (void *)mapped;
    *map_size_out = map_size;
    ring->producer = (unsigned int *)((unsigned char *)mapped + offset->producer);
    ring->consumer = (unsigned int *)((unsigned char *)mapped + offset->consumer);
    ring->flags = (unsigned int *)((unsigned char *)mapped + offset->flags);
    ring->desc = (void *)((unsigned char *)mapped + offset->desc);
    ring->mask = entries - 1U;
    return 0;
}

static void xsk_close(XsklatXsk *xsk) {
    if (xsk->tx_map != 0) linux_syscall2(LINUX_SYS_MUNMAP, (long)xsk->tx_map, (long)xsk->tx_map_size);
    if (xsk->fill_map != 0) linux_syscall2(LINUX_SYS_MUNMAP, (long)xsk->fill_map, (long)xsk->fill_map_size);
    if (xsk->completion_map != 0) linux_syscall2(LINUX_SYS_MUNMAP, (long)xsk->completion_map, (long)xsk->completion_map_size);
    if (xsk->umem != 0) linux_syscall2(LINUX_SYS_MUNMAP, (long)xsk->umem, (long)xsk->umem_size);
    if (xsk->fd >= 0) linux_syscall1(LINUX_SYS_CLOSE, xsk->fd);
    memset(xsk, 0, sizeof(*xsk));
    xsk->fd = -1;
}

static int xsk_open(XsklatXsk *xsk, int ifindex, unsigned int queue_id) {
    XsklatUmemReg umem_reg;
    XsklatMmapOffsets offsets;
    XsklatSockaddrXdp bind_addr;
    unsigned int ring_entries = XSKLAT_RING_ENTRIES;
    unsigned int optlen = sizeof(offsets);
    unsigned int i;
    long fd;
    long mapped;
    long result;

    xsklat_last_setup_step = "socket";
    xsklat_last_setup_errno = 0;
    memset(xsk, 0, sizeof(*xsk));
    xsk->fd = -1;
    fd = linux_syscall3(LINUX_SYS_SOCKET, XSKLAT_AF_XDP, XSKLAT_SOCK_RAW | XSKLAT_SOCK_CLOEXEC, 0);
    if (fd == -LINUX_EINVAL) fd = linux_syscall3(LINUX_SYS_SOCKET, XSKLAT_AF_XDP, XSKLAT_SOCK_RAW, 0);
    if (fd < 0) {
        xsklat_last_setup_errno = fd;
        return -1;
    }
    xsk->fd = (int)fd;

    xsklat_last_setup_step = "mmap umem";
    mapped = linux_syscall6(LINUX_SYS_MMAP, 0, XSKLAT_UMEM_SIZE, LINUX_PROT_READ | LINUX_PROT_WRITE, LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS, -1, 0);
    if (mapped < 0) {
        xsklat_last_setup_errno = mapped;
        goto fail;
    }
    xsk->umem = (void *)mapped;
    xsk->umem_size = XSKLAT_UMEM_SIZE;

    memset(&umem_reg, 0, sizeof(umem_reg));
    umem_reg.addr = (unsigned long long)(unsigned long)xsk->umem;
    umem_reg.len = XSKLAT_UMEM_SIZE;
    umem_reg.chunk_size = XSKLAT_CHUNK_SIZE;
    xsklat_last_setup_step = "setsockopt XDP_UMEM_REG";
    result = linux_syscall5(LINUX_SYS_SETSOCKOPT, fd, XSKLAT_SOL_XDP, XSKLAT_XDP_UMEM_REG, (long)&umem_reg, sizeof(umem_reg));
    if (result < 0) { xsklat_last_setup_errno = result; goto fail; }
    xsklat_last_setup_step = "setsockopt XDP_TX_RING";
    result = linux_syscall5(LINUX_SYS_SETSOCKOPT, fd, XSKLAT_SOL_XDP, XSKLAT_XDP_TX_RING, (long)&ring_entries, sizeof(ring_entries));
    if (result < 0) { xsklat_last_setup_errno = result; goto fail; }
    xsklat_last_setup_step = "setsockopt XDP_UMEM_FILL_RING";
    result = linux_syscall5(LINUX_SYS_SETSOCKOPT, fd, XSKLAT_SOL_XDP, XSKLAT_XDP_UMEM_FILL_RING, (long)&ring_entries, sizeof(ring_entries));
    if (result < 0) { xsklat_last_setup_errno = result; goto fail; }
    xsklat_last_setup_step = "setsockopt XDP_UMEM_COMPLETION_RING";
    result = linux_syscall5(LINUX_SYS_SETSOCKOPT, fd, XSKLAT_SOL_XDP, XSKLAT_XDP_UMEM_COMPLETION_RING, (long)&ring_entries, sizeof(ring_entries));
    if (result < 0) { xsklat_last_setup_errno = result; goto fail; }
    xsklat_last_setup_step = "getsockopt XDP_MMAP_OFFSETS";
    result = linux_syscall5(LINUX_SYS_GETSOCKOPT, fd, XSKLAT_SOL_XDP, XSKLAT_XDP_MMAP_OFFSETS, (long)&offsets, (long)&optlen);
    if (result < 0) { xsklat_last_setup_errno = result; goto fail; }
    xsklat_last_setup_step = "mmap XDP_TX_RING";
    if (xsk_ring_mmap(&xsk->tx, (int)fd, &offsets.tx, ring_entries, XSKLAT_XDP_PGOFF_TX_RING, sizeof(XsklatDesc), &xsk->tx_map, &xsk->tx_map_size) != 0) goto fail;
    xsklat_last_setup_step = "mmap XDP_UMEM_FILL_RING";
    if (xsk_ring_mmap(&xsk->fill, (int)fd, &offsets.fr, ring_entries, XSKLAT_XDP_UMEM_PGOFF_FILL_RING, sizeof(unsigned long long), &xsk->fill_map, &xsk->fill_map_size) != 0) goto fail;
    xsklat_last_setup_step = "mmap XDP_UMEM_COMPLETION_RING";
    if (xsk_ring_mmap(&xsk->completion, (int)fd, &offsets.cr, ring_entries, XSKLAT_XDP_UMEM_PGOFF_COMPLETION_RING, sizeof(unsigned long long), &xsk->completion_map, &xsk->completion_map_size) != 0) goto fail;

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sxdp_family = XSKLAT_AF_XDP;
    bind_addr.sxdp_flags = XSKLAT_XDP_COPY;
    bind_addr.sxdp_ifindex = (unsigned int)ifindex;
    bind_addr.sxdp_queue_id = queue_id;
    xsklat_last_setup_step = "bind AF_XDP copy";
    result = linux_syscall3(LINUX_SYS_BIND, fd, (long)&bind_addr, sizeof(bind_addr));
    if (result == -LINUX_EINVAL) {
        bind_addr.sxdp_flags = 0;
        xsklat_last_setup_step = "bind AF_XDP auto";
        result = linux_syscall3(LINUX_SYS_BIND, fd, (long)&bind_addr, sizeof(bind_addr));
    }
    if (result < 0) { xsklat_last_setup_errno = result; goto fail; }

    for (i = 0U; i < XSKLAT_RING_ENTRIES; ++i) xsk->free_addrs[i] = (unsigned long long)i * XSKLAT_CHUNK_SIZE;
    xsk->free_count = XSKLAT_RING_ENTRIES;
    return 0;

fail:
    xsk_close(xsk);
    return -1;
}

static void xsk_reclaim_completions(XsklatXsk *xsk) {
    unsigned int consumer = *xsk->completion.consumer;
    unsigned int producer = *xsk->completion.producer;
    unsigned long long *completion = (unsigned long long *)xsk->completion.desc;

    compiler_barrier();
    while (consumer != producer && xsk->free_count < XSKLAT_RING_ENTRIES) {
        xsk->free_addrs[xsk->free_count++] = completion[consumer & xsk->completion.mask];
        consumer += 1U;
        if (xsk->inflight > 0U) xsk->inflight -= 1U;
    }
    compiler_barrier();
    *xsk->completion.consumer = consumer;
}

static int xsk_wait_free_slot(XsklatXsk *xsk) {
    unsigned int spin;

    for (spin = 0U; spin < 1000000U; ++spin) {
        unsigned int producer;
        unsigned int consumer;

        xsk_reclaim_completions(xsk);
        producer = *xsk->tx.producer;
        consumer = *xsk->tx.consumer;
        if (xsk->free_count > 0U && producer - consumer < XSKLAT_RING_ENTRIES) return 0;
        (void)linux_syscall6(LINUX_SYS_SENDTO, xsk->fd, 0, 0, XSKLAT_MSG_DONTWAIT, 0, 0);
    }
    return -1;
}

static int xsk_send_frame(XsklatXsk *xsk, const unsigned char *frame, unsigned int frame_size) {
    XsklatDesc *descs = (XsklatDesc *)xsk->tx.desc;
    unsigned int producer;
    unsigned int index;
    unsigned long long addr;

    if (frame_size > XSKLAT_CHUNK_SIZE) return -1;
    if (xsk_wait_free_slot(xsk) != 0) return -1;
    addr = xsk->free_addrs[--xsk->free_count];
    memcpy((unsigned char *)xsk->umem + addr, frame, frame_size);
    producer = *xsk->tx.producer;
    index = producer & xsk->tx.mask;
    descs[index].addr = addr;
    descs[index].len = frame_size;
    descs[index].options = 0U;
    compiler_barrier();
    *xsk->tx.producer = producer + 1U;
    xsk->inflight += 1U;
    if (linux_syscall6(LINUX_SYS_SENDTO, xsk->fd, 0, 0, XSKLAT_MSG_DONTWAIT, 0, 0) < 0) return -1;
    return 0;
}

static int packet_poll(int fd, unsigned int timeout_ms) {
    XsklatPollfd pollfd;
    struct linux_timespec timeout;
    long result;

    pollfd.fd = fd;
    pollfd.events = XSKLAT_POLLIN;
    pollfd.revents = 0;
    timeout.tv_sec = (long)(timeout_ms / 1000U);
    timeout.tv_nsec = (long)((timeout_ms % 1000U) * 1000000U);
    result = linux_syscall5(LINUX_SYS_PPOLL, (long)&pollfd, 1, (long)&timeout, 0, 0);
    if (result <= 0) return (int)result;
    return (pollfd.revents & XSKLAT_POLLIN) != 0 ? 1 : -1;
}

static unsigned int remaining_timeout_ms(unsigned long long deadline_ns) {
    unsigned long long now = platform_get_monotonic_time_ns();
    unsigned long long remaining;

    if (now >= deadline_ns) return 0U;
    remaining = deadline_ns - now;
    remaining = (remaining + 999999ULL) / 1000000ULL;
    return remaining > 0xffffffffULL ? 0xffffffffU : (unsigned int)remaining;
}

static int mac_is_broadcast(const unsigned char mac[XSKLAT_ETH_ALEN]) {
    size_t i;

    for (i = 0U; i < XSKLAT_ETH_ALEN; ++i) {
        if (mac[i] != 0xffU) return 0;
    }
    return 1;
}

static int frame_is_for_us(const unsigned char *frame, const unsigned char mac[XSKLAT_ETH_ALEN]) {
    return memcmp(frame, mac, XSKLAT_ETH_ALEN) == 0 || mac_is_broadcast(frame);
}

static int latency_compare(const void *left, const void *right) {
    const unsigned long long lhs = *(const unsigned long long *)left;
    const unsigned long long rhs = *(const unsigned long long *)right;

    return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
}

static void write_mac(const unsigned char mac[XSKLAT_ETH_ALEN]) {
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0U; i < XSKLAT_ETH_ALEN; ++i) {
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

static int run_ping(const XsklatOptions *options) {
    XsklatPacketSocket packet_socket;
    XsklatXsk xsk;
    unsigned char frame[XSKLAT_MAX_FRAME_SIZE];
    unsigned char receive_buffer[XSKLAT_MAX_FRAME_SIZE];
    unsigned int received = 0U;
    unsigned int seq;
    unsigned int nonce;

    memset(&xsk, 0, sizeof(xsk));
    xsk.fd = -1;
    if (packet_open(&packet_socket, options->ifname, options->ethertype) != 0) {
        write_error("cannot open AF_PACKET socket; root or CAP_NET_RAW is usually required");
        return 1;
    }
    if (xsk_open(&xsk, packet_socket.ifindex, options->queue_id) != 0) {
        write_error_errno("cannot open AF_XDP TX socket at ", xsklat_last_setup_step != 0 ? xsklat_last_setup_step : "unknown step", xsklat_last_setup_errno);
        packet_close(&packet_socket);
        return 1;
    }
    nonce = (unsigned int)(platform_get_monotonic_time_ns() ^ ((unsigned long long)packet_socket.ifindex << 16U));
    if (!options->quiet) {
        rt_write_cstr(1, "iface\t");
        rt_write_cstr(1, options->ifname);
        rt_write_cstr(1, "\tqueue\t");
        rt_write_uint(1, options->queue_id);
        rt_write_cstr(1, "\tsrc\t");
        write_mac(packet_socket.mac);
        rt_write_cstr(1, "\tdst\t");
        write_mac(options->dst_mac);
        rt_write_char(1, '\n');
    }
    if (options->samples_tsv) rt_write_line(1, "kind\tseq\tpayload_bytes\trtt_ns");

    for (seq = 0U; seq < options->count; ++seq) {
        unsigned long long send_ns = platform_get_monotonic_time_ns();
        unsigned long long deadline = send_ns + (unsigned long long)options->timeout_ms * 1000000ULL;
        int got_reply = 0;

        build_frame(frame, options->dst_mac, packet_socket.mac, options->ethertype, XSKLAT_OP_PING, seq, send_ns, 0ULL, options->payload_size, nonce);
        if (xsk_send_frame(&xsk, frame, XSKLAT_ETH_HEADER_SIZE + options->payload_size) != 0) {
            write_error("AF_XDP send failed");
            xsk_close(&xsk);
            packet_close(&packet_socket);
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

            if (timeout_ms == 0U || packet_poll(packet_socket.fd, timeout_ms) <= 0) break;
            bytes = linux_syscall6(LINUX_SYS_RECVFROM, packet_socket.fd, (long)receive_buffer, sizeof(receive_buffer), 0, 0, 0);
            if (bytes < (long)(XSKLAT_ETH_HEADER_SIZE + XSKLAT_PROTOCOL_HEADER_SIZE)) continue;
            if (load_u16_be(receive_buffer + 12U) != options->ethertype || !frame_is_for_us(receive_buffer, packet_socket.mac)) continue;
            if (parse_payload(receive_buffer + XSKLAT_ETH_HEADER_SIZE, (size_t)bytes - XSKLAT_ETH_HEADER_SIZE, &op, &reply_seq, &original_send_ns, &peer_recv_ns, &payload_size, &reply_nonce) != 0) continue;
            (void)peer_recv_ns;
            if (op != XSKLAT_OP_PONG || reply_seq != seq || reply_nonce != nonce || original_send_ns != send_ns) continue;
            latency_samples[received] = platform_get_monotonic_time_ns() - send_ns;
            received += 1U;
            got_reply = 1;
            if (options->samples_tsv) write_sample(seq, payload_size, latency_samples[received - 1U]);
        }
        if (options->interval_us != 0U) platform_sleep_milliseconds((options->interval_us + 999U) / 1000U);
    }

    write_summary(options->count, received, options->payload_size);
    xsk_close(&xsk);
    packet_close(&packet_socket);
    return received == options->count ? 0 : 1;
}

int main(int argc, char **argv) {
    XsklatOptions options;
    int parse_result = parse_options(argc, argv, &options);

    if (parse_result > 0) return 0;
    if (parse_result < 0) return 2;
    return run_ping(&options);
}