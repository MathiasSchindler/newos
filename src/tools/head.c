#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef enum {
    HEAD_MODE_LINES,
    HEAD_MODE_BYTES
} HeadMode;

typedef enum {
    HEAD_COUNT_FIRST,
    HEAD_COUNT_FROM_START,
    HEAD_COUNT_EXCLUDE_LAST
} HeadCountStyle;

typedef struct {
    HeadMode mode;
    HeadCountStyle style;
    unsigned long long count;
    int quiet;
    int verbose;
} HeadOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-n [+|-]COUNT | -c [+|-]COUNT] [-qv] [file ...]");
}

static int parse_count_value(const char *program_name,
                             const char *flag_name,
                             const char *value_text,
                             HeadCountStyle *style_out,
                             unsigned long long *count_out) {
    HeadCountStyle style = HEAD_COUNT_FIRST;

    if (value_text == 0 || value_text[0] == '\0') {
        tool_write_error(program_name, "invalid ", flag_name);
        return -1;
    }

    if (value_text[0] == '+') {
        style = HEAD_COUNT_FROM_START;
        value_text += 1;
    } else if (value_text[0] == '-') {
        style = HEAD_COUNT_EXCLUDE_LAST;
        value_text += 1;
    }

    if (tool_parse_uint_arg(value_text, count_out, program_name, flag_name) != 0) {
        return -1;
    }

    *style_out = style;
    return 0;
}

static int is_digit_text(const char *text) {
    size_t i = 0;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }

    while (text[i] != '\0') {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        i += 1;
    }

    return 1;
}

static int parse_options(int argc, char **argv, HeadOptions *options, int *arg_index_out) {
    int arg_index = 1;

    options->mode = HEAD_MODE_LINES;
    options->style = HEAD_COUNT_FIRST;
    options->count = 10;
    options->quiet = 0;
    options->verbose = 0;

    while (arg_index < argc && argv[arg_index][0] == '-' && argv[arg_index][1] != '\0') {
        const char *arg = argv[arg_index];

        if (rt_strcmp(arg, "--") == 0) {
            arg_index += 1;
            break;
        }

        if (rt_strcmp(arg, "-q") == 0) {
            options->quiet = 1;
            options->verbose = 0;
            arg_index += 1;
            continue;
        }

        if (rt_strcmp(arg, "-v") == 0) {
            options->verbose = 1;
            options->quiet = 0;
            arg_index += 1;
            continue;
        }

        if (rt_strcmp(arg, "-n") == 0 || rt_strcmp(arg, "-c") == 0) {
            if (arg_index + 1 >= argc) {
                return -1;
            }
            if (parse_count_value("head", "count", argv[arg_index + 1], &options->style, &options->count) != 0) {
                return -1;
            }
            options->mode = (arg[1] == 'c') ? HEAD_MODE_BYTES : HEAD_MODE_LINES;
            arg_index += 2;
            continue;
        }

        if ((arg[1] == 'n' || arg[1] == 'c') && arg[2] != '\0') {
            if (parse_count_value("head", "count", arg + 2, &options->style, &options->count) != 0) {
                return -1;
            }
            options->mode = (arg[1] == 'c') ? HEAD_MODE_BYTES : HEAD_MODE_LINES;
            arg_index += 1;
            continue;
        }

        if (is_digit_text(arg + 1)) {
            options->mode = HEAD_MODE_LINES;
            options->style = HEAD_COUNT_FIRST;
            if (tool_parse_uint_arg(arg + 1, &options->count, "head", "count") != 0) {
                return -1;
            }
            arg_index += 1;
            continue;
        }

        return -1;
    }

    *arg_index_out = arg_index;
    return 0;
}

static int print_head_lines(int fd, unsigned long long limit) {
    char buffer[4096];
    long bytes_read;
    unsigned long long lines_seen = 0;

    if (limit == 0) {
        return 0;
    }

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (rt_write_char(1, buffer[i]) != 0) {
                return -1;
            }

            if (buffer[i] == '\n') {
                lines_seen += 1;
                if (lines_seen >= limit) {
                    return 0;
                }
            }
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int print_head_bytes(int fd, unsigned long long limit) {
    char buffer[4096];
    long bytes_read;
    unsigned long long bytes_written = 0;

    if (limit == 0) {
        return 0;
    }

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        size_t to_write = (size_t)bytes_read;

        if (bytes_written + (unsigned long long)to_write > limit) {
            to_write = (size_t)(limit - bytes_written);
        }

        if (to_write > 0 && rt_write_all(1, buffer, to_write) != 0) {
            return -1;
        }

        bytes_written += (unsigned long long)to_write;
        if (bytes_written >= limit) {
            return 0;
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int print_head_from_lines(int fd, unsigned long long start_line) {
    char buffer[4096];
    long bytes_read;
    unsigned long long current_line = 1ULL;

    if (start_line <= 1ULL) {
        start_line = 1ULL;
    }

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (current_line >= start_line && rt_write_char(1, buffer[i]) != 0) {
                return -1;
            }

            if (buffer[i] == '\n') {
                current_line += 1ULL;
            }
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int print_head_from_bytes(int fd, unsigned long long start_byte) {
    char buffer[4096];
    long bytes_read;
    unsigned long long current_byte = 1ULL;

    if (start_byte <= 1ULL) {
        start_byte = 1ULL;
    }

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (current_byte >= start_byte && rt_write_char(1, buffer[i]) != 0) {
                return -1;
            }
            current_byte += 1ULL;
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int flush_pending_prefix(char *pending,
                                size_t *pending_len,
                                size_t *newline_offsets,
                                size_t *newline_count,
                                size_t flush_len) {
    size_t i;
    size_t removed = 0;

    if (flush_len == 0U) {
        return 0;
    }

    if (rt_write_all(1, pending, flush_len) != 0) {
        return -1;
    }

    if (flush_len < *pending_len) {
        memmove(pending, pending + flush_len, *pending_len - flush_len);
    }
    *pending_len -= flush_len;

    while (removed < *newline_count && newline_offsets[removed] <= flush_len) {
        removed += 1U;
    }

    for (i = removed; i < *newline_count; ++i) {
        newline_offsets[i - removed] = newline_offsets[i] - flush_len;
    }
    *newline_count -= removed;
    return 0;
}

static int print_head_excluding_lines(int fd, unsigned long long exclude_count) {
    char chunk[4096];
    char pending[65536];
    size_t newline_offsets[4096];
    size_t pending_len = 0U;
    size_t newline_count = 0U;
    long bytes_read;

    if (exclude_count == 0ULL) {
        return print_head_from_lines(fd, 1ULL);
    }

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (pending_len >= sizeof(pending)) {
                if ((unsigned long long)newline_count > exclude_count &&
                    flush_pending_prefix(pending, &pending_len, newline_offsets, &newline_count, newline_offsets[0]) != 0) {
                    return -1;
                }
                if (pending_len >= sizeof(pending)) {
                    return -1;
                }
            }

            pending[pending_len++] = chunk[i];
            if (chunk[i] == '\n') {
                if (newline_count >= sizeof(newline_offsets) / sizeof(newline_offsets[0])) {
                    return -1;
                }
                newline_offsets[newline_count++] = pending_len;

                while ((unsigned long long)newline_count > exclude_count) {
                    if (flush_pending_prefix(pending, &pending_len, newline_offsets, &newline_count, newline_offsets[0]) != 0) {
                        return -1;
                    }
                }
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (pending_len > 0U) {
        int has_partial = (newline_count == 0U || newline_offsets[newline_count - 1U] < pending_len);
        unsigned long long total_lines = (unsigned long long)newline_count + (has_partial ? 1ULL : 0ULL);

        if (total_lines > exclude_count) {
            unsigned long long safe_lines = total_lines - exclude_count;
            size_t flush_len = pending_len;

            if (safe_lines <= (unsigned long long)newline_count) {
                flush_len = newline_offsets[(size_t)(safe_lines - 1ULL)];
            }

            if (rt_write_all(1, pending, flush_len) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int print_head_excluding_bytes(int fd, unsigned long long exclude_count) {
    char chunk[4096];
    char ring[65536];
    size_t ring_size;
    size_t head = 0U;
    size_t used = 0U;
    long bytes_read;

    if (exclude_count == 0ULL) {
        return print_head_from_bytes(fd, 1ULL);
    }

    ring_size = (exclude_count < (unsigned long long)sizeof(ring)) ? (size_t)exclude_count : sizeof(ring);
    if (ring_size == 0U) {
        return print_head_from_bytes(fd, 1ULL);
    }

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (used < ring_size) {
                ring[(head + used) % ring_size] = chunk[i];
                used += 1U;
            } else {
                if (rt_write_char(1, ring[head]) != 0) {
                    return -1;
                }
                ring[head] = chunk[i];
                head = (head + 1U) % ring_size;
            }
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int print_stream(int fd, const HeadOptions *options) {
    if (options->mode == HEAD_MODE_BYTES && options->style == HEAD_COUNT_FIRST) {
        return print_head_bytes(fd, options->count);
    }
    if (options->mode == HEAD_MODE_LINES && options->style == HEAD_COUNT_FIRST) {
        return print_head_lines(fd, options->count);
    }
    if (options->mode == HEAD_MODE_BYTES && options->style == HEAD_COUNT_FROM_START) {
        return print_head_from_bytes(fd, options->count);
    }
    if (options->mode == HEAD_MODE_LINES && options->style == HEAD_COUNT_FROM_START) {
        return print_head_from_lines(fd, options->count);
    }
    if (options->mode == HEAD_MODE_BYTES) {
        return print_head_excluding_bytes(fd, options->count);
    }
    return print_head_excluding_lines(fd, options->count);
}

static int should_print_header(const HeadOptions *options, int path_count) {
    if (options->verbose) {
        return 1;
    }
    if (options->quiet) {
        return 0;
    }
    return path_count > 1;
}

int main(int argc, char **argv) {
    HeadOptions options;
    int arg_index = 1;
    int path_count;
    int i;
    int exit_code = 0;
    int show_header;

    if (parse_options(argc, argv, &options, &arg_index) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    path_count = argc - arg_index;
    show_header = should_print_header(&options, path_count);

    if (path_count <= 0) {
        return print_stream(0, &options) == 0 ? 0 : 1;
    }

    for (i = arg_index; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            tool_write_error("head", "cannot open ", argv[i]);
            exit_code = 1;
            continue;
        }

        if (show_header) {
            if (i > arg_index) {
                rt_write_char(1, '\n');
            }
            rt_write_cstr(1, "==> ");
            rt_write_cstr(1, argv[i]);
            rt_write_line(1, " <==");
        }

        if (print_stream(fd, &options) != 0) {
            tool_write_error("head", "read error on ", argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
