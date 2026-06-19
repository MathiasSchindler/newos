#include "internal.h"

static int sql_read_condition_literal(SqlParser *parser, SqlConditionValue *value) {
    rt_memset(value, 0, sizeof(*value));
    value->aggregate_index = -1;
    if (sql_read_value_or_null(parser, value->value, sizeof(value->value), &value->is_null) != 0) {
        return -1;
    }
    return 0;
}

static int sql_parse_row_condition_expr(SqlParser *parser, const SqlTable *table, SqlConditionList *list, int *node_out);

static int sql_parse_row_condition_leaf(SqlParser *parser, const SqlTable *table, SqlConditionList *list, int *node_out) {
    SqlCondition condition;
    char column_name[SQL_NAME_SIZE];
    int negated = 0;
    int column;

    rt_memset(&condition, 0, sizeof(condition));
    if (sql_try_word(parser, "not")) {
        negated = 1;
    }
    if (sql_read_identifier(parser, column_name, sizeof(column_name)) != 0 || sql_next_token(parser) != 0) {
        return -1;
    }
    column = sql_find_column(table, column_name);
    if (column < 0) {
        return -1;
    }
    condition.left.is_column = 1;
    condition.left.aggregate_index = -1;
    condition.left.column.table_index = 0;
    condition.left.column.column_index = column;
    if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "not")) {
        negated = !negated;
        if (sql_next_token(parser) != 0) {
            return -1;
        }
    }
    if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "between")) {
        SqlCondition upper;

        condition.operator_kind = negated ? SQL_CONDITION_LT : SQL_CONDITION_GE;
        condition.negated = 0;
        if (sql_read_condition_literal(parser, &condition.right) != 0 || !sql_expect_word(parser, "and")) {
            return -1;
        }
        condition.present = 1;
        *node_out = sql_add_condition_leaf(list, &condition);
        if (*node_out < 0) {
            return -1;
        }
        upper = condition;
        upper.operator_kind = negated ? SQL_CONDITION_GT : SQL_CONDITION_LE;
        if (sql_read_condition_literal(parser, &upper.right) != 0) {
            return -1;
        }
        {
            int upper_node = sql_add_condition_leaf(list, &upper);
            if (upper_node < 0) {
                return -1;
            }
            *node_out = sql_add_condition_node(list, negated ? SQL_CONNECT_OR : SQL_CONNECT_AND, *node_out, upper_node);
            return *node_out < 0 ? -1 : 0;
        }
    }
    if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "like")) {
        condition.operator_kind = SQL_CONDITION_LIKE;
    } else if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "in")) {
        if (!sql_expect_symbol(parser, '(')) {
            return -1;
        }
        for (;;) {
            if (condition.value_count >= SQL_MAX_IN_VALUES || sql_read_condition_literal(parser, &condition.values[condition.value_count]) != 0) {
                return -1;
            }
            condition.value_count += 1U;
            if (sql_try_symbol(parser, ')')) {
                break;
            }
            if (!sql_expect_symbol(parser, ',')) {
                return -1;
            }
        }
        condition.operator_kind = SQL_CONDITION_IN;
        condition.negated = negated;
        condition.present = 1;
        *node_out = sql_add_condition_leaf(list, &condition);
        return *node_out < 0 ? -1 : 0;
    } else if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "is")) {
        if (sql_try_word(parser, "not")) {
            negated = !negated;
        }
        if (sql_try_word(parser, "empty")) {
            condition.operator_kind = SQL_CONDITION_EMPTY;
        } else if (sql_try_word(parser, "null")) {
            condition.operator_kind = SQL_CONDITION_NULL;
        } else {
            return -1;
        }
        condition.negated = negated;
        condition.present = 1;
            *node_out = sql_add_condition_leaf(list, &condition);
            return *node_out < 0 ? -1 : 0;
    } else if (sql_parse_condition_operator_current(parser, &condition.operator_kind) != 0) {
        return -1;
    }
    if (sql_read_condition_literal(parser, &condition.right) != 0) {
        return -1;
    }
    condition.negated = negated;
    condition.present = 1;
    *node_out = sql_add_condition_leaf(list, &condition);
    return *node_out < 0 ? -1 : 0;
}

static int sql_parse_row_condition_primary(SqlParser *parser, const SqlTable *table, SqlConditionList *list, int *node_out) {
    if (sql_try_word(parser, "not")) {
        int inner;
        if (sql_parse_row_condition_primary(parser, table, list, &inner) != 0) {
            return -1;
        }
        *node_out = sql_add_condition_node(list, SQL_CONNECT_NOT, inner, -1);
        return *node_out < 0 ? -1 : 0;
    }
    if (sql_try_symbol(parser, '(')) {
        if (sql_parse_row_condition_expr(parser, table, list, node_out) != 0 || !sql_expect_symbol(parser, ')')) {
            return -1;
        }
        return 0;
    }
    return sql_parse_row_condition_leaf(parser, table, list, node_out);
}

static int sql_parse_row_condition_and(SqlParser *parser, const SqlTable *table, SqlConditionList *list, int *node_out) {
    int left;

    if (sql_parse_row_condition_primary(parser, table, list, &left) != 0) {
        return -1;
    }
    while (sql_try_word(parser, "and")) {
        int right;
        if (sql_parse_row_condition_primary(parser, table, list, &right) != 0) {
            return -1;
        }
        left = sql_add_condition_node(list, SQL_CONNECT_AND, left, right);
        if (left < 0) {
            return -1;
        }
    }
    *node_out = left;
    return 0;
}

static int sql_parse_row_condition_expr(SqlParser *parser, const SqlTable *table, SqlConditionList *list, int *node_out) {
    int left;

    if (sql_parse_row_condition_and(parser, table, list, &left) != 0) {
        return -1;
    }
    while (sql_try_word(parser, "or")) {
        int right;
        if (sql_parse_row_condition_and(parser, table, list, &right) != 0) {
            return -1;
        }
        left = sql_add_condition_node(list, SQL_CONNECT_OR, left, right);
        if (left < 0) {
            return -1;
        }
    }
    *node_out = left;
    return 0;
}

static int sql_parse_row_condition_list(SqlParser *parser, const SqlTable *table, SqlConditionList *list) {
    return sql_parse_row_condition_expr(parser, table, list, &list->root);
}

static int sql_parse_where(SqlParser *parser, const SqlTable *table, SqlConditionList *where_out) {
    size_t saved = parser->pos;

    rt_memset(where_out, 0, sizeof(*where_out));

    if (sql_next_token(parser) != 0) {
        return -1;
    }
    if (parser->token_type == SQL_TOKEN_END) {
        return 0;
    }
    if (parser->token_type != SQL_TOKEN_WORD || !tool_str_equal_ignore_case_ascii(parser->token, "where")) {
        parser->pos = saved;
        return -1;
    }
    if (sql_parse_row_condition_list(parser, table, where_out) != 0 || !sql_at_end(parser)) {
        return -1;
    }
    return 0;
}

static int sql_row_condition_list_matches(const SqlTable *table, unsigned int row_index, const SqlConditionList *where) {
    SqlResultRow result;

    rt_memset(&result, 0, sizeof(result));
    if (table != 0 && row_index < table->row_count) {
        result.tables[0] = table;
        result.rows[0] = &table->rows[row_index];
        result.row_indices[0] = row_index;
    } else {
        result.row_indices[0] = SQL_ROW_INDEX_NONE;
    }
    return sql_condition_list_matches(where, &result);
}

