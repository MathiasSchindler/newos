#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define SORT_MAX_LINES 2048
#define SORT_MAX_LINE_LENGTH 512

typedef struct {
    int numeric;
    int reverse;
    int unique;
    int have_key;
    unsigned long long key_start;
    unsigned long long key_end;
    int have_separator;
    char separator;
} SortOptions;

static int parse_key_spec(const char *text, SortOptions *options) {
    unsigned long long start = 0ULL;
    unsigned long long end = 0ULL;
    size_t index = 0U;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[index] >= '0' && text[index] <= '9') {
        start = start * 10ULL + (unsigned long long)(text[index] - '0');
        index += 1U;
    }

    if (start == 0ULL) {
        return -1;
    }

    end = start;
    if (text[index] == ',') {
        index += 1U;
        if (text[index] == '\0') {
            return -1;
        }

        end = 0ULL;
        while (text[index] >= '0' && text[index] <= '9') {
            end = end * 10ULL + (unsigned long long)(text[index] - '0');
            index += 1U;
        }

        if (end == 0ULL || end < start) {
            return -1;
        }
    }

    if (text[index] != '\0') {
        return -1;
    }

    options->have_key = 1;
    options->key_start = start;
    options->key_end = end;
    return 0;
}

static int store_line(char lines[SORT_MAX_LINES][SORT_MAX_LINE_LENGTH], size_t *count, const char *line, size_t len) {
    size_t copy_len = len;

    if (*count >= SORT_MAX_LINES) {
        return -1;
    }

    if (copy_len >= SORT_MAX_LINE_LENGTH) {
        copy_len = SORT_MAX_LINE_LENGTH - 1;
    }

    memcpy(lines[*count], line, copy_len);
    lines[*count][copy_len] = '\0';
    *count += 1;
    return 0;
}

static int collect_lines_from_fd(int fd, char lines[SORT_MAX_LINES][SORT_MAX_LINE_LENGTH], size_t *count) {
    char buffer[2048];
    char current[SORT_MAX_LINE_LENGTH];
    size_t current_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];

            if (ch == '\n') {
                if (store_line(lines, count, current, current_len) != 0) {
                    return -1;
                }
                current_len = 0;
            } else if (current_len + 1 < sizeof(current)) {
                current[current_len] = ch;
                current_len += 1;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (current_len > 0) {
        return store_line(lines, count, current, current_len);
    }

    return 0;
}

static long long parse_signed_number(const char *text, int *ok_out) {
    long long value = 0;
    long long sign = 1;
    int saw_digit = 0;

    while (*text == ' ' || *text == '\t') {
        text += 1;
    }

    if (*text == '-') {
        sign = -1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }

    while (*text >= '0' && *text <= '9') {
        saw_digit = 1;
        value = value * 10 + (long long)(*text - '0');
        text += 1;
    }

    if (ok_out != 0) {
        *ok_out = saw_digit;
    }

    return value * sign;
}

static int is_sort_space(char ch) {
    return ch == ' ' || ch == '\t';
}

static int extract_key_text(const char *line, char *buffer, size_t buffer_size, const SortOptions *options) {
    size_t start_index = 0U;
    size_t end_index = rt_strlen(line);
    size_t out_len;

    if (buffer_size == 0U) {
        return -1;
    }

    if (!options->have_key) {
        rt_copy_string(buffer, buffer_size, line);
        return 0;
    }

    if (options->have_separator) {
        unsigned long long field = 1ULL;
        size_t i = 0U;
        int have_start = 0;
        int have_end = 0;

        while (1) {
            size_t field_begin = i;

            while (line[i] != '\0' && line[i] != options->separator) {
                i += 1U;
            }

            if (field == options->key_start) {
                start_index = field_begin;
                have_start = 1;
            }
            if (field == options->key_end) {
                end_index = i;
                have_end = 1;
            }

            if (line[i] == '\0') {
                break;
            }

            i += 1U;
            field += 1ULL;
        }

        if (!have_start) {
            buffer[0] = '\0';
            return 0;
        }
        if (!have_end) {
            end_index = rt_strlen(line);
        }
    } else {
        unsigned long long field = 0ULL;
        size_t i = 0U;
        int have_start = 0;
        int have_end = 0;

        while (line[i] != '\0') {
            while (line[i] != '\0' && is_sort_space(line[i])) {
                i += 1U;
            }
            if (line[i] == '\0') {
                break;
            }

            field += 1ULL;
            if (field == options->key_start) {
                start_index = i;
                have_start = 1;
            }

            while (line[i] != '\0' && !is_sort_space(line[i])) {
                i += 1U;
            }

            if (field == options->key_end) {
                end_index = i;
                have_end = 1;
                break;
            }
        }

        if (!have_start) {
            buffer[0] = '\0';
            return 0;
        }
        if (!have_end) {
            end_index = rt_strlen(line);
        }
    }

    if (end_index < start_index) {
        buffer[0] = '\0';
        return 0;
    }

    out_len = end_index - start_index;
    if (out_len + 1U > buffer_size) {
        out_len = buffer_size - 1U;
    }

    memcpy(buffer, line + start_index, out_len);
    buffer[out_len] = '\0';
    return 0;
}

static int compare_lines(const char *left, const char *right, const SortOptions *options) {
    int result = 0;

    if (!options->have_key) {
        if (options->numeric) {
            int left_ok = 0;
            int right_ok = 0;
            long long left_value = parse_signed_number(left, &left_ok);
            long long right_value = parse_signed_number(right, &right_ok);

            if (left_ok && right_ok && left_value != right_value) {
                result = (left_value < right_value) ? -1 : 1;
            }
        }

        if (result == 0) {
            result = rt_strcmp(left, right);
        }

        return options->reverse ? -result : result;
    }

    {
        char left_key[SORT_MAX_LINE_LENGTH];
        char right_key[SORT_MAX_LINE_LENGTH];
        const char *left_text = left;
        const char *right_text = right;

        if (extract_key_text(left, left_key, sizeof(left_key), options) != 0 ||
            extract_key_text(right, right_key, sizeof(right_key), options) != 0) {
            return 0;
        }

        left_text = left_key;
        right_text = right_key;

        if (options->numeric) {
            int left_ok = 0;
            int right_ok = 0;
            long long left_value = parse_signed_number(left_text, &left_ok);
            long long right_value = parse_signed_number(right_text, &right_ok);

            if (left_ok && right_ok && left_value != right_value) {
                result = (left_value < right_value) ? -1 : 1;
            }
        }

        if (result == 0) {
            result = rt_strcmp(left_text, right_text);
        }

        if (result == 0) {
            result = rt_strcmp(left, right);
        }
    }

    return options->reverse ? -result : result;
}

static void merge_line_order(const char **order,
                             const char **scratch,
                             size_t left,
                             size_t middle,
                             size_t right,
                             const SortOptions *options) {
    size_t left_index = left;
    size_t right_index = middle;
    size_t out = left;

    while (left_index < middle && right_index < right) {
        if (compare_lines(order[left_index], order[right_index], options) <= 0) {
            scratch[out++] = order[left_index++];
        } else {
            scratch[out++] = order[right_index++];
        }
    }

    while (left_index < middle) {
        scratch[out++] = order[left_index++];
    }
    while (right_index < right) {
        scratch[out++] = order[right_index++];
    }

    for (out = left; out < right; ++out) {
        order[out] = scratch[out];
    }
}

static void merge_sort_lines(const char **order,
                             const char **scratch,
                             size_t left,
                             size_t right,
                             const SortOptions *options) {
    size_t middle;

    if (right - left < 2U) {
        return;
    }

    middle = left + ((right - left) / 2U);
    merge_sort_lines(order, scratch, left, middle, options);
    merge_sort_lines(order, scratch, middle, right, options);
    merge_line_order(order, scratch, left, middle, right, options);
}

static void sort_lines(const char **order, size_t count, const SortOptions *options) {
    const char *scratch[SORT_MAX_LINES];

    if (count < 2U) {
        return;
    }

    merge_sort_lines(order, scratch, 0U, count, options);
}

int main(int argc, char **argv) {
    char lines[SORT_MAX_LINES][SORT_MAX_LINE_LENGTH];
    const char *order[SORT_MAX_LINES];
    size_t count = 0;
    int exit_code = 0;
    int argi = 1;
    int i;
    SortOptions options;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-k") == 0 || (argv[argi][1] == 'k' && argv[argi][2] != '\0')) {
            const char *value = (rt_strcmp(argv[argi], "-k") == 0) ? ((argi + 1 < argc) ? argv[argi + 1] : 0) : argv[argi] + 2;
            if (parse_key_spec(value, &options) != 0) {
                rt_write_line(2, "Usage: sort [-nru] [-t CHAR] [-k FIELD[,FIELD]] [file ...]");
                return 1;
            }
            argi += (rt_strcmp(argv[argi], "-k") == 0) ? 2 : 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-t") == 0 || (argv[argi][1] == 't' && argv[argi][2] != '\0')) {
            const char *value = (rt_strcmp(argv[argi], "-t") == 0) ? ((argi + 1 < argc) ? argv[argi + 1] : 0) : argv[argi] + 2;
            if (value == 0 || value[0] == '\0' || value[1] != '\0') {
                rt_write_line(2, "Usage: sort [-nru] [-t CHAR] [-k FIELD[,FIELD]] [file ...]");
                return 1;
            }
            options.have_separator = 1;
            options.separator = value[0];
            argi += (rt_strcmp(argv[argi], "-t") == 0) ? 2 : 1;
            continue;
        }

        flag = argv[argi] + 1;
        while (*flag != '\0') {
            if (*flag == 'n') {
                options.numeric = 1;
            } else if (*flag == 'r') {
                options.reverse = 1;
            } else if (*flag == 'u') {
                options.unique = 1;
            } else {
                rt_write_line(2, "Usage: sort [-nru] [-t CHAR] [-k FIELD[,FIELD]] [file ...]");
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (argi == argc) {
        if (collect_lines_from_fd(0, lines, &count) != 0) {
            rt_write_line(2, "sort: read error");
            return 1;
        }
    } else {
        for (i = argi; i < argc; ++i) {
            int fd;
            int should_close;

            if (tool_open_input(argv[i], &fd, &should_close) != 0) {
                rt_write_cstr(2, "sort: cannot open ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
                continue;
            }

            if (collect_lines_from_fd(fd, lines, &count) != 0) {
                rt_write_cstr(2, "sort: read error on ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
            }

            tool_close_input(fd, should_close);
        }
    }

    for (i = 0; i < (int)count; ++i) {
        order[i] = lines[i];
    }

    sort_lines(order, count, &options);
    for (i = 0; i < (int)count; ++i) {
        if (options.unique && i > 0 && rt_strcmp(order[i], order[i - 1]) == 0) {
            continue;
        }
        rt_write_line(1, order[i]);
    }

    return exit_code;
}
