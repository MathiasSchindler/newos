#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define EXPAND_MAX_TABSTOPS 32

typedef struct {
    unsigned long long stops[EXPAND_MAX_TABSTOPS];
    size_t stop_count;
    int initial_only;
    int zero_terminated;
} ExpandOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-iz] [-t TABSTOP[,TABSTOP...]] [file ...]");
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

static int parse_tabstop_list(const char *text, ExpandOptions *options) {
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

            if (count >= EXPAND_MAX_TABSTOPS || i == start) {
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

static unsigned long long next_tabstop(const ExpandOptions *options, unsigned long long column) {
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

static int expand_stream(int fd, const ExpandOptions *options) {
    char buffer[4096 + 8];
    size_t carry = 0U;
    unsigned long long column = 0ULL;
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

            if (segment.codepoint == '\t' && (!options->initial_only || leading)) {
                unsigned long long stop = next_tabstop(options, column);
                unsigned long long spaces = stop > column ? stop - column : 1ULL;
                if (write_spaces(spaces) != 0) {
                    return -1;
                }
                column += spaces;
            } else {
                if (rt_write_all(1, buffer + segment.start, segment.end - segment.start) != 0) {
                    return -1;
                }

                if (segment.codepoint == '\n' || segment.codepoint == '\r' || (options->zero_terminated && segment.codepoint == 0U)) {
                    column = 0ULL;
                    leading = 1;
                } else if (segment.codepoint == '\t') {
                    unsigned long long stop = next_tabstop(options, column);
                    column = stop > column ? stop : column + 1ULL;
                } else if ((segment.flags & RT_TEXT_SEGMENT_ANSI) != 0U) {
                    column = rt_text_apply_segment_width_tabstop(column, &segment, 8U);
                } else {
                    column = rt_text_apply_segment_width_tabstop(column, &segment, 8U);
                    if (!rt_text_segment_is_space(buffer, total, &segment)) {
                        leading = 0;
                    }
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

    while (carry > 0U) {
        RtTextSegment segment;
        size_t before = 0U;

        if (rt_text_next_segment(buffer, carry, 0U, &segment) != 0) {
            break;
        }
        segment.flags &= ~RT_TEXT_SEGMENT_INCOMPLETE;
        if (segment.codepoint == '\t' && (!options->initial_only || leading)) {
            unsigned long long stop = next_tabstop(options, column);
            unsigned long long spaces = stop > column ? stop - column : 1ULL;
            if (write_spaces(spaces) != 0) {
                return -1;
            }
            column += spaces;
        } else {
            if (rt_write_all(1, buffer + segment.start, segment.end - segment.start) != 0) {
                return -1;
            }
            column = rt_text_apply_segment_width_tabstop(column, &segment, 8U);
            if (!rt_text_segment_is_space(buffer, carry, &segment)) {
                leading = 0;
            }
        }
        if (segment.end <= before) {
            segment.end = before + 1U;
        }
        if (segment.end < carry) {
            memmove(buffer, buffer + segment.end, carry - segment.end);
        }
        carry -= segment.end;
    }

    return 0;
}

int main(int argc, char **argv) {
    ExpandOptions options;
    int argi = 1;
    int exit_code = 0;

    options.stops[0] = 8ULL;
    options.stop_count = 1U;
    options.initial_only = 0;
    options.zero_terminated = 0;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-i") == 0) {
            options.initial_only = 1;
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
