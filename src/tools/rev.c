#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define REV_LINE_CAPACITY 4096

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[file ...]");
}

static int emit_reversed_line(const char *line, size_t len, int terminated) {
    while (len > 0U) {
        len -= 1U;
        if (rt_write_char(1, line[len]) != 0) {
            return -1;
        }
    }

    if (terminated) {
        return rt_write_char(1, '\n');
    }

    return 0;
}

static int rev_stream(int fd) {
    char chunk[4096];
    char line[REV_LINE_CAPACITY];
    size_t line_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                if (emit_reversed_line(line, line_len, 1) != 0) {
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
        return emit_reversed_line(line, line_len, 0);
    }

    return 0;
}

int main(int argc, char **argv) {
    int i;
    int exit_code = 0;

    if (argc == 1) {
        return rev_stream(0) == 0 ? 0 : 1;
    }

    for (i = 1; i < argc; ++i) {
        int fd;
        int should_close;

        if (argv[i][0] == '-' && argv[i][1] != '\0' && rt_strcmp(argv[i], "-") != 0) {
            print_usage(argv[0]);
            return 1;
        }

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            tool_write_error("rev", "cannot open ", argv[i]);
            exit_code = 1;
            continue;
        }

        if (rev_stream(fd) != 0) {
            tool_write_error("rev", "read error on ", argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
