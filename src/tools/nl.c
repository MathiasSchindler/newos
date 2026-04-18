#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define NL_LINE_CAPACITY 4096

typedef enum {
    NL_STYLE_NONEMPTY = 0,
    NL_STYLE_ALL = 1,
    NL_STYLE_NONE = 2
} NlStyle;

typedef enum {
    NL_SECTION_HEADER = 0,
    NL_SECTION_BODY = 1,
    NL_SECTION_FOOTER = 2
} NlSection;

typedef enum {
    NL_FORMAT_RIGHT = 0,
    NL_FORMAT_RIGHT_ZERO = 1,
    NL_FORMAT_LEFT = 2
} NlFormat;

typedef struct {
    NlStyle header_style;
    NlStyle body_style;
    NlStyle footer_style;
    unsigned long long start;
    unsigned long long increment;
    unsigned long long width;
    NlFormat format;
    char delimiters[3];
    char separator[32];
    size_t separator_len;
} NlOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-ba|-bt|-bn] [-h STYLE] [-b STYLE] [-f STYLE] [-d CC] [-n FORMAT] [-v START] [-i INCREMENT] [-w WIDTH] [-s SEP] [file ...]");
}

static int write_padding(size_t count, char fill) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (rt_write_char(1, fill) != 0) {
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

static int parse_style_value(const char *text, NlStyle *style_out) {
    if (rt_strcmp(text, "a") == 0) {
        *style_out = NL_STYLE_ALL;
    } else if (rt_strcmp(text, "t") == 0) {
        *style_out = NL_STYLE_NONEMPTY;
    } else if (rt_strcmp(text, "n") == 0) {
        *style_out = NL_STYLE_NONE;
    } else {
        return -1;
    }
    return 0;
}

static NlStyle current_section_style(const NlOptions *options, NlSection section) {
    if (section == NL_SECTION_HEADER) {
        return options->header_style;
    }
    if (section == NL_SECTION_FOOTER) {
        return options->footer_style;
    }
    return options->body_style;
}

static int should_number_line(NlStyle style, size_t line_len) {
    return style == NL_STYLE_ALL || (style == NL_STYLE_NONEMPTY && line_len > 0U);
}

static int is_repeated_delimiter(const char *line, const char *delimiter, size_t repeat_count) {
    size_t i;
    size_t expected_len = repeat_count * 2U;

    if (rt_strlen(line) != expected_len) {
        return 0;
    }

    for (i = 0; i < expected_len; ++i) {
        if (line[i] != delimiter[i % 2U]) {
            return 0;
        }
    }

    return 1;
}

static int detect_section_marker(const char *line, const NlOptions *options, NlSection *section_out) {
    if (is_repeated_delimiter(line, options->delimiters, 3U)) {
        *section_out = NL_SECTION_HEADER;
        return 1;
    }

    if (is_repeated_delimiter(line, options->delimiters, 2U)) {
        *section_out = NL_SECTION_BODY;
        return 1;
    }

    if (is_repeated_delimiter(line, options->delimiters, 1U)) {
        *section_out = NL_SECTION_FOOTER;
        return 1;
    }

    return 0;
}

static int emit_line(const char *line, int should_number, unsigned long long *line_no, const NlOptions *options) {
    if (should_number) {
        size_t digits = count_digits(*line_no);
        size_t padding = 0;
        char fill = options->format == NL_FORMAT_RIGHT_ZERO ? '0' : ' ';

        if (digits < (size_t)options->width) {
            padding = (size_t)options->width - digits;
        }

        if (options->format == NL_FORMAT_LEFT) {
            if (rt_write_uint(1, *line_no) != 0 ||
                write_padding(padding, ' ') != 0 ||
                (options->separator_len > 0U && rt_write_all(1, options->separator, options->separator_len) != 0) ||
                rt_write_line(1, line) != 0) {
                return -1;
            }
        } else if (write_padding(padding, fill) != 0 ||
                   rt_write_uint(1, *line_no) != 0 ||
                   (options->separator_len > 0U && rt_write_all(1, options->separator, options->separator_len) != 0) ||
                   rt_write_line(1, line) != 0) {
            return -1;
        }

        *line_no += options->increment;
        return 0;
    }

    return write_padding((size_t)options->width, ' ') == 0 &&
           (options->separator_len == 0U || rt_write_all(1, options->separator, options->separator_len) == 0) &&
           rt_write_line(1, line) == 0
               ? 0
               : -1;
}

static int nl_stream(int fd, const NlOptions *options) {
    char chunk[4096];
    char line[NL_LINE_CAPACITY];
    size_t line_len = 0;
    unsigned long long line_no = options->start;
    NlSection section = NL_SECTION_BODY;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                int should_number;
                line[line_len] = '\0';

                if (detect_section_marker(line, options, &section)) {
                    if (section == NL_SECTION_HEADER) {
                        line_no = options->start;
                    }
                    line_len = 0;
                    continue;
                }

                should_number = should_number_line(current_section_style(options, section), line_len);
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

        if (detect_section_marker(line, options, &section)) {
            return 0;
        }

        should_number = should_number_line(current_section_style(options, section), line_len);
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

    options.header_style = NL_STYLE_NONE;
    options.body_style = NL_STYLE_NONEMPTY;
    options.footer_style = NL_STYLE_NONE;
    options.start = 1ULL;
    options.increment = 1ULL;
    options.width = 6ULL;
    options.format = NL_FORMAT_RIGHT;
    options.delimiters[0] = '\\';
    options.delimiters[1] = ':';
    options.delimiters[2] = '\0';
    rt_copy_string(options.separator, sizeof(options.separator), "\t");
    options.separator_len = 1U;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-ba") == 0) {
            options.body_style = NL_STYLE_ALL;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-bt") == 0) {
            options.body_style = NL_STYLE_NONEMPTY;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-bn") == 0) {
            options.body_style = NL_STYLE_NONE;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-ha") == 0) {
            options.header_style = NL_STYLE_ALL;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-ht") == 0) {
            options.header_style = NL_STYLE_NONEMPTY;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-hn") == 0) {
            options.header_style = NL_STYLE_NONE;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-fa") == 0) {
            options.footer_style = NL_STYLE_ALL;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-ft") == 0) {
            options.footer_style = NL_STYLE_NONEMPTY;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-fn") == 0) {
            options.footer_style = NL_STYLE_NONE;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-b") == 0) {
            if (argi + 1 >= argc || parse_style_value(argv[argi + 1], &options.body_style) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-h") == 0) {
            if (argi + 1 >= argc || parse_style_value(argv[argi + 1], &options.header_style) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-f") == 0) {
            if (argi + 1 >= argc || parse_style_value(argv[argi + 1], &options.footer_style) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-d") == 0) {
            size_t delimiter_len;

            if (argi + 1 >= argc ||
                tool_parse_escaped_string(argv[argi + 1], options.delimiters, sizeof(options.delimiters), &delimiter_len) != 0 ||
                delimiter_len == 0U || delimiter_len > 2U) {
                print_usage(argv[0]);
                return 1;
            }
            if (delimiter_len == 1U) {
                options.delimiters[1] = options.delimiters[0];
            }
            options.delimiters[2] = '\0';
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-n") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            if (rt_strcmp(argv[argi + 1], "ln") == 0) {
                options.format = NL_FORMAT_LEFT;
            } else if (rt_strcmp(argv[argi + 1], "rn") == 0) {
                options.format = NL_FORMAT_RIGHT;
            } else if (rt_strcmp(argv[argi + 1], "rz") == 0) {
                options.format = NL_FORMAT_RIGHT_ZERO;
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
            if (argi + 1 >= argc ||
                tool_parse_escaped_string(argv[argi + 1], options.separator, sizeof(options.separator), &options.separator_len) != 0) {
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
