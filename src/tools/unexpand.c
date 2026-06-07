#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define UNEXPAND_MAX_TABSTOPS 32
#define UNEXPAND_OUTPUT_BUFFER_SIZE 256U

typedef struct {
    unsigned long long stops[UNEXPAND_MAX_TABSTOPS];
    size_t stop_count;
    int convert_all;
    int zero_terminated;
} UnexpandOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-aiz] [-t TABSTOP[,TABSTOP...]] [file ...]");
}

static int parse_tabstop_list(const char *text, UnexpandOptions *options) {
    unsigned long long previous = 0ULL;
    size_t count = 0;
    size_t start = 0;
    size_t i = 0;

    while (1) {
        char ch = text[i];
        if (ch == ',' || ch == '\0') {
            char token[32];
            size_t length;
            unsigned long long value = 0ULL;

            if (count >= UNEXPAND_MAX_TABSTOPS || i == start) {
                return -1;
            }
            length = i - start;
            if (length + 1U > sizeof(token)) {
                return -1;
            }
            memcpy(token, text + start, length);
            token[length] = '\0';
            if (rt_parse_uint(token, &value) != 0 || value == 0ULL) {
                return -1;
            }
            if (count > 0U && value <= previous) {
                return -1;
            }

            options->stops[count++] = value;
            previous = value;

            if (ch == '\0') {
                break;
            }
            start = i + 1U;
        }
        i += 1;
    }

    options->stop_count = count;
    return count > 0U ? 0 : -1;
}

static unsigned long long next_tabstop(const UnexpandOptions *options, unsigned long long column) {
    size_t i;

    if (options->stop_count == 0U) {
        return column + 8ULL - (column % 8ULL);
    }

    if (options->stop_count == 1U) {
        unsigned long long stop = options->stops[0];
        return column + stop - (column % stop);
    }

    for (i = 0; i < options->stop_count; ++i) {
        if (options->stops[i] > column) {
            return options->stops[i];
        }
    }

    return column + 1ULL;
}

static int append_blank_char(char *buffer, size_t *used, char ch) {
    if (*used == UNEXPAND_OUTPUT_BUFFER_SIZE) {
        if (rt_write_all(1, buffer, *used) != 0) {
            return -1;
        }
        *used = 0U;
    }
    buffer[*used] = ch;
    *used += 1U;
    return 0;
}

static int write_blank_run(unsigned long long start_column,
                           unsigned long long count,
                           const UnexpandOptions *options) {
    char output[UNEXPAND_OUTPUT_BUFFER_SIZE];
    size_t used = 0U;
    unsigned long long column = start_column;

    while (count > 0ULL) {
        unsigned long long stop = next_tabstop(options, column);
        unsigned long long to_next_stop = stop > column ? stop - column : 1ULL;

        if (to_next_stop <= count && to_next_stop > 1ULL) {
            if (append_blank_char(output, &used, '\t') != 0) {
                return -1;
            }
            column += to_next_stop;
            count -= to_next_stop;
        } else {
            if (append_blank_char(output, &used, ' ') != 0) {
                return -1;
            }
            column += 1ULL;
            count -= 1ULL;
        }
    }

    return used == 0U ? 0 : rt_write_all(1, output, used);
}

static int unexpand_stream(int fd, const UnexpandOptions *options) {
    char buffer[4096 + 8];
    size_t carry = 0U;
    unsigned long long column = 0ULL;
    unsigned long long pending_spaces = 0ULL;
    int leading = 1;
    long bytes_read;

    while ((bytes_read = platform_read(fd, buffer + carry, sizeof(buffer) - carry)) > 0) {
        size_t total = carry + (size_t)bytes_read;
        size_t index = 0U;

        carry = 0U;
        while (index < total) {
            RtTextSegment segment;
            size_t before = index;

            if (rt_text_next_segment(buffer, total, index, &segment) != 0) {
                break;
            }
            if ((segment.flags & RT_TEXT_SEGMENT_INCOMPLETE) != 0U) {
                carry = total - index;
                memmove(buffer, buffer + index, carry);
                break;
            }

            if (segment.codepoint == ' ' && (options->convert_all || leading)) {
                pending_spaces += 1ULL;
                index = segment.end;
                continue;
            }

            if (pending_spaces > 0ULL) {
                if (write_blank_run(column, pending_spaces, options) != 0) {
                    return -1;
                }
                column += pending_spaces;
                pending_spaces = 0ULL;
            }

            if (segment.codepoint == '\n' || segment.codepoint == '\r' || (options->zero_terminated && segment.codepoint == 0U)) {
                if (rt_write_all(1, buffer + segment.start, segment.end - segment.start) != 0) {
                    return -1;
                }
                column = 0ULL;
                leading = 1;
            } else if (segment.codepoint == '\t') {
                if (rt_write_all(1, buffer + segment.start, segment.end - segment.start) != 0) {
                    return -1;
                }
                {
                    unsigned long long stop = next_tabstop(options, column);
                    column = stop > column ? stop : column + 1ULL;
                }
                if (!options->convert_all) {
                    leading = 1;
                }
            } else {
                if (rt_write_all(1, buffer + segment.start, segment.end - segment.start) != 0) {
                    return -1;
                }
                column = rt_text_apply_segment_width_tabstop(column, &segment, 8U);
                if ((segment.flags & RT_TEXT_SEGMENT_ANSI) == 0U && !rt_text_segment_is_space(buffer, total, &segment)) {
                    leading = 0;
                }
            }
            index = segment.end;
            if (index <= before) {
                index = before + 1U;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (pending_spaces > 0ULL) {
        if (write_blank_run(column, pending_spaces, options) != 0) {
            return -1;
        }
    }

    while (carry > 0U) {
        RtTextSegment segment;
        if (rt_text_next_segment(buffer, carry, 0U, &segment) != 0) {
            break;
        }
        segment.flags &= ~RT_TEXT_SEGMENT_INCOMPLETE;
        if (segment.codepoint == ' ' && (options->convert_all || leading)) {
            pending_spaces += 1ULL;
        } else {
            if (pending_spaces > 0ULL) {
                if (write_blank_run(column, pending_spaces, options) != 0) {
                    return -1;
                }
                column += pending_spaces;
                pending_spaces = 0ULL;
            }
            if (rt_write_all(1, buffer + segment.start, segment.end - segment.start) != 0) {
                return -1;
            }
            column = rt_text_apply_segment_width_tabstop(column, &segment, 8U);
            if ((segment.flags & RT_TEXT_SEGMENT_ANSI) == 0U && !rt_text_segment_is_space(buffer, carry, &segment)) {
                leading = 0;
            }
        }
        if (segment.end == 0U) {
            segment.end = 1U;
        }
        if (segment.end < carry) {
            memmove(buffer, buffer + segment.end, carry - segment.end);
        }
        carry -= segment.end;
    }

    if (pending_spaces > 0ULL) {
        if (write_blank_run(column, pending_spaces, options) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    UnexpandOptions options;
    int argi = 1;
    int exit_code = 0;

    options.stops[0] = 8ULL;
    options.stop_count = 1U;
    options.convert_all = 0;
    options.zero_terminated = 0;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-a") == 0) {
            options.convert_all = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-i") == 0) {
            options.convert_all = 0;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-z") == 0 || rt_strcmp(argv[argi], "--zero-terminated") == 0) {
            options.zero_terminated = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-t") == 0) {
            if (argi + 1 >= argc || parse_tabstop_list(argv[argi + 1], &options) != 0) {
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
