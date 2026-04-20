#include "archive_util.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define TAR_BLOCK_SIZE 512
#define TAR_PATH_CAPACITY 1024
#define TAR_ENTRY_CAPACITY 1024
#define TAR_MAX_PATTERNS 64

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

static const char *tar_base_name(const char *path) {
    const char *base = path;
    size_t i = 0;

    if (path == 0) {
        return "item";
    }
    while (path[i] != '\0') {
        if (path[i] == '/') {
            base = path + i + 1;
        }
        i += 1;
    }
    return (base[0] != '\0') ? base : "item";
}

static void tar_copy_trimmed_path(char *dst, size_t dst_size, const char *src) {
    size_t len = rt_strlen(src);

    while (len > 1 && src[len - 1] == '/') {
        len -= 1;
    }
    if (len + 1 > dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int tar_split_header_path(TarHeader *header, const char *stored_name) {
    char normalized[TAR_PATH_CAPACITY];
    size_t len;
    size_t i;

    tar_copy_trimmed_path(normalized, sizeof(normalized), stored_name);
    len = rt_strlen(normalized);
    if (len == 0) {
        return -1;
    }

    if (len < sizeof(header->name)) {
        tar_copy_name(header->name, sizeof(header->name), normalized);
        return 0;
    }

    for (i = len; i > 0; --i) {
        size_t prefix_len;
        size_t name_len;
        if (normalized[i - 1] != '/') {
            continue;
        }
        prefix_len = i - 1;
        name_len = len - i;
        if (prefix_len > 0 && prefix_len < sizeof(header->prefix) && name_len > 0 && name_len < sizeof(header->name)) {
            memcpy(header->prefix, normalized, prefix_len);
            header->prefix[prefix_len] = '\0';
            memcpy(header->name, normalized + i, name_len);
            header->name[name_len] = '\0';
            return 0;
        }
    }

    return -1;
}

static int tar_name_requires_extension(const char *stored_name) {
    TarHeader header;

    rt_memset(&header, 0, sizeof(header));
    return tar_split_header_path(&header, stored_name) != 0;
}

static int tar_link_requires_extension(const char *link_name) {
    return link_name != 0 && rt_strlen(link_name) >= sizeof(((TarHeader *)0)->linkname);
}

static int tar_header_path(const TarHeader *header, char *buffer, size_t buffer_size) {
    if (header->prefix[0] != '\0') {
        return tool_join_path(header->prefix, header->name, buffer, buffer_size);
    }
    rt_copy_string(buffer, buffer_size, header->name);
    return 0;
}

static int tar_path_matches(const char *path, char patterns[][TAR_PATH_CAPACITY], size_t pattern_count) {
    char normalized_path[TAR_PATH_CAPACITY];
    size_t i;

    if (pattern_count == 0) {
        return 1;
    }

    tar_copy_trimmed_path(normalized_path, sizeof(normalized_path), path);
    for (i = 0; i < pattern_count; ++i) {
        char normalized_pattern[TAR_PATH_CAPACITY];
        tar_copy_trimmed_path(normalized_pattern, sizeof(normalized_pattern), patterns[i]);
        if (tool_wildcard_match(normalized_pattern, normalized_path)) {
            return 1;
        }
    }

    return 0;
}

static int tar_path_is_unsafe(const char *path) {
    size_t i = 0;

    if (path == 0 || path[0] == '\0' || path[0] == '/') {
        return 1;
    }

    while (path[i] != '\0') {
        size_t start;
        size_t len;

        while (path[i] == '/') {
            i += 1;
        }
        start = i;
        while (path[i] != '\0' && path[i] != '/') {
            i += 1;
        }
        len = i - start;
        if (len == 2 && path[start] == '.' && path[start + 1] == '.') {
            return 1;
        }
    }

    return 0;
}

static int tar_strip_components(const char *path, unsigned int strip_components, char *buffer, size_t buffer_size) {
    size_t i = 0;
    unsigned int stripped = 0;

    while (path[i] == '/') {
        i += 1;
    }
    while (stripped < strip_components) {
        if (path[i] == '\0') {
            return 1;
        }
        while (path[i] != '\0' && path[i] != '/') {
            i += 1;
        }
        while (path[i] == '/') {
            i += 1;
        }
        stripped += 1U;
    }

    if (path[i] == '\0') {
        return 1;
    }

    rt_copy_string(buffer, buffer_size, path + i);
    return 0;
}

static int tar_write_display_name(const char *path, int is_directory) {
    size_t len = rt_strlen(path);

    if (is_directory && (len == 0 || path[len - 1] != '/')) {
        char display[TAR_PATH_CAPACITY];
        if (len + 2 > sizeof(display)) {
            return rt_write_line(1, path);
        }
        memcpy(display, path, len);
        display[len] = '/';
        display[len + 1] = '\0';
        return rt_write_line(1, display);
    }

    return rt_write_line(1, path);
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

static int fill_header(TarHeader *header,
                       const char *stored_name,
                       const char *link_name,
                       const PlatformDirEntry *entry,
                       char typeflag) {
    rt_memset(header, 0, sizeof(*header));

    if (tar_split_header_path(header, stored_name) != 0) {
        tar_copy_name(header->name, sizeof(header->name), tar_base_name(stored_name));
    }

    archive_write_octal(header->mode, sizeof(header->mode), (unsigned long long)(entry->mode & 0777U));
    archive_write_octal(header->uid, sizeof(header->uid), 0);
    archive_write_octal(header->gid, sizeof(header->gid), 0);
    archive_write_octal(header->size,
                        sizeof(header->size),
                        (typeflag == '5' || typeflag == '1' || typeflag == '2') ? 0ULL : entry->size);
    archive_write_octal(header->mtime, sizeof(header->mtime), (unsigned long long)entry->mtime);
    header->typeflag = typeflag;
    tar_copy_name(header->magic, sizeof(header->magic), "ustar");
    tar_copy_name(header->version, sizeof(header->version), "00");
    if (link_name != 0) {
        tar_copy_name(header->linkname, sizeof(header->linkname), link_name);
    }
    tar_copy_name(header->uname, sizeof(header->uname), entry->owner);
    tar_copy_name(header->gname, sizeof(header->gname), entry->group);
    archive_write_octal(header->checksum, sizeof(header->checksum), header_checksum(header));
    return 0;
}

static int write_special_metadata_header(int archive_fd, const char *value, char typeflag) {
    TarHeader header;
    PlatformDirEntry entry;

    rt_memset(&entry, 0, sizeof(entry));
    entry.mode = 0644U;
    entry.size = (unsigned long long)(rt_strlen(value) + 1U);
    entry.mtime = platform_get_epoch_time();
    if (fill_header(&header, "././@LongLink", 0, &entry, typeflag) != 0) {
        return -1;
    }
    if (rt_write_all(archive_fd, &header, sizeof(header)) != 0 ||
        rt_write_all(archive_fd, value, (size_t)entry.size) != 0) {
        return -1;
    }
    return write_padding(archive_fd, entry.size);
}

static int write_archive_header(int archive_fd,
                                const char *stored_name,
                                const char *link_name,
                                const PlatformDirEntry *entry,
                                char typeflag) {
    TarHeader header;

    if (tar_name_requires_extension(stored_name) && write_special_metadata_header(archive_fd, stored_name, 'L') != 0) {
        return -1;
    }
    if (tar_link_requires_extension(link_name) && write_special_metadata_header(archive_fd, link_name, 'K') != 0) {
        return -1;
    }
    if (fill_header(&header, stored_name, link_name, entry, typeflag) != 0) {
        return -1;
    }
    return rt_write_all(archive_fd, &header, sizeof(header));
}

static int archive_path(int archive_fd,
                        const char *path,
                        const char *stored_name,
                        int verbose,
                        char exclude_patterns[][TAR_PATH_CAPACITY],
                        size_t exclude_count) {
    PlatformDirEntry entry;
    int is_directory = 0;
    char link_target[TAR_PATH_CAPACITY];
    int is_symlink = 0;

    if (exclude_count > 0 && tar_path_matches(stored_name, exclude_patterns, exclude_count)) {
        return 0;
    }

    if (platform_get_path_info(path, &entry) != 0) {
        return -1;
    }
    is_directory = entry.is_dir;
    link_target[0] = '\0';
    if (!is_directory && platform_read_symlink(path, link_target, sizeof(link_target)) == 0) {
        is_symlink = 1;
    }

    if (is_directory) {
        PlatformDirEntry entries[TAR_ENTRY_CAPACITY];
        size_t count = 0;
        size_t i;
        if (write_archive_header(archive_fd, stored_name, 0, &entry, '5') != 0) {
            return -1;
        }
        if (verbose && tar_write_display_name(stored_name, 1) != 0) {
            return -1;
        }

        if (platform_collect_entries(path, 1, entries, TAR_ENTRY_CAPACITY, &count, &is_directory) != 0 || !is_directory) {
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

            if (archive_path(archive_fd, child_path, child_name, verbose, exclude_patterns, exclude_count) != 0) {
                return -1;
            }
        }

        return 0;
    }

    {
        int file_fd;
        unsigned char buffer[4096];

        if (is_symlink) {
            if (write_archive_header(archive_fd, stored_name, link_target, &entry, '2') != 0) {
                return -1;
            }
            if (verbose && tar_write_display_name(stored_name, 0) != 0) {
                return -1;
            }
            return 0;
        }

        if (write_archive_header(archive_fd, stored_name, 0, &entry, '0') != 0) {
            return -1;
        }
        if (verbose && tar_write_display_name(stored_name, 0) != 0) {
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
        return write_padding(archive_fd, entry.size);
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

static int tar_read_text_payload(int archive_fd, unsigned long long size, char *buffer, size_t buffer_size) {
    unsigned char chunk[256];
    size_t used = 0;

    if (buffer == 0 || buffer_size == 0U) {
        return -1;
    }

    while (size > 0ULL) {
        size_t piece = (size > (unsigned long long)sizeof(chunk)) ? sizeof(chunk) : (size_t)size;
        size_t copy_length = 0U;

        if (read_exact(archive_fd, chunk, piece) != 0) {
            return -1;
        }
        if (used + 1U < buffer_size) {
            copy_length = piece;
            if (used + copy_length + 1U > buffer_size) {
                copy_length = buffer_size - used - 1U;
            }
            if (copy_length > 0U) {
                memcpy(buffer + used, chunk, copy_length);
                used += copy_length;
            }
        }
        size -= (unsigned long long)piece;
    }

    while (used > 0U && (buffer[used - 1U] == '\0' || buffer[used - 1U] == '\n')) {
        used -= 1U;
    }
    buffer[used] = '\0';
    return 0;
}

static void tar_copy_record_value(char *buffer, size_t buffer_size, const char *value, size_t value_length) {
    if (buffer_size == 0U) {
        return;
    }
    if (value_length + 1U > buffer_size) {
        value_length = buffer_size - 1U;
    }
    memcpy(buffer, value, value_length);
    buffer[value_length] = '\0';
}

static void tar_parse_pax_payload(const char *data,
                                  size_t data_length,
                                  char *path_buffer,
                                  size_t path_buffer_size,
                                  char *link_buffer,
                                  size_t link_buffer_size) {
    size_t index = 0U;

    while (index < data_length) {
        size_t record_start = index;
        unsigned long long record_length = 0ULL;
        size_t payload_start;
        size_t payload_length;
        size_t key_length = 0U;

        while (index < data_length && data[index] >= '0' && data[index] <= '9') {
            record_length = (record_length * 10ULL) + (unsigned long long)(data[index] - '0');
            index += 1U;
        }
        if (record_length == 0ULL || index >= data_length || data[index] != ' ') {
            break;
        }
        payload_start = index + 1U;
        if (record_start + (size_t)record_length > data_length || payload_start <= record_start) {
            break;
        }
        payload_length = (record_start + (size_t)record_length) - payload_start;
        if (payload_length > 0U && data[payload_start + payload_length - 1U] == '\n') {
            payload_length -= 1U;
        }
        while (key_length < payload_length && data[payload_start + key_length] != '=') {
            key_length += 1U;
        }
        if (key_length < payload_length) {
            const char *value = data + payload_start + key_length + 1U;
            size_t value_length = payload_length - key_length - 1U;

            if (key_length == 4U && rt_strncmp(data + payload_start, "path", 4) == 0) {
                tar_copy_record_value(path_buffer, path_buffer_size, value, value_length);
            } else if (key_length == 8U && rt_strncmp(data + payload_start, "linkpath", 8) == 0) {
                tar_copy_record_value(link_buffer, link_buffer_size, value, value_length);
            }
        }
        index = record_start + (size_t)record_length;
    }
}

static int tar_read_pax_attributes(int archive_fd,
                                   unsigned long long size,
                                   char *path_buffer,
                                   size_t path_buffer_size,
                                   char *link_buffer,
                                   size_t link_buffer_size) {
    char pax_data[TAR_PATH_CAPACITY * 4];

    pax_data[0] = '\0';
    if (tar_read_text_payload(archive_fd, size, pax_data, sizeof(pax_data)) != 0) {
        return -1;
    }
    tar_parse_pax_payload(pax_data, rt_strlen(pax_data), path_buffer, path_buffer_size, link_buffer, link_buffer_size);
    return 0;
}

static int extract_archive(int archive_fd,
                           int list_only,
                           int verbose,
                           char member_patterns[][TAR_PATH_CAPACITY],
                           size_t member_count,
                           char exclude_patterns[][TAR_PATH_CAPACITY],
                           size_t exclude_count,
                           unsigned int strip_components,
                           int allow_absolute_names) {
    unsigned char block[TAR_BLOCK_SIZE];
    char pending_path[TAR_PATH_CAPACITY];
    char pending_link[TAR_PATH_CAPACITY];

    pending_path[0] = '\0';
    pending_link[0] = '\0';

    for (;;) {
        TarHeader *header;
        unsigned long long size;
        unsigned long long padding;
        char stored_path[TAR_PATH_CAPACITY];
        char output_path[TAR_PATH_CAPACITY];
        char link_target[TAR_PATH_CAPACITY];
        int is_directory;
        int strip_result;
        char typeflag;

        if (read_exact(archive_fd, block, sizeof(block)) != 0) {
            return -1;
        }

        if (is_zero_block(block)) {
            return 0;
        }

        header = (TarHeader *)block;
        size = archive_parse_octal(header->size, sizeof(header->size));
        padding = (TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
        typeflag = (header->typeflag == '\0') ? '0' : header->typeflag;

        if (typeflag == 'L' || typeflag == 'K') {
            char *target = (typeflag == 'L') ? pending_path : pending_link;
            if (tar_read_text_payload(archive_fd, size, target, TAR_PATH_CAPACITY) != 0) {
                return -1;
            }
            if (padding > 0ULL && skip_exact(archive_fd, padding) != 0) {
                return -1;
            }
            continue;
        }

        if (typeflag == 'x' || typeflag == 'g') {
            if (typeflag == 'x') {
                pending_path[0] = '\0';
                pending_link[0] = '\0';
                if (tar_read_pax_attributes(archive_fd,
                                            size,
                                            pending_path,
                                            sizeof(pending_path),
                                            pending_link,
                                            sizeof(pending_link)) != 0) {
                    return -1;
                }
            } else if (skip_exact(archive_fd, size) != 0) {
                return -1;
            }
            if (padding > 0ULL && skip_exact(archive_fd, padding) != 0) {
                return -1;
            }
            continue;
        }

        if (tar_header_path(header, stored_path, sizeof(stored_path)) != 0) {
            return -1;
        }
        if (pending_path[0] != '\0') {
            rt_copy_string(stored_path, sizeof(stored_path), pending_path);
            pending_path[0] = '\0';
        }
        link_target[0] = '\0';
        if (pending_link[0] != '\0') {
            rt_copy_string(link_target, sizeof(link_target), pending_link);
            pending_link[0] = '\0';
        } else {
            rt_copy_string(link_target, sizeof(link_target), header->linkname);
        }
        is_directory = (typeflag == '5');

        if (!tar_path_matches(stored_path, member_patterns, member_count) ||
            (exclude_count > 0 && tar_path_matches(stored_path, exclude_patterns, exclude_count))) {
            if (skip_exact(archive_fd, size + padding) != 0) {
                return -1;
            }
            continue;
        }

        if (!list_only && !allow_absolute_names &&
            (tar_path_is_unsafe(stored_path) ||
             ((typeflag == '1' || typeflag == '2') && link_target[0] != '\0' && tar_path_is_unsafe(link_target)))) {
            tool_write_error("tar", "refusing unsafe path in archive: ", stored_path);
            return -1;
        }

        strip_result = tar_strip_components(stored_path, strip_components, output_path, sizeof(output_path));
        if (strip_result != 0) {
            if (skip_exact(archive_fd, size + padding) != 0) {
                return -1;
            }
            continue;
        }

        if (list_only || verbose) {
            if (tar_write_display_name(output_path, is_directory) != 0) {
                return -1;
            }
        }

        if (list_only) {
            if (skip_exact(archive_fd, size + padding) != 0) {
                return -1;
            }
            continue;
        }

        if (typeflag != '0' && typeflag != '5' && typeflag != '1' && typeflag != '2') {
            if (skip_exact(archive_fd, size + padding) != 0) {
                return -1;
            }
            continue;
        }

        ensure_parent_dirs(output_path);

        if (is_directory) {
            (void)platform_make_directory(output_path, 0755U);
            continue;
        }

        if (typeflag == '2') {
            (void)platform_remove_file(output_path);
            if (platform_create_symbolic_link(link_target, output_path) != 0) {
                return -1;
            }
            if (size + padding > 0ULL && skip_exact(archive_fd, size + padding) != 0) {
                return -1;
            }
            continue;
        }

        if (typeflag == '1') {
            const char *link_path = link_target;
            char stripped_link[TAR_PATH_CAPACITY];

            if (strip_components != 0U) {
                if (tar_strip_components(link_target, strip_components, stripped_link, sizeof(stripped_link)) != 0) {
                    if (skip_exact(archive_fd, size + padding) != 0) {
                        return -1;
                    }
                    continue;
                }
                link_path = stripped_link;
            }
            (void)platform_remove_file(output_path);
            if (platform_create_hard_link(link_path, output_path) != 0) {
                return -1;
            }
            if (size + padding > 0ULL && skip_exact(archive_fd, size + padding) != 0) {
                return -1;
            }
            continue;
        }

        {
            int out_fd = platform_open_write(output_path, 0644U);
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
            (void)platform_change_mode(output_path, (unsigned int)archive_parse_octal(header->mode, sizeof(header->mode)));
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
    int allow_absolute_names = 0;
    TarCompression compression = TAR_COMPRESS_NONE;
    char archive_path_buffer[TAR_PATH_CAPACITY];
    char exclude_patterns[TAR_MAX_PATTERNS][TAR_PATH_CAPACITY];
    char member_patterns[TAR_MAX_PATTERNS][TAR_PATH_CAPACITY];
    size_t exclude_count = 0;
    size_t member_count = 0;
    const char *archive_path_name = 0;
    const char *change_dir = 0;
    unsigned int strip_components = 0U;
    int path_start = argc;
    int i;

    archive_path_buffer[0] = '\0';

    if (argc < 3) {
        rt_write_line(2, "Usage: tar [-v] [-P] [-C dir] [--exclude PATTERN] [-cf archive.tar paths...]");
        rt_write_line(2, "       tar [-v] [-P] [-C dir] [--strip-components N] -xf archive.tar [members...]");
        rt_write_line(2, "       tar [-v] [--strip-components N] -tf archive.tar [members...]");
        return 1;
    }

    for (i = 1; i < argc; ++i) {
        if (rt_strcmp(argv[i], "--help") == 0) {
            rt_write_line(2, "Usage: tar [-v] [-P] [-C dir] [--exclude PATTERN] [-cf archive.tar paths...]");
            rt_write_line(2, "       tar [-v] [-P] [-C dir] [--strip-components N] -xf archive.tar [members...]");
            rt_write_line(2, "       tar [-v] [--strip-components N] -tf archive.tar [members...]");
            return 0;
        }
        if (rt_strcmp(argv[i], "--") == 0) {
            path_start = i + 1;
            break;
        }
        if (rt_strcmp(argv[i], "--absolute-names") == 0) {
            allow_absolute_names = 1;
            continue;
        }
        if (rt_strncmp(argv[i], "--exclude=", 10) == 0) {
            if (exclude_count >= TAR_MAX_PATTERNS) {
                rt_write_line(2, "tar: too many exclude patterns");
                return 1;
            }
            rt_copy_string(exclude_patterns[exclude_count++], TAR_PATH_CAPACITY, argv[i] + 10);
            continue;
        }
        if (rt_strcmp(argv[i], "--exclude") == 0) {
            if (i + 1 >= argc || exclude_count >= TAR_MAX_PATTERNS) {
                rt_write_line(2, "tar: pattern required after --exclude");
                return 1;
            }
            rt_copy_string(exclude_patterns[exclude_count++], TAR_PATH_CAPACITY, argv[++i]);
            continue;
        }
        if (rt_strncmp(argv[i], "--strip-components=", 19) == 0) {
            unsigned long long strip_value = 0ULL;
            if (tool_parse_uint_arg(argv[i] + 19, &strip_value, "tar", "strip-components") != 0) {
                return 1;
            }
            strip_components = (unsigned int)strip_value;
            continue;
        }
        if (rt_strcmp(argv[i], "--strip-components") == 0) {
            unsigned long long strip_value = 0ULL;
            if (i + 1 >= argc || tool_parse_uint_arg(argv[++i], &strip_value, "tar", "strip-components") != 0) {
                return 1;
            }
            strip_components = (unsigned int)strip_value;
            continue;
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
                } else if (*opt == 'P') {
                    allow_absolute_names = 1;
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

    if ((create_mode + extract_mode + list_mode) != 1) {
        rt_write_line(2, "tar: choose exactly one of -c, -x, or -t");
        return 1;
    }

    if (compression == TAR_COMPRESS_NONE) {
        compression = detect_compression(archive_path_name);
    }

    if ((extract_mode || list_mode) && path_start < argc) {
        for (i = path_start; i < argc; ++i) {
            if (member_count >= TAR_MAX_PATTERNS) {
                rt_write_line(2, "tar: too many member patterns");
                return 1;
            }
            rt_copy_string(member_patterns[member_count++], TAR_PATH_CAPACITY, argv[i]);
        }
    }

    if (create_mode && strip_components != 0U) {
        rt_write_line(2, "tar: --strip-components is only valid with -x or -t");
        return 1;
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
            if ((!is_dash_path(archive_path_name) && tool_paths_equal(argv[i], archive_path_name)) ||
                (!is_dash_path(write_path) && tool_paths_equal(argv[i], write_path))) {
                continue;
            }
            if (archive_path(archive_fd, argv[i], argv[i], verbose_mode, exclude_patterns, exclude_count) != 0) {
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

        result = extract_archive(archive_fd,
                                 list_mode,
                                 verbose_mode,
                                 member_patterns,
                                 member_count,
                                 exclude_patterns,
                                 exclude_count,
                                 strip_components,
                                 allow_absolute_names);
        if (close_archive_fd) {
            platform_close(archive_fd);
        }
        (void)platform_remove_file(temp_copy_path);
        (void)platform_remove_file(temp_plain_path);
        return (result == 0) ? 0 : 1;
    }

    rt_write_line(2, "tar: choose exactly one of -c, -x, or -t");
    return 1;
}
