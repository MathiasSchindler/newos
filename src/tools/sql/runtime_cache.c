#include "internal.h"

static int sql_find_row_location(const SqlRow *row, const SqlTable **table_out, unsigned int *row_index_out) {
    unsigned int table_index;

    if (row == 0) {
        return -1;
    }
    for (table_index = 0U; table_index < sql_database.table_count; ++table_index) {
        const SqlTable *table = &sql_database.tables[table_index];
        unsigned int row_index;
        for (row_index = 0U; row_index < table->row_count; ++row_index) {
            if (&table->rows[row_index] == row) {
                if (table_out != 0) {
                    *table_out = table;
                }
                if (row_index_out != 0) {
                    *row_index_out = row_index;
                }
                return 0;
            }
        }
    }
    return -1;
}

static SqlNumericCache *sql_numeric_cache_for_column(const SqlTable *table, unsigned int column) {
    unsigned int slot_index;
    unsigned int row_index;
    SqlNumericCache *slot = 0;

    if (table == 0 || column >= table->column_count || table->column_types[column] == SQL_TYPE_TEXT) {
        return 0;
    }
    for (slot_index = 0U; slot_index < SQL_NUMERIC_CACHE_SLOTS; ++slot_index) {
        if (sql_numeric_caches[slot_index].valid && sql_numeric_caches[slot_index].table == table && sql_numeric_caches[slot_index].column == column) {
            return &sql_numeric_caches[slot_index];
        }
        if (slot == 0 && !sql_numeric_caches[slot_index].valid) {
            slot = &sql_numeric_caches[slot_index];
        }
    }
    if (slot == 0) {
        slot = &sql_numeric_caches[0];
    }
    if (slot->row_capacity < table->row_count) {
        long long *values = (long long *)sql_resize_array(slot->values, slot->row_capacity, table->row_count, sizeof(long long));
        unsigned char *present;
        if (values == 0) {
            slot->valid = 0;
            return 0;
        }
        slot->values = values;
        present = (unsigned char *)sql_resize_array(slot->present, slot->row_capacity, table->row_count, sizeof(unsigned char));
        if (present == 0) {
            slot->valid = 0;
            return 0;
        }
        slot->present = present;
        slot->row_capacity = table->row_count;
    }
    slot->table = table;
    slot->column = column;
    slot->valid = 1;
    for (row_index = 0U; row_index < table->row_count; ++row_index) {
        long long scaled;
        if (!sql_row_value_is_null(&table->rows[row_index], column) && sql_parse_decimal_scaled(sql_row_value(&table->rows[row_index], column), &scaled, 0) == 0) {
            slot->values[row_index] = scaled;
            slot->present[row_index] = 1U;
        } else {
            slot->values[row_index] = 0LL;
            slot->present[row_index] = 0U;
        }
    }
    return slot;
}

static int sql_table_row_numeric_value(const SqlTable *table, unsigned int row_index, unsigned int column, long long *value_out) {
    SqlNumericCache *cache = sql_numeric_cache_for_column(table, column);

    if (cache == 0 || row_index >= table->row_count || !cache->present[row_index]) {
        return -1;
    }
    *value_out = cache->values[row_index];
    return 0;
}

static int sql_row_numeric_value(const SqlRow *row, unsigned int column, long long *value_out) {
    const SqlTable *table;
    unsigned int row_index;

    if (sql_find_row_location(row, &table, &row_index) != 0) {
        return -1;
    }
    return sql_table_row_numeric_value(table, row_index, column, value_out);
}

static int sql_result_row_numeric_value(const SqlResultRow *row, unsigned int table_index, unsigned int column, long long *value_out) {
    if (row == 0 || table_index >= SQL_MAX_QUERY_TABLES || row->tables[table_index] == 0 || row->rows[table_index] == 0 ||
        row->row_indices[table_index] == SQL_ROW_INDEX_NONE) {
        return -1;
    }
    return sql_table_row_numeric_value(row->tables[table_index], row->row_indices[table_index], column, value_out);
}

static int sql_condition_value_numeric(const SqlConditionValue *value, const SqlResultRow *row, long long *number_out) {
    if (value->is_null || value->is_aggregate || value->is_count) {
        return -1;
    }
    if (value->is_column) {
        unsigned int table_index = (unsigned int)value->column.table_index;
        if (sql_result_row_numeric_value(row, table_index, (unsigned int)value->column.column_index, number_out) == 0) {
            return 0;
        }
        if (row != 0 && table_index < SQL_MAX_QUERY_TABLES) {
            return sql_row_numeric_value(row->rows[table_index], (unsigned int)value->column.column_index, number_out);
        }
        return -1;
    }
    return sql_parse_decimal_scaled(value->value, number_out, 0);
}

static int sql_compare_condition_values(const SqlConditionValue *left, const SqlConditionValue *right, const SqlResultRow *row) {
    long long left_number;
    long long right_number;

    if (sql_condition_value_numeric(left, row, &left_number) == 0 && sql_condition_value_numeric(right, row, &right_number) == 0) {
        if (left_number < right_number) {
            return -1;
        }
        if (left_number > right_number) {
            return 1;
        }
        return 0;
    }
    return sql_compare_values(sql_condition_value_text(left, row), sql_condition_value_text(right, row));
}

static int sql_compare_table_row_value_to_text(const SqlTable *table, unsigned int row_index, unsigned int column, const char *text) {
    long long left_number;
    long long right_number;

    if (sql_table_row_numeric_value(table, row_index, column, &left_number) == 0 && sql_parse_decimal_scaled(text, &right_number, 0) == 0) {
        if (left_number < right_number) {
            return -1;
        }
        if (left_number > right_number) {
            return 1;
        }
        return 0;
    }
    return sql_compare_values(sql_row_value(&table->rows[row_index], column), text);
}

static int sql_compare_table_rows_by_column(const SqlTable *table, unsigned int left_row, unsigned int right_row, unsigned int column) {
    long long left_number;
    long long right_number;

    if (sql_table_row_numeric_value(table, left_row, column, &left_number) == 0 && sql_table_row_numeric_value(table, right_row, column, &right_number) == 0) {
        if (left_number < right_number) {
            return -1;
        }
        if (left_number > right_number) {
            return 1;
        }
        return 0;
    }
    return sql_compare_values(sql_row_value(&table->rows[left_row], column), sql_row_value(&table->rows[right_row], column));
}

static void sql_swap_uint(unsigned int *left, unsigned int *right) {
    unsigned int tmp = *left;
    *left = *right;
    *right = tmp;
}

static void sql_sift_index_heap(SqlIndexCache *cache, unsigned int start, unsigned int end) {
    unsigned int root = start;

    for (;;) {
        unsigned int child = root * 2U + 1U;
        unsigned int swap_index = root;

        if (child > end) {
            return;
        }
        if (sql_compare_table_rows_by_column(cache->table, cache->row_ids[swap_index], cache->row_ids[child], cache->column) < 0) {
            swap_index = child;
        }
        if (child + 1U <= end && sql_compare_table_rows_by_column(cache->table, cache->row_ids[swap_index], cache->row_ids[child + 1U], cache->column) < 0) {
            swap_index = child + 1U;
        }
        if (swap_index == root) {
            return;
        }
        sql_swap_uint(&cache->row_ids[root], &cache->row_ids[swap_index]);
        root = swap_index;
    }
}

static void sql_sort_index_cache(SqlIndexCache *cache) {
    unsigned int start;
    unsigned int end;

    if (cache->row_count < 2U) {
        return;
    }
    start = (cache->row_count - 2U) / 2U + 1U;
    while (start > 0U) {
        start -= 1U;
        sql_sift_index_heap(cache, start, cache->row_count - 1U);
    }
    end = cache->row_count - 1U;
    while (end > 0U) {
        sql_swap_uint(&cache->row_ids[end], &cache->row_ids[0]);
        end -= 1U;
        sql_sift_index_heap(cache, 0U, end);
    }
}

static SqlIndexCache *sql_index_cache_for_column(const SqlTable *table, unsigned int column) {
    unsigned int slot_index;
    unsigned int row_index;
    SqlIndexCache *slot = 0;

    if (table == 0 || column >= table->column_count) {
        return 0;
    }
    for (slot_index = 0U; slot_index < SQL_INDEX_SLOTS; ++slot_index) {
        if (sql_index_caches[slot_index].valid && sql_index_caches[slot_index].table == table && sql_index_caches[slot_index].column == column) {
            return &sql_index_caches[slot_index];
        }
        if (slot == 0 && !sql_index_caches[slot_index].valid) {
            slot = &sql_index_caches[slot_index];
        }
    }
    if (slot == 0) {
        slot = &sql_index_caches[0];
    }
    if (slot->row_capacity < table->row_count) {
        unsigned int *row_ids = (unsigned int *)sql_resize_array(slot->row_ids, slot->row_capacity, table->row_count, sizeof(unsigned int));
        if (row_ids == 0) {
            slot->valid = 0;
            return 0;
        }
        slot->row_ids = row_ids;
        slot->row_capacity = table->row_count;
    }
    slot->table = table;
    slot->column = column;
    slot->row_count = table->row_count;
    slot->valid = 1;
    for (row_index = 0U; row_index < table->row_count; ++row_index) {
        slot->row_ids[row_index] = row_index;
    }
    sql_sort_index_cache(slot);
    return slot;
}

static void sql_index_equal_range(const SqlIndexCache *index, const char *value, unsigned int *start_out, unsigned int *end_out) {
    unsigned int low = 0U;
    unsigned int high = index->row_count;

    while (low < high) {
        unsigned int mid = low + (high - low) / 2U;
        if (sql_compare_table_row_value_to_text(index->table, index->row_ids[mid], index->column, value) < 0) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    *start_out = low;
    high = index->row_count;
    while (low < high) {
        unsigned int mid = low + (high - low) / 2U;
        if (sql_compare_table_row_value_to_text(index->table, index->row_ids[mid], index->column, value) <= 0) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    *end_out = low;
}
