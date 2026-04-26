#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define RG_ENTRY_CAPACITY 1024
#define RG_LINE_CAPACITY 8192
#define RG_PATH_CAPACITY 1024
#define RG_MAX_GLOBS 32
#define RG_MAX_TYPES 16

typedef struct {
    int ignore_case;
    int fixed_strings;
    int whole_word;
    int invert_match;
    int line_numbers;
    int count_only;
    int quiet;
    int files_with_matches;
    int files_without_match;
    int no_filename;
    int with_filename;
    int include_hidden;
    int files_mode;
    int color_mode;
    const char *globs[RG_MAX_GLOBS];
    int glob_count;
    const char *type_filters[RG_MAX_TYPES];
    int type_count;
} RgOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[OPTIONS] PATTERN [PATH ...]");
}

static int is_hidden_name(const char *name) {
    return name[0] == '.' && rt_strcmp(name, ".") != 0 && rt_strcmp(name, "..") != 0;
}

static const char *path_base_name(const char *path) {
    const char *base = path;
    size_t i = 0;

    while (path[i] != '\0') {
        if (path[i] == '/') {
            base = path + i + 1U;
        }
        i += 1U;
    }
    return base;
}

static const char *path_extension(const char *path) {
    const char *base = path_base_name(path);
    const char *extension = "";
    size_t i = 0;

    while (base[i] != '\0') {
        if (base[i] == '.') {
            extension = base + i + 1U;
        }
        i += 1U;
    }
    return extension;
}

static int has_path_separator(const char *text) {
    size_t i = 0;
    while (text[i] != '\0') {
        if (text[i] == '/') {
            return 1;
        }
        i += 1U;
    }
    return 0;
}

static int glob_matches_path(const char *glob, const char *path) {
    if (tool_wildcard_match(glob, path)) {
        return 1;
    }
    if (!has_path_separator(glob) && tool_wildcard_match(glob, path_base_name(path))) {
        return 1;
    }
    return 0;
}

static int path_matches_globs(const RgOptions *options, const char *path) {
    int has_positive = 0;
    int positive_match = 0;
    int i;

    for (i = 0; i < options->glob_count; ++i) {
        const char *glob = options->globs[i];
        int negated = glob[0] == '!';
        const char *pattern = negated ? glob + 1 : glob;

        if (!negated) {
            has_positive = 1;
        }
        if (glob_matches_path(pattern, path)) {
            if (negated) {
                return 0;
            }
            positive_match = 1;
        }
    }

    return !has_positive || positive_match;
}

static int extension_is_one_of(const char *extension, const char *const *values, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) {
        if (rt_strcmp(extension, values[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int path_matches_type_name(const char *path, const char *type_name) {
    static const char *c_exts[] = {"c", "h"};
    static const char *cpp_exts[] = {"cc", "cpp", "cxx", "hh", "hpp", "hxx"};
    static const char *shell_exts[] = {"sh", "bash"};
    static const char *text_exts[] = {"txt", "text", "md", "markdown"};
    const char *extension = path_extension(path);

    if (rt_strcmp(type_name, "c") == 0) {
        return extension_is_one_of(extension, c_exts, sizeof(c_exts) / sizeof(c_exts[0]));
    }
    if (rt_strcmp(type_name, "cpp") == 0 || rt_strcmp(type_name, "c++") == 0) {
        return extension_is_one_of(extension, cpp_exts, sizeof(cpp_exts) / sizeof(cpp_exts[0]));
    }
    if (rt_strcmp(type_name, "md") == 0 || rt_strcmp(type_name, "markdown") == 0) {
        return rt_strcmp(extension, "md") == 0 || rt_strcmp(extension, "markdown") == 0;
    }
    if (rt_strcmp(type_name, "sh") == 0 || rt_strcmp(type_name, "shell") == 0) {
        return extension_is_one_of(extension, shell_exts, sizeof(shell_exts) / sizeof(shell_exts[0]));
    }
    if (rt_strcmp(type_name, "text") == 0 || rt_strcmp(type_name, "txt") == 0) {
        return extension_is_one_of(extension, text_exts, sizeof(text_exts) / sizeof(text_exts[0]));
    }
    return rt_strcmp(extension, type_name) == 0;
}

static int path_matches_types(const RgOptions *options, const char *path) {
    int i;

    if (options->type_count == 0) {
        return 1;
    }
    for (i = 0; i < options->type_count; ++i) {
        if (path_matches_type_name(path, options->type_filters[i])) {
            return 1;
        }
    }
    return 0;
}

static int should_search_file(const RgOptions *options, const char *path) {
    return path_matches_globs(options, path) && path_matches_types(options, path);
}

static int starts_with_literal(const char *pattern, const char *text, int ignore_case, size_t *consumed_out) {
    size_t pattern_len = rt_strlen(pattern);
    size_t text_len = rt_strlen(text);
    size_t pi = 0U;
    size_t ti = 0U;

    while (pi < pattern_len) {
        unsigned int lhs = 0;
        unsigned int rhs = 0;

        if (ti >= text_len || rt_utf8_decode(pattern, pattern_len, &pi, &lhs) != 0 ||
            rt_utf8_decode(text, text_len, &ti, &rhs) != 0) {
            return 0;
        }
        if (ignore_case) {
            lhs = rt_unicode_simple_fold(lhs);
            rhs = rt_unicode_simple_fold(rhs);
        }
        if (lhs != rhs) {
            return 0;
        }
    }

    *consumed_out = ti;
    return 1;
}

static int find_fixed_match(const char *pattern,
                            const char *text,
                            int ignore_case,
                            size_t search_start,
                            size_t *start_out,
                            size_t *end_out) {
    size_t pos = search_start;
    size_t pattern_len = rt_strlen(pattern);

    if (pattern_len == 0U) {
        *start_out = search_start;
        *end_out = search_start;
        return 1;
    }

    while (1) {
        size_t consumed = 0U;

        if (starts_with_literal(pattern, text + pos, ignore_case, &consumed)) {
            *start_out = pos;
            *end_out = pos + consumed;
            return 1;
        }
        if (text[pos] == '\0') {
            break;
        }
        pos += 1U;
        while ((text[pos] & 0xc0) == 0x80) {
            pos += 1U;
        }
    }

    return 0;
}

static int is_utf8_continuation_byte(unsigned char ch) {
    return (ch & 0xc0U) == 0x80U;
}

static size_t previous_codepoint_start(const char *text, size_t index) {
    if (index == 0U) {
        return 0U;
    }
    index -= 1U;
    while (index > 0U && is_utf8_continuation_byte((unsigned char)text[index])) {
        index -= 1U;
    }
    return index;
}

static int match_has_word_boundaries(const char *text, size_t start, size_t end) {
    size_t length = rt_strlen(text);

    if (start > 0U) {
        size_t prev = previous_codepoint_start(text, start);
        size_t index = prev;
        unsigned int codepoint = 0;
        if (rt_utf8_decode(text, length, &index, &codepoint) == 0 && rt_unicode_is_word(codepoint)) {
            return 0;
        }
    }
    if (end < length) {
        size_t index = end;
        unsigned int codepoint = 0;
        if (rt_utf8_decode(text, length, &index, &codepoint) == 0 && rt_unicode_is_word(codepoint)) {
            return 0;
        }
    }
    return 1;
}

static int find_next_match(const RgOptions *options,
                           const char *pattern,
                           const char *text,
                           size_t search_start,
                           size_t *start_out,
                           size_t *end_out) {
    size_t candidate_start = 0U;
    size_t candidate_end = 0U;
    size_t next_start = search_start;

    while (1) {
        int found;

        if (options->fixed_strings) {
            found = find_fixed_match(pattern, text, options->ignore_case, next_start, &candidate_start, &candidate_end);
        } else {
            found = tool_regex_search(pattern, text, options->ignore_case, next_start, &candidate_start, &candidate_end);
        }
        if (!found) {
            return 0;
        }
        if (!options->whole_word || match_has_word_boundaries(text, candidate_start, candidate_end)) {
            *start_out = candidate_start;
            *end_out = candidate_end;
            return 1;
        }
        if (text[candidate_start] == '\0') {
            break;
        }
        next_start = candidate_start + 1U;
    }
    return 0;
}

static int rg_use_color(const RgOptions *options) {
    return tool_should_use_color_fd(1, options->color_mode);
}

static int write_segment(const char *text, size_t length) {
    return length == 0U ? 0 : rt_write_all(1, text, length);
}

static int write_highlight(const RgOptions *options, const char *text, size_t length) {
    if (length == 0U) {
        return 0;
    }
    if (rg_use_color(options)) {
        tool_style_begin(1, options->color_mode, TOOL_STYLE_BOLD_RED);
    }
    if (rt_write_all(1, text, length) != 0) {
        if (rg_use_color(options)) {
            tool_style_end(1, options->color_mode);
        }
        return -1;
    }
    if (rg_use_color(options)) {
        tool_style_end(1, options->color_mode);
    }
    return 0;
}

static int write_highlighted_line(const RgOptions *options, const char *pattern, const char *line) {
    size_t search_start = 0U;
    size_t rendered = 0U;

    while (1) {
        size_t start = 0U;
        size_t end = 0U;

        if (!find_next_match(options, pattern, line, search_start, &start, &end)) {
            break;
        }
        if (start > rendered && write_segment(line + rendered, start - rendered) != 0) {
            return -1;
        }
        if (write_highlight(options, line + start, end - start) != 0) {
            return -1;
        }
        rendered = end;
        if (end == start) {
            if (line[end] == '\0') {
                break;
            }
            search_start = end + 1U;
        } else {
            search_start = end;
        }
    }

    return rt_write_line(1, line + rendered);
}

static int print_prefix(const RgOptions *options,
                        const char *path,
                        int show_path,
                        unsigned long long line_no) {
    if (show_path) {
        if (rg_use_color(options)) {
            tool_style_begin(1, options->color_mode, TOOL_STYLE_BOLD_MAGENTA);
        }
        if (rt_write_cstr(1, path) != 0) {
            return -1;
        }
        if (rg_use_color(options)) {
            tool_style_end(1, options->color_mode);
        }
        if (rt_write_char(1, ':') != 0) {
            return -1;
        }
    }
    if (options->line_numbers) {
        if (rg_use_color(options)) {
            tool_style_begin(1, options->color_mode, TOOL_STYLE_BOLD_GREEN);
        }
        if (rt_write_uint(1, line_no) != 0) {
            return -1;
        }
        if (rg_use_color(options)) {
            tool_style_end(1, options->color_mode);
        }
        if (rt_write_char(1, ':') != 0) {
            return -1;
        }
    }
    return 0;
}

static int print_match_line(const RgOptions *options,
                            const char *pattern,
                            const char *path,
                            int show_path,
                            unsigned long long line_no,
                            const char *line) {
    if (print_prefix(options, path, show_path, line_no) != 0) {
        return -1;
    }
    if (!rg_use_color(options) || options->invert_match) {
        return rt_write_line(1, line);
    }
    return write_highlighted_line(options, pattern, line);
}

static int rg_stream_file(int fd,
                          const char *path,
                          const char *pattern,
                          const RgOptions *options,
                          int show_path,
                          int *matched_out) {
    char chunk[4096];
    char line[RG_LINE_CAPACITY];
    size_t line_len = 0U;
    unsigned long long line_no = 1ULL;
    unsigned long long match_count = 0ULL;
    int matched = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;
        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];
            if (ch == '\0') {
                if (matched_out != 0) {
                    *matched_out = 0;
                }
                return 0;
            }
            if (ch == '\n') {
                size_t start = 0U;
                size_t end = 0U;
                int line_matches;

                line[line_len] = '\0';
                line_matches = find_next_match(options, pattern, line, 0U, &start, &end);
                if (options->invert_match) {
                    line_matches = !line_matches;
                }
                if (line_matches) {
                    matched = 1;
                    match_count += 1ULL;
                    if (options->quiet) {
                        if (matched_out != 0) {
                            *matched_out = 1;
                        }
                        return 0;
                    }
                    if (!options->count_only && !options->files_with_matches && !options->files_without_match) {
                        if (print_match_line(options, pattern, path, show_path, line_no, line) != 0) {
                            return -1;
                        }
                    }
                }
                line_len = 0U;
                line_no += 1ULL;
            } else if (line_len + 1U < sizeof(line)) {
                line[line_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }
    if (line_len > 0U) {
        size_t start = 0U;
        size_t end = 0U;
        int line_matches;

        line[line_len] = '\0';
        line_matches = find_next_match(options, pattern, line, 0U, &start, &end);
        if (options->invert_match) {
            line_matches = !line_matches;
        }
        if (line_matches) {
            matched = 1;
            match_count += 1ULL;
            if (options->quiet) {
                if (matched_out != 0) {
                    *matched_out = 1;
                }
                return 0;
            }
            if (!options->count_only && !options->files_with_matches && !options->files_without_match) {
                if (print_match_line(options, pattern, path, show_path, line_no, line) != 0) {
                    return -1;
                }
            }
        }
    }

    if (options->count_only && !options->quiet && !options->files_with_matches && !options->files_without_match) {
        if (show_path && (rt_write_cstr(1, path) != 0 || rt_write_char(1, ':') != 0)) {
            return -1;
        }
        if (rt_write_uint(1, match_count) != 0 || rt_write_char(1, '\n') != 0) {
            return -1;
        }
    }
    if (matched_out != 0) {
        *matched_out = matched;
    }
    return 0;
}

static int rg_search_path(const char *path,
                          const char *pattern,
                          const RgOptions *options,
                          int multiple_roots,
                          int *matched_out,
                          int *error_out);

static int rg_search_file(const char *path,
                          const char *pattern,
                          const RgOptions *options,
                          int show_path,
                          int *matched_out,
                          int *error_out) {
    int fd;
    int should_close;
    int matched = 0;

    if (!should_search_file(options, path)) {
        if (matched_out != 0) {
            *matched_out = 0;
        }
        return 0;
    }
    if (options->files_mode) {
        if (rt_write_line(1, path) != 0) {
            *error_out = 1;
            return -1;
        }
        if (matched_out != 0) {
            *matched_out = 0;
        }
        return 0;
    }
    if (tool_open_input(path, &fd, &should_close) != 0) {
        tool_write_error("ripgrep", "cannot open ", path);
        *error_out = 1;
        return -1;
    }
    if (rg_stream_file(fd, path, pattern, options, show_path, &matched) != 0) {
        tool_write_error("ripgrep", "read error on ", path);
        tool_close_input(fd, should_close);
        *error_out = 1;
        return -1;
    }
    tool_close_input(fd, should_close);

    if (!options->quiet && options->files_with_matches && matched) {
        if (rt_write_line(1, path) != 0) {
            *error_out = 1;
            return -1;
        }
    }
    if (!options->quiet && options->files_without_match && !matched) {
        if (rt_write_line(1, path) != 0) {
            *error_out = 1;
            return -1;
        }
    }
    if (matched_out != 0) {
        *matched_out = matched;
    }
    return 0;
}

static int rg_search_directory(const char *path,
                               const char *pattern,
                               const RgOptions *options,
                               int *matched_out,
                               int *error_out) {
    PlatformDirEntry entries[RG_ENTRY_CAPACITY];
    size_t count = 0U;
    size_t i;
    int is_directory = 0;
    int any_match = 0;

    if (platform_collect_entries(path, 1, entries, RG_ENTRY_CAPACITY, &count, &is_directory) != 0 || !is_directory) {
        tool_write_error("ripgrep", "cannot read directory ", path);
        *error_out = 1;
        return -1;
    }

    for (i = 0; i < count; ++i) {
        char child_path[RG_PATH_CAPACITY];
        int child_matched = 0;

        if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
            continue;
        }
        if (!options->include_hidden && (entries[i].is_hidden || is_hidden_name(entries[i].name))) {
            continue;
        }
        if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0) {
            tool_write_error("ripgrep", "path too long under ", path);
            *error_out = 1;
            continue;
        }
        if (rg_search_path(child_path, pattern, options, 1, &child_matched, error_out) != 0) {
            continue;
        }
        if (child_matched) {
            any_match = 1;
            if (options->quiet) {
                break;
            }
        }
    }

    platform_free_entries(entries, count);
    if (matched_out != 0) {
        *matched_out = any_match;
    }
    return *error_out ? -1 : 0;
}

static int rg_search_path(const char *path,
                          const char *pattern,
                          const RgOptions *options,
                          int multiple_roots,
                          int *matched_out,
                          int *error_out) {
    int is_directory = 0;

    if (platform_path_is_directory(path, &is_directory) == 0 && is_directory) {
        return rg_search_directory(path, pattern, options, matched_out, error_out);
    }

    return rg_search_file(path, pattern, options, !options->no_filename && (options->with_filename || multiple_roots), matched_out, error_out);
}

static int add_glob(RgOptions *options, const char *glob) {
    if (options->glob_count >= RG_MAX_GLOBS) {
        tool_write_error("ripgrep", "too many glob filters", "");
        return -1;
    }
    options->globs[options->glob_count++] = glob;
    return 0;
}

static int add_type(RgOptions *options, const char *type_name) {
    if (options->type_count >= RG_MAX_TYPES) {
        tool_write_error("ripgrep", "too many type filters", "");
        return -1;
    }
    options->type_filters[options->type_count++] = type_name;
    return 0;
}

static int parse_long_option(RgOptions *options, ToolOptState *s) {
    const char *flag = s->flag;

    if (rt_strcmp(flag, "--files") == 0) {
        options->files_mode = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--hidden") == 0) {
        options->include_hidden = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--ignore-case") == 0) {
        options->ignore_case = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--fixed-strings") == 0) {
        options->fixed_strings = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--word-regexp") == 0) {
        options->whole_word = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--invert-match") == 0) {
        options->invert_match = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--line-number") == 0) {
        options->line_numbers = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--no-line-number") == 0) {
        options->line_numbers = 0;
        return 0;
    }
    if (rt_strcmp(flag, "--count") == 0) {
        options->count_only = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--quiet") == 0) {
        options->quiet = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--files-with-matches") == 0) {
        options->files_with_matches = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--files-without-match") == 0) {
        options->files_without_match = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--no-filename") == 0) {
        options->no_filename = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--with-filename") == 0) {
        options->with_filename = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--glob") == 0) {
        if (tool_opt_require_value(s) != 0) {
            return -1;
        }
        return add_glob(options, s->value);
    }
    if (rt_strncmp(flag, "--glob=", 7U) == 0) {
        return add_glob(options, flag + 7);
    }
    if (rt_strcmp(flag, "--type") == 0) {
        if (tool_opt_require_value(s) != 0) {
            return -1;
        }
        return add_type(options, s->value);
    }
    if (rt_strncmp(flag, "--type=", 7U) == 0) {
        return add_type(options, flag + 7);
    }
    if (rt_strcmp(flag, "--color") == 0 || rt_strcmp(flag, "--colour") == 0) {
        options->color_mode = TOOL_COLOR_AUTO;
        tool_set_global_color_mode(options->color_mode);
        return 0;
    }
    if (rt_strncmp(flag, "--color=", 8U) == 0 || rt_strncmp(flag, "--colour=", 9U) == 0) {
        const char *value = (rt_strncmp(flag, "--color=", 8U) == 0) ? flag + 8 : flag + 9;
        if (tool_parse_color_mode(value, &options->color_mode) != 0) {
            return -1;
        }
        tool_set_global_color_mode(options->color_mode);
        return 0;
    }

    return -1;
}

static int parse_short_options(RgOptions *options, ToolOptState *s) {
    const char *flag = s->flag + 1;

    while (*flag != '\0') {
        if (*flag == 'i') {
            options->ignore_case = 1;
        } else if (*flag == 'F') {
            options->fixed_strings = 1;
        } else if (*flag == 'w') {
            options->whole_word = 1;
        } else if (*flag == 'v') {
            options->invert_match = 1;
        } else if (*flag == 'n') {
            options->line_numbers = 1;
        } else if (*flag == 'c') {
            options->count_only = 1;
        } else if (*flag == 'q') {
            options->quiet = 1;
        } else if (*flag == 'l') {
            options->files_with_matches = 1;
        } else if (*flag == 'L') {
            options->files_without_match = 1;
        } else if (*flag == 'H') {
            options->with_filename = 1;
        } else if (*flag == 'h') {
            options->no_filename = 1;
        } else if (*flag == 'g' || *flag == 't') {
            const char *value = flag + 1;
            if (*value == '\0') {
                if (s->argi >= s->argc) {
                    return -1;
                }
                value = s->argv[s->argi++];
            }
            if (*flag == 'g') {
                return add_glob(options, value);
            }
            return add_type(options, value);
        } else {
            return -1;
        }
        flag += 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    RgOptions options;
    ToolOptState s;
    const char *pattern = 0;
    int r;
    int path_start;
    int path_count;
    int i;
    int any_match = 0;
    int had_error = 0;

    rt_memset(&options, 0, sizeof(options));
    options.color_mode = TOOL_COLOR_AUTO;
    options.line_numbers = 1;

    tool_opt_init(&s, argc, argv, tool_base_name(argv[0]),
                  "[OPTIONS] PATTERN [PATH ...]");
    while ((r = tool_opt_next(&s)) == TOOL_OPT_FLAG) {
        if (s.flag[0] == '-' && s.flag[1] == '-') {
            if (parse_long_option(&options, &s) != 0) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (parse_short_options(&options, &s) != 0) {
            print_usage(argv[0]);
            return 2;
        }
    }
    if (r == TOOL_OPT_HELP) {
        print_usage(argv[0]);
        return 0;
    }
    if (r == TOOL_OPT_ERROR) {
        return 2;
    }

    if (options.quiet) {
        options.count_only = 0;
        options.files_with_matches = 0;
        options.files_without_match = 0;
    }
    if (options.files_mode) {
        pattern = "";
        path_start = s.argi;
    } else {
        if (s.argi >= argc) {
            print_usage(argv[0]);
            return 2;
        }
        pattern = argv[s.argi++];
        path_start = s.argi;
    }

    path_count = argc - path_start;
    if (path_count <= 0) {
        int matched = 0;
        if (options.files_mode) {
            if (rg_search_path(".", pattern, &options, 1, &matched, &had_error) != 0 && had_error) {
                return 2;
            }
            return 0;
        }
        if (rg_search_path(".", pattern, &options, 1, &matched, &had_error) != 0 && had_error) {
            return 2;
        }
        return matched ? 0 : 1;
    }

    for (i = path_start; i < argc; ++i) {
        int matched = 0;
        if (rg_search_path(argv[i], pattern, &options, path_count > 1, &matched, &had_error) != 0 && options.quiet && matched) {
            any_match = 1;
            break;
        }
        if (matched) {
            any_match = 1;
        }
        if (options.quiet && any_match) {
            break;
        }
    }

    if (had_error) {
        return 2;
    }
    if (options.files_mode) {
        return 0;
    }
    return any_match ? 0 : 1;
}