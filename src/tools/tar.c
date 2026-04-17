#include "archive_util.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define TAR_BLOCK_SIZE 512
#define TAR_PATH_CAPACITY 1024
#define TAR_ENTRY_CAPACITY 1024

typedef enum {
    TAR_COMPRESS_NONE = 0,
    TAR_COMPRESS_GZIP = 1,
    TAR_COMPRESS_BZIP2 = 2,
    TAR_COMPRESS_XZ = 3
} TarCompression;

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} TarHeader;

static int read_exact(int fd, unsigned char *buffer, size_t count) {
    size_t offset = 0;

    while (offset < count) {
        long bytes = platform_read(fd, buffer + offset, count - offset);
        if (bytes <= 0) {
            return -1;
        }
        offset += (size_t)bytes;
    }

    return 0;
}

static int skip_exact(int fd, unsigned long long count) {
    unsigned char buffer[256];

    while (count > 0ULL) {
        size_t chunk = (count > (unsigned long long)sizeof(buffer)) ? sizeof(buffer) : (size_t)count;
        if (read_exact(fd, buffer, chunk) != 0) {
            return -1;
        }
        count -= (unsigned long long)chunk;
    }

    return 0;
}

static int is_zero_block(const unsigned char *block) {
    size_t i;

    for (i = 0; i < TAR_BLOCK_SIZE; ++i) {
        if (block[i] != 0) {
            return 0;
        }
    }

    return 1;
}

static int has_suffix(const char *text, const char *suffix) {
    size_t text_len = rt_strlen(text);
    size_t suffix_len = rt_strlen(suffix);

    if (suffix_len > text_len) {
        return 0;
    }

    return rt_strcmp(text + text_len - suffix_len, suffix) == 0;
}

static int is_dash_path(const char *path) {
    return path != 0 && path[0] == '-' && path[1] == '\0';
}

static TarCompression detect_compression(const char *archive_name) {
    if (has_suffix(archive_name, ".tar.gz") || has_suffix(archive_name, ".tgz") || has_suffix(archive_name, ".gz")) {
        return TAR_COMPRESS_GZIP;
    }
    if (has_suffix(archive_name, ".tar.bz2") || has_suffix(archive_name, ".bz2")) {
        return TAR_COMPRESS_BZIP2;
    }
    if (has_suffix(archive_name, ".tar.xz") || has_suffix(archive_name, ".xz")) {
        return TAR_COMPRESS_XZ;
    }
    return TAR_COMPRESS_NONE;
}

static const char *compression_suffix(TarCompression compression) {
    if (compression == TAR_COMPRESS_GZIP) return ".gz";
    if (compression == TAR_COMPRESS_BZIP2) return ".bz2";
    if (compression == TAR_COMPRESS_XZ) return ".xz";
    return "";
}

static const char *compressor_name(TarCompression compression) {
    if (compression == TAR_COMPRESS_GZIP) return "gzip";
    if (compression == TAR_COMPRESS_BZIP2) return "bzip2";
    if (compression == TAR_COMPRESS_XZ) return "xz";
    return "";
}

static const char *decompressor_name(TarCompression compression) {
    if (compression == TAR_COMPRESS_GZIP) return "gunzip";
    if (compression == TAR_COMPRESS_BZIP2) return "bunzip2";
    if (compression == TAR_COMPRESS_XZ) return "unxz";
    return "";
}

static int contains_slash(const char *text) {
    size_t i = 0;
    while (text[i] != '\0') {
        if (text[i] == '/') {
            return 1;
        }
        i += 1;
    }
    return 0;
}

static int path_is_absolute(const char *path) {
    return path != 0 && path[0] == '/';
}

static int remember_archive_path(const char *path, char *buffer, size_t buffer_size) {
    char cwd[TAR_PATH_CAPACITY];

    if (path == 0) {
        return -1;
    }
    if (is_dash_path(path) || path_is_absolute(path)) {
        rt_copy_string(buffer, buffer_size, path);
        return 0;
    }
    if (platform_get_current_directory(cwd, sizeof(cwd)) != 0) {
        return -1;
    }
    return tool_join_path(cwd, path, buffer, buffer_size);
}

static void tar_copy_name(char *dst, size_t dst_size, const char *src) {
    size_t i = 0;
    while (i + 1 < dst_size && src[i] != '\0') {
        dst[i] = src[i];
        i += 1;
    }
    if (dst_size > 0) {
        dst[i < dst_size ? i : (dst_size - 1)] = '\0';
    }
}

static int build_temp_path(const char *base_path, const char *suffix, char *buffer, size_t buffer_size) {
    size_t base_len = rt_strlen(base_path);
    size_t suffix_len = rt_strlen(suffix);

    if (base_len + suffix_len + 1 > buffer_size) {
        return -1;
    }

    memcpy(buffer, base_path, base_len);
    memcpy(buffer + base_len, suffix, suffix_len + 1);
    return 0;
}

static void get_program_dir(const char *argv0, char *buffer, size_t buffer_size) {
    size_t len;
    size_t i;

    if (argv0 == 0 || argv0[0] == '\0' || !contains_slash(argv0)) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    len = rt_strlen(argv0);
    if (len + 1 > buffer_size) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    memcpy(buffer, argv0, len + 1);
    for (i = len; i > 0; --i) {
        if (buffer[i - 1] == '/') {
            if (i == 1) {
                buffer[1] = '\0';
            } else {
                buffer[i - 1] = '\0';
            }
            return;
        }
    }

    rt_copy_string(buffer, buffer_size, ".");
}

static int build_helper_path(const char *argv0, const char *tool_name, char *buffer, size_t buffer_size) {
    char dir[TAR_PATH_CAPACITY];

    if (argv0 == 0 || !contains_slash(argv0)) {
        rt_copy_string(buffer, buffer_size, tool_name);
        return 0;
    }

    get_program_dir(argv0, dir, sizeof(dir));
    return tool_join_path(dir, tool_name, buffer, buffer_size);
}

static int run_helper_tool(const char *argv0, const char *tool_name, const char *archive_path_name) {
    char helper_path[TAR_PATH_CAPACITY];
    char *const helper_argv[] = { helper_path, (char *)archive_path_name, 0 };
    int pid = 0;
    int status = 1;

    if (build_helper_path(argv0, tool_name, helper_path, sizeof(helper_path)) != 0) {
        return -1;
    }

    if (platform_spawn_process(helper_argv, -1, -1, 0, 0, 0, &pid) != 0) {
        return -1;
    }

    if (platform_wait_process(pid, &status) != 0) {
        return -1;
    }

    return (status == 0) ? 0 : -1;
}

static unsigned int header_checksum(const TarHeader *header) {
    const unsigned char *bytes = (const unsigned char *)header;
    unsigned int sum = 0;
    size_t i;

    for (i = 0; i < TAR_BLOCK_SIZE; ++i) {
        if (i >= 148 && i < 156) {
            sum += (unsigned int)' ';
        } else {
            sum += bytes[i];
        }
    }

    return sum;
}

static int write_padding(int fd, unsigned long long size) {
    unsigned char zeros[TAR_BLOCK_SIZE];
    unsigned long long padding = (TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;

    rt_memset(zeros, 0, sizeof(zeros));
    if (padding == 0ULL) {
        return 0;
    }

    return rt_write_all(fd, zeros, (size_t)padding);
}

static int fill_header(TarHeader *header, const char *stored_name, const PlatformDirEntry *entry, char typeflag) {
    rt_memset(header, 0, sizeof(*header));

    if (rt_strlen(stored_name) >= sizeof(header->name)) {
        return -1;
    }

    tar_copy_name(header->name, sizeof(header->name), stored_name);
    archive_write_octal(header->mode, sizeof(header->mode), (unsigned long long)(entry->mode & 0777U));
    archive_write_octal(header->uid, sizeof(header->uid), 0);
    archive_write_octal(header->gid, sizeof(header->gid), 0);
    archive_write_octal(header->size, sizeof(header->size), (typeflag == '5') ? 0ULL : entry->size);
    archive_write_octal(header->mtime, sizeof(header->mtime), (unsigned long long)entry->mtime);
    header->typeflag = typeflag;
    tar_copy_name(header->magic, sizeof(header->magic), "ustar");
    tar_copy_name(header->version, sizeof(header->version), "00");
    tar_copy_name(header->uname, sizeof(header->uname), entry->owner);
    tar_copy_name(header->gname, sizeof(header->gname), entry->group);
    archive_write_octal(header->checksum, sizeof(header->checksum), header_checksum(header));
    return 0;
}

static int archive_path(int archive_fd, const char *path, const char *stored_name, int verbose) {
    PlatformDirEntry entries[TAR_ENTRY_CAPACITY];
    size_t count = 0;
    int is_directory = 0;
    size_t i;

    if (platform_collect_entries(path, 1, entries, TAR_ENTRY_CAPACITY, &count, &is_directory) != 0 || count == 0) {
        return -1;
    }

    if (is_directory) {
        TarHeader header;
        char dir_name[TAR_PATH_CAPACITY];
        size_t len = rt_strlen(stored_name);

        if (len + 2 > sizeof(dir_name)) {
            return -1;
        }

        memcpy(dir_name, stored_name, len);
        if (len == 0 || dir_name[len - 1] != '/') {
            dir_name[len] = '/';
            dir_name[len + 1] = '\0';
        } else {
            dir_name[len] = '\0';
        }

        if (fill_header(&header, dir_name, &entries[0], '5') != 0 || rt_write_all(archive_fd, &header, sizeof(header)) != 0) {
            return -1;
        }
        if (verbose && rt_write_line(1, dir_name) != 0) {
            return -1;
        }

        for (i = 0; i < count; ++i) {
            char child_path[TAR_PATH_CAPACITY];
            char child_name[TAR_PATH_CAPACITY];

            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
                continue;
            }

            if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0 ||
                tool_join_path(stored_name, entries[i].name, child_name, sizeof(child_name)) != 0) {
                return -1;
            }

            if (archive_path(archive_fd, child_path, child_name, verbose) != 0) {
                return -1;
            }
        }

        return 0;
    }

    {
        TarHeader header;
        int file_fd;
        unsigned char buffer[4096];

        if (fill_header(&header, stored_name, &entries[0], '0') != 0 || rt_write_all(archive_fd, &header, sizeof(header)) != 0) {
            return -1;
        }
        if (verbose && rt_write_line(1, stored_name) != 0) {
            return -1;
        }

        file_fd = platform_open_read(path);
        if (file_fd < 0) {
            return -1;
        }

        for (;;) {
            long bytes = platform_read(file_fd, buffer, sizeof(buffer));
            if (bytes < 0) {
                platform_close(file_fd);
                return -1;
            }
            if (bytes == 0) {
                break;
            }
            if (rt_write_all(archive_fd, buffer, (size_t)bytes) != 0) {
                platform_close(file_fd);
                return -1;
            }
        }

        platform_close(file_fd);
        return write_padding(archive_fd, entries[0].size);
    }
}

static int ensure_parent_dirs(char *path) {
    size_t i;

    for (i = 1; path[i] != '\0'; ++i) {
        if (path[i] == '/') {
            path[i] = '\0';
            (void)platform_make_directory(path, 0755U);
            path[i] = '/';
        }
    }

    return 0;
}

static int extract_archive(int archive_fd, int list_only, int verbose) {
    unsigned char block[TAR_BLOCK_SIZE];

    for (;;) {
        TarHeader *header;
        unsigned long long size;
        unsigned long long padding;
        char path[TAR_PATH_CAPACITY];

        if (read_exact(archive_fd, block, sizeof(block)) != 0) {
            return -1;
        }

        if (is_zero_block(block)) {
            return 0;
        }

        header = (TarHeader *)block;
        tar_copy_name(path, sizeof(path), header->name);
        size = archive_parse_octal(header->size, sizeof(header->size));
        padding = (TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;

        if (list_only || verbose) {
            if (rt_write_line(1, path) != 0) {
                return -1;
            }
        }

        if (list_only) {
            if (skip_exact(archive_fd, size + padding) != 0) {
                return -1;
            }
            continue;
        }

        ensure_parent_dirs(path);

        if (header->typeflag == '5') {
            (void)platform_make_directory(path, 0755U);
            continue;
        }

        {
            int out_fd = platform_open_write(path, 0644U);
            unsigned char data[4096];
            unsigned long long remaining = size;

            if (out_fd < 0) {
                return -1;
            }

            while (remaining > 0ULL) {
                size_t chunk = (remaining > (unsigned long long)sizeof(data)) ? sizeof(data) : (size_t)remaining;
                if (read_exact(archive_fd, data, chunk) != 0 || rt_write_all(out_fd, data, chunk) != 0) {
                    platform_close(out_fd);
                    return -1;
                }
                remaining -= (unsigned long long)chunk;
            }

            platform_close(out_fd);
            (void)platform_change_mode(path, (unsigned int)archive_parse_octal(header->mode, sizeof(header->mode)));
        }

        if (padding > 0ULL && skip_exact(archive_fd, padding) != 0) {
            return -1;
        }
    }
}

static int compress_archive_file(const char *argv0, TarCompression compression, const char *plain_archive_path, const char *final_archive_path) {
    char compressed_output[TAR_PATH_CAPACITY];

    if (compression == TAR_COMPRESS_NONE) {
        return 0;
    }

    if (build_temp_path(plain_archive_path, compression_suffix(compression), compressed_output, sizeof(compressed_output)) != 0) {
        return -1;
    }

    if (run_helper_tool(argv0, compressor_name(compression), plain_archive_path) != 0) {
        return -1;
    }

    (void)platform_remove_file(final_archive_path);
    if (platform_rename_path(compressed_output, final_archive_path) != 0) {
        return -1;
    }

    (void)platform_remove_file(plain_archive_path);
    return 0;
}

static int prepare_archive_for_read(const char *argv0, TarCompression compression, const char *archive_path_name, char *temp_copy_path, size_t temp_copy_size, char *temp_plain_path, size_t temp_plain_size, const char **read_path_out) {
    char compressed_copy[TAR_PATH_CAPACITY];

    if (compression == TAR_COMPRESS_NONE) {
        *read_path_out = archive_path_name;
        return 0;
    }

    if (build_temp_path(archive_path_name, ".newos-tmp", temp_plain_path, temp_plain_size) != 0 ||
        build_temp_path(temp_plain_path, compression_suffix(compression), compressed_copy, sizeof(compressed_copy)) != 0) {
        return -1;
    }

    rt_copy_string(temp_copy_path, temp_copy_size, compressed_copy);
    if (tool_copy_file(archive_path_name, compressed_copy) != 0) {
        return -1;
    }

    if (run_helper_tool(argv0, decompressor_name(compression), compressed_copy) != 0) {
        (void)platform_remove_file(compressed_copy);
        return -1;
    }

    *read_path_out = temp_plain_path;
    return 0;
}

int main(int argc, char **argv) {
    int create_mode = 0;
    int extract_mode = 0;
    int list_mode = 0;
    int verbose_mode = 0;
    TarCompression compression = TAR_COMPRESS_NONE;
    char archive_path_buffer[TAR_PATH_CAPACITY];
    const char *archive_path_name = 0;
    const char *change_dir = 0;
    int path_start = argc;
    int i;

    archive_path_buffer[0] = '\0';

    if (argc < 3) {
        rt_write_line(2, "Usage: tar [-v] [-C dir] -cf archive.tar paths... | tar [-v] [-C dir] -xf archive.tar | tar -tf archive.tar");
        return 1;
    }

    for (i = 1; i < argc; ++i) {
        if (rt_strcmp(argv[i], "--help") == 0) {
            rt_write_line(2, "Usage: tar [-v] [-C dir] -cf archive.tar paths... | tar [-v] [-C dir] -xf archive.tar | tar -tf archive.tar");
            return 0;
        }
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            path_start = i;
            break;
        }

        {
            const char *opt = argv[i] + 1;
            while (*opt != '\0') {
                if (*opt == 'c') {
                    create_mode = 1;
                } else if (*opt == 'x') {
                    extract_mode = 1;
                } else if (*opt == 't') {
                    list_mode = 1;
                } else if (*opt == 'v') {
                    verbose_mode = 1;
                } else if (*opt == 'z') {
                    compression = TAR_COMPRESS_GZIP;
                } else if (*opt == 'j') {
                    compression = TAR_COMPRESS_BZIP2;
                } else if (*opt == 'J') {
                    compression = TAR_COMPRESS_XZ;
                } else if (*opt == 'C') {
                    if (i + 1 >= argc) {
                        rt_write_line(2, "tar: directory required after -C");
                        return 1;
                    }
                    change_dir = argv[++i];
                    break;
                } else if (*opt == 'f') {
                    if (i + 1 >= argc) {
                        rt_write_line(2, "tar: archive path required after -f");
                        return 1;
                    }
                    if (remember_archive_path(argv[++i], archive_path_buffer, sizeof(archive_path_buffer)) != 0) {
                        rt_write_line(2, "tar: archive path too long");
                        return 1;
                    }
                    archive_path_name = archive_path_buffer;
                    break;
                }
                opt += 1;
            }
        }
    }

    if (archive_path_name == 0) {
        rt_write_line(2, "tar: archive path required");
        return 1;
    }

    if (compression == TAR_COMPRESS_NONE) {
        compression = detect_compression(archive_path_name);
    }

    if (create_mode) {
        char plain_archive_path[TAR_PATH_CAPACITY];
        const char *write_path = archive_path_name;
        int archive_fd;
        int close_archive_fd = 1;
        unsigned char zeros[TAR_BLOCK_SIZE * 2];

        if (path_start >= argc) {
            rt_write_line(2, "tar: no input paths provided");
            return 1;
        }

        if (is_dash_path(archive_path_name) && compression != TAR_COMPRESS_NONE) {
            rt_write_line(2, "tar: compressed output to stdout is not supported yet");
            return 1;
        }

        if (compression != TAR_COMPRESS_NONE) {
            if (build_temp_path(archive_path_name, ".newos-plain", plain_archive_path, sizeof(plain_archive_path)) != 0) {
                return 1;
            }
            write_path = plain_archive_path;
        }

        if (is_dash_path(write_path)) {
            archive_fd = 1;
            close_archive_fd = 0;
        } else {
            archive_fd = platform_open_write(write_path, 0644U);
            if (archive_fd < 0) {
                return 1;
            }
        }

        if (change_dir != 0 && platform_change_directory(change_dir) != 0) {
            if (close_archive_fd) {
                platform_close(archive_fd);
            }
            rt_write_line(2, "tar: cannot change directory");
            return 1;
        }

        rt_memset(zeros, 0, sizeof(zeros));
        for (i = path_start; i < argc; ++i) {
            if (rt_strcmp(argv[i], archive_path_name) == 0) {
                continue;
            }
            if (archive_path(archive_fd, argv[i], argv[i], verbose_mode) != 0) {
                if (close_archive_fd) {
                    platform_close(archive_fd);
                }
                return 1;
            }
        }

        if (rt_write_all(archive_fd, zeros, sizeof(zeros)) != 0) {
            if (close_archive_fd) {
                platform_close(archive_fd);
            }
            return 1;
        }

        if (close_archive_fd) {
            platform_close(archive_fd);
        }

        if (compression != TAR_COMPRESS_NONE) {
            return (compress_archive_file(argv[0], compression, write_path, archive_path_name) == 0) ? 0 : 1;
        }

        return 0;
    }

    if (extract_mode || list_mode) {
        char temp_copy_path[TAR_PATH_CAPACITY];
        char temp_plain_path[TAR_PATH_CAPACITY];
        const char *read_path = archive_path_name;
        int archive_fd;
        int close_archive_fd = 1;
        int result;

        temp_copy_path[0] = '\0';
        temp_plain_path[0] = '\0';

        if (is_dash_path(archive_path_name) && compression != TAR_COMPRESS_NONE) {
            rt_write_line(2, "tar: compressed input from stdin is not supported yet");
            return 1;
        }

        if (prepare_archive_for_read(argv[0], compression, archive_path_name, temp_copy_path, sizeof(temp_copy_path), temp_plain_path, sizeof(temp_plain_path), &read_path) != 0) {
            return 1;
        }

        if (is_dash_path(read_path)) {
            archive_fd = 0;
            close_archive_fd = 0;
        } else {
            archive_fd = platform_open_read(read_path);
        }
        if (archive_fd < 0) {
            (void)platform_remove_file(temp_copy_path);
            (void)platform_remove_file(temp_plain_path);
            return 1;
        }

        if (change_dir != 0 && platform_change_directory(change_dir) != 0) {
            if (close_archive_fd) {
                platform_close(archive_fd);
            }
            (void)platform_remove_file(temp_copy_path);
            (void)platform_remove_file(temp_plain_path);
            rt_write_line(2, "tar: cannot change directory");
            return 1;
        }

        result = extract_archive(archive_fd, list_mode, verbose_mode);
        if (close_archive_fd) {
            platform_close(archive_fd);
        }
        (void)platform_remove_file(temp_copy_path);
        (void)platform_remove_file(temp_plain_path);
        return (result == 0) ? 0 : 1;
    }

    rt_write_line(2, "tar: choose one of -c, -x, or -t");
    return 1;
}
