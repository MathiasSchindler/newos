#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-n] PATTERN [file ...]");
}

static int match_here(const char *pattern, const char *text);

static int match_star(int token, const char *pattern, const char *text) {
    do {
        if (match_here(pattern, text)) {
            return 1;
        }
    } while (*text != '\0' && (*text++ == token || token == '.'));

    return 0;
}

static int match_here(const char *pattern, const char *text) {
    if (pattern[0] == '\0') {
        return 1;
    }

    if (pattern[1] == '*') {
        return match_star(pattern[0], pattern + 2, text);
    }

    if (pattern[0] == '$' && pattern[1] == '\0') {
        return text[0] == '\0';
    }

    if (text[0] != '\0' && (pattern[0] == '.' || pattern[0] == text[0])) {
        return match_here(pattern + 1, text + 1);
    }

    return 0;
}

static int match_regex(const char *pattern, const char *text) {
    if (pattern[0] == '^') {
        return match_here(pattern + 1, text);
    }

    do {
        if (match_here(pattern, text)) {
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

static int grep_stream(int fd, const char *pattern, int show_line_no, const char *label, int show_label, int *matched_out) {
    char chunk[4096];
    char line[8192];
    size_t line_len = 0;
    unsigned long long line_no = 1;
    int matched = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                line[line_len] = '\0';
                if (match_regex(pattern, line)) {
                    matched = 1;
                    if (print_match(label, show_label, line_no, show_line_no, line) != 0) {
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
        line[line_len] = '\0';
        if (match_regex(pattern, line)) {
            matched = 1;
            if (print_match(label, show_label, line_no, show_line_no, line) != 0) {
                return -1;
            }
        }
    }

    if (matched_out != 0) {
        *matched_out = matched;
    }

    return 0;
}

int main(int argc, char **argv) {
    int show_line_no = 0;
    int arg_index = 1;
    int file_count;
    int i;
    int exit_code = 0;
    int any_match = 0;

    if (argc > 1 && rt_strcmp(argv[1], "-n") == 0) {
        show_line_no = 1;
        arg_index = 2;
    }

    if (argc <= arg_index) {
        print_usage(argv[0]);
        return 1;
    }

    file_count = argc - arg_index - 1;
    if (file_count <= 0) {
        int matched = 0;
        return grep_stream(0, argv[arg_index], show_line_no, "", 0, &matched) == 0 ? (matched ? 0 : 1) : 1;
    }

    for (i = arg_index + 1; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "grep: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        {
            int matched = 0;

            if (grep_stream(fd, argv[arg_index], show_line_no, argv[i], file_count > 1, &matched) != 0) {
                rt_write_cstr(2, "grep: read error on ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
            } else if (matched) {
                any_match = 1;
            }
        }

        tool_close_input(fd, should_close);
    }

    if (exit_code != 0) {
        return exit_code;
    }

    return any_match ? 0 : 1;
}
