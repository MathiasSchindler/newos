#include "crypto/md5.h"
#include "platform.h"
#include "runtime.h"
#include "stdint.h"

#define ELF_EHDR_SIZE 64U
#define ELF_PHDR_SIZE 56U
#define ELFCLASS64 2U
#define ELFDATA2LSB 1U
#define EV_CURRENT 1U
#define ET_EXEC 2U
#define ET_DYN 3U
#define EM_X86_64 62U
#define PT_LOAD 1U
#define PF_X 1U
#define PF_R 4U
#define COLLISION_ALIGNMENT 64U
#define COLLISION_MAX_DIGEST_SIZE 64U
#define COLLISION_MAX_PAYLOAD_SIZE 4096U
#define ZERO_CHUNK_SIZE 4096U
#define MD5_COLLISION_BLOCK_SIZE 128U
#define BACKEND_MAX_PAYLOAD_SIZE (16U * 1024U * 1024U)
#define BACKEND_MAX_ARGS 32U
#define PATH_BUFFER_SIZE 512U
#define EXIT_NOT_A_COLLISION 2
#define CONTROLLED_ELF_PREFIX_SIZE 128U
#define CONTROLLED_ELF_PAYLOAD_SIZE 128U
#define CONTROLLED_ELF_CODE_OFFSET 8192U
#define CONTROLLED_ELF_FILE_SIZE 12288U
#define CONTROLLED_ELF_BASE 0x400000ULL
#define CONTROLLED_ELF_CODE_MAX 32U
#define WRAPPER_CODE_MAX 512U
#define WRAPPER_EMBED_OFFSET CONTROLLED_ELF_FILE_SIZE
#define WRAPPER_LOADER_DISP32_OFFSET 6U
#define WRAPPER_LOADER_SHIFT_OFFSET 12U
#define WRAPPER_LOADER_INVERT_OFFSET 16U
#define WRAPPER_LOADER_A_OFFSET_OFFSET 25U
#define WRAPPER_LOADER_A_SIZE_OFFSET 35U
#define WRAPPER_LOADER_B_OFFSET_OFFSET 47U
#define WRAPPER_LOADER_B_SIZE_OFFSET 57U

static const char md5_collision_block_a_hex[] =
    "d131dd02c5e6eec4693d9a0698aff95c2fcab58712467eab4004583eb8fb7f89"
    "55ad340609f4b30283e488832571415a085125e8f7cdc99fd91dbdf280373c5b"
    "d8823e3156348f5bae6dacd436c919c6dd53e2b487da03fd02396306d248cda0"
    "e99f33420f577ee8ce54b67080a80d1ec69821bcb6a8839396f9652b6ff72a70";

static const char md5_collision_block_b_hex[] =
    "d131dd02c5e6eec4693d9a0698aff95c2fcab50712467eab4004583eb8fb7f89"
    "55ad340609f4b30283e4888325f1415a085125e8f7cdc99fd91dbd7280373c5b"
    "d8823e3156348f5bae6dacd436c919c6dd53e23487da03fd02396306d248cda0"
    "e99f33420f577ee8ce54b67080280d1ec69821bcb6a8839396f965ab6ff72a70";

static const unsigned char plain_common_suffix[] =
    "\n"
    "newos experimental/md5files\n"
    "These files intentionally differ in the first 128 bytes.\n"
    "The remainder is identical, openly visible metadata text.\n"
    "Do not use MD5 as an identity or integrity check.\n";

static const unsigned char elf_metadata_prelude[] =
    "\nnewos-md5-collision-metadata\n"
    "format: elf-trailer-scaffold-v1\n"
    "status: candidate trailer; collision block follows after zero padding\n";

static const unsigned char elf_metadata_suffix[] =
    "\nnewos-md5-collision-metadata-end\n"
    "note: appended bytes are outside the original ELF load segments\n"
    "note: fixed public blocks collide only for their controlled MD5 prefix state\n"
    "note: arbitrary ELF pairs need a chosen-prefix MD5 backend\n";

static unsigned char zero_chunk[ZERO_CHUNK_SIZE];

static const unsigned char wrapper_loader_template[] = {
    0x48, 0x89, 0xe3, 0x0f, 0xb6, 0x3d, 0x11, 0x11, 0x11, 0x11, 0xc1, 0xef, 0x07, 0x83, 0xe7, 0x01,
    0x83, 0xf7, 0x01, 0x85, 0xff, 0x75, 0x16, 0x49, 0xbc, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
    0x22, 0x49, 0xbd, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0xeb, 0x14, 0x49, 0xbc, 0x44,
    0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x49, 0xbd, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x48, 0x8d, 0x3d, 0xc8, 0x00, 0x00, 0x00, 0x31, 0xf6, 0x31, 0xd2, 0xb8, 0x02, 0x00, 0x00,
    0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x0f, 0x88, 0xa8, 0x00, 0x00, 0x00, 0x49, 0x89, 0xc6, 0x48,
    0x8d, 0x3d, 0xb9, 0x00, 0x00, 0x00, 0x31, 0xf6, 0xb8, 0x3f, 0x01, 0x00, 0x00, 0x0f, 0x05, 0x48,
    0x85, 0xc0, 0x0f, 0x88, 0x8c, 0x00, 0x00, 0x00, 0x49, 0x89, 0xc7, 0x4c, 0x89, 0xf7, 0x4c, 0x89,
    0xe6, 0x31, 0xd2, 0xb8, 0x08, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x78, 0x75, 0x48,
    0x81, 0xec, 0x00, 0x10, 0x00, 0x00, 0x4d, 0x85, 0xed, 0x74, 0x46, 0xba, 0x00, 0x10, 0x00, 0x00,
    0x49, 0x39, 0xd5, 0x49, 0x0f, 0x42, 0xd5, 0x4c, 0x89, 0xf7, 0x48, 0x89, 0xe6, 0x31, 0xc0, 0x0f,
    0x05, 0x48, 0x85, 0xc0, 0x7e, 0x4e, 0x49, 0x89, 0xc0, 0x49, 0x89, 0xc1, 0x49, 0x89, 0xe2, 0x4c,
    0x89, 0xff, 0x4c, 0x89, 0xd6, 0x4c, 0x89, 0xca, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48,
    0x85, 0xc0, 0x7e, 0x30, 0x49, 0x01, 0xc2, 0x49, 0x29, 0xc1, 0x75, 0xe3, 0x4d, 0x29, 0xc5, 0xeb,
    0xb5, 0x4c, 0x89, 0xff, 0x48, 0x8d, 0x35, 0x42, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x53, 0x08, 0x48,
    0x8b, 0x0b, 0x4c, 0x8d, 0x54, 0xcb, 0x10, 0x41, 0xb8, 0x00, 0x10, 0x00, 0x00, 0xb8, 0x42, 0x01,
    0x00, 0x00, 0x0f, 0x05, 0xbf, 0x6f, 0x00, 0x00, 0x00, 0xb8, 0x3c, 0x00, 0x00, 0x00, 0x0f, 0x05,
    0x2f, 0x70, 0x72, 0x6f, 0x63, 0x2f, 0x73, 0x65, 0x6c, 0x66, 0x2f, 0x65, 0x78, 0x65, 0x00, 0x6e,
    0x65, 0x77, 0x6f, 0x73, 0x2d, 0x6d, 0x64, 0x35, 0x2d, 0x65, 0x6c, 0x66, 0x00, 0x00
};

static const unsigned char controlled_elf_prefix[CONTROLLED_ELF_PREFIX_SIZE] = {
    0x7f, 0x45, 0x4c, 0x46, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x3e, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x20, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x38, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const unsigned char controlled_payload_a[CONTROLLED_ELF_PAYLOAD_SIZE] = {
    0x51, 0xdd, 0x83, 0xa1, 0xd7, 0x28, 0x11, 0x16, 0xce, 0x8b, 0xa9, 0xc6, 0x41, 0xe7, 0x29, 0xa8,
    0x55, 0x24, 0xa4, 0xf8, 0xf7, 0x50, 0xf2, 0xd4, 0xf5, 0x89, 0x7f, 0xc2, 0x08, 0xb2, 0x3a, 0x9f,
    0x47, 0x13, 0xcf, 0xcb, 0xd0, 0xbe, 0x86, 0xcf, 0xef, 0x3b, 0x4c, 0x64, 0xe2, 0x53, 0xa2, 0x72,
    0x9d, 0xc0, 0x8c, 0x18, 0x97, 0x00, 0xaf, 0xfa, 0xb2, 0x89, 0xf9, 0xad, 0xc0, 0x33, 0x66, 0x29,
    0x59, 0x53, 0xad, 0x6e, 0xc9, 0x7d, 0xf1, 0xf6, 0xcc, 0x50, 0xd0, 0xaa, 0x68, 0x23, 0x6b, 0x8e,
    0x32, 0xf1, 0x3a, 0xb5, 0x74, 0x89, 0x52, 0xfd, 0x0e, 0xc9, 0xa9, 0xf6, 0xcb, 0x0b, 0x61, 0xbd,
    0x6d, 0xc8, 0x72, 0x6a, 0xfd, 0xb9, 0xdd, 0xa7, 0xa2, 0x5d, 0xf5, 0x69, 0xa1, 0x64, 0x15, 0x9e,
    0xbb, 0x04, 0x22, 0x81, 0xc3, 0xd0, 0xe3, 0xf5, 0xb9, 0x2c, 0x9a, 0xf3, 0x40, 0x0d, 0xdd, 0xc8
};

static const unsigned char controlled_payload_b[CONTROLLED_ELF_PAYLOAD_SIZE] = {
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
    unsigned char *data;
    size_t size;
    unsigned int mode;
} FileImage;

typedef struct {
    uint16_t type;
    uint16_t phnum;
    uint64_t phoff;
    uint64_t loaded_end;
} ElfInfo;

typedef struct {
    const char *in1;
    const char *in2;
    const char *out_dir;
    const char *out1;
    const char *out2;
    const char *backend;
} ElfOptions;

typedef struct {
    const char *out_dir;
    const char *out_true;
    const char *out_false;
} ControlledElfOptions;

typedef struct {
    int quiet;
    const char *prefix_path;
    const char *out1;
    const char *out2;
} ControlledFastcollOptions;

typedef struct {
    CryptoMd5Context md5;
} CollisionDigestContext;

typedef struct {
    const char *name;
    const char *uppercase_name;
    size_t digest_size;
    size_t block_alignment;
    size_t collision_block_size;
    const char *collision_block_a_hex;
    const char *collision_block_b_hex;
    const unsigned char *controlled_prefix;
    size_t controlled_prefix_size;
    const unsigned char *controlled_payload_a;
    const unsigned char *controlled_payload_b;
    size_t controlled_payload_size;
    void (*digest_init)(CollisionDigestContext *context);
    void (*digest_update)(CollisionDigestContext *context, const unsigned char *data, size_t size);
    void (*digest_final)(CollisionDigestContext *context, unsigned char *digest);
} CollisionProfile;

static void md5_digest_init(CollisionDigestContext *context) {
    crypto_md5_init(&context->md5);
}

static void md5_digest_update(CollisionDigestContext *context, const unsigned char *data, size_t size) {
    crypto_md5_update(&context->md5, data, size);
}

static void md5_digest_final(CollisionDigestContext *context, unsigned char *digest) {
    crypto_md5_final(&context->md5, digest);
}

static const CollisionProfile md5_profile = {
    "md5",
    "MD5",
    CRYPTO_MD5_DIGEST_SIZE,
    COLLISION_ALIGNMENT,
    MD5_COLLISION_BLOCK_SIZE,
    md5_collision_block_a_hex,
    md5_collision_block_b_hex,
    controlled_elf_prefix,
    CONTROLLED_ELF_PREFIX_SIZE,
    controlled_payload_a,
    controlled_payload_b,
    CONTROLLED_ELF_PAYLOAD_SIZE,
    md5_digest_init,
    md5_digest_update,
    md5_digest_final
};

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void write_text(int fd, const char *text) {
    (void)rt_write_cstr(fd, text);
}

static void write_size_value(int fd, size_t value) {
    (void)rt_write_uint(fd, (unsigned long long)value);
}

static void write_error_path(const char *message, const char *path) {
    write_text(2, "generate: ");
    write_text(2, message);
    if (path != 0) {
        write_text(2, path);
    }
    write_text(2, "\n");
}

static void write_error_size(const char *message, const char *path, size_t value) {
    write_text(2, "generate: ");
    write_text(2, message);
    if (path != 0) {
        write_text(2, path);
    }
    write_text(2, " ");
    write_size_value(2, value);
    write_text(2, "\n");
}

static void print_path_line(const char *label, const char *path) {
    write_text(1, label);
    write_text(1, path);
    write_text(1, "\n");
}

static int decode_hex_bytes(const char *hex, unsigned char *out, size_t out_size) {
    size_t offset;

    if (rt_strlen(hex) != out_size * 2U) {
        return 1;
    }
    for (offset = 0; offset < out_size; ++offset) {
        int high = hex_value(hex[offset * 2U]);
        int low = hex_value(hex[offset * 2U + 1U]);

        if (high < 0 || low < 0) {
            return 1;
        }
        out[offset] = (unsigned char)((high << 4) | low);
    }
    return 0;
}

static uint16_t read_u16_le(const unsigned char *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const unsigned char *data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint64_t read_u64_le(const unsigned char *data) {
    uint64_t value = 0;
    unsigned int shift;

    for (shift = 0; shift < 8U; ++shift) {
        value |= (uint64_t)data[shift] << (shift * 8U);
    }
    return value;
}

static int add_overflows_size(size_t left, size_t right) {
    return left > (size_t)-1 - right;
}

static size_t align_up_size(size_t value, size_t alignment) {
    size_t remainder = value % alignment;

    return remainder == 0U ? value : value + (alignment - remainder);
}

static int range_valid_u64(uint64_t offset, uint64_t size, uint64_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

static void digest_to_hex(const CollisionProfile *profile, const unsigned char *digest, char *hex) {
    static const char digits[] = "0123456789abcdef";
    size_t offset;

    for (offset = 0; offset < profile->digest_size; ++offset) {
        hex[offset * 2U] = digits[(digest[offset] >> 4) & 0x0fU];
        hex[offset * 2U + 1U] = digits[digest[offset] & 0x0fU];
    }
    hex[profile->digest_size * 2U] = '\0';
}

static int join_path(char *buffer, size_t buffer_size, const char *directory, const char *name) {
    return rt_join_path(directory, name, buffer, buffer_size) == 0 ? 0 : 1;
}

static int ensure_directory(const char *path) {
    if (platform_make_directory(path, 0777U) == 0 || platform_path_access(path, PLATFORM_ACCESS_EXISTS) == 0) {
        return 0;
    }
    write_error_path("cannot create ", path);
    return 1;
}

static int set_backend_env_size(const char *name, size_t value) {
    char buffer[64];

    rt_unsigned_to_string((unsigned long long)value, buffer, sizeof(buffer));
    if (platform_setenv(name, buffer, 1) != 0) {
        write_error_path("cannot set ", name);
        return 1;
    }
    return 0;
}

static int set_backend_env_path(const char *name, const char *path) {
    if (platform_setenv(name, path, 1) != 0) {
        write_error_path("cannot set ", name);
        return 1;
    }
    return 0;
}

static void free_file_image(FileImage *image) {
    if (image != 0) {
        rt_free(image->data);
        image->data = 0;
        image->size = 0U;
    }
}

static int read_file_image(const char *path, FileImage *image) {
    PlatformDirEntry entry;
    int fd;
    size_t offset = 0U;

    memset(image, 0, sizeof(*image));
    fd = platform_open_read_secure(path, &entry);
    if (fd < 0) {
        write_error_path("cannot open ", path);
        return 1;
    }
    if (entry.is_dir) {
        write_error_path("not a regular file: ", path);
        (void)platform_close(fd);
        return 1;
    }
    if (entry.size > (unsigned long long)(size_t)-1) {
        write_error_path("too large for this scaffold: ", path);
        (void)platform_close(fd);
        return 1;
    }
    image->size = (size_t)entry.size;
    image->mode = entry.mode & 07777U;
    image->data = (unsigned char *)rt_malloc(image->size == 0U ? 1U : image->size);
    if (image->data == 0) {
        write_error_path("out of memory reading ", path);
        (void)platform_close(fd);
        return 1;
    }
    while (offset < image->size) {
        long n = platform_read(fd, image->data + offset, image->size - offset);

        if (n <= 0) {
            write_error_path("cannot read ", path);
            (void)platform_close(fd);
            free_file_image(image);
            return 1;
        }
        offset += (size_t)n;
    }
    if (platform_close(fd) != 0) {
        write_error_path("cannot close ", path);
        free_file_image(image);
        return 1;
    }
    return 0;
}

static int validate_elf64(const char *path, const FileImage *image, ElfInfo *info) {
    uint16_t phentsize;
    uint64_t file_size = (uint64_t)image->size;
    uint64_t ph_table_size;
    size_t index;

    memset(info, 0, sizeof(*info));
    if (image->size < ELF_EHDR_SIZE ||
        image->data[0] != 0x7fU || image->data[1] != 'E' || image->data[2] != 'L' || image->data[3] != 'F') {
        write_error_path("not an ELF file: ", path);
        return 1;
    }
    if (image->data[4] != ELFCLASS64 || image->data[5] != ELFDATA2LSB || image->data[6] != EV_CURRENT) {
        write_error_path("not a little-endian ELF64 file: ", path);
        return 1;
    }
    info->type = read_u16_le(image->data + 16U);
    if (info->type != ET_EXEC && info->type != ET_DYN) {
        write_error_path("not an executable/shared ELF image: ", path);
        return 1;
    }
    info->phoff = read_u64_le(image->data + 32U);
    phentsize = read_u16_le(image->data + 54U);
    info->phnum = read_u16_le(image->data + 56U);
    if (info->phnum == 0U) {
        write_error_path("has no program headers: ", path);
        return 1;
    }
    if (phentsize != ELF_PHDR_SIZE) {
        write_error_size("has unsupported ELF program header size for ", path, phentsize);
        return 1;
    }
    ph_table_size = (uint64_t)info->phnum * phentsize;
    if (!range_valid_u64(info->phoff, ph_table_size, file_size)) {
        write_error_path("has program headers outside the file: ", path);
        return 1;
    }

    for (index = 0; index < info->phnum; ++index) {
        const unsigned char *header = image->data + info->phoff + (uint64_t)index * phentsize;
        uint32_t type = read_u32_le(header);

        if (type == PT_LOAD) {
            uint64_t offset = read_u64_le(header + 8U);
            uint64_t file_bytes = read_u64_le(header + 32U);
            uint64_t memory_bytes = read_u64_le(header + 40U);
            uint64_t end = offset + file_bytes;

            if (memory_bytes < file_bytes) {
                write_error_path("has a LOAD segment with p_memsz < p_filesz: ", path);
                return 1;
            }
            if (!range_valid_u64(offset, file_bytes, file_size) || end < offset) {
                write_error_path("has a LOAD segment outside the file: ", path);
                return 1;
            }
            if (end > info->loaded_end) {
                info->loaded_end = end;
            }
        }
    }
    if (info->loaded_end == 0U) {
        write_error_path("has no loadable file bytes: ", path);
        return 1;
    }
    return 0;
}

static void collision_candidate_digest(const CollisionProfile *profile,
                                       const FileImage *image,
                                       size_t collision_offset,
                                       const unsigned char *collision_payload,
                                       size_t collision_payload_size,
                                       unsigned char *digest) {
    CollisionDigestContext context;
    size_t prefix_size = image->size + sizeof(elf_metadata_prelude) - 1U;
    size_t padding = collision_offset - prefix_size;

    profile->digest_init(&context);
    profile->digest_update(&context, image->data, image->size);
    profile->digest_update(&context, elf_metadata_prelude, sizeof(elf_metadata_prelude) - 1U);
    while (padding != 0U) {
        size_t chunk = padding < sizeof(zero_chunk) ? padding : sizeof(zero_chunk);

        profile->digest_update(&context, zero_chunk, chunk);
        padding -= chunk;
    }
    profile->digest_update(&context, collision_payload, collision_payload_size);
    profile->digest_update(&context, elf_metadata_suffix, sizeof(elf_metadata_suffix) - 1U);
    profile->digest_final(&context, digest);
}

static int read_backend_payload_file(const char *path, unsigned char **payload_out, size_t *payload_size_out) {
    FileImage image;

    if (read_file_image(path, &image) != 0) {
        write_error_path("backend did not produce a readable payload at ", path);
        return 1;
    }
    if (image.size == 0U || image.size > BACKEND_MAX_PAYLOAD_SIZE) {
        write_error_size("backend payload has unsupported size at ", path, image.size);
        free_file_image(&image);
        return 1;
    }
    *payload_out = image.data;
    *payload_size_out = image.size;
    image.data = 0;
    free_file_image(&image);
    return 0;
}

static int write_zero_padding(int fd, size_t padding, const char *path) {
    while (padding != 0U) {
        size_t chunk = padding < sizeof(zero_chunk) ? padding : sizeof(zero_chunk);

        if (rt_write_all(fd, zero_chunk, chunk) != 0) {
            write_error_path("cannot write ", path);
            return 1;
        }
        padding -= chunk;
    }
    return 0;
}

static int seek_zero_padding(int fd, size_t padding, const char *path) {
    if (padding == 0U) {
        return 0;
    }
    if (padding <= (size_t)(((unsigned long long)-1) >> 1) &&
        platform_seek(fd, (long long)padding, PLATFORM_SEEK_CUR) >= 0) {
        return 0;
    }
    return write_zero_padding(fd, padding, path);
}

static int finish_zero_padding(int fd, size_t padding, const char *path) {
    if (padding == 0U) {
        return 0;
    }
    if (padding <= (size_t)(((unsigned long long)-1) >> 1) &&
        platform_seek(fd, (long long)(padding - 1U), PLATFORM_SEEK_CUR) >= 0) {
        if (rt_write_all(fd, zero_chunk, 1U) != 0) {
            write_error_path("cannot write ", path);
            return 1;
        }
        return 0;
    }
    return write_zero_padding(fd, padding, path);
}

static int write_prefix_fd(int fd, const char *path, const FileImage *image, size_t collision_offset) {
    size_t prefix_size = image->size + sizeof(elf_metadata_prelude) - 1U;
    size_t padding = collision_offset - prefix_size;

    if ((image->size != 0U && rt_write_all(fd, image->data, image->size) != 0) ||
        rt_write_all(fd, elf_metadata_prelude, sizeof(elf_metadata_prelude) - 1U) != 0) {
        write_error_path("cannot write ", path);
        return 1;
    }

    return write_zero_padding(fd, padding, path);
}

static int create_prefix_file(char *path, size_t path_size, const char *prefix, const FileImage *image, size_t collision_offset) {
    int fd = platform_create_temp_file(path, path_size, prefix, 0600U);

    if (fd < 0) {
        write_error_path("cannot create temporary prefix file ", prefix);
        return 1;
    }
    if (write_prefix_fd(fd, path, image, collision_offset) != 0) {
        (void)platform_close(fd);
        return 1;
    }
    if (platform_close(fd) != 0) {
        write_error_path("cannot close ", path);
        return 1;
    }
    return 0;
}

static int create_backend_output_path(char *path, size_t path_size, const char *prefix) {
    int fd = platform_create_temp_file(path, path_size, prefix, 0600U);

    if (fd < 0) {
        write_error_path("cannot create temporary backend output ", prefix);
        return 1;
    }
    if (platform_close(fd) != 0) {
        write_error_path("cannot close ", path);
        return 1;
    }
    return 0;
}

static int split_backend_command(const char *command, char *storage, size_t storage_size, char **argv, size_t argv_capacity) {
    size_t in = 0U;
    size_t out = 0U;
    size_t argc = 0U;

    if (command == 0 || command[0] == '\0') {
        write_error_path("empty backend command", 0);
        return 1;
    }
    while (command[in] != '\0') {
        char quote = '\0';

        while (command[in] == ' ' || command[in] == '\t' || command[in] == '\n' || command[in] == '\r') {
            in += 1U;
        }
        if (command[in] == '\0') {
            break;
        }
        if (argc + 1U >= argv_capacity) {
            write_error_path("backend command has too many arguments", 0);
            return 1;
        }
        argv[argc++] = storage + out;
        while (command[in] != '\0') {
            char ch = command[in++];

            if (quote != '\0') {
                if (ch == quote) {
                    quote = '\0';
                    continue;
                }
            } else if (ch == '\'' || ch == '"') {
                quote = ch;
                continue;
            } else if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                break;
            }
            if (out + 1U >= storage_size) {
                write_error_path("backend command is too long", 0);
                return 1;
            }
            storage[out++] = ch;
        }
        if (quote != '\0') {
            write_error_path("backend command has an unterminated quote", 0);
            return 1;
        }
        if (out >= storage_size) {
            write_error_path("backend command is too long", 0);
            return 1;
        }
        storage[out++] = '\0';
    }
    if (argc == 0U) {
        write_error_path("empty backend command", 0);
        return 1;
    }
    argv[argc] = 0;
    return 0;
}

static int run_backend(const CollisionProfile *profile,
                       const char *command,
                       const FileImage *image_a,
                       const FileImage *image_b,
                       size_t collision_offset,
                       unsigned char **payload_a_out,
                       unsigned char **payload_b_out,
                       size_t *payload_size_out) {
    char prefix_a_path[PATH_BUFFER_SIZE] = "";
    char prefix_b_path[PATH_BUFFER_SIZE] = "";
    char block_a_path[PATH_BUFFER_SIZE] = "";
    char block_b_path[PATH_BUFFER_SIZE] = "";
    char backend_storage[PATH_BUFFER_SIZE];
    char *backend_argv[BACKEND_MAX_ARGS];
    int backend_pid;
    int backend_status;
    int status = 1;

    if (create_prefix_file(prefix_a_path, sizeof(prefix_a_path), "/tmp/newos-md5-prefix-a-", image_a, collision_offset) != 0 ||
        create_prefix_file(prefix_b_path, sizeof(prefix_b_path), "/tmp/newos-md5-prefix-b-", image_b, collision_offset) != 0 ||
        create_backend_output_path(block_a_path, sizeof(block_a_path), "/tmp/newos-md5-block-a-") != 0 ||
        create_backend_output_path(block_b_path, sizeof(block_b_path), "/tmp/newos-md5-block-b-") != 0) {
        goto cleanup;
    }
    if (set_backend_env_path("NEWOS_MD5_PREFIX_A", prefix_a_path) != 0 ||
        set_backend_env_path("NEWOS_MD5_PREFIX_B", prefix_b_path) != 0 ||
        set_backend_env_path("NEWOS_MD5_BLOCK_A", block_a_path) != 0 ||
        set_backend_env_path("NEWOS_MD5_BLOCK_B", block_b_path) != 0 ||
        set_backend_env_size("NEWOS_MD5_COLLISION_OFFSET", collision_offset) != 0 ||
        set_backend_env_size("NEWOS_MD5_BLOCK_SIZE", profile->collision_block_size) != 0 ||
        set_backend_env_size("NEWOS_MD5_MAX_PAYLOAD_SIZE", BACKEND_MAX_PAYLOAD_SIZE) != 0) {
        goto cleanup;
    }

    print_path_line("backend: ", command);
    print_path_line("backend prefix A: ", prefix_a_path);
    print_path_line("backend prefix B: ", prefix_b_path);
    if (split_backend_command(command, backend_storage, sizeof(backend_storage), backend_argv, BACKEND_MAX_ARGS) != 0) {
        goto cleanup;
    }
    if (platform_spawn_process(backend_argv, -1, -1, 0, 0, 0, &backend_pid) != 0) {
        write_error_path("cannot run backend ", backend_argv[0]);
        goto cleanup;
    }
    if (platform_wait_process(backend_pid, &backend_status) != 0) {
        write_error_path("cannot wait for backend ", backend_argv[0]);
        goto cleanup;
    }
    if (backend_status != 0) {
        write_text(2, "generate: backend failed with exit status ");
        (void)rt_write_int(2, backend_status);
        write_text(2, "\n");
        goto cleanup;
    }
    if (read_backend_payload_file(block_a_path, payload_a_out, payload_size_out) != 0 ||
        read_backend_payload_file(block_b_path, payload_b_out, payload_size_out + 1) != 0) {
        goto cleanup;
    }
    if (*payload_size_out != *(payload_size_out + 1)) {
        write_text(2, "generate: backend payload sizes differ: ");
        write_size_value(2, *payload_size_out);
        write_text(2, " vs ");
        write_size_value(2, *(payload_size_out + 1));
        write_text(2, "\n");
        goto cleanup;
    }
    status = 0;

cleanup:
    if (prefix_a_path[0] != '\0') (void)platform_remove_file(prefix_a_path);
    if (prefix_b_path[0] != '\0') (void)platform_remove_file(prefix_b_path);
    if (block_a_path[0] != '\0') (void)platform_remove_file(block_a_path);
    if (block_b_path[0] != '\0') (void)platform_remove_file(block_b_path);
    return status;
}

static int write_candidate(const char *path,
                           const FileImage *image,
                           size_t collision_offset,
                           const unsigned char *collision_payload,
                           size_t collision_payload_size) {
    int fd = platform_open_write(path, 0644U);

    if (fd < 0) {
        write_error_path("cannot open ", path);
        return 1;
    }
    if (write_prefix_fd(fd, path, image, collision_offset) != 0) {
        (void)platform_close(fd);
        return 1;
    }
    if (rt_write_all(fd, collision_payload, collision_payload_size) != 0 ||
        rt_write_all(fd, elf_metadata_suffix, sizeof(elf_metadata_suffix) - 1U) != 0) {
        write_error_path("cannot write ", path);
        (void)platform_close(fd);
        return 1;
    }
    if (platform_close(fd) != 0) {
        write_error_path("cannot close ", path);
        return 1;
    }
    if (platform_change_mode(path, image->mode) != 0) {
        write_error_path("cannot chmod ", path);
        return 1;
    }
    return 0;
}

static int write_plain_file(const CollisionProfile *profile,
                            const char *path,
                            const unsigned char *collision_block) {
    int fd = platform_open_write(path, 0644U);

    if (fd < 0) {
        write_error_path("cannot open ", path);
        return 1;
    }
    if (rt_write_all(fd, collision_block, profile->collision_block_size) != 0 ||
        rt_write_all(fd, plain_common_suffix, sizeof(plain_common_suffix) - 1U) != 0) {
        write_error_path("cannot write ", path);
        (void)platform_close(fd);
        return 1;
    }
    if (platform_close(fd) != 0) {
        write_error_path("cannot close ", path);
        return 1;
    }
    return 0;
}

static void collision_prefix_payload_digest(const CollisionProfile *profile,
                                            const unsigned char *prefix,
                                            const unsigned char *payload,
                                            unsigned char *digest) {
    CollisionDigestContext context;

    profile->digest_init(&context);
    profile->digest_update(&context, prefix, profile->controlled_prefix_size);
    profile->digest_update(&context, payload, profile->controlled_payload_size);
    profile->digest_final(&context, digest);
}

static int verify_controlled_payloads(const CollisionProfile *profile, unsigned char *digest) {
    unsigned char digest_b[COLLISION_MAX_DIGEST_SIZE];

    collision_prefix_payload_digest(profile, profile->controlled_prefix, profile->controlled_payload_a, digest);
    collision_prefix_payload_digest(profile, profile->controlled_prefix, profile->controlled_payload_b, digest_b);
    if (memcmp(digest, digest_b, profile->digest_size) != 0 ||
        memcmp(profile->controlled_payload_a, profile->controlled_payload_b, profile->controlled_payload_size) == 0) {
        write_error_path("internal controlled collision payload is invalid", 0);
        return 1;
    }
    return 0;
}

static int read_controlled_prefix(const char *path, unsigned char prefix[CONTROLLED_ELF_PREFIX_SIZE]) {
    FileImage image;
    int status = 1;

    if (read_file_image(path, &image) != 0) {
        return 1;
    }
    if (image.size != CONTROLLED_ELF_PREFIX_SIZE) {
        write_error_path("prefix must be the controlled 128-byte ELF prefix: ", path);
        goto cleanup;
    }
    memcpy(prefix, image.data, CONTROLLED_ELF_PREFIX_SIZE);
    status = 0;

cleanup:
    free_file_image(&image);
    return status;
}

static int write_controlled_prefix_payload(const CollisionProfile *profile, const char *path, const unsigned char *payload) {
    int fd = platform_open_write(path, 0644U);

    if (fd < 0) {
        write_error_path("cannot open ", path);
        return 1;
    }
    if (rt_write_all(fd, profile->controlled_prefix, profile->controlled_prefix_size) != 0 ||
        rt_write_all(fd, payload, profile->controlled_payload_size) != 0) {
        write_error_path("cannot write ", path);
        (void)platform_close(fd);
        return 1;
    }
    if (platform_close(fd) != 0) {
        write_error_path("cannot close ", path);
        return 1;
    }
    return 0;
}

static int run_controlled_fastcoll(const CollisionProfile *profile, const ControlledFastcollOptions *options) {
    unsigned char prefix[CONTROLLED_ELF_PREFIX_SIZE];
    unsigned char digest[COLLISION_MAX_DIGEST_SIZE];
    char hex[COLLISION_MAX_DIGEST_SIZE * 2U + 1U];

    if (read_controlled_prefix(options->prefix_path, prefix) != 0) {
        return 1;
    }
    if (profile->controlled_prefix_size != CONTROLLED_ELF_PREFIX_SIZE ||
        memcmp(prefix, profile->controlled_prefix, CONTROLLED_ELF_PREFIX_SIZE) != 0) {
        write_error_path("unsupported prefix; this tool only matches the controlled ELF demo prefix", 0);
        return 1;
    }
    if (verify_controlled_payloads(profile, digest) != 0) {
        return 1;
    }
    if (write_controlled_prefix_payload(profile, options->out1, profile->controlled_payload_a) != 0 ||
        write_controlled_prefix_payload(profile, options->out2, profile->controlled_payload_b) != 0) {
        return 1;
    }
    if (!options->quiet) {
        digest_to_hex(profile, digest, hex);
        write_text(1, "same "); write_text(1, profile->name); write_text(1, ": "); write_text(1, hex); write_text(1, "\n");
    }
    return 0;
}

static int choose_controlled_behavior_bit(const CollisionProfile *profile, size_t *offset_out, unsigned int *bit_out, unsigned int *invert_out) {
    size_t payload_offset;

    for (payload_offset = 0U; payload_offset < profile->controlled_payload_size; ++payload_offset) {
        unsigned char left = profile->controlled_payload_a[payload_offset];
        unsigned char right = profile->controlled_payload_b[payload_offset];
        unsigned char diff = (unsigned char)(left ^ right);
        unsigned int bit;

        if (diff == 0U) {
            continue;
        }
        for (bit = 0U; bit < 8U; ++bit) {
            if ((diff & (unsigned char)(1U << bit)) != 0U) {
                unsigned int left_bit = (left >> bit) & 1U;
                unsigned int right_bit = (right >> bit) & 1U;
                unsigned int invert = left_bit;

                if (((left_bit ^ invert) == 0U) && ((right_bit ^ invert) == 1U)) {
                    *offset_out = profile->controlled_prefix_size + payload_offset;
                    *bit_out = bit;
                    *invert_out = invert;
                    return 0;
                }
            }
        }
    }
    write_error_path("no usable differing controlled collision bit found", 0);
    return 1;
}

static void write_u32_le(unsigned char *data, uint32_t value) {
    data[0] = (unsigned char)(value & 0xffU);
    data[1] = (unsigned char)((value >> 8U) & 0xffU);
    data[2] = (unsigned char)((value >> 16U) & 0xffU);
    data[3] = (unsigned char)((value >> 24U) & 0xffU);
}

static void write_u64_le(unsigned char *data, unsigned long long value) {
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        data[index] = (unsigned char)((value >> (index * 8U)) & 0xffU);
    }
}

static void collision_digest_zero_padding(const CollisionProfile *profile, CollisionDigestContext *context, size_t padding) {
    while (padding != 0U) {
        size_t chunk = padding < sizeof(zero_chunk) ? padding : sizeof(zero_chunk);

        profile->digest_update(context, zero_chunk, chunk);
        padding -= chunk;
    }
}

static int build_controlled_exit_code(size_t selected_offset,
                                      unsigned int bit,
                                      unsigned int invert,
                                      unsigned char code[CONTROLLED_ELF_CODE_MAX],
                                      size_t *code_size_out) {
    uint64_t entry = CONTROLLED_ELF_BASE + CONTROLLED_ELF_CODE_OFFSET;
    uint64_t target = CONTROLLED_ELF_BASE + selected_offset;
    uint64_t rip_after_mov = entry + 7U;
    int64_t displacement = (int64_t)target - (int64_t)rip_after_mov;
    size_t offset = 0U;

    if (displacement < -2147483648LL || displacement > 2147483647LL) {
        write_error_path("selected controlled byte is outside RIP-relative range", 0);
        return 1;
    }
    code[offset++] = 0x0fU;
    code[offset++] = 0xb6U;
    code[offset++] = 0x3dU;
    write_u32_le(code + offset, (uint32_t)displacement);
    offset += 4U;
    code[offset++] = 0xc1U;
    code[offset++] = 0xefU;
    code[offset++] = (unsigned char)bit;
    code[offset++] = 0x83U;
    code[offset++] = 0xe7U;
    code[offset++] = 0x01U;
    if (invert != 0U) {
        code[offset++] = 0x83U;
        code[offset++] = 0xf7U;
        code[offset++] = 0x01U;
    }
    code[offset++] = 0xb8U;
    code[offset++] = 0x3cU;
    code[offset++] = 0x00U;
    code[offset++] = 0x00U;
    code[offset++] = 0x00U;
    code[offset++] = 0x0fU;
    code[offset++] = 0x05U;
    *code_size_out = offset;
    return 0;
}

static int build_wrapper_loader_code(size_t selected_offset,
                                     unsigned int bit,
                                     unsigned int invert,
                                     size_t embed_a_offset,
                                     size_t embed_a_size,
                                     size_t embed_b_offset,
                                     size_t embed_b_size,
                                     unsigned char code[WRAPPER_CODE_MAX],
                                     size_t *code_size_out) {
    uint64_t target = CONTROLLED_ELF_BASE + selected_offset;
    uint64_t rip_after_mov = CONTROLLED_ELF_BASE + CONTROLLED_ELF_CODE_OFFSET + 10U;
    int64_t displacement = (int64_t)target - (int64_t)rip_after_mov;

    if (sizeof(wrapper_loader_template) > WRAPPER_CODE_MAX) {
        write_error_path("internal wrapper loader is too large", 0);
        return 1;
    }
    if (displacement < -2147483648LL || displacement > 2147483647LL) {
        write_error_path("selected controlled byte is outside wrapper RIP-relative range", 0);
        return 1;
    }
    if (bit > 7U) {
        write_error_path("selected controlled bit is invalid", 0);
        return 1;
    }
    if (embed_a_offset > (size_t)(((unsigned long long)-1) >> 1) ||
        embed_b_offset > (size_t)(((unsigned long long)-1) >> 1)) {
        write_error_path("embedded ELF offset is too large for Linux lseek", 0);
        return 1;
    }

    memcpy(code, wrapper_loader_template, sizeof(wrapper_loader_template));
    write_u32_le(code + WRAPPER_LOADER_DISP32_OFFSET, (uint32_t)displacement);
    code[WRAPPER_LOADER_SHIFT_OFFSET] = (unsigned char)bit;
    if (invert == 0U) {
        code[WRAPPER_LOADER_INVERT_OFFSET] = 0x90U;
        code[WRAPPER_LOADER_INVERT_OFFSET + 1U] = 0x90U;
        code[WRAPPER_LOADER_INVERT_OFFSET + 2U] = 0x90U;
    }
    write_u64_le(code + WRAPPER_LOADER_A_OFFSET_OFFSET, (unsigned long long)embed_a_offset);
    write_u64_le(code + WRAPPER_LOADER_A_SIZE_OFFSET, (unsigned long long)embed_a_size);
    write_u64_le(code + WRAPPER_LOADER_B_OFFSET_OFFSET, (unsigned long long)embed_b_offset);
    write_u64_le(code + WRAPPER_LOADER_B_SIZE_OFFSET, (unsigned long long)embed_b_size);
    *code_size_out = sizeof(wrapper_loader_template);
    return 0;
}

static void collision_controlled_elf_digest(const CollisionProfile *profile,
                                            const unsigned char *payload,
                                            const unsigned char *code,
                                            size_t code_size,
                                            unsigned char *digest) {
    CollisionDigestContext context;
    size_t image_size = profile->controlled_prefix_size + profile->controlled_payload_size;
    size_t padding_before_code = CONTROLLED_ELF_CODE_OFFSET - image_size;
    size_t padding_after_code = CONTROLLED_ELF_FILE_SIZE - CONTROLLED_ELF_CODE_OFFSET - code_size;

    profile->digest_init(&context);
    profile->digest_update(&context, profile->controlled_prefix, profile->controlled_prefix_size);
    profile->digest_update(&context, payload, profile->controlled_payload_size);
    collision_digest_zero_padding(profile, &context, padding_before_code);
    profile->digest_update(&context, code, code_size);
    collision_digest_zero_padding(profile, &context, padding_after_code);
    profile->digest_final(&context, digest);
}

static void collision_wrapped_elf_digest(const CollisionProfile *profile,
                                         const unsigned char *payload,
                                         const unsigned char *code,
                                         size_t code_size,
                                         const FileImage *image_a,
                                         const FileImage *image_b,
                                         unsigned char *digest) {
    CollisionDigestContext context;
    size_t image_size = profile->controlled_prefix_size + profile->controlled_payload_size;
    size_t padding_before_code = CONTROLLED_ELF_CODE_OFFSET - image_size;
    size_t padding_before_embed = WRAPPER_EMBED_OFFSET - CONTROLLED_ELF_CODE_OFFSET - code_size;

    profile->digest_init(&context);
    profile->digest_update(&context, profile->controlled_prefix, profile->controlled_prefix_size);
    profile->digest_update(&context, payload, profile->controlled_payload_size);
    collision_digest_zero_padding(profile, &context, padding_before_code);
    profile->digest_update(&context, code, code_size);
    collision_digest_zero_padding(profile, &context, padding_before_embed);
    profile->digest_update(&context, image_a->data, image_a->size);
    profile->digest_update(&context, image_b->data, image_b->size);
    profile->digest_final(&context, digest);
}

static int write_controlled_elf_image(const CollisionProfile *profile,
                                      const char *path,
                                      const unsigned char *payload,
                                      const unsigned char *code,
                                      size_t code_size) {
    size_t image_size = profile->controlled_prefix_size + profile->controlled_payload_size;
    int fd = platform_open_write(path, 0644U);

    if (fd < 0) {
        write_error_path("cannot open ", path);
        return 1;
    }
    if (rt_write_all(fd, profile->controlled_prefix, profile->controlled_prefix_size) != 0 ||
        rt_write_all(fd, payload, profile->controlled_payload_size) != 0 ||
        seek_zero_padding(fd, CONTROLLED_ELF_CODE_OFFSET - image_size, path) != 0 ||
        rt_write_all(fd, code, code_size) != 0 ||
        finish_zero_padding(fd, CONTROLLED_ELF_FILE_SIZE - CONTROLLED_ELF_CODE_OFFSET - code_size, path) != 0) {
        (void)platform_close(fd);
        return 1;
    }
    if (platform_close(fd) != 0) {
        write_error_path("cannot close ", path);
        return 1;
    }
    if (platform_change_mode(path, 0755U) != 0) {
        write_error_path("cannot chmod ", path);
        return 1;
    }
    return 0;
}

static int write_wrapped_elf_image(const CollisionProfile *profile,
                                   const char *path,
                                   const unsigned char *payload,
                                   const unsigned char *code,
                                   size_t code_size,
                                   const FileImage *image_a,
                                   const FileImage *image_b) {
    size_t image_size = profile->controlled_prefix_size + profile->controlled_payload_size;
    int fd = platform_open_write(path, 0644U);

    if (fd < 0) {
        write_error_path("cannot open ", path);
        return 1;
    }
    if (rt_write_all(fd, profile->controlled_prefix, profile->controlled_prefix_size) != 0 ||
        rt_write_all(fd, payload, profile->controlled_payload_size) != 0 ||
        seek_zero_padding(fd, CONTROLLED_ELF_CODE_OFFSET - image_size, path) != 0 ||
        rt_write_all(fd, code, code_size) != 0 ||
        seek_zero_padding(fd, WRAPPER_EMBED_OFFSET - CONTROLLED_ELF_CODE_OFFSET - code_size, path) != 0 ||
        rt_write_all(fd, image_a->data, image_a->size) != 0 ||
        rt_write_all(fd, image_b->data, image_b->size) != 0) {
        (void)platform_close(fd);
        return 1;
    }
    if (platform_close(fd) != 0) {
        write_error_path("cannot close ", path);
        return 1;
    }
    if (platform_change_mode(path, 0755U) != 0) {
        write_error_path("cannot chmod ", path);
        return 1;
    }
    return 0;
}

static int run_wrapped_elf_pair(const CollisionProfile *profile,
                                const char *out1,
                                const char *out2,
                                const FileImage *image_a,
                                const FileImage *image_b) {
    unsigned char digest_a[COLLISION_MAX_DIGEST_SIZE];
    unsigned char digest_b[COLLISION_MAX_DIGEST_SIZE];
    unsigned char code[WRAPPER_CODE_MAX];
    char hex[COLLISION_MAX_DIGEST_SIZE * 2U + 1U];
    size_t selected_offset;
    size_t code_size;
    size_t embed_a_offset = WRAPPER_EMBED_OFFSET;
    size_t embed_b_offset;
    size_t output_size;
    unsigned int bit;
    unsigned int invert;

    if (add_overflows_size(embed_a_offset, image_a->size) ||
        add_overflows_size(embed_a_offset + image_a->size, image_b->size)) {
        write_error_path("wrapped ELF output would be too large", 0);
        return 1;
    }
    embed_b_offset = embed_a_offset + image_a->size;
    output_size = embed_b_offset + image_b->size;
    if (verify_controlled_payloads(profile, digest_a) != 0 ||
        choose_controlled_behavior_bit(profile, &selected_offset, &bit, &invert) != 0 ||
        build_wrapper_loader_code(selected_offset, bit, invert,
                                  embed_a_offset, image_a->size,
                                  embed_b_offset, image_b->size,
                                  code, &code_size) != 0) {
        return 1;
    }
    if (code_size > WRAPPER_EMBED_OFFSET - CONTROLLED_ELF_CODE_OFFSET) {
        write_error_path("wrapper loader does not fit in controlled ELF image", 0);
        return 1;
    }
    collision_wrapped_elf_digest(profile, profile->controlled_payload_a, code, code_size, image_a, image_b, digest_a);
    collision_wrapped_elf_digest(profile, profile->controlled_payload_b, code, code_size, image_a, image_b, digest_b);
    if (memcmp(digest_a, digest_b, profile->digest_size) != 0) {
        write_error_path("wrapped ELF files do not collide", 0);
        return 1;
    }
    if (write_wrapped_elf_image(profile, out1, profile->controlled_payload_a, code, code_size, image_a, image_b) != 0 ||
        write_wrapped_elf_image(profile, out2, profile->controlled_payload_b, code, code_size, image_a, image_b) != 0) {
        return 1;
    }
    digest_to_hex(profile, digest_a, hex);
    print_path_line("wrote ", out1);
    print_path_line("wrote ", out2);
    write_text(1, "same "); write_text(1, profile->name); write_text(1, ": "); write_text(1, hex); write_text(1, "\n");
    write_text(1, "different files: yes\n");
    write_text(1, "wrapper output size: "); write_size_value(1, output_size); write_text(1, " bytes\n");
    write_text(1, "embedded A offset: "); write_size_value(1, embed_a_offset); write_text(1, "; embedded B offset: "); write_size_value(1, embed_b_offset); write_text(1, "\n");
    return 0;
}

static int run_controlled_elf_demo(const CollisionProfile *profile, const ControlledElfOptions *options) {
    unsigned char prefix_payload_digest[COLLISION_MAX_DIGEST_SIZE];
    unsigned char digest_a[COLLISION_MAX_DIGEST_SIZE];
    unsigned char digest_b[COLLISION_MAX_DIGEST_SIZE];
    unsigned char code[CONTROLLED_ELF_CODE_MAX];
    char default_true[PATH_BUFFER_SIZE];
    char default_false[PATH_BUFFER_SIZE];
    char hex[COLLISION_MAX_DIGEST_SIZE * 2U + 1U];
    const char *true_path = options->out_true;
    const char *false_path = options->out_false;
    size_t selected_offset;
    size_t code_size;
    unsigned int bit;
    unsigned int invert;
    int need_out_dir = true_path == 0 || false_path == 0;

    if (need_out_dir && ensure_directory(options->out_dir) != 0) {
        return 1;
    }
    if (true_path == 0) {
        if (join_path(default_true, sizeof(default_true), options->out_dir, "elf-true") != 0) {
            write_error_path("output path is too long", 0);
            return 1;
        }
        true_path = default_true;
    }
    if (false_path == 0) {
        if (join_path(default_false, sizeof(default_false), options->out_dir, "elf-false") != 0) {
            write_error_path("output path is too long", 0);
            return 1;
        }
        false_path = default_false;
    }
    if (verify_controlled_payloads(profile, prefix_payload_digest) != 0 ||
        choose_controlled_behavior_bit(profile, &selected_offset, &bit, &invert) != 0 ||
        build_controlled_exit_code(selected_offset, bit, invert, code, &code_size) != 0) {
        return 1;
    }
    collision_controlled_elf_digest(profile, profile->controlled_payload_a, code, code_size, digest_a);
    collision_controlled_elf_digest(profile, profile->controlled_payload_b, code, code_size, digest_b);
    if (memcmp(digest_a, digest_b, profile->digest_size) != 0) {
        write_error_path("controlled ELF files do not collide", 0);
        return 1;
    }
    if (write_controlled_elf_image(profile, true_path, profile->controlled_payload_a, code, code_size) != 0 ||
        write_controlled_elf_image(profile, false_path, profile->controlled_payload_b, code, code_size) != 0) {
        return 1;
    }
    digest_to_hex(profile, digest_a, hex);
    print_path_line("wrote ", true_path);
    print_path_line("wrote ", false_path);
    write_text(1, "same "); write_text(1, profile->name); write_text(1, ": "); write_text(1, hex); write_text(1, "\n");
    write_text(1, "different files: yes\n");
    write_text(1, "behavior bit: file offset "); write_size_value(1, selected_offset); write_text(1, ", bit "); (void)rt_write_uint(1, bit); write_text(1, ", invert "); (void)rt_write_uint(1, invert); write_text(1, "\n");
    write_text(1, "sizes: "); write_size_value(1, CONTROLLED_ELF_FILE_SIZE); write_text(1, " bytes, "); write_size_value(1, CONTROLLED_ELF_FILE_SIZE); write_text(1, " bytes\n");
    return 0;
}

static int run_plain_demo(const CollisionProfile *profile,
                          const char *output_directory,
                          const unsigned char *collision_block_a,
                          const unsigned char *collision_block_b) {
    char path_a[512];
    char path_b[512];

    if (ensure_directory(output_directory) != 0) {
        return 1;
    }
    if (join_path(path_a, sizeof(path_a), output_directory, "md5file-a.bin") != 0 ||
        join_path(path_b, sizeof(path_b), output_directory, "md5file-b.bin") != 0) {
        write_error_path("output path is too long", 0);
        return 1;
    }
    if (write_plain_file(profile, path_a, collision_block_a) != 0 ||
        write_plain_file(profile, path_b, collision_block_b) != 0) {
        return 1;
    }
    print_path_line("wrote ", path_a);
    print_path_line("wrote ", path_b);
    return 0;
}

static int parse_elf_options(int argc, char **argv, ElfOptions *options) {
    int argument;

    memset(options, 0, sizeof(*options));
    options->out_dir = "out";
    for (argument = 1; argument < argc; ++argument) {
        const char *option = argv[argument];

        if (rt_strcmp(option, "--in1") == 0 || rt_strcmp(option, "--in2") == 0 ||
            rt_strcmp(option, "--out-dir") == 0 || rt_strcmp(option, "--out1") == 0 ||
            rt_strcmp(option, "--out2") == 0 || rt_strcmp(option, "--backend") == 0) {
            if (argument + 1 >= argc) {
                write_error_path("option needs an argument: ", option);
                return 1;
            }
            ++argument;
            if (rt_strcmp(option, "--in1") == 0) {
                options->in1 = argv[argument];
            } else if (rt_strcmp(option, "--in2") == 0) {
                options->in2 = argv[argument];
            } else if (rt_strcmp(option, "--out-dir") == 0) {
                options->out_dir = argv[argument];
            } else if (rt_strcmp(option, "--out1") == 0) {
                options->out1 = argv[argument];
            } else if (rt_strcmp(option, "--backend") == 0) {
                options->backend = argv[argument];
            } else {
                options->out2 = argv[argument];
            }
        } else {
            write_error_path("unknown option ", option);
            return 1;
        }
    }
    if (options->in1 == 0 || options->in2 == 0) {
        write_error_path("--in1 and --in2 are required for ELF scaffold mode", 0);
        return 1;
    }
    return 0;
}

static int parse_controlled_elf_options(int argc, char **argv, ControlledElfOptions *options) {
    int argument;

    memset(options, 0, sizeof(*options));
    options->out_dir = "out";
    for (argument = 2; argument < argc; ++argument) {
        const char *option = argv[argument];

        if (rt_strcmp(option, "--out-dir") == 0 || rt_strcmp(option, "--out1") == 0 || rt_strcmp(option, "--out2") == 0) {
            if (argument + 1 >= argc) {
                write_error_path("option needs an argument: ", option);
                return 1;
            }
            ++argument;
            if (rt_strcmp(option, "--out-dir") == 0) {
                options->out_dir = argv[argument];
            } else if (rt_strcmp(option, "--out1") == 0) {
                options->out_true = argv[argument];
            } else {
                options->out_false = argv[argument];
            }
        } else {
            write_error_path("unknown option ", option);
            return 1;
        }
    }
    return 0;
}

static int parse_controlled_fastcoll_options(int argc, char **argv, ControlledFastcollOptions *options) {
    int argument;

    memset(options, 0, sizeof(*options));
    for (argument = 2; argument < argc; ++argument) {
        const char *option = argv[argument];

        if (rt_strcmp(option, "-q") == 0 || rt_strcmp(option, "--quiet") == 0) {
            options->quiet = 1;
        } else if (rt_strcmp(option, "-p") == 0 || rt_strcmp(option, "--prefixfile") == 0) {
            if (argument + 1 >= argc) {
                write_error_path("option needs an argument: ", option);
                return 1;
            }
            options->prefix_path = argv[++argument];
        } else if (rt_strcmp(option, "-o") == 0 || rt_strcmp(option, "--out") == 0) {
            if (argument + 2 >= argc) {
                write_error_path("-o needs two output paths", 0);
                return 1;
            }
            options->out1 = argv[++argument];
            options->out2 = argv[++argument];
            if (argument + 1 != argc) {
                write_error_path("-o must be followed by the final two arguments", 0);
                return 1;
            }
        } else {
            write_error_path("unknown option ", option);
            return 1;
        }
    }
    if (options->prefix_path == 0 || options->out1 == 0 || options->out2 == 0) {
        write_error_path("--controlled-fastcoll requires -p PREFIX -o OUT1 OUT2", 0);
        return 1;
    }
    return 0;
}

static int run_elf_scaffold(const CollisionProfile *profile,
                            const ElfOptions *options,
                            const unsigned char *collision_block_a,
                            const unsigned char *collision_block_b) {
    FileImage image_a;
    FileImage image_b;
    ElfInfo info_a;
    ElfInfo info_b;
    unsigned char digest_a[COLLISION_MAX_DIGEST_SIZE];
    unsigned char digest_b[COLLISION_MAX_DIGEST_SIZE];
    unsigned char active_block_a[COLLISION_MAX_PAYLOAD_SIZE];
    unsigned char active_block_b[COLLISION_MAX_PAYLOAD_SIZE];
    unsigned char *backend_payload_a = 0;
    unsigned char *backend_payload_b = 0;
    size_t backend_payload_sizes[2];
    const unsigned char *active_payload_a;
    const unsigned char *active_payload_b;
    size_t active_payload_size;
    char hex_a[COLLISION_MAX_DIGEST_SIZE * 2U + 1U];
    char hex_b[COLLISION_MAX_DIGEST_SIZE * 2U + 1U];
    char default_out1[512];
    char default_out2[512];
    const char *out1 = options->out1;
    const char *out2 = options->out2;
    size_t prefix_a_size;
    size_t prefix_b_size;
    size_t collision_offset;
    size_t output_size;
    int need_out_dir = out1 == 0 || out2 == 0;
    int status = 1;

    memset(&image_a, 0, sizeof(image_a));
    memset(&image_b, 0, sizeof(image_b));
    if (need_out_dir && ensure_directory(options->out_dir) != 0) {
        return 1;
    }
    if (out1 == 0) {
        if (join_path(default_out1, sizeof(default_out1), options->out_dir, "elf-md5file-a.bin") != 0) {
            write_error_path("output path is too long", 0);
            return 1;
        }
        out1 = default_out1;
    }
    if (out2 == 0) {
        if (join_path(default_out2, sizeof(default_out2), options->out_dir, "elf-md5file-b.bin") != 0) {
            write_error_path("output path is too long", 0);
            return 1;
        }
        out2 = default_out2;
    }
    if (read_file_image(options->in1, &image_a) != 0 || read_file_image(options->in2, &image_b) != 0) {
        goto cleanup;
    }
    if (validate_elf64(options->in1, &image_a, &info_a) != 0 ||
        validate_elf64(options->in2, &image_b, &info_b) != 0) {
        goto cleanup;
    }
    write_text(1, "input A: "); write_text(1, options->in1); write_text(1, " ("); write_size_value(1, image_a.size); write_text(1, " bytes, loaded bytes end at "); (void)rt_write_uint(1, info_a.loaded_end); write_text(1, ")\n");
    write_text(1, "input B: "); write_text(1, options->in2); write_text(1, " ("); write_size_value(1, image_b.size); write_text(1, " bytes, loaded bytes end at "); (void)rt_write_uint(1, info_b.loaded_end); write_text(1, ")\n");
    if (options->backend == 0) {
        write_text(1, "mode: controlled collision ELF wrapper\n");
        status = run_wrapped_elf_pair(profile, out1, out2, &image_a, &image_b);
        goto cleanup;
    }
    if (add_overflows_size(image_a.size, sizeof(elf_metadata_prelude) - 1U) ||
        add_overflows_size(image_b.size, sizeof(elf_metadata_prelude) - 1U)) {
        write_error_path("input is too large for scaffold metadata", 0);
        goto cleanup;
    }
    prefix_a_size = image_a.size + sizeof(elf_metadata_prelude) - 1U;
    prefix_b_size = image_b.size + sizeof(elf_metadata_prelude) - 1U;
    collision_offset = align_up_size(prefix_a_size > prefix_b_size ? prefix_a_size : prefix_b_size, profile->block_alignment);
    if (collision_offset < prefix_a_size || collision_offset < prefix_b_size ||
        add_overflows_size(collision_offset, profile->collision_block_size) ||
        add_overflows_size(collision_offset + profile->collision_block_size, sizeof(elf_metadata_suffix) - 1U)) {
        write_error_path("scaffold output would be too large", 0);
        goto cleanup;
    }
    if (profile->collision_block_size > sizeof(active_block_a)) {
        write_error_path("built-in collision block is too large", 0);
        goto cleanup;
    }
    memcpy(active_block_a, collision_block_a, profile->collision_block_size);
    memcpy(active_block_b, collision_block_b, profile->collision_block_size);
    active_payload_a = active_block_a;
    active_payload_b = active_block_b;
    active_payload_size = profile->collision_block_size;

    write_text(1, "mode: backend trailer scaffold\n");
    write_text(1, "metadata trailer A starts at "); write_size_value(1, image_a.size); write_text(1, "; metadata trailer B starts at "); write_size_value(1, image_b.size); write_text(1, "\n");
    write_text(1, "collision block offset: "); write_size_value(1, collision_offset); write_text(1, " (64-byte aligned)\n");

    if (options->backend != 0) {
        if (run_backend(profile, options->backend, &image_a, &image_b, collision_offset, &backend_payload_a, &backend_payload_b, backend_payload_sizes) != 0) {
            goto cleanup;
        }
        active_payload_a = backend_payload_a;
        active_payload_b = backend_payload_b;
        active_payload_size = backend_payload_sizes[0];
    }
    if (add_overflows_size(collision_offset, active_payload_size) ||
        add_overflows_size(collision_offset + active_payload_size, sizeof(elf_metadata_suffix) - 1U)) {
        write_error_path("scaffold output would be too large", 0);
        goto cleanup;
    }
    output_size = collision_offset + active_payload_size + sizeof(elf_metadata_suffix) - 1U;
    write_text(1, "collision payload size: "); write_size_value(1, active_payload_size); write_text(1, " bytes\n");
    write_text(1, "candidate output size: "); write_size_value(1, output_size); write_text(1, " bytes\n");

    collision_candidate_digest(profile, &image_a, collision_offset, active_payload_a, active_payload_size, digest_a);
    collision_candidate_digest(profile, &image_b, collision_offset, active_payload_b, active_payload_size, digest_b);
    digest_to_hex(profile, digest_a, hex_a);
    digest_to_hex(profile, digest_b, hex_b);

    if (write_candidate(out1, &image_a, collision_offset, active_payload_a, active_payload_size) != 0 ||
        write_candidate(out2, &image_b, collision_offset, active_payload_b, active_payload_size) != 0) {
        goto cleanup;
    }
    print_path_line("wrote ", out1);
    print_path_line("wrote ", out2);
    print_path_line("md5 A: ", hex_a);
    print_path_line("md5 B: ", hex_b);

    if (memcmp(digest_a, digest_b, profile->digest_size) != 0) {
        if (options->backend != 0) {
            write_error_path("candidate outputs do not collide; backend payloads are not valid for these prefixes", 0);
        } else {
            write_error_path("candidate outputs do not collide; fixed public blocks need their controlled MD5 prefix state", 0);
            write_error_path("arbitrary ELF inputs require a chosen-prefix collision backend", 0);
        }
        status = EXIT_NOT_A_COLLISION;
        goto cleanup;
    }
    print_path_line("same md5: ", hex_a);
    status = 0;

cleanup:
    rt_free(backend_payload_a);
    rt_free(backend_payload_b);
    free_file_image(&image_a);
    free_file_image(&image_b);
    return status;
}

static void print_usage(void) {
    write_text(1, "usage:\n");
    write_text(1, "  generate [OUTDIR]\n");
    write_text(1, "  generate --elf-demo [--out-dir DIR] [--out1 PATH] [--out2 PATH]\n");
    write_text(1, "  generate --controlled-fastcoll [-q] -p PREFIX -o OUT1 OUT2\n");
    write_text(1, "  generate --in1 ELF-A --in2 ELF-B [--out-dir DIR] [--out1 PATH] [--out2 PATH] [--backend COMMAND]\n");
}

int main(int argc, char **argv) {
    const CollisionProfile *profile = &md5_profile;
    unsigned char collision_block_a[COLLISION_MAX_PAYLOAD_SIZE];
    unsigned char collision_block_b[COLLISION_MAX_PAYLOAD_SIZE];
    ElfOptions options;
    ControlledElfOptions controlled_options;
    ControlledFastcollOptions fastcoll_options;

    if (profile->digest_size > COLLISION_MAX_DIGEST_SIZE ||
        profile->collision_block_size > COLLISION_MAX_PAYLOAD_SIZE ||
        profile->controlled_payload_size > COLLISION_MAX_PAYLOAD_SIZE) {
        write_error_path("selected collision profile exceeds internal limits", 0);
        return 1;
    }
    if (decode_hex_bytes(profile->collision_block_a_hex, collision_block_a, profile->collision_block_size) != 0 ||
        decode_hex_bytes(profile->collision_block_b_hex, collision_block_b, profile->collision_block_size) != 0) {
        write_error_path("internal collision block is malformed", 0);
        return 1;
    }
    if (argc >= 2 && (rt_strcmp(argv[1], "--help") == 0 || rt_strcmp(argv[1], "-h") == 0)) {
        print_usage();
        return 0;
    }
    if (argc >= 2 && rt_strcmp(argv[1], "--elf-demo") == 0) {
        if (parse_controlled_elf_options(argc, argv, &controlled_options) != 0) {
            print_usage();
            return 1;
        }
        return run_controlled_elf_demo(profile, &controlled_options);
    }
    if (argc >= 2 && rt_strcmp(argv[1], "--controlled-fastcoll") == 0) {
        if (parse_controlled_fastcoll_options(argc, argv, &fastcoll_options) != 0) {
            print_usage();
            return 1;
        }
        return run_controlled_fastcoll(profile, &fastcoll_options);
    }
    if (argc == 1 || argv[1][0] != '-') {
        const char *output_directory = argc > 1 ? argv[1] : "out";

        return run_plain_demo(profile, output_directory, collision_block_a, collision_block_b);
    }
    if (parse_elf_options(argc, argv, &options) != 0) {
        print_usage();
        return 1;
    }
    return run_elf_scaffold(profile, &options, collision_block_a, collision_block_b);
}
