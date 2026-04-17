#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PAGER_BUFFER_SIZE 4096
#define DEFAULT_PAGE_LINES 23

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-N] [file ...]");
}

static unsigned int pager_page_lines(void) {
    const char *text = platform_getenv("LINES");
    unsigned long long value = 0;

    if (text != 0 && rt_parse_uint(text, &value) == 0 && value > 1 && value < 1000) {
        return (unsigned int)(value - 1);
    }

    return DEFAULT_PAGE_LINES;
}

static int pager_prompt(unsigned int page_lines, unsigned int *lines_seen) {
    char input[16];
    long bytes_read;

    if (rt_write_cstr(1, "--More--") != 0) {
        return -1;
    }

    bytes_read = platform_read(0, input, sizeof(input));
    (void)rt_write_cstr(1, "\r        \r");

    if (bytes_read <= 0) {
        return 1;
    }

    if (input[0] == 'q' || input[0] == 'Q') {
        return 1;
    }

    if (input[0] == ' ') {
        *lines_seen = 0;
    } else {
        *lines_seen = page_lines > 0 ? (page_lines - 1) : 0;
    }

    return 0;
}

static int page_stream(int fd, int interactive, int show_numbers) {
    char buffer[PAGER_BUFFER_SIZE];
    long bytes_read;
    unsigned int page_lines = pager_page_lines();
    unsigned int lines_seen = 0;
    unsigned long long line_number = 1;
    int at_line_start = 1;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (show_numbers && at_line_start) {
                if (rt_write_uint(1, line_number) != 0 ||
                    rt_write_cstr(1, "\t") != 0) {
                    return -1;
                }
                at_line_start = 0;
            }

            if (rt_write_char(1, buffer[i]) != 0) {
                return -1;
            }

            if (buffer[i] == '\n') {
                at_line_start = 1;
                line_number += 1;

                if (interactive) {
                    lines_seen += 1;
                    if (lines_seen >= page_lines) {
                        int prompt_result = pager_prompt(page_lines, &lines_seen);
                        if (prompt_result != 0) {
                            return prompt_result > 0 ? 0 : -1;
                        }
                    }
                }
            }
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    int arg_index = 1;
    int show_numbers = 0;
    int path_count;
    int i;
    int exit_code = 0;

    if (argc > 1 && rt_strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (argc > 1 && rt_strcmp(argv[1], "-N") == 0) {
        show_numbers = 1;
        arg_index = 2;
    }

    path_count = argc - arg_index;
    if (path_count <= 0) {
        return page_stream(0, 0, show_numbers) == 0 ? 0 : 1;
    }

    for (i = arg_index; i < argc; ++i) {
        int fd;
        int should_close;
        int interactive;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            tool_write_error("less", "cannot open ", argv[i]);
            exit_code = 1;
            continue;
        }

        interactive = (fd != 0 && platform_isatty(0) != 0 && platform_isatty(1) != 0);

        if (path_count > 1) {
            if (i > arg_index) {
                rt_write_char(1, '\n');
            }
            rt_write_cstr(1, "==> ");
            rt_write_cstr(1, argv[i]);
            rt_write_line(1, " <==");
        }

        if (page_stream(fd, interactive, show_numbers) != 0) {
            tool_write_error("less", "read error on ", argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
