#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define UMOUNT_USAGE "[-flv] TARGET..."
#define UMOUNT_FIELD_CAPACITY 1024

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
    char normalized_left[UMOUNT_FIELD_CAPACITY];
    char normalized_right[UMOUNT_FIELD_CAPACITY];

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

static int resolve_mount_target_from_table(const char *table_path,
                                           const char *input,
                                           char *buffer,
                                           size_t buffer_size) {
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
                char source[UMOUNT_FIELD_CAPACITY];
                char target[UMOUNT_FIELD_CAPACITY];
                size_t index = 0U;

                line[line_used] = '\0';
                if (next_mount_field(line, line_used, &index, source, sizeof(source)) == 0 &&
                    next_mount_field(line, line_used, &index, target, sizeof(target)) == 0 &&
                    (mount_value_matches(source, input) || mount_value_matches(target, input))) {
                    rt_copy_string(buffer, buffer_size, target);
                    platform_close(fd);
                    return 0;
                }
                line_used = 0U;
                continue;
            }
            if (line_used + 1U < sizeof(line)) {
                line[line_used++] = ch;
            }
        }
    }

    platform_close(fd);
    return -1;
}

static void resolve_mount_target(const char *input, char *buffer, size_t buffer_size) {
    rt_copy_string(buffer, buffer_size, input);
    if (resolve_mount_target_from_table("/proc/self/mounts", input, buffer, buffer_size) == 0) {
        return;
    }
    (void)resolve_mount_target_from_table("/proc/mounts", input, buffer, buffer_size);
}

static void print_help(const char *program_name) {
    rt_write_cstr(1, "Usage: ");
    rt_write_cstr(1, program_name);
    rt_write_cstr(1, " " UMOUNT_USAGE "\n");
    rt_write_line(1, "Unmount one or more targets. This is a small Linux-first umount implementation.");
    rt_write_line(1, "Each TARGET may be a mount point or the mounted source shown by mount.");
}

int main(int argc, char **argv) {
    ToolOptState options;
    int force = 0;
    int lazy = 0;
    int verbose = 0;
    int parse_result;
    int exit_status = 0;

    tool_opt_init(&options, argc, argv, tool_base_name(argv[0]), UMOUNT_USAGE);

    while ((parse_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-f") == 0 || rt_strcmp(options.flag, "--force") == 0) {
            force = 1;
        } else if (rt_strcmp(options.flag, "-l") == 0 || rt_strcmp(options.flag, "--lazy") == 0) {
            lazy = 1;
        } else if (rt_strcmp(options.flag, "-v") == 0 || rt_strcmp(options.flag, "--verbose") == 0) {
            verbose = 1;
        } else {
            tool_write_error("umount", "unknown option: ", options.flag);
            print_help(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        print_help(argv[0]);
        return 0;
    }
    if (options.argi >= argc) {
        print_help(argv[0]);
        return 1;
    }

    while (options.argi < argc) {
        const char *target = argv[options.argi++];
        char resolved_target[UMOUNT_FIELD_CAPACITY];

        resolve_mount_target(target, resolved_target, sizeof(resolved_target));
        if (platform_unmount_filesystem(resolved_target, force, lazy) != 0) {
            tool_write_error("umount", "unmount failed for ", target);
            exit_status = 1;
            continue;
        }
        if (verbose) {
            rt_write_cstr(1, "unmounted ");
            rt_write_line(1, resolved_target);
        }
    }

    return exit_status;
}
