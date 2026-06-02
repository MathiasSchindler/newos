#include "internal.h"

static void sql_clear_table_metadata(SqlTable *table) {
    sql_free_table_storage(table);
    table->name[0] = '\0';
    table->column_count = 0U;
    table->column_capacity = 0U;
    table->row_count = 0U;
    table->row_capacity = 0U;
    table->rows = 0;
    table->row_values = 0;
}

static void sql_clear_database(SqlDatabase *db) {
    sql_invalidate_runtime_caches();
    sql_free_database_storage(db);
    sql_free_bytes(db->values);
    db->values = 0;
    db->value_capacity = 0U;
    db->value_used = 1U;
}

static int sql_line_next(char **cursor_io, char **line_out) {
    char *cursor = *cursor_io;
    char *line = cursor;

    if (cursor == 0 || *cursor == '\0') {
        return 0;
    }
    while (*cursor != '\0' && *cursor != '\n') {
        cursor += 1;
    }
    if (*cursor == '\n') {
        *cursor = '\0';
        cursor += 1;
    }
    *cursor_io = cursor;
    *line_out = line;
    return 1;
}

static char *sql_next_delimited_field(char **cursor_io, char delimiter) {
    char *field = *cursor_io;
    char *cursor = field;

    while (*cursor != '\0' && *cursor != delimiter) {
        cursor += 1;
    }
    if (*cursor == delimiter) {
        *cursor = '\0';
        cursor += 1;
    }
    *cursor_io = cursor;
    return field;
}

static int sql_parse_database_row(SqlDatabase *db, char *line, SqlTable *table) {
    char *cursor = line + 2;
    unsigned int value_index;

    if (table->row_count >= SQL_MAX_ROWS || sql_ensure_row_capacity(table, table->row_count + 1U) != 0) {
        return -1;
    }
    for (value_index = 0U; value_index < table->column_count; ++value_index) {
        unsigned long long length = 0ULL;
        char digits[16];
        size_t digit_count = 0U;
        char value[SQL_VALUE_SIZE];
        size_t copy_index;

        if (*cursor == 'N') {
            cursor += 1;
            if (*cursor != ':') {
                return -1;
            }
            cursor += 1;
            if (sql_store_row_null(&table->rows[table->row_count], value_index) != 0) {
                return -1;
            }
            continue;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            if (digit_count + 1U >= sizeof(digits)) {
                return -1;
            }
            digits[digit_count++] = *cursor++;
        }
        digits[digit_count] = '\0';
        if (*cursor != ':' || digit_count == 0U || rt_parse_uint(digits, &length) != 0 || length >= SQL_VALUE_SIZE) {
            return -1;
        }
        cursor += 1;
        if (length >= sizeof(value)) {
            return -1;
        }
        for (copy_index = 0U; copy_index < (size_t)length; ++copy_index) {
            if (cursor[copy_index] == '\0') {
                return -1;
            }
            value[copy_index] = cursor[copy_index];
        }
        value[length] = '\0';
        if (sql_store_row_value(db, &table->rows[table->row_count], value_index, value) != 0) {
            return -1;
        }
        cursor += length;
    }
    if (*cursor != '\0') {
        return -1;
    }
    table->row_count += 1U;
    return 0;
}

static int sql_parse_database_text(SqlDatabase *db, char *buffer) {
    char *cursor = buffer;
    char *line;
    SqlTable *current = 0;

    sql_clear_database(db);
    if (!sql_line_next(&cursor, &line)) {
        return 0;
    }
    if (rt_strcmp(line, "SQS1") != 0) {
        return -1;
    }

    while (sql_line_next(&cursor, &line)) {
        if (line[0] == '\0') {
            continue;
        }
        if (line[0] == 'T' && line[1] == ' ') {
            char *field_cursor = line + 2;
            char *name;
            char *column_count_text;
            unsigned long long column_count;
            unsigned int i;

            if (db->table_count >= SQL_MAX_TABLES || sql_ensure_table_capacity(db, db->table_count + 1U) != 0) {
                return -1;
            }
            name = sql_next_delimited_field(&field_cursor, ' ');
            column_count_text = sql_next_delimited_field(&field_cursor, ' ');
            if (name[0] == '\0' || rt_parse_uint(column_count_text, &column_count) != 0 || column_count == 0ULL || column_count > SQL_MAX_COLUMNS) {
                return -1;
            }
            current = &db->tables[db->table_count++];
            sql_clear_table_metadata(current);
            if (sql_copy_checked(current->name, sizeof(current->name), name) != 0) {
                return -1;
            }
            if (sql_ensure_column_capacity(current, (unsigned int)column_count) != 0) {
                return -1;
            }
            current->column_count = (unsigned int)column_count;
            for (i = 0U; i < current->column_count; ++i) {
                char *column = sql_next_delimited_field(&field_cursor, ' ');
                if (column[0] == '\0' || sql_copy_checked(current->columns[i], sizeof(current->columns[i]), column) != 0) {
                    return -1;
                }
            }
            if (*field_cursor != '\0') {
                return -1;
            }
        } else if (line[0] == 'C' && line[1] == ' ') {
            char *field_cursor = line + 2;
            char *index_text;
            char *flags_text;
            char *length_text;
            unsigned long long column_index_value;
            unsigned long long flags;
            unsigned long long length;
            char *value;

            if (current == 0) {
                return -1;
            }
            index_text = sql_next_delimited_field(&field_cursor, ' ');
            flags_text = sql_next_delimited_field(&field_cursor, ' ');
            length_text = sql_next_delimited_field(&field_cursor, ':');
            if (rt_parse_uint(index_text, &column_index_value) != 0 || column_index_value >= current->column_count ||
                rt_parse_uint(flags_text, &flags) != 0 || rt_parse_uint(length_text, &length) != 0 || length >= SQL_VALUE_SIZE) {
                return -1;
            }
            value = field_cursor;
            if (rt_strlen(value) != (size_t)length) {
                return -1;
            }
            current->not_null[column_index_value] = (flags & 1ULL) ? 1U : 0U;
            current->has_default[column_index_value] = (flags & 2ULL) ? 1U : 0U;
            current->unique[column_index_value] = (flags & 8ULL) ? 1U : 0U;
            current->primary_key[column_index_value] = (flags & 16ULL) ? 1U : 0U;
            current->column_types[column_index_value] = (flags & 64ULL) ? SQL_TYPE_REAL : ((flags & 32ULL) ? SQL_TYPE_INTEGER : SQL_TYPE_TEXT);
            if ((flags & 4ULL) != 0ULL) {
                current->defaults[column_index_value] = SQL_NULL_OFFSET;
            } else if (sql_store_value(db, value, &current->defaults[column_index_value]) != 0) {
                return -1;
            }
        } else if (line[0] == 'R' && line[1] == ' ') {
            if (current == 0 || sql_parse_database_row(db, line, current) != 0) {
                return -1;
            }
        } else if (line[0] == 'E' && line[1] == '\0') {
            current = 0;
        } else {
            return -1;
        }
    }
    return 0;
}

static void sql_line_reader_init(SqlLineReader *reader, int fd) {
    reader->fd = fd;
    reader->pos = 0U;
    reader->used = 0U;
    reader->eof = 0;
}

static int sql_line_reader_next(SqlLineReader *reader, SqlTextBuffer *line, int *read_out) {
    size_t out = 0U;

    if (line == 0 || read_out == 0) {
        return -1;
    }
    *read_out = 0;
    for (;;) {
        char ch;
        if (reader->pos >= reader->used) {
            long bytes;
            if (reader->eof) {
                if (out == 0U) {
                    if (sql_ensure_text_capacity(line, 1U, SQL_MAX_IMPORT_LINE_BYTES) != 0) {
                        return -1;
                    }
                    line->data[0] = '\0';
                    return 0;
                }
                if (sql_ensure_text_capacity(line, (unsigned int)(out + 1U), SQL_MAX_IMPORT_LINE_BYTES) != 0) {
                    return -1;
                }
                line->data[out] = '\0';
                *read_out = 1;
                return 0;
            }
            bytes = platform_read(reader->fd, reader->buffer, sizeof(reader->buffer));
            if (bytes < 0) {
                return -1;
            }
            if (bytes == 0) {
                reader->eof = 1;
                continue;
            }
            reader->pos = 0U;
            reader->used = (size_t)bytes;
        }
        ch = reader->buffer[reader->pos++];
        if (ch == '\n') {
            if (out > 0U && line->data[out - 1U] == '\r') {
                out -= 1U;
            }
            if (sql_ensure_text_capacity(line, (unsigned int)(out + 1U), SQL_MAX_IMPORT_LINE_BYTES) != 0) {
                return -1;
            }
            line->data[out] = '\0';
            *read_out = 1;
            return 0;
        }
        if (out + 2U > (size_t)SQL_MAX_IMPORT_LINE_BYTES || sql_ensure_text_capacity(line, (unsigned int)(out + 2U), SQL_MAX_IMPORT_LINE_BYTES) != 0) {
            return -1;
        }
        line->data[out++] = ch;
    }
}

static char *sql_read_text_file(const char *path);
static char *sql_next_import_field(char **cursor_io, int csv, int *ok_out);

static int sql_load_database(const char *path, SqlDatabase *db) {
    PlatformDirEntry entry;
    char *buffer;
    int result;

    sql_clear_database(db);
    if (platform_get_path_info(path, &entry) != 0) {
        return 0;
    }
    buffer = sql_read_text_file(path);
    if (buffer == 0) {
        return -1;
    }
    result = sql_parse_database_text(db, buffer);
    sql_free_bytes(buffer);
    return result;
}

static char *sql_read_text_file(const char *path) {
    PlatformDirEntry entry;
    int fd;
    size_t used = 0U;
    size_t buffer_size;
    char *buffer;

    if (platform_get_path_info(path, &entry) != 0 || entry.size > (unsigned long long)(((size_t)-1) - 1U)) {
        return 0;
    }
    buffer_size = (size_t)entry.size + 1U;
    buffer = (char *)sql_resize_bytes(0, 0U, buffer_size);
    if (buffer == 0) {
        return 0;
    }
    fd = platform_open_read(path);
    if (fd < 0) {
        sql_free_bytes(buffer);
        return 0;
    }
    while (used + 1U < buffer_size) {
        long bytes = platform_read(fd, buffer + used, buffer_size - used - 1U);
        if (bytes < 0) {
            (void)platform_close(fd);
            sql_free_bytes(buffer);
            return 0;
        }
        if (bytes == 0) {
            break;
        }
        used += (size_t)bytes;
    }
    (void)platform_close(fd);
    if (used >= buffer_size) {
        sql_free_bytes(buffer);
        return 0;
    }
    buffer[used] = '\0';
    return buffer;
}

static int sql_write_name(int fd, const char *name) {
    size_t i;

    if (name == 0 || name[0] == '\0') {
        return -1;
    }
    for (i = 0U; name[i] != '\0'; ++i) {
        if (!sql_identifier_char(name[i])) {
            return -1;
        }
    }
    return rt_write_cstr(fd, name);
}

static int sql_temp_save_path(const char *path, char *buffer, size_t buffer_size) {
    size_t length = rt_strlen(path);
    const char *suffix = ".tmp";
    size_t suffix_length = rt_strlen(suffix);

    if (length == 0U || length + suffix_length + 1U > buffer_size) {
        return -1;
    }
    memcpy(buffer, path, length);
    memcpy(buffer + length, suffix, suffix_length + 1U);
    return 0;
}

static int sql_write_database_file(const char *path, const SqlDatabase *db) {
    int fd = platform_open_write(path, 0644U);
    unsigned int table_index;

    if (fd < 0) {
        return -1;
    }
    if (rt_write_cstr(fd, "SQS1\n") != 0) {
        (void)platform_close(fd);
        return -1;
    }
    for (table_index = 0U; table_index < db->table_count; ++table_index) {
        const SqlTable *table = &db->tables[table_index];
        unsigned int column_index;
        unsigned int row_index;

        if (rt_write_cstr(fd, "T ") != 0 ||
            sql_write_name(fd, table->name) != 0 ||
            rt_write_char(fd, ' ') != 0 ||
            rt_write_uint(fd, table->column_count) != 0) {
            (void)platform_close(fd);
            return -1;
        }
        for (column_index = 0U; column_index < table->column_count; ++column_index) {
            if (rt_write_char(fd, ' ') != 0 || sql_write_name(fd, table->columns[column_index]) != 0) {
                (void)platform_close(fd);
                return -1;
            }
        }
        if (rt_write_char(fd, '\n') != 0) {
            (void)platform_close(fd);
            return -1;
        }
        for (column_index = 0U; column_index < table->column_count; ++column_index) {
            if (table->not_null[column_index] || table->has_default[column_index] || table->unique[column_index] || table->primary_key[column_index] || table->column_types[column_index] != SQL_TYPE_TEXT) {
                unsigned int flags = 0U;
                const char *default_value = "";
                if (table->not_null[column_index]) {
                    flags |= 1U;
                }
                if (table->unique[column_index]) {
                    flags |= 8U;
                }
                if (table->primary_key[column_index]) {
                    flags |= 16U;
                }
                if (table->column_types[column_index] == SQL_TYPE_INTEGER) {
                    flags |= 32U;
                } else if (table->column_types[column_index] == SQL_TYPE_REAL) {
                    flags |= 64U;
                }
                if (table->has_default[column_index]) {
                    flags |= 2U;
                    if (sql_offset_is_null(table->defaults[column_index])) {
                        flags |= 4U;
                    } else {
                        default_value = sql_value_at(table->defaults[column_index]);
                    }
                }
                if (rt_write_cstr(fd, "C ") != 0 ||
                    rt_write_uint(fd, column_index) != 0 ||
                    rt_write_char(fd, ' ') != 0 ||
                    rt_write_uint(fd, flags) != 0 ||
                    rt_write_char(fd, ' ') != 0 ||
                    rt_write_uint(fd, rt_strlen(default_value)) != 0 ||
                    rt_write_char(fd, ':') != 0 ||
                    rt_write_cstr(fd, default_value) != 0 ||
                    rt_write_char(fd, '\n') != 0) {
                    (void)platform_close(fd);
                    return -1;
                }
            }
        }
        for (row_index = 0U; row_index < table->row_count; ++row_index) {
            if (rt_write_cstr(fd, "R ") != 0) {
                (void)platform_close(fd);
                return -1;
            }
            for (column_index = 0U; column_index < table->column_count; ++column_index) {
                const char *value = sql_row_value(&table->rows[row_index], column_index);
                if (sql_row_value_is_null(&table->rows[row_index], column_index)) {
                    if (rt_write_cstr(fd, "N:") != 0) {
                        (void)platform_close(fd);
                        return -1;
                    }
                    continue;
                }
                if (rt_write_uint(fd, rt_strlen(value)) != 0 || rt_write_char(fd, ':') != 0 || rt_write_cstr(fd, value) != 0) {
                    (void)platform_close(fd);
                    return -1;
                }
            }
            if (rt_write_char(fd, '\n') != 0) {
                (void)platform_close(fd);
                return -1;
            }
        }
        if (rt_write_cstr(fd, "E\n") != 0) {
            (void)platform_close(fd);
            return -1;
        }
    }
    return platform_close(fd);
}

static int sql_save_database(const char *path, const SqlDatabase *db) {
    char tmp_path[SQL_PATH_SIZE];

    if (sql_temp_save_path(path, tmp_path, sizeof(tmp_path)) != 0) {
        return -1;
    }
    (void)platform_remove_file(tmp_path);
    if (sql_write_database_file(tmp_path, db) != 0) {
        (void)platform_remove_file(tmp_path);
        return -1;
    }
    if (platform_sync_path_data(tmp_path) != 0) {
        (void)platform_remove_file(tmp_path);
        return -1;
    }
    if (platform_rename_path(tmp_path, path) != 0) {
        (void)platform_remove_file(tmp_path);
        return -1;
    }
    (void)platform_sync_path(path);
    return 0;
}
