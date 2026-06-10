#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define REV_RECORD_CAPACITY 65536U
#define REV_IO_BUFFER_SIZE 8192U

typedef struct {
    int zero_terminated;
} RevOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-0] [file ...]");
}

#define is_utf8_continuation tool_utf8_is_continuation_byte

static int decode_previous_codepoint(const char *text, size_t end, size_t *start_out, unsigned int *codepoint_out) {
    size_t start;
    size_t index;
    unsigned int codepoint = 0U;

    if (end == 0U) {
        return -1;
    }

    start = end - 1U;
    while (start > 0U && is_utf8_continuation((unsigned char)text[start])) {
        start -= 1U;
    }

    index = start;
    if (rt_utf8_decode(text, end, &index, &codepoint) != 0 || index != end) {
        start = end - 1U;
        codepoint = (unsigned char)text[start];
    }

    *start_out = start;
    *codepoint_out = codepoint;
    return 0;
}

static int is_combining_codepoint(unsigned int codepoint) {
    return (codepoint >= 0x0300U && codepoint <= 0x036fU) ||
           (codepoint >= 0x1ab0U && codepoint <= 0x1affU) ||
           (codepoint >= 0x1dc0U && codepoint <= 0x1dffU) ||
           (codepoint >= 0x20d0U && codepoint <= 0x20ffU) ||
           (codepoint >= 0xfe20U && codepoint <= 0xfe2fU);
}

static int is_variation_or_modifier(unsigned int codepoint) {
    return (codepoint >= 0xfe00U && codepoint <= 0xfe0fU) ||
           (codepoint >= 0xe0100U && codepoint <= 0xe01efU) ||
           (codepoint >= 0x1f3fbU && codepoint <= 0x1f3ffU);
}

static int is_grapheme_extension(unsigned int codepoint) {
    return is_combining_codepoint(codepoint) || is_variation_or_modifier(codepoint);
}

static int parse_escape_sequence(const char *text, size_t len, size_t start, size_t *end_out) {
    size_t index;

    if ((unsigned char)text[start] != 0x1bU || start + 1U >= len) {
        return -1;
    }

    index = start + 1U;
    if (text[index] == '[') {
        index += 1U;
        while (index < len) {
            unsigned char ch = (unsigned char)text[index++];
            if (ch >= 0x40U && ch <= 0x7eU) {
                *end_out = index;
                return 0;
            }
        }
        return -1;
    }

    if (text[index] == ']') {
        index += 1U;
        while (index < len) {
            if ((unsigned char)text[index] == 0x07U) {
                *end_out = index + 1U;
                return 0;
            }
            if ((unsigned char)text[index] == 0x1bU && index + 1U < len && text[index + 1U] == '\\') {
                *end_out = index + 2U;
                return 0;
            }
            index += 1U;
        }
        return -1;
    }

    *end_out = start + 2U;
    return 0;
}

static int find_escape_ending_at(const char *text, size_t end, size_t *start_out) {
    size_t start = end;

    while (start > 0U) {
        size_t parsed_end;

        start -= 1U;
        if ((unsigned char)text[start] != 0x1bU) {
            continue;
        }
        if (parse_escape_sequence(text, end, start, &parsed_end) == 0 && parsed_end == end) {
            *start_out = start;
            return 0;
        }
        break;
    }

    return -1;
}

static size_t grapheme_cluster_start(const char *text, size_t end) {
    size_t start = end;
    unsigned int codepoint = 0U;

    if (decode_previous_codepoint(text, end, &start, &codepoint) != 0) {
        return end - 1U;
    }

    while (start > 0U) {
        size_t previous_start;
        unsigned int previous_codepoint;

        if (decode_previous_codepoint(text, start, &previous_start, &previous_codepoint) != 0) {
            break;
        }

        if (is_grapheme_extension(codepoint)) {
            start = previous_start;
            codepoint = previous_codepoint;
            continue;
        }

        if (previous_codepoint == 0x200dU) {
            start = previous_start;
            if (start > 0U && decode_previous_codepoint(text, start, &previous_start, &previous_codepoint) == 0) {
                start = previous_start;
                codepoint = previous_codepoint;
            }
            continue;
        }

        break;
    }

    return start;
}

static int emit_reversed_record(const char *line, size_t len, int terminated, const RevOptions *options) {
    size_t end = len;
    char separator = options->zero_terminated ? '\0' : '\n';

    while (end > 0U) {
        size_t start;

        if (find_escape_ending_at(line, end, &start) != 0) {
            start = grapheme_cluster_start(line, end);
        }

        if (rt_write_all(1, line + start, end - start) != 0) {
            return -1;
        }

        end = start;
    }

    if (terminated) {
        return rt_write_char(1, separator);
    }

    return 0;
}

static int rev_stream(int fd, const RevOptions *options) {
    char chunk[REV_IO_BUFFER_SIZE];
    char line[REV_RECORD_CAPACITY];
    size_t line_len = 0;
    char separator = options->zero_terminated ? '\0' : '\n';
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == separator) {
                if (emit_reversed_record(line, line_len, 1, options) != 0) {
                    return -1;
                }
                line_len = 0;
            } else if (line_len + 1U < sizeof(line)) {
                line[line_len++] = ch;
            } else {
                return -1;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (line_len > 0U) {
        return emit_reversed_record(line, line_len, 0, options);
    }

    return 0;
}

int main(int argc, char **argv) {
    RevOptions options;
    int i = 1;
    int exit_code = 0;

    options.zero_terminated = 0;

    while (i < argc && argv[i][0] == '-') {
        if (rt_strcmp(argv[i], "-0") == 0) {
            options.zero_terminated = 1;
            i += 1;
        } else if (rt_strcmp(argv[i], "--") == 0) {
            i += 1;
            break;
        } else if (rt_strcmp(argv[i], "-") == 0) {
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (i == argc) {
        return rev_stream(0, &options) == 0 ? 0 : 1;
    }

    for (; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            tool_write_error("rev", "cannot open ", argv[i]);
            exit_code = 1;
            continue;
        }

        if (rev_stream(fd, &options) != 0) {
            tool_write_error("rev", "read error on ", argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
