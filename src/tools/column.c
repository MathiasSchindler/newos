#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define COLUMN_SEPARATOR_CAPACITY 32
#define COLUMN_SPACE_BUFFER_SIZE 128U

typedef struct {
    char input_separators[COLUMN_SEPARATOR_CAPACITY];
    char output_separator[COLUMN_SEPARATOR_CAPACITY];
    size_t output_separator_len;
    unsigned long long max_width;
    int transpose;
} ColumnOptions;

typedef struct {
    char *text;
    size_t width;
} ColumnCell;

typedef struct {
    ToolArray cells;
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

static size_t utf8_display_width(const char *text) {
    return tool_text_display_width_n(text, rt_strlen(text));
}

static size_t utf8_prefix_bytes_for_width(const char *text, size_t max_width) {
    size_t length = rt_strlen(text);
    return rt_text_prefix_bytes_for_width(text, length, (unsigned long long)max_width, 0ULL);
}

static ColumnCell *column_cell(ColumnRow *row, size_t index) {
    return (ColumnCell *)tool_array_get(&row->cells, index);
}

static int column_add_cell(ColumnRow *row, const char *text, size_t length) {
    ColumnCell *cell = (ColumnCell *)tool_array_append(&row->cells);

    if (cell == 0) return -1;
    cell->text = (char *)rt_malloc(length + 1U);
    if (cell->text == 0) {
        row->cells.count -= 1U;
        return -1;
    }
    if (length > 0U) memcpy(cell->text, text, length);
    cell->text[length] = '\0';
    cell->width = utf8_display_width(cell->text);
    return 0;
}

static int parse_row(const char *line, const char *separators, ColumnRow *row) {
    size_t i = 0;

    tool_array_init(&row->cells, sizeof(ColumnCell));

    if (separators == 0 || separators[0] == '\0') {
        while (line[i] != '\0') {
            size_t start;

            while (line[i] != '\0' && rt_is_space(line[i])) {
                i += 1U;
            }

            if (line[i] == '\0') {
                break;
            }

            start = i;
            while (line[i] != '\0' && !rt_is_space(line[i])) {
                i += 1U;
            }
            if (column_add_cell(row, line + start, i - start) != 0) return -1;
        }
    } else {
        size_t start = 0;

        while (1) {
            size_t cursor = start;

            while (line[cursor] != '\0' && !is_in_set(line[cursor], separators)) {
                cursor += 1U;
            }
            if (column_add_cell(row, line + start, cursor - start) != 0) return -1;

            if (line[cursor] == '\0') {
                break;
            }

            start = cursor + 1U;
        }
    }

    return 0;
}

static int collect_rows_from_fd(int fd, ToolArray *rows, const char *separators) {
    ToolRecordReader reader;
    ToolByteBuffer record;
    int result = 0;

    tool_record_reader_init(&reader, fd, '\n');
    tool_byte_buffer_init(&record);
    for (;;) {
        int has_record = 0;
        ColumnRow *row;
        if (tool_record_reader_next_buffer(&reader, &record, &has_record) != 0) {
            result = -1;
            break;
        }
        if (!has_record) break;
        row = (ColumnRow *)tool_array_append(rows);
        if (row == 0 || parse_row((const char *)record.data, separators, row) != 0) {
            result = -1;
            break;
        }
    }
    tool_byte_buffer_free(&record);
    return result;
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
    size_t *widths;
    size_t width_count = row_count;
    size_t i;
    size_t j;

    for (i = 0U; i < row_count; ++i) {
        if (rows[i].cells.count > width_count) width_count = rows[i].cells.count;
    }
    widths = (size_t *)rt_malloc_array(width_count == 0U ? 1U : width_count, sizeof(size_t));
    if (widths == 0) return -1;
    rt_memset(widths, 0, width_count * sizeof(size_t));

    if (options->transpose) {
        size_t max_cells = 0;

        for (i = 0; i < row_count; ++i) {
            if (rows[i].cells.count > max_cells) {
                max_cells = rows[i].cells.count;
            }
            for (j = 0; j < rows[i].cells.count; ++j) {
                ColumnCell *cell = column_cell(&rows[i], j);
                if (cell->width > widths[i]) {
                    widths[i] = cell->width;
                }
            }
        }

        for (i = 0; i < max_cells; ++i) {
            size_t used = 0;
            size_t last_col = 0;
            int have_cells = 0;

            for (j = 0; j < row_count; ++j) {
                if (rows[j].cells.count > i) {
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
                ColumnCell *cell = rows[j].cells.count > i ? column_cell(&rows[j], i) : 0;
                size_t cell_width = cell != 0 ? cell->width : 0U;
                size_t cell_bytes = cell != 0 ? rt_strlen(cell->text) : 0U;
                size_t write_bytes = cell_bytes;
                size_t written_width = cell_width;

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

                if (options->max_width != 0ULL && used < (size_t)options->max_width && used + cell_width > (size_t)options->max_width) {
                    write_bytes = utf8_prefix_bytes_for_width(cell->text, (size_t)options->max_width - used);
                    written_width = tool_text_display_width_n(cell->text, write_bytes);
                }

                if (write_bytes > 0U && cell != 0 && rt_write_all(1, cell->text, write_bytes) != 0) {
                    return -1;
                }
                used += written_width;

                if (write_bytes < cell_bytes) {
                    break;
                }

                if (j < last_col) {
                    size_t padding = widths[j] > cell_width ? widths[j] - cell_width : 0U;
                    if (options->max_width != 0ULL && used >= (size_t)options->max_width) {
                        break;
                    }
                    if (options->max_width != 0ULL && used + padding > (size_t)options->max_width) {
                        padding = (size_t)options->max_width - used;
                    }
                    if (tool_write_repeated_char(1, ' ', padding) != 0) {
                        return -1;
                    }
                    used += padding;
                }
            }

            if (rt_write_char(1, '\n') != 0) {
                return -1;
            }
        }

        rt_free(widths);
        return 0;
    }

    for (i = 0; i < row_count; ++i) {
        for (j = 0; j < rows[i].cells.count; ++j) {
            ColumnCell *cell = column_cell(&rows[i], j);
            if (cell->width > widths[j]) {
                widths[j] = cell->width;
            }
        }
    }

    for (i = 0; i < row_count; ++i) {
        size_t used = 0;

        if (rows[i].cells.count == 0U) {
            if (rt_write_char(1, '\n') != 0) {
                return -1;
            }
            continue;
        }

        for (j = 0; j < rows[i].cells.count; ++j) {
            ColumnCell *cell = column_cell(&rows[i], j);
            size_t cell_width = cell->width;
            size_t cell_bytes = rt_strlen(cell->text);
            size_t write_bytes = cell_bytes;
            size_t written_width = cell_width;

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

            if (options->max_width != 0ULL && used < (size_t)options->max_width && used + cell_width > (size_t)options->max_width) {
                write_bytes = utf8_prefix_bytes_for_width(cell->text, (size_t)options->max_width - used);
                written_width = tool_text_display_width_n(cell->text, write_bytes);
            }

            if (write_bytes > 0U && rt_write_all(1, cell->text, write_bytes) != 0) {
                return -1;
            }
            used += written_width;

            if (write_bytes < cell_bytes) {
                break;
            }

            if (j + 1U < rows[i].cells.count) {
                size_t padding = widths[j] > cell_width ? widths[j] - cell_width : 0U;
                if (options->max_width != 0ULL && used >= (size_t)options->max_width) {
                    break;
                }
                if (options->max_width != 0ULL && used + padding > (size_t)options->max_width) {
                    padding = (size_t)options->max_width - used;
                }
                if (tool_write_repeated_char(1, ' ', padding) != 0) {
                    return -1;
                }
                used += padding;
            }
        }

        if (rt_write_char(1, '\n') != 0) {
            return -1;
        }
    }

    rt_free(widths);
    return 0;
}

static void free_rows(ToolArray *rows) {
    size_t i;
    for (i = 0U; i < rows->count; ++i) {
        ColumnRow *row = (ColumnRow *)tool_array_get(rows, i);
        size_t j;
        for (j = 0U; j < row->cells.count; ++j) rt_free(column_cell(row, j)->text);
        tool_array_free(&row->cells);
    }
    tool_array_free(rows);
}

int main(int argc, char **argv) {
    ToolArray rows;
    ColumnOptions options;
    int argi = 1;
    int exit_code = 0;
    const char *input_separators = 0;

    options.input_separators[0] = '\0';
    rt_copy_string(options.output_separator, sizeof(options.output_separator), "  ");
    options.output_separator_len = 2U;
    options.max_width = 0ULL;
    options.transpose = 0;
    tool_array_init(&rows, sizeof(ColumnRow));

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
        if (collect_rows_from_fd(0, &rows, input_separators) != 0) {
            tool_write_error("column", "read error", "");
            free_rows(&rows);
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

            if (collect_rows_from_fd(fd, &rows, input_separators) != 0) {
                tool_write_error("column", "read error on ", argv[argi]);
                exit_code = 1;
            }

            tool_close_input(fd, should_close);
        }
    }

    if (rows.count > 0U && print_rows((ColumnRow *)rows.data, rows.count, &options) != 0) {
        free_rows(&rows);
        return 1;
    }

    free_rows(&rows);
    return exit_code;
}
