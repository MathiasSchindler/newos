#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-n COUNT] [file ...]");
}

static int print_head_stream(int fd, unsigned long long limit) {
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

int main(int argc, char **argv) {
    unsigned long long line_limit = 10;
    int arg_index = 1;
    int path_count;
    int i;
    int exit_code = 0;

    if (argc > 1 && rt_strcmp(argv[1], "-n") == 0) {
        if (argc < 3 || tool_parse_uint_arg(argv[2], &line_limit, "head", "count") != 0) {
            print_usage(argv[0]);
            return 1;
        }
        arg_index = 3;
    }

    path_count = argc - arg_index;
    if (path_count <= 0) {
        return print_head_stream(0, line_limit) == 0 ? 0 : 1;
    }

    for (i = arg_index; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            tool_write_error("head", "cannot open ", argv[i]);
            exit_code = 1;
            continue;
        }

        if (path_count > 1) {
            if (i > arg_index) {
                rt_write_char(1, '\n');
            }
            rt_write_cstr(1, "==> ");
            rt_write_cstr(1, argv[i]);
            rt_write_line(1, " <==");
        }

        if (print_head_stream(fd, line_limit) != 0) {
            tool_write_error("head", "read error on ", argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
