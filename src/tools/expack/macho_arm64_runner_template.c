typedef unsigned char ExpackRunnerU8;
typedef unsigned int ExpackRunnerU32;
typedef unsigned long long ExpackRunnerU64;

#define EXPACK_RUNNER_CODEC_LZSS 0U
#define EXPACK_RUNNER_CODEC_LZREP 3U
#define EXPACK_RUNNER_CODEC_LZ4 7U
#define EXPACK_RUNNER_METADATA_SIZE 48ULL
#define EXPACK_RUNNER_MIN_MATCH 3U

#define EXPACK_DARWIN_SYS_EXIT 1
#define EXPACK_DARWIN_SYS_WRITE 4
#define EXPACK_DARWIN_SYS_OPEN 5
#define EXPACK_DARWIN_SYS_CLOSE 6
#define EXPACK_DARWIN_SYS_GETPID 20
#define EXPACK_DARWIN_SYS_EXECVE 59
#define EXPACK_DARWIN_SYS_MMAP 197

#define EXPACK_DARWIN_PROT_READ 1
#define EXPACK_DARWIN_PROT_WRITE 2
#define EXPACK_DARWIN_MAP_PRIVATE 2
#define EXPACK_DARWIN_MAP_ANON 0x1000
#define EXPACK_DARWIN_O_WRONLY 0x0001
#define EXPACK_DARWIN_O_CREAT 0x0200
#define EXPACK_DARWIN_O_TRUNC 0x0400

#ifndef EXPACK_MACHO_RUNNER_CODEC
#define EXPACK_MACHO_RUNNER_CODEC EXPACK_RUNNER_CODEC_LZ4
#endif

__asm__(
    ".section __TEXT,__text,regular,pure_instructions\n"
    ".globl _expack_runner_entry\n"
    "_expack_runner_entry:\n"
    "b _start\n"
    ".p2align 3\n"
    "L_expack_payload_delta:\n"
    ".quad 0x1122334455667788\n"
    "L_expack_payload_size_value:\n"
    ".quad 0x8877665544332211\n"
    "L_expack_original_size_value:\n"
    ".quad 0x0102030405060708\n"
    ".globl _expack_runner_payload_delta_slot\n"
    "_expack_runner_payload_delta_slot:\n"
    "adr x0, L_expack_payload_delta\n"
    "ret\n"
    ".globl _expack_runner_payload_size_slot\n"
    "_expack_runner_payload_size_slot:\n"
    "adr x0, L_expack_payload_size_value\n"
    "ret\n"
    ".globl _expack_runner_original_size_slot\n"
    "_expack_runner_original_size_slot:\n"
    "adr x0, L_expack_original_size_value\n"
    "ret\n");

const ExpackRunnerU64 *expack_runner_payload_delta_slot(void);
const ExpackRunnerU64 *expack_runner_payload_size_slot(void);
const ExpackRunnerU64 *expack_runner_original_size_slot(void);

static long expack_runner_syscall1(long number, long arg0) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = arg0;

    __asm__ volatile("svc #0x80\n\tcneg %0, %0, cs" : "+r"(x0), "+r"(x16) : : "memory", "cc");
    return x0;
}

static long expack_runner_syscall3(long number, long arg0, long arg1, long arg2) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;

    __asm__ volatile("svc #0x80\n\tcneg %0, %0, cs" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x16) : : "memory", "cc");
    return x0;
}

static long expack_runner_syscall6(long number, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;
    register long x3 __asm__("x3") = arg3;
    register long x4 __asm__("x4") = arg4;
    register long x5 __asm__("x5") = arg5;

    __asm__ volatile("svc #0x80\n\tcneg %0, %0, cs" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x16) : : "memory", "cc");
    return x0;
}

static void expack_runner_exit127(void) {
    expack_runner_syscall1(EXPACK_DARWIN_SYS_EXIT, 127);
    for (;;) {
    }
}

static ExpackRunnerU32 expack_runner_read_u32_le(const ExpackRunnerU8 *data) {
    return (ExpackRunnerU32)data[0] |
           ((ExpackRunnerU32)data[1] << 8U) |
           ((ExpackRunnerU32)data[2] << 16U) |
           ((ExpackRunnerU32)data[3] << 24U);
}

static void expack_runner_write_all(int fd, const ExpackRunnerU8 *data, ExpackRunnerU64 size) {
    while (size != 0ULL) {
        long chunk = size > 0x40000000ULL ? 0x40000000L : (long)size;
        long written = expack_runner_syscall3(EXPACK_DARWIN_SYS_WRITE, fd, (long)data, chunk);

        if (written <= 0) {
            expack_runner_exit127();
        }
        data += written;
        size -= (ExpackRunnerU64)written;
    }
}

static char expack_runner_hex_digit(ExpackRunnerU32 value) {
    value &= 0x0fU;
    return (char)(value < 10U ? ('0' + value) : ('a' + value - 10U));
}

static void expack_runner_append_hex8(char *out, ExpackRunnerU32 value) {
    int index;

    for (index = 0; index < 8; ++index) {
        out[index] = expack_runner_hex_digit(value >> (28U - (ExpackRunnerU32)index * 4U));
    }
}

#if EXPACK_MACHO_RUNNER_CODEC == EXPACK_RUNNER_CODEC_LZSS
static int expack_runner_lzss_length_bits(ExpackRunnerU32 profile_id) {
    if (profile_id == 0U) return 3;
    if (profile_id == 1U) return 4;
    if (profile_id == 2U) return 5;
    if (profile_id == 3U) return 6;
    return -1;
}

static int expack_runner_decode_payload(const ExpackRunnerU8 *payload, ExpackRunnerU64 payload_size, ExpackRunnerU8 *output, ExpackRunnerU64 output_size) {
    const ExpackRunnerU8 *metadata = payload - EXPACK_RUNNER_METADATA_SIZE;
    ExpackRunnerU64 input_offset = 0ULL;
    ExpackRunnerU64 output_offset = 0ULL;
    int length_bits = expack_runner_lzss_length_bits(expack_runner_read_u32_le(metadata + 32U));
    ExpackRunnerU32 length_mask;

    if (length_bits < 0) {
        return -1;
    }
    length_mask = (1U << (ExpackRunnerU32)length_bits) - 1U;
    while (output_offset < output_size) {
        ExpackRunnerU8 flags;
        ExpackRunnerU32 bit;

        if (input_offset >= payload_size) {
            return -1;
        }
        flags = payload[input_offset++];
        for (bit = 0U; bit < 8U && output_offset < output_size; ++bit) {
            if ((flags & (ExpackRunnerU8)(1U << bit)) == 0U) {
                if (input_offset >= payload_size) {
                    return -1;
                }
                output[output_offset++] = payload[input_offset++];
            } else {
                ExpackRunnerU32 token;
                ExpackRunnerU32 distance;
                ExpackRunnerU32 length;
                ExpackRunnerU32 index;

                if (input_offset + 2ULL > payload_size) {
                    return -1;
                }
                token = (ExpackRunnerU32)payload[input_offset] | ((ExpackRunnerU32)payload[input_offset + 1ULL] << 8U);
                input_offset += 2ULL;
                distance = ((token & 0x00ffU) | ((token >> (8U + (ExpackRunnerU32)length_bits)) << 8U)) + 1U;
                length = ((token >> 8U) & length_mask) + EXPACK_RUNNER_MIN_MATCH;
                if ((ExpackRunnerU64)distance > output_offset || (ExpackRunnerU64)length > output_size - output_offset) {
                    return -1;
                }
                for (index = 0U; index < length; ++index) {
                    output[output_offset] = output[output_offset - (ExpackRunnerU64)distance];
                    output_offset += 1ULL;
                }
            }
        }
    }
    return input_offset == payload_size ? 0 : -1;
}
#elif EXPACK_MACHO_RUNNER_CODEC == EXPACK_RUNNER_CODEC_LZREP
static int expack_runner_decode_payload(const ExpackRunnerU8 *payload, ExpackRunnerU64 payload_size, ExpackRunnerU8 *output, ExpackRunnerU64 output_size) {
    ExpackRunnerU64 input_offset = 0ULL;
    ExpackRunnerU64 output_offset = 0ULL;
    ExpackRunnerU32 last_distance = 1U;

    while (output_offset < output_size) {
        ExpackRunnerU8 flags;
        ExpackRunnerU32 bit;

        if (input_offset >= payload_size) {
            return -1;
        }
        flags = payload[input_offset++];
        for (bit = 0U; bit < 8U && output_offset < output_size; ++bit) {
            if ((flags & (ExpackRunnerU8)(1U << bit)) == 0U) {
                if (input_offset >= payload_size) {
                    return -1;
                }
                output[output_offset++] = payload[input_offset++];
            } else {
                ExpackRunnerU8 token;
                ExpackRunnerU32 length;
                ExpackRunnerU32 distance;
                ExpackRunnerU32 index;

                if (input_offset >= payload_size) {
                    return -1;
                }
                token = payload[input_offset++];
                if ((token & 0x80U) != 0U) {
                    length = (ExpackRunnerU32)(token & 0x7fU) + EXPACK_RUNNER_MIN_MATCH;
                    distance = last_distance;
                } else {
                    if (input_offset >= payload_size) {
                        return -1;
                    }
                    length = (ExpackRunnerU32)(token & 0x0fU) + EXPACK_RUNNER_MIN_MATCH;
                    distance = ((((ExpackRunnerU32)token >> 4U) << 8U) | (ExpackRunnerU32)payload[input_offset++]) + 1U;
                    last_distance = distance;
                }
                if (distance == 0U || (ExpackRunnerU64)distance > output_offset || (ExpackRunnerU64)length > output_size - output_offset) {
                    return -1;
                }
                for (index = 0U; index < length; ++index) {
                    output[output_offset] = output[output_offset - (ExpackRunnerU64)distance];
                    output_offset += 1ULL;
                }
            }
        }
    }
    return input_offset == payload_size ? 0 : -1;
}
#elif EXPACK_MACHO_RUNNER_CODEC == EXPACK_RUNNER_CODEC_LZ4
static int expack_runner_decode_payload(const ExpackRunnerU8 *payload, ExpackRunnerU64 payload_size, ExpackRunnerU8 *output, ExpackRunnerU64 output_size) {
    ExpackRunnerU64 input_offset = 0ULL;
    ExpackRunnerU64 output_offset = 0ULL;

    while (output_offset < output_size) {
        ExpackRunnerU8 token;
        ExpackRunnerU64 literal_length;
        ExpackRunnerU64 match_length;
        ExpackRunnerU32 distance;
        ExpackRunnerU64 index;

        if (input_offset >= payload_size) {
            return -1;
        }
        token = payload[input_offset++];
        literal_length = (ExpackRunnerU64)(token >> 4U);
        if (literal_length == 15ULL) {
            ExpackRunnerU8 extra;
            do {
                if (input_offset >= payload_size) {
                    return -1;
                }
                extra = payload[input_offset++];
                literal_length += (ExpackRunnerU64)extra;
            } while (extra == 255U);
        }
        if (literal_length > output_size - output_offset || literal_length > payload_size - input_offset) {
            return -1;
        }
        for (index = 0ULL; index < literal_length; ++index) {
            output[output_offset++] = payload[input_offset++];
        }
        if (output_offset >= output_size) {
            break;
        }
        if (input_offset + 2ULL > payload_size) {
            return -1;
        }
        distance = (ExpackRunnerU32)payload[input_offset] | ((ExpackRunnerU32)payload[input_offset + 1ULL] << 8U);
        input_offset += 2ULL;
        if (distance == 0U || (ExpackRunnerU64)distance > output_offset) {
            return -1;
        }
        match_length = (ExpackRunnerU64)(token & 0x0fU) + 4ULL;
        if ((token & 0x0fU) == 15U) {
            ExpackRunnerU8 extra;
            do {
                if (input_offset >= payload_size) {
                    return -1;
                }
                extra = payload[input_offset++];
                match_length += (ExpackRunnerU64)extra;
            } while (extra == 255U);
        }
        if (match_length > output_size - output_offset) {
            return -1;
        }
        for (index = 0ULL; index < match_length; ++index) {
            output[output_offset] = output[output_offset - (ExpackRunnerU64)distance];
            output_offset += 1ULL;
        }
    }
    return input_offset == payload_size ? 0 : -1;
}
#else
#error unsupported Mach-O runner codec
#endif

static void expack_runner_make_path(char *path) {
    volatile char *out = path;

    out[0] = '/'; out[1] = 't'; out[2] = 'm'; out[3] = 'p'; out[4] = '/';
    out[5] = 'e'; out[6] = 'x'; out[7] = 'p'; out[8] = 'a'; out[9] = 'c';
    out[10] = 'k'; out[11] = '-'; out[12] = 'r'; out[13] = 'u'; out[14] = 'n'; out[15] = '-';
    expack_runner_append_hex8(path + 16, (ExpackRunnerU32)expack_runner_syscall1(EXPACK_DARWIN_SYS_GETPID, 0));
    out[24] = 0;
}

void start(int argc, char **argv, char **envp) {
    const ExpackRunnerU64 *payload_delta_slot = expack_runner_payload_delta_slot();
    const ExpackRunnerU8 *payload = (const ExpackRunnerU8 *)payload_delta_slot + *payload_delta_slot;
    ExpackRunnerU64 payload_size = *expack_runner_payload_size_slot();
    ExpackRunnerU64 original_size = *expack_runner_original_size_slot();
    ExpackRunnerU8 *image;
    char path[32];
    int fd;

    (void)argc;
    image = (ExpackRunnerU8 *)expack_runner_syscall6(EXPACK_DARWIN_SYS_MMAP, 0, (long)original_size,
        EXPACK_DARWIN_PROT_READ | EXPACK_DARWIN_PROT_WRITE, EXPACK_DARWIN_MAP_PRIVATE | EXPACK_DARWIN_MAP_ANON, -1, 0);
    if ((long)image <= 0) {
        expack_runner_exit127();
    }
    if (expack_runner_decode_payload(payload, payload_size, image, original_size) < 0) {
        expack_runner_exit127();
    }
    expack_runner_make_path(path);
    fd = (int)expack_runner_syscall3(EXPACK_DARWIN_SYS_OPEN, (long)path,
        EXPACK_DARWIN_O_WRONLY | EXPACK_DARWIN_O_CREAT | EXPACK_DARWIN_O_TRUNC, 0700);
    if (fd < 0) {
        expack_runner_exit127();
    }
    expack_runner_write_all(fd, image, original_size);
    expack_runner_syscall1(EXPACK_DARWIN_SYS_CLOSE, fd);
    argv[0] = path;
    expack_runner_syscall3(EXPACK_DARWIN_SYS_EXECVE, (long)path, (long)argv, (long)envp);
    expack_runner_exit127();
}