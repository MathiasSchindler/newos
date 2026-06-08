#include "crypto/md5.h"
#include "platform.h"
#include "runtime.h"
#include "stdint.h"

#define CONTROLLED_PREFIX_SIZE 128U
#define CONTROLLED_PAYLOAD_SIZE 128U

static const unsigned char controlled_elf_prefix[CONTROLLED_PREFIX_SIZE] = {
    0x7f, 0x45, 0x4c, 0x46, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x3e, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x20, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x38, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const unsigned char collision_payload_a[CONTROLLED_PAYLOAD_SIZE] = {
    0x51, 0xdd, 0x83, 0xa1, 0xd7, 0x28, 0x11, 0x16, 0xce, 0x8b, 0xa9, 0xc6, 0x41, 0xe7, 0x29, 0xa8,
    0x55, 0x24, 0xa4, 0xf8, 0xf7, 0x50, 0xf2, 0xd4, 0xf5, 0x89, 0x7f, 0xc2, 0x08, 0xb2, 0x3a, 0x9f,
    0x47, 0x13, 0xcf, 0xcb, 0xd0, 0xbe, 0x86, 0xcf, 0xef, 0x3b, 0x4c, 0x64, 0xe2, 0x53, 0xa2, 0x72,
    0x9d, 0xc0, 0x8c, 0x18, 0x97, 0x00, 0xaf, 0xfa, 0xb2, 0x89, 0xf9, 0xad, 0xc0, 0x33, 0x66, 0x29,
    0x59, 0x53, 0xad, 0x6e, 0xc9, 0x7d, 0xf1, 0xf6, 0xcc, 0x50, 0xd0, 0xaa, 0x68, 0x23, 0x6b, 0x8e,
    0x32, 0xf1, 0x3a, 0xb5, 0x74, 0x89, 0x52, 0xfd, 0x0e, 0xc9, 0xa9, 0xf6, 0xcb, 0x0b, 0x61, 0xbd,
    0x6d, 0xc8, 0x72, 0x6a, 0xfd, 0xb9, 0xdd, 0xa7, 0xa2, 0x5d, 0xf5, 0x69, 0xa1, 0x64, 0x15, 0x9e,
    0xbb, 0x04, 0x22, 0x81, 0xc3, 0xd0, 0xe3, 0xf5, 0xb9, 0x2c, 0x9a, 0xf3, 0x40, 0x0d, 0xdd, 0xc8
};

static const unsigned char collision_payload_b[CONTROLLED_PAYLOAD_SIZE] = {
    0x51, 0xdd, 0x83, 0xa1, 0xd7, 0x28, 0x11, 0x16, 0xce, 0x8b, 0xa9, 0xc6, 0x41, 0xe7, 0x29, 0xa8,
    0x55, 0x24, 0xa4, 0x78, 0xf7, 0x50, 0xf2, 0xd4, 0xf5, 0x89, 0x7f, 0xc2, 0x08, 0xb2, 0x3a, 0x9f,
    0x47, 0x13, 0xcf, 0xcb, 0xd0, 0xbe, 0x86, 0xcf, 0xef, 0x3b, 0x4c, 0x64, 0xe2, 0xd3, 0xa2, 0x72,
    0x9d, 0xc0, 0x8c, 0x18, 0x97, 0x00, 0xaf, 0xfa, 0xb2, 0x89, 0xf9, 0x2d, 0xc0, 0x33, 0x66, 0x29,
    0x59, 0x53, 0xad, 0x6e, 0xc9, 0x7d, 0xf1, 0xf6, 0xcc, 0x50, 0xd0, 0xaa, 0x68, 0x23, 0x6b, 0x8e,
    0x32, 0xf1, 0x3a, 0x35, 0x74, 0x89, 0x52, 0xfd, 0x0e, 0xc9, 0xa9, 0xf6, 0xcb, 0x0b, 0x61, 0xbd,
    0x6d, 0xc8, 0x72, 0x6a, 0xfd, 0xb9, 0xdd, 0xa7, 0xa2, 0x5d, 0xf5, 0x69, 0xa1, 0xe4, 0x14, 0x9e,
    0xbb, 0x04, 0x22, 0x81, 0xc3, 0xd0, 0xe3, 0xf5, 0xb9, 0x2c, 0x9a, 0x73, 0x40, 0x0d, 0xdd, 0xc8
};

typedef struct {
    int quiet;
    const char *prefix_path;
    const char *out_a_path;
    const char *out_b_path;
} Options;

static void write_text(int fd, const char *text) {
    (void)rt_write_cstr(fd, text);
}

static void write_error(const char *message, const char *detail) {
    write_text(2, "controlled-fastcoll: ");
    write_text(2, message);
    if (detail != 0) {
        write_text(2, detail);
    }
    write_text(2, "\n");
}

static void digest_to_hex(const unsigned char digest[CRYPTO_MD5_DIGEST_SIZE], char hex[CRYPTO_MD5_DIGEST_SIZE * 2U + 1U]) {
    static const char digits[] = "0123456789abcdef";
    size_t offset;

    for (offset = 0; offset < CRYPTO_MD5_DIGEST_SIZE; ++offset) {
        hex[offset * 2U] = digits[(digest[offset] >> 4) & 0x0fU];
        hex[offset * 2U + 1U] = digits[digest[offset] & 0x0fU];
    }
    hex[CRYPTO_MD5_DIGEST_SIZE * 2U] = '\0';
}

static int parse_options(int argc, char **argv, Options *options) {
    int argi;

    memset(options, 0, sizeof(*options));
    for (argi = 1; argi < argc; ++argi) {
        const char *arg = argv[argi];

        if (rt_strcmp(arg, "-q") == 0 || rt_strcmp(arg, "--quiet") == 0) {
            options->quiet = 1;
        } else if (rt_strcmp(arg, "-p") == 0 || rt_strcmp(arg, "--prefixfile") == 0) {
            if (argi + 1 >= argc) {
                write_error("missing prefix path after ", arg);
                return 1;
            }
            options->prefix_path = argv[++argi];
        } else if (rt_strcmp(arg, "-o") == 0 || rt_strcmp(arg, "--out") == 0) {
            if (argi + 2 >= argc) {
                write_error("-o needs two output paths", 0);
                return 1;
            }
            options->out_a_path = argv[++argi];
            options->out_b_path = argv[++argi];
            if (argi + 1 != argc) {
                write_error("-o must be followed by the final two arguments", 0);
                return 1;
            }
        } else if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--help") == 0) {
            write_text(1, "usage: controlled-fastcoll [-q] -p PREFIX -o OUT1 OUT2\n");
            return 2;
        } else {
            write_error("unknown option ", arg);
            return 1;
        }
    }
    if (options->prefix_path == 0 || options->out_a_path == 0 || options->out_b_path == 0) {
        write_error("usage: controlled-fastcoll [-q] -p PREFIX -o OUT1 OUT2", 0);
        return 1;
    }
    return 0;
}

static int read_exact_prefix(const char *path, unsigned char prefix[CONTROLLED_PREFIX_SIZE]) {
    int fd = platform_open_read(path);
    size_t offset = 0U;
    unsigned char extra;

    if (fd < 0) {
        write_error("cannot open prefix ", path);
        return 1;
    }
    while (offset < CONTROLLED_PREFIX_SIZE) {
        long n = platform_read(fd, prefix + offset, CONTROLLED_PREFIX_SIZE - offset);

        if (n <= 0) {
            write_error("cannot read complete controlled prefix from ", path);
            (void)platform_close(fd);
            return 1;
        }
        offset += (size_t)n;
    }
    if (platform_read(fd, &extra, 1U) != 0) {
        write_error("prefix is not exactly the controlled 128-byte ELF prefix: ", path);
        (void)platform_close(fd);
        return 1;
    }
    if (platform_close(fd) != 0) {
        write_error("cannot close prefix ", path);
        return 1;
    }
    return 0;
}

static void md5_prefix_payload(const unsigned char *prefix, const unsigned char *payload, unsigned char digest[CRYPTO_MD5_DIGEST_SIZE]) {
    CryptoMd5Context context;

    crypto_md5_init(&context);
    crypto_md5_update(&context, prefix, CONTROLLED_PREFIX_SIZE);
    crypto_md5_update(&context, payload, CONTROLLED_PAYLOAD_SIZE);
    crypto_md5_final(&context, digest);
}

static int write_output(const char *path, const unsigned char *prefix, const unsigned char *payload) {
    int fd = platform_open_write(path, 0644U);

    if (fd < 0) {
        write_error("cannot open output ", path);
        return 1;
    }
    if (rt_write_all(fd, prefix, CONTROLLED_PREFIX_SIZE) != 0 ||
        rt_write_all(fd, payload, CONTROLLED_PAYLOAD_SIZE) != 0) {
        write_error("cannot write output ", path);
        (void)platform_close(fd);
        return 1;
    }
    if (platform_close(fd) != 0) {
        write_error("cannot close output ", path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    Options options;
    unsigned char prefix[CONTROLLED_PREFIX_SIZE];
    unsigned char digest_a[CRYPTO_MD5_DIGEST_SIZE];
    unsigned char digest_b[CRYPTO_MD5_DIGEST_SIZE];
    char hex[CRYPTO_MD5_DIGEST_SIZE * 2U + 1U];
    int parse_result = parse_options(argc, argv, &options);

    if (parse_result == 2) {
        return 0;
    }
    if (parse_result != 0) {
        return 1;
    }
    if (read_exact_prefix(options.prefix_path, prefix) != 0) {
        return 1;
    }
    if (memcmp(prefix, controlled_elf_prefix, CONTROLLED_PREFIX_SIZE) != 0) {
        write_error("unsupported prefix; this tool only matches the controlled ELF demo prefix", 0);
        return 1;
    }

    md5_prefix_payload(prefix, collision_payload_a, digest_a);
    md5_prefix_payload(prefix, collision_payload_b, digest_b);
    if (memcmp(digest_a, digest_b, CRYPTO_MD5_DIGEST_SIZE) != 0 ||
        memcmp(collision_payload_a, collision_payload_b, CONTROLLED_PAYLOAD_SIZE) == 0) {
        write_error("internal controlled collision payload is invalid", 0);
        return 1;
    }

    if (write_output(options.out_a_path, prefix, collision_payload_a) != 0 ||
        write_output(options.out_b_path, prefix, collision_payload_b) != 0) {
        return 1;
    }

    if (!options.quiet) {
        digest_to_hex(digest_a, hex);
        write_text(1, "same md5: ");
        write_text(1, hex);
        write_text(1, "\n");
    }
    return 0;
}