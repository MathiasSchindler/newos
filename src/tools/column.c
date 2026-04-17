#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define COLUMN_MAX_ROWS 512
#define COLUMN_MAX_COLS 32
#define COLUMN_CELL_CAPACITY 256
#define COLUMN_LINE_CAPACITY 2048

typedef struct {
    char cells[COLUMN_MAX_COLS][COLUMN_CELL_CAPACITY];
    size_t widths[COLUMN_MAX_COLS];
    size_t count;
} ColumnRow;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-t] [-s SEP] [file ...]");
}

static int is_in_set(char ch, const char *set) {
    size_t i = 0;

    while (set[i] != '\0') {
        if (set[i] == ch) {
            return 1;
        }
        i += 1;
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
                i += 1;
            }

            if (line[i] == '\0') {
                break;
            }

            while (line[i] != '\0' && !rt_is_space(line[i])) {
                if (cell_len + 1U < COLUMN_CELL_CAPACITY) {
                    row->cells[row->count][cell_len++] = line[i];
                }
                i += 1;
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
                cursor += 1;
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

static int print_rows(ColumnRow *rows, size_t row_count) {
    size_t widths[COLUMN_MAX_COLS];
    size_t i;
    size_t j;

    rt_memset(widths, 0, sizeof(widths));

    for (i = 0; i < row_count; ++i) {
        for (j = 0; j < rows[i].count; ++j) {
            if (rows[i].widths[j] > widths[j]) {
                widths[j] = rows[i].widths[j];
            }
        }
    }

    for (i = 0; i < row_count; ++i) {
        if (rows[i].count == 0U) {
            if (rt_write_char(1, '\n') != 0) {
                return -1;
            }
            continue;
        }

        for (j = 0; j < rows[i].count; ++j) {
            size_t len = rows[i].widths[j];

            if (rt_write_cstr(1, rows[i].cells[j]) != 0) {
                return -1;
            }

            if (j + 1U < rows[i].count) {
                size_t padding = widths[j] > len ? widths[j] - len : 0U;
                if (write_spaces(padding + 2U) != 0) {
                    return -1;
                }
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
    const char *separators = 0;
    int argi = 1;
    int exit_code = 0;
    size_t row_count = 0;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-t") == 0) {
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-s") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            separators = argv[argi + 1];
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

    if (argi == argc) {
        if (collect_rows_from_fd(0, rows, &row_count, separators) != 0) {
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

            if (collect_rows_from_fd(fd, rows, &row_count, separators) != 0) {
                tool_write_error("column", "read error on ", argv[argi]);
                exit_code = 1;
            }

            tool_close_input(fd, should_close);
        }
    }

    if (row_count > 0U && print_rows(rows, row_count) != 0) {
        return 1;
    }

    return exit_code;
}
