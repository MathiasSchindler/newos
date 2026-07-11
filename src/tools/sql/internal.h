#ifndef SQL_INTERNAL_H
#define SQL_INTERNAL_H

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define SQL_COLLECTION_MAX ((unsigned int)-1)
#ifndef SQL_INITIAL_COLUMN_CAPACITY
#define SQL_INITIAL_COLUMN_CAPACITY 4U
#endif
#ifndef SQL_INITIAL_TABLE_CAPACITY
#define SQL_INITIAL_TABLE_CAPACITY 4U
#endif
#ifndef SQL_INITIAL_ROW_CAPACITY
#define SQL_INITIAL_ROW_CAPACITY 16U
#endif
#ifndef SQL_NAME_SIZE
#define SQL_NAME_SIZE 32U
#endif
#ifndef SQL_VALUE_SIZE
#define SQL_VALUE_SIZE 512U
#endif
#ifndef SQL_INITIAL_TEXT_CAPACITY
#define SQL_INITIAL_TEXT_CAPACITY 4096U
#endif
#ifndef SQL_MAX_STATEMENT_BYTES
#define SQL_MAX_STATEMENT_BYTES 1048576U
#endif
#ifndef SQL_MAX_IMPORT_LINE_BYTES
#define SQL_MAX_IMPORT_LINE_BYTES 1048576U
#endif
#ifndef SQL_INITIAL_VALUE_CAPACITY
#define SQL_INITIAL_VALUE_CAPACITY 4096U
#endif
#ifndef SQL_MAX_VALUE_BYTES
#define SQL_MAX_VALUE_BYTES (SQL_NULL_OFFSET - 1U)
#endif
#ifndef SQL_IO_BUFFER_SIZE
#define SQL_IO_BUFFER_SIZE 4096U
#endif
#ifndef SQL_INITIAL_RESULT_CAPACITY
#define SQL_INITIAL_RESULT_CAPACITY 256U
#endif
#ifndef SQL_NUMERIC_CACHE_SLOTS
#define SQL_NUMERIC_CACHE_SLOTS 16U
#endif
#ifndef SQL_INDEX_SLOTS
#define SQL_INDEX_SLOTS 16U
#endif
#define SQL_NULL_OFFSET ((unsigned int)0xffffffffU)
#define SQL_ROW_INDEX_NONE ((unsigned int)0xffffffffU)
#ifndef SQL_PATH_SIZE
#define SQL_PATH_SIZE 1024U
#endif
#define SQL_DECIMAL_SCALE 1000000LL
#define SQL_DECIMAL_LIMIT 900000000000000000LL

typedef struct {
    unsigned int *values;
} SqlRow;

typedef struct {
    char name[SQL_NAME_SIZE];
    unsigned int column_count;
    unsigned int column_capacity;
    char (*columns)[SQL_NAME_SIZE];
    unsigned char *column_types;
    unsigned char *not_null;
    unsigned char *has_default;
    unsigned char *unique;
    unsigned char *primary_key;
    unsigned int *defaults;
    unsigned int row_count;
    unsigned int row_capacity;
    SqlRow *rows;
    unsigned int *row_values;
} SqlTable;

typedef struct {
    unsigned int table_count;
    unsigned int table_capacity;
    unsigned int value_used;
    unsigned int value_capacity;
    char *values;
    SqlTable *tables;
} SqlDatabase;

typedef struct {
    const SqlTable *table;
    unsigned int column;
    unsigned int row_capacity;
    long long *values;
    unsigned char *present;
    int valid;
} SqlNumericCache;

typedef struct {
    const SqlTable *table;
    unsigned int column;
    unsigned int row_capacity;
    unsigned int *row_ids;
    unsigned int row_count;
    int valid;
} SqlIndexCache;

#ifndef SQL_UNITY_BUILD
#ifndef SQL_CORE_FRAGMENT
extern SqlDatabase sql_database;
extern SqlNumericCache sql_numeric_caches[SQL_NUMERIC_CACHE_SLOTS];
extern SqlIndexCache sql_index_caches[SQL_INDEX_SLOTS];
#endif
#endif

typedef struct {
    const char *input;
    size_t pos;
    char token[SQL_VALUE_SIZE];
    int token_type;
} SqlParser;

typedef struct {
    int column;
    int kind;
    int source_column;
    int operator_kind;
    int is_null;
    char value[SQL_VALUE_SIZE];
} SqlAssignment;

typedef struct {
    SqlTable *table;
    char name[SQL_NAME_SIZE];
    char alias[SQL_NAME_SIZE];
} SqlQuerySource;

typedef struct {
    int table_index;
    int column_index;
} SqlColumnRef;

typedef struct {
    int is_column;
    int is_null;
    int is_count;
    int is_aggregate;
    int has_numeric;
    int aggregate_index;
    long long numeric_value;
    SqlColumnRef column;
    char value[SQL_VALUE_SIZE];
} SqlConditionValue;

typedef struct {
    int present;
    int negated;
    int operator_kind;
    SqlConditionValue left;
    SqlConditionValue right;
    SqlConditionValue *values;
    unsigned int value_count;
    unsigned int value_capacity;
} SqlCondition;

typedef struct {
    int kind;
    int left;
    int right;
    SqlCondition condition;
} SqlConditionNode;

typedef struct {
    SqlConditionNode *nodes;
    unsigned int count;
    unsigned int capacity;
    int root;
} SqlConditionList;

typedef struct {
    int kind;
    SqlColumnRef column;
    SqlColumnRef right_column;
    int has_right_column;
    int aggregate_index;
    char literal[SQL_VALUE_SIZE];
    char label[SQL_VALUE_SIZE];
} SqlSelectItem;

typedef struct {
    int kind;
    SqlColumnRef column;
    char label[SQL_VALUE_SIZE];
} SqlAggregate;

typedef struct {
    SqlColumnRef column;
    char label[SQL_VALUE_SIZE];
    int desc;
} SqlOrderKey;

typedef struct {
    const SqlTable **tables;
    const SqlRow **rows;
    unsigned int *row_indices;
    unsigned int *values;
    unsigned int *aggregates;
    unsigned int count;
} SqlResultRow;

typedef struct {
    SqlResultRow *rows;
    const SqlTable **tables;
    const SqlRow **row_refs;
    unsigned int *row_indices;
    unsigned int *values;
    unsigned int *aggregates;
    unsigned int count;
    unsigned int capacity;
    unsigned int value_slots;
    unsigned int aggregate_slots;
    unsigned int source_slots;
} SqlResultBuffer;

typedef struct {
    char *data;
    unsigned int capacity;
} SqlTextBuffer;

#ifndef SQL_UNITY_BUILD
#ifndef SQL_CORE_FRAGMENT
extern SqlTextBuffer sql_import_line;
#endif
#endif

typedef struct {
    SqlQuerySource *sources;
    unsigned int source_count;
    unsigned int source_capacity;
    SqlSelectItem *items;
    unsigned int item_count;
    unsigned int item_capacity;
    int select_all;
    SqlCondition *joins;
    int *join_types;
    unsigned int join_count;
    SqlConditionList where;
    SqlColumnRef *group_by;
    unsigned int group_count;
    unsigned int group_capacity;
    SqlConditionList having;
    SqlOrderKey *order_by;
    unsigned int order_count;
    unsigned int order_capacity;
    int has_limit;
    unsigned int limit;
    int has_offset;
    unsigned int offset;
    SqlAggregate *aggregates;
    unsigned int aggregate_count;
    unsigned int aggregate_capacity;
    int collection_limit_enabled;
    unsigned int collection_limit;
    int distinct;
} SqlSelectQuery;

typedef struct {
    char (*raw_items)[SQL_VALUE_SIZE];
    char (*raw_expr_right)[SQL_VALUE_SIZE];
    int *raw_kinds;
    char (*raw_labels)[SQL_VALUE_SIZE];
    unsigned int capacity;
} SqlSelectScratch;

typedef struct {
    int fd;
    char buffer[SQL_IO_BUFFER_SIZE];
    size_t pos;
    size_t used;
    int eof;
} SqlLineReader;

typedef ToolOutputBuffer SqlDatabaseWriter;

static int sql_copy_checked(char *dst, size_t dst_size, const char *src);
static int sql_valid_identifier(const char *text);
static int sql_identifier_char(char ch);
static void sql_skip_space(SqlParser *parser);
static void sql_parser_init(SqlParser *parser, const char *input);
static int sql_next_token(SqlParser *parser);
static int sql_expect_symbol(SqlParser *parser, char symbol);
static int sql_try_symbol(SqlParser *parser, char symbol);
static int sql_expect_word(SqlParser *parser, const char *word);
static int sql_try_word(SqlParser *parser, const char *word);
static int sql_at_end(SqlParser *parser);
static int sql_read_identifier(SqlParser *parser, char *buffer, size_t buffer_size);
static int sql_read_value(SqlParser *parser, char *buffer, size_t buffer_size);
static int sql_read_value_or_null(SqlParser *parser, char *buffer, size_t buffer_size, int *is_null_out);
static const char *sql_value_at(unsigned int offset);
static int sql_offset_is_null(unsigned int offset);
static const char *sql_row_value(const SqlRow *row, unsigned int column);
static int sql_row_value_is_null(const SqlRow *row, unsigned int column);
static const char *sql_row_display_value(const SqlRow *row, unsigned int column);
static int sql_store_value_len(SqlDatabase *db, const char *value, size_t length, unsigned int *offset_out);
static int sql_store_value(SqlDatabase *db, const char *value, unsigned int *offset_out);
static int sql_store_row_null(SqlRow *row, unsigned int column);
static int sql_store_row_value_len(SqlDatabase *db, SqlRow *row, unsigned int column, const char *value, size_t length);
static int sql_store_row_value(SqlDatabase *db, SqlRow *row, unsigned int column, const char *value);
static int sql_store_row_value_or_null(SqlDatabase *db, SqlRow *row, unsigned int column, const char *value, int is_null);
static int sql_prepare_new_row(const SqlTable *table, SqlRow *row);
static int sql_validate_typed_value(unsigned int column_type, int is_null, const char *value);
static int sql_validate_row_constraints(const SqlTable *table, const SqlRow *row);
static int sql_parse_decimal_scaled(const char *text, long long *scaled_out, int *integer_out);
static int sql_store_decimal_scaled(long long scaled, char *buffer, size_t buffer_size);
static int sql_compare_values(const char *left, const char *right);
static void sql_store_uint_text(unsigned long long value, char *buffer, size_t buffer_size);
static int sql_row_numeric_value(const SqlRow *row, unsigned int column, long long *value_out);
static int sql_result_row_numeric_value(const SqlResultRow *row, unsigned int table_index, unsigned int column, long long *value_out);
static int sql_table_row_numeric_value(const SqlTable *table, unsigned int row_index, unsigned int column, long long *value_out);
static int sql_multiply_size(size_t left, size_t right, size_t *out);
static int sql_next_capacity(unsigned int current, unsigned int needed, unsigned int max, unsigned int initial, unsigned int *capacity_out);
static void sql_free_bytes(void *ptr);
static void *sql_resize_bytes(void *ptr, size_t old_size, size_t new_size);
static void *sql_resize_array(void *ptr, unsigned int old_count, unsigned int new_count, size_t item_size);
static int sql_ensure_value_capacity(SqlDatabase *db, unsigned int needed);
static int sql_ensure_table_capacity(SqlDatabase *db, unsigned int needed);
static int sql_ensure_column_capacity(SqlTable *table, unsigned int needed);
static int sql_ensure_row_capacity(SqlTable *table, unsigned int needed);
static int sql_ensure_result_capacity(SqlResultBuffer *buffer, unsigned int needed);
static int sql_ensure_select_item_capacity(SqlSelectQuery *query, unsigned int needed);
static int sql_ensure_select_aggregate_capacity(SqlSelectQuery *query, unsigned int needed);
static int sql_ensure_select_scratch_capacity(SqlSelectScratch *scratch, unsigned int needed);
static int sql_ensure_text_capacity(SqlTextBuffer *buffer, unsigned int needed, unsigned int max);
static void sql_free_text_buffer(SqlTextBuffer *buffer);
static void sql_free_select_query(SqlSelectQuery *query);
static void sql_free_select_scratch(SqlSelectScratch *scratch);
static void sql_copy_row_values(const SqlTable *table, SqlRow *dst, const SqlRow *src);
static void sql_free_table_rows(SqlTable *table);
static void sql_free_table_columns(SqlTable *table);
static void sql_free_table_storage(SqlTable *table);
static void sql_free_database_storage(SqlDatabase *db);
static void sql_clear_table_metadata(SqlTable *table);
static void sql_clear_database(SqlDatabase *db);
static SqlTable *sql_find_table(SqlDatabase *db, const char *name);
static int sql_find_column(const SqlTable *table, const char *name);
static const char *sql_column_type_name(unsigned int type);
static int sql_column_type_from_name(const char *name);
static void sql_invalidate_runtime_caches(void);
static void sql_invalidate_table_runtime_caches(const SqlTable *table);
static void sql_line_reader_init(SqlLineReader *reader, int fd);
static int sql_line_reader_next(SqlLineReader *reader, SqlTextBuffer *line, int *read_out);
static int sql_line_next(char **cursor_io, char **line_out);
static char *sql_read_text_file(const char *path);
static int sql_load_database(const char *path, SqlDatabase *db);
static int sql_save_database(const char *path, const SqlDatabase *db);
static int sql_write_database_file(const char *path, const SqlDatabase *db);
static int sql_temp_save_path(const char *path, char *buffer, size_t buffer_size);
static int sql_write_name(SqlDatabaseWriter *writer, const char *name);
static char *sql_next_import_field(char **cursor_io, int csv, int *ok_out);
static char *sql_next_csv_field(char **cursor_io, int *ok_out);
static char *sql_next_delimited_field(char **cursor_io, char delimiter);
static int sql_write_csv_value(int fd, const char *value);
static int sql_csv_value_needs_quotes(const char *value);
static int sql_value_is_tsv_safe(const char *value);
static int sql_execute_create(SqlDatabase *db, SqlParser *parser);
static int sql_execute_insert(SqlDatabase *db, SqlParser *parser);
static int sql_execute_select(SqlDatabase *db, SqlParser *parser);
static int sql_execute_update(SqlDatabase *db, SqlParser *parser);
static int sql_execute_delete(SqlDatabase *db, SqlParser *parser);
static int sql_execute_drop(SqlDatabase *db, SqlParser *parser);
static int sql_execute_schema(SqlDatabase *db, SqlParser *parser);
static int sql_execute_alter(SqlDatabase *db, SqlParser *parser);
static int sql_execute_import(SqlDatabase *db, SqlParser *parser);
static int sql_execute_export(SqlDatabase *db, SqlParser *parser);
static int sql_execute_statement(SqlDatabase *db, const char *statement);
static int sql_execute_script(SqlDatabase *db, const char *script, int *changed_out);
static int sql_append_arg(SqlTextBuffer *buffer, unsigned int *used_io, const char *text, int add_space);
static int sql_read_stdin(SqlTextBuffer *buffer, unsigned int *used_out);
static int sql_parse_row_condition_list(SqlParser *parser, const SqlTable *table, SqlConditionList *list);
static int sql_parse_row_condition_expr(SqlParser *parser, const SqlTable *table, SqlConditionList *list, int *node_out);
static int sql_parse_row_condition_and(SqlParser *parser, const SqlTable *table, SqlConditionList *list, int *node_out);
static int sql_parse_row_condition_primary(SqlParser *parser, const SqlTable *table, SqlConditionList *list, int *node_out);
static int sql_parse_row_condition_leaf(SqlParser *parser, const SqlTable *table, SqlConditionList *list, int *node_out);
static int sql_read_condition_literal(SqlParser *parser, SqlConditionValue *value);
static int sql_parse_where(SqlParser *parser, const SqlTable *table, SqlConditionList *where_out);
static int sql_row_condition_list_matches(const SqlTable *table, unsigned int row_index, const SqlConditionList *where);
static int sql_parse_condition_operator_current(SqlParser *parser, int *operator_out);
static int sql_parse_condition_operator(SqlParser *parser, int *operator_out);
static int sql_set_condition_literal(SqlConditionValue *value, const char *text, int is_null);
static int sql_add_condition_leaf(SqlConditionList *list, const SqlCondition *condition);
static int sql_add_condition_node(SqlConditionList *list, int kind, int left, int right);
static const char *sql_condition_value_text(const SqlConditionValue *value, const SqlResultRow *row);
static int sql_compare_condition_values(const SqlConditionValue *left, const SqlConditionValue *right, const SqlResultRow *row);
static int sql_condition_value_numeric(const SqlConditionValue *value, const SqlResultRow *row, long long *number_out);
static int sql_bound_condition_value_text(const SqlConditionValue *value, const SqlResultRow *row, const char **text_out);
static int sql_condition_value_is_null(const SqlConditionValue *value, const SqlResultRow *row);
static int sql_condition_matches(const SqlCondition *condition, const SqlResultRow *row);
static int sql_condition_list_matches(const SqlConditionList *list, const SqlResultRow *row);
static int sql_condition_node_matches(const SqlConditionList *list, int node_index, const SqlResultRow *row);
static int sql_condition_list_uses_aggregate(const SqlConditionList *list);
static int sql_condition_node_uses_aggregate(const SqlConditionList *list, int node_index);
static int sql_condition_index_lookup(const SqlCondition *condition, unsigned int source_index, const SqlResultRow *row, unsigned int *column_out, const char **value_out);
static int sql_condition_list_index_lookup(const SqlConditionList *list, unsigned int source_index, const SqlResultRow *row, unsigned int *column_out, const char **value_out);
static int sql_condition_node_index_lookup(const SqlConditionList *list, int node_index, unsigned int source_index, const SqlResultRow *row, unsigned int *column_out, const char **value_out);
static int sql_resolve_column(const SqlSelectQuery *query, const char *text, SqlColumnRef *ref_out);
static int sql_parse_select_condition_expr(SqlParser *parser, SqlSelectQuery *query, SqlConditionList *list, int *node_out);
static int sql_parse_select_condition_and(SqlParser *parser, SqlSelectQuery *query, SqlConditionList *list, int *node_out);
static int sql_parse_select_condition_primary(SqlParser *parser, SqlSelectQuery *query, SqlConditionList *list, int *node_out);
static int sql_parse_select_condition_leaf(SqlParser *parser, SqlSelectQuery *query, SqlConditionList *list, int *node_out);
static int sql_parse_select_condition_list(SqlParser *parser, SqlSelectQuery *query, SqlConditionList *list);
static int sql_parse_condition(SqlParser *parser, SqlSelectQuery *query, SqlCondition *condition);
static int sql_parse_aggregate_call(SqlParser *parser, SqlSelectQuery *query, int kind, int allow_star, SqlColumnRef *column_out, int *aggregate_index_out);
static int sql_aggregate_kind_from_name(const char *name);
static const char *sql_aggregate_label(int kind);
static int sql_add_aggregate(SqlSelectQuery *query, int kind, const SqlColumnRef *column, int *index_out);
static int sql_copy_label(char *dst, size_t dst_size, const char *prefix, const char *suffix);
static int sql_read_column_ref(SqlParser *parser, const SqlSelectQuery *query, SqlColumnRef *ref_out, char *label, size_t label_size);
static int sql_read_optional_source_alias(SqlParser *parser, char *alias, size_t alias_size);
static int sql_add_query_source(SqlDatabase *db, SqlSelectQuery *query, const char *name, const char *alias);
static int sql_select_tail_keyword(const char *word);
static int sql_parse_select_aggregate(SqlParser *parser, int kind, SqlSelectItem *item, char *raw_argument, size_t raw_argument_size);
static int sql_parse_select_list(SqlParser *parser, SqlSelectQuery *query, SqlSelectScratch *scratch);
static int sql_resolve_select_items(SqlSelectQuery *query, SqlSelectScratch *scratch);
static int sql_parse_select_tail(SqlDatabase *db, SqlParser *parser, SqlSelectQuery *query, SqlSelectScratch *scratch);
static int sql_find_select_label(const SqlSelectQuery *query, const char *label);
static int sql_label_matches_item(const SqlSelectItem *item, const char *label);
static int sql_find_row_location(const SqlRow *row, const SqlTable **table_out, unsigned int *row_index_out);
static SqlNumericCache *sql_numeric_cache_for_column(const SqlTable *table, unsigned int column);
static SqlIndexCache *sql_index_cache_for_column(const SqlTable *table, unsigned int column);
static int sql_compare_table_row_value_to_text(const SqlTable *table, unsigned int row_index, unsigned int column, const char *text);
static int sql_compare_table_rows_by_column(const SqlTable *table, unsigned int left_row, unsigned int right_row, unsigned int column);
static void sql_sort_index_cache(SqlIndexCache *cache);
static void sql_sift_index_heap(SqlIndexCache *cache, unsigned int start, unsigned int end);
static void sql_swap_uint(unsigned int *left, unsigned int *right);
static void sql_index_equal_range(const SqlIndexCache *index, const char *value, unsigned int *start_out, unsigned int *end_out);
static int sql_select_index_lookup(const SqlSelectQuery *query, unsigned int depth, const SqlResultRow *current, unsigned int *column_out, const char **value_out);
static int sql_collect_select_rows(const SqlSelectQuery *query, unsigned int depth, SqlResultRow *current, SqlResultBuffer *result);
static int sql_group_select_rows(const SqlSelectQuery *query, const SqlResultRow *rows, unsigned int row_count, SqlResultBuffer *groups);
static void sql_init_result_buffer(SqlResultBuffer *buffer, unsigned int value_slots, unsigned int aggregate_slots, unsigned int source_slots);
static void sql_free_result_buffer(SqlResultBuffer *buffer);
static void sql_copy_result_row(const SqlSelectQuery *query, SqlResultRow *dst, const SqlResultRow *src);
static void sql_set_result_buffer_row(const SqlSelectQuery *query, SqlResultBuffer *buffer, unsigned int index, const SqlResultRow *src);
static int sql_query_uses_aggregate(const SqlSelectQuery *query);
static int sql_result_rows_same_group(const SqlResultRow *left, const SqlResultRow *right, const SqlSelectQuery *query);
static int sql_compute_aggregate_value(const SqlSelectQuery *query, const SqlResultRow *representative, const SqlResultRow *rows, unsigned int row_count, const SqlAggregate *aggregate, char *buffer, size_t buffer_size);
static int sql_compute_group_aggregates(SqlDatabase *db, const SqlSelectQuery *query, const SqlResultRow *all_rows, unsigned int all_row_count, SqlResultRow *groups, unsigned int group_count);
static int sql_select_item_offset(SqlDatabase *db, const SqlSelectItem *item, const SqlResultRow *row, unsigned int *offset_out);
static int sql_project_select_rows(SqlDatabase *db, const SqlSelectQuery *query, SqlResultRow *rows, unsigned int *row_count_io);
static int sql_projected_rows_equal(const SqlSelectQuery *query, const SqlResultRow *left, const SqlResultRow *right);
static void sql_distinct_select_rows(const SqlSelectQuery *query, SqlResultRow *rows, unsigned int *row_count_io);
static const char *sql_order_value(const SqlSelectQuery *query, const SqlOrderKey *key, const SqlResultRow *row);
static int sql_compare_order_rows(const SqlSelectQuery *query, const SqlResultRow *left, const SqlResultRow *right);
static void sql_swap_result_rows(SqlResultRow *left, SqlResultRow *right);
static void sql_sift_order_heap(const SqlSelectQuery *query, SqlResultRow *rows, unsigned int start, unsigned int end);
static void sql_trim_order_limit_rows(const SqlSelectQuery *query, SqlResultRow *rows, unsigned int *row_count_io);
static void sql_sort_select_rows(const SqlSelectQuery *query, SqlResultRow *rows, unsigned int row_count);
static void sql_write_select_rows(const SqlSelectQuery *query, const SqlResultRow *rows, unsigned int row_count);
static int sql_parse_assignment(SqlParser *parser, const SqlTable *table, SqlAssignment *assignment);
static int sql_apply_assignment(SqlDatabase *db, SqlRow *row, const SqlAssignment *assignment);
static int sql_column_seen(char (*columns)[SQL_NAME_SIZE], unsigned int column_count, const char *name);
static int sql_split_ref(const char *text, char *table_name, size_t table_size, char *column_name, size_t column_size);
static void sql_write_error(const char *message, const char *detail);
static void sql_write_row_count(unsigned int count);
static void sql_usage(const char *program_name);

enum {
    SQL_SELECT_COLUMN = 0,
    SQL_SELECT_COUNT_ALL = 1,
    SQL_SELECT_COUNT_COLUMN = 2,
    SQL_SELECT_SUM = 3,
    SQL_SELECT_MIN = 4,
    SQL_SELECT_MAX = 5,
    SQL_SELECT_AVG = 6,
    SQL_SELECT_ADD = 7,
    SQL_SELECT_SUB = 8,
    SQL_SELECT_CONCAT = 9,
    SQL_SELECT_TOTAL = 10,
    SQL_SELECT_FIRST = 11,
    SQL_SELECT_LAST = 12,
    SQL_SELECT_GROUP_CONCAT = 13
};

enum {
    SQL_TYPE_TEXT = 0,
    SQL_TYPE_INTEGER = 1,
    SQL_TYPE_REAL = 2
};

enum {
    SQL_JOIN_INNER = 0,
    SQL_JOIN_LEFT = 1,
    SQL_JOIN_FULL = 2,
    SQL_JOIN_RIGHT = 3
};

enum {
    SQL_CONDITION_EQ = 0,
    SQL_CONDITION_NE = 1,
    SQL_CONDITION_LT = 2,
    SQL_CONDITION_LE = 3,
    SQL_CONDITION_GT = 4,
    SQL_CONDITION_GE = 5,
    SQL_CONDITION_LIKE = 6,
    SQL_CONDITION_IN = 7,
    SQL_CONDITION_EMPTY = 8,
    SQL_CONDITION_NULL = 9
};

enum {
    SQL_ASSIGN_LITERAL = 0,
    SQL_ASSIGN_ADD = 1,
    SQL_ASSIGN_SUB = 2
};

enum {
    SQL_CONNECT_AND = 0,
    SQL_CONNECT_OR = 1,
    SQL_CONNECT_NOT = 2,
    SQL_CONNECT_LEAF = 3
};

enum {
    SQL_TOKEN_END = 0,
    SQL_TOKEN_WORD = 1,
    SQL_TOKEN_STRING = 2,
    SQL_TOKEN_SYMBOL = 3
};

#endif
