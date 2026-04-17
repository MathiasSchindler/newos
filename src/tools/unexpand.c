#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    unsigned long long tabstop;
    int convert_all;
} UnexpandOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-a] [-t TABSTOP] [file ...]");
}

static int write_blank_run(unsigned long long start_column, unsigned long long count, unsigned long long tabstop) {
    unsigned long long column = start_column;

    while (count > 0ULL) {
        unsigned long long to_next_stop = tabstop - (column % tabstop);

        if (to_next_stop <= count && to_next_stop > 1ULL) {
            if (rt_write_char(1, '\t') != 0) {
                return -1;
            }
            column += to_next_stop;
            count -= to_next_stop;
        } else {
            if (rt_write_char(1, ' ') != 0) {
                return -1;
            }
            column += 1ULL;
            count -= 1ULL;
        }
    }

    return 0;
}

static int unexpand_stream(int fd, const UnexpandOptions *options) {
    char buffer[4096];
    unsigned long long column = 0ULL;
    unsigned long long pending_spaces = 0ULL;
    int leading = 1;
    long bytes_read;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];

            if (ch == ' ' && (options->convert_all || leading)) {
                pending_spaces += 1ULL;
                continue;
            }

            if (pending_spaces > 0ULL) {
                if (write_blank_run(column, pending_spaces, options->tabstop) != 0) {
                    return -1;
                }
                column += pending_spaces;
                pending_spaces = 0ULL;
            }

            if (ch == '\n' || ch == '\r') {
                if (rt_write_char(1, ch) != 0) {
                    return -1;
                }
                column = 0ULL;
                leading = 1;
            } else if (ch == '\t') {
                if (rt_write_char(1, ch) != 0) {
                    return -1;
                }
                column += options->tabstop - (column % options->tabstop);
                if (!options->convert_all) {
                    leading = 1;
                }
            } else {
                if (rt_write_char(1, ch) != 0) {
                    return -1;
                }
                column += 1ULL;
                leading = 0;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (pending_spaces > 0ULL) {
        if (write_blank_run(column, pending_spaces, options->tabstop) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    UnexpandOptions options;
    int argi = 1;
    int exit_code = 0;

    options.tabstop = 8ULL;
    options.convert_all = 0;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-a") == 0) {
            options.convert_all = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-t") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.tabstop, "unexpand", "tabstop") != 0 || options.tabstop == 0ULL) {
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
        return unexpand_stream(0, &options) == 0 ? 0 : 1;
    }

    for (; argi < argc; ++argi) {
        int fd;
        int should_close;

        if (tool_open_input(argv[argi], &fd, &should_close) != 0) {
            tool_write_error("unexpand", "cannot open ", argv[argi]);
            exit_code = 1;
            continue;
        }

        if (unexpand_stream(fd, &options) != 0) {
            tool_write_error("unexpand", "read error on ", argv[argi]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
