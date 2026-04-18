#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define REV_LINE_CAPACITY 4096

typedef struct {
    int zero_terminated;
} RevOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-0] [file ...]");
}

static int is_utf8_continuation(unsigned char byte) {
    return (byte & 0xC0U) == 0x80U;
}

static size_t utf8_sequence_length(unsigned char lead) {
    if ((lead & 0x80U) == 0U) {
        return 1U;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2U;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3U;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4U;
    }
    return 1U;
}

static int emit_reversed_record(const char *line, size_t len, int terminated, const RevOptions *options) {
    size_t end = len;
    char separator = options->zero_terminated ? '\0' : '\n';

    while (end > 0U) {
        size_t start = end - 1U;
        size_t expected_len;

        while (start > 0U && is_utf8_continuation((unsigned char)line[start])) {
            start -= 1U;
        }

        expected_len = utf8_sequence_length((unsigned char)line[start]);
        if (start + expected_len != end) {
            start = end - 1U;
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
    char chunk[4096];
    char line[REV_LINE_CAPACITY];
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
