#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "archive_util.h"

#define AR_MAGIC "!<arch>\n"
#define AR_HEADER_SIZE 60U
#define AR_BUFFER_SIZE 4096U
#define AR_NAME_CAPACITY 256U
#define AR_STRING_TABLE_CAPACITY 65536U

typedef struct {
    char name[AR_NAME_CAPACITY];
    long long header_offset;
    long long payload_offset;
    long long data_offset;
    long long next_offset;
    unsigned long long payload_size;
    unsigned long long data_size;
    unsigned int mode;
    long long mtime;
    int is_string_table;
    int valid;
} ArMemberInfo;

static void ar_write_usage(void) {
    rt_write_line(2, "Usage: ar [rcstpxvq] archive [file ...]");
}

static int text_equals(const char *lhs, const char *rhs) {
    return rt_strcmp(lhs, rhs) == 0;
}

static int has_archive_magic(const char *buffer) {
    return buffer[0] == '!' &&
           buffer[1] == '<' &&
           buffer[2] == 'a' &&
           buffer[3] == 'r' &&
           buffer[4] == 'c' &&
           buffer[5] == 'h' &&
           buffer[6] == '>' &&
           buffer[7] == '\n';
}

static void build_temp_prefix(const char *target_path, const char *stem, char *buffer, size_t buffer_size) {
    size_t slash = 0U;
    size_t i = 0U;
    size_t prefix_length;

    while (target_path != 0 && target_path[i] != '\0') {
        if (target_path[i] == '/') {
            slash = i + 1U;
        }
        i += 1U;
    }

    if (slash == 0U) {
        rt_copy_string(buffer, buffer_size, "./");
        prefix_length = rt_strlen(buffer);
    } else {
        prefix_length = slash < (buffer_size - 1U) ? slash : (buffer_size - 1U);
        memcpy(buffer, target_path, prefix_length);
        buffer[prefix_length] = '\0';
    }

    rt_copy_string(buffer + prefix_length, buffer_size - prefix_length, stem);
}

static unsigned long long parse_decimal_field(const char *field, size_t field_size) {
    unsigned long long value = 0ULL;
    size_t i = 0U;

    while (i < field_size && (field[i] == ' ' || field[i] == '\0')) {
        i += 1U;
    }
    while (i < field_size && field[i] >= '0' && field[i] <= '9') {
        value = (value * 10ULL) + (unsigned long long)(field[i] - '0');
        i += 1U;
    }
    return value;
}

static void write_decimal_field(char *field, size_t field_size, unsigned long long value) {
    char digits[32];
    size_t digit_count = 0U;
    size_t i;

    for (i = 0U; i < field_size; ++i) {
        field[i] = ' ';
    }

    do {
        digits[digit_count++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    } while (value != 0ULL && digit_count < sizeof(digits));

    if (digit_count > field_size) {
        digit_count = field_size;
    }

    for (i = 0U; i < digit_count; ++i) {
        field[i] = digits[digit_count - 1U - i];
    }
}

static void write_octal_field(char *field, size_t field_size, unsigned int value) {
    char digits[32];
    size_t digit_count = 0U;
    size_t i;

    for (i = 0U; i < field_size; ++i) {
        field[i] = ' ';
    }

    do {
        digits[digit_count++] = (char)('0' + (value & 7U));
        value >>= 3U;
    } while (value != 0U && digit_count < sizeof(digits));

    if (digit_count > field_size) {
        digit_count = field_size;
    }

    for (i = 0U; i < digit_count; ++i) {
        field[i] = digits[digit_count - 1U - i];
    }
}

static void copy_trimmed_field(char *buffer, size_t buffer_size, const char *field, size_t field_size) {
    size_t start = 0U;
    size_t end = field_size;
    size_t length;

    while (start < end && field[start] == ' ') {
        start += 1U;
    }
    while (end > start && (field[end - 1U] == ' ' || field[end - 1U] == '/')) {
        end -= 1U;
    }

    length = end - start;
    if (length + 1U > buffer_size) {
        length = buffer_size - 1U;
    }

    if (length > 0U) {
        memcpy(buffer, field + start, length);
    }
    buffer[length] = '\0';
}

static void copy_name_from_string_table(char *buffer,
                                        size_t buffer_size,
                                        const char *string_table,
                                        size_t string_table_size,
                                        size_t offset) {
    size_t length = 0U;

    if (offset >= string_table_size || buffer_size == 0U) {
        if (buffer_size > 0U) {
            buffer[0] = '\0';
        }
        return;
    }

    while (offset + length < string_table_size &&
           string_table[offset + length] != '\n' &&
           string_table[offset + length] != '/' &&
           string_table[offset + length] != '\0') {
        if (length + 1U >= buffer_size) {
            break;
        }
        buffer[length] = string_table[offset + length];
        length += 1U;
    }
    buffer[length] = '\0';
}

static int copy_exact_bytes(int input_fd, int output_fd, unsigned long long count) {
    unsigned char buffer[AR_BUFFER_SIZE];

    while (count > 0ULL) {
        size_t chunk = count > sizeof(buffer) ? sizeof(buffer) : (size_t)count;
        long bytes_read = platform_read(input_fd, buffer, chunk);
        if (bytes_read <= 0) {
            return -1;
        }
        if (rt_write_all(output_fd, buffer, (size_t)bytes_read) != 0) {
            return -1;
        }
        count -= (unsigned long long)bytes_read;
    }

    return 0;
}

static int skip_bytes(int fd, unsigned long long count) {
    unsigned char buffer[AR_BUFFER_SIZE];

    while (count > 0ULL) {
        size_t chunk = count > sizeof(buffer) ? sizeof(buffer) : (size_t)count;
        long bytes_read = platform_read(fd, buffer, chunk);
        if (bytes_read <= 0) {
            return -1;
        }
        count -= (unsigned long long)bytes_read;
    }

    return 0;
}

static int member_selected(const char *name, int argc, char **argv, int start_index) {
    int i;

    if (start_index >= argc) {
        return 1;
    }

    for (i = start_index; i < argc; ++i) {
        if (text_equals(name, tool_base_name(argv[i])) || text_equals(name, argv[i])) {
            return 1;
        }
    }
    return 0;
}

static int read_member_info(int fd,
                            ArMemberInfo *info,
                            char *string_table,
                            size_t *string_table_size) {
    unsigned char header[AR_HEADER_SIZE];
    long long header_offset = platform_seek(fd, 0, PLATFORM_SEEK_CUR);
    char raw_name[17];
    unsigned long long payload_size;
    size_t i;

    rt_memset(info, 0, sizeof(*info));
    info->header_offset = header_offset;

    if (archive_read_exact(fd, header, sizeof(header)) != 0) {
        return 0;
    }

    if (header[58] != '`' || header[59] != '\n') {
        return -1;
    }

    for (i = 0U; i < 16U; ++i) {
        raw_name[i] = (char)header[i];
    }
    raw_name[16] = '\0';
    payload_size = parse_decimal_field((const char *)header + 48, 10U);

    info->payload_offset = platform_seek(fd, 0, PLATFORM_SEEK_CUR);
    info->payload_size = payload_size;
    info->data_offset = info->payload_offset;
    info->data_size = payload_size;
    info->next_offset = info->payload_offset + (long long)payload_size + ((payload_size & 1ULL) != 0ULL ? 1LL : 0LL);
    info->mtime = (long long)parse_decimal_field((const char *)header + 16, 12U);
    info->mode = (unsigned int)archive_parse_octal((const char *)header + 40, 8U);
    info->valid = 1;

    copy_trimmed_field(info->name, sizeof(info->name), raw_name, 16U);

    if (text_equals(info->name, "//")) {
        info->is_string_table = 1;
        if (payload_size < AR_STRING_TABLE_CAPACITY) {
            if (archive_read_exact(fd, (unsigned char *)string_table, (size_t)payload_size) != 0) {
                return -1;
            }
            string_table[payload_size] = '\0';
            *string_table_size = (size_t)payload_size;
        } else {
            if (skip_bytes(fd, payload_size) != 0) {
                return -1;
            }
            *string_table_size = 0U;
        }
    } else if (info->name[0] == '/' && info->name[1] >= '0' && info->name[1] <= '9') {
        size_t offset = 0U;
        for (i = 1U; info->name[i] >= '0' && info->name[i] <= '9'; ++i) {
            offset = (offset * 10U) + (size_t)(info->name[i] - '0');
        }
        copy_name_from_string_table(info->name, sizeof(info->name), string_table, *string_table_size, offset);
    } else if (info->name[0] == '#' && info->name[1] == '1' && info->name[2] == '/') {
        unsigned long long name_length = parse_decimal_field(info->name + 3, rt_strlen(info->name + 3));
        size_t to_read = (size_t)(name_length < (unsigned long long)(sizeof(info->name) - 1U) ? name_length : (unsigned long long)(sizeof(info->name) - 1U));
        if (to_read > 0U) {
            if (archive_read_exact(fd, (unsigned char *)info->name, to_read) != 0) {
                return -1;
            }
        }
        info->name[to_read] = '\0';
        if (name_length > (unsigned long long)to_read) {
            if (skip_bytes(fd, name_length - (unsigned long long)to_read) != 0) {
                return -1;
            }
        }
        info->data_offset = info->payload_offset + (long long)name_length;
        info->data_size = payload_size >= name_length ? payload_size - name_length : 0ULL;
    }

    {
        long long current = platform_seek(fd, 0, PLATFORM_SEEK_CUR);
        long long payload_end = info->payload_offset + (long long)payload_size;
        if (current < payload_end && skip_bytes(fd, (unsigned long long)(payload_end - current)) != 0) {
            return -1;
        }
        if ((payload_size & 1ULL) != 0ULL && skip_bytes(fd, 1ULL) != 0) {
            return -1;
        }
    }

    return 1;
}

static int stream_member_data(int archive_fd, const ArMemberInfo *info, int output_fd) {
    if (platform_seek(archive_fd, info->data_offset, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }
    return copy_exact_bytes(archive_fd, output_fd, info->data_size);
}

static int append_file_member(int output_fd, const char *path, int verbose) {
    PlatformDirEntry entry;
    const char *member_name = tool_base_name(path);
    int input_fd = -1;
    unsigned char header[AR_HEADER_SIZE];
    size_t name_length = rt_strlen(member_name);
    unsigned long long file_size;

    if (platform_get_path_info(path, &entry) != 0 || entry.is_dir) {
        rt_write_cstr(2, "ar: cannot add ");
        rt_write_line(2, path);
        return -1;
    }

    input_fd = platform_open_read(path);
    if (input_fd < 0) {
        rt_write_cstr(2, "ar: cannot open ");
        rt_write_line(2, path);
        return -1;
    }

    rt_memset(header, ' ', sizeof(header));
    file_size = entry.size;

    if (name_length <= 15U) {
        memcpy(header, member_name, name_length);
        header[name_length] = '/';
    } else {
        const char prefix[] = "#1/";
        memcpy(header, prefix, 3U);
        write_decimal_field((char *)header + 3, 13U, (unsigned long long)name_length);
        file_size += (unsigned long long)name_length;
    }

    write_decimal_field((char *)header + 16, 12U, entry.mtime < 0 ? 0ULL : (unsigned long long)entry.mtime);
    write_decimal_field((char *)header + 28, 6U, 0ULL);
    write_decimal_field((char *)header + 34, 6U, 0ULL);
    write_octal_field((char *)header + 40, 8U, entry.mode & 0777U);
    write_decimal_field((char *)header + 48, 10U, file_size);
    header[58] = '`';
    header[59] = '\n';

    if (rt_write_all(output_fd, header, sizeof(header)) != 0) {
        platform_close(input_fd);
        return -1;
    }

    if (name_length > 15U && rt_write_all(output_fd, member_name, name_length) != 0) {
        platform_close(input_fd);
        return -1;
    }

    if (copy_exact_bytes(input_fd, output_fd, entry.size) != 0) {
        platform_close(input_fd);
        return -1;
    }

    if ((file_size & 1ULL) != 0ULL) {
        rt_write_char(output_fd, '\n');
    }

    platform_close(input_fd);
    if (verbose) {
        rt_write_line(1, member_name);
    }
    return 0;
}

static int archive_contains_requested_name(const char *member_name, int argc, char **argv, int start_index) {
    return member_selected(member_name, argc, argv, start_index);
}

static int member_name_is_unsafe(const char *name) {
    size_t i = 0U;

    if (name == 0 || name[0] == '\0' || name[0] == '/') {
        return 1;
    }
    if (rt_strcmp(name, ".") == 0 || rt_strcmp(name, "..") == 0) {
        return 1;
    }

    while (name[i] != '\0') {
        size_t start = i;
        size_t length;

        while (name[i] == '/') {
            return 1;
        }
        while (name[i] != '\0' && name[i] != '/') {
            i += 1U;
        }

        length = i - start;
        if (length == 2U && name[start] == '.' && name[start + 1U] == '.') {
            return 1;
        }
        if (name[i] == '/') {
            return 1;
        }
    }

    return 0;
}

static int list_or_extract_archive(const char *archive_path, int mode, int verbose, int argc, char **argv, int member_index) {
    int fd = platform_open_read(archive_path);
    char magic[8];
    char string_table[AR_STRING_TABLE_CAPACITY + 1U];
    size_t string_table_size = 0U;
    int exit_code = 0;

    if (fd < 0) {
        rt_write_cstr(2, "ar: cannot open ");
        rt_write_line(2, archive_path);
        return 1;
    }

    if (archive_read_exact(fd, (unsigned char *)magic, 8U) != 0 || !has_archive_magic(magic)) {
        rt_write_cstr(2, "ar: invalid archive ");
        rt_write_line(2, archive_path);
        platform_close(fd);
        return 1;
    }

    for (;;) {
        ArMemberInfo info;
        int status = read_member_info(fd, &info, string_table, &string_table_size);
        if (status == 0) {
            break;
        }
        if (status < 0) {
            rt_write_cstr(2, "ar: malformed archive ");
            rt_write_line(2, archive_path);
            exit_code = 1;
            break;
        }
        if (info.is_string_table) {
            continue;
        }
        if (!archive_contains_requested_name(info.name, argc, argv, member_index)) {
            continue;
        }

        if (mode == 't') {
            rt_write_line(1, info.name);
        } else if (mode == 'p') {
            if (verbose) {
                rt_write_cstr(1, "<");
                rt_write_cstr(1, info.name);
                rt_write_line(1, ">");
            }
            if (stream_member_data(fd, &info, 1) != 0) {
                exit_code = 1;
                break;
            }
        } else if (mode == 'x') {
            if (member_name_is_unsafe(info.name)) {
                rt_write_cstr(2, "ar: refusing unsafe member path ");
                rt_write_line(2, info.name);
                exit_code = 1;
                continue;
            }
            int out_fd = platform_open_write(info.name, info.mode == 0U ? 0644U : info.mode);
            if (out_fd < 0) {
                rt_write_cstr(2, "ar: cannot extract ");
                rt_write_line(2, info.name);
                exit_code = 1;
                break;
            }
            if (stream_member_data(fd, &info, out_fd) != 0) {
                rt_write_cstr(2, "ar: extract failed for ");
                rt_write_line(2, info.name);
                platform_close(out_fd);
                exit_code = 1;
                break;
            }
            platform_close(out_fd);
            if (verbose) {
                rt_write_line(1, info.name);
            }
        }

        if (platform_seek(fd, info.next_offset, PLATFORM_SEEK_SET) < 0) {
            exit_code = 1;
            break;
        }
    }

    platform_close(fd);
    return exit_code;
}

static int create_or_replace_archive(const char *archive_path,
                                     int replace_mode,
                                     int verbose,
                                     int argc,
                                     char **argv,
                                     int file_index) {
    char temp_path[1024];
    char temp_prefix[1024];
    int temp_fd;
    int existing_fd = -1;
    int exit_code = 0;
    int i;

    build_temp_prefix(archive_path, ".newos-ar-", temp_prefix, sizeof(temp_prefix));
    temp_fd = platform_create_temp_file(temp_path, sizeof(temp_path), temp_prefix, 0600U);
    if (temp_fd < 0) {
        rt_write_line(2, "ar: could not create temporary archive");
        return 1;
    }

    if (rt_write_all(temp_fd, AR_MAGIC, 8U) != 0) {
        platform_close(temp_fd);
        (void)platform_remove_file(temp_path);
        return 1;
    }

    existing_fd = platform_open_read(archive_path);
    if (existing_fd >= 0) {
        char magic[8];
        char string_table[AR_STRING_TABLE_CAPACITY + 1U];
        size_t string_table_size = 0U;

        if (archive_read_exact(existing_fd, (unsigned char *)magic, 8U) != 0 || !has_archive_magic(magic)) {
            rt_write_cstr(2, "ar: invalid archive ");
            rt_write_line(2, archive_path);
            platform_close(existing_fd);
            platform_close(temp_fd);
            (void)platform_remove_file(temp_path);
            return 1;
        }

        for (;;) {
            ArMemberInfo info;
            int status = read_member_info(existing_fd, &info, string_table, &string_table_size);
            int copy_member = 1;

            if (status == 0) {
                break;
            }
            if (status < 0) {
                rt_write_cstr(2, "ar: malformed archive ");
                rt_write_line(2, archive_path);
                exit_code = 1;
                break;
            }

            if (!info.is_string_table && replace_mode && archive_contains_requested_name(info.name, argc, argv, file_index)) {
                copy_member = 0;
            }

            if (copy_member) {
                unsigned long long raw_size = AR_HEADER_SIZE + info.payload_size + ((info.payload_size & 1ULL) != 0ULL ? 1ULL : 0ULL);
                if (platform_seek(existing_fd, info.header_offset, PLATFORM_SEEK_SET) < 0 ||
                    copy_exact_bytes(existing_fd, temp_fd, raw_size) != 0) {
                    exit_code = 1;
                    break;
                }
            }
        }

        platform_close(existing_fd);
    }

    if (exit_code == 0) {
        for (i = file_index; i < argc; ++i) {
            if (append_file_member(temp_fd, argv[i], verbose) != 0) {
                exit_code = 1;
                break;
            }
        }
    }

    platform_close(temp_fd);

    if (exit_code == 0) {
        if (platform_rename_path(temp_path, archive_path) != 0) {
            rt_write_cstr(2, "ar: could not replace ");
            rt_write_line(2, archive_path);
            (void)platform_remove_file(temp_path);
            return 1;
        }
        return 0;
    }

    (void)platform_remove_file(temp_path);
    return 1;
}

int main(int argc, char **argv) {
    const char *mode_text;
    const char *archive_path;
    int verbose = 0;
    int replace_mode = 0;
    int mode = 0;
    size_t i = 0U;

    if (argc < 3) {
        ar_write_usage();
        return 1;
    }

    mode_text = argv[1];
    archive_path = argv[2];

    while (mode_text[i] != '\0') {
        char ch = mode_text[i++];
        if (ch == 'v') {
            verbose = 1;
        } else if (ch == 'r' || ch == 'q') {
            mode = ch;
            replace_mode = (ch == 'r');
        } else if (ch == 't' || ch == 'p' || ch == 'x') {
            mode = ch;
        } else if (ch == 'c' || ch == 's') {
            /* accepted for compatibility */
        } else {
            ar_write_usage();
            return 1;
        }
    }

    if (mode == 0) {
        ar_write_usage();
        return 1;
    }

    if ((mode == 'r' || mode == 'q') && argc < 4) {
        ar_write_usage();
        return 1;
    }

    if (mode == 'r' || mode == 'q') {
        return create_or_replace_archive(archive_path, replace_mode, verbose, argc, argv, 3);
    }

    return list_or_extract_archive(archive_path, mode, verbose, argc, argv, 3);
}
