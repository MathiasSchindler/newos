#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define COLUMN_MAX_ROWS 512
#define COLUMN_MAX_COLS 32
#define COLUMN_CELL_CAPACITY 256
#define COLUMN_LINE_CAPACITY 2048
#define COLUMN_SEPARATOR_CAPACITY 32

typedef struct {
    char input_separators[COLUMN_SEPARATOR_CAPACITY];
    char output_separator[COLUMN_SEPARATOR_CAPACITY];
    size_t output_separator_len;
    unsigned long long max_width;
    int transpose;
} ColumnOptions;

typedef struct {
    char cells[COLUMN_MAX_COLS][COLUMN_CELL_CAPACITY];
    size_t widths[COLUMN_MAX_COLS];
    size_t count;
} ColumnRow;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-t] [-x] [-s SEP] [-o OUTSEP] [-c WIDTH] [file ...]");
}

static int is_in_set(char ch, const char *set) {
    size_t i = 0;

    while (set[i] != '\0') {
        if (set[i] == ch) {
            return 1;
        }
        i += 1U;
    }

    return 0;
}

static int parse_row(const char *line, const char *separators, ColumnRow *row) {
    size_t i = 0;

    row->count = 0;

    if (separators == 0 || separators[0] == '\0') {
        while (line[i] != '\0' && row->count < COLUMN_MAX_COLS) {
            size_t cell_len = 0;

            while (line[i] != '\0' && rt_is_space(line[i])) {
                i += 1U;
            }

            if (line[i] == '\0') {
                break;
            }

            while (line[i] != '\0' && !rt_is_space(line[i])) {
                if (cell_len + 1U < COLUMN_CELL_CAPACITY) {
                    row->cells[row->count][cell_len++] = line[i];
                }
                i += 1U;
            }

            row->cells[row->count][cell_len] = '\0';
            row->widths[row->count] = cell_len;
            row->count += 1U;
        }
    } else {
        size_t start = 0;

        while (row->count < COLUMN_MAX_COLS) {
            size_t cell_len = 0;
            size_t cursor = start;

            while (line[cursor] != '\0' && !is_in_set(line[cursor], separators)) {
                if (cell_len + 1U < COLUMN_CELL_CAPACITY) {
                    row->cells[row->count][cell_len++] = line[cursor];
                }
                cursor += 1U;
            }

            row->cells[row->count][cell_len] = '\0';
            row->widths[row->count] = cell_len;
            row->count += 1U;

            if (line[cursor] == '\0') {
                break;
            }

            start = cursor + 1U;
        }
    }

    return 0;
}

static int collect_rows_from_fd(int fd, ColumnRow *rows, size_t *row_count, const char *separators) {
    char chunk[4096];
    char current[COLUMN_LINE_CAPACITY];
    size_t current_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                if (*row_count >= COLUMN_MAX_ROWS) {
                    return -1;
                }

                current[current_len] = '\0';
                parse_row(current, separators, &rows[*row_count]);
                *row_count += 1U;
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
        if (*row_count >= COLUMN_MAX_ROWS) {
            return -1;
        }

        current[current_len] = '\0';
        parse_row(current, separators, &rows[*row_count]);
        *row_count += 1U;
    }

    return 0;
}

static int write_spaces(size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (rt_write_char(1, ' ') != 0) {
            return -1;
        }
    }

    return 0;
}

static int write_separator(const ColumnOptions *options, size_t *used) {
    if (options->output_separator_len > 0U) {
        if (rt_write_all(1, options->output_separator, options->output_separator_len) != 0) {
            return -1;
        }
        *used += options->output_separator_len;
    }
    return 0;
}

static int print_rows(ColumnRow *rows, size_t row_count, const ColumnOptions *options) {
    size_t widths[COLUMN_MAX_ROWS];
    size_t i;
    size_t j;

    rt_memset(widths, 0, sizeof(widths));

    if (options->transpose) {
        size_t max_cells = 0;

        for (i = 0; i < row_count; ++i) {
            if (rows[i].count > max_cells) {
                max_cells = rows[i].count;
            }
            for (j = 0; j < rows[i].count; ++j) {
                if (rows[i].widths[j] > widths[i]) {
                    widths[i] = rows[i].widths[j];
                }
            }
        }

        for (i = 0; i < max_cells; ++i) {
            size_t used = 0;
            size_t last_col = 0;
            int have_cells = 0;

            for (j = 0; j < row_count; ++j) {
                if (rows[j].count > i) {
                    last_col = j;
                    have_cells = 1;
                }
            }

            if (!have_cells) {
                if (rt_write_char(1, '\n') != 0) {
                    return -1;
                }
                continue;
            }

            for (j = 0; j <= last_col; ++j) {
                size_t cell_len = rows[j].count > i ? rows[j].widths[i] : 0U;
                size_t len = cell_len;

                if (j > 0U) {
                    if (options->max_width != 0ULL && used >= (size_t)options->max_width) {
                        break;
                    }
                    if (options->max_width != 0ULL && used + options->output_separator_len > (size_t)options->max_width) {
                        break;
                    }
                    if (write_separator(options, &used) != 0) {
                        return -1;
                    }
                }

                if (options->max_width != 0ULL && used < (size_t)options->max_width && used + len > (size_t)options->max_width) {
                    len = (size_t)options->max_width - used;
                }

                if (len > 0U && rows[j].count > i && rt_write_all(1, rows[j].cells[i], len) != 0) {
                    return -1;
                }
                used += len;

                if (len < cell_len) {
                    break;
                }

                if (j < last_col) {
                    size_t padding = widths[j] > cell_len ? widths[j] - cell_len : 0U;
                    if (options->max_width != 0ULL && used >= (size_t)options->max_width) {
                        break;
                    }
                    if (options->max_width != 0ULL && used + padding > (size_t)options->max_width) {
                        padding = (size_t)options->max_width - used;
                    }
                    if (write_spaces(padding) != 0) {
                        return -1;
                    }
                    used += padding;
                }
            }

            if (rt_write_char(1, '\n') != 0) {
                return -1;
            }
        }

        return 0;
    }

    for (i = 0; i < row_count; ++i) {
        for (j = 0; j < rows[i].count; ++j) {
            if (rows[i].widths[j] > widths[j]) {
                widths[j] = rows[i].widths[j];
            }
        }
    }

    for (i = 0; i < row_count; ++i) {
        size_t used = 0;

        if (rows[i].count == 0U) {
            if (rt_write_char(1, '\n') != 0) {
                return -1;
            }
            continue;
        }

        for (j = 0; j < rows[i].count; ++j) {
            size_t cell_len = rows[i].widths[j];
            size_t len = cell_len;

            if (j > 0U) {
                if (options->max_width != 0ULL && used >= (size_t)options->max_width) {
                    break;
                }
                if (options->max_width != 0ULL && used + options->output_separator_len > (size_t)options->max_width) {
                    break;
                }
                if (write_separator(options, &used) != 0) {
                    return -1;
                }
            }

            if (options->max_width != 0ULL && used < (size_t)options->max_width && used + len > (size_t)options->max_width) {
                len = (size_t)options->max_width - used;
            }

            if (len > 0U && rt_write_all(1, rows[i].cells[j], len) != 0) {
                return -1;
            }
            used += len;

            if (len < cell_len) {
                break;
            }

            if (j + 1U < rows[i].count) {
                size_t padding = widths[j] > cell_len ? widths[j] - cell_len : 0U;
                if (options->max_width != 0ULL && used >= (size_t)options->max_width) {
                    break;
                }
                if (options->max_width != 0ULL && used + padding > (size_t)options->max_width) {
                    padding = (size_t)options->max_width - used;
                }
                if (write_spaces(padding) != 0) {
                    return -1;
                }
                used += padding;
            }
        }

        if (rt_write_char(1, '\n') != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    static ColumnRow rows[COLUMN_MAX_ROWS];
    ColumnOptions options;
    int argi = 1;
    int exit_code = 0;
    size_t row_count = 0;
    const char *input_separators = 0;

    options.input_separators[0] = '\0';
    rt_copy_string(options.output_separator, sizeof(options.output_separator), "  ");
    options.output_separator_len = 2U;
    options.max_width = 0ULL;
    options.transpose = 0;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-t") == 0) {
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-x") == 0) {
            options.transpose = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-s") == 0) {
            if (argi + 1 >= argc ||
                tool_parse_escaped_string(argv[argi + 1], options.input_separators, sizeof(options.input_separators), 0) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-o") == 0) {
            if (argi + 1 >= argc ||
                tool_parse_escaped_string(
                    argv[argi + 1],
                    options.output_separator,
                    sizeof(options.output_separator),
                    &options.output_separator_len
                ) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-c") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.max_width, "column", "width") != 0 || options.max_width == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else if (rt_strcmp(argv[argi], "-") == 0) {
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (options.input_separators[0] != '\0') {
        input_separators = options.input_separators;
    }

    if (argi == argc) {
        if (collect_rows_from_fd(0, rows, &row_count, input_separators) != 0) {
            tool_write_error("column", "read error", "");
            return 1;
        }
    } else {
        for (; argi < argc; ++argi) {
            int fd;
            int should_close;

            if (tool_open_input(argv[argi], &fd, &should_close) != 0) {
                tool_write_error("column", "cannot open ", argv[argi]);
                exit_code = 1;
                continue;
            }

            if (collect_rows_from_fd(fd, rows, &row_count, input_separators) != 0) {
                tool_write_error("column", "read error on ", argv[argi]);
                exit_code = 1;
            }

            tool_close_input(fd, should_close);
        }
    }

    if (row_count > 0U && print_rows(rows, row_count, &options) != 0) {
        return 1;
    }

    return exit_code;
}
