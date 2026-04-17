#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define CSPLIT_MAX_LINES 4096
#define CSPLIT_MAX_LINE_LENGTH 1024

typedef struct {
    char lines[CSPLIT_MAX_LINES][CSPLIT_MAX_LINE_LENGTH];
    size_t count;
} LineBuffer;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-f PREFIX] file PATTERN...");
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

static int make_output_name(const char *prefix, unsigned long long index, char *buffer, size_t buffer_size) {
    size_t prefix_len = rt_strlen(prefix);

    if (index >= 100ULL || prefix_len + 3U > buffer_size) {
        return -1;
    }

    memcpy(buffer, prefix, prefix_len);
    buffer[prefix_len] = (char)('0' + (char)((index / 10ULL) % 10ULL));
    buffer[prefix_len + 1U] = (char)('0' + (char)(index % 10ULL));
    buffer[prefix_len + 2U] = '\0';
    return 0;
}

static int write_segment(const LineBuffer *buffer, size_t start, size_t end, const char *prefix, unsigned long long index) {
    char path[256];
    int fd;
    unsigned long long bytes_written = 0ULL;
    size_t i;

    if (make_output_name(prefix, index, path, sizeof(path)) != 0) {
        tool_write_error("csplit", "too many output files for prefix ", prefix);
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
    rt_write_uint(1, bytes_written);
    rt_write_char(1, '\n');
    return 0;
}

int main(int argc, char **argv) {
    const char *prefix = "xx";
    const char *input_path;
    LineBuffer buffer;
    int argi = 1;
    int fd;
    int should_close = 0;
    size_t start = 0U;
    unsigned long long output_index = 0ULL;

    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (argi + 2 < argc && rt_strcmp(argv[argi], "-f") == 0) {
        prefix = argv[argi + 1];
        argi += 2;
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
        size_t split_index = 0U;

        if (find_split_index(&buffer, start, argv[argi], &split_index) != 0) {
            tool_write_error("csplit", "invalid or unmatched pattern ", argv[argi]);
            return 1;
        }

        if (write_segment(&buffer, start, split_index, prefix, output_index) != 0) {
            return 1;
        }

        output_index += 1ULL;
        start = split_index;
        argi += 1;
    }

    if (write_segment(&buffer, start, buffer.count, prefix, output_index) != 0) {
        return 1;
    }

    return 0;
}
