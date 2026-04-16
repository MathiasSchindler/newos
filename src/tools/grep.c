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
} GrepOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-nivr] PATTERN [file ...]");
}

static char to_lower_ascii(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int match_here(const char *pattern, const char *text, int ignore_case);

static int same_char(char pattern_ch, char text_ch, int ignore_case) {
    if (pattern_ch == '.') {
        return 1;
    }

    if (ignore_case) {
        return to_lower_ascii(pattern_ch) == to_lower_ascii(text_ch);
    }

    return pattern_ch == text_ch;
}

static int match_star(int token, const char *pattern, const char *text, int ignore_case) {
    do {
        if (match_here(pattern, text, ignore_case)) {
            return 1;
        }
    } while (*text != '\0' && (token == '.' || same_char((char)token, *text++, ignore_case)));

    return 0;
}

static int match_here(const char *pattern, const char *text, int ignore_case) {
    if (pattern[0] == '\0') {
        return 1;
    }

    if (pattern[1] == '*') {
        return match_star(pattern[0], pattern + 2, text, ignore_case);
    }

    if (pattern[0] == '$' && pattern[1] == '\0') {
        return text[0] == '\0';
    }

    if (text[0] != '\0' && same_char(pattern[0], text[0], ignore_case)) {
        return match_here(pattern + 1, text + 1, ignore_case);
    }

    return 0;
}

static int match_regex(const char *pattern, const char *text, int ignore_case) {
    if (pattern[0] == '^') {
        return match_here(pattern + 1, text, ignore_case);
    }

    do {
        if (match_here(pattern, text, ignore_case)) {
            return 1;
        }
    } while (*text++ != '\0');

    return 0;
}

static int print_match(const char *label, int show_label, unsigned long long line_no, int show_line_no, const char *line) {
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

    return rt_write_line(1, line);
}

static int grep_stream(int fd, const char *pattern, const GrepOptions *options, const char *label, int show_label, int *matched_out) {
    char chunk[4096];
    char line[GREP_LINE_CAPACITY];
    size_t line_len = 0;
    unsigned long long line_no = 1;
    int matched = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                int line_matches;

                line[line_len] = '\0';
                line_matches = match_regex(pattern, line, options->ignore_case);
                if (options->invert_match) {
                    line_matches = !line_matches;
                }

                if (line_matches) {
                    matched = 1;
                    if (print_match(label, show_label, line_no, options->show_line_no, line) != 0) {
                        return -1;
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

        line[line_len] = '\0';
        line_matches = match_regex(pattern, line, options->ignore_case);
        if (options->invert_match) {
            line_matches = !line_matches;
        }

        if (line_matches) {
            matched = 1;
            if (print_match(label, show_label, line_no, options->show_line_no, line) != 0) {
                return -1;
            }
        }
    }

    if (matched_out != 0) {
        *matched_out = matched;
    }

    return 0;
}

static int grep_path(const char *path, const char *pattern, const GrepOptions *options, int show_label, int *matched_out) {
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

        if (platform_collect_entries(path, 1, entries, GREP_ENTRY_CAPACITY, &count, &path_is_directory) != 0 || !path_is_directory) {
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

        while (*flag != '\0') {
            if (*flag == 'n') {
                options.show_line_no = 1;
            } else if (*flag == 'i') {
                options.ignore_case = 1;
            } else if (*flag == 'v') {
                options.invert_match = 1;
            } else if (*flag == 'r' || *flag == 'R') {
                options.recursive = 1;
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
