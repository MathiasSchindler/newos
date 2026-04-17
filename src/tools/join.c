#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define JOIN_MAX_LINES 1024
#define JOIN_LINE_CAPACITY 1024

typedef struct {
    unsigned long long left_field;
    unsigned long long right_field;
    char delimiter;
    int print_unpaired_left;
    int print_unpaired_right;
} JoinOptions;

static int store_line(char lines[JOIN_MAX_LINES][JOIN_LINE_CAPACITY], size_t *count, const char *line, size_t len) {
    size_t copy_len = len;

    if (*count >= JOIN_MAX_LINES) {
        return -1;
    }

    if (copy_len >= JOIN_LINE_CAPACITY) {
        copy_len = JOIN_LINE_CAPACITY - 1U;
    }

    memcpy(lines[*count], line, copy_len);
    lines[*count][copy_len] = '\0';
    *count += 1U;
    return 0;
}

static int collect_lines_from_fd(int fd, char lines[JOIN_MAX_LINES][JOIN_LINE_CAPACITY], size_t *count) {
    char chunk[4096];
    char current[JOIN_LINE_CAPACITY];
    size_t current_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                if (store_line(lines, count, current, current_len) != 0) {
                    return -1;
                }
                current_len = 0;
            } else if (current_len + 1U < sizeof(current)) {
                current[current_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (current_len > 0U) {
        return store_line(lines, count, current, current_len);
    }

    return 0;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-a1] [-a2] [-1 FIELD] [-2 FIELD] [-t CHAR] file1 file2");
}

static char parse_delimiter(const char *text) {
    if (text[0] == '\\' && text[1] == 't' && text[2] == '\0') {
        return '\t';
    }
    return text[0];
}

static int extract_field(const char *line, unsigned long long field_no, char delimiter, char *out, size_t out_size) {
    size_t i = 0;
    unsigned long long current_field = 1ULL;
    size_t out_len = 0;

    if (delimiter == '\0') {
        while (line[i] != '\0') {
            while (line[i] != '\0' && rt_is_space(line[i])) {
                i += 1;
            }

            if (line[i] == '\0') {
                break;
            }

            if (current_field == field_no) {
                while (line[i] != '\0' && !rt_is_space(line[i])) {
                    if (out_len + 1U < out_size) {
                        out[out_len++] = line[i];
                    }
                    i += 1;
                }
                out[out_len] = '\0';
                return 0;
            }

            while (line[i] != '\0' && !rt_is_space(line[i])) {
                i += 1;
            }
            current_field += 1ULL;
        }
    } else {
        size_t start = 0;

        while (1) {
            if (current_field == field_no) {
                while (line[start] != '\0' && line[start] != delimiter) {
                    if (out_len + 1U < out_size) {
                        out[out_len++] = line[start];
                    }
                    start += 1;
                }
                out[out_len] = '\0';
                return 0;
            }

            while (line[start] != '\0' && line[start] != delimiter) {
                start += 1;
            }

            if (line[start] == '\0') {
                break;
            }

            start += 1;
            current_field += 1ULL;
        }
    }

    out[0] = '\0';
    return -1;
}

static int write_separator(char delimiter, int *first_out) {
    if (*first_out) {
        *first_out = 0;
        return 0;
    }

    return rt_write_char(1, delimiter == '\0' ? ' ' : delimiter);
}

static int emit_text_field(const char *text, size_t len, char delimiter, int *first_out) {
    if (write_separator(delimiter, first_out) != 0) {
        return -1;
    }

    return rt_write_all(1, text, len);
}

static int emit_fields_except(const char *line, unsigned long long skip_field, char delimiter, int *first_out) {
    size_t i = 0;
    unsigned long long field_no = 1ULL;

    if (delimiter == '\0') {
        while (line[i] != '\0') {
            size_t start;
            size_t len = 0;

            while (line[i] != '\0' && rt_is_space(line[i])) {
                i += 1;
            }

            if (line[i] == '\0') {
                break;
            }

            start = i;
            while (line[i] != '\0' && !rt_is_space(line[i])) {
                i += 1;
                len += 1;
            }

            if (field_no != skip_field) {
                if (emit_text_field(line + start, len, delimiter, first_out) != 0) {
                    return -1;
                }
            }

            field_no += 1ULL;
        }
    } else {
        size_t start = 0;

        while (1) {
            size_t len = 0;
            while (line[start + len] != '\0' && line[start + len] != delimiter) {
                len += 1;
            }

            if (field_no != skip_field) {
                if (emit_text_field(line + start, len, delimiter, first_out) != 0) {
                    return -1;
                }
            }

            if (line[start + len] == '\0') {
                break;
            }

            start += len + 1U;
            field_no += 1ULL;
        }
    }

    return 0;
}

static int emit_joined_line(const char *left, const char *right, const JoinOptions *options) {
    char key[JOIN_LINE_CAPACITY];
    int first = 1;

    if (extract_field(left, options->left_field, options->delimiter, key, sizeof(key)) != 0 &&
        extract_field(right, options->right_field, options->delimiter, key, sizeof(key)) != 0) {
        key[0] = '\0';
    }

    if (emit_text_field(key, rt_strlen(key), options->delimiter, &first) != 0 ||
        emit_fields_except(left, options->left_field, options->delimiter, &first) != 0 ||
        emit_fields_except(right, options->right_field, options->delimiter, &first) != 0 ||
        rt_write_char(1, '\n') != 0) {
        return -1;
    }

    return 0;
}

static int join_files(const char *left_path, const char *right_path, const JoinOptions *options) {
    static char left_lines[JOIN_MAX_LINES][JOIN_LINE_CAPACITY];
    static char right_lines[JOIN_MAX_LINES][JOIN_LINE_CAPACITY];
    int right_matched[JOIN_MAX_LINES];
    size_t left_count = 0;
    size_t right_count = 0;
    int left_fd;
    int left_close;
    int right_fd;
    int right_close;
    size_t i;

    rt_memset(right_matched, 0, sizeof(right_matched));

    if (tool_open_input(left_path, &left_fd, &left_close) != 0) {
        tool_write_error("join", "cannot open ", left_path);
        return -1;
    }

    if (collect_lines_from_fd(left_fd, left_lines, &left_count) != 0) {
        tool_close_input(left_fd, left_close);
        tool_write_error("join", "read error on ", left_path);
        return -1;
    }
    tool_close_input(left_fd, left_close);

    if (tool_open_input(right_path, &right_fd, &right_close) != 0) {
        tool_write_error("join", "cannot open ", right_path);
        return -1;
    }

    if (collect_lines_from_fd(right_fd, right_lines, &right_count) != 0) {
        tool_close_input(right_fd, right_close);
        tool_write_error("join", "read error on ", right_path);
        return -1;
    }
    tool_close_input(right_fd, right_close);

    for (i = 0; i < left_count; ++i) {
        char left_key[JOIN_LINE_CAPACITY];
        int matched = 0;
        size_t j;

        if (extract_field(left_lines[i], options->left_field, options->delimiter, left_key, sizeof(left_key)) != 0) {
            left_key[0] = '\0';
        }

        for (j = 0; j < right_count; ++j) {
            char right_key[JOIN_LINE_CAPACITY];

            if (extract_field(right_lines[j], options->right_field, options->delimiter, right_key, sizeof(right_key)) != 0) {
                right_key[0] = '\0';
            }

            if (rt_strcmp(left_key, right_key) == 0) {
                matched = 1;
                right_matched[j] = 1;
                if (emit_joined_line(left_lines[i], right_lines[j], options) != 0) {
                    return -1;
                }
            }
        }

        if (!matched && options->print_unpaired_left) {
            if (rt_write_line(1, left_lines[i]) != 0) {
                return -1;
            }
        }
    }

    if (options->print_unpaired_right) {
        for (i = 0; i < right_count; ++i) {
            if (!right_matched[i] && rt_write_line(1, right_lines[i]) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    JoinOptions options;
    int argi = 1;

    options.left_field = 1ULL;
    options.right_field = 1ULL;
    options.delimiter = '\0';
    options.print_unpaired_left = 0;
    options.print_unpaired_right = 0;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-a1") == 0) {
            options.print_unpaired_left = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-a2") == 0) {
            options.print_unpaired_right = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-1") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.left_field, "join", "field") != 0 || options.left_field == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-2") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.right_field, "join", "field") != 0 || options.right_field == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-t") == 0) {
            if (argi + 1 >= argc || argv[argi + 1][0] == '\0') {
                print_usage(argv[0]);
                return 1;
            }
            options.delimiter = parse_delimiter(argv[argi + 1]);
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (argc - argi != 2) {
        print_usage(argv[0]);
        return 1;
    }

    return join_files(argv[argi], argv[argi + 1], &options) == 0 ? 0 : 1;
}
