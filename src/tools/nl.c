#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define NL_LINE_CAPACITY 4096

typedef enum {
    NL_STYLE_NONEMPTY = 0,
    NL_STYLE_ALL = 1,
    NL_STYLE_NONE = 2
} NlStyle;

typedef struct {
    NlStyle style;
    unsigned long long start;
    unsigned long long increment;
    unsigned long long width;
    char separator[32];
} NlOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-ba|-bt|-bn] [-v START] [-i INCREMENT] [-w WIDTH] [-s SEP] [file ...]");
}

static int write_padding(size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (rt_write_char(1, ' ') != 0) {
            return -1;
        }
    }

    return 0;
}

static size_t count_digits(unsigned long long value) {
    size_t digits = 1U;

    while (value >= 10ULL) {
        value /= 10ULL;
        digits += 1U;
    }

    return digits;
}

static int emit_line(const char *line, int should_number, unsigned long long *line_no, const NlOptions *options) {
    if (should_number) {
        size_t digits = count_digits(*line_no);
        size_t padding = 0;

        if (digits < (size_t)options->width) {
            padding = (size_t)options->width - digits;
        }

        if (write_padding(padding) != 0 ||
            rt_write_uint(1, *line_no) != 0 ||
            rt_write_cstr(1, options->separator) != 0 ||
            rt_write_line(1, line) != 0) {
            return -1;
        }

        *line_no += options->increment;
        return 0;
    }

    return write_padding((size_t)options->width) == 0 &&
           rt_write_cstr(1, options->separator) == 0 &&
           rt_write_line(1, line) == 0
                ? 0
                : -1;
}

static int nl_stream(int fd, const NlOptions *options) {
    char chunk[4096];
    char line[NL_LINE_CAPACITY];
    size_t line_len = 0;
    unsigned long long line_no = options->start;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                int should_number;
                line[line_len] = '\0';
                should_number = options->style == NL_STYLE_ALL || (options->style == NL_STYLE_NONEMPTY && line_len > 0U);
                if (emit_line(line, should_number, &line_no, options) != 0) {
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
        int should_number;
        line[line_len] = '\0';
        should_number = options->style == NL_STYLE_ALL || (options->style == NL_STYLE_NONEMPTY && line_len > 0U);
        if (emit_line(line, should_number, &line_no, options) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    NlOptions options;
    int argi = 1;
    int exit_code = 0;

    options.style = NL_STYLE_NONEMPTY;
    options.start = 1ULL;
    options.increment = 1ULL;
    options.width = 6ULL;
    rt_copy_string(options.separator, sizeof(options.separator), "\t");

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-ba") == 0) {
            options.style = NL_STYLE_ALL;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-bt") == 0) {
            options.style = NL_STYLE_NONEMPTY;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-bn") == 0) {
            options.style = NL_STYLE_NONE;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-b") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            if (rt_strcmp(argv[argi + 1], "a") == 0) {
                options.style = NL_STYLE_ALL;
            } else if (rt_strcmp(argv[argi + 1], "t") == 0) {
                options.style = NL_STYLE_NONEMPTY;
            } else if (rt_strcmp(argv[argi + 1], "n") == 0) {
                options.style = NL_STYLE_NONE;
            } else {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-v") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.start, "nl", "start") != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-i") == 0) {
            if (argi + 1 >= argc ||
                tool_parse_uint_arg(argv[argi + 1], &options.increment, "nl", "increment") != 0 ||
                options.increment == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-w") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.width, "nl", "width") != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-s") == 0) {
            if (argi + 1 >= argc || argv[argi + 1][0] == '\0') {
                print_usage(argv[0]);
                return 1;
            }
            rt_copy_string(options.separator, sizeof(options.separator), argv[argi + 1]);
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
        return nl_stream(0, &options) == 0 ? 0 : 1;
    }

    for (; argi < argc; ++argi) {
        int fd;
        int should_close;

        if (tool_open_input(argv[argi], &fd, &should_close) != 0) {
            tool_write_error("nl", "cannot open ", argv[argi]);
            exit_code = 1;
            continue;
        }

        if (nl_stream(fd, &options) != 0) {
            tool_write_error("nl", "read error on ", argv[argi]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
