#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    unsigned long long tabstop;
} ExpandOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-t TABSTOP] [file ...]");
}

static int write_spaces(unsigned long long count) {
    while (count > 0ULL) {
        if (rt_write_char(1, ' ') != 0) {
            return -1;
        }
        count -= 1ULL;
    }

    return 0;
}

static int expand_stream(int fd, const ExpandOptions *options) {
    char buffer[4096];
    unsigned long long column = 0ULL;
    long bytes_read;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];

            if (ch == '\t') {
                unsigned long long spaces = options->tabstop - (column % options->tabstop);
                if (write_spaces(spaces) != 0) {
                    return -1;
                }
                column += spaces;
            } else {
                if (rt_write_char(1, ch) != 0) {
                    return -1;
                }

                if (ch == '\n' || ch == '\r') {
                    column = 0ULL;
                } else if (ch == '\b') {
                    if (column > 0ULL) {
                        column -= 1ULL;
                    }
                } else {
                    column += 1ULL;
                }
            }
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    ExpandOptions options;
    int argi = 1;
    int exit_code = 0;

    options.tabstop = 8ULL;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-t") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.tabstop, "expand", "tabstop") != 0 || options.tabstop == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else if (rt_strcmp(argv[argi], "-") == 0) {
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (argi == argc) {
        return expand_stream(0, &options) == 0 ? 0 : 1;
    }

    for (; argi < argc; ++argi) {
        int fd;
        int should_close;

        if (tool_open_input(argv[argi], &fd, &should_close) != 0) {
            tool_write_error("expand", "cannot open ", argv[argi]);
            exit_code = 1;
            continue;
        }

        if (expand_stream(fd, &options) != 0) {
            tool_write_error("expand", "read error on ", argv[argi]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
