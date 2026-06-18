#include "internal.h"

typedef struct {
    char (*columns)[SQL_NAME_SIZE];
    unsigned char *column_types;
    unsigned char *not_null;
    unsigned char *has_default;
    unsigned char *default_is_null;
    unsigned char *unique;
    unsigned char *primary_key;
    char (*defaults)[SQL_VALUE_SIZE];
    unsigned int capacity;
} SqlCreateScratch;

static int sql_ensure_create_scratch_capacity(SqlCreateScratch *scratch, unsigned int needed) {
    char (*columns)[SQL_NAME_SIZE];
    unsigned char *column_types;
    unsigned char *not_null;
    unsigned char *has_default;
    unsigned char *default_is_null;
    unsigned char *unique;
    unsigned char *primary_key;
    char (*defaults)[SQL_VALUE_SIZE];
    unsigned int capacity;

    if (scratch == 0 || sql_next_capacity(scratch->capacity, needed, SQL_MAX_COLUMNS, SQL_INITIAL_COLUMN_CAPACITY, &capacity) != 0) {
        return -1;
    }
    if (capacity == scratch->capacity) {
        return 0;
    }
    columns = (char (*)[SQL_NAME_SIZE])sql_resize_array(scratch->columns, scratch->capacity, capacity, sizeof(char[SQL_NAME_SIZE]));
    if (columns == 0) {
        return -1;
    }
    scratch->columns = columns;
    column_types = (unsigned char *)sql_resize_array(scratch->column_types, scratch->capacity, capacity, sizeof(unsigned char));
    if (column_types == 0) {
        return -1;
    }
    scratch->column_types = column_types;
    not_null = (unsigned char *)sql_resize_array(scratch->not_null, scratch->capacity, capacity, sizeof(unsigned char));
    if (not_null == 0) {
        return -1;
    }
    scratch->not_null = not_null;
    has_default = (unsigned char *)sql_resize_array(scratch->has_default, scratch->capacity, capacity, sizeof(unsigned char));
    if (has_default == 0) {
        return -1;
    }
    scratch->has_default = has_default;
    default_is_null = (unsigned char *)sql_resize_array(scratch->default_is_null, scratch->capacity, capacity, sizeof(unsigned char));
    if (default_is_null == 0) {
        return -1;
    }
    scratch->default_is_null = default_is_null;
    unique = (unsigned char *)sql_resize_array(scratch->unique, scratch->capacity, capacity, sizeof(unsigned char));
    if (unique == 0) {
        return -1;
    }
    scratch->unique = unique;
    primary_key = (unsigned char *)sql_resize_array(scratch->primary_key, scratch->capacity, capacity, sizeof(unsigned char));
    if (primary_key == 0) {
        return -1;
    }
    scratch->primary_key = primary_key;
    defaults = (char (*)[SQL_VALUE_SIZE])sql_resize_array(scratch->defaults, scratch->capacity, capacity, sizeof(char[SQL_VALUE_SIZE]));
    if (defaults == 0) {
        return -1;
    }
    scratch->defaults = defaults;
    scratch->capacity = capacity;
    return 0;
}

static void sql_free_create_scratch(SqlCreateScratch *scratch) {
    if (scratch == 0) {
        return;
    }
    sql_free_bytes(scratch->columns);
    sql_free_bytes(scratch->column_types);
    sql_free_bytes(scratch->not_null);
    sql_free_bytes(scratch->has_default);
    sql_free_bytes(scratch->default_is_null);
    sql_free_bytes(scratch->unique);
    sql_free_bytes(scratch->primary_key);
    sql_free_bytes(scratch->defaults);
    rt_memset(scratch, 0, sizeof(*scratch));
}

static int sql_execute_create(SqlDatabase *db, SqlParser *parser) {
    SqlTable *table;
    char table_name[SQL_NAME_SIZE];
    SqlCreateScratch scratch;
    unsigned int column_count = 0U;
    unsigned int copy_index;
    unsigned int primary_key_count = 0U;
    int if_not_exists = 0;
    int table_reserved = 0;
    int result = -1;

    rt_memset(&scratch, 0, sizeof(scratch));

    if (!sql_expect_word(parser, "table")) {
        goto out;
    }
    if (sql_try_word(parser, "if")) {
        if (!sql_expect_word(parser, "not") || !sql_expect_word(parser, "exists")) {
            goto out;
        }
        if_not_exists = 1;
    }
    if (sql_read_identifier(parser, table_name, sizeof(table_name)) != 0 || !sql_expect_symbol(parser, '(')) {
        goto out;
    }
    for (;;) {
        int done_column = 0;
        if (column_count >= SQL_MAX_COLUMNS || sql_ensure_create_scratch_capacity(&scratch, column_count + 1U) != 0 ||
            sql_read_identifier(parser, scratch.columns[column_count], sizeof(scratch.columns[column_count])) != 0 ||
            sql_column_seen(scratch.columns, column_count, scratch.columns[column_count])) {
            goto out;
        }
        scratch.not_null[column_count] = 0U;
        scratch.column_types[column_count] = SQL_TYPE_TEXT;
        scratch.has_default[column_count] = 0U;
        scratch.default_is_null[column_count] = 0U;
        scratch.unique[column_count] = 0U;
        scratch.primary_key[column_count] = 0U;
        scratch.defaults[column_count][0] = '\0';
        while (!done_column) {
            if (sql_next_token(parser) != 0) {
                goto out;
            }
            if (parser->token_type == SQL_TOKEN_SYMBOL && parser->token[0] == ')') {
                done_column = 1;
                column_count += 1U;
                break;
            }
            if (parser->token_type == SQL_TOKEN_SYMBOL && parser->token[0] == ',') {
                done_column = 1;
                column_count += 1U;
                break;
            }
            if (parser->token_type == SQL_TOKEN_WORD && sql_column_type_from_name(parser->token) >= 0) {
                if (scratch.column_types[column_count] != SQL_TYPE_TEXT) {
                    goto out;
                }
                scratch.column_types[column_count] = (unsigned char)sql_column_type_from_name(parser->token);
            } else if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "not")) {
                if (!sql_expect_word(parser, "null")) {
                    goto out;
                }
                scratch.not_null[column_count] = 1U;
            } else if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "primary")) {
                if (!sql_expect_word(parser, "key")) {
                    goto out;
                }
                scratch.primary_key[column_count] = 1U;
                scratch.unique[column_count] = 1U;
                scratch.not_null[column_count] = 1U;
            } else if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "unique")) {
                scratch.unique[column_count] = 1U;
            } else if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "default")) {
                int is_null = 0;
                if (scratch.has_default[column_count] || sql_read_value_or_null(parser, scratch.defaults[column_count], sizeof(scratch.defaults[column_count]), &is_null) != 0) {
                    goto out;
                }
                scratch.has_default[column_count] = 1U;
                scratch.default_is_null[column_count] = is_null ? 1U : 0U;
            } else {
                goto out;
            }
        }
        if (parser->token_type == SQL_TOKEN_SYMBOL && parser->token[0] == ')') {
            break;
        }
    }
    if (!sql_at_end(parser)) {
        goto out;
    }
    for (copy_index = 0U; copy_index < column_count; ++copy_index) {
        if (scratch.primary_key[copy_index]) {
            primary_key_count += 1U;
        }
    }
    if (primary_key_count > 1U) {
        goto out;
    }
    if (sql_find_table(db, table_name) != 0) {
        if (!if_not_exists) {
            goto out;
        }
        rt_write_line(1, "ok");
        result = 0;
        goto out;
    }
    if (db->table_count >= SQL_MAX_TABLES || sql_ensure_table_capacity(db, db->table_count + 1U) != 0) {
        goto out;
    }
    table = &db->tables[db->table_count];
    table_reserved = 1;
    sql_clear_table_metadata(table);
    if (sql_copy_checked(table->name, sizeof(table->name), table_name) != 0) {
        goto out;
    }
    if (sql_ensure_column_capacity(table, column_count) != 0) {
        goto out;
    }
    for (copy_index = 0U; copy_index < column_count; ++copy_index) {
        if (sql_copy_checked(table->columns[copy_index], sizeof(table->columns[copy_index]), scratch.columns[copy_index]) != 0) {
            goto out;
        }
        table->not_null[copy_index] = scratch.not_null[copy_index];
        table->column_types[copy_index] = scratch.column_types[copy_index];
        table->has_default[copy_index] = scratch.has_default[copy_index];
        table->unique[copy_index] = scratch.unique[copy_index];
        table->primary_key[copy_index] = scratch.primary_key[copy_index];
        if (scratch.has_default[copy_index]) {
            if (scratch.default_is_null[copy_index]) {
                table->defaults[copy_index] = SQL_NULL_OFFSET;
            } else if (sql_store_value(db, scratch.defaults[copy_index], &table->defaults[copy_index]) != 0) {
                goto out;
            }
        }
        if (table->not_null[copy_index] && table->has_default[copy_index] && sql_offset_is_null(table->defaults[copy_index])) {
            goto out;
        }
        if (table->primary_key[copy_index] && table->has_default[copy_index] && sql_offset_is_null(table->defaults[copy_index])) {
            goto out;
        }
        if (table->has_default[copy_index] && !sql_offset_is_null(table->defaults[copy_index]) && sql_validate_typed_value(table->column_types[copy_index], 0, sql_value_at(table->defaults[copy_index])) != 0) {
            goto out;
        }
    }
    db->table_count += 1U;
    table_reserved = 0;
    table->column_count = column_count;
    sql_invalidate_runtime_caches();
    rt_write_line(1, "ok");
    result = 1;

out:
    if (result < 0 && table_reserved) {
        sql_clear_table_metadata(table);
    }
    sql_free_create_scratch(&scratch);
    return result;
}

static int sql_execute_insert(SqlDatabase *db, SqlParser *parser) {
    char table_name[SQL_NAME_SIZE];
    SqlTable *table;
    int *target_columns = 0;
    char (*values)[SQL_VALUE_SIZE] = 0;
    int *value_is_null = 0;
    unsigned int target_count = 0U;
    unsigned int value_count = 0U;
    unsigned int inserted = 0U;
    int result = -1;

    if (!sql_expect_word(parser, "into") || sql_read_identifier(parser, table_name, sizeof(table_name)) != 0) {
        return -1;
    }
    table = sql_find_table(db, table_name);
    if (table == 0 || table->row_count >= SQL_MAX_ROWS) {
        return -1;
    }
    target_columns = (int *)sql_resize_array(0, 0U, table->column_count, sizeof(int));
    values = (char (*)[SQL_VALUE_SIZE])sql_resize_array(0, 0U, table->column_count, sizeof(char[SQL_VALUE_SIZE]));
    value_is_null = (int *)sql_resize_array(0, 0U, table->column_count, sizeof(int));
    if (target_columns == 0 || values == 0 || value_is_null == 0) {
        goto out;
    }
    if (sql_try_symbol(parser, '(')) {
        for (;;) {
            char column_name[SQL_NAME_SIZE];
            int column;
            unsigned int i;

            if (target_count >= table->column_count || sql_read_identifier(parser, column_name, sizeof(column_name)) != 0) {
                goto out;
            }
            column = sql_find_column(table, column_name);
            if (column < 0) {
                goto out;
            }
            for (i = 0U; i < target_count; ++i) {
                if (target_columns[i] == column) {
                    goto out;
                }
            }
            target_columns[target_count++] = column;
            if (sql_next_token(parser) != 0) {
                goto out;
            }
            if (parser->token_type == SQL_TOKEN_SYMBOL && parser->token[0] == ')') {
                break;
            }
            if (parser->token_type != SQL_TOKEN_SYMBOL || parser->token[0] != ',') {
                goto out;
            }
        }
    } else {
        if (table->column_count > SQL_MAX_COLUMNS) {
            goto out;
        }
        for (target_count = 0U; target_count < table->column_count; ++target_count) {
            target_columns[target_count] = (int)target_count;
        }
    }
    if (target_count == 0U || !sql_expect_word(parser, "values") || !sql_expect_symbol(parser, '(')) {
        goto out;
    }
    for (;;) {
        value_count = 0U;
        for (;;) {
            if (value_count >= target_count || sql_read_value_or_null(parser, values[value_count], SQL_VALUE_SIZE, &value_is_null[value_count]) != 0) {
                goto out;
            }
            value_count += 1U;
            if (sql_next_token(parser) != 0) {
                goto out;
            }
            if (parser->token_type == SQL_TOKEN_SYMBOL && parser->token[0] == ')') {
                break;
            }
            if (parser->token_type != SQL_TOKEN_SYMBOL || parser->token[0] != ',') {
                goto out;
            }
        }
        if (value_count != target_count || table->row_count >= SQL_MAX_ROWS || sql_ensure_row_capacity(table, table->row_count + 1U) != 0 || sql_prepare_new_row(table, &table->rows[table->row_count]) != 0) {
            goto out;
        }
        for (value_count = 0U; value_count < target_count; ++value_count) {
            if (sql_store_row_value_or_null(db, &table->rows[table->row_count], (unsigned int)target_columns[value_count], values[value_count], value_is_null[value_count]) != 0) {
                goto out;
            }
        }
        if (sql_validate_row_constraints(table, &table->rows[table->row_count]) != 0) {
            goto out;
        }
        table->row_count += 1U;
        inserted += 1U;
        if (sql_next_token(parser) != 0) {
            goto out;
        }
        if (parser->token_type == SQL_TOKEN_END) {
            break;
        }
        if (parser->token_type != SQL_TOKEN_SYMBOL || parser->token[0] != ',') {
            goto out;
        }
        if (!sql_expect_symbol(parser, '(')) {
            goto out;
        }
    }
    if (inserted == 1U) {
        rt_write_line(1, "ok");
    } else {
        sql_write_row_count(inserted);
    }
    sql_invalidate_table_runtime_caches(table);
    result = 1;

out:
    sql_free_bytes(target_columns);
    sql_free_bytes(values);
    sql_free_bytes(value_is_null);
    return result;
}

