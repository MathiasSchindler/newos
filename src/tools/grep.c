#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define GREP_ENTRY_CAPACITY 1024
#define GREP_LINE_CAPACITY 8192
#define GREP_PATH_CAPACITY 1024

typedef struct {
    int show_line_no;
    int ignore_case;
    int invert_match;
    int recursive;
    int count_only;
    int quiet;
    int list_files;
    int fixed_strings;
    int only_matching;
} GrepOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-EinvrcqloF] PATTERN [file ...]");
}

static int print_count(const char *label, int show_label, unsigned long long count) {
    if (show_label) {
        if (rt_write_cstr(1, label) != 0 || rt_write_char(1, ':') != 0) {
            return -1;
        }
    }

    if (rt_write_uint(1, count) != 0 || rt_write_char(1, '\n') != 0) {
        return -1;
    }

    return 0;
}

static char to_lower_ascii(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int same_char(char lhs, char rhs, int ignore_case) {
    if (lhs == '.') {
        return 1;
    }
    if (ignore_case) {
        return to_lower_ascii(lhs) == to_lower_ascii(rhs);
    }
    return lhs == rhs;
}

static int match_here_span(const char *pattern, const char *text, int ignore_case, const char **end_out);

static int match_star_span(int token,
                           const char *pattern,
                           const char *text,
                           int ignore_case,
                           const char **end_out) {
    const char *cursor = text;

    while (*cursor != '\0' && (token == '.' || same_char((char)token, *cursor, ignore_case))) {
        cursor += 1;
    }

    do {
        if (match_here_span(pattern, cursor, ignore_case, end_out)) {
            return 1;
        }
    } while (cursor-- > text);

    return 0;
}

static int match_here_span(const char *pattern, const char *text, int ignore_case, const char **end_out) {
    if (pattern[0] == '\0') {
        *end_out = text;
        return 1;
    }

    if (pattern[1] == '*') {
        return match_star_span(pattern[0], pattern + 2, text, ignore_case, end_out);
    }

    if (pattern[0] == '$' && pattern[1] == '\0') {
        if (text[0] == '\0') {
            *end_out = text;
            return 1;
        }
        return 0;
    }

    if (text[0] != '\0' && same_char(pattern[0], text[0], ignore_case)) {
        return match_here_span(pattern + 1, text + 1, ignore_case, end_out);
    }

    return 0;
}

static int starts_with_literal(const char *pattern, const char *text, int ignore_case) {
    size_t i = 0;

    while (pattern[i] != '\0') {
        char lhs = pattern[i];
        char rhs = text[i];

        if (rhs == '\0') {
            return 0;
        }
        if (ignore_case) {
            lhs = to_lower_ascii(lhs);
            rhs = to_lower_ascii(rhs);
        }
        if (lhs != rhs) {
            return 0;
        }
        i += 1;
    }

    return 1;
}

static int find_fixed_match(const char *pattern,
                            const char *text,
                            int ignore_case,
                            size_t search_start,
                            size_t *start_out,
                            size_t *end_out) {
    size_t pattern_len = rt_strlen(pattern);
    size_t pos = search_start;

    if (pattern_len == 0) {
        *start_out = search_start;
        *end_out = search_start;
        return 1;
    }

    while (1) {
        if (starts_with_literal(pattern, text + pos, ignore_case)) {
            *start_out = pos;
            *end_out = pos + pattern_len;
            return 1;
        }

        if (text[pos] == '\0') {
            break;
        }
        pos += 1;
    }

    return 0;
}

static int find_regex_match(const char *pattern,
                            const char *text,
                            int ignore_case,
                            size_t search_start,
                            size_t *start_out,
                            size_t *end_out) {
    size_t pos = search_start;

    if (pattern[0] == '^') {
        const char *end = 0;

        if (search_start != 0) {
            return 0;
        }
        if (match_here_span(pattern + 1, text, ignore_case, &end)) {
            *start_out = 0;
            *end_out = (size_t)(end - text);
            return 1;
        }
        return 0;
    }

    while (1) {
        const char *end = 0;

        if (match_here_span(pattern, text + pos, ignore_case, &end)) {
            *start_out = pos;
            *end_out = (size_t)(end - text);
            return 1;
        }

        if (text[pos] == '\0') {
            break;
        }
        pos += 1;
    }

    return 0;
}

static int find_next_match(const GrepOptions *options,
                           const char *pattern,
                           const char *text,
                           size_t search_start,
                           size_t *start_out,
                           size_t *end_out) {
    if (options->fixed_strings) {
        return find_fixed_match(pattern, text, options->ignore_case, search_start, start_out, end_out);
    }
    return find_regex_match(pattern, text, options->ignore_case, search_start, start_out, end_out);
}

static int print_prefix(const char *label, int show_label, unsigned long long line_no, int show_line_no) {
    if (show_label) {
        if (rt_write_cstr(1, label) != 0 || rt_write_char(1, ':') != 0) {
            return -1;
        }
    }

    if (show_line_no) {
        if (rt_write_uint(1, line_no) != 0 || rt_write_char(1, ':') != 0) {
            return -1;
        }
    }

    return 0;
}

static int print_match_line(const char *label,
                            int show_label,
                            unsigned long long line_no,
                            int show_line_no,
                            const char *line) {
    if (print_prefix(label, show_label, line_no, show_line_no) != 0) {
        return -1;
    }
    return rt_write_line(1, line);
}

static int print_match_fragment(const char *label,
                                int show_label,
                                unsigned long long line_no,
                                int show_line_no,
                                const char *line,
                                size_t start,
                                size_t end) {
    if (print_prefix(label, show_label, line_no, show_line_no) != 0) {
        return -1;
    }
    if (end > start && rt_write_all(1, line + start, end - start) != 0) {
        return -1;
    }
    return rt_write_char(1, '\n');
}

static int emit_only_matches(const char *pattern,
                             const GrepOptions *options,
                             const char *label,
                             int show_label,
                             unsigned long long line_no,
                             const char *line) {
    size_t search_start = 0;
    int printed = 0;

    while (1) {
        size_t start = 0;
        size_t end = 0;

        if (!find_next_match(options, pattern, line, search_start, &start, &end)) {
            break;
        }

        if (start == end) {
            if (!printed && print_match_fragment(label, show_label, line_no, options->show_line_no, line, start, end) != 0) {
                return -1;
            }
            printed = 1;
            if (line[start] == '\0') {
                break;
            }
            search_start = start + 1;
            continue;
        }

        if (print_match_fragment(label, show_label, line_no, options->show_line_no, line, start, end) != 0) {
            return -1;
        }
        printed = 1;
        search_start = end;
    }

    return 0;
}

static int grep_stream(int fd,
                       const char *pattern,
                       const GrepOptions *options,
                       const char *label,
                       int show_label,
                       int *matched_out) {
    char chunk[4096];
    char line[GREP_LINE_CAPACITY];
    size_t line_len = 0;
    unsigned long long line_no = 1;
    unsigned long long match_count = 0;
    int matched = 0;
    int listed = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                int line_matches;
                size_t match_start = 0;
                size_t match_end = 0;

                line[line_len] = '\0';
                line_matches = find_next_match(options, pattern, line, 0, &match_start, &match_end);
                if (options->invert_match) {
                    line_matches = !line_matches;
                }

                if (line_matches) {
                    matched = 1;
                    match_count += 1ULL;
                    if (options->quiet) {
                        if (matched_out != 0) {
                            *matched_out = matched;
                        }
                        return 0;
                    }
                    if (options->list_files) {
                        if (!listed) {
                            if (rt_write_line(1, label) != 0) {
                                return -1;
                            }
                            listed = 1;
                        }
                    } else if (!options->count_only) {
                        if (options->only_matching && !options->invert_match) {
                            if (emit_only_matches(pattern, options, label, show_label, line_no, line) != 0) {
                                return -1;
                            }
                        } else if (!options->only_matching) {
                            if (print_match_line(label, show_label, line_no, options->show_line_no, line) != 0) {
                                return -1;
                            }
                        }
                    }
                }

                line_len = 0;
                line_no += 1;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (line_len > 0) {
        int line_matches;
        size_t match_start = 0;
        size_t match_end = 0;

        line[line_len] = '\0';
        line_matches = find_next_match(options, pattern, line, 0, &match_start, &match_end);
        if (options->invert_match) {
            line_matches = !line_matches;
        }

        if (line_matches) {
            matched = 1;
            match_count += 1ULL;
            if (options->quiet) {
                if (matched_out != 0) {
                    *matched_out = matched;
                }
                return 0;
            }
            if (options->list_files) {
                if (!listed) {
                    if (rt_write_line(1, label) != 0) {
                        return -1;
                    }
                    listed = 1;
                }
            } else if (!options->count_only) {
                if (options->only_matching && !options->invert_match) {
                    if (emit_only_matches(pattern, options, label, show_label, line_no, line) != 0) {
                        return -1;
                    }
                } else if (!options->only_matching) {
                    if (print_match_line(label, show_label, line_no, options->show_line_no, line) != 0) {
                        return -1;
                    }
                }
            }
        }
    }

    if (options->count_only && !options->quiet && !options->list_files) {
        if (print_count(label, show_label, match_count) != 0) {
            return -1;
        }
    }

    if (matched_out != 0) {
        *matched_out = matched;
    }

    return 0;
}

static int grep_path(const char *path,
                     const char *pattern,
                     const GrepOptions *options,
                     int show_label,
                     int *matched_out) {
    int is_directory = 0;

    if (platform_path_is_directory(path, &is_directory) == 0 && is_directory) {
        size_t count = 0;
        size_t i;
        PlatformDirEntry entries[GREP_ENTRY_CAPACITY];
        int path_is_directory = 0;
        int matched = 0;
        int result = 0;

        if (!options->recursive) {
            rt_write_cstr(2, "grep: ");
            rt_write_cstr(2, path);
            rt_write_line(2, ": is a directory");
            return -1;
        }

        if (platform_collect_entries(path, 1, entries, GREP_ENTRY_CAPACITY, &count, &path_is_directory) != 0 ||
            !path_is_directory) {
            return -1;
        }

        for (i = 0; i < count; ++i) {
            char child_path[GREP_PATH_CAPACITY];
            int child_matched = 0;

            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
                continue;
            }

            if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0 ||
                grep_path(child_path, pattern, options, 1, &child_matched) != 0) {
                result = -1;
                break;
            }

            if (child_matched) {
                matched = 1;
            }
        }

        platform_free_entries(entries, count);

        if (matched_out != 0) {
            *matched_out = matched;
        }

        return result;
    }

    {
        int fd;
        int should_close;
        int matched = 0;

        if (tool_open_input(path, &fd, &should_close) != 0) {
            rt_write_cstr(2, "grep: cannot open ");
            rt_write_line(2, path);
            return -1;
        }

        if (grep_stream(fd, pattern, options, path, show_label || options->recursive, &matched) != 0) {
            tool_close_input(fd, should_close);
            return -1;
        }

        tool_close_input(fd, should_close);

        if (matched_out != 0) {
            *matched_out = matched;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    GrepOptions options;
    int arg_index = 1;
    int file_count;
    int i;
    int exit_code = 0;
    int any_match = 0;

    rt_memset(&options, 0, sizeof(options));

    while (arg_index < argc && argv[arg_index][0] == '-' && argv[arg_index][1] != '\0') {
        const char *flag = argv[arg_index] + 1;

        if (rt_strcmp(argv[arg_index], "--") == 0) {
            arg_index += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 'n') {
                options.show_line_no = 1;
            } else if (*flag == 'i') {
                options.ignore_case = 1;
            } else if (*flag == 'v') {
                options.invert_match = 1;
            } else if (*flag == 'r' || *flag == 'R') {
                options.recursive = 1;
            } else if (*flag == 'c') {
                options.count_only = 1;
            } else if (*flag == 'q') {
                options.quiet = 1;
            } else if (*flag == 'l') {
                options.list_files = 1;
            } else if (*flag == 'F') {
                options.fixed_strings = 1;
            } else if (*flag == 'E') {
                options.fixed_strings = 0;
            } else if (*flag == 'o') {
                options.only_matching = 1;
            } else {
                print_usage(argv[0]);
                return 1;
            }
            flag += 1;
        }

        arg_index += 1;
    }

    if (argc <= arg_index) {
        print_usage(argv[0]);
        return 1;
    }

    if (options.quiet) {
        options.count_only = 0;
        options.list_files = 0;
    }

    file_count = argc - arg_index - 1;
    if (file_count <= 0) {
        int matched = 0;
        return grep_stream(0, argv[arg_index], &options, "", 0, &matched) == 0 ? (matched ? 0 : 1) : 1;
    }

    for (i = arg_index + 1; i < argc; ++i) {
        int matched = 0;

        if (grep_path(argv[i], argv[arg_index], &options, file_count > 1, &matched) != 0) {
            rt_write_cstr(2, "grep: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        } else if (matched) {
            any_match = 1;
        }
    }

    if (exit_code != 0) {
        return exit_code;
    }

    return any_match ? 0 : 1;
}
