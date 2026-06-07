#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#include <limits.h>

#define CUT_IO_BUFFER_SIZE 8192U

typedef struct {
    unsigned long long start;
    unsigned long long end;
    int open_end;
} CutRange;

typedef enum {
    CUT_MODE_NONE,
    CUT_MODE_CHARS,
    CUT_MODE_BYTES,
    CUT_MODE_FIELDS
} CutMode;

typedef struct {
    CutMode mode;
    const char *selections;
    char delimiter;
    char line_delimiter;
    int complement;
} CutOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [--complement] [-z] (-b LIST | -c LIST | -f LIST [-d DELIM]) [file ...]");
}

static int parse_uint_span(const char *text, size_t length, unsigned long long *value_out) {
    size_t i = 0;
    unsigned long long value = 0ULL;

    if (length == 0U) {
        return -1;
    }

    while (i < length) {
        unsigned long long digit;

        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }
        digit = (unsigned long long)(text[i] - '0');
        if (value > (ULLONG_MAX - digit) / 10ULL) {
            return -1;
        }
        value = value * 10ULL + digit;
        i += 1U;
    }

    *value_out = value;
    return 0;
}

static int parse_range_token(const char *text, size_t length, CutRange *range) {
    size_t i = 0;
    size_t dash_index = length;

    rt_memset(range, 0, sizeof(*range));

    while (i < length) {
        if (text[i] == '-') {
            dash_index = i;
            break;
        }
        i += 1U;
    }

    if (dash_index == length) {
        if (parse_uint_span(text, length, &range->start) != 0 || range->start == 0ULL) {
            return -1;
        }
        range->end = range->start;
        return 0;
    }

    if (dash_index == 0U) {
        range->start = 1ULL;
    } else if (parse_uint_span(text, dash_index, &range->start) != 0 || range->start == 0ULL) {
        return -1;
    }

    if (dash_index + 1U == length) {
        range->open_end = 1;
        range->end = 0ULL;
    } else if (parse_uint_span(text + dash_index + 1U, length - dash_index - 1U, &range->end) != 0 || range->end < range->start) {
        return -1;
    }

    return 0;
}

static int validate_range_list(const char *text) {
    size_t i = 0;
    size_t token_start = 0;
    size_t count = 0;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    while (1) {
        char ch = text[i];

        if (ch == ',' || ch == '\0') {
            CutRange range;
            size_t token_len = i - token_start;

            if (token_len == 0U || parse_range_token(text + token_start, token_len, &range) != 0) {
                return -1;
            }
            count += 1U;

            if (ch == '\0') {
                break;
            }
            token_start = i + 1U;
        }

        i += 1U;
    }

    return count > 0U ? 0 : -1;
}

static int range_list_contains(unsigned long long position, const char *text) {
    size_t i = 0;
    size_t token_start = 0;

    while (1) {
        char ch = text[i];

        if (ch == ',' || ch == '\0') {
            CutRange range;

            if (parse_range_token(text + token_start, i - token_start, &range) == 0 &&
                position >= range.start && (range.open_end || position <= range.end)) {
                return 1;
            }
            if (ch == '\0') {
                break;
            }
            token_start = i + 1U;
        }

        i += 1U;
    }

    return 0;
}

static int cut_position_matches(unsigned long long position,
                                const char *list,
                                int complement) {
    int included = range_list_contains(position, list);
    return complement ? !included : included;
}

static int cut_byte_stream(int fd, const CutOptions *options) {
    char buffer[CUT_IO_BUFFER_SIZE];
    char out[CUT_IO_BUFFER_SIZE];
    size_t out_len = 0U;
    long bytes_read;
    unsigned long long position = 1;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];

            if (ch == options->line_delimiter) {
                if (out_len + 1U > sizeof(out)) {
                    if (rt_write_all(1, out, out_len) != 0) {
                        return -1;
                    }
                    out_len = 0U;
                }
                out[out_len++] = options->line_delimiter;
                if (out_len == sizeof(out) && rt_write_all(1, out, out_len) != 0) {
                    return -1;
                }
                if (out_len == sizeof(out)) out_len = 0U;
                position = 1;
                continue;
            }

            if (cut_position_matches(position, options->selections, options->complement)) {
                if (out_len + 1U > sizeof(out)) {
                    if (rt_write_all(1, out, out_len) != 0) {
                        return -1;
                    }
                    out_len = 0U;
                }
                out[out_len++] = ch;
                if (out_len == sizeof(out) && rt_write_all(1, out, out_len) != 0) {
                    return -1;
                }
                if (out_len == sizeof(out)) out_len = 0U;
            }

            position += 1;
        }
    }

    if (bytes_read < 0) {
        return -1;
    }
    return out_len > 0U ? rt_write_all(1, out, out_len) : 0;
}

static int selection_includes(unsigned long long position, const CutOptions *options) {
    int included = range_list_contains(position, options->selections);
    return options->complement ? !included : included;
}

static size_t utf8_expected_length(unsigned char ch) {
    if (ch < 0x80U) {
        return 1U;
    }
    if (ch >= 0xc2U && ch <= 0xdfU) {
        return 2U;
    }
    if (ch >= 0xe0U && ch <= 0xefU) {
        return 3U;
    }
    if (ch >= 0xf0U && ch <= 0xf4U) {
        return 4U;
    }
    return 1U;
}

static void remove_pending_prefix(char *pending, size_t *pending_len, size_t count) {
    size_t i;

    for (i = count; i < *pending_len; ++i) {
        pending[i - count] = pending[i];
    }
    *pending_len -= count;
}

static int consume_pending_codepoints(char *pending,
                                      size_t *pending_len,
                                      unsigned long long *position,
                                      const CutOptions *options,
                                      int force) {
    while (*pending_len > 0U) {
        size_t expected = utf8_expected_length((unsigned char)pending[0]);
        size_t emit_len = 1U;

        if (*pending_len < expected && !force) {
            return 0;
        }
        if (*pending_len >= expected) {
            size_t index = 0U;
            unsigned int codepoint;

            if (expected == 1U || (rt_utf8_decode(pending, expected, &index, &codepoint) == 0 && index == expected)) {
                emit_len = expected;
            }
        }

        if (cut_position_matches(*position, options->selections, options->complement) && rt_write_all(1, pending, emit_len) != 0) {
            return -1;
        }
        remove_pending_prefix(pending, pending_len, emit_len);
        *position += 1ULL;
    }

    return 0;
}

static int cut_codepoint_stream(int fd, const CutOptions *options) {
    char chunk[CUT_IO_BUFFER_SIZE];
    char pending[4];
    size_t pending_len = 0U;
    unsigned long long position = 1ULL;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == options->line_delimiter) {
                if (consume_pending_codepoints(pending, &pending_len, &position, options, 1) != 0 ||
                    rt_write_char(1, options->line_delimiter) != 0) {
                    return -1;
                }
                pending_len = 0U;
                position = 1ULL;
            } else {
                if (pending_len >= sizeof(pending) && consume_pending_codepoints(pending, &pending_len, &position, options, 1) != 0) {
                    return -1;
                }
                pending[pending_len++] = ch;
                if (consume_pending_codepoints(pending, &pending_len, &position, options, 0) != 0) {
                    return -1;
                }
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    return consume_pending_codepoints(pending, &pending_len, &position, options, 1);
}

static int field_start_selected_output(int *wrote_field, int *field_output_started, const CutOptions *options) {
    if (!*field_output_started) {
        if (*wrote_field && rt_write_char(1, options->delimiter) != 0) {
            return -1;
        }
        *wrote_field = 1;
        *field_output_started = 1;
    }
    return 0;
}

static int finish_selected_empty_field(int selected, int *wrote_field, int *field_output_started, const CutOptions *options) {
    if (selected && !*field_output_started) {
        if (*wrote_field && rt_write_char(1, options->delimiter) != 0) {
            return -1;
        }
        *wrote_field = 1;
    }
    return 0;
}

static int cut_field_stream(int fd, const CutOptions *options) {
    char chunk[CUT_IO_BUFFER_SIZE];
    long bytes_read;
    unsigned long long field_no = 1;
    int wrote_field = 0;
    int field_output_started = 0;
    int selected = selection_includes(1ULL, options);
    int have_record = 0;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == options->line_delimiter) {
                if (finish_selected_empty_field(selected, &wrote_field, &field_output_started, options) != 0 ||
                    rt_write_char(1, options->line_delimiter) != 0) {
                    return -1;
                }
                field_no = 1ULL;
                wrote_field = 0;
                field_output_started = 0;
                selected = selection_includes(field_no, options);
                have_record = 0;
                continue;
            }

            have_record = 1;
            if (ch == options->delimiter) {
                if (finish_selected_empty_field(selected, &wrote_field, &field_output_started, options) != 0) {
                    return -1;
                }
                field_no += 1ULL;
                field_output_started = 0;
                selected = selection_includes(field_no, options);
            } else if (selected) {
                if (field_start_selected_output(&wrote_field, &field_output_started, options) != 0 ||
                    rt_write_char(1, ch) != 0) {
                    return -1;
                }
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (have_record && finish_selected_empty_field(selected, &wrote_field, &field_output_started, options) != 0) {
        return -1;
    }

    return 0;
}

static int cut_stream(int fd, const CutOptions *options) {
    if (options->mode == CUT_MODE_FIELDS) {
        return cut_field_stream(fd, options);
    }
    if (options->mode == CUT_MODE_BYTES) {
        return cut_byte_stream(fd, options);
    }
    return cut_codepoint_stream(fd, options);
}

static int parse_options(int argc, char **argv, CutOptions *options, int *arg_index_out) {
    int arg_index = 1;

    rt_memset(options, 0, sizeof(*options));
    options->delimiter = '\t';
    options->line_delimiter = '\n';

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

        if (rt_strcmp(argv[arg_index], "-z") == 0 || rt_strcmp(argv[arg_index], "--zero-terminated") == 0) {
            options->line_delimiter = '\0';
            arg_index += 1;
            continue;
        }

        if ((rt_strcmp(argv[arg_index], "-b") == 0 ||
             rt_strcmp(argv[arg_index], "-c") == 0 ||
             rt_strcmp(argv[arg_index], "-f") == 0) &&
            arg_index + 1 < argc) {
            if (validate_range_list(argv[arg_index + 1]) != 0) {
                return -1;
            }
            options->selections = argv[arg_index + 1];
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
