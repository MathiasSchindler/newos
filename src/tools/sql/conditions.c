#include "internal.h"

static int sql_like_match(const char *text, const char *pattern) {
    if (*pattern == '\0') {
        return *text == '\0';
    }
    if (*pattern == '%') {
        return sql_like_match(text, pattern + 1) || (*text != '\0' && sql_like_match(text + 1, pattern));
    }
    if (*pattern == '_') {
        return *text != '\0' && sql_like_match(text + 1, pattern + 1);
    }
    return *text == *pattern && *text != '\0' && sql_like_match(text + 1, pattern + 1);
}

static int sql_parse_condition_value(SqlParser *parser, SqlSelectQuery *query, SqlConditionValue *value_out) {
    size_t saved;
    int aggregate_kind;

    if (sql_next_token(parser) != 0 || (parser->token_type != SQL_TOKEN_WORD && parser->token_type != SQL_TOKEN_STRING)) {
        return -1;
    }
    value_out->is_count = 0;
    value_out->is_aggregate = 0;
    value_out->is_null = 0;
    value_out->aggregate_index = -1;
    if (parser->token_type == SQL_TOKEN_WORD && sql_resolve_column(query, parser->token, &value_out->column) == 0) {
        value_out->is_column = 1;
        value_out->value[0] = '\0';
        return 0;
    }
    saved = parser->pos;
    aggregate_kind = parser->token_type == SQL_TOKEN_WORD ? sql_aggregate_kind_from_name(parser->token) : -1;
    if (aggregate_kind >= 0) {
        int index = -1;
        int rc = sql_parse_aggregate_call(parser, query, aggregate_kind, aggregate_kind == SQL_SELECT_COUNT_COLUMN, &value_out->column, &index);
        if (rc < 0) {
            return -1;
        }
        if (rc > 0) {
            value_out->is_column = 0;
            value_out->is_count = 0;
            value_out->is_aggregate = 1;
            value_out->aggregate_index = index;
            value_out->value[0] = '\0';
            return 0;
        }
    }
    parser->pos = saved;
    value_out->is_column = 0;
    if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "null")) {
        value_out->is_null = 1;
        value_out->value[0] = '\0';
        return 0;
    }
    return sql_copy_checked(value_out->value, sizeof(value_out->value), parser->token);
}

static int sql_parse_condition_operator_current(SqlParser *parser, int *operator_out) {
    if (parser->token_type != SQL_TOKEN_SYMBOL) {
        return -1;
    }
    if (parser->token[0] == '=') {
        *operator_out = SQL_CONDITION_EQ;
        return 0;
    }
    if (parser->token[0] == '!') {
        if (!sql_expect_symbol(parser, '=')) {
            return -1;
        }
        *operator_out = SQL_CONDITION_NE;
        return 0;
    }
    if (parser->token[0] == '<') {
        if (sql_try_symbol(parser, '=')) {
            *operator_out = SQL_CONDITION_LE;
        } else if (sql_try_symbol(parser, '>')) {
            *operator_out = SQL_CONDITION_NE;
        } else {
            *operator_out = SQL_CONDITION_LT;
        }
        return 0;
    }
    if (parser->token[0] == '>') {
        if (sql_try_symbol(parser, '=')) {
            *operator_out = SQL_CONDITION_GE;
        } else {
            *operator_out = SQL_CONDITION_GT;
        }
        return 0;
    }
    return -1;
}

static int sql_parse_condition_operator(SqlParser *parser, int *operator_out) {
    if (sql_next_token(parser) != 0) {
        return -1;
    }
    return sql_parse_condition_operator_current(parser, operator_out);
}

static int sql_parse_condition(SqlParser *parser, SqlSelectQuery *query, SqlCondition *condition) {
    if (sql_parse_condition_value(parser, query, &condition->left) != 0 ||
        sql_parse_condition_operator(parser, &condition->operator_kind) != 0 ||
        sql_parse_condition_value(parser, query, &condition->right) != 0) {
        return -1;
    }
    condition->present = 1;
    return 0;
}

static int sql_add_condition_leaf(SqlConditionList *list, const SqlCondition *condition) {
    int index;

    if (list->count >= SQL_MAX_CONDITION_NODES) {
        return -1;
    }
    index = (int)list->count;
    list->nodes[index].kind = SQL_CONNECT_LEAF;
    list->nodes[index].left = -1;
    list->nodes[index].right = -1;
    list->nodes[index].condition = *condition;
    list->count += 1U;
    list->root = index;
    return index;
}

static int sql_add_condition_node(SqlConditionList *list, int kind, int left, int right) {
    int index;

    if (list->count >= SQL_MAX_CONDITION_NODES) {
        return -1;
    }
    index = (int)list->count;
    list->nodes[index].kind = kind;
    list->nodes[index].left = left;
    list->nodes[index].right = right;
    list->count += 1U;
    list->root = index;
    return index;
}

static int sql_parse_select_condition_expr(SqlParser *parser, SqlSelectQuery *query, SqlConditionList *list, int *node_out);

static int sql_parse_select_condition_leaf(SqlParser *parser, SqlSelectQuery *query, SqlConditionList *list, int *node_out) {
    SqlCondition condition;
    int negated = 0;

    rt_memset(&condition, 0, sizeof(condition));
    if (sql_try_word(parser, "not")) {
        negated = 1;
    }
    if (sql_parse_condition_value(parser, query, &condition.left) != 0 || sql_next_token(parser) != 0) {
        return -1;
    }
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
        if (sql_parse_condition_value(parser, query, &condition.right) != 0 || !sql_expect_word(parser, "and")) {
            return -1;
        }
        condition.present = 1;
        *node_out = sql_add_condition_leaf(list, &condition);
        if (*node_out < 0) {
            return -1;
        }
        upper = condition;
        upper.operator_kind = negated ? SQL_CONDITION_GT : SQL_CONDITION_LE;
        if (sql_parse_condition_value(parser, query, &upper.right) != 0) {
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
            if (condition.value_count >= SQL_MAX_IN_VALUES || sql_parse_condition_value(parser, query, &condition.values[condition.value_count]) != 0) {
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
    if (sql_parse_condition_value(parser, query, &condition.right) != 0) {
        return -1;
    }
    condition.negated = negated;
    condition.present = 1;
    *node_out = sql_add_condition_leaf(list, &condition);
    return *node_out < 0 ? -1 : 0;
}

static int sql_parse_select_condition_primary(SqlParser *parser, SqlSelectQuery *query, SqlConditionList *list, int *node_out) {
    if (sql_try_word(parser, "not")) {
        int inner;
        if (sql_parse_select_condition_primary(parser, query, list, &inner) != 0) {
            return -1;
        }
        *node_out = sql_add_condition_node(list, SQL_CONNECT_NOT, inner, -1);
        return *node_out < 0 ? -1 : 0;
    }
    if (sql_try_symbol(parser, '(')) {
        if (sql_parse_select_condition_expr(parser, query, list, node_out) != 0 || !sql_expect_symbol(parser, ')')) {
            return -1;
        }
        return 0;
    }
    return sql_parse_select_condition_leaf(parser, query, list, node_out);
}

static int sql_parse_select_condition_and(SqlParser *parser, SqlSelectQuery *query, SqlConditionList *list, int *node_out) {
    int left;

    if (sql_parse_select_condition_primary(parser, query, list, &left) != 0) {
        return -1;
    }
    while (sql_try_word(parser, "and")) {
        int right;
        if (sql_parse_select_condition_primary(parser, query, list, &right) != 0) {
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

static int sql_parse_select_condition_expr(SqlParser *parser, SqlSelectQuery *query, SqlConditionList *list, int *node_out) {
    int left;

    if (sql_parse_select_condition_and(parser, query, list, &left) != 0) {
        return -1;
    }
    while (sql_try_word(parser, "or")) {
        int right;
        if (sql_parse_select_condition_and(parser, query, list, &right) != 0) {
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

static int sql_parse_select_condition_list(SqlParser *parser, SqlSelectQuery *query, SqlConditionList *list) {
    return sql_parse_select_condition_expr(parser, query, list, &list->root);
}

static const char *sql_condition_value_text(const SqlConditionValue *value, const SqlResultRow *row) {
    if (value->is_aggregate) {
        return sql_value_at(row->aggregates[value->aggregate_index]);
    }
    if (value->is_column) {
        return sql_row_value(row->rows[value->column.table_index], (unsigned int)value->column.column_index);
    }
    return value->value;
}

static int sql_condition_value_is_null(const SqlConditionValue *value, const SqlResultRow *row) {
    if (value->is_null) {
        return 1;
    }
    if (value->is_column) {
        return sql_row_value_is_null(row->rows[value->column.table_index], (unsigned int)value->column.column_index);
    }
    return 0;
}

static int sql_condition_matches(const SqlCondition *condition, const SqlResultRow *row) {
    int comparison;
    int matches;
    unsigned int i;

    if (!condition->present) {
        return 1;
    }
    if (condition->operator_kind == SQL_CONDITION_NULL) {
        matches = sql_condition_value_is_null(&condition->left, row);
        return condition->negated ? !matches : matches;
    }
    if (sql_condition_value_is_null(&condition->left, row) || sql_condition_value_is_null(&condition->right, row)) {
        return 0;
    }
    comparison = sql_compare_condition_values(&condition->left, &condition->right, row);
    if (condition->operator_kind == SQL_CONDITION_LIKE) {
        matches = sql_like_match(sql_condition_value_text(&condition->left, row), sql_condition_value_text(&condition->right, row));
        return condition->negated ? !matches : matches;
    }
    if (condition->operator_kind == SQL_CONDITION_IN) {
        matches = 0;
        for (i = 0U; i < condition->value_count; ++i) {
            if (sql_condition_value_is_null(&condition->values[i], row)) {
                continue;
            }
            if (sql_compare_condition_values(&condition->left, &condition->values[i], row) == 0) {
                matches = 1;
                break;
            }
        }
        return condition->negated ? !matches : matches;
    }
    if (condition->operator_kind == SQL_CONDITION_EMPTY) {
        matches = !sql_condition_value_is_null(&condition->left, row) && sql_condition_value_text(&condition->left, row)[0] == '\0';
        return condition->negated ? !matches : matches;
    }
    switch (condition->operator_kind) {
        case SQL_CONDITION_EQ:
            matches = comparison == 0;
            break;
        case SQL_CONDITION_NE:
            matches = comparison != 0;
            break;
        case SQL_CONDITION_LT:
            matches = comparison < 0;
            break;
        case SQL_CONDITION_LE:
            matches = comparison <= 0;
            break;
        case SQL_CONDITION_GT:
            matches = comparison > 0;
            break;
        case SQL_CONDITION_GE:
            matches = comparison >= 0;
            break;
        default:
            return 0;
    }
    return condition->negated ? !matches : matches;
}

static int sql_condition_node_matches(const SqlConditionList *list, int node_index, const SqlResultRow *row) {
    const SqlConditionNode *node;

    if (node_index < 0 || (unsigned int)node_index >= list->count) {
        return 1;
    }
    node = &list->nodes[node_index];
    if (node->kind == SQL_CONNECT_LEAF) {
        return sql_condition_matches(&node->condition, row);
    }
    if (node->kind == SQL_CONNECT_NOT) {
        return !sql_condition_node_matches(list, node->left, row);
    }
    if (node->kind == SQL_CONNECT_AND) {
        return sql_condition_node_matches(list, node->left, row) && sql_condition_node_matches(list, node->right, row);
    }
    if (node->kind == SQL_CONNECT_OR) {
        return sql_condition_node_matches(list, node->left, row) || sql_condition_node_matches(list, node->right, row);
    }
    return 0;
}

static int sql_condition_list_matches(const SqlConditionList *list, const SqlResultRow *row) {
    if (list->count == 0U) {
        return 1;
    }
    return sql_condition_node_matches(list, list->root, row);
}

static int sql_condition_node_uses_aggregate(const SqlConditionList *list, int node_index) {
    const SqlConditionNode *node;

    if (node_index < 0 || (unsigned int)node_index >= list->count) {
        return 0;
    }
    node = &list->nodes[node_index];
    if (node->kind == SQL_CONNECT_LEAF) {
        return node->condition.left.is_aggregate || node->condition.right.is_aggregate;
    }
    return sql_condition_node_uses_aggregate(list, node->left) || sql_condition_node_uses_aggregate(list, node->right);
}

static int sql_condition_list_uses_aggregate(const SqlConditionList *list) {
    return list->count > 0U && sql_condition_node_uses_aggregate(list, list->root);
}

