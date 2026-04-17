#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define CSPLIT_MAX_LINES 4096
#define CSPLIT_MAX_LINE_LENGTH 1024

typedef struct {
    char lines[CSPLIT_MAX_LINES][CSPLIT_MAX_LINE_LENGTH];
    size_t count;
} LineBuffer;

typedef struct {
    const char *prefix;
    int quiet;
    int keep_files;
    unsigned long long suffix_length;
} CsplitOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-f PREFIX] [-n DIGITS] [-s] [-k] file PATTERN...");
}

static int contains_text(const char *line, const char *pattern) {
    size_t i = 0;

    if (pattern[0] == '\0') {
        return 1;
    }

    while (line[i] != '\0') {
        size_t j = 0;

        while (line[i + j] != '\0' && pattern[j] != '\0' && line[i + j] == pattern[j]) {
            j += 1;
        }

        if (pattern[j] == '\0') {
            return 1;
        }

        i += 1;
    }

    return 0;
}

static int store_line(LineBuffer *buffer, const char *line, size_t len) {
    size_t copy_len = len;

    if (buffer->count >= CSPLIT_MAX_LINES) {
        return -1;
    }

    if (copy_len >= CSPLIT_MAX_LINE_LENGTH) {
        copy_len = CSPLIT_MAX_LINE_LENGTH - 1;
    }

    memcpy(buffer->lines[buffer->count], line, copy_len);
    buffer->lines[buffer->count][copy_len] = '\0';
    buffer->count += 1U;
    return 0;
}

static int collect_lines_from_fd(int fd, LineBuffer *buffer) {
    char chunk[2048];
    char current[CSPLIT_MAX_LINE_LENGTH];
    size_t current_len = 0;

    buffer->count = 0;

    for (;;) {
        long bytes_read = platform_read(fd, chunk, sizeof(chunk));
        long i;

        if (bytes_read < 0) {
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                if (store_line(buffer, current, current_len) != 0) {
                    return -1;
                }
                current_len = 0;
            } else if (current_len + 1U < sizeof(current)) {
                current[current_len++] = ch;
            }
        }
    }

    if (current_len > 0U || buffer->count == 0U) {
        if (store_line(buffer, current, current_len) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_line_number(const char *text, unsigned long long *value_out) {
    return rt_parse_uint(text, value_out) == 0 && *value_out > 0ULL ? 0 : -1;
}

static int find_split_index(const LineBuffer *buffer, size_t start, const char *pattern, size_t *index_out) {
    size_t len = rt_strlen(pattern);
    unsigned long long line_number = 0ULL;
    size_t i;

    if (len >= 2U && pattern[0] == '/' && pattern[len - 1U] == '/') {
        char needle[CSPLIT_MAX_LINE_LENGTH];
        size_t copy_len = len - 2U;

        if (copy_len >= sizeof(needle)) {
            copy_len = sizeof(needle) - 1U;
        }

        memcpy(needle, pattern + 1, copy_len);
        needle[copy_len] = '\0';

        for (i = start; i < buffer->count; ++i) {
            if (contains_text(buffer->lines[i], needle)) {
                *index_out = i;
                return 0;
            }
        }

        return -1;
    }

    if (parse_line_number(pattern, &line_number) != 0) {
        return -1;
    }

    if (line_number == 0ULL || line_number > (unsigned long long)buffer->count) {
        return -1;
    }

    *index_out = (size_t)(line_number - 1ULL);
    return *index_out >= start ? 0 : -1;
}

static int make_output_name(const char *prefix,
                            unsigned long long index,
                            unsigned long long suffix_length,
                            char *buffer,
                            size_t buffer_size) {
    size_t prefix_len = rt_strlen(prefix);
    size_t i;

    if (suffix_length == 0ULL || prefix_len + (size_t)suffix_length + 1U > buffer_size) {
        return -1;
    }

    memcpy(buffer, prefix, prefix_len);
    for (i = 0U; i < (size_t)suffix_length; ++i) {
        size_t pos = prefix_len + (size_t)suffix_length - 1U - i;
        buffer[pos] = (char)('0' + (char)(index % 10ULL));
        index /= 10ULL;
    }
    if (index != 0ULL) {
        return -1;
    }
    buffer[prefix_len + (size_t)suffix_length] = '\0';
    return 0;
}

static void cleanup_outputs(const CsplitOptions *options, unsigned long long created_count) {
    unsigned long long index;

    if (options->keep_files) {
        return;
    }

    for (index = 0ULL; index < created_count; ++index) {
        char path[256];
        if (make_output_name(options->prefix, index, options->suffix_length, path, sizeof(path)) == 0) {
            (void)platform_remove_file(path);
        }
    }
}

static int write_segment(const LineBuffer *buffer,
                         size_t start,
                         size_t end,
                         const CsplitOptions *options,
                         unsigned long long index) {
    char path[256];
    int fd;
    unsigned long long bytes_written = 0ULL;
    size_t i;

    if (make_output_name(options->prefix, index, options->suffix_length, path, sizeof(path)) != 0) {
        tool_write_error("csplit", "too many output files for prefix ", options->prefix);
        return -1;
    }

    fd = platform_open_write(path, 0644U);
    if (fd < 0) {
        tool_write_error("csplit", "cannot create ", path);
        return -1;
    }

    for (i = start; i < end; ++i) {
        size_t len = rt_strlen(buffer->lines[i]);

        if (rt_write_all(fd, buffer->lines[i], len) != 0 || rt_write_char(fd, '\n') != 0) {
            platform_close(fd);
            tool_write_error("csplit", "write error on ", path);
            return -1;
        }

        bytes_written += (unsigned long long)len + 1ULL;
    }

    platform_close(fd);
    if (!options->quiet) {
        rt_write_uint(1, bytes_written);
        rt_write_char(1, '\n');
    }
    return 0;
}

static int parse_pattern_argument(const char *arg, char *pattern_out, size_t pattern_out_size, unsigned long long *repeat_out) {
    size_t len;
    size_t i;

    if (arg == 0 || pattern_out == 0 || pattern_out_size == 0 || repeat_out == 0) {
        return -1;
    }

    *repeat_out = 0ULL;
    rt_copy_string(pattern_out, pattern_out_size, arg);
    len = rt_strlen(pattern_out);

    if (len >= 4U && pattern_out[len - 1U] == '}') {
        unsigned long long repeat = 0ULL;
        size_t brace = len - 2U;

        while (brace > 0U && pattern_out[brace] >= '0' && pattern_out[brace] <= '9') {
            brace -= 1U;
        }

        if (pattern_out[brace] == '{' && brace + 1U < len - 1U) {
            for (i = brace + 1U; i < len - 1U; ++i) {
                repeat = repeat * 10ULL + (unsigned long long)(pattern_out[i] - '0');
            }
            pattern_out[brace] = '\0';
            *repeat_out = repeat;
            return pattern_out[0] == '\0' ? -1 : 0;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    CsplitOptions options;
    const char *input_path;
    LineBuffer buffer;
    int argi = 1;
    int fd;
    int should_close = 0;
    size_t start = 0U;
    unsigned long long output_index = 0ULL;
    int exit_code = 1;

    options.prefix = "xx";
    options.quiet = 0;
    options.keep_files = 0;
    options.suffix_length = 2ULL;

    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-f") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            options.prefix = argv[argi + 1];
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-n") == 0) {
            if (argi + 1 >= argc ||
                tool_parse_uint_arg(argv[argi + 1], &options.suffix_length, "csplit", "digits") != 0 ||
                options.suffix_length == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-s") == 0) {
            options.quiet = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-k") == 0) {
            options.keep_files = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (argi >= argc - 1) {
        print_usage(argv[0]);
        return 1;
    }

    input_path = argv[argi++];
    if (tool_open_input(input_path, &fd, &should_close) != 0) {
        tool_write_error("csplit", "cannot open ", input_path);
        return 1;
    }

    if (collect_lines_from_fd(fd, &buffer) != 0) {
        tool_close_input(fd, should_close);
        tool_write_error("csplit", "failed to read ", input_path);
        return 1;
    }
    tool_close_input(fd, should_close);

    while (argi < argc) {
        char pattern[CSPLIT_MAX_LINE_LENGTH];
        unsigned long long repeat = 0ULL;
        unsigned long long pass;

        if (parse_pattern_argument(argv[argi], pattern, sizeof(pattern), &repeat) != 0) {
            cleanup_outputs(&options, output_index);
            tool_write_error("csplit", "invalid pattern ", argv[argi]);
            return 1;
        }

        for (pass = 0ULL; pass <= repeat; ++pass) {
            size_t split_index = 0U;
            size_t search_start = start;

            if (pass > 0ULL) {
                if (search_start >= buffer.count) {
                    cleanup_outputs(&options, output_index);
                    tool_write_error("csplit", "invalid or unmatched pattern ", argv[argi]);
                    return 1;
                }
                search_start += 1U;
            }

            if (find_split_index(&buffer, search_start, pattern, &split_index) != 0) {
                cleanup_outputs(&options, output_index);
                tool_write_error("csplit", "invalid or unmatched pattern ", argv[argi]);
                return 1;
            }

            if (write_segment(&buffer, start, split_index, &options, output_index) != 0) {
                cleanup_outputs(&options, output_index);
                return 1;
            }

            output_index += 1ULL;
            start = split_index;
        }
        argi += 1;
    }

    if (write_segment(&buffer, start, buffer.count, &options, output_index) != 0) {
        cleanup_outputs(&options, output_index);
        return 1;
    }

    exit_code = 0;
    return exit_code;
}
