#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define COMM_MAX_LINES 2048
#define COMM_LINE_CAPACITY 2048

typedef struct {
    int suppress[3];
} CommOptions;

static char left_lines[COMM_MAX_LINES][COMM_LINE_CAPACITY];
static char right_lines[COMM_MAX_LINES][COMM_LINE_CAPACITY];

static void print_usage(void) {
    tool_write_usage("comm", "[-123] FILE1 FILE2");
}

static int store_line(char lines[COMM_MAX_LINES][COMM_LINE_CAPACITY], size_t *count, const char *line, size_t len) {
    size_t copy_len = len;

    if (*count >= COMM_MAX_LINES) {
        return -1;
    }
    if (copy_len >= COMM_LINE_CAPACITY) {
        copy_len = COMM_LINE_CAPACITY - 1U;
    }

    memcpy(lines[*count], line, copy_len);
    lines[*count][copy_len] = '\0';
    *count += 1U;
    return 0;
}

static int collect_lines(int fd, char lines[COMM_MAX_LINES][COMM_LINE_CAPACITY], size_t *count) {
    char chunk[4096];
    char current[COMM_LINE_CAPACITY];
    size_t current_len = 0U;
    long bytes_read;

    *count = 0U;
    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                if (store_line(lines, count, current, current_len) != 0) {
                    return -1;
                }
                current_len = 0U;
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

static int read_file_lines(const char *path, char lines[COMM_MAX_LINES][COMM_LINE_CAPACITY], size_t *count) {
    int fd = -1;
    int should_close = 0;
    int result;

    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }
    result = collect_lines(fd, lines, count);
    tool_close_input(fd, should_close);
    return result;
}

static int emit_line(const CommOptions *options, int column, const char *line) {
    int previous;

    for (previous = 1; previous < column; ++previous) {
        if (!options->suppress[previous - 1]) {
            if (rt_write_char(1, '\t') != 0) {
                return -1;
            }
        }
    }
    if (rt_write_cstr(1, line) != 0 || rt_write_char(1, '\n') != 0) {
        return -1;
    }
    return 0;
}

static int compare_lines(const char *left, const char *right) {
    int cmp = rt_strcmp(left, right);

    if (cmp < 0) {
        return -1;
    }
    if (cmp > 0) {
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    CommOptions options;
    int argi = 1;
    size_t left_count = 0U;
    size_t right_count = 0U;
    size_t left_index = 0U;
    size_t right_index = 0U;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *arg = argv[argi];
        size_t i;

        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        }
        if (rt_strcmp(arg, "-") == 0) {
            break;
        }

        for (i = 1U; arg[i] != '\0'; ++i) {
            if (arg[i] >= '1' && arg[i] <= '3') {
                options.suppress[arg[i] - '1'] = 1;
            } else {
                print_usage();
                return 1;
            }
        }
        argi += 1;
    }

    if (argc - argi != 2) {
        print_usage();
        return 1;
    }

    if (read_file_lines(argv[argi], left_lines, &left_count) != 0) {
        tool_write_error("comm", "cannot read ", argv[argi]);
        return 1;
    }
    if (read_file_lines(argv[argi + 1], right_lines, &right_count) != 0) {
        tool_write_error("comm", "cannot read ", argv[argi + 1]);
        return 1;
    }

    while (left_index < left_count || right_index < right_count) {
        int cmp;

        if (left_index >= left_count) {
            cmp = 1;
        } else if (right_index >= right_count) {
            cmp = -1;
        } else {
            cmp = compare_lines(left_lines[left_index], right_lines[right_index]);
        }

        if (cmp < 0) {
            if (!options.suppress[0] && emit_line(&options, 1, left_lines[left_index]) != 0) {
                return 1;
            }
            left_index += 1U;
        } else if (cmp > 0) {
            if (!options.suppress[1] && emit_line(&options, 2, right_lines[right_index]) != 0) {
                return 1;
            }
            right_index += 1U;
        } else {
            if (!options.suppress[2] && emit_line(&options, 3, left_lines[left_index]) != 0) {
                return 1;
            }
            left_index += 1U;
            right_index += 1U;
        }
    }

    return 0;
}
