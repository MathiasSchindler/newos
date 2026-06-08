#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "crypto/md5.h"

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
#define MD5_ALIGNMENT 64U
#define COLLISION_BLOCK_SIZE 128U
#define BACKEND_MAX_PAYLOAD_SIZE (16U * 1024U * 1024U)
#define EXIT_NOT_A_COLLISION 2

static const char collision_block_a_hex[] =
    "d131dd02c5e6eec4693d9a0698aff95c2fcab58712467eab4004583eb8fb7f89"
    "55ad340609f4b30283e488832571415a085125e8f7cdc99fd91dbdf280373c5b"
    "d8823e3156348f5bae6dacd436c919c6dd53e2b487da03fd02396306d248cda0"
    "e99f33420f577ee8ce54b67080a80d1ec69821bcb6a8839396f9652b6ff72a70";

static const char collision_block_b_hex[] =
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

typedef struct {
    unsigned char *data;
    size_t size;
    mode_t mode;
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

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int decode_hex_block(const char *hex, unsigned char out[COLLISION_BLOCK_SIZE]) {
    size_t offset;

    if (strlen(hex) != COLLISION_BLOCK_SIZE * 2U) {
        return 1;
    }
    for (offset = 0; offset < COLLISION_BLOCK_SIZE; ++offset) {
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

static void digest_to_hex(const unsigned char digest[CRYPTO_MD5_DIGEST_SIZE], char hex[CRYPTO_MD5_DIGEST_SIZE * 2U + 1U]) {
    static const char digits[] = "0123456789abcdef";
    size_t offset;

    for (offset = 0; offset < CRYPTO_MD5_DIGEST_SIZE; ++offset) {
        hex[offset * 2U] = digits[(digest[offset] >> 4) & 0x0fU];
        hex[offset * 2U + 1U] = digits[digest[offset] & 0x0fU];
    }
    hex[CRYPTO_MD5_DIGEST_SIZE * 2U] = '\0';
}

static int join_path(char *buffer, size_t buffer_size, const char *directory, const char *name) {
    int written = snprintf(buffer, buffer_size, "%s/%s", directory, name);

    return written > 0 && (size_t)written < buffer_size ? 0 : 1;
}

static int ensure_directory(const char *path) {
    struct stat metadata;

    if (mkdir(path, 0777) == 0) {
        return 0;
    }
    if (errno == EEXIST && stat(path, &metadata) == 0 && S_ISDIR(metadata.st_mode)) {
        return 0;
    }
    fprintf(stderr, "generate: cannot create %s: %s\n", path, strerror(errno));
    return 1;
}

static int set_backend_env_size(const char *name, size_t value) {
    char buffer[64];
    int written = snprintf(buffer, sizeof(buffer), "%zu", value);

    if (written <= 0 || (size_t)written >= sizeof(buffer) || setenv(name, buffer, 1) != 0) {
        fprintf(stderr, "generate: cannot set %s\n", name);
        return 1;
    }
    return 0;
}

static int set_backend_env_path(const char *name, const char *path) {
    if (setenv(name, path, 1) != 0) {
        fprintf(stderr, "generate: cannot set %s: %s\n", name, strerror(errno));
        return 1;
    }
    return 0;
}

static int make_temp_directory(char *buffer, size_t buffer_size) {
    const char *base = getenv("TMPDIR");
    int written;

    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    written = snprintf(buffer, buffer_size, "%s/newos-md5files-XXXXXX", base);
    if (written <= 0 || (size_t)written >= buffer_size) {
        fprintf(stderr, "generate: temporary directory path is too long\n");
        return 1;
    }
    if (mkdtemp(buffer) == NULL) {
        fprintf(stderr, "generate: cannot create temporary directory: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static void free_file_image(FileImage *image) {
    if (image != NULL) {
        free(image->data);
        image->data = NULL;
        image->size = 0U;
    }
}

static int read_file_image(const char *path, FileImage *image) {
    struct stat metadata;
    FILE *file;

    memset(image, 0, sizeof(*image));
    if (stat(path, &metadata) != 0) {
        fprintf(stderr, "generate: cannot stat %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISREG(metadata.st_mode)) {
        fprintf(stderr, "generate: %s is not a regular file\n", path);
        return 1;
    }
    if ((uintmax_t)metadata.st_size > (uintmax_t)(size_t)-1) {
        fprintf(stderr, "generate: %s is too large for this scaffold\n", path);
        return 1;
    }
    image->size = (size_t)metadata.st_size;
    image->mode = metadata.st_mode & 07777;
    image->data = (unsigned char *)malloc(image->size == 0U ? 1U : image->size);
    if (image->data == NULL) {
        fprintf(stderr, "generate: out of memory reading %s\n", path);
        return 1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "generate: cannot open %s: %s\n", path, strerror(errno));
        free_file_image(image);
        return 1;
    }
    if (image->size != 0U && fread(image->data, 1U, image->size, file) != image->size) {
        fprintf(stderr, "generate: cannot read %s: %s\n", path, ferror(file) ? strerror(errno) : "short read");
        fclose(file);
        free_file_image(image);
        return 1;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "generate: cannot close %s: %s\n", path, strerror(errno));
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
        fprintf(stderr, "generate: %s is not an ELF file\n", path);
        return 1;
    }
    if (image->data[4] != ELFCLASS64 || image->data[5] != ELFDATA2LSB || image->data[6] != EV_CURRENT) {
        fprintf(stderr, "generate: %s is not a little-endian ELF64 file\n", path);
        return 1;
    }
    info->type = read_u16_le(image->data + 16U);
    if (info->type != ET_EXEC && info->type != ET_DYN) {
        fprintf(stderr, "generate: %s is not an executable/shared ELF image\n", path);
        return 1;
    }
    info->phoff = read_u64_le(image->data + 32U);
    phentsize = read_u16_le(image->data + 54U);
    info->phnum = read_u16_le(image->data + 56U);
    if (info->phnum == 0U) {
        fprintf(stderr, "generate: %s has no program headers\n", path);
        return 1;
    }
    if (phentsize != ELF_PHDR_SIZE) {
        fprintf(stderr, "generate: %s has unsupported ELF program header size %u\n", path, (unsigned int)phentsize);
        return 1;
    }
    ph_table_size = (uint64_t)info->phnum * phentsize;
    if (!range_valid_u64(info->phoff, ph_table_size, file_size)) {
        fprintf(stderr, "generate: %s has program headers outside the file\n", path);
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
                fprintf(stderr, "generate: %s has a LOAD segment with p_memsz < p_filesz\n", path);
                return 1;
            }
            if (!range_valid_u64(offset, file_bytes, file_size) || end < offset) {
                fprintf(stderr, "generate: %s has a LOAD segment outside the file\n", path);
                return 1;
            }
            if (end > info->loaded_end) {
                info->loaded_end = end;
            }
        }
    }
    if (info->loaded_end == 0U) {
        fprintf(stderr, "generate: %s has no loadable file bytes\n", path);
        return 1;
    }
    return 0;
}

static void md5_candidate(const FileImage *image,
                          size_t collision_offset,
                          const unsigned char *collision_payload,
                          size_t collision_payload_size,
                          unsigned char digest[CRYPTO_MD5_DIGEST_SIZE]) {
    static const unsigned char zeroes[MD5_ALIGNMENT] = {0};
    CryptoMd5Context context;
    size_t prefix_size = image->size + sizeof(elf_metadata_prelude) - 1U;
    size_t padding = collision_offset - prefix_size;

    crypto_md5_init(&context);
    crypto_md5_update(&context, image->data, image->size);
    crypto_md5_update(&context, elf_metadata_prelude, sizeof(elf_metadata_prelude) - 1U);
    while (padding != 0U) {
        size_t chunk = padding < sizeof(zeroes) ? padding : sizeof(zeroes);

        crypto_md5_update(&context, zeroes, chunk);
        padding -= chunk;
    }
    crypto_md5_update(&context, collision_payload, collision_payload_size);
    crypto_md5_update(&context, elf_metadata_suffix, sizeof(elf_metadata_suffix) - 1U);
    crypto_md5_final(&context, digest);
}

static int read_backend_payload_file(const char *path, unsigned char **payload_out, size_t *payload_size_out) {
    FileImage image;

    if (read_file_image(path, &image) != 0) {
        fprintf(stderr, "generate: backend did not produce a readable payload at %s\n", path);
        return 1;
    }
    if (image.size == 0U || image.size > BACKEND_MAX_PAYLOAD_SIZE) {
        fprintf(stderr, "generate: backend payload %s has unsupported size %zu\n", path, image.size);
        free_file_image(&image);
        return 1;
    }
    *payload_out = image.data;
    *payload_size_out = image.size;
    image.data = NULL;
    free_file_image(&image);
    return 0;
}

static int write_prefix_file(const char *path, const FileImage *image, size_t collision_offset) {
    static const unsigned char zeroes[MD5_ALIGNMENT] = {0};
    size_t prefix_size = image->size + sizeof(elf_metadata_prelude) - 1U;
    size_t padding = collision_offset - prefix_size;
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        fprintf(stderr, "generate: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if ((image->size != 0U && fwrite(image->data, 1U, image->size, file) != image->size) ||
        fwrite(elf_metadata_prelude, 1U, sizeof(elf_metadata_prelude) - 1U, file) != sizeof(elf_metadata_prelude) - 1U) {
        fprintf(stderr, "generate: cannot write %s: %s\n", path, strerror(errno));
        fclose(file);
        return 1;
    }
    while (padding != 0U) {
        size_t chunk = padding < sizeof(zeroes) ? padding : sizeof(zeroes);

        if (fwrite(zeroes, 1U, chunk, file) != chunk) {
            fprintf(stderr, "generate: cannot write %s: %s\n", path, strerror(errno));
            fclose(file);
            return 1;
        }
        padding -= chunk;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "generate: cannot close %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

static int run_backend(const char *command,
                       const FileImage *image_a,
                       const FileImage *image_b,
                       size_t collision_offset,
                       unsigned char **payload_a_out,
                       unsigned char **payload_b_out,
                       size_t *payload_size_out) {
    char temp_dir[512];
    char prefix_a_path[512] = "";
    char prefix_b_path[512] = "";
    char block_a_path[512] = "";
    char block_b_path[512] = "";
    int backend_status;
    int status = 1;

    if (make_temp_directory(temp_dir, sizeof(temp_dir)) != 0) {
        return 1;
    }
    if (join_path(prefix_a_path, sizeof(prefix_a_path), temp_dir, "prefix-a.bin") != 0 ||
        join_path(prefix_b_path, sizeof(prefix_b_path), temp_dir, "prefix-b.bin") != 0 ||
        join_path(block_a_path, sizeof(block_a_path), temp_dir, "block-a.bin") != 0 ||
        join_path(block_b_path, sizeof(block_b_path), temp_dir, "block-b.bin") != 0) {
        fprintf(stderr, "generate: temporary path is too long\n");
        goto cleanup;
    }
    if (write_prefix_file(prefix_a_path, image_a, collision_offset) != 0 ||
        write_prefix_file(prefix_b_path, image_b, collision_offset) != 0) {
        goto cleanup;
    }
    if (set_backend_env_path("NEWOS_MD5_PREFIX_A", prefix_a_path) != 0 ||
        set_backend_env_path("NEWOS_MD5_PREFIX_B", prefix_b_path) != 0 ||
        set_backend_env_path("NEWOS_MD5_BLOCK_A", block_a_path) != 0 ||
        set_backend_env_path("NEWOS_MD5_BLOCK_B", block_b_path) != 0 ||
        set_backend_env_size("NEWOS_MD5_COLLISION_OFFSET", collision_offset) != 0 ||
        set_backend_env_size("NEWOS_MD5_BLOCK_SIZE", COLLISION_BLOCK_SIZE) != 0 ||
        set_backend_env_size("NEWOS_MD5_MAX_PAYLOAD_SIZE", BACKEND_MAX_PAYLOAD_SIZE) != 0) {
        goto cleanup;
    }

    printf("backend: %s\n", command);
    printf("backend prefix A: %s\n", prefix_a_path);
    printf("backend prefix B: %s\n", prefix_b_path);
    backend_status = system(command);
    if (backend_status == -1) {
        fprintf(stderr, "generate: cannot run backend: %s\n", strerror(errno));
        goto cleanup;
    }
    if (!WIFEXITED(backend_status) || WEXITSTATUS(backend_status) != 0) {
        fprintf(stderr, "generate: backend failed");
        if (WIFEXITED(backend_status)) {
            fprintf(stderr, " with exit status %d", WEXITSTATUS(backend_status));
        }
        fprintf(stderr, "\n");
        goto cleanup;
    }
    if (read_backend_payload_file(block_a_path, payload_a_out, payload_size_out) != 0 ||
        read_backend_payload_file(block_b_path, payload_b_out, payload_size_out + 1) != 0) {
        goto cleanup;
    }
    if (*payload_size_out != *(payload_size_out + 1)) {
        fprintf(stderr, "generate: backend payload sizes differ: %zu vs %zu\n", *payload_size_out, *(payload_size_out + 1));
        goto cleanup;
    }
    status = 0;

cleanup:
    remove(prefix_a_path);
    remove(prefix_b_path);
    remove(block_a_path);
    remove(block_b_path);
    rmdir(temp_dir);
    return status;
}

static int write_candidate(const char *path,
                           const FileImage *image,
                           size_t collision_offset,
                           const unsigned char *collision_payload,
                           size_t collision_payload_size) {
    static const unsigned char zeroes[MD5_ALIGNMENT] = {0};
    size_t prefix_size = image->size + sizeof(elf_metadata_prelude) - 1U;
    size_t padding = collision_offset - prefix_size;
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        fprintf(stderr, "generate: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if ((image->size != 0U && fwrite(image->data, 1U, image->size, file) != image->size) ||
        fwrite(elf_metadata_prelude, 1U, sizeof(elf_metadata_prelude) - 1U, file) != sizeof(elf_metadata_prelude) - 1U) {
        fprintf(stderr, "generate: cannot write %s: %s\n", path, strerror(errno));
        fclose(file);
        return 1;
    }
    while (padding != 0U) {
        size_t chunk = padding < sizeof(zeroes) ? padding : sizeof(zeroes);

        if (fwrite(zeroes, 1U, chunk, file) != chunk) {
            fprintf(stderr, "generate: cannot write %s: %s\n", path, strerror(errno));
            fclose(file);
            return 1;
        }
        padding -= chunk;
    }
    if (fwrite(collision_payload, 1U, collision_payload_size, file) != collision_payload_size ||
        fwrite(elf_metadata_suffix, 1U, sizeof(elf_metadata_suffix) - 1U, file) != sizeof(elf_metadata_suffix) - 1U) {
        fprintf(stderr, "generate: cannot write %s: %s\n", path, strerror(errno));
        fclose(file);
        return 1;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "generate: cannot close %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (chmod(path, image->mode) != 0) {
        fprintf(stderr, "generate: cannot chmod %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

static int write_plain_file(const char *path,
                            const unsigned char collision_block[COLLISION_BLOCK_SIZE]) {
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        fprintf(stderr, "generate: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (fwrite(collision_block, 1U, COLLISION_BLOCK_SIZE, file) != COLLISION_BLOCK_SIZE ||
        fwrite(plain_common_suffix, 1U, sizeof(plain_common_suffix) - 1U, file) != sizeof(plain_common_suffix) - 1U) {
        fprintf(stderr, "generate: cannot write %s: %s\n", path, strerror(errno));
        fclose(file);
        return 1;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "generate: cannot close %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

static int run_plain_demo(const char *output_directory,
                          const unsigned char collision_block_a[COLLISION_BLOCK_SIZE],
                          const unsigned char collision_block_b[COLLISION_BLOCK_SIZE]) {
    char path_a[512];
    char path_b[512];

    if (ensure_directory(output_directory) != 0) {
        return 1;
    }
    if (join_path(path_a, sizeof(path_a), output_directory, "md5file-a.bin") != 0 ||
        join_path(path_b, sizeof(path_b), output_directory, "md5file-b.bin") != 0) {
        fprintf(stderr, "generate: output path is too long\n");
        return 1;
    }
    if (write_plain_file(path_a, collision_block_a) != 0 ||
        write_plain_file(path_b, collision_block_b) != 0) {
        return 1;
    }
    printf("wrote %s\n", path_a);
    printf("wrote %s\n", path_b);
    return 0;
}

static int parse_elf_options(int argc, char **argv, ElfOptions *options) {
    int argument;

    memset(options, 0, sizeof(*options));
    options->out_dir = "out";
    for (argument = 1; argument < argc; ++argument) {
        const char *option = argv[argument];

        if (strcmp(option, "--in1") == 0 || strcmp(option, "--in2") == 0 ||
            strcmp(option, "--out-dir") == 0 || strcmp(option, "--out1") == 0 ||
            strcmp(option, "--out2") == 0 || strcmp(option, "--backend") == 0) {
            if (argument + 1 >= argc) {
                fprintf(stderr, "generate: %s needs an argument\n", option);
                return 1;
            }
            ++argument;
            if (strcmp(option, "--in1") == 0) {
                options->in1 = argv[argument];
            } else if (strcmp(option, "--in2") == 0) {
                options->in2 = argv[argument];
            } else if (strcmp(option, "--out-dir") == 0) {
                options->out_dir = argv[argument];
            } else if (strcmp(option, "--out1") == 0) {
                options->out1 = argv[argument];
            } else if (strcmp(option, "--backend") == 0) {
                options->backend = argv[argument];
            } else {
                options->out2 = argv[argument];
            }
        } else {
            fprintf(stderr, "generate: unknown option %s\n", option);
            return 1;
        }
    }
    if (options->in1 == NULL || options->in2 == NULL) {
        fprintf(stderr, "generate: --in1 and --in2 are required for ELF scaffold mode\n");
        return 1;
    }
    return 0;
}

static int run_elf_scaffold(const ElfOptions *options,
                            const unsigned char collision_block_a[COLLISION_BLOCK_SIZE],
                            const unsigned char collision_block_b[COLLISION_BLOCK_SIZE]) {
    FileImage image_a;
    FileImage image_b;
    ElfInfo info_a;
    ElfInfo info_b;
    unsigned char digest_a[CRYPTO_MD5_DIGEST_SIZE];
    unsigned char digest_b[CRYPTO_MD5_DIGEST_SIZE];
    unsigned char active_block_a[COLLISION_BLOCK_SIZE];
    unsigned char active_block_b[COLLISION_BLOCK_SIZE];
    unsigned char *backend_payload_a = NULL;
    unsigned char *backend_payload_b = NULL;
    size_t backend_payload_sizes[2];
    const unsigned char *active_payload_a;
    const unsigned char *active_payload_b;
    size_t active_payload_size;
    char hex_a[CRYPTO_MD5_DIGEST_SIZE * 2U + 1U];
    char hex_b[CRYPTO_MD5_DIGEST_SIZE * 2U + 1U];
    char default_out1[512];
    char default_out2[512];
    const char *out1 = options->out1;
    const char *out2 = options->out2;
    size_t prefix_a_size;
    size_t prefix_b_size;
    size_t collision_offset;
    size_t output_size;
    int status = 1;

    memset(&image_a, 0, sizeof(image_a));
    memset(&image_b, 0, sizeof(image_b));
    if (out1 == NULL) {
        if (join_path(default_out1, sizeof(default_out1), options->out_dir, "elf-md5file-a.bin") != 0) {
            fprintf(stderr, "generate: output path is too long\n");
            return 1;
        }
        out1 = default_out1;
    }
    if (out2 == NULL) {
        if (join_path(default_out2, sizeof(default_out2), options->out_dir, "elf-md5file-b.bin") != 0) {
            fprintf(stderr, "generate: output path is too long\n");
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
    if (add_overflows_size(image_a.size, sizeof(elf_metadata_prelude) - 1U) ||
        add_overflows_size(image_b.size, sizeof(elf_metadata_prelude) - 1U)) {
        fprintf(stderr, "generate: input is too large for scaffold metadata\n");
        goto cleanup;
    }
    prefix_a_size = image_a.size + sizeof(elf_metadata_prelude) - 1U;
    prefix_b_size = image_b.size + sizeof(elf_metadata_prelude) - 1U;
    collision_offset = align_up_size(prefix_a_size > prefix_b_size ? prefix_a_size : prefix_b_size, MD5_ALIGNMENT);
    if (collision_offset < prefix_a_size || collision_offset < prefix_b_size ||
        add_overflows_size(collision_offset, COLLISION_BLOCK_SIZE) ||
        add_overflows_size(collision_offset + COLLISION_BLOCK_SIZE, sizeof(elf_metadata_suffix) - 1U)) {
        fprintf(stderr, "generate: scaffold output would be too large\n");
        goto cleanup;
    }
    memcpy(active_block_a, collision_block_a, COLLISION_BLOCK_SIZE);
    memcpy(active_block_b, collision_block_b, COLLISION_BLOCK_SIZE);
    active_payload_a = active_block_a;
    active_payload_b = active_block_b;
    active_payload_size = COLLISION_BLOCK_SIZE;

    printf("input A: %s (%zu bytes, loaded bytes end at %llu)\n", options->in1, image_a.size, (unsigned long long)info_a.loaded_end);
    printf("input B: %s (%zu bytes, loaded bytes end at %llu)\n", options->in2, image_b.size, (unsigned long long)info_b.loaded_end);
    printf("metadata trailer A starts at %zu; metadata trailer B starts at %zu\n", image_a.size, image_b.size);
    printf("collision block offset: %zu (64-byte aligned)\n", collision_offset);

    if (options->backend != NULL) {
        if (run_backend(options->backend, &image_a, &image_b, collision_offset, &backend_payload_a, &backend_payload_b, backend_payload_sizes) != 0) {
            goto cleanup;
        }
        active_payload_a = backend_payload_a;
        active_payload_b = backend_payload_b;
        active_payload_size = backend_payload_sizes[0];
    }
    if (add_overflows_size(collision_offset, active_payload_size) ||
        add_overflows_size(collision_offset + active_payload_size, sizeof(elf_metadata_suffix) - 1U)) {
        fprintf(stderr, "generate: scaffold output would be too large\n");
        goto cleanup;
    }
    output_size = collision_offset + active_payload_size + sizeof(elf_metadata_suffix) - 1U;
    printf("collision payload size: %zu bytes\n", active_payload_size);
    printf("candidate output size: %zu bytes\n", output_size);

    md5_candidate(&image_a, collision_offset, active_payload_a, active_payload_size, digest_a);
    md5_candidate(&image_b, collision_offset, active_payload_b, active_payload_size, digest_b);
    digest_to_hex(digest_a, hex_a);
    digest_to_hex(digest_b, hex_b);

    if (write_candidate(out1, &image_a, collision_offset, active_payload_a, active_payload_size) != 0 ||
        write_candidate(out2, &image_b, collision_offset, active_payload_b, active_payload_size) != 0) {
        goto cleanup;
    }
    printf("wrote %s\n", out1);
    printf("wrote %s\n", out2);
    printf("md5 A: %s\n", hex_a);
    printf("md5 B: %s\n", hex_b);

    if (memcmp(digest_a, digest_b, sizeof(digest_a)) != 0) {
        if (options->backend != NULL) {
            fprintf(stderr,
                    "generate: candidate outputs do not collide; backend payloads are not valid for these prefixes\n");
        } else {
            fprintf(stderr,
                    "generate: candidate outputs do not collide; fixed public blocks need their controlled MD5 prefix state\n"
                    "generate: arbitrary ELF inputs require a chosen-prefix collision backend\n");
        }
        status = EXIT_NOT_A_COLLISION;
        goto cleanup;
    }
    printf("same md5: %s\n", hex_a);
    status = 0;

cleanup:
    free(backend_payload_a);
    free(backend_payload_b);
    free_file_image(&image_a);
    free_file_image(&image_b);
    return status;
}

static void print_usage(void) {
    printf("usage:\n");
    printf("  generate [OUTDIR]\n");
    printf("  generate --in1 ELF-A --in2 ELF-B [--out-dir DIR] [--out1 PATH] [--out2 PATH] [--backend COMMAND]\n");
}

int main(int argc, char **argv) {
    unsigned char collision_block_a[COLLISION_BLOCK_SIZE];
    unsigned char collision_block_b[COLLISION_BLOCK_SIZE];
    ElfOptions options;

    if (decode_hex_block(collision_block_a_hex, collision_block_a) != 0 ||
        decode_hex_block(collision_block_b_hex, collision_block_b) != 0) {
        fprintf(stderr, "generate: internal collision block is malformed\n");
        return 1;
    }
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage();
        return 0;
    }
    if (argc == 1 || argv[1][0] != '-') {
        const char *output_directory = argc > 1 ? argv[1] : "out";

        return run_plain_demo(output_directory, collision_block_a, collision_block_b);
    }
    if (parse_elf_options(argc, argv, &options) != 0) {
        print_usage();
        return 1;
    }
    return run_elf_scaffold(&options, collision_block_a, collision_block_b);
}
