#define SQL_CORE_FRAGMENT 1
#include "internal.h"
#undef SQL_CORE_FRAGMENT

static SqlDatabase sql_database;
static SqlNumericCache sql_numeric_caches[SQL_NUMERIC_CACHE_SLOTS];
static SqlIndexCache sql_index_caches[SQL_INDEX_SLOTS];

static int sql_valid_identifier(const char *text);
static int sql_next_token(SqlParser *parser);
static int sql_expect_symbol(SqlParser *parser, char symbol);
static int sql_try_symbol(SqlParser *parser, char symbol);
static const char *sql_value_at(unsigned int offset);
static const char *sql_row_value(const SqlRow *row, unsigned int column);
static int sql_row_value_is_null(const SqlRow *row, unsigned int column);
static const char *sql_row_display_value(const SqlRow *row, unsigned int column);
static int sql_store_value(SqlDatabase *db, const char *value, unsigned int *offset_out);
static int sql_parse_decimal_scaled(const char *text, long long *scaled_out, int *integer_out);
static int sql_store_decimal_scaled(long long scaled, char *buffer, size_t buffer_size);
static int sql_compare_values(const char *left, const char *right);
static void sql_store_uint_text(unsigned long long value, char *buffer, size_t buffer_size);
static int sql_row_numeric_value(const SqlRow *row, unsigned int column, long long *value_out);
static void sql_invalidate_runtime_caches(void);

static int sql_multiply_size(size_t left, size_t right, size_t *out) {
    if (out == 0 || (right != 0U && left > ((size_t)-1) / right)) {
        return -1;
    }
    *out = left * right;
    return 0;
}

static void sql_free_bytes(void *ptr) {
    rt_free(ptr);
}

static void *sql_resize_bytes(void *ptr, size_t old_size, size_t new_size) {
    if (new_size == 0U) {
        sql_free_bytes(ptr);
        return 0;
    }
    (void)old_size;
    return rt_realloc(ptr, new_size);
}

static void *sql_resize_array(void *ptr, unsigned int old_count, unsigned int new_count, size_t item_size) {
    size_t old_size;
    size_t new_size;
    void *new_ptr;

    if (sql_multiply_size((size_t)old_count, item_size, &old_size) != 0 ||
        sql_multiply_size((size_t)new_count, item_size, &new_size) != 0) {
        return 0;
    }
    new_ptr = sql_resize_bytes(ptr, old_size, new_size);
    if (new_ptr != 0 && new_count > old_count) {
        memset((char *)new_ptr + old_size, 0, new_size - old_size);
    }
    return new_ptr;
}

static SqlTextBuffer sql_import_line;

static int sql_next_capacity(unsigned int current, unsigned int needed, unsigned int max, unsigned int initial, unsigned int *capacity_out) {
    unsigned int capacity = current;

    if (needed > max || capacity_out == 0) {
        return -1;
    }
    if (capacity >= needed) {
        *capacity_out = capacity;
        return 0;
    }
    if (capacity == 0U) {
        capacity = initial;
    }
    while (capacity < needed) {
        unsigned int next = capacity * 2U;
        if (next <= capacity || next > max) {
            next = max;
        }
        capacity = next;
    }
    *capacity_out = capacity;
    return 0;
}

static int sql_ensure_value_capacity(SqlDatabase *db, unsigned int needed) {
    unsigned int capacity;
    char *values;

    if (db == 0 || sql_next_capacity(db->value_capacity, needed, SQL_MAX_VALUE_BYTES, SQL_INITIAL_VALUE_CAPACITY, &capacity) != 0) {
        return -1;
    }
    if (capacity == db->value_capacity) {
        return 0;
    }
    values = (char *)sql_resize_array(db->values, db->value_capacity, capacity, sizeof(char));
    if (values == 0) {
        return -1;
    }
    db->values = values;
    db->value_capacity = capacity;
    db->values[0] = '\0';
    return 0;
}

static int sql_ensure_result_capacity(SqlResultBuffer *buffer, unsigned int needed) {
    unsigned int capacity;
    SqlResultRow *rows;
    unsigned int *values = 0;
    unsigned int *aggregates = 0;
    size_t old_value_count;
    size_t new_value_count;
    size_t old_aggregate_count;
    size_t new_aggregate_count;
    unsigned int row_index;

    if (buffer == 0 || sql_next_capacity(buffer->capacity, needed, SQL_MAX_RESULT_ROWS, SQL_INITIAL_RESULT_CAPACITY, &capacity) != 0) {
        return -1;
    }
    if (capacity == buffer->capacity) {
        return 0;
    }
    rows = (SqlResultRow *)sql_resize_array(buffer->rows, buffer->capacity, capacity, sizeof(SqlResultRow));
    if (rows == 0) {
        return -1;
    }
    buffer->rows = rows;
    if (sql_multiply_size((size_t)buffer->capacity, (size_t)buffer->value_slots, &old_value_count) != 0 ||
        sql_multiply_size((size_t)capacity, (size_t)buffer->value_slots, &new_value_count) != 0 ||
        old_value_count > (size_t)((unsigned int)-1) || new_value_count > (size_t)((unsigned int)-1)) {
        return -1;
    }
    if (buffer->value_slots != 0U) {
        values = (unsigned int *)sql_resize_array(buffer->values, (unsigned int)old_value_count, (unsigned int)new_value_count, sizeof(unsigned int));
        if (values == 0) {
            return -1;
        }
        buffer->values = values;
    }
    if (sql_multiply_size((size_t)buffer->capacity, (size_t)buffer->aggregate_slots, &old_aggregate_count) != 0 ||
        sql_multiply_size((size_t)capacity, (size_t)buffer->aggregate_slots, &new_aggregate_count) != 0 ||
        old_aggregate_count > (size_t)((unsigned int)-1) || new_aggregate_count > (size_t)((unsigned int)-1)) {
        return -1;
    }
    if (buffer->aggregate_slots != 0U) {
        aggregates = (unsigned int *)sql_resize_array(buffer->aggregates, (unsigned int)old_aggregate_count, (unsigned int)new_aggregate_count, sizeof(unsigned int));
        if (aggregates == 0) {
            return -1;
        }
        buffer->aggregates = aggregates;
    }
    buffer->capacity = capacity;
    for (row_index = 0U; row_index < buffer->capacity; ++row_index) {
        buffer->rows[row_index].values = buffer->value_slots == 0U ? 0 : buffer->values + ((size_t)row_index * buffer->value_slots);
        buffer->rows[row_index].aggregates = buffer->aggregate_slots == 0U ? 0 : buffer->aggregates + ((size_t)row_index * buffer->aggregate_slots);
    }
    return 0;
}

static void sql_init_result_buffer(SqlResultBuffer *buffer, unsigned int value_slots, unsigned int aggregate_slots) {
    rt_memset(buffer, 0, sizeof(*buffer));
    buffer->value_slots = value_slots;
    buffer->aggregate_slots = aggregate_slots;
}

static void sql_free_result_buffer(SqlResultBuffer *buffer) {
    if (buffer == 0) {
        return;
    }
    sql_free_bytes(buffer->rows);
    sql_free_bytes(buffer->values);
    sql_free_bytes(buffer->aggregates);
    rt_memset(buffer, 0, sizeof(*buffer));
}

static int sql_ensure_select_item_capacity(SqlSelectQuery *query, unsigned int needed) {
    unsigned int capacity;
    SqlSelectItem *items;

    if (query == 0 || sql_next_capacity(query->item_capacity, needed, SQL_MAX_COLUMNS, SQL_INITIAL_COLUMN_CAPACITY, &capacity) != 0) {
        return -1;
    }
    if (capacity == query->item_capacity) {
        return 0;
    }
    items = (SqlSelectItem *)sql_resize_array(query->items, query->item_capacity, capacity, sizeof(SqlSelectItem));
    if (items == 0) {
        return -1;
    }
    query->items = items;
    query->item_capacity = capacity;
    return 0;
}

static int sql_ensure_select_aggregate_capacity(SqlSelectQuery *query, unsigned int needed) {
    unsigned int capacity;
    SqlAggregate *aggregates;

    if (query == 0 || sql_next_capacity(query->aggregate_capacity, needed, SQL_MAX_COLUMNS, SQL_INITIAL_COLUMN_CAPACITY, &capacity) != 0) {
        return -1;
    }
    if (capacity == query->aggregate_capacity) {
        return 0;
    }
    aggregates = (SqlAggregate *)sql_resize_array(query->aggregates, query->aggregate_capacity, capacity, sizeof(SqlAggregate));
    if (aggregates == 0) {
        return -1;
    }
    query->aggregates = aggregates;
    query->aggregate_capacity = capacity;
    return 0;
}

static int sql_ensure_select_scratch_capacity(SqlSelectScratch *scratch, unsigned int needed) {
    unsigned int capacity;
    char (*raw_items)[SQL_VALUE_SIZE];
    char (*raw_expr_right)[SQL_VALUE_SIZE];
    int *raw_kinds;
    char (*raw_labels)[SQL_VALUE_SIZE];

    if (scratch == 0 || sql_next_capacity(scratch->capacity, needed, SQL_MAX_COLUMNS, SQL_INITIAL_COLUMN_CAPACITY, &capacity) != 0) {
        return -1;
    }
    if (capacity == scratch->capacity) {
        return 0;
    }
    raw_items = (char (*)[SQL_VALUE_SIZE])sql_resize_array(scratch->raw_items, scratch->capacity, capacity, sizeof(char[SQL_VALUE_SIZE]));
    if (raw_items == 0) {
        return -1;
    }
    scratch->raw_items = raw_items;
    raw_expr_right = (char (*)[SQL_VALUE_SIZE])sql_resize_array(scratch->raw_expr_right, scratch->capacity, capacity, sizeof(char[SQL_VALUE_SIZE]));
    if (raw_expr_right == 0) {
        return -1;
    }
    scratch->raw_expr_right = raw_expr_right;
    raw_kinds = (int *)sql_resize_array(scratch->raw_kinds, scratch->capacity, capacity, sizeof(int));
    if (raw_kinds == 0) {
        return -1;
    }
    scratch->raw_kinds = raw_kinds;
    raw_labels = (char (*)[SQL_VALUE_SIZE])sql_resize_array(scratch->raw_labels, scratch->capacity, capacity, sizeof(char[SQL_VALUE_SIZE]));
    if (raw_labels == 0) {
        return -1;
    }
    scratch->raw_labels = raw_labels;
    scratch->capacity = capacity;
    return 0;
}

static void sql_free_select_query(SqlSelectQuery *query) {
    if (query == 0) {
        return;
    }
    sql_free_bytes(query->items);
    sql_free_bytes(query->aggregates);
    query->items = 0;
    query->aggregates = 0;
    query->item_capacity = 0U;
    query->aggregate_capacity = 0U;
}

static void sql_free_select_scratch(SqlSelectScratch *scratch) {
    if (scratch == 0) {
        return;
    }
    sql_free_bytes(scratch->raw_items);
    sql_free_bytes(scratch->raw_expr_right);
    sql_free_bytes(scratch->raw_kinds);
    sql_free_bytes(scratch->raw_labels);
    rt_memset(scratch, 0, sizeof(*scratch));
}

static void sql_copy_result_row(const SqlSelectQuery *query, SqlResultRow *dst, const SqlResultRow *src) {
    unsigned int source_index;

    if (dst == src) {
        return;
    }
    for (source_index = 0U; source_index < SQL_MAX_QUERY_TABLES; ++source_index) {
        dst->tables[source_index] = src->tables[source_index];
        dst->rows[source_index] = src->rows[source_index];
        dst->row_indices[source_index] = src->row_indices[source_index];
    }
    dst->count = src->count;
    if (dst->values != 0 && src->values != 0 && query->item_count != 0U) {
        memcpy(dst->values, src->values, (size_t)query->item_count * sizeof(unsigned int));
    }
    if (dst->aggregates != 0 && src->aggregates != 0 && query->aggregate_count != 0U) {
        memcpy(dst->aggregates, src->aggregates, (size_t)query->aggregate_count * sizeof(unsigned int));
    }
}

static void sql_set_result_buffer_row(const SqlSelectQuery *query, SqlResultBuffer *buffer, unsigned int index, const SqlResultRow *src) {
    sql_copy_result_row(query, &buffer->rows[index], src);
}

static int sql_ensure_text_capacity(SqlTextBuffer *buffer, unsigned int needed, unsigned int max) {
    unsigned int capacity;
    char *data;

    if (buffer == 0 || sql_next_capacity(buffer->capacity, needed, max, SQL_INITIAL_TEXT_CAPACITY, &capacity) != 0) {
        return -1;
    }
    if (capacity == buffer->capacity) {
        return 0;
    }
    data = (char *)sql_resize_array(buffer->data, buffer->capacity, capacity, sizeof(char));
    if (data == 0) {
        return -1;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 0;
}

static void sql_free_text_buffer(SqlTextBuffer *buffer) {
    if (buffer == 0) {
        return;
    }
    sql_free_bytes(buffer->data);
    buffer->data = 0;
    buffer->capacity = 0U;
}

static void sql_free_table_rows(SqlTable *table) {
    if (table != 0) {
        sql_free_bytes(table->row_values);
        table->row_values = 0;
        sql_free_bytes(table->rows);
        table->rows = 0;
        table->row_capacity = 0U;
        table->row_count = 0U;
    }
}

static void sql_bind_table_rows(SqlTable *table) {
    unsigned int row_index;

    if (table == 0 || table->rows == 0 || table->row_values == 0 || table->column_capacity == 0U) {
        return;
    }
    for (row_index = 0U; row_index < table->row_capacity; ++row_index) {
        table->rows[row_index].values = table->row_values + ((size_t)row_index * table->column_capacity);
    }
}

static int sql_resize_table_row_values(SqlTable *table, unsigned int row_capacity, unsigned int column_capacity) {
    unsigned int *values;
    size_t new_value_count;
    unsigned int row_index;
    unsigned int copy_columns;

    if (table == 0 || row_capacity == 0U || column_capacity == 0U) {
        sql_free_bytes(table != 0 ? table->row_values : 0);
        if (table != 0) {
            table->row_values = 0;
        }
        return 0;
    }
    if (sql_multiply_size((size_t)row_capacity, (size_t)column_capacity, &new_value_count) != 0 || new_value_count > (size_t)((unsigned int)-1)) {
        return -1;
    }
    values = (unsigned int *)sql_resize_array(0, 0U, (unsigned int)new_value_count, sizeof(unsigned int));
    if (values == 0) {
        return -1;
    }
    copy_columns = table->column_count < table->column_capacity ? table->column_count : table->column_capacity;
    if (copy_columns > column_capacity) {
        copy_columns = column_capacity;
    }
    for (row_index = 0U; row_index < table->row_count; ++row_index) {
        memcpy(values + ((size_t)row_index * column_capacity),
               table->row_values + ((size_t)row_index * table->column_capacity),
               (size_t)copy_columns * sizeof(unsigned int));
    }
    sql_free_bytes(table->row_values);
    table->row_values = values;
    return 0;
}

static void sql_copy_row_values(const SqlTable *table, SqlRow *dst, const SqlRow *src) {
    if (table == 0 || dst == 0 || src == 0 || dst == src || table->column_count == 0U) {
        return;
    }
    memcpy(dst->values, src->values, (size_t)table->column_count * sizeof(unsigned int));
}

static void sql_free_table_columns(SqlTable *table) {
    if (table == 0) {
        return;
    }
    sql_free_bytes(table->columns);
    sql_free_bytes(table->column_types);
    sql_free_bytes(table->not_null);
    sql_free_bytes(table->has_default);
    sql_free_bytes(table->unique);
    sql_free_bytes(table->primary_key);
    sql_free_bytes(table->defaults);
    table->columns = 0;
    table->column_types = 0;
    table->not_null = 0;
    table->has_default = 0;
    table->unique = 0;
    table->primary_key = 0;
    table->defaults = 0;
    table->column_capacity = 0U;
    table->column_count = 0U;
}

static void sql_free_table_storage(SqlTable *table) {
    sql_free_table_rows(table);
    sql_free_table_columns(table);
}

static int sql_ensure_table_capacity(SqlDatabase *db, unsigned int needed) {
    unsigned int capacity;
    SqlTable *tables;

    if (db == 0 || sql_next_capacity(db->table_capacity, needed, SQL_MAX_TABLES, SQL_INITIAL_TABLE_CAPACITY, &capacity) != 0) {
        return -1;
    }
    if (capacity == db->table_capacity) {
        return 0;
    }
    tables = (SqlTable *)sql_resize_array(db->tables, db->table_capacity, capacity, sizeof(SqlTable));
    if (tables == 0) {
        return -1;
    }
    db->tables = tables;
    db->table_capacity = capacity;
    sql_invalidate_runtime_caches();
    return 0;
}

static int sql_ensure_row_capacity(SqlTable *table, unsigned int needed) {
    unsigned int capacity;
    SqlRow *rows;

    if (table == 0 || table->column_count == 0U ||
        sql_next_capacity(table->row_capacity, needed, SQL_MAX_ROWS, SQL_INITIAL_ROW_CAPACITY, &capacity) != 0 ||
        sql_ensure_column_capacity(table, table->column_count) != 0) {
        return -1;
    }
    if (capacity == table->row_capacity) {
        return 0;
    }
    rows = (SqlRow *)sql_resize_array(table->rows, table->row_capacity, capacity, sizeof(SqlRow));
    if (rows == 0) {
        return -1;
    }
    table->rows = rows;
    if (sql_resize_table_row_values(table, capacity, table->column_capacity) != 0) {
        return -1;
    }
    table->row_capacity = capacity;
    sql_bind_table_rows(table);
    return 0;
}

static int sql_ensure_column_capacity(SqlTable *table, unsigned int needed) {
    unsigned int capacity;
    char (*columns)[SQL_NAME_SIZE];
    unsigned char *column_types;
    unsigned char *not_null;
    unsigned char *has_default;
    unsigned char *unique;
    unsigned char *primary_key;
    unsigned int *defaults;
    unsigned int column_index;

    if (table == 0 || sql_next_capacity(table->column_capacity, needed, SQL_MAX_COLUMNS, SQL_INITIAL_COLUMN_CAPACITY, &capacity) != 0) {
        return -1;
    }
    if (capacity == table->column_capacity) {
        return 0;
    }
    columns = (char (*)[SQL_NAME_SIZE])sql_resize_array(table->columns, table->column_capacity, capacity, sizeof(char[SQL_NAME_SIZE]));
    if (columns == 0) {
        return -1;
    }
    table->columns = columns;
    column_types = (unsigned char *)sql_resize_array(table->column_types, table->column_capacity, capacity, sizeof(unsigned char));
    if (column_types == 0) {
        return -1;
    }
    table->column_types = column_types;
    not_null = (unsigned char *)sql_resize_array(table->not_null, table->column_capacity, capacity, sizeof(unsigned char));
    if (not_null == 0) {
        return -1;
    }
    table->not_null = not_null;
    has_default = (unsigned char *)sql_resize_array(table->has_default, table->column_capacity, capacity, sizeof(unsigned char));
    if (has_default == 0) {
        return -1;
    }
    table->has_default = has_default;
    unique = (unsigned char *)sql_resize_array(table->unique, table->column_capacity, capacity, sizeof(unsigned char));
    if (unique == 0) {
        return -1;
    }
    table->unique = unique;
    primary_key = (unsigned char *)sql_resize_array(table->primary_key, table->column_capacity, capacity, sizeof(unsigned char));
    if (primary_key == 0) {
        return -1;
    }
    table->primary_key = primary_key;
    defaults = (unsigned int *)sql_resize_array(table->defaults, table->column_capacity, capacity, sizeof(unsigned int));
    if (defaults == 0) {
        return -1;
    }
    table->defaults = defaults;
    for (column_index = table->column_capacity; column_index < capacity; ++column_index) {
        table->columns[column_index][0] = '\0';
        table->column_types[column_index] = SQL_TYPE_TEXT;
        table->not_null[column_index] = 0U;
        table->has_default[column_index] = 0U;
        table->unique[column_index] = 0U;
        table->primary_key[column_index] = 0U;
        table->defaults[column_index] = 0U;
    }
    if (table->row_capacity != 0U && sql_resize_table_row_values(table, table->row_capacity, capacity) != 0) {
        return -1;
    }
    table->column_capacity = capacity;
    sql_bind_table_rows(table);
    return 0;
}

static void sql_free_database_storage(SqlDatabase *db) {
    unsigned int table_index;

    if (db == 0) {
        return;
    }
    for (table_index = 0U; table_index < db->table_count; ++table_index) {
        sql_free_table_storage(&db->tables[table_index]);
    }
    sql_free_bytes(db->tables);
    db->tables = 0;
    db->table_capacity = 0U;
    db->table_count = 0U;
}

static void sql_invalidate_runtime_caches(void) {
    unsigned int i;

    for (i = 0U; i < SQL_NUMERIC_CACHE_SLOTS; ++i) {
        sql_numeric_caches[i].valid = 0;
        sql_numeric_caches[i].table = 0;
    }
    for (i = 0U; i < SQL_INDEX_SLOTS; ++i) {
        sql_index_caches[i].valid = 0;
        sql_index_caches[i].table = 0;
    }
}

static void sql_invalidate_table_runtime_caches(const SqlTable *table) {
    unsigned int i;

    for (i = 0U; i < SQL_NUMERIC_CACHE_SLOTS; ++i) {
        if (sql_numeric_caches[i].table == table) {
            sql_numeric_caches[i].valid = 0;
            sql_numeric_caches[i].table = 0;
        }
    }
    for (i = 0U; i < SQL_INDEX_SLOTS; ++i) {
        if (sql_index_caches[i].table == table) {
            sql_index_caches[i].valid = 0;
            sql_index_caches[i].table = 0;
        }
    }
}

static int sql_copy_checked(char *dst, size_t dst_size, const char *src) {
    if (dst == 0 || dst_size == 0U || src == 0 || rt_strlen(src) + 1U > dst_size) {
        return -1;
    }
    rt_copy_string(dst, dst_size, src);
    return 0;
}

static void sql_write_error(const char *message, const char *detail) {
    rt_write_cstr(2, "sql: ");
    rt_write_cstr(2, message);
    if (detail != 0) {
        rt_write_cstr(2, detail);
    }
    rt_write_char(2, '\n');
}

static void sql_write_row_count(unsigned int count) {
    rt_write_uint(1, count);
    rt_write_line(1, " rows");
}

static SqlTable *sql_find_table(SqlDatabase *db, const char *name) {
    unsigned int i;

    for (i = 0U; i < db->table_count; ++i) {
        if (tool_str_equal_ignore_case_ascii(db->tables[i].name, name)) {
            return &db->tables[i];
        }
    }
    return 0;
}

static int sql_find_column(const SqlTable *table, const char *name) {
    unsigned int i;

    for (i = 0U; i < table->column_count; ++i) {
        if (tool_str_equal_ignore_case_ascii(table->columns[i], name)) {
            return (int)i;
        }
    }
    return -1;
}

static int sql_column_seen(char (*columns)[SQL_NAME_SIZE], unsigned int column_count, const char *name) {
    unsigned int i;

    for (i = 0U; i < column_count; ++i) {
        if (tool_str_equal_ignore_case_ascii(columns[i], name)) {
            return 1;
        }
    }
    return 0;
}

static int sql_copy_label(char *dst, size_t dst_size, const char *prefix, const char *suffix) {
    size_t prefix_len = rt_strlen(prefix);
    size_t suffix_len = suffix != 0 ? rt_strlen(suffix) : 0U;

    if (prefix_len + suffix_len + 1U > dst_size) {
        return -1;
    }
    memcpy(dst, prefix, prefix_len);
    if (suffix_len > 0U) {
        memcpy(dst + prefix_len, suffix, suffix_len);
    }
    dst[prefix_len + suffix_len] = '\0';
    return 0;
}

static int sql_split_ref(const char *text, char *table_name, size_t table_size, char *column_name, size_t column_size) {
    size_t i = 0U;
    size_t dot = (size_t)-1;

    while (text[i] != '\0') {
        if (text[i] == '.') {
            if (dot != (size_t)-1) {
                return -1;
            }
            dot = i;
        }
        i += 1U;
    }
    if (dot == (size_t)-1) {
        table_name[0] = '\0';
        return sql_copy_checked(column_name, column_size, text);
    }
    if (dot == 0U || text[dot + 1U] == '\0' || dot + 1U > table_size || i - dot > column_size) {
        return -1;
    }
    memcpy(table_name, text, dot);
    table_name[dot] = '\0';
    rt_copy_string(column_name, column_size, text + dot + 1U);
    return sql_valid_identifier(table_name) && sql_valid_identifier(column_name) ? 0 : -1;
}

static int sql_resolve_column(const SqlSelectQuery *query, const char *text, SqlColumnRef *ref_out) {
    char table_name[SQL_NAME_SIZE];
    char column_name[SQL_NAME_SIZE];
    int found_table = -1;
    int found_column = -1;
    unsigned int i;

    if (sql_split_ref(text, table_name, sizeof(table_name), column_name, sizeof(column_name)) != 0) {
        return -1;
    }
    for (i = 0U; i < query->source_count; ++i) {
        int column;
        if (table_name[0] != '\0' &&
            !tool_str_equal_ignore_case_ascii(query->sources[i].name, table_name) &&
            !tool_str_equal_ignore_case_ascii(query->sources[i].alias, table_name)) {
            continue;
        }
        column = sql_find_column(query->sources[i].table, column_name);
        if (column < 0) {
            continue;
        }
        if (found_column >= 0) {
            return -1;
        }
        found_table = (int)i;
        found_column = column;
    }
    if (found_column < 0) {
        return -1;
    }
    ref_out->table_index = found_table;
    ref_out->column_index = found_column;
    return 0;
}

static int sql_read_column_ref(SqlParser *parser, const SqlSelectQuery *query, SqlColumnRef *ref_out, char *label, size_t label_size) {
    if (sql_next_token(parser) != 0 || parser->token_type != SQL_TOKEN_WORD) {
        return -1;
    }
    if (label != 0 && sql_copy_checked(label, label_size, parser->token) != 0) {
        return -1;
    }
    return sql_resolve_column(query, parser->token, ref_out);
}

static int sql_aggregate_kind_from_name(const char *name);
static const char *sql_aggregate_label(int kind);
static int sql_parse_aggregate_call(SqlParser *parser, SqlSelectQuery *query, int kind, int allow_star, SqlColumnRef *column_out, int *aggregate_index_out);
static int sql_compare_condition_values(const SqlConditionValue *left, const SqlConditionValue *right, const SqlResultRow *row);

static int sql_result_rows_same_group(const SqlResultRow *left, const SqlResultRow *right, const SqlSelectQuery *query) {
    unsigned int i;

    for (i = 0U; i < query->group_count; ++i) {
        int table_index = query->group_by[i].table_index;
        int column_index = query->group_by[i].column_index;
        if (rt_strcmp(sql_row_value(left->rows[table_index], (unsigned int)column_index), sql_row_value(right->rows[table_index], (unsigned int)column_index)) != 0) {
            return 0;
        }
    }
    return 1;
}

static int sql_aggregate_kind_from_name(const char *name) {
    if (tool_str_equal_ignore_case_ascii(name, "count")) {
        return SQL_SELECT_COUNT_COLUMN;
    }
    if (tool_str_equal_ignore_case_ascii(name, "sum")) {
        return SQL_SELECT_SUM;
    }
    if (tool_str_equal_ignore_case_ascii(name, "min")) {
        return SQL_SELECT_MIN;
    }
    if (tool_str_equal_ignore_case_ascii(name, "max")) {
        return SQL_SELECT_MAX;
    }
    if (tool_str_equal_ignore_case_ascii(name, "avg")) {
        return SQL_SELECT_AVG;
    }
    if (tool_str_equal_ignore_case_ascii(name, "total")) {
        return SQL_SELECT_TOTAL;
    }
    if (tool_str_equal_ignore_case_ascii(name, "first")) {
        return SQL_SELECT_FIRST;
    }
    if (tool_str_equal_ignore_case_ascii(name, "last")) {
        return SQL_SELECT_LAST;
    }
    if (tool_str_equal_ignore_case_ascii(name, "group_concat")) {
        return SQL_SELECT_GROUP_CONCAT;
    }
    return -1;
}

static const char *sql_aggregate_label(int kind) {
    switch (kind) {
        case SQL_SELECT_COUNT_ALL:
        case SQL_SELECT_COUNT_COLUMN:
            return "count";
        case SQL_SELECT_SUM:
            return "sum";
        case SQL_SELECT_MIN:
            return "min";
        case SQL_SELECT_MAX:
            return "max";
        case SQL_SELECT_AVG:
            return "avg";
        case SQL_SELECT_TOTAL:
            return "total";
        case SQL_SELECT_FIRST:
            return "first";
        case SQL_SELECT_LAST:
            return "last";
        case SQL_SELECT_GROUP_CONCAT:
            return "group_concat";
        default:
            return "";
    }
}

static int sql_add_aggregate(SqlSelectQuery *query, int kind, const SqlColumnRef *column, int *index_out) {
    SqlAggregate *aggregate;

    if (query->aggregate_count >= SQL_MAX_COLUMNS || index_out == 0 || sql_ensure_select_aggregate_capacity(query, query->aggregate_count + 1U) != 0) {
        return -1;
    }
    aggregate = &query->aggregates[query->aggregate_count];
    aggregate->kind = kind;
    if (column != 0) {
        aggregate->column = *column;
    } else {
        aggregate->column.table_index = -1;
        aggregate->column.column_index = -1;
    }
    if (sql_copy_checked(aggregate->label, sizeof(aggregate->label), sql_aggregate_label(kind)) != 0) {
        return -1;
    }
    *index_out = (int)query->aggregate_count;
    query->aggregate_count += 1U;
    return 0;
}

static int sql_parse_aggregate_call(SqlParser *parser, SqlSelectQuery *query, int kind, int allow_star, SqlColumnRef *column_out, int *aggregate_index_out) {
    SqlColumnRef column;

    if (!sql_try_symbol(parser, '(')) {
        return 0;
    }
    if (allow_star && sql_try_symbol(parser, '*')) {
        if (!sql_expect_symbol(parser, ')')) {
            return -1;
        }
        return sql_add_aggregate(query, SQL_SELECT_COUNT_ALL, 0, aggregate_index_out) == 0 ? 1 : -1;
    }
    if (sql_next_token(parser) != 0 || parser->token_type != SQL_TOKEN_WORD || sql_resolve_column(query, parser->token, &column) != 0 || !sql_expect_symbol(parser, ')')) {
        return -1;
    }
    if (column_out != 0) {
        *column_out = column;
    }
    return sql_add_aggregate(query, kind, &column, aggregate_index_out) == 0 ? 1 : -1;
}

static int sql_compute_aggregate_value(const SqlSelectQuery *query, const SqlResultRow *representative, const SqlResultRow *rows, unsigned int row_count, const SqlAggregate *aggregate, char *buffer, size_t buffer_size) {
    unsigned int i;
    long long sum = 0LL;
    unsigned int value_count = 0U;
    const char *best = 0;
    char joined[SQL_VALUE_SIZE];
    size_t joined_length = 0U;

    joined[0] = '\0';

    if (aggregate->kind == SQL_SELECT_COUNT_ALL) {
        sql_store_uint_text(representative->count, buffer, buffer_size);
        return 0;
    }
    for (i = 0U; i < row_count; ++i) {
        const SqlResultRow *candidate = &rows[i];
        const char *value;
        if (query->group_count > 0U && !sql_result_rows_same_group(representative, candidate, query)) {
            continue;
        }
        value = sql_row_value(candidate->rows[aggregate->column.table_index], (unsigned int)aggregate->column.column_index);
        if (sql_row_value_is_null(candidate->rows[aggregate->column.table_index], (unsigned int)aggregate->column.column_index)) {
            continue;
        }
        if (aggregate->kind == SQL_SELECT_COUNT_COLUMN) {
            if (value[0] != '\0') {
                value_count += 1U;
            }
        } else if (aggregate->kind == SQL_SELECT_SUM || aggregate->kind == SQL_SELECT_AVG || aggregate->kind == SQL_SELECT_TOTAL) {
            long long number;
            if (sql_result_row_numeric_value(candidate, (unsigned int)aggregate->column.table_index, (unsigned int)aggregate->column.column_index, &number) != 0 &&
                sql_parse_decimal_scaled(value, &number, 0) != 0) {
                return -1;
            }
            sum += number;
            value_count += 1U;
        } else if (aggregate->kind == SQL_SELECT_FIRST) {
            if (best == 0) {
                best = value;
            }
        } else if (aggregate->kind == SQL_SELECT_LAST) {
            best = value;
        } else if (aggregate->kind == SQL_SELECT_GROUP_CONCAT) {
            size_t value_length = rt_strlen(value);
            if (value_count > 0U) {
                if (joined_length + 1U >= sizeof(joined)) {
                    return -1;
                }
                joined[joined_length++] = ',';
            }
            if (joined_length + value_length + 1U > sizeof(joined)) {
                return -1;
            }
            memcpy(joined + joined_length, value, value_length + 1U);
            joined_length += value_length;
            value_count += 1U;
        } else if (best == 0 ||
                   (aggregate->kind == SQL_SELECT_MIN && sql_compare_values(value, best) < 0) ||
                   (aggregate->kind == SQL_SELECT_MAX && sql_compare_values(value, best) > 0)) {
            best = value;
        }
    }
    if (aggregate->kind == SQL_SELECT_COUNT_COLUMN) {
        sql_store_uint_text(value_count, buffer, buffer_size);
        return 0;
    }
    if (aggregate->kind == SQL_SELECT_SUM || aggregate->kind == SQL_SELECT_TOTAL) {
        return sql_store_decimal_scaled(sum, buffer, buffer_size);
    }
    if (aggregate->kind == SQL_SELECT_AVG) {
        return sql_store_decimal_scaled(value_count == 0U ? 0LL : sum / (long long)value_count, buffer, buffer_size);
    }
    if (aggregate->kind == SQL_SELECT_GROUP_CONCAT) {
        if (value_count == 0U) {
            return sql_copy_checked(buffer, buffer_size, "NULL");
        }
        return sql_copy_checked(buffer, buffer_size, joined);
    }
    if (aggregate->kind == SQL_SELECT_FIRST || aggregate->kind == SQL_SELECT_LAST) {
        return sql_copy_checked(buffer, buffer_size, best != 0 ? best : "NULL");
    }
    if (aggregate->kind == SQL_SELECT_SUM) {
        sql_store_uint_text((unsigned long long)sum, buffer, buffer_size);
        return 0;
    }
    return sql_copy_checked(buffer, buffer_size, best != 0 ? best : "NULL");
}

static int sql_label_matches_item(const SqlSelectItem *item, const char *label) {
    return tool_str_equal_ignore_case_ascii(item->label, label);
}

static int sql_find_select_label(const SqlSelectQuery *query, const char *label) {
    unsigned int i;

    for (i = 0U; i < query->item_count; ++i) {
        if (sql_label_matches_item(&query->items[i], label)) {
            return (int)i;
        }
    }
    return -1;
}

static int sql_select_item_offset(SqlDatabase *db, const SqlSelectItem *item, const SqlResultRow *row, unsigned int *offset_out) {
    if (item->kind == SQL_SELECT_COLUMN) {
        return sql_store_value(db, sql_row_display_value(row->rows[item->column.table_index], (unsigned int)item->column.column_index), offset_out);
    }
    if (item->kind == SQL_SELECT_ADD || item->kind == SQL_SELECT_SUB) {
        unsigned long long left;
        unsigned long long right;
        unsigned long long result;
        char value[SQL_VALUE_SIZE];

        if (sql_row_value_is_null(row->rows[item->column.table_index], (unsigned int)item->column.column_index) ||
            rt_parse_uint(sql_row_value(row->rows[item->column.table_index], (unsigned int)item->column.column_index), &left) != 0 ||
            rt_parse_uint(item->literal, &right) != 0) {
            return -1;
        }
        if (item->kind == SQL_SELECT_ADD) {
            result = left + right;
        } else {
            if (left < right) {
                return -1;
            }
            result = left - right;
        }
        sql_store_uint_text(result, value, sizeof(value));
        return sql_store_value(db, value, offset_out);
    }
    if (item->kind == SQL_SELECT_CONCAT) {
        char value[SQL_VALUE_SIZE];
        const char *left;
        const char *right;
        size_t left_length;
        size_t right_length;

        if (sql_row_value_is_null(row->rows[item->column.table_index], (unsigned int)item->column.column_index)) {
            return sql_store_value(db, "NULL", offset_out);
        }
        left = sql_row_value(row->rows[item->column.table_index], (unsigned int)item->column.column_index);
        if (item->has_right_column) {
            if (sql_row_value_is_null(row->rows[item->right_column.table_index], (unsigned int)item->right_column.column_index)) {
                return sql_store_value(db, "NULL", offset_out);
            }
            right = sql_row_value(row->rows[item->right_column.table_index], (unsigned int)item->right_column.column_index);
        } else {
            right = item->literal;
        }
        left_length = rt_strlen(left);
        right_length = rt_strlen(right);
        if (left_length + right_length + 1U > sizeof(value)) {
            return -1;
        }
        memcpy(value, left, left_length);
        memcpy(value + left_length, right, right_length + 1U);
        return sql_store_value(db, value, offset_out);
    }
    return sql_store_value(db, sql_value_at(row->aggregates[item->aggregate_index]), offset_out);
}

