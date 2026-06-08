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
#define MD5_ALIGNMENT 64U
#define COLLISION_BLOCK_SIZE 128U
#define BACKEND_MAX_PAYLOAD_SIZE (16U * 1024U * 1024U)
#define BACKEND_MAX_ARGS 32U
#define PATH_BUFFER_SIZE 512U
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

static int decode_hex_block(const char *hex, unsigned char out[COLLISION_BLOCK_SIZE]) {
    size_t offset;

    if (rt_strlen(hex) != COLLISION_BLOCK_SIZE * 2U) {
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
    static const unsigned char zeroes[MD5_ALIGNMENT] = {0};

    while (padding != 0U) {
        size_t chunk = padding < sizeof(zeroes) ? padding : sizeof(zeroes);

        if (rt_write_all(fd, zeroes, chunk) != 0) {
            write_error_path("cannot write ", path);
            return 1;
        }
        padding -= chunk;
    }
    return 0;
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

static int run_backend(const char *command,
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
        set_backend_env_size("NEWOS_MD5_BLOCK_SIZE", COLLISION_BLOCK_SIZE) != 0 ||
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

static int write_plain_file(const char *path,
                            const unsigned char collision_block[COLLISION_BLOCK_SIZE]) {
    int fd = platform_open_write(path, 0644U);

    if (fd < 0) {
        write_error_path("cannot open ", path);
        return 1;
    }
    if (rt_write_all(fd, collision_block, COLLISION_BLOCK_SIZE) != 0 ||
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
        write_error_path("output path is too long", 0);
        return 1;
    }
    if (write_plain_file(path_a, collision_block_a) != 0 ||
        write_plain_file(path_b, collision_block_b) != 0) {
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
    unsigned char *backend_payload_a = 0;
    unsigned char *backend_payload_b = 0;
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
    if (add_overflows_size(image_a.size, sizeof(elf_metadata_prelude) - 1U) ||
        add_overflows_size(image_b.size, sizeof(elf_metadata_prelude) - 1U)) {
        write_error_path("input is too large for scaffold metadata", 0);
        goto cleanup;
    }
    prefix_a_size = image_a.size + sizeof(elf_metadata_prelude) - 1U;
    prefix_b_size = image_b.size + sizeof(elf_metadata_prelude) - 1U;
    collision_offset = align_up_size(prefix_a_size > prefix_b_size ? prefix_a_size : prefix_b_size, MD5_ALIGNMENT);
    if (collision_offset < prefix_a_size || collision_offset < prefix_b_size ||
        add_overflows_size(collision_offset, COLLISION_BLOCK_SIZE) ||
        add_overflows_size(collision_offset + COLLISION_BLOCK_SIZE, sizeof(elf_metadata_suffix) - 1U)) {
        write_error_path("scaffold output would be too large", 0);
        goto cleanup;
    }
    memcpy(active_block_a, collision_block_a, COLLISION_BLOCK_SIZE);
    memcpy(active_block_b, collision_block_b, COLLISION_BLOCK_SIZE);
    active_payload_a = active_block_a;
    active_payload_b = active_block_b;
    active_payload_size = COLLISION_BLOCK_SIZE;

    write_text(1, "input A: "); write_text(1, options->in1); write_text(1, " ("); write_size_value(1, image_a.size); write_text(1, " bytes, loaded bytes end at "); (void)rt_write_uint(1, info_a.loaded_end); write_text(1, ")\n");
    write_text(1, "input B: "); write_text(1, options->in2); write_text(1, " ("); write_size_value(1, image_b.size); write_text(1, " bytes, loaded bytes end at "); (void)rt_write_uint(1, info_b.loaded_end); write_text(1, ")\n");
    write_text(1, "metadata trailer A starts at "); write_size_value(1, image_a.size); write_text(1, "; metadata trailer B starts at "); write_size_value(1, image_b.size); write_text(1, "\n");
    write_text(1, "collision block offset: "); write_size_value(1, collision_offset); write_text(1, " (64-byte aligned)\n");

    if (options->backend != 0) {
        if (run_backend(options->backend, &image_a, &image_b, collision_offset, &backend_payload_a, &backend_payload_b, backend_payload_sizes) != 0) {
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

    md5_candidate(&image_a, collision_offset, active_payload_a, active_payload_size, digest_a);
    md5_candidate(&image_b, collision_offset, active_payload_b, active_payload_size, digest_b);
    digest_to_hex(digest_a, hex_a);
    digest_to_hex(digest_b, hex_b);

    if (write_candidate(out1, &image_a, collision_offset, active_payload_a, active_payload_size) != 0 ||
        write_candidate(out2, &image_b, collision_offset, active_payload_b, active_payload_size) != 0) {
        goto cleanup;
    }
    print_path_line("wrote ", out1);
    print_path_line("wrote ", out2);
    print_path_line("md5 A: ", hex_a);
    print_path_line("md5 B: ", hex_b);

    if (memcmp(digest_a, digest_b, sizeof(digest_a)) != 0) {
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
    write_text(1, "  generate --in1 ELF-A --in2 ELF-B [--out-dir DIR] [--out1 PATH] [--out2 PATH] [--backend COMMAND]\n");
}

int main(int argc, char **argv) {
    unsigned char collision_block_a[COLLISION_BLOCK_SIZE];
    unsigned char collision_block_b[COLLISION_BLOCK_SIZE];
    ElfOptions options;

    if (decode_hex_block(collision_block_a_hex, collision_block_a) != 0 ||
        decode_hex_block(collision_block_b_hex, collision_block_b) != 0) {
        write_error_path("internal collision block is malformed", 0);
        return 1;
    }
    if (argc >= 2 && (rt_strcmp(argv[1], "--help") == 0 || rt_strcmp(argv[1], "-h") == 0)) {
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
