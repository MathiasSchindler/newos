#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define TAC_MAX_LINES 4096
#define TAC_LINE_CAPACITY 1024

static int store_line(
    char lines[TAC_MAX_LINES][TAC_LINE_CAPACITY],
    int terminated[TAC_MAX_LINES],
    size_t *count,
    const char *line,
    size_t len,
    int had_newline
) {
    size_t copy_len = len;

    if (*count >= TAC_MAX_LINES) {
        return -1;
    }

    if (copy_len >= TAC_LINE_CAPACITY) {
        copy_len = TAC_LINE_CAPACITY - 1U;
    }

    memcpy(lines[*count], line, copy_len);
    lines[*count][copy_len] = '\0';
    terminated[*count] = had_newline;
    *count += 1U;
    return 0;
}

static int print_usage_and_fail(const char *program_name) {
    tool_write_usage(program_name, "[file ...]");
    return 1;
}

static int tac_stream(int fd) {
    static char lines[TAC_MAX_LINES][TAC_LINE_CAPACITY];
    static int terminated[TAC_MAX_LINES];
    char chunk[4096];
    char current[TAC_LINE_CAPACITY];
    size_t current_len = 0;
    size_t count = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                if (store_line(lines, terminated, &count, current, current_len, 1) != 0) {
                    return -1;
                }
                current_len = 0;
            } else if (current_len + 1U < sizeof(current)) {
                current[current_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (current_len > 0U) {
        if (store_line(lines, terminated, &count, current, current_len, 0) != 0) {
            return -1;
        }
    }

    while (count > 0U) {
        count -= 1U;

        if (rt_write_cstr(1, lines[count]) != 0) {
            return -1;
        }

        if (count > 0U || terminated[count]) {
            if (rt_write_char(1, '\n') != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    int i;
    int exit_code = 0;

    if (argc == 1) {
        return tac_stream(0) == 0 ? 0 : 1;
    }

    for (i = 1; i < argc; ++i) {
        int fd;
        int should_close;

        if (argv[i][0] == '-' && argv[i][1] != '\0' && !(argv[i][1] == '-' && argv[i][2] == '\0')) {
            if (rt_strcmp(argv[i], "-") != 0) {
                return print_usage_and_fail(argv[0]);
            }
        }

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            tool_write_error("tac", "cannot open ", argv[i]);
            exit_code = 1;
            continue;
        }

        if (tac_stream(fd) != 0) {
            tool_write_error("tac", "read error on ", argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
