#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define FMT_DEFAULT_WIDTH 75ULL
#define FMT_MAX_LINE 2048
#define FMT_MAX_WORD 512
#define FMT_MAX_PREFIX 128

typedef struct {
    unsigned long long width;
    int split_only;
    int uniform_spacing;
    int crown_margin;
    int have_prefix_filter;
    char prefix_filter[FMT_MAX_PREFIX];
} FmtOptions;

typedef struct {
    const FmtOptions *options;
    char first_prefix[FMT_MAX_PREFIX];
    char body_prefix[FMT_MAX_PREFIX];
    size_t first_prefix_len;
    size_t body_prefix_len;
    size_t first_prefix_width;
    size_t body_prefix_width;
    char current_line[FMT_MAX_LINE];
    size_t current_line_len;
    size_t current_line_width;
    char previous_word[FMT_MAX_WORD];
    size_t previous_word_len;
    unsigned int input_line_count;
    int paragraph_active;
    int use_first_prefix;
} FmtState;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-w WIDTH] [-s] [-u] [-c] [-p PREFIX] [file ...]");
}

static int starts_with_text(const char *text, const char *prefix) {
    size_t i = 0U;
    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }
    return 1;
}

static const char *option_attached_value(const char *arg, const char *short_name, const char *long_name) {
    size_t short_len = rt_strlen(short_name);
    size_t long_len = rt_strlen(long_name);

    if (starts_with_text(arg, short_name) && arg[short_len] != '\0') {
        return arg + short_len;
    }
    if (starts_with_text(arg, long_name) && arg[long_len] == '=') {
        return arg + long_len + 1U;
    }
    return 0;
}

static int is_prefix_marker_char(char ch) {
    return ch == '#' || ch == '>' || ch == '*' || ch == '/' ||
           ch == '-' || ch == ';' || ch == '%' || ch == '|' ||
           ch == ':';
}

static int is_blank_text(const char *text) {
    size_t i = 0U;
    while (text[i] != '\0') {
        if (!rt_is_space(text[i])) {
            return 0;
        }
        i += 1U;
    }
    return 1;
}

static size_t utf8_display_width_n(const char *text, size_t length) {
    size_t index = 0U;
    size_t width = 0U;

    while (index < length) {
        size_t before = index;
        unsigned int codepoint = 0;

        if (rt_utf8_decode(text, length, &index, &codepoint) != 0) {
            index = before + 1U;
            codepoint = 0xfffdU;
        }

        if (codepoint == '\t') {
            width += 8U - (width % 8U);
        } else {
            width += (size_t)rt_unicode_display_width(codepoint);
        }
    }

    return width;
}

static void detect_prefix(const char *line, char *prefix, size_t prefix_size, const char **content_out) {
    size_t pos = 0U;
    size_t prefix_len = 0U;
    size_t marker_start;
    size_t marker_end;

    while (line[pos] == ' ' || line[pos] == '\t') {
        if (prefix_len + 1U < prefix_size) {
            prefix[prefix_len++] = line[pos];
        }
        pos += 1U;
    }

    marker_start = pos;
    while (line[pos] != '\0' && is_prefix_marker_char(line[pos]) && (pos - marker_start) < 4U) {
        pos += 1U;
    }
    marker_end = pos;

    if (marker_end > marker_start && (line[pos] == ' ' || line[pos] == '\t')) {
        size_t i;
        for (i = marker_start; i < marker_end; ++i) {
            if (prefix_len + 1U < prefix_size) {
                prefix[prefix_len++] = line[i];
            }
        }
        while (line[pos] == ' ' || line[pos] == '\t') {
            if (prefix_len + 1U < prefix_size) {
                prefix[prefix_len++] = line[pos];
            }
            pos += 1U;
        }
    } else if (marker_end > marker_start) {
        pos = marker_start;
    }

    prefix[prefix_len] = '\0';
    *content_out = line + pos;
}

static int word_ends_sentence(const char *word, size_t word_len) {
    while (word_len > 0U) {
        char ch = word[word_len - 1U];
        if (ch == '"' || ch == '\'' || ch == ')' || ch == ']' || ch == '}') {
            word_len -= 1U;
            continue;
        }
        return ch == '.' || ch == '!' || ch == '?' || ch == ':';
    }
    return 0;
}

static void fmt_reset_paragraph(FmtState *state) {
    state->first_prefix[0] = '\0';
    state->body_prefix[0] = '\0';
    state->first_prefix_len = 0U;
    state->body_prefix_len = 0U;
    state->first_prefix_width = 0U;
    state->body_prefix_width = 0U;
    state->current_line_len = 0U;
    state->current_line_width = 0U;
    state->previous_word_len = 0U;
    state->input_line_count = 0U;
    state->paragraph_active = 0;
    state->use_first_prefix = 1;
}

static int fmt_flush_output_line(FmtState *state) {
    if (state->current_line_len == 0U) {
        return 0;
    }

    state->current_line[state->current_line_len] = '\0';
    if (rt_write_line(1, state->current_line) != 0) {
        return -1;
    }

    state->current_line_len = 0U;
    state->current_line_width = 0U;
    state->use_first_prefix = 0;
    return 0;
}

static int fmt_flush_paragraph(FmtState *state) {
    if (fmt_flush_output_line(state) != 0) {
        return -1;
    }
    fmt_reset_paragraph(state);
    return 0;
}

static void fmt_start_paragraph(FmtState *state, const char *prefix) {
    fmt_reset_paragraph(state);
    rt_copy_string(state->first_prefix, sizeof(state->first_prefix), prefix);
    rt_copy_string(state->body_prefix, sizeof(state->body_prefix), prefix);
    state->first_prefix_len = rt_strlen(state->first_prefix);
    state->body_prefix_len = rt_strlen(state->body_prefix);
    state->first_prefix_width = utf8_display_width_n(state->first_prefix, state->first_prefix_len);
    state->body_prefix_width = utf8_display_width_n(state->body_prefix, state->body_prefix_len);
    state->paragraph_active = 1;
}

static int fmt_append_word(FmtState *state, const char *word, size_t word_len) {
    size_t max_width = (state->options->width > (unsigned long long)(FMT_MAX_LINE - 1))
                           ? (FMT_MAX_LINE - 1U)
                           : (size_t)state->options->width;
    size_t copy_len = word_len;
    size_t word_width;
    const char *prefix = state->use_first_prefix ? state->first_prefix : state->body_prefix;
    size_t prefix_len = state->use_first_prefix ? state->first_prefix_len : state->body_prefix_len;
    size_t prefix_width = state->use_first_prefix ? state->first_prefix_width : state->body_prefix_width;

    if (copy_len >= FMT_MAX_WORD) {
        copy_len = FMT_MAX_WORD - 1U;
    }
    word_width = utf8_display_width_n(word, copy_len);

    if (state->current_line_len == 0U) {
        if (prefix_len > 0U) {
            memcpy(state->current_line, prefix, prefix_len);
        }
        memcpy(state->current_line + prefix_len, word, copy_len);
        state->current_line_len = prefix_len + copy_len;
        state->current_line_width = prefix_width + word_width;
    } else {
        size_t gap = 1U;

        if (state->options->uniform_spacing && word_ends_sentence(state->previous_word, state->previous_word_len)) {
            gap = 2U;
        }

        if (state->current_line_width + gap + word_width > max_width && state->current_line_width > prefix_width) {
            if (fmt_flush_output_line(state) != 0) {
                return -1;
            }
            prefix = state->use_first_prefix ? state->first_prefix : state->body_prefix;
            prefix_len = state->use_first_prefix ? state->first_prefix_len : state->body_prefix_len;
            prefix_width = state->use_first_prefix ? state->first_prefix_width : state->body_prefix_width;
            if (prefix_len > 0U) {
                memcpy(state->current_line, prefix, prefix_len);
            }
            memcpy(state->current_line + prefix_len, word, copy_len);
            state->current_line_len = prefix_len + copy_len;
            state->current_line_width = prefix_width + word_width;
        } else {
            size_t i;
            for (i = 0U; i < gap; ++i) {
                state->current_line[state->current_line_len++] = ' ';
            }
            memcpy(state->current_line + state->current_line_len, word, copy_len);
            state->current_line_len += copy_len;
            state->current_line_width += gap + word_width;
        }
    }

    if (copy_len > 0U) {
        memcpy(state->previous_word, word, copy_len);
    }
    state->previous_word[copy_len] = '\0';
    state->previous_word_len = copy_len;

    if (state->current_line_width >= max_width) {
        return fmt_flush_output_line(state);
    }

    return 0;
}

static int fmt_add_text(FmtState *state, const char *text) {
    char word[FMT_MAX_WORD];
    size_t word_len = 0U;
    size_t i;

    for (i = 0U;; ++i) {
        char ch = text[i];
        if (ch == '\0' || rt_is_space(ch)) {
            if (word_len > 0U) {
                if (fmt_append_word(state, word, word_len) != 0) {
                    return -1;
                }
                word_len = 0U;
            }
            if (ch == '\0') {
                break;
            }
        } else if (word_len + 1U < sizeof(word)) {
            word[word_len++] = ch;
        }
    }

    return 0;
}

static int process_input_line(FmtState *state, const char *raw_line) {
    char prefix[FMT_MAX_PREFIX];
    const char *content = raw_line;

    if (is_blank_text(raw_line)) {
        if (fmt_flush_paragraph(state) != 0) {
            return -1;
        }
        return rt_write_char(1, '\n');
    }

    if (state->options->have_prefix_filter) {
        size_t prefix_len = rt_strlen(state->options->prefix_filter);
        if (!starts_with_text(raw_line, state->options->prefix_filter)) {
            if (fmt_flush_paragraph(state) != 0) {
                return -1;
            }
            return rt_write_line(1, raw_line);
        }
        rt_copy_string(prefix, sizeof(prefix), state->options->prefix_filter);
        content = raw_line + prefix_len;
    } else {
        detect_prefix(raw_line, prefix, sizeof(prefix), &content);
    }

    if (is_blank_text(content)) {
        if (fmt_flush_paragraph(state) != 0) {
            return -1;
        }
        return rt_write_line(1, raw_line);
    }

    if (state->options->split_only && state->paragraph_active) {
        if (fmt_flush_paragraph(state) != 0) {
            return -1;
        }
    }

    if (!state->paragraph_active) {
        fmt_start_paragraph(state, prefix);
    } else if (state->options->crown_margin && state->input_line_count == 1U) {
        if (rt_strcmp(prefix, state->first_prefix) != 0) {
            rt_copy_string(state->body_prefix, sizeof(state->body_prefix), prefix);
            state->body_prefix_len = rt_strlen(state->body_prefix);
            state->body_prefix_width = utf8_display_width_n(state->body_prefix, state->body_prefix_len);
        }
    } else {
        const char *expected_prefix = state->options->crown_margin ? state->body_prefix : state->first_prefix;
        if (rt_strcmp(prefix, expected_prefix) != 0) {
            if (fmt_flush_paragraph(state) != 0) {
                return -1;
            }
            fmt_start_paragraph(state, prefix);
        }
    }

    if (fmt_add_text(state, content) != 0) {
        return -1;
    }

    state->input_line_count += 1U;
    return 0;
}

static int format_stream(int fd, const FmtOptions *options) {
    FmtState state;
    char raw_line[FMT_MAX_LINE];
    size_t raw_len = 0U;

    rt_memset(&state, 0, sizeof(state));
    state.options = options;
    state.use_first_prefix = 1;

    for (;;) {
        char ch = '\0';
        long bytes_read = platform_read(fd, &ch, 1U);

        if (bytes_read < 0) {
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        if (ch == '\n') {
            raw_line[raw_len] = '\0';
            if (process_input_line(&state, raw_line) != 0) {
                return -1;
            }
            raw_len = 0U;
        } else if (raw_len + 1U < sizeof(raw_line)) {
            raw_line[raw_len++] = ch;
        }
    }

    if (raw_len > 0U) {
        raw_line[raw_len] = '\0';
        if (process_input_line(&state, raw_line) != 0) {
            return -1;
        }
    }

    return fmt_flush_paragraph(&state);
}

int main(int argc, char **argv) {
    FmtOptions options;
    int argi = 1;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));
    options.width = FMT_DEFAULT_WIDTH;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *value_text = 0;

        if (rt_strcmp(argv[argi], "-w") == 0 || rt_strcmp(argv[argi], "--width") == 0) {
            if (argi + 1 >= argc ||
                tool_parse_uint_arg(argv[argi + 1], &options.width, "fmt", "width") != 0 ||
                options.width == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if ((value_text = option_attached_value(argv[argi], "-w", "--width")) != 0) {
            if (tool_parse_uint_arg(value_text, &options.width, "fmt", "width") != 0 || options.width == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-s") == 0 || rt_strcmp(argv[argi], "--split-only") == 0) {
            options.split_only = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-u") == 0 || rt_strcmp(argv[argi], "--uniform-spacing") == 0) {
            options.uniform_spacing = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-c") == 0 || rt_strcmp(argv[argi], "--crown-margin") == 0) {
            options.crown_margin = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-p") == 0 || rt_strcmp(argv[argi], "--prefix") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            rt_copy_string(options.prefix_filter, sizeof(options.prefix_filter), argv[argi + 1]);
            options.have_prefix_filter = 1;
            argi += 2;
        } else if ((value_text = option_attached_value(argv[argi], "-p", "--prefix")) != 0) {
            rt_copy_string(options.prefix_filter, sizeof(options.prefix_filter), value_text);
            options.have_prefix_filter = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (argi == argc) {
        return format_stream(0, &options) == 0 ? 0 : 1;
    }

    while (argi < argc) {
        int fd;
        int should_close;

        if (tool_open_input(argv[argi], &fd, &should_close) != 0) {
            tool_write_error("fmt", "cannot open ", argv[argi]);
            exit_code = 1;
        } else {
            if (format_stream(fd, &options) != 0) {
                tool_write_error("fmt", "read error on ", argv[argi]);
                exit_code = 1;
            }
            tool_close_input(fd, should_close);
        }
        argi += 1;
    }

    return exit_code;
}
