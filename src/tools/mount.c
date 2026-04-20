#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MOUNT_USAGE "[-rvwBp] [-t TYPE] [-o OPTIONS] [SOURCE [TARGET]]"
#define MOUNT_FIELD_CAPACITY 1024

static int token_equals(const char *text, size_t text_length, const char *token) {
    size_t i = 0U;

    while (i < text_length && token[i] != '\0') {
        if (text[i] != token[i]) {
            return 0;
        }
        i += 1U;
    }
    return i == text_length && token[i] == '\0';
}

static int starts_with(const char *text, const char *prefix) {
    size_t i = 0U;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }
    return 1;
}

static int mount_is_octal_digit(char ch) {
    return ch >= '0' && ch <= '7';
}

static void mount_copy_trimmed(char *buffer, size_t buffer_size, const char *text) {
    size_t length = rt_strlen(text);

    while (length > 1U && text[length - 1U] == '/') {
        length -= 1U;
    }
    if (length + 1U > buffer_size) {
        length = buffer_size - 1U;
    }
    memcpy(buffer, text, length);
    buffer[length] = '\0';
}

static int mount_value_matches(const char *left, const char *right) {
    char normalized_left[MOUNT_FIELD_CAPACITY];
    char normalized_right[MOUNT_FIELD_CAPACITY];

    mount_copy_trimmed(normalized_left, sizeof(normalized_left), left);
    mount_copy_trimmed(normalized_right, sizeof(normalized_right), right);
    return rt_strcmp(normalized_left, normalized_right) == 0;
}

static int decode_mount_field(const char *text, size_t text_length, char *buffer, size_t buffer_size) {
    size_t source_index = 0U;
    size_t target_index = 0U;

    if (buffer_size == 0U) {
        return -1;
    }

    while (source_index < text_length) {
        char ch = text[source_index];

        if (target_index + 1U >= buffer_size) {
            return -1;
        }
        if (ch == '\\' &&
            source_index + 3U < text_length &&
            mount_is_octal_digit(text[source_index + 1U]) &&
            mount_is_octal_digit(text[source_index + 2U]) &&
            mount_is_octal_digit(text[source_index + 3U])) {
            buffer[target_index++] = (char)(((text[source_index + 1U] - '0') << 6) |
                                            ((text[source_index + 2U] - '0') << 3) |
                                            (text[source_index + 3U] - '0'));
            source_index += 4U;
            continue;
        }
        buffer[target_index++] = ch;
        source_index += 1U;
    }
    buffer[target_index] = '\0';
    return 0;
}

static int next_mount_field(const char *line,
                            size_t line_length,
                            size_t *index_io,
                            char *buffer,
                            size_t buffer_size) {
    size_t start;

    while (*index_io < line_length && (line[*index_io] == ' ' || line[*index_io] == '\t')) {
        *index_io += 1U;
    }
    start = *index_io;
    while (*index_io < line_length && line[*index_io] != ' ' && line[*index_io] != '\t') {
        *index_io += 1U;
    }
    if (start == *index_io) {
        return -1;
    }
    return decode_mount_field(line + start, *index_io - start, buffer, buffer_size);
}

static int mount_matches_filter(const char *source, const char *target, const char *filesystem_type, const char *filter) {
    if (filter == 0 || filter[0] == '\0') {
        return 1;
    }
    return mount_value_matches(source, filter) ||
           mount_value_matches(target, filter) ||
           rt_strcmp(filesystem_type, filter) == 0;
}

static void write_mount_entry(const char *source, const char *target, const char *filesystem_type, const char *options) {
    rt_write_cstr(1, source);
    rt_write_cstr(1, " on ");
    rt_write_cstr(1, target);
    rt_write_cstr(1, " type ");
    rt_write_cstr(1, filesystem_type);
    rt_write_cstr(1, " (");
    rt_write_cstr(1, options);
    rt_write_cstr(1, ")\n");
}

static void process_mount_line(const char *line, size_t line_length, const char *filter, int *matched_io) {
    char source[MOUNT_FIELD_CAPACITY];
    char target[MOUNT_FIELD_CAPACITY];
    char filesystem_type[MOUNT_FIELD_CAPACITY];
    char options[MOUNT_FIELD_CAPACITY];
    size_t index = 0U;

    if (next_mount_field(line, line_length, &index, source, sizeof(source)) != 0 ||
        next_mount_field(line, line_length, &index, target, sizeof(target)) != 0 ||
        next_mount_field(line, line_length, &index, filesystem_type, sizeof(filesystem_type)) != 0 ||
        next_mount_field(line, line_length, &index, options, sizeof(options)) != 0) {
        return;
    }
    if (!mount_matches_filter(source, target, filesystem_type, filter)) {
        return;
    }
    *matched_io = 1;
    write_mount_entry(source, target, filesystem_type, options);
}

static int stream_mount_table(const char *table_path, const char *filter, int *matched_io) {
    int fd = platform_open_read(table_path);
    char chunk[512];
    char line[2048];
    size_t line_used = 0U;

    if (fd < 0) {
        return -1;
    }

    for (;;) {
        long bytes_read = platform_read(fd, chunk, sizeof(chunk));
        long i;

        if (bytes_read < 0) {
            platform_close(fd);
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                line[line_used] = '\0';
                process_mount_line(line, line_used, filter, matched_io);
                line_used = 0U;
                continue;
            }
            if (line_used + 1U < sizeof(line)) {
                line[line_used++] = ch;
            }
        }
    }

    if (line_used > 0U) {
        line[line_used] = '\0';
        process_mount_line(line, line_used, filter, matched_io);
    }
    platform_close(fd);
    return 0;
}

static int ensure_directory_path(const char *path) {
    char buffer[MOUNT_FIELD_CAPACITY];
    size_t i;

    if (path == 0 || path[0] == '\0') {
        return -1;
    }

    rt_copy_string(buffer, sizeof(buffer), path);
    for (i = 1U; buffer[i] != '\0'; ++i) {
        int is_directory = 0;

        if (buffer[i] != '/') {
            continue;
        }
        buffer[i] = '\0';
        if (platform_make_directory(buffer, 0755U) != 0 &&
            (platform_path_is_directory(buffer, &is_directory) != 0 || !is_directory)) {
            return -1;
        }
        buffer[i] = '/';
    }

    {
        int is_directory = 0;
        if (platform_make_directory(buffer, 0755U) != 0 &&
            (platform_path_is_directory(buffer, &is_directory) != 0 || !is_directory)) {
            return -1;
        }
    }
    return 0;
}

static int append_data_token(char *buffer, size_t buffer_size, size_t *used, const char *token, size_t token_length) {
    if (buffer == 0 || used == 0) {
        return -1;
    }
    if (*used != 0U) {
        if (*used + 1U >= buffer_size) {
            return -1;
        }
        buffer[*used] = ',';
        *used += 1U;
    }
    if (*used + token_length + 1U > buffer_size) {
        return -1;
    }
    memcpy(buffer + *used, token, token_length);
    *used += token_length;
    buffer[*used] = '\0';
    return 0;
}

static int parse_option_token(
    const char *token,
    size_t token_length,
    unsigned long long *flags_io,
    char *data_buffer,
    size_t data_buffer_size,
    size_t *data_used_io
) {
    if (token_length == 0U || flags_io == 0 || data_used_io == 0) {
        return 0;
    }

    if (token_equals(token, token_length, "ro")) {
        *flags_io |= PLATFORM_MOUNT_RDONLY;
    } else if (token_equals(token, token_length, "rw")) {
        *flags_io &= ~PLATFORM_MOUNT_RDONLY;
    } else if (token_equals(token, token_length, "nosuid")) {
        *flags_io |= PLATFORM_MOUNT_NOSUID;
    } else if (token_equals(token, token_length, "nodev")) {
        *flags_io |= PLATFORM_MOUNT_NODEV;
    } else if (token_equals(token, token_length, "noexec")) {
        *flags_io |= PLATFORM_MOUNT_NOEXEC;
    } else if (token_equals(token, token_length, "sync")) {
        *flags_io |= PLATFORM_MOUNT_SYNC;
    } else if (token_equals(token, token_length, "remount")) {
        *flags_io |= PLATFORM_MOUNT_REMOUNT;
    } else if (token_equals(token, token_length, "mand")) {
        *flags_io |= PLATFORM_MOUNT_MANDLOCK;
    } else if (token_equals(token, token_length, "dirsync")) {
        *flags_io |= PLATFORM_MOUNT_DIRSYNC;
    } else if (token_equals(token, token_length, "noatime")) {
        *flags_io |= PLATFORM_MOUNT_NOATIME;
    } else if (token_equals(token, token_length, "nodiratime")) {
        *flags_io |= PLATFORM_MOUNT_NODIRATIME;
    } else if (token_equals(token, token_length, "bind")) {
        *flags_io |= PLATFORM_MOUNT_BIND;
    } else if (token_equals(token, token_length, "rbind") || token_equals(token, token_length, "bind,rec")) {
        *flags_io |= PLATFORM_MOUNT_BIND | PLATFORM_MOUNT_REC;
    } else if (token_equals(token, token_length, "rec")) {
        *flags_io |= PLATFORM_MOUNT_REC;
    } else if (token_equals(token, token_length, "silent")) {
        *flags_io |= PLATFORM_MOUNT_SILENT;
    } else if (token_equals(token, token_length, "relatime")) {
        *flags_io |= PLATFORM_MOUNT_RELATIME;
    } else if (token_equals(token, token_length, "strictatime")) {
        *flags_io |= PLATFORM_MOUNT_STRICTATIME;
    } else if (token_equals(token, token_length, "lazytime")) {
        *flags_io |= PLATFORM_MOUNT_LAZYTIME;
    } else {
        if (append_data_token(data_buffer, data_buffer_size, data_used_io, token, token_length) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_mount_options(
    const char *text,
    unsigned long long *flags_io,
    char *data_buffer,
    size_t data_buffer_size
) {
    size_t index = 0U;
    size_t data_used = rt_strlen(data_buffer);

    if (text == 0 || flags_io == 0 || data_buffer == 0 || data_buffer_size == 0U) {
        return -1;
    }

    while (text[index] != '\0') {
        size_t start;
        size_t length;

        while (text[index] == ' ' || text[index] == '\t' || text[index] == ',') {
            index += 1U;
        }
        start = index;
        while (text[index] != '\0' && text[index] != ',') {
            index += 1U;
        }
        length = index - start;
        while (length > 0U && (text[start + length - 1U] == ' ' || text[start + length - 1U] == '\t')) {
            length -= 1U;
        }

        if (parse_option_token(text + start, length, flags_io, data_buffer, data_buffer_size, &data_used) != 0) {
            return -1;
        }
    }

    return 0;
}

static int list_mounts(const char *filter) {
    int matched = 0;

    if (stream_mount_table("/proc/self/mounts", filter, &matched) == 0 ||
        stream_mount_table("/proc/mounts", filter, &matched) == 0) {
        if (filter != 0 && filter[0] != '\0' && !matched) {
            tool_write_error("mount", "no matching mount for ", filter);
            return 1;
        }
        return 0;
    }

    tool_write_error("mount", "mount listing is not available on this platform", 0);
    return 1;
}

static void print_help(const char *program_name) {
    rt_write_cstr(1, "Usage: ");
    rt_write_cstr(1, program_name);
    rt_write_cstr(1, " " MOUNT_USAGE "\n");
    rt_write_line(1, "Without SOURCE and TARGET, mount lists the current kernel mount table when available.");
    rt_write_line(1, "With a single path-like argument, mount prints the matching mount entry.");
    rt_write_line(1, "Use -p or --mkdir to create the target directory tree before mounting.");
}

int main(int argc, char **argv) {
    ToolOptState options;
    const char *filesystem_type = 0;
    const char *source = 0;
    const char *target = 0;
    char mount_data[512];
    unsigned long long flags = 0ULL;
    int create_target_dir = 0;
    int verbose = 0;
    int parse_result;

    mount_data[0] = '\0';
    tool_opt_init(&options, argc, argv, tool_base_name(argv[0]), MOUNT_USAGE);

    while ((parse_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-t") == 0 || rt_strcmp(options.flag, "--types") == 0) {
            if (tool_opt_require_value(&options) != 0) {
                return 1;
            }
            filesystem_type = options.value;
        } else if (rt_strcmp(options.flag, "-o") == 0 || rt_strcmp(options.flag, "--options") == 0) {
            if (tool_opt_require_value(&options) != 0 ||
                parse_mount_options(options.value, &flags, mount_data, sizeof(mount_data)) != 0) {
                tool_write_error("mount", "invalid option list", 0);
                return 1;
            }
        } else if (starts_with(options.flag, "--options=")) {
            if (parse_mount_options(options.flag + 10, &flags, mount_data, sizeof(mount_data)) != 0) {
                tool_write_error("mount", "invalid option list", 0);
                return 1;
            }
        } else if (rt_strcmp(options.flag, "-r") == 0 || rt_strcmp(options.flag, "--read-only") == 0) {
            flags |= PLATFORM_MOUNT_RDONLY;
        } else if (rt_strcmp(options.flag, "-w") == 0 || rt_strcmp(options.flag, "--read-write") == 0) {
            flags &= ~PLATFORM_MOUNT_RDONLY;
        } else if (rt_strcmp(options.flag, "-B") == 0 || rt_strcmp(options.flag, "--bind") == 0) {
            flags |= PLATFORM_MOUNT_BIND;
        } else if (rt_strcmp(options.flag, "-p") == 0 || rt_strcmp(options.flag, "--mkdir") == 0) {
            create_target_dir = 1;
        } else if (rt_strcmp(options.flag, "-v") == 0 || rt_strcmp(options.flag, "--verbose") == 0) {
            verbose = 1;
        } else {
            tool_write_error("mount", "unknown option: ", options.flag);
            print_help(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        print_help(argv[0]);
        return 0;
    }

    if (options.argi >= argc) {
        return list_mounts(0);
    }
    if (argc - options.argi == 1) {
        target = argv[options.argi];
        if (filesystem_type == 0 && flags == 0ULL && mount_data[0] == '\0' && !create_target_dir) {
            return list_mounts(target);
        }
        if ((flags & PLATFORM_MOUNT_BIND) != 0ULL) {
            tool_write_error("mount", "bind mounts require SOURCE and TARGET", 0);
            return 1;
        }
        if ((flags & PLATFORM_MOUNT_REMOUNT) != 0ULL) {
            source = 0;
        } else if (filesystem_type != 0 && filesystem_type[0] != '\0') {
            source = filesystem_type;
        } else {
            tool_write_error("mount", "single-target mounts require -t TYPE or remount options", 0);
            print_help(argv[0]);
            return 1;
        }
    } else if (argc - options.argi == 2) {
        source = argv[options.argi];
        target = argv[options.argi + 1];
    } else {
        print_help(argv[0]);
        return 1;
    }

    if (create_target_dir && ensure_directory_path(target) != 0) {
        tool_write_error("mount", "failed to create mount target ", target);
        return 1;
    }

    if (platform_mount_filesystem(source, target, filesystem_type, flags,
                                  mount_data[0] != '\0' ? mount_data : 0) != 0) {
        tool_write_error("mount", "mount failed for ", target);
        return 1;
    }

    if (verbose) {
        rt_write_cstr(1, "mounted ");
        rt_write_cstr(1, (source != 0 && source[0] != '\0') ? source : "none");
        rt_write_cstr(1, " on ");
        rt_write_line(1, target);
    }
    return 0;
}
