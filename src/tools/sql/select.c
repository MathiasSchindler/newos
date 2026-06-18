#include "internal.h"

static int sql_parse_select_aggregate(SqlParser *parser, int kind, SqlSelectItem *item, char *raw_argument, size_t raw_argument_size) {
    size_t saved = parser->pos;

    if (!sql_try_symbol(parser, '(')) {
        parser->pos = saved;
        return 0;
    }
    if (kind == SQL_SELECT_COUNT_COLUMN && sql_try_symbol(parser, '*')) {
        if (!sql_expect_symbol(parser, ')')) {
            return -1;
        }
        item->kind = SQL_SELECT_COUNT_ALL;
        if (sql_copy_checked(item->label, sizeof(item->label), "count") != 0) {
            return -1;
        }
        raw_argument[0] = '\0';
        return 1;
    }
    if (sql_next_token(parser) != 0 || parser->token_type != SQL_TOKEN_WORD) {
        return -1;
    }
    if (sql_copy_checked(raw_argument, raw_argument_size, parser->token) != 0) {
        return -1;
    }
    if (!sql_expect_symbol(parser, ')')) {
        return -1;
    }
    item->kind = kind;
    if (sql_copy_checked(item->label, sizeof(item->label), sql_aggregate_label(kind)) != 0) {
        return -1;
    }
    return 1;
}

static int sql_parse_select_list(SqlParser *parser, SqlSelectQuery *query, SqlSelectScratch *scratch) {
    if (sql_next_token(parser) != 0) {
        return -1;
    }
    if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "distinct")) {
        query->distinct = 1;
        if (sql_next_token(parser) != 0) {
            return -1;
        }
    }
    if (parser->token_type == SQL_TOKEN_SYMBOL && parser->token[0] == '*') {
        query->select_all = 1;
        return sql_expect_word(parser, "from") ? 0 : -1;
    }
    if (parser->token_type != SQL_TOKEN_WORD) {
        return -1;
    }
    for (;;) {
        if (query->item_count >= SQL_MAX_COLUMNS ||
            sql_ensure_select_item_capacity(query, query->item_count + 1U) != 0 ||
            sql_ensure_select_scratch_capacity(scratch, query->item_count + 1U) != 0) {
            return -1;
        }
        scratch->raw_labels[query->item_count][0] = '\0';
        scratch->raw_expr_right[query->item_count][0] = '\0';
        {
            int aggregate_kind = sql_aggregate_kind_from_name(parser->token);
            if (aggregate_kind >= 0) {
            int rc = sql_parse_select_aggregate(parser, aggregate_kind, &query->items[query->item_count], scratch->raw_items[query->item_count], SQL_VALUE_SIZE);
            if (rc < 0) {
                return -1;
            }
            if (rc > 0) {
                scratch->raw_kinds[query->item_count] = query->items[query->item_count].kind;
            } else {
                scratch->raw_kinds[query->item_count] = SQL_SELECT_COLUMN;
                if (sql_copy_checked(scratch->raw_items[query->item_count], SQL_VALUE_SIZE, parser->token) != 0) {
                    return -1;
                }
            }
        } else {
            scratch->raw_kinds[query->item_count] = SQL_SELECT_COLUMN;
            if (sql_copy_checked(scratch->raw_items[query->item_count], SQL_VALUE_SIZE, parser->token) != 0) {
                return -1;
            }
        }
        }
        query->item_count += 1U;
        if (sql_next_token(parser) != 0) {
            return -1;
        }
        if (parser->token_type == SQL_TOKEN_SYMBOL && (parser->token[0] == '+' || parser->token[0] == '|')) {
            unsigned int expression_index = query->item_count - 1U;
            if (parser->token[0] == '|') {
                if (!sql_expect_symbol(parser, '|')) {
                    return -1;
                }
                scratch->raw_kinds[expression_index] = SQL_SELECT_CONCAT;
            } else {
                scratch->raw_kinds[expression_index] = SQL_SELECT_ADD;
            }
            if (sql_read_value(parser, scratch->raw_expr_right[expression_index], SQL_VALUE_SIZE) != 0 || sql_next_token(parser) != 0) {
                return -1;
            }
        } else if (parser->token_type == SQL_TOKEN_WORD && rt_strcmp(parser->token, "-") == 0) {
            unsigned int expression_index = query->item_count - 1U;
            scratch->raw_kinds[expression_index] = SQL_SELECT_SUB;
            if (sql_read_value(parser, scratch->raw_expr_right[expression_index], SQL_VALUE_SIZE) != 0 || sql_next_token(parser) != 0) {
                return -1;
            }
        }
        if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "as")) {
            if (sql_read_identifier(parser, scratch->raw_labels[query->item_count - 1U], SQL_VALUE_SIZE) != 0 || sql_next_token(parser) != 0) {
                return -1;
            }
        }
        if (parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, "from")) {
            return 0;
        }
        if (parser->token_type != SQL_TOKEN_SYMBOL || parser->token[0] != ',') {
            return -1;
        }
        if (sql_next_token(parser) != 0 || parser->token_type != SQL_TOKEN_WORD) {
            return -1;
        }
    }
}

static int sql_select_tail_keyword(const char *word) {
    return tool_str_equal_ignore_case_ascii(word, "join") ||
           tool_str_equal_ignore_case_ascii(word, "left") ||
           tool_str_equal_ignore_case_ascii(word, "right") ||
           tool_str_equal_ignore_case_ascii(word, "full") ||
           tool_str_equal_ignore_case_ascii(word, "natural") ||
           tool_str_equal_ignore_case_ascii(word, "outer") ||
           tool_str_equal_ignore_case_ascii(word, "on") ||
           tool_str_equal_ignore_case_ascii(word, "using") ||
           tool_str_equal_ignore_case_ascii(word, "where") ||
           tool_str_equal_ignore_case_ascii(word, "group") ||
           tool_str_equal_ignore_case_ascii(word, "having") ||
           tool_str_equal_ignore_case_ascii(word, "order") ||
           tool_str_equal_ignore_case_ascii(word, "limit") ||
           tool_str_equal_ignore_case_ascii(word, "offset");
}

static int sql_add_query_source(SqlDatabase *db, SqlSelectQuery *query, const char *name, const char *alias) {
    SqlTable *table;
    unsigned int i;

    if (query->source_count >= SQL_MAX_QUERY_TABLES) {
        return -1;
    }
    table = sql_find_table(db, name);
    if (table == 0 || sql_copy_checked(query->sources[query->source_count].name, sizeof(query->sources[query->source_count].name), name) != 0) {
        return -1;
    }
    if (alias != 0 && alias[0] != '\0') {
        for (i = 0U; i < query->source_count; ++i) {
            if (tool_str_equal_ignore_case_ascii(query->sources[i].name, alias) || tool_str_equal_ignore_case_ascii(query->sources[i].alias, alias)) {
                return -1;
            }
        }
        if (sql_copy_checked(query->sources[query->source_count].alias, sizeof(query->sources[query->source_count].alias), alias) != 0) {
            return -1;
        }
    } else {
        query->sources[query->source_count].alias[0] = '\0';
    }
    query->sources[query->source_count].table = table;
    query->source_count += 1U;
    return 0;
}

static int sql_read_optional_source_alias(SqlParser *parser, char *alias, size_t alias_size) {
    size_t saved = parser->pos;

    alias[0] = '\0';
    if (sql_try_word(parser, "as")) {
        return sql_read_identifier(parser, alias, alias_size);
    }
    if (sql_next_token(parser) == 0 && parser->token_type == SQL_TOKEN_WORD && !sql_select_tail_keyword(parser->token)) {
        return sql_copy_checked(alias, alias_size, parser->token);
    }
    parser->pos = saved;
    return 0;
}

static int sql_resolve_select_items(SqlSelectQuery *query, SqlSelectScratch *scratch) {
    unsigned int source_index;
    unsigned int item_index;

    if (query->select_all) {
        query->item_count = 0U;
        for (source_index = 0U; source_index < query->source_count; ++source_index) {
            SqlTable *table = query->sources[source_index].table;
            unsigned int column_index;
            for (column_index = 0U; column_index < table->column_count; ++column_index) {
                SqlSelectItem *item;
                if (query->item_count >= SQL_MAX_COLUMNS || sql_ensure_select_item_capacity(query, query->item_count + 1U) != 0) {
                    return -1;
                }
                item = &query->items[query->item_count++];
                item->kind = SQL_SELECT_COLUMN;
                item->column.table_index = (int)source_index;
                item->column.column_index = (int)column_index;
                if (query->source_count > 1U) {
                    if (sql_copy_label(item->label, sizeof(item->label), query->sources[source_index].name, ".") != 0 ||
                        sql_copy_label(item->label + rt_strlen(item->label), sizeof(item->label) - rt_strlen(item->label), table->columns[column_index], 0) != 0) {
                        return -1;
                    }
                } else if (sql_copy_checked(item->label, sizeof(item->label), table->columns[column_index]) != 0) {
                    return -1;
                }
            }
        }
        return 0;
    }
    for (item_index = 0U; item_index < query->item_count; ++item_index) {
        query->items[item_index].kind = scratch->raw_kinds[item_index];
        if (scratch->raw_kinds[item_index] == SQL_SELECT_COLUMN || scratch->raw_kinds[item_index] == SQL_SELECT_ADD || scratch->raw_kinds[item_index] == SQL_SELECT_SUB || scratch->raw_kinds[item_index] == SQL_SELECT_CONCAT) {
            if (sql_resolve_column(query, scratch->raw_items[item_index], &query->items[item_index].column) != 0 ||
                sql_copy_checked(query->items[item_index].label, sizeof(query->items[item_index].label), scratch->raw_items[item_index]) != 0) {
                return -1;
            }
            query->items[item_index].has_right_column = 0;
            query->items[item_index].right_column.table_index = -1;
            query->items[item_index].right_column.column_index = -1;
            if (scratch->raw_kinds[item_index] == SQL_SELECT_ADD || scratch->raw_kinds[item_index] == SQL_SELECT_SUB || scratch->raw_kinds[item_index] == SQL_SELECT_CONCAT) {
                if (sql_copy_checked(query->items[item_index].literal, sizeof(query->items[item_index].literal), scratch->raw_expr_right[item_index]) != 0) {
                    return -1;
                }
                if (scratch->raw_kinds[item_index] == SQL_SELECT_CONCAT && sql_resolve_column(query, scratch->raw_expr_right[item_index], &query->items[item_index].right_column) == 0) {
                    query->items[item_index].has_right_column = 1;
                }
                if (scratch->raw_labels[item_index][0] == '\0') {
                    const char *operator_text = scratch->raw_kinds[item_index] == SQL_SELECT_ADD ? "+" : (scratch->raw_kinds[item_index] == SQL_SELECT_SUB ? "-" : "||");
                    if (sql_copy_label(query->items[item_index].label, sizeof(query->items[item_index].label), scratch->raw_items[item_index], operator_text) != 0 ||
                        sql_copy_label(query->items[item_index].label + rt_strlen(query->items[item_index].label), sizeof(query->items[item_index].label) - rt_strlen(query->items[item_index].label), scratch->raw_expr_right[item_index], 0) != 0) {
                        return -1;
                    }
                }
            }
        } else {
            SqlColumnRef column;
            SqlColumnRef *column_ptr = &column;
            if (scratch->raw_kinds[item_index] == SQL_SELECT_COUNT_ALL) {
                column_ptr = 0;
            } else if (sql_resolve_column(query, scratch->raw_items[item_index], &column) != 0) {
                return -1;
            }
            if (sql_add_aggregate(query, scratch->raw_kinds[item_index], column_ptr, &query->items[item_index].aggregate_index) != 0 ||
                sql_copy_checked(query->items[item_index].label, sizeof(query->items[item_index].label), sql_aggregate_label(scratch->raw_kinds[item_index])) != 0) {
                return -1;
            }
            if (column_ptr != 0) {
                query->items[item_index].column = column;
            }
        }
        if (scratch->raw_labels[item_index][0] != '\0' && sql_copy_checked(query->items[item_index].label, sizeof(query->items[item_index].label), scratch->raw_labels[item_index]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int sql_parse_select_tail(SqlDatabase *db, SqlParser *parser, SqlSelectQuery *query, SqlSelectScratch *scratch) {
    char table_name[SQL_NAME_SIZE];
    char alias[SQL_NAME_SIZE];
    unsigned int join_index;

    if (sql_read_identifier(parser, table_name, sizeof(table_name)) != 0 ||
        sql_read_optional_source_alias(parser, alias, sizeof(alias)) != 0 ||
        sql_add_query_source(db, query, table_name, alias) != 0) {
        return -1;
    }
    for (;;) {
        int join_type = SQL_JOIN_INNER;
        int natural = sql_try_word(parser, "natural");
        if (sql_try_word(parser, "left")) {
            join_type = SQL_JOIN_LEFT;
            (void)sql_try_word(parser, "outer");
            if (!sql_expect_word(parser, "join")) {
                return -1;
            }
        } else if (sql_try_word(parser, "right")) {
            join_type = SQL_JOIN_RIGHT;
            (void)sql_try_word(parser, "outer");
            if (!sql_expect_word(parser, "join")) {
                return -1;
            }
        } else if (sql_try_word(parser, "full")) {
            join_type = SQL_JOIN_FULL;
            (void)sql_try_word(parser, "outer");
            if (!sql_expect_word(parser, "join")) {
                return -1;
            }
        } else if (!sql_try_word(parser, "join")) {
            if (natural) {
                return -1;
            }
            break;
        }
        if (sql_read_identifier(parser, table_name, sizeof(table_name)) != 0 ||
            sql_read_optional_source_alias(parser, alias, sizeof(alias)) != 0 ||
            sql_add_query_source(db, query, table_name, alias) != 0) {
            return -1;
        }
        if (query->join_count >= SQL_MAX_QUERY_TABLES - 1U) {
            return -1;
        }
        if (natural) {
            unsigned int left_index;
            unsigned int right_index;
            int left_column = -1;
            int right_column = -1;
            SqlCondition *condition = &query->joins[query->join_count];
            SqlTable *left_table = query->sources[query->source_count - 2U].table;
            SqlTable *right_table = query->sources[query->source_count - 1U].table;

            rt_memset(condition, 0, sizeof(*condition));
            for (left_index = 0U; left_index < left_table->column_count; ++left_index) {
                for (right_index = 0U; right_index < right_table->column_count; ++right_index) {
                    if (tool_str_equal_ignore_case_ascii(left_table->columns[left_index], right_table->columns[right_index])) {
                        if (left_column >= 0) {
                            return -1;
                        }
                        left_column = (int)left_index;
                        right_column = (int)right_index;
                    }
                }
            }
            if (left_column < 0 || right_column < 0) {
                return -1;
            }
            condition->present = 1;
            condition->operator_kind = SQL_CONDITION_EQ;
            condition->left.is_column = 1;
            condition->left.aggregate_index = -1;
            condition->left.column.table_index = (int)query->source_count - 2;
            condition->left.column.column_index = left_column;
            condition->right.is_column = 1;
            condition->right.aggregate_index = -1;
            condition->right.column.table_index = (int)query->source_count - 1;
            condition->right.column.column_index = right_column;
        } else if (sql_try_word(parser, "using")) {
            char using_column[SQL_NAME_SIZE];
            int left_column;
            int right_column;
            SqlCondition *condition = &query->joins[query->join_count];
            SqlTable *left_table = query->sources[query->source_count - 2U].table;
            SqlTable *right_table = query->sources[query->source_count - 1U].table;

            rt_memset(condition, 0, sizeof(*condition));
            if (!sql_expect_symbol(parser, '(') || sql_read_identifier(parser, using_column, sizeof(using_column)) != 0 || !sql_expect_symbol(parser, ')')) {
                return -1;
            }
            left_column = sql_find_column(left_table, using_column);
            right_column = sql_find_column(right_table, using_column);
            if (left_column < 0 || right_column < 0) {
                return -1;
            }
            condition->present = 1;
            condition->operator_kind = SQL_CONDITION_EQ;
            condition->left.is_column = 1;
            condition->left.aggregate_index = -1;
            condition->left.column.table_index = (int)query->source_count - 2;
            condition->left.column.column_index = left_column;
            condition->right.is_column = 1;
            condition->right.aggregate_index = -1;
            condition->right.column.table_index = (int)query->source_count - 1;
            condition->right.column.column_index = right_column;
        } else {
            if (!sql_expect_word(parser, "on") || sql_parse_condition(parser, query, &query->joins[query->join_count]) != 0) {
                return -1;
            }
        }
        query->join_types[query->join_count] = join_type;
        query->join_count += 1U;
    }
    for (join_index = 0U; join_index < query->join_count; ++join_index) {
        if (query->join_types[join_index] == SQL_JOIN_RIGHT && (query->source_count != 2U || query->join_count != 1U)) {
            return -1;
        }
    }
    if (sql_resolve_select_items(query, scratch) != 0) {
        return -1;
    }
    if (sql_try_word(parser, "where")) {
        if (sql_parse_select_condition_list(parser, query, &query->where) != 0) {
            return -1;
        }
    }
    if (sql_try_word(parser, "group")) {
        if (!sql_expect_word(parser, "by")) {
            return -1;
        }
        for (;;) {
            if (query->group_count >= SQL_MAX_GROUP_KEYS || sql_read_column_ref(parser, query, &query->group_by[query->group_count], 0, 0) != 0) {
                return -1;
            }
            query->group_count += 1U;
            if (!sql_try_symbol(parser, ',')) {
                break;
            }
        }
    }
    if (sql_try_word(parser, "having")) {
        if (sql_parse_select_condition_list(parser, query, &query->having) != 0) {
            return -1;
        }
    }
    if (sql_try_word(parser, "order")) {
        if (!sql_expect_word(parser, "by")) {
            return -1;
        }
        for (;;) {
            SqlOrderKey *key;

            if (query->order_count >= SQL_MAX_ORDER_KEYS || sql_next_token(parser) != 0 || parser->token_type != SQL_TOKEN_WORD) {
                return -1;
            }
            key = &query->order_by[query->order_count];
            key->desc = 0;
            if (sql_find_select_label(query, parser->token) >= 0) {
                if (sql_copy_checked(key->label, sizeof(key->label), parser->token) != 0) {
                    return -1;
                }
                key->column.table_index = -1;
                key->column.column_index = -1;
            } else if (sql_resolve_column(query, parser->token, &key->column) == 0) {
                key->label[0] = '\0';
            } else {
                return -1;
            }
            if (sql_try_word(parser, "desc")) {
                key->desc = 1;
            } else {
                (void)sql_try_word(parser, "asc");
            }
            query->order_count += 1U;
            if (!sql_try_symbol(parser, ',')) {
                break;
            }
        }
    }
    if (sql_try_word(parser, "limit")) {
        unsigned long long limit_value;
        if (sql_next_token(parser) != 0 || parser->token_type != SQL_TOKEN_WORD || rt_parse_uint(parser->token, &limit_value) != 0 || limit_value > SQL_MAX_RESULT_ROWS) {
            return -1;
        }
        if (sql_try_symbol(parser, ',')) {
            unsigned long long comma_limit_value;
            if (sql_next_token(parser) != 0 || parser->token_type != SQL_TOKEN_WORD || rt_parse_uint(parser->token, &comma_limit_value) != 0 || comma_limit_value > SQL_MAX_RESULT_ROWS) {
                return -1;
            }
            query->has_offset = 1;
            query->offset = (unsigned int)limit_value;
            query->has_limit = 1;
            query->limit = (unsigned int)comma_limit_value;
        } else {
            query->has_limit = 1;
            query->limit = (unsigned int)limit_value;
        }
    }
    if (sql_try_word(parser, "offset")) {
        unsigned long long offset_value;
        if (sql_next_token(parser) != 0 || parser->token_type != SQL_TOKEN_WORD || rt_parse_uint(parser->token, &offset_value) != 0 || offset_value > SQL_MAX_RESULT_ROWS) {
            return -1;
        }
        query->has_offset = 1;
        query->offset = (unsigned int)offset_value;
    }
    return sql_at_end(parser) ? 0 : -1;
}

static int sql_bound_condition_value_text(const SqlConditionValue *value, const SqlResultRow *row, const char **text_out) {
    if (value->is_null || value->is_aggregate || value->is_count) {
        return 0;
    }
    if (value->is_column) {
        const SqlRow *source_row = row->rows[value->column.table_index];
        if (source_row == 0 || sql_row_value_is_null(source_row, (unsigned int)value->column.column_index)) {
            return 0;
        }
        *text_out = sql_row_value(source_row, (unsigned int)value->column.column_index);
        return 1;
    }
    *text_out = value->value;
    return 1;
}

static int sql_condition_index_lookup(const SqlCondition *condition, unsigned int source_index, const SqlResultRow *row, unsigned int *column_out, const char **value_out) {
    const SqlConditionValue *indexed;
    const SqlConditionValue *bound;

    if (!condition->present || condition->negated || condition->operator_kind != SQL_CONDITION_EQ) {
        return 0;
    }
    indexed = 0;
    bound = 0;
    if (condition->left.is_column && condition->left.column.table_index == (int)source_index) {
        indexed = &condition->left;
        bound = &condition->right;
    } else if (condition->right.is_column && condition->right.column.table_index == (int)source_index) {
        indexed = &condition->right;
        bound = &condition->left;
    }
    if (indexed == 0 || indexed->is_aggregate || indexed->is_count) {
        return 0;
    }
    if (bound->is_column && bound->column.table_index >= (int)source_index) {
        return 0;
    }
    if (!sql_bound_condition_value_text(bound, row, value_out)) {
        return 0;
    }
    *column_out = (unsigned int)indexed->column.column_index;
    return 1;
}

static int sql_condition_node_index_lookup(const SqlConditionList *list, int node_index, unsigned int source_index, const SqlResultRow *row, unsigned int *column_out, const char **value_out) {
    const SqlConditionNode *node;

    if (node_index < 0 || (unsigned int)node_index >= list->count) {
        return 0;
    }
    node = &list->nodes[node_index];
    if (node->kind == SQL_CONNECT_LEAF) {
        return sql_condition_index_lookup(&node->condition, source_index, row, column_out, value_out);
    }
    if (node->kind == SQL_CONNECT_AND) {
        if (sql_condition_node_index_lookup(list, node->left, source_index, row, column_out, value_out)) {
            return 1;
        }
        return sql_condition_node_index_lookup(list, node->right, source_index, row, column_out, value_out);
    }
    return 0;
}

static int sql_condition_list_index_lookup(const SqlConditionList *list, unsigned int source_index, const SqlResultRow *row, unsigned int *column_out, const char **value_out) {
    if (list->count == 0U) {
        return 0;
    }
    return sql_condition_node_index_lookup(list, list->root, source_index, row, column_out, value_out);
}

static int sql_select_index_lookup(const SqlSelectQuery *query, unsigned int depth, const SqlResultRow *current, unsigned int *column_out, const char **value_out) {
    if (depth > 0U) {
        unsigned int join_index = depth - 1U;
        if (join_index < query->join_count && sql_condition_index_lookup(&query->joins[join_index], depth, current, column_out, value_out)) {
            return 1;
        }
    }
    return sql_condition_list_index_lookup(&query->where, depth, current, column_out, value_out);
}

static int sql_select_collection_limit_reached(const SqlSelectQuery *query, const SqlResultBuffer *result) {
    return query->collection_limit_enabled && result->count >= query->collection_limit;
}

static int sql_collect_select_rows(const SqlSelectQuery *query, unsigned int depth, SqlResultRow *current, SqlResultBuffer *result) {
    SqlTable *table;
    unsigned int row_index;
    unsigned int index_column = 0U;
    const char *index_value = 0;
    SqlIndexCache *index = 0;
    unsigned int index_start = 0U;
    unsigned int index_end = 0U;
    int use_index = 0;

    if (sql_select_collection_limit_reached(query, result)) {
        return 0;
    }
    if (depth == query->source_count) {
        if (!sql_condition_list_matches(&query->where, current)) {
            return 0;
        }
        if (result->count >= SQL_MAX_RESULT_ROWS || sql_ensure_result_capacity(result, result->count + 1U) != 0) {
            return -1;
        }
        sql_set_result_buffer_row(query, result, result->count, current);
        result->rows[result->count].count = 1U;
        result->count += 1U;
        return 0;
    }
    table = query->sources[depth].table;
    if (sql_select_index_lookup(query, depth, current, &index_column, &index_value)) {
        index = sql_index_cache_for_column(table, index_column);
        if (index != 0) {
            sql_index_equal_range(index, index_value, &index_start, &index_end);
            use_index = 1;
        }
    }
    if (depth == 0U) {
        unsigned int scan_count = use_index ? index_end - index_start : table->row_count;
        for (row_index = 0U; row_index < scan_count; ++row_index) {
            unsigned int source_row = use_index ? index->row_ids[index_start + row_index] : row_index;
            current->rows[depth] = &table->rows[source_row];
            if (sql_collect_select_rows(query, depth + 1U, current, result) != 0) {
                return -1;
            }
            if (sql_select_collection_limit_reached(query, result)) {
                return 0;
            }
        }
        if (query->source_count == 2U && query->join_count == 1U && (query->join_types[0] == SQL_JOIN_FULL || query->join_types[0] == SQL_JOIN_RIGHT)) {
            SqlTable *right_table = query->sources[1].table;
            unsigned int right_index;

            for (right_index = 0U; right_index < right_table->row_count; ++right_index) {
                int matched = 0;
                unsigned int left_index;

                current->rows[1] = &right_table->rows[right_index];
                for (left_index = 0U; left_index < table->row_count; ++left_index) {
                    current->rows[0] = &table->rows[left_index];
                    if (sql_condition_matches(&query->joins[0], current)) {
                        matched = 1;
                        break;
                    }
                }
                if (!matched) {
                    current->rows[0] = 0;
                    if (!sql_condition_list_matches(&query->where, current)) {
                        continue;
                    }
                    if (result->count >= SQL_MAX_RESULT_ROWS || sql_ensure_result_capacity(result, result->count + 1U) != 0) {
                        return -1;
                    }
                    sql_set_result_buffer_row(query, result, result->count, current);
                    result->rows[result->count].count = 1U;
                    result->count += 1U;
                    if (sql_select_collection_limit_reached(query, result)) {
                        return 0;
                    }
                }
            }
        }
        return 0;
    }
    {
        int matched = 0;
        unsigned int join_index = depth - 1U;
        unsigned int scan_count = use_index ? index_end - index_start : table->row_count;
        for (row_index = 0U; row_index < scan_count; ++row_index) {
            unsigned int source_row = use_index ? index->row_ids[index_start + row_index] : row_index;
            current->rows[depth] = &table->rows[source_row];
            if (!sql_condition_matches(&query->joins[join_index], current)) {
                continue;
            }
            matched = 1;
            if (sql_collect_select_rows(query, depth + 1U, current, result) != 0) {
                return -1;
            }
            if (sql_select_collection_limit_reached(query, result)) {
                return 0;
            }
        }
        if (!matched && (query->join_types[join_index] == SQL_JOIN_LEFT || query->join_types[join_index] == SQL_JOIN_FULL)) {
            current->rows[depth] = 0;
            if (sql_collect_select_rows(query, depth + 1U, current, result) != 0) {
                return -1;
            }
            if (sql_select_collection_limit_reached(query, result)) {
                return 0;
            }
        }
        return 0;
    }
}

static int sql_query_uses_aggregate(const SqlSelectQuery *query) {
    unsigned int i;

    if (query->aggregate_count > 0U) {
        return 1;
    }
    for (i = 0U; i < query->item_count; ++i) {
        if (query->items[i].kind != SQL_SELECT_COLUMN && query->items[i].kind != SQL_SELECT_ADD && query->items[i].kind != SQL_SELECT_SUB && query->items[i].kind != SQL_SELECT_CONCAT) {
            return 1;
        }
    }
    return sql_condition_list_uses_aggregate(&query->having);
}

static void sql_configure_select_collection_limit(SqlSelectQuery *query) {
    unsigned long long needed;

    if (query == 0 || !query->has_limit || query->order_count != 0U || query->distinct || query->group_count != 0U || query->having.count != 0U || sql_query_uses_aggregate(query)) {
        return;
    }
    needed = (unsigned long long)query->offset + (unsigned long long)query->limit;
    if (query->limit == 0U) {
        needed = 0ULL;
    } else if (needed > (unsigned long long)SQL_MAX_RESULT_ROWS) {
        needed = (unsigned long long)SQL_MAX_RESULT_ROWS;
    }
    query->collection_limit_enabled = 1;
    query->collection_limit = (unsigned int)needed;
}

static int sql_group_select_rows(const SqlSelectQuery *query, const SqlResultRow *rows, unsigned int row_count, SqlResultBuffer *groups) {
    unsigned int row_index;
    int aggregate = sql_query_uses_aggregate(query);

    groups->count = 0U;
    if (query->group_count == 0U && !aggregate) {
        if (sql_ensure_result_capacity(groups, row_count) != 0) {
            return -1;
        }
        for (row_index = 0U; row_index < row_count; ++row_index) {
            sql_set_result_buffer_row(query, groups, row_index, &rows[row_index]);
        }
        groups->count = row_count;
        return 0;
    }
    if (query->group_count == 0U && aggregate) {
        unsigned int i;
        for (i = 0U; i < query->item_count; ++i) {
            if (query->items[i].kind == SQL_SELECT_COLUMN) {
                return -1;
            }
        }
        if (sql_ensure_result_capacity(groups, 1U) != 0) {
            return -1;
        }
        rt_memset(groups->rows[0].rows, 0, sizeof(groups->rows[0].rows));
        groups->rows[0].count = row_count;
        if (row_count > 0U) {
            sql_set_result_buffer_row(query, groups, 0U, &rows[0]);
            groups->rows[0].count = row_count;
        }
        groups->count = 1U;
        return 0;
    }
    for (row_index = 0U; row_index < row_count; ++row_index) {
        unsigned int group_index;
        for (group_index = 0U; group_index < groups->count; ++group_index) {
            if (sql_result_rows_same_group(&groups->rows[group_index], &rows[row_index], query)) {
                groups->rows[group_index].count += 1U;
                break;
            }
        }
        if (group_index == groups->count) {
            if (groups->count >= SQL_MAX_RESULT_ROWS || sql_ensure_result_capacity(groups, groups->count + 1U) != 0) {
                return -1;
            }
            sql_set_result_buffer_row(query, groups, groups->count, &rows[row_index]);
            groups->rows[groups->count].count = 1U;
            groups->count += 1U;
        }
    }
    return 0;
}

static int sql_compute_group_aggregates(SqlDatabase *db, const SqlSelectQuery *query, const SqlResultRow *all_rows, unsigned int all_row_count, SqlResultRow *groups, unsigned int group_count) {
    unsigned int group_index;

    for (group_index = 0U; group_index < group_count; ++group_index) {
        unsigned int aggregate_index;
        for (aggregate_index = 0U; aggregate_index < query->aggregate_count; ++aggregate_index) {
            char value[SQL_VALUE_SIZE];
            if (sql_compute_aggregate_value(query, &groups[group_index], all_rows, all_row_count, &query->aggregates[aggregate_index], value, sizeof(value)) != 0 ||
                sql_store_value(db, value, &groups[group_index].aggregates[aggregate_index]) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int sql_project_select_rows(SqlDatabase *db, const SqlSelectQuery *query, SqlResultRow *rows, unsigned int *row_count_io) {
    unsigned int read_index;
    unsigned int write_index = 0U;

    for (read_index = 0U; read_index < *row_count_io; ++read_index) {
        unsigned int item_index;
        if (!sql_condition_list_matches(&query->having, &rows[read_index])) {
            continue;
        }
        if (write_index != read_index) {
            sql_copy_result_row(query, &rows[write_index], &rows[read_index]);
        }
        for (item_index = 0U; item_index < query->item_count; ++item_index) {
            if (sql_select_item_offset(db, &query->items[item_index], &rows[write_index], &rows[write_index].values[item_index]) != 0) {
                return -1;
            }
        }
        write_index += 1U;
    }
    *row_count_io = write_index;
    return 0;
}

static int sql_projected_rows_equal(const SqlSelectQuery *query, const SqlResultRow *left, const SqlResultRow *right) {
    unsigned int item_index;

    for (item_index = 0U; item_index < query->item_count; ++item_index) {
        if (rt_strcmp(sql_value_at(left->values[item_index]), sql_value_at(right->values[item_index])) != 0) {
            return 0;
        }
    }
    return 1;
}

static void sql_distinct_select_rows(const SqlSelectQuery *query, SqlResultRow *rows, unsigned int *row_count_io) {
    unsigned int read_index;
    unsigned int write_index = 0U;

    if (!query->distinct) {
        return;
    }
    for (read_index = 0U; read_index < *row_count_io; ++read_index) {
        unsigned int seen_index;
        int seen = 0;
        for (seen_index = 0U; seen_index < write_index; ++seen_index) {
            if (sql_projected_rows_equal(query, &rows[seen_index], &rows[read_index])) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            if (write_index != read_index) {
                sql_copy_result_row(query, &rows[write_index], &rows[read_index]);
            }
            write_index += 1U;
        }
    }
    *row_count_io = write_index;
}

static const char *sql_order_value(const SqlSelectQuery *query, const SqlOrderKey *key, const SqlResultRow *row) {
    int label_index;

    if (key->label[0] != '\0') {
        label_index = sql_find_select_label(query, key->label);
        return label_index >= 0 ? sql_value_at(row->values[label_index]) : "";
    }
    return sql_row_value(row->rows[key->column.table_index], (unsigned int)key->column.column_index);
}

static int sql_compare_order_rows(const SqlSelectQuery *query, const SqlResultRow *left, const SqlResultRow *right) {
    unsigned int key_index;

    for (key_index = 0U; key_index < query->order_count; ++key_index) {
        const SqlOrderKey *key = &query->order_by[key_index];
        int cmp;

        if (key->label[0] == '\0') {
            long long left_number;
            long long right_number;
            const SqlRow *left_row = left->rows[key->column.table_index];
            const SqlRow *right_row = right->rows[key->column.table_index];
            if (sql_row_numeric_value(left_row, (unsigned int)key->column.column_index, &left_number) == 0 &&
                sql_row_numeric_value(right_row, (unsigned int)key->column.column_index, &right_number) == 0) {
                cmp = left_number < right_number ? -1 : (left_number > right_number ? 1 : 0);
            } else {
                cmp = sql_compare_values(sql_order_value(query, key, left), sql_order_value(query, key, right));
            }
        } else {
            cmp = sql_compare_values(sql_order_value(query, key, left), sql_order_value(query, key, right));
        }
        if (key->desc) {
            cmp = -cmp;
        }
        if (cmp != 0) {
            return cmp;
        }
    }
    return 0;
}

static void sql_swap_result_rows(SqlResultRow *left, SqlResultRow *right) {
    SqlResultRow tmp = *left;
    *left = *right;
    *right = tmp;
}

static void sql_sift_order_heap(const SqlSelectQuery *query, SqlResultRow *rows, unsigned int start, unsigned int end) {
    unsigned int root = start;

    for (;;) {
        unsigned int child = root * 2U + 1U;
        unsigned int swap_index = root;

        if (child > end) {
            return;
        }
        if (sql_compare_order_rows(query, &rows[swap_index], &rows[child]) < 0) {
            swap_index = child;
        }
        if (child + 1U <= end && sql_compare_order_rows(query, &rows[swap_index], &rows[child + 1U]) < 0) {
            swap_index = child + 1U;
        }
        if (swap_index == root) {
            return;
        }
        sql_swap_result_rows(&rows[root], &rows[swap_index]);
        root = swap_index;
    }
}

static void sql_sort_select_rows(const SqlSelectQuery *query, SqlResultRow *rows, unsigned int row_count) {
    unsigned int start;
    unsigned int end;

    if (query->order_count == 0U || row_count < 2U) {
        return;
    }
    start = (row_count - 2U) / 2U + 1U;
    while (start > 0U) {
        start -= 1U;
        sql_sift_order_heap(query, rows, start, row_count - 1U);
    }
    end = row_count - 1U;
    while (end > 0U) {
        sql_swap_result_rows(&rows[end], &rows[0]);
        end -= 1U;
        sql_sift_order_heap(query, rows, 0U, end);
    }
}

static void sql_write_select_rows(const SqlSelectQuery *query, const SqlResultRow *rows, unsigned int row_count) {
    unsigned int item_index;
    unsigned int row_index;
    unsigned int start = query->has_offset ? (query->offset < row_count ? query->offset : row_count) : 0U;
    unsigned int available = row_count - start;
    unsigned int limit = query->has_limit && query->limit < available ? query->limit : available;

    for (item_index = 0U; item_index < query->item_count; ++item_index) {
        if (item_index > 0U) {
            rt_write_char(1, '\t');
        }
        rt_write_cstr(1, query->items[item_index].label);
    }
    rt_write_char(1, '\n');
    for (row_index = start; row_index < start + limit; ++row_index) {
        for (item_index = 0U; item_index < query->item_count; ++item_index) {
            if (item_index > 0U) {
                rt_write_char(1, '\t');
            }
            rt_write_cstr(1, sql_value_at(rows[row_index].values[item_index]));
        }
        rt_write_char(1, '\n');
    }
}

static int sql_execute_select(SqlDatabase *db, SqlParser *parser) {
    SqlSelectQuery query;
    SqlResultRow current;
    SqlResultBuffer result_rows;
    SqlResultBuffer group_rows;
    SqlSelectScratch scratch;
    int result = -1;

    sql_invalidate_runtime_caches();
    rt_memset(&query, 0, sizeof(query));
    rt_memset(&current, 0, sizeof(current));
    rt_memset(&scratch, 0, sizeof(scratch));
    sql_init_result_buffer(&result_rows, 0U, 0U);
    sql_init_result_buffer(&group_rows, 0U, 0U);
    if (sql_parse_select_list(parser, &query, &scratch) != 0 ||
        sql_parse_select_tail(db, parser, &query, &scratch) != 0) {
        goto out;
    }
    sql_configure_select_collection_limit(&query);
    group_rows.value_slots = query.item_count;
    group_rows.aggregate_slots = query.aggregate_count;
    if (sql_collect_select_rows(&query, 0U, &current, &result_rows) != 0 ||
        sql_group_select_rows(&query, result_rows.rows, result_rows.count, &group_rows) != 0 ||
        sql_compute_group_aggregates(db, &query, result_rows.rows, result_rows.count, group_rows.rows, group_rows.count) != 0 ||
        sql_project_select_rows(db, &query, group_rows.rows, &group_rows.count) != 0) {
        goto out;
    }
    sql_distinct_select_rows(&query, group_rows.rows, &group_rows.count);
    sql_sort_select_rows(&query, group_rows.rows, group_rows.count);
    sql_write_select_rows(&query, group_rows.rows, group_rows.count);
    result = 0;

out:
    sql_free_result_buffer(&result_rows);
    sql_free_result_buffer(&group_rows);
    sql_free_select_scratch(&scratch);
    sql_free_select_query(&query);
    return result;
}

