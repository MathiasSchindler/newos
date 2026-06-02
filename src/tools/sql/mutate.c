#include "internal.h"

static int sql_parse_assignment(SqlParser *parser, const SqlTable *table, SqlAssignment *assignment) {
    char column_name[SQL_NAME_SIZE];
    char first_value[SQL_VALUE_SIZE];
    size_t saved;

    rt_memset(assignment, 0, sizeof(*assignment));
    assignment->kind = SQL_ASSIGN_LITERAL;
    assignment->source_column = -1;
    if (sql_read_identifier(parser, column_name, sizeof(column_name)) != 0 || !sql_expect_symbol(parser, '=')) {
        return -1;
    }
    assignment->column = sql_find_column(table, column_name);
    if (assignment->column < 0 || sql_read_value_or_null(parser, first_value, sizeof(first_value), &assignment->is_null) != 0) {
        return -1;
    }
    if (sql_copy_checked(assignment->value, sizeof(assignment->value), first_value) != 0) {
        return -1;
    }
    saved = parser->pos;
    if (sql_next_token(parser) == 0 &&
        ((parser->token_type == SQL_TOKEN_SYMBOL && parser->token[0] == '+') ||
         (parser->token_type == SQL_TOKEN_WORD && rt_strcmp(parser->token, "-") == 0))) {
        int source_column = sql_find_column(table, first_value);
        assignment->kind = parser->token[0] == '+' ? SQL_ASSIGN_ADD : SQL_ASSIGN_SUB;
        if (source_column < 0 || sql_read_value(parser, assignment->value, sizeof(assignment->value)) != 0) {
            return -1;
        }
        assignment->source_column = source_column;
        return 0;
    }
    parser->pos = saved;
    return 0;
}

static int sql_apply_assignment(SqlDatabase *db, SqlRow *row, const SqlAssignment *assignment) {
    if (assignment->kind == SQL_ASSIGN_LITERAL) {
        return sql_store_row_value_or_null(db, row, (unsigned int)assignment->column, assignment->value, assignment->is_null);
    }
    {
        unsigned long long left;
        unsigned long long right;
        unsigned long long result;

        if (rt_parse_uint(sql_row_value(row, (unsigned int)assignment->source_column), &left) != 0 || rt_parse_uint(assignment->value, &right) != 0) {
            return -1;
        }
        if (assignment->kind == SQL_ASSIGN_ADD) {
            result = left + right;
        } else {
            if (left < right) {
                return -1;
            }
            result = left - right;
        }
        {
            char value[SQL_VALUE_SIZE];
            sql_store_uint_text(result, value, sizeof(value));
            if (sql_store_row_value(db, row, (unsigned int)assignment->column, value) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int sql_execute_update(SqlDatabase *db, SqlParser *parser) {
    char table_name[SQL_NAME_SIZE];
    SqlTable *table;
    SqlAssignment assignments[SQL_SET_CAPACITY];
    unsigned int assignment_count = 0U;
    SqlConditionList where;
    unsigned int changed = 0U;
    unsigned int row_index;

    rt_memset(&where, 0, sizeof(where));

    if (sql_read_identifier(parser, table_name, sizeof(table_name)) != 0 || !sql_expect_word(parser, "set")) {
        return -1;
    }
    table = sql_find_table(db, table_name);
    if (table == 0) {
        return -1;
    }
    for (;;) {
        if (assignment_count >= SQL_SET_CAPACITY || sql_parse_assignment(parser, table, &assignments[assignment_count]) != 0) {
            return -1;
        }
        assignment_count += 1U;
        if (sql_next_token(parser) != 0) {
            return -1;
        }
        if (parser->token_type == SQL_TOKEN_END) {
            break;
        }
        if (parser->token_type == SQL_TOKEN_WORD && sql_equal_ignore_case(parser->token, "where")) {
            if (sql_parse_row_condition_list(parser, table, &where) != 0 || !sql_at_end(parser)) {
                return -1;
            }
            break;
        }
        if (parser->token_type != SQL_TOKEN_SYMBOL || parser->token[0] != ',') {
            return -1;
        }
    }
    for (row_index = 0U; row_index < table->row_count; ++row_index) {
        unsigned int assignment_index;
        if (!sql_row_condition_list_matches(&table->rows[row_index], &where)) {
            continue;
        }
        for (assignment_index = 0U; assignment_index < assignment_count; ++assignment_index) {
            if (sql_apply_assignment(db, &table->rows[row_index], &assignments[assignment_index]) != 0) {
                return -1;
            }
        }
        if (sql_validate_row_constraints(table, &table->rows[row_index]) != 0) {
            return -1;
        }
        changed += 1U;
    }
    if (changed > 0U) {
        sql_invalidate_table_runtime_caches(table);
    }
    sql_write_row_count(changed);
    return 1;
}

static int sql_execute_delete(SqlDatabase *db, SqlParser *parser) {
    char table_name[SQL_NAME_SIZE];
    SqlTable *table;
    SqlConditionList where;
    unsigned int read_index;
    unsigned int write_index = 0U;
    unsigned int changed;

    if (!sql_expect_word(parser, "from") || sql_read_identifier(parser, table_name, sizeof(table_name)) != 0) {
        return -1;
    }
    table = sql_find_table(db, table_name);
    if (table == 0 || sql_parse_where(parser, table, &where) != 0) {
        return -1;
    }
    for (read_index = 0U; read_index < table->row_count; ++read_index) {
        if (sql_row_condition_list_matches(&table->rows[read_index], &where)) {
            continue;
        }
        if (write_index != read_index) {
            sql_copy_row_values(table, &table->rows[write_index], &table->rows[read_index]);
        }
        write_index += 1U;
    }
    changed = table->row_count - write_index;
    table->row_count = write_index;
    if (changed > 0U) {
        sql_invalidate_table_runtime_caches(table);
    }
    sql_write_row_count(changed);
    return 1;
}

static int sql_execute_drop(SqlDatabase *db, SqlParser *parser) {
    char table_name[SQL_NAME_SIZE];
    unsigned int i;
    int if_exists = 0;

    if (!sql_expect_word(parser, "table")) {
        return -1;
    }
    if (sql_try_word(parser, "if")) {
        if (!sql_expect_word(parser, "exists")) {
            return -1;
        }
        if_exists = 1;
    }
    if (sql_read_identifier(parser, table_name, sizeof(table_name)) != 0 || !sql_at_end(parser)) {
        return -1;
    }
    for (i = 0U; i < db->table_count; ++i) {
        if (sql_equal_ignore_case(db->tables[i].name, table_name)) {
            unsigned int move;
            sql_free_table_storage(&db->tables[i]);
            for (move = i + 1U; move < db->table_count; ++move) {
                db->tables[move - 1U] = db->tables[move];
            }
            db->table_count -= 1U;
            if (db->table_count < db->table_capacity) {
                memset(&db->tables[db->table_count], 0, sizeof(db->tables[db->table_count]));
            }
            sql_invalidate_runtime_caches();
            rt_write_line(1, "ok");
            return 1;
        }
    }
    if (if_exists) {
        rt_write_line(1, "ok");
        return 0;
    }
    return -1;
}

static int sql_execute_schema(SqlDatabase *db, SqlParser *parser) {
    unsigned int table_index;

    if (!sql_at_end(parser)) {
        return -1;
    }
    for (table_index = 0U; table_index < db->table_count; ++table_index) {
        SqlTable *table = &db->tables[table_index];
        unsigned int column_index;

        rt_write_cstr(1, "CREATE TABLE ");
        rt_write_cstr(1, table->name);
        rt_write_cstr(1, " (");
        for (column_index = 0U; column_index < table->column_count; ++column_index) {
            if (column_index > 0U) {
                rt_write_cstr(1, ", ");
            }
            rt_write_cstr(1, table->columns[column_index]);
            if (table->column_types[column_index] != SQL_TYPE_TEXT) {
                rt_write_char(1, ' ');
                rt_write_cstr(1, sql_column_type_name(table->column_types[column_index]));
            }
            if (table->primary_key[column_index]) {
                rt_write_cstr(1, " PRIMARY KEY");
            } else if (table->unique[column_index]) {
                rt_write_cstr(1, " UNIQUE");
            }
            if (table->not_null[column_index]) {
                rt_write_cstr(1, " NOT NULL");
            }
            if (table->has_default[column_index]) {
                rt_write_cstr(1, " DEFAULT ");
                if (sql_offset_is_null(table->defaults[column_index])) {
                    rt_write_cstr(1, "NULL");
                } else if (sql_value_at(table->defaults[column_index])[0] == '\0') {
                    rt_write_cstr(1, "''");
                } else {
                    rt_write_cstr(1, sql_value_at(table->defaults[column_index]));
                }
            }
        }
        rt_write_line(1, ");");
    }
    return 0;
}

static int sql_execute_alter(SqlDatabase *db, SqlParser *parser) {
    char table_name[SQL_NAME_SIZE];
    char column_name[SQL_NAME_SIZE];
    SqlTable *table;
    unsigned int row_index;
    int not_null = 0;
    int has_default = 0;
    int default_is_null = 0;
    int unique = 0;
    int primary_key = 0;
    int column_type = SQL_TYPE_TEXT;
    char default_value[SQL_VALUE_SIZE];
    unsigned int default_offset = 0U;

    if (!sql_expect_word(parser, "table") ||
        sql_read_identifier(parser, table_name, sizeof(table_name)) != 0) {
        return -1;
    }
    table = sql_find_table(db, table_name);
    if (table == 0) {
        return -1;
    }
    if (sql_try_word(parser, "rename")) {
        if (sql_try_word(parser, "column")) {
            char new_column_name[SQL_NAME_SIZE];
            int column;

            if (sql_read_identifier(parser, column_name, sizeof(column_name)) != 0 ||
                !sql_expect_word(parser, "to") ||
                sql_read_identifier(parser, new_column_name, sizeof(new_column_name)) != 0 ||
                !sql_at_end(parser)) {
                return -1;
            }
            column = sql_find_column(table, column_name);
            if (column < 0 || sql_find_column(table, new_column_name) >= 0 || sql_copy_checked(table->columns[column], sizeof(table->columns[column]), new_column_name) != 0) {
                return -1;
            }
        } else {
            char new_table_name[SQL_NAME_SIZE];
            if (!sql_expect_word(parser, "to") ||
                sql_read_identifier(parser, new_table_name, sizeof(new_table_name)) != 0 ||
                !sql_at_end(parser) ||
                sql_find_table(db, new_table_name) != 0 ||
                sql_copy_checked(table->name, sizeof(table->name), new_table_name) != 0) {
                return -1;
            }
        }
        sql_invalidate_table_runtime_caches(table);
        rt_write_line(1, "ok");
        return 1;
    }
    if (sql_try_word(parser, "drop")) {
        int column;
        unsigned int column_index;

        (void)sql_try_word(parser, "column");
        if (sql_read_identifier(parser, column_name, sizeof(column_name)) != 0 || !sql_at_end(parser)) {
            return -1;
        }
        column = sql_find_column(table, column_name);
        if (column < 0 || table->column_count <= 1U) {
            return -1;
        }
        for (column_index = (unsigned int)column + 1U; column_index < table->column_count; ++column_index) {
            unsigned int row_move;
            if (sql_copy_checked(table->columns[column_index - 1U], sizeof(table->columns[column_index - 1U]), table->columns[column_index]) != 0) {
                return -1;
            }
            table->not_null[column_index - 1U] = table->not_null[column_index];
            table->has_default[column_index - 1U] = table->has_default[column_index];
            table->column_types[column_index - 1U] = table->column_types[column_index];
            table->unique[column_index - 1U] = table->unique[column_index];
            table->primary_key[column_index - 1U] = table->primary_key[column_index];
            table->defaults[column_index - 1U] = table->defaults[column_index];
            for (row_move = 0U; row_move < table->row_count; ++row_move) {
                table->rows[row_move].values[column_index - 1U] = table->rows[row_move].values[column_index];
            }
        }
        table->column_count -= 1U;
        table->columns[table->column_count][0] = '\0';
        table->not_null[table->column_count] = 0U;
        table->column_types[table->column_count] = SQL_TYPE_TEXT;
        table->has_default[table->column_count] = 0U;
        table->unique[table->column_count] = 0U;
        table->primary_key[table->column_count] = 0U;
        table->defaults[table->column_count] = 0U;
        for (row_index = 0U; row_index < table->row_count; ++row_index) {
            table->rows[row_index].values[table->column_count] = 0U;
        }
        sql_invalidate_table_runtime_caches(table);
        rt_write_line(1, "ok");
        return 1;
    }
    if (!sql_expect_word(parser, "add")) {
        return -1;
    }
    (void)sql_try_word(parser, "column");
    if (sql_read_identifier(parser, column_name, sizeof(column_name)) != 0) {
        return -1;
    }
    for (;;) {
        size_t saved = parser->pos;
        if (sql_next_token(parser) != 0) {
            return -1;
        }
        if (parser->token_type == SQL_TOKEN_END) {
            break;
        }
        if (parser->token_type == SQL_TOKEN_WORD && sql_column_type_from_name(parser->token) >= 0) {
            if (column_type != SQL_TYPE_TEXT) {
                return -1;
            }
            column_type = sql_column_type_from_name(parser->token);
        } else if (parser->token_type == SQL_TOKEN_WORD && sql_equal_ignore_case(parser->token, "not")) {
            if (!sql_expect_word(parser, "null")) {
                return -1;
            }
            not_null = 1;
        } else if (parser->token_type == SQL_TOKEN_WORD && sql_equal_ignore_case(parser->token, "primary")) {
            if (!sql_expect_word(parser, "key")) {
                return -1;
            }
            primary_key = 1;
            unique = 1;
            not_null = 1;
        } else if (parser->token_type == SQL_TOKEN_WORD && sql_equal_ignore_case(parser->token, "unique")) {
            unique = 1;
        } else if (parser->token_type == SQL_TOKEN_WORD && sql_equal_ignore_case(parser->token, "default")) {
            int is_null = 0;
            if (has_default || sql_read_value_or_null(parser, default_value, sizeof(default_value), &is_null) != 0) {
                return -1;
            }
            has_default = 1;
            default_is_null = is_null;
        } else {
            parser->pos = saved;
            return -1;
        }
    }
    if (table->column_count >= SQL_MAX_COLUMNS || sql_find_column(table, column_name) >= 0 || sql_ensure_column_capacity(table, table->column_count + 1U) != 0) {
        return -1;
    }
    if ((unique || primary_key) && table->row_count > 1U) {
        return -1;
    }
    if (primary_key) {
        unsigned int existing_column;
        for (existing_column = 0U; existing_column < table->column_count; ++existing_column) {
            if (table->primary_key[existing_column]) {
                return -1;
            }
        }
    }
    if ((not_null || primary_key) && (!has_default || default_is_null)) {
        return -1;
    }
    if (has_default) {
        if (default_is_null) {
            default_offset = SQL_NULL_OFFSET;
        } else if (sql_validate_typed_value((unsigned int)column_type, 0, default_value) != 0 || sql_store_value(db, default_value, &default_offset) != 0) {
            return -1;
        }
    }
    if (sql_copy_checked(table->columns[table->column_count], sizeof(table->columns[table->column_count]), column_name) != 0) {
        return -1;
    }
    table->not_null[table->column_count] = not_null ? 1U : 0U;
    table->column_types[table->column_count] = (unsigned char)column_type;
    table->has_default[table->column_count] = has_default ? 1U : 0U;
    table->unique[table->column_count] = unique ? 1U : 0U;
    table->primary_key[table->column_count] = primary_key ? 1U : 0U;
    table->defaults[table->column_count] = default_offset;
    for (row_index = 0U; row_index < table->row_count; ++row_index) {
        table->rows[row_index].values[table->column_count] = has_default ? default_offset : 0U;
    }
    table->column_count += 1U;
    for (row_index = 0U; row_index < table->row_count; ++row_index) {
        if (sql_validate_row_constraints(table, &table->rows[row_index]) != 0) {
            return -1;
        }
    }
    sql_invalidate_table_runtime_caches(table);
    rt_write_line(1, "ok");
    return 1;
}

