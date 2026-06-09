#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define RG_ENTRY_CAPACITY 1024
#define RG_LINE_CAPACITY 8192
#define RG_PATH_CAPACITY 1024
#define RG_GLOB_CAPACITY 128
#define RG_MAX_GLOBS 16
#define RG_MAX_TYPES 16
#define RG_MAX_PATTERNS 32
#define RG_MAX_IGNORE_PATTERNS 32
#define RG_IGNORE_BASE_CAPACITY 96
#define RG_IGNORE_PATTERN_CAPACITY 96

typedef struct {
    char base[RG_IGNORE_BASE_CAPACITY];
    char pattern[RG_IGNORE_PATTERN_CAPACITY];
    int negated;
    int directory_only;
} RgIgnorePattern;

typedef struct {
    int ignore_case;
    int smart_case;
    int fixed_strings;
    int whole_word;
    int invert_match;
    int line_numbers;
    int column_numbers;
    int count_only;
    int quiet;
    int files_with_matches;
    int files_without_match;
    int only_matching;
    int no_filename;
    int with_filename;
    int heading;
    int no_heading;
    int no_messages;
    int include_hidden;
    int use_ignore_files;
    int follow_symlinks;
    int files_mode;
    int type_list;
    int sort_path;
    int stats;
    int has_max_count;
    unsigned long long max_count;
    int max_depth;
    int color_mode;
    const char *patterns[RG_MAX_PATTERNS];
    int pattern_count;
    const char *globs[RG_MAX_GLOBS];
    char glob_storage[RG_MAX_GLOBS][RG_GLOB_CAPACITY];
    int glob_count;
    const char *type_filters[RG_MAX_TYPES];
    int type_count;
    const char *type_not_filters[RG_MAX_TYPES];
    int type_not_count;
    RgIgnorePattern *ignore_patterns;
    int ignore_pattern_count;
    int ignore_pattern_capacity;
    unsigned long long stats_files_searched;
    unsigned long long stats_files_with_matches;
    unsigned long long stats_matched_lines;
    unsigned long long stats_matches;
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

#define has_path_separator tool_path_has_separator

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

static int path_matches_type_name(const char *path, const char *type_name) {
    const char *extension = path_extension(path);

    if (rt_strcmp(type_name, "c") == 0) {
        return rt_strcmp(extension, "c") == 0 || rt_strcmp(extension, "h") == 0;
    }
    if (rt_strcmp(type_name, "cpp") == 0 || rt_strcmp(type_name, "c++") == 0) {
        return rt_strcmp(extension, "cc") == 0 || rt_strcmp(extension, "cpp") == 0 ||
               rt_strcmp(extension, "cxx") == 0 || rt_strcmp(extension, "hh") == 0 ||
               rt_strcmp(extension, "hpp") == 0 || rt_strcmp(extension, "hxx") == 0;
    }
    if (rt_strcmp(type_name, "md") == 0 || rt_strcmp(type_name, "markdown") == 0) {
        return rt_strcmp(extension, "md") == 0 || rt_strcmp(extension, "markdown") == 0;
    }
    if (rt_strcmp(type_name, "sh") == 0 || rt_strcmp(type_name, "shell") == 0) {
        return rt_strcmp(extension, "sh") == 0 || rt_strcmp(extension, "bash") == 0;
    }
    if (rt_strcmp(type_name, "text") == 0 || rt_strcmp(type_name, "txt") == 0) {
        return rt_strcmp(extension, "txt") == 0 || rt_strcmp(extension, "text") == 0 ||
               rt_strcmp(extension, "md") == 0 || rt_strcmp(extension, "markdown") == 0;
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

static int path_matches_type_not(const RgOptions *options, const char *path) {
    int i;

    for (i = 0; i < options->type_not_count; ++i) {
        if (path_matches_type_name(path, options->type_not_filters[i])) {
            return 1;
        }
    }
    return 0;
}

static int should_visit_path(const RgOptions *options, const char *path) {
    return path_matches_globs(options, path) && !path_matches_type_not(options, path);
}

static int should_search_file(const RgOptions *options, const char *path) {
    return should_visit_path(options, path) && path_matches_types(options, path);
}

static int path_excluded_by_negative_glob(const RgOptions *options, const char *path) {
    int i;

    for (i = 0; i < options->glob_count; ++i) {
        const char *glob = options->globs[i];
        if (glob[0] == '!' && glob_matches_path(glob + 1, path)) return 1;
    }
    return 0;
}

static int compare_dir_entries_by_name(const void *left_ptr, const void *right_ptr) {
    const PlatformDirEntry *left = (const PlatformDirEntry *)left_ptr;
    const PlatformDirEntry *right = (const PlatformDirEntry *)right_ptr;
    return rt_strcmp(left->name, right->name);
}

static int path_is_directory_for_options(const RgOptions *options, const char *path, int *is_directory_out) {
    PlatformDirEntry entry;

    if (options->follow_symlinks) {
        if (platform_get_path_info_follow(path, &entry) != 0) return -1;
        *is_directory_out = entry.is_dir;
        return 0;
    }
    return platform_path_is_directory(path, is_directory_out);
}

static int text_has_uppercase(const char *text) {
    size_t index = 0U;

    while (text[index] != '\0') {
        unsigned char ch = (unsigned char)text[index++];
        if (ch >= 'A' && ch <= 'Z') return 1;
    }
    return 0;
}

static void apply_smart_case(RgOptions *options) {
    int i;

    if (!options->smart_case) return;
    options->ignore_case = 1;
    for (i = 0; i < options->pattern_count; ++i) {
        if (text_has_uppercase(options->patterns[i])) {
            options->ignore_case = 0;
            return;
        }
    }
}

static const char *relative_to_base(const char *base, const char *path) {
    size_t base_length;

    if (base == 0 || base[0] == '\0' || rt_strcmp(base, ".") == 0) return path;
    base_length = rt_strlen(base);
    if (rt_strncmp(path, base, base_length) != 0) return 0;
    if (path[base_length] == '\0') return path + base_length;
    if (path[base_length] != '/') return 0;
    return path + base_length + 1U;
}

static int ignore_pattern_matches(const RgIgnorePattern *pattern, const char *path, int is_directory) {
    const char *relative = relative_to_base(pattern->base, path);

    if (relative == 0) return 0;
    if (pattern->directory_only && !is_directory) return 0;
    if (has_path_separator(pattern->pattern)) {
        return tool_wildcard_match(pattern->pattern, relative);
    }
    return tool_wildcard_match(pattern->pattern, path_base_name(path));
}

static int path_is_ignored(const RgOptions *options, const char *path, int is_directory) {
    int ignored = 0;
    int i;

    if (!options->use_ignore_files) return 0;
    for (i = 0; i < options->ignore_pattern_count; ++i) {
        if (ignore_pattern_matches(&options->ignore_patterns[i], path, is_directory)) {
            ignored = !options->ignore_patterns[i].negated;
        }
    }
    return ignored;
}

static void trim_ignore_line(char *line) {
    size_t start = 0U;
    size_t end;
    size_t index;

    while (line[start] == ' ' || line[start] == '\t' || line[start] == '\r') start += 1U;
    if (start > 0U) {
        index = 0U;
        while (line[start] != '\0') line[index++] = line[start++];
        line[index] = '\0';
    }
    end = rt_strlen(line);
    while (end > 0U && (line[end - 1U] == ' ' || line[end - 1U] == '\t' || line[end - 1U] == '\r')) {
        line[--end] = '\0';
    }
}

static int add_ignore_pattern(RgOptions *options, const char *base, const char *line) {
    RgIgnorePattern *entry;
    const char *pattern = line;
    size_t length;

    if (options->ignore_pattern_count >= options->ignore_pattern_capacity) {
        if (!options->no_messages) tool_write_error("ripgrep", "too many ignore patterns", "");
        return -1;
    }
    if (pattern == 0 || pattern[0] == '\0' || pattern[0] == '#') return 0;
    entry = &options->ignore_patterns[options->ignore_pattern_count];
    rt_memset(entry, 0, sizeof(*entry));
    rt_copy_string(entry->base, sizeof(entry->base), base != 0 ? base : ".");
    if (pattern[0] == '!') {
        entry->negated = 1;
        pattern += 1;
    }
    if (pattern[0] == '/') pattern += 1;
    rt_copy_string(entry->pattern, sizeof(entry->pattern), pattern);
    length = rt_strlen(entry->pattern);
    if (length > 0U && entry->pattern[length - 1U] == '/') {
        entry->directory_only = 1;
        entry->pattern[length - 1U] = '\0';
    }
    if (entry->pattern[0] == '\0') return 0;
    options->ignore_pattern_count += 1;
    return 0;
}

static int process_ignore_line(RgOptions *options, const char *base, char *line) {
    trim_ignore_line(line);
    return add_ignore_pattern(options, base, line);
}

static int load_ignore_file(RgOptions *options, const char *path, const char *base, int required) {
    char chunk[512];
    char line[RG_PATH_CAPACITY];
    size_t line_length = 0U;
    long bytes_read;
    int fd;
    int should_close;

    if (!options->use_ignore_files) return 0;
    if (tool_open_input(path, &fd, &should_close) != 0) {
        if (required && !options->no_messages) tool_write_error("ripgrep", "cannot open ignore file ", path);
        return required ? -1 : 0;
    }
    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;
        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];
            if (ch == '\n') {
                line[line_length] = '\0';
                if (process_ignore_line(options, base, line) != 0) {
                    tool_close_input(fd, should_close);
                    return -1;
                }
                line_length = 0U;
            } else if (line_length + 1U < sizeof(line)) {
                line[line_length++] = ch;
            }
        }
    }
    if (line_length > 0U) {
        line[line_length] = '\0';
        if (process_ignore_line(options, base, line) != 0) {
            tool_close_input(fd, should_close);
            return -1;
        }
    }
    tool_close_input(fd, should_close);
    if (bytes_read < 0) {
        if (!options->no_messages) tool_write_error("ripgrep", "cannot read ignore file ", path);
        return -1;
    }
    return 0;
}

static int load_directory_ignore_files(RgOptions *options, const char *dir_path) {
    char path[RG_PATH_CAPACITY];

    if (tool_join_path(dir_path, ".gitignore", path, sizeof(path)) == 0) {
        if (load_ignore_file(options, path, dir_path, 0) != 0) return -1;
    }
    if (tool_join_path(dir_path, ".ignore", path, sizeof(path)) == 0) {
        if (load_ignore_file(options, path, dir_path, 0) != 0) return -1;
    }
    if (tool_join_path(dir_path, ".rgignore", path, sizeof(path)) == 0) {
        if (load_ignore_file(options, path, dir_path, 0) != 0) return -1;
    }
    return 0;
}

static int starts_with_literal(const char *pattern, const char *text, int ignore_case, size_t *consumed_out) {
    size_t pattern_len = rt_strlen(pattern);
    size_t text_len = rt_strlen(text);
    size_t pi = 0U;
    size_t ti = 0U;

    while (pi < pattern_len) {
        unsigned int lhs = 0;
        unsigned int rhs = 0;
        unsigned char pattern_ch;
        unsigned char text_ch;

        if (ti >= text_len) {
            return 0;
        }
        pattern_ch = (unsigned char)pattern[pi];
        text_ch = (unsigned char)text[ti];
        if (pattern_ch < 0x80U && text_ch < 0x80U) {
            if (ignore_case) {
                if (pattern_ch >= 'A' && pattern_ch <= 'Z') pattern_ch = (unsigned char)(pattern_ch + ('a' - 'A'));
                if (text_ch >= 'A' && text_ch <= 'Z') text_ch = (unsigned char)(text_ch + ('a' - 'A'));
            }
            if (pattern_ch != text_ch) {
                return 0;
            }
            pi += 1U;
            ti += 1U;
            continue;
        }
        if (rt_utf8_decode(pattern, pattern_len, &pi, &lhs) != 0 ||
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

static int ascii_is_word_byte(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
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
        if (((unsigned char)text[prev]) < 0x80U) {
            if (ascii_is_word_byte((unsigned char)text[prev])) {
                return 0;
            }
        } else if (rt_utf8_decode(text, length, &index, &codepoint) == 0 && rt_unicode_is_word(codepoint)) {
            return 0;
        }
    }
    if (end < length) {
        size_t index = end;
        unsigned int codepoint = 0;
        if (((unsigned char)text[end]) < 0x80U) {
            if (ascii_is_word_byte((unsigned char)text[end])) {
                return 0;
            }
        } else if (rt_utf8_decode(text, length, &index, &codepoint) == 0 && rt_unicode_is_word(codepoint)) {
            return 0;
        }
    }
    return 1;
}

static int find_next_match_for_pattern(const RgOptions *options,
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

static int find_next_match(const RgOptions *options,
                           const char *text,
                           size_t search_start,
                           size_t *start_out,
                           size_t *end_out) {
    size_t best_start = 0U;
    size_t best_end = 0U;
    int found_any = 0;
    int i;

    for (i = 0; i < options->pattern_count; ++i) {
        size_t start = 0U;
        size_t end = 0U;
        if (find_next_match_for_pattern(options, options->patterns[i], text, search_start, &start, &end)) {
            if (!found_any || start < best_start || (start == best_start && end > best_end)) {
                best_start = start;
                best_end = end;
                found_any = 1;
            }
        }
    }
    if (!found_any) return 0;
    *start_out = best_start;
    *end_out = best_end;
    return 1;
}

static unsigned long long count_line_matches(const RgOptions *options, const char *line) {
    size_t search_start = 0U;
    unsigned long long count = 0ULL;

    while (1) {
        size_t start = 0U;
        size_t end = 0U;
        if (!find_next_match(options, line, search_start, &start, &end)) break;
        count += 1ULL;
        if (end == start) {
            if (line[end] == '\0') break;
            search_start = end + 1U;
        } else {
            search_start = end;
        }
    }
    return count;
}

static int rg_use_color(const RgOptions *options) {
    return tool_should_use_color_fd(1, options->color_mode);
}

static int rg_write_uint_buffered(ToolOutputBuffer *output, unsigned long long value) {
    char digits[32];

    rt_unsigned_to_string(value, digits, sizeof(digits));
    return tool_output_buffer_write_cstr(output, digits);
}

static int rg_can_use_simple_output(const RgOptions *options, int show_path) {
    return !show_path &&
           !options->column_numbers &&
           !options->invert_match &&
           !options->count_only &&
           !options->quiet &&
           !options->files_with_matches &&
           !options->files_without_match &&
           !options->only_matching &&
           !options->heading &&
           !options->stats &&
           !options->has_max_count &&
           !rg_use_color(options);
}

static int rg_write_simple_line(ToolOutputBuffer *output, const RgOptions *options, unsigned long long line_no, const char *line) {
    if (options->line_numbers &&
        (rg_write_uint_buffered(output, line_no) != 0 || tool_output_buffer_write_char(output, ':') != 0)) {
        return -1;
    }
    return tool_output_buffer_write_cstr(output, line) != 0 ||
           tool_output_buffer_write_char(output, '\n') != 0 ? -1 : 0;
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

static int write_highlighted_line(const RgOptions *options, const char *line) {
    size_t search_start = 0U;
    size_t rendered = 0U;

    while (1) {
        size_t start = 0U;
        size_t end = 0U;

        if (!find_next_match(options, line, search_start, &start, &end)) {
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
                        unsigned long long line_no,
                        size_t column) {
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
    if (options->column_numbers) {
        if (rg_use_color(options)) {
            tool_style_begin(1, options->color_mode, TOOL_STYLE_BOLD_GREEN);
        }
        if (rt_write_uint(1, (unsigned long long)column + 1ULL) != 0) {
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
                            const char *path,
                            int show_path,
                            unsigned long long line_no,
                            size_t column,
                            const char *line) {
    if (print_prefix(options, path, show_path, line_no, column) != 0) {
        return -1;
    }
    if (!rg_use_color(options) || options->invert_match) {
        return rt_write_line(1, line);
    }
    return write_highlighted_line(options, line);
}

static int print_heading_if_needed(const char *path, int show_path, int *heading_printed) {
    if (!show_path || heading_printed == 0 || *heading_printed) return 0;
    if (rt_write_line(1, path) != 0) return -1;
    *heading_printed = 1;
    return 0;
}

static int print_only_matching_line(const RgOptions *options,
                                    const char *path,
                                    int show_path,
                                    unsigned long long line_no,
                                    const char *line,
                                    int *heading_printed) {
    size_t search_start = 0U;

    while (1) {
        size_t start = 0U;
        size_t end = 0U;
        if (!find_next_match(options, line, search_start, &start, &end)) break;
        if (print_heading_if_needed(path, show_path && options->heading && !options->no_heading, heading_printed) != 0) return -1;
        if (print_prefix(options, path, show_path && (!options->heading || options->no_heading), line_no, start) != 0) return -1;
        if (rt_write_all(1, line + start, end - start) != 0 || rt_write_char(1, '\n') != 0) return -1;
        if (end == start) {
            if (line[end] == '\0') break;
            search_start = end + 1U;
        } else {
            search_start = end;
        }
    }
    return 0;
}

static int process_rg_line(RgOptions *options,
                           const char *path,
                           int show_path,
                           unsigned long long line_no,
                           const char *line,
                           unsigned long long *match_count_io,
                           int *matched_io,
                           int *heading_printed,
                           int *stop_out) {
    size_t start = 0U;
    size_t end = 0U;
    unsigned long long occurrences = 0ULL;
    int line_matches = find_next_match(options, line, 0U, &start, &end);

    if (options->invert_match) {
        line_matches = !line_matches;
        occurrences = line_matches ? 1ULL : 0ULL;
        start = 0U;
    } else if (line_matches) {
        occurrences = options->stats ? count_line_matches(options, line) : 1ULL;
    }
    if (!line_matches) return 0;

    *matched_io = 1;
    *match_count_io += 1ULL;
    options->stats_matched_lines += 1ULL;
    options->stats_matches += occurrences;
    if (options->quiet) {
        *stop_out = 1;
        return 0;
    }
    if (options->has_max_count && *match_count_io > options->max_count) {
        *stop_out = 1;
        return 0;
    }
    if (!options->count_only && !options->files_with_matches && !options->files_without_match) {
        int heading_mode = show_path && options->heading && !options->no_heading;
        if (print_heading_if_needed(path, heading_mode, heading_printed) != 0) return -1;
        if (options->only_matching && !options->invert_match) {
            if (print_only_matching_line(options, path, show_path, line_no, line, heading_printed) != 0) return -1;
        } else if (print_match_line(options, path, show_path && !heading_mode, line_no, start, line) != 0) {
            return -1;
        }
    }
    if (options->has_max_count && *match_count_io >= options->max_count) *stop_out = 1;
    return 0;
}

static int rg_stream_file(int fd,
                          const char *path,
                          RgOptions *options,
                          int show_path,
                          int *matched_out) {
    char chunk[16384];
    char line[RG_LINE_CAPACITY];
    ToolOutputBuffer simple_output;
    size_t line_len = 0U;
    unsigned long long line_no = 1ULL;
    unsigned long long match_count = 0ULL;
    int heading_printed = 0;
    int matched = 0;
    int stop = 0;
    long bytes_read;

    if (rg_can_use_simple_output(options, show_path)) {
        tool_output_buffer_init(&simple_output, 1);
        while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
            long i;

            for (i = 0; i < bytes_read; ++i) {
                char ch = chunk[i];
                if (ch == '\0') {
                    if (matched_out != 0) *matched_out = 0;
                    return 0;
                }
                if (ch == '\n') {
                    size_t start = 0U;
                    size_t end = 0U;

                    line[line_len] = '\0';
                    if (find_next_match(options, line, 0U, &start, &end)) {
                        matched = 1;
                        if (rg_write_simple_line(&simple_output, options, line_no, line) != 0) {
                            return -1;
                        }
                    }
                    line_len = 0U;
                    line_no += 1ULL;
                } else if (line_len + 1U < sizeof(line)) {
                    line[line_len++] = ch;
                }
            }
        }
        if (bytes_read < 0) return -1;
        if (line_len > 0U) {
            size_t start = 0U;
            size_t end = 0U;

            line[line_len] = '\0';
            if (find_next_match(options, line, 0U, &start, &end)) {
                matched = 1;
                if (rg_write_simple_line(&simple_output, options, line_no, line) != 0) {
                    return -1;
                }
            }
        }
        if (tool_output_buffer_flush(&simple_output) != 0) return -1;
        if (matched_out != 0) *matched_out = matched;
        return 0;
    }

    while (!stop && (bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;
        for (i = 0; i < bytes_read && !stop; ++i) {
            char ch = chunk[i];
            if (ch == '\0') {
                if (matched_out != 0) {
                    *matched_out = 0;
                }
                return 0;
            }
            if (ch == '\n') {
                line[line_len] = '\0';
                if (process_rg_line(options, path, show_path, line_no, line, &match_count, &matched, &heading_printed, &stop) != 0) return -1;
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
    if (!stop && line_len > 0U) {
        line[line_len] = '\0';
        if (process_rg_line(options, path, show_path, line_no, line, &match_count, &matched, &heading_printed, &stop) != 0) return -1;
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
                          RgOptions *options,
                          int multiple_roots,
                          int depth,
                          int *matched_out,
                          int *error_out);

static int rg_search_file(const char *path,
                          RgOptions *options,
                          int show_path,
                          int *matched_out,
                          int *error_out) {
    int fd;
    int should_close;
    int matched = 0;

    if (path_is_ignored(options, path, 0) || !should_search_file(options, path)) {
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
        if (!options->no_messages) tool_write_error("ripgrep", "cannot open ", path);
        *error_out = 1;
        return -1;
    }
    options->stats_files_searched += 1ULL;
    if (rg_stream_file(fd, path, options, show_path, &matched) != 0) {
        if (!options->no_messages) tool_write_error("ripgrep", "read error on ", path);
        tool_close_input(fd, should_close);
        *error_out = 1;
        return -1;
    }
    tool_close_input(fd, should_close);
    if (matched) options->stats_files_with_matches += 1ULL;

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
                               RgOptions *options,
                               int depth,
                               int *matched_out,
                               int *error_out) {
    PlatformDirEntry entries[RG_ENTRY_CAPACITY];
    size_t count = 0U;
    size_t i;
    int is_directory = 0;
    int any_match = 0;

    int ignore_mark = options->ignore_pattern_count;

    if (options->max_depth >= 0 && depth > options->max_depth) {
        if (matched_out != 0) *matched_out = 0;
        return 0;
    }
    if (path_is_ignored(options, path, 1) || path_excluded_by_negative_glob(options, path)) {
        if (matched_out != 0) *matched_out = 0;
        return 0;
    }
    if (load_directory_ignore_files(options, path) != 0) {
        *error_out = 1;
        return -1;
    }
    if (platform_collect_entries(path, 1, entries, RG_ENTRY_CAPACITY, &count, &is_directory) != 0 || !is_directory) {
        if (!options->no_messages) tool_write_error("ripgrep", "cannot read directory ", path);
        *error_out = 1;
        options->ignore_pattern_count = ignore_mark;
        return -1;
    }
    if (options->sort_path) rt_sort(entries, count, sizeof(entries[0]), compare_dir_entries_by_name);

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
            if (!options->no_messages) tool_write_error("ripgrep", "path too long under ", path);
            *error_out = 1;
            continue;
        }
        if (path_is_ignored(options, child_path, entries[i].is_dir)) {
            continue;
        }
        if (rg_search_path(child_path, options, 1, depth + 1, &child_matched, error_out) != 0) {
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
    options->ignore_pattern_count = ignore_mark;
    if (matched_out != 0) {
        *matched_out = any_match;
    }
    return *error_out ? -1 : 0;
}

static int rg_search_path(const char *path,
                          RgOptions *options,
                          int multiple_roots,
                          int depth,
                          int *matched_out,
                          int *error_out) {
    int is_directory = 0;

    if (path_is_directory_for_options(options, path, &is_directory) == 0 && is_directory) {
        return rg_search_directory(path, options, depth, matched_out, error_out);
    }

    return rg_search_file(path, options, !options->no_filename && (options->with_filename || multiple_roots), matched_out, error_out);
}

static int add_glob(RgOptions *options, const char *glob) {
    if (options->glob_count >= RG_MAX_GLOBS) {
        tool_write_error("ripgrep", "too many glob filters", "");
        return -1;
    }
    options->globs[options->glob_count++] = glob;
    return 0;
}

static int add_exclude_glob(RgOptions *options, const char *glob) {
    char *slot;

    if (options->glob_count >= RG_MAX_GLOBS) {
        tool_write_error("ripgrep", "too many glob filters", "");
        return -1;
    }
    slot = options->glob_storage[options->glob_count];
    slot[0] = '!';
    rt_copy_string(slot + 1, RG_PATH_CAPACITY - 1U, glob);
    options->globs[options->glob_count++] = slot;
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

static int add_type_not(RgOptions *options, const char *type_name) {
    if (options->type_not_count >= RG_MAX_TYPES) {
        tool_write_error("ripgrep", "too many excluded type filters", "");
        return -1;
    }
    options->type_not_filters[options->type_not_count++] = type_name;
    return 0;
}

static int add_pattern(RgOptions *options, const char *pattern) {
    if (options->pattern_count >= RG_MAX_PATTERNS) {
        tool_write_error("ripgrep", "too many patterns", "");
        return -1;
    }
    options->patterns[options->pattern_count++] = pattern;
    return 0;
}

static void write_type_list(void) {
    rt_write_line(1, "c: *.c, *.h");
    rt_write_line(1, "cpp: *.cc, *.cpp, *.cxx, *.hh, *.hpp, *.hxx");
    rt_write_line(1, "md: *.md, *.markdown");
    rt_write_line(1, "sh: *.sh, *.bash");
    rt_write_line(1, "text: *.txt, *.text, *.md, *.markdown");
    rt_write_line(1, "unknown names: treated as literal file extensions");
}

static int parse_count_value(const char *text, unsigned long long *value_out) {
    return text != 0 && rt_parse_uint(text, value_out) == 0 ? 0 : -1;
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
    if (rt_strcmp(flag, "--ignore") == 0) {
        options->use_ignore_files = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--no-ignore") == 0) {
        options->use_ignore_files = 0;
        return 0;
    }
    if (rt_strcmp(flag, "--ignore-file") == 0) {
        if (tool_opt_require_value(s) != 0) return -1;
        return load_ignore_file(options, s->value, ".", 1);
    }
    if (rt_strncmp(flag, "--ignore-file=", 14U) == 0) {
        return load_ignore_file(options, flag + 14, ".", 1);
    }
    if (rt_strcmp(flag, "--ignore-case") == 0) {
        options->ignore_case = 1;
        options->smart_case = 0;
        return 0;
    }
    if (rt_strcmp(flag, "--case-sensitive") == 0) {
        options->ignore_case = 0;
        options->smart_case = 0;
        return 0;
    }
    if (rt_strcmp(flag, "--smart-case") == 0) {
        options->smart_case = 1;
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
    if (rt_strcmp(flag, "--column") == 0) {
        options->column_numbers = 1;
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
    if (rt_strcmp(flag, "--only-matching") == 0) {
        options->only_matching = 1;
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
    if (rt_strcmp(flag, "--heading") == 0) {
        options->heading = 1;
        options->no_heading = 0;
        return 0;
    }
    if (rt_strcmp(flag, "--no-heading") == 0) {
        options->no_heading = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--no-messages") == 0) {
        options->no_messages = 1;
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
    if (rt_strcmp(flag, "--include") == 0) {
        if (tool_opt_require_value(s) != 0) return -1;
        return add_glob(options, s->value);
    }
    if (rt_strncmp(flag, "--include=", 10U) == 0) {
        return add_glob(options, flag + 10);
    }
    if (rt_strcmp(flag, "--exclude") == 0) {
        if (tool_opt_require_value(s) != 0) return -1;
        return add_exclude_glob(options, s->value);
    }
    if (rt_strncmp(flag, "--exclude=", 10U) == 0) {
        return add_exclude_glob(options, flag + 10);
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
    if (rt_strcmp(flag, "--type-not") == 0) {
        if (tool_opt_require_value(s) != 0) return -1;
        return add_type_not(options, s->value);
    }
    if (rt_strncmp(flag, "--type-not=", 11U) == 0) {
        return add_type_not(options, flag + 11);
    }
    if (rt_strcmp(flag, "--type-list") == 0) {
        options->type_list = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--max-count") == 0) {
        if (tool_opt_require_value(s) != 0 || parse_count_value(s->value, &options->max_count) != 0) return -1;
        options->has_max_count = 1;
        return 0;
    }
    if (rt_strncmp(flag, "--max-count=", 12U) == 0) {
        if (parse_count_value(flag + 12, &options->max_count) != 0) return -1;
        options->has_max_count = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--max-depth") == 0) {
        unsigned long long value;
        if (tool_opt_require_value(s) != 0 || parse_count_value(s->value, &value) != 0 || value > 1024ULL) return -1;
        options->max_depth = (int)value;
        return 0;
    }
    if (rt_strncmp(flag, "--max-depth=", 12U) == 0) {
        unsigned long long value;
        if (parse_count_value(flag + 12, &value) != 0 || value > 1024ULL) return -1;
        options->max_depth = (int)value;
        return 0;
    }
    if (rt_strcmp(flag, "--follow") == 0) {
        options->follow_symlinks = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--sort") == 0) {
        if (tool_opt_require_value(s) != 0) return -1;
        if (rt_strcmp(s->value, "path") != 0) return -1;
        options->sort_path = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--sort=path") == 0) {
        options->sort_path = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--stats") == 0) {
        options->stats = 1;
        return 0;
    }
    if (rt_strcmp(flag, "--regexp") == 0) {
        if (tool_opt_require_value(s) != 0) return -1;
        return add_pattern(options, s->value);
    }
    if (rt_strncmp(flag, "--regexp=", 9U) == 0) {
        return add_pattern(options, flag + 9);
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
            options->smart_case = 0;
        } else if (*flag == 's') {
            options->ignore_case = 0;
            options->smart_case = 0;
        } else if (*flag == 'S') {
            options->smart_case = 1;
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
        } else if (*flag == 'o') {
            options->only_matching = 1;
        } else if (*flag == 'g' || *flag == 't' || *flag == 'm' || *flag == 'e') {
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
            if (*flag == 't') {
                return add_type(options, value);
            }
            if (*flag == 'm') {
                if (parse_count_value(value, &options->max_count) != 0) return -1;
                options->has_max_count = 1;
                return 0;
            }
            return add_pattern(options, value);
        } else {
            return -1;
        }
        flag += 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    RgOptions options;
    RgIgnorePattern ignore_patterns[RG_MAX_IGNORE_PATTERNS];
    ToolOptState s;
    int r;
    int path_start;
    int path_count;
    int i;
    int any_match = 0;
    int had_error = 0;

    rt_memset(&options, 0, sizeof(options));
    options.color_mode = TOOL_COLOR_AUTO;
    options.line_numbers = 1;
    options.use_ignore_files = 1;
    options.max_depth = -1;
    options.ignore_patterns = ignore_patterns;
    options.ignore_pattern_capacity = RG_MAX_IGNORE_PATTERNS;

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

    if (options.type_list) {
        write_type_list();
        return 0;
    }

    if (options.quiet) {
        options.count_only = 0;
        options.files_with_matches = 0;
        options.files_without_match = 0;
    }
    if (options.files_mode) {
        path_start = s.argi;
    } else {
        if (options.pattern_count == 0) {
            if (s.argi >= argc) {
                print_usage(argv[0]);
                return 2;
            }
            if (add_pattern(&options, argv[s.argi++]) != 0) return 2;
        }
        if (options.pattern_count == 0) {
            print_usage(argv[0]);
            return 2;
        }
        path_start = s.argi;
        apply_smart_case(&options);
    }

    path_count = argc - path_start;
    if (path_count <= 0) {
        int matched = 0;
        if (options.files_mode) {
            if (rg_search_path(".", &options, 1, 0, &matched, &had_error) != 0 && had_error) {
                return 2;
            }
            return 0;
        }
        if (rg_search_path(".", &options, 1, 0, &matched, &had_error) != 0 && had_error) {
            return 2;
        }
        if (options.stats) {
            rt_write_cstr(1, "files searched: "); rt_write_uint(1, options.stats_files_searched); rt_write_char(1, '\n');
            rt_write_cstr(1, "files with matches: "); rt_write_uint(1, options.stats_files_with_matches); rt_write_char(1, '\n');
            rt_write_cstr(1, "matching lines: "); rt_write_uint(1, options.stats_matched_lines); rt_write_char(1, '\n');
            rt_write_cstr(1, "matches: "); rt_write_uint(1, options.stats_matches); rt_write_char(1, '\n');
        }
        return matched ? 0 : 1;
    }

    for (i = path_start; i < argc; ++i) {
        int matched = 0;
        if (rg_search_path(argv[i], &options, path_count > 1, 0, &matched, &had_error) != 0 && options.quiet && matched) {
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
    if (options.stats) {
        rt_write_cstr(1, "files searched: "); rt_write_uint(1, options.stats_files_searched); rt_write_char(1, '\n');
        rt_write_cstr(1, "files with matches: "); rt_write_uint(1, options.stats_files_with_matches); rt_write_char(1, '\n');
        rt_write_cstr(1, "matching lines: "); rt_write_uint(1, options.stats_matched_lines); rt_write_char(1, '\n');
        rt_write_cstr(1, "matches: "); rt_write_uint(1, options.stats_matches); rt_write_char(1, '\n');
    }
    return any_match ? 0 : 1;
}