#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define CUT_MAX_RANGES 32
#define CUT_LINE_CAPACITY 8192

typedef struct {
    unsigned long long start;
    unsigned long long end;
    int open_end;
} CutRange;

typedef struct {
    CutRange ranges[CUT_MAX_RANGES];
    size_t count;
} CutRangeList;

typedef enum {
    CUT_MODE_NONE,
    CUT_MODE_CHARS,
    CUT_MODE_BYTES,
    CUT_MODE_FIELDS
} CutMode;

typedef struct {
    CutMode mode;
    CutRangeList selections;
    char delimiter;
    int complement;
} CutOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [--complement] (-b LIST | -c LIST | -f LIST [-d DELIM]) [file ...]");
}

static int parse_single_range(const char *text, CutRange *range) {
    const char *dash = 0;
    size_t i = 0;
    char left[32];
    char right[32];
    size_t left_len = 0;
    size_t right_len = 0;

    rt_memset(range, 0, sizeof(*range));

    while (text[i] != '\0') {
        if (text[i] == '-') {
            dash = text + i;
            break;
        }
        i += 1;
    }

    if (dash == 0) {
        if (rt_parse_uint(text, &range->start) != 0 || range->start == 0) {
            return -1;
        }
        range->end = range->start;
        return 0;
    }

    i = 0;
    while (text[i] != '\0' && text[i] != '-' && left_len + 1 < sizeof(left)) {
        left[left_len++] = text[i++];
    }
    left[left_len] = '\0';

    if (text[i] == '-') {
        i += 1;
    }

    while (text[i] != '\0' && right_len + 1 < sizeof(right)) {
        right[right_len++] = text[i++];
    }
    right[right_len] = '\0';

    if (left_len == 0) {
        range->start = 1;
    } else if (rt_parse_uint(left, &range->start) != 0 || range->start == 0) {
        return -1;
    }

    if (right_len == 0) {
        range->open_end = 1;
        range->end = 0;
    } else if (rt_parse_uint(right, &range->end) != 0 || range->end < range->start) {
        return -1;
    }

    return 0;
}

static int parse_range_list(const char *text, CutRangeList *list) {
    char token[64];
    size_t token_len = 0;
    size_t i = 0;

    rt_memset(list, 0, sizeof(*list));

    while (1) {
        char ch = text[i];

        if (ch == ',' || ch == '\0') {
            if (token_len == 0 || list->count >= CUT_MAX_RANGES) {
                return -1;
            }
            token[token_len] = '\0';
            if (parse_single_range(token, &list->ranges[list->count]) != 0) {
                return -1;
            }
            list->count += 1;
            token_len = 0;

            if (ch == '\0') {
                break;
            }
        } else if (token_len + 1 < sizeof(token)) {
            token[token_len++] = ch;
        } else {
            return -1;
        }

        i += 1;
    }

    return list->count > 0 ? 0 : -1;
}

static int range_list_contains(unsigned long long position, const CutRangeList *list) {
    size_t i;

    for (i = 0; i < list->count; ++i) {
        const CutRange *range = &list->ranges[i];

        if (position < range->start) {
            continue;
        }
        if (!range->open_end && position > range->end) {
            continue;
        }
        return 1;
    }

    return 0;
}

static size_t cut_decode_codepoint(const char *text, size_t length, size_t start, unsigned int *codepoint_out) {
    size_t index = start;
    unsigned int local_codepoint = 0U;
    unsigned int *target = codepoint_out != 0 ? codepoint_out : &local_codepoint;

    if (start >= length) {
        if (codepoint_out != 0) {
            *codepoint_out = 0U;
        }
        return 0U;
    }

    if (rt_utf8_decode(text, length, &index, target) != 0 || index <= start) {
        if (codepoint_out != 0) {
            *codepoint_out = (unsigned char)text[start];
        }
        return 1U;
    }

    return index - start;
}

static int cut_byte_stream(int fd, const CutRangeList *list) {
    char buffer[4096];
    long bytes_read;
    unsigned long long position = 1;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];

            if (ch == '\n') {
                if (rt_write_char(1, '\n') != 0) {
                    return -1;
                }
                position = 1;
                continue;
            }

            if (range_list_contains(position, list) && rt_write_char(1, ch) != 0) {
                return -1;
            }

            position += 1;
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int selection_includes(unsigned long long position, const CutOptions *options) {
    int included = range_list_contains(position, &options->selections);
    return options->complement ? !included : included;
}

static int cut_position_matches(unsigned long long position,
                                const CutRangeList *list,
                                int complement) {
    int included = range_list_contains(position, list);
    return complement ? !included : included;
}

static int cut_text_stream(int fd, const CutRangeList *list, int complement) {
    char buffer[4096];
    long bytes_read;
    unsigned long long position = 1;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];

            if (ch == '\n') {
                if (rt_write_char(1, '\n') != 0) {
                    return -1;
                }
                position = 1;
                continue;
            }

            if (cut_position_matches(position, list, complement) && rt_write_char(1, ch) != 0) {
                return -1;
            }

            position += 1;
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int write_selected_codepoints(const char *line, const CutRangeList *list, int complement, int end_with_newline) {
    size_t length = rt_strlen(line);
    size_t index = 0U;
    unsigned long long position = 1ULL;

    while (index < length) {
        size_t advance = cut_decode_codepoint(line, length, index, 0);

        if (cut_position_matches(position, list, complement) && rt_write_all(1, line + index, advance) != 0) {
            return -1;
        }

        index += advance;
        position += 1ULL;
    }

    if (end_with_newline && rt_write_char(1, '\n') != 0) {
        return -1;
    }

    return 0;
}

static int cut_codepoint_stream(int fd, const CutRangeList *list, int complement) {
    char chunk[4096];
    char line[CUT_LINE_CAPACITY];
    size_t line_len = 0U;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                line[line_len] = '\0';
                if (write_selected_codepoints(line, list, complement, 1) != 0) {
                    return -1;
                }
                line_len = 0U;
            } else if (line_len + 1U < sizeof(line)) {
                line[line_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (line_len > 0U) {
        line[line_len] = '\0';
        if (write_selected_codepoints(line, list, complement, 0) != 0) {
            return -1;
        }
    }

    return 0;
}

static int write_selected_fields(const char *line, const CutOptions *options, int end_with_newline) {
    unsigned long long field_no = 1;
    size_t field_start = 0;
    size_t i = 0;
    int wrote_field = 0;

    while (1) {
        char ch = line[i];

        if (ch == options->delimiter || ch == '\0') {
            if (selection_includes(field_no, options)) {
                if (wrote_field && rt_write_char(1, options->delimiter) != 0) {
                    return -1;
                }
                if (i > field_start && rt_write_all(1, line + field_start, i - field_start) != 0) {
                    return -1;
                }
                wrote_field = 1;
            }

            if (ch == '\0') {
                break;
            }

            field_no += 1;
            field_start = i + 1;
        }

        i += 1;
    }

    if (end_with_newline && rt_write_char(1, '\n') != 0) {
        return -1;
    }
    return 0;
}

static int cut_field_stream(int fd, const CutOptions *options) {
    char chunk[4096];
    char line[CUT_LINE_CAPACITY];
    size_t line_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                line[line_len] = '\0';
                if (write_selected_fields(line, options, 1) != 0) {
                    return -1;
                }
                line_len = 0;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        if (write_selected_fields(line, options, 0) != 0) {
            return -1;
        }
    }

    return 0;
}

static int cut_stream(int fd, const CutOptions *options) {
    if (options->mode == CUT_MODE_FIELDS) {
        return cut_field_stream(fd, options);
    }
    if (options->mode == CUT_MODE_BYTES) {
        if (!options->complement) {
            return cut_byte_stream(fd, &options->selections);
        }
        return cut_text_stream(fd, &options->selections, options->complement);
    }
    return cut_codepoint_stream(fd, &options->selections, options->complement);
}

static int parse_options(int argc, char **argv, CutOptions *options, int *arg_index_out) {
    int arg_index = 1;

    rt_memset(options, 0, sizeof(*options));
    options->delimiter = '\t';

    while (arg_index < argc && argv[arg_index][0] == '-' && argv[arg_index][1] != '\0') {
        if (rt_strcmp(argv[arg_index], "--") == 0) {
            arg_index += 1;
            break;
        }

        if (rt_strcmp(argv[arg_index], "--complement") == 0) {
            options->complement = 1;
            arg_index += 1;
            continue;
        }

        if ((rt_strcmp(argv[arg_index], "-b") == 0 ||
             rt_strcmp(argv[arg_index], "-c") == 0 ||
             rt_strcmp(argv[arg_index], "-f") == 0) &&
            arg_index + 1 < argc) {
            if (parse_range_list(argv[arg_index + 1], &options->selections) != 0) {
                return -1;
            }
            if (argv[arg_index][1] == 'f') {
                options->mode = CUT_MODE_FIELDS;
            } else if (argv[arg_index][1] == 'b') {
                options->mode = CUT_MODE_BYTES;
            } else {
                options->mode = CUT_MODE_CHARS;
            }
            arg_index += 2;
            continue;
        }

        if (rt_strcmp(argv[arg_index], "-d") == 0 && arg_index + 1 < argc) {
            if (argv[arg_index + 1][0] == '\0') {
                return -1;
            }
            options->delimiter = argv[arg_index + 1][0];
            arg_index += 2;
            continue;
        }

        return -1;
    }

    if (options->mode == CUT_MODE_NONE) {
        return -1;
    }

    *arg_index_out = arg_index;
    return 0;
}

int main(int argc, char **argv) {
    CutOptions options;
    int arg_index = 1;
    int i;
    int exit_code = 0;

    if (parse_options(argc, argv, &options, &arg_index) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (arg_index >= argc) {
        return cut_stream(0, &options) == 0 ? 0 : 1;
    }

    for (i = arg_index; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "cut: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (cut_stream(fd, &options) != 0) {
            rt_write_cstr(2, "cut: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
