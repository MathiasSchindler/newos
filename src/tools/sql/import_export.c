#include "internal.h"

static int sql_execute_import(SqlDatabase *db, SqlParser *parser) {
    char table_name[SQL_NAME_SIZE];
    char path[SQL_VALUE_SIZE];
    SqlTable *table;
    char *line;
    SqlLineReader reader;
    int fd;
    int has_line;
    int create_table = 0;
    int csv = 0;
    unsigned int imported = 0U;
    unsigned int column_index;
    size_t after_path;

    if (sql_read_identifier(parser, table_name, sizeof(table_name)) != 0 ||
        !sql_expect_word(parser, "from") ||
        sql_read_value(parser, path, sizeof(path)) != 0) {
        return -1;
    }
    after_path = parser->pos;
    if (sql_next_token(parser) != 0) {
        return -1;
    }
    while (parser->token_type != SQL_TOKEN_END) {
        parser->pos = after_path;
        if (sql_try_word(parser, "csv")) {
            csv = 1;
        } else if (sql_try_word(parser, "create")) {
            if (!sql_expect_word(parser, "table")) {
                return -1;
            }
            create_table = 1;
        } else {
            return -1;
        }
        after_path = parser->pos;
        if (sql_next_token(parser) != 0) {
            return -1;
        }
    }
    table = sql_find_table(db, table_name);
    if (table == 0 && !create_table) {
        return -1;
    }
    if (table != 0 && create_table) {
        return -1;
    }
    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }
    sql_line_reader_init(&reader, fd);
    if (sql_line_reader_next(&reader, &sql_import_line, &has_line) != 0 || !has_line) {
        (void)platform_close(fd);
        return -1;
    }
    line = sql_import_line.data;
    if (create_table) {
        char *field_cursor = line;
        unsigned int new_column_count = 0U;
        if (db->table_count >= SQL_MAX_TABLES || sql_ensure_table_capacity(db, db->table_count + 1U) != 0) {
            (void)platform_close(fd);
            return -1;
        }
        table = &db->tables[db->table_count];
        sql_clear_table_metadata(table);
        if (sql_copy_checked(table->name, sizeof(table->name), table_name) != 0) {
            (void)platform_close(fd);
            return -1;
        }
        for (;;) {
            int ok;
            char *field = sql_next_import_field(&field_cursor, csv, &ok);
            if (!ok || new_column_count >= SQL_MAX_COLUMNS || !sql_valid_identifier(field) || sql_find_column(table, field) >= 0) {
                (void)platform_close(fd);
                return -1;
            }
            if (sql_ensure_column_capacity(table, new_column_count + 1U) != 0) {
                (void)platform_close(fd);
                return -1;
            }
            if (sql_copy_checked(table->columns[new_column_count], sizeof(table->columns[new_column_count]), field) != 0) {
                (void)platform_close(fd);
                return -1;
            }
            new_column_count += 1U;
            table->column_count = new_column_count;
            if (*field_cursor == '\0') {
                break;
            }
        }
        if (new_column_count == 0U) {
            (void)platform_close(fd);
            return -1;
        }
        db->table_count += 1U;
    } else {
        char *field_cursor = line;
        for (column_index = 0U; column_index < table->column_count; ++column_index) {
            int ok;
            char *field = sql_next_import_field(&field_cursor, csv, &ok);
            if (!ok || !tool_str_equal_ignore_case_ascii(field, table->columns[column_index])) {
                (void)platform_close(fd);
                return -1;
            }
        }
        if (*field_cursor != '\0') {
            (void)platform_close(fd);
            return -1;
        }
    }
    while (sql_line_reader_next(&reader, &sql_import_line, &has_line) == 0 && has_line) {
        char *field_cursor;
        line = sql_import_line.data;
        if (line[0] == '\0') {
            continue;
        }
        if (table->row_count >= SQL_MAX_ROWS || sql_ensure_row_capacity(table, table->row_count + 1U) != 0) {
            (void)platform_close(fd);
            return -1;
        }
        field_cursor = line;
        if (sql_prepare_new_row(table, &table->rows[table->row_count]) != 0) {
            (void)platform_close(fd);
            return -1;
        }
        for (column_index = 0U; column_index < table->column_count; ++column_index) {
            int ok;
            char *field = sql_next_import_field(&field_cursor, csv, &ok);
            if (!ok || sql_store_row_value(db, &table->rows[table->row_count], column_index, field) != 0) {
                (void)platform_close(fd);
                return -1;
            }
        }
        if (*field_cursor != '\0') {
            (void)platform_close(fd);
            return -1;
        }
        if (sql_validate_row_constraints(table, &table->rows[table->row_count]) != 0) {
            (void)platform_close(fd);
            return -1;
        }
        table->row_count += 1U;
        imported += 1U;
    }
    if (!reader.eof) {
        (void)platform_close(fd);
        return -1;
    }
    if (platform_close(fd) != 0) {
        return -1;
    }
    if (imported > 0U) {
        sql_invalidate_table_runtime_caches(table);
    }
    sql_write_row_count(imported);
    return imported > 0U ? 1 : 0;
}

static int sql_value_is_tsv_safe(const char *value) {
    size_t i;

    for (i = 0U; value[i] != '\0'; ++i) {
        if (value[i] == '\t' || value[i] == '\n' || value[i] == '\r') {
            return 0;
        }
    }
    return 1;
}

static char *sql_next_csv_field(char **cursor_io, int *ok_out) {
    char *cursor = *cursor_io;
    char *field = cursor;
    char *out = cursor;

    *ok_out = 1;
    if (*cursor == '"') {
        cursor += 1;
        for (;;) {
            if (*cursor == '\0') {
                *ok_out = 0;
                return field;
            }
            if (*cursor == '"') {
                if (cursor[1] == '"') {
                    *out++ = '"';
                    cursor += 2;
                    continue;
                }
                cursor += 1;
                break;
            }
            *out++ = *cursor++;
        }
        if (*cursor != ',' && *cursor != '\0') {
            *ok_out = 0;
            return field;
        }
    } else {
        while (*cursor != '\0' && *cursor != ',') {
            *out++ = *cursor++;
        }
    }
    if (*cursor == ',') {
        cursor += 1;
    }
    *out = '\0';
    *cursor_io = cursor;
    return field;
}

static char *sql_next_import_field(char **cursor_io, int csv, int *ok_out) {
    if (csv) {
        return sql_next_csv_field(cursor_io, ok_out);
    }
    *ok_out = 1;
    return sql_next_delimited_field(cursor_io, '\t');
}

static int sql_csv_value_needs_quotes(const char *value) {
    size_t i;

    for (i = 0U; value[i] != '\0'; ++i) {
        if (value[i] == ',' || value[i] == '"' || value[i] == '\n' || value[i] == '\r') {
            return 1;
        }
    }
    return value[0] == '\0';
}

static int sql_write_csv_value(int fd, const char *value) {
    size_t i;

    if (!sql_csv_value_needs_quotes(value)) {
        return rt_write_cstr(fd, value);
    }
    if (rt_write_char(fd, '"') != 0) {
        return -1;
    }
    for (i = 0U; value[i] != '\0'; ++i) {
        if (value[i] == '"' && rt_write_char(fd, '"') != 0) {
            return -1;
        }
        if (rt_write_char(fd, value[i]) != 0) {
            return -1;
        }
    }
    return rt_write_char(fd, '"');
}

static int sql_execute_export(SqlDatabase *db, SqlParser *parser) {
    char table_name[SQL_NAME_SIZE];
    char path[SQL_VALUE_SIZE];
    SqlTable *table;
    int fd;
    unsigned int column_index;
    unsigned int row_index;
    int stdout_export;
    int csv = 0;
    size_t after_path;

    if (sql_read_identifier(parser, table_name, sizeof(table_name)) != 0 ||
        !sql_expect_word(parser, "to") ||
        sql_read_value(parser, path, sizeof(path)) != 0) {
        return -1;
    }
    after_path = parser->pos;
    if (sql_next_token(parser) != 0) {
        return -1;
    }
    if (parser->token_type != SQL_TOKEN_END) {
        parser->pos = after_path;
        if (!sql_try_word(parser, "csv") || !sql_at_end(parser)) {
            return -1;
        }
        csv = 1;
    }
    table = sql_find_table(db, table_name);
    if (table == 0) {
        return -1;
    }
    fd = platform_open_write(path, 0644U);
    if (fd < 0) {
        return -1;
    }
    for (column_index = 0U; column_index < table->column_count; ++column_index) {
        if (column_index > 0U && rt_write_char(fd, csv ? ',' : '\t') != 0) {
            (void)platform_close(fd);
            return -1;
        }
        if ((csv ? sql_write_csv_value(fd, table->columns[column_index]) : rt_write_cstr(fd, table->columns[column_index])) != 0) {
            (void)platform_close(fd);
            return -1;
        }
    }
    if (rt_write_char(fd, '\n') != 0) {
        (void)platform_close(fd);
        return -1;
    }
    for (row_index = 0U; row_index < table->row_count; ++row_index) {
        for (column_index = 0U; column_index < table->column_count; ++column_index) {
            const char *value = sql_row_display_value(&table->rows[row_index], column_index);
            if (column_index > 0U && rt_write_char(fd, csv ? ',' : '\t') != 0) {
                (void)platform_close(fd);
                return -1;
            }
            if ((!csv && !sql_value_is_tsv_safe(value)) || (csv ? sql_write_csv_value(fd, value) : rt_write_cstr(fd, value)) != 0) {
                (void)platform_close(fd);
                return -1;
            }
        }
        if (rt_write_char(fd, '\n') != 0) {
            (void)platform_close(fd);
            return -1;
        }
    }
    stdout_export = rt_strcmp(path, "-") == 0;
    if (!stdout_export && platform_close(fd) != 0) {
        return -1;
    }
    if (!stdout_export) {
        sql_write_row_count(table->row_count);
    }
    return 0;
}

