static int sql_execute_create(SqlDatabase *db, SqlParser *parser) {
    SqlTable *table;
    char table_name[SQL_NAME_SIZE];
    char columns[SQL_MAX_COLUMNS][SQL_NAME_SIZE];
    unsigned char column_types[SQL_MAX_COLUMNS];
    unsigned char not_null[SQL_MAX_COLUMNS];
    unsigned char has_default[SQL_MAX_COLUMNS];
    unsigned char default_is_null[SQL_MAX_COLUMNS];
    unsigned char unique[SQL_MAX_COLUMNS];
    unsigned char primary_key[SQL_MAX_COLUMNS];
    char defaults[SQL_MAX_COLUMNS][SQL_VALUE_SIZE];
    unsigned int column_count = 0U;
    unsigned int copy_index;
    unsigned int primary_key_count = 0U;
    int if_not_exists = 0;

    if (!sql_expect_word(parser, "table")) {
        return -1;
    }
    if (sql_try_word(parser, "if")) {
        if (!sql_expect_word(parser, "not") || !sql_expect_word(parser, "exists")) {
            return -1;
        }
        if_not_exists = 1;
    }
    if (sql_read_identifier(parser, table_name, sizeof(table_name)) != 0 || !sql_expect_symbol(parser, '(')) {
        return -1;
    }
    for (;;) {
        int done_column = 0;
        if (column_count >= SQL_MAX_COLUMNS || sql_read_identifier(parser, columns[column_count], sizeof(columns[column_count])) != 0 || sql_column_seen(columns, column_count, columns[column_count])) {
            return -1;
        }
        not_null[column_count] = 0U;
        column_types[column_count] = SQL_TYPE_TEXT;
        has_default[column_count] = 0U;
        default_is_null[column_count] = 0U;
        unique[column_count] = 0U;
        primary_key[column_count] = 0U;
        defaults[column_count][0] = '\0';
        while (!done_column) {
            if (sql_next_token(parser) != 0) {
                return -1;
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
                if (column_types[column_count] != SQL_TYPE_TEXT) {
                    return -1;
                }
                column_types[column_count] = (unsigned char)sql_column_type_from_name(parser->token);
            } else if (parser->token_type == SQL_TOKEN_WORD && sql_equal_ignore_case(parser->token, "not")) {
                if (!sql_expect_word(parser, "null")) {
                    return -1;
                }
                not_null[column_count] = 1U;
            } else if (parser->token_type == SQL_TOKEN_WORD && sql_equal_ignore_case(parser->token, "primary")) {
                if (!sql_expect_word(parser, "key")) {
                    return -1;
                }
                primary_key[column_count] = 1U;
                unique[column_count] = 1U;
                not_null[column_count] = 1U;
            } else if (parser->token_type == SQL_TOKEN_WORD && sql_equal_ignore_case(parser->token, "unique")) {
                unique[column_count] = 1U;
            } else if (parser->token_type == SQL_TOKEN_WORD && sql_equal_ignore_case(parser->token, "default")) {
                int is_null = 0;
                if (has_default[column_count] || sql_read_value_or_null(parser, defaults[column_count], sizeof(defaults[column_count]), &is_null) != 0) {
                    return -1;
                }
                has_default[column_count] = 1U;
                default_is_null[column_count] = is_null ? 1U : 0U;
            } else {
                return -1;
            }
        }
        if (parser->token_type == SQL_TOKEN_SYMBOL && parser->token[0] == ')') {
            break;
        }
    }
    if (!sql_at_end(parser)) {
        return -1;
    }
    for (copy_index = 0U; copy_index < column_count; ++copy_index) {
        if (primary_key[copy_index]) {
            primary_key_count += 1U;
        }
    }
    if (primary_key_count > 1U) {
        return -1;
    }
    if (sql_find_table(db, table_name) != 0) {
        if (!if_not_exists) {
            return -1;
        }
        rt_write_line(1, "ok");
        return 0;
    }
    if (db->table_count >= SQL_MAX_TABLES || sql_ensure_table_capacity(db, db->table_count + 1U) != 0) {
        return -1;
    }
    table = &db->tables[db->table_count];
    sql_clear_table_metadata(table);
    if (sql_copy_checked(table->name, sizeof(table->name), table_name) != 0) {
        return -1;
    }
    for (copy_index = 0U; copy_index < column_count; ++copy_index) {
        if (sql_copy_checked(table->columns[copy_index], sizeof(table->columns[copy_index]), columns[copy_index]) != 0) {
            return -1;
        }
        table->not_null[copy_index] = not_null[copy_index];
        table->column_types[copy_index] = column_types[copy_index];
        table->has_default[copy_index] = has_default[copy_index];
        table->unique[copy_index] = unique[copy_index];
        table->primary_key[copy_index] = primary_key[copy_index];
        if (has_default[copy_index]) {
            if (default_is_null[copy_index]) {
                table->defaults[copy_index] = SQL_NULL_OFFSET;
            } else if (sql_store_value(db, defaults[copy_index], &table->defaults[copy_index]) != 0) {
                return -1;
            }
        }
        if (table->not_null[copy_index] && table->has_default[copy_index] && sql_offset_is_null(table->defaults[copy_index])) {
            return -1;
        }
        if (table->primary_key[copy_index] && table->has_default[copy_index] && sql_offset_is_null(table->defaults[copy_index])) {
            return -1;
        }
        if (table->has_default[copy_index] && !sql_offset_is_null(table->defaults[copy_index]) && sql_validate_typed_value(table->column_types[copy_index], 0, sql_value_at(table->defaults[copy_index])) != 0) {
            return -1;
        }
    }
    db->table_count += 1U;
    table->column_count = column_count;
    sql_invalidate_runtime_caches();
    rt_write_line(1, "ok");
    return 1;
}

static int sql_execute_insert(SqlDatabase *db, SqlParser *parser) {
    char table_name[SQL_NAME_SIZE];
    SqlTable *table;
    int target_columns[SQL_MAX_COLUMNS];
    char values[SQL_MAX_COLUMNS][SQL_VALUE_SIZE];
    int value_is_null[SQL_MAX_COLUMNS];
    unsigned int target_count = 0U;
    unsigned int value_count = 0U;
    unsigned int inserted = 0U;

    if (!sql_expect_word(parser, "into") || sql_read_identifier(parser, table_name, sizeof(table_name)) != 0) {
        return -1;
    }
    table = sql_find_table(db, table_name);
    if (table == 0 || table->row_count >= SQL_MAX_ROWS) {
        return -1;
    }
    if (sql_try_symbol(parser, '(')) {
        for (;;) {
            char column_name[SQL_NAME_SIZE];
            int column;
            unsigned int i;

            if (target_count >= table->column_count || sql_read_identifier(parser, column_name, sizeof(column_name)) != 0) {
                return -1;
            }
            column = sql_find_column(table, column_name);
            if (column < 0) {
                return -1;
            }
            for (i = 0U; i < target_count; ++i) {
                if (target_columns[i] == column) {
                    return -1;
                }
            }
            target_columns[target_count++] = column;
            if (sql_next_token(parser) != 0) {
                return -1;
            }
            if (parser->token_type == SQL_TOKEN_SYMBOL && parser->token[0] == ')') {
                break;
            }
            if (parser->token_type != SQL_TOKEN_SYMBOL || parser->token[0] != ',') {
                return -1;
            }
        }
    } else {
        for (target_count = 0U; target_count < table->column_count; ++target_count) {
            target_columns[target_count] = (int)target_count;
        }
    }
    if (target_count == 0U || !sql_expect_word(parser, "values") || !sql_expect_symbol(parser, '(')) {
        return -1;
    }
    for (;;) {
        value_count = 0U;
        for (;;) {
            if (value_count >= target_count || sql_read_value_or_null(parser, values[value_count], SQL_VALUE_SIZE, &value_is_null[value_count]) != 0) {
                return -1;
            }
            value_count += 1U;
            if (sql_next_token(parser) != 0) {
                return -1;
            }
            if (parser->token_type == SQL_TOKEN_SYMBOL && parser->token[0] == ')') {
                break;
            }
            if (parser->token_type != SQL_TOKEN_SYMBOL || parser->token[0] != ',') {
                return -1;
            }
        }
        if (value_count != target_count || table->row_count >= SQL_MAX_ROWS || sql_ensure_row_capacity(table, table->row_count + 1U) != 0 || sql_prepare_new_row(table, &table->rows[table->row_count]) != 0) {
            return -1;
        }
        for (value_count = 0U; value_count < target_count; ++value_count) {
            if (sql_store_row_value_or_null(db, &table->rows[table->row_count], (unsigned int)target_columns[value_count], values[value_count], value_is_null[value_count]) != 0) {
                return -1;
            }
        }
        if (sql_validate_row_constraints(table, &table->rows[table->row_count]) != 0) {
            return -1;
        }
        table->row_count += 1U;
        inserted += 1U;
        if (sql_next_token(parser) != 0) {
            return -1;
        }
        if (parser->token_type == SQL_TOKEN_END) {
            break;
        }
        if (parser->token_type != SQL_TOKEN_SYMBOL || parser->token[0] != ',') {
            return -1;
        }
        if (!sql_expect_symbol(parser, '(')) {
            return -1;
        }
    }
    if (inserted == 1U) {
        rt_write_line(1, "ok");
    } else {
        sql_write_row_count(inserted);
    }
    sql_invalidate_table_runtime_caches(table);
    return 1;
}

