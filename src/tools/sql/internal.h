#ifndef SQL_INTERNAL_H
#define SQL_INTERNAL_H

#include "platform.h"
#include "runtime.h"

#ifndef SQL_MAX_TABLES
#define SQL_MAX_TABLES 16U
#endif
#ifndef SQL_MAX_COLUMNS
#define SQL_MAX_COLUMNS 32U
#endif
#ifndef SQL_MAX_ROWS
#define SQL_MAX_ROWS 32768U
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
#define SQL_VALUE_SIZE 128U
#endif
#ifndef SQL_STATEMENT_SIZE
#define SQL_STATEMENT_SIZE 4096U
#endif
#ifndef SQL_FILE_SIZE
#define SQL_FILE_SIZE 16777216U
#endif
#ifndef SQL_VALUE_ARENA_SIZE
#define SQL_VALUE_ARENA_SIZE 16777216U
#endif
#ifndef SQL_IO_BUFFER_SIZE
#define SQL_IO_BUFFER_SIZE 4096U
#endif
#define SQL_IMPORT_LINE_SIZE ((SQL_MAX_COLUMNS * SQL_VALUE_SIZE) + SQL_MAX_COLUMNS + 1U)
#ifndef SQL_SET_CAPACITY
#define SQL_SET_CAPACITY 32U
#endif
#ifndef SQL_MAX_QUERY_TABLES
#define SQL_MAX_QUERY_TABLES 4U
#endif
#ifndef SQL_MAX_RESULT_ROWS
#define SQL_MAX_RESULT_ROWS 32768U
#endif
#ifndef SQL_MAX_GROUP_KEYS
#define SQL_MAX_GROUP_KEYS 8U
#endif
#ifndef SQL_MAX_ORDER_KEYS
#define SQL_MAX_ORDER_KEYS 8U
#endif
#ifndef SQL_MAX_CONDITIONS
#define SQL_MAX_CONDITIONS 8U
#endif
#ifndef SQL_MAX_CONDITION_NODES
#define SQL_MAX_CONDITION_NODES 32U
#endif
#ifndef SQL_MAX_IN_VALUES
#define SQL_MAX_IN_VALUES 32U
#endif
#ifndef SQL_NUMERIC_CACHE_SLOTS
#define SQL_NUMERIC_CACHE_SLOTS 16U
#endif
#ifndef SQL_INDEX_SLOTS
#define SQL_INDEX_SLOTS 16U
#endif
#define SQL_NULL_OFFSET ((unsigned int)0xffffffffU)
#ifndef SQL_PATH_SIZE
#define SQL_PATH_SIZE 1024U
#endif
#define SQL_DECIMAL_SCALE 1000000LL
#define SQL_DECIMAL_LIMIT 900000000000000000LL

typedef struct {
    unsigned int values[SQL_MAX_COLUMNS];
} SqlRow;

typedef struct {
    char name[SQL_NAME_SIZE];
    unsigned int column_count;
    char columns[SQL_MAX_COLUMNS][SQL_NAME_SIZE];
    unsigned char column_types[SQL_MAX_COLUMNS];
    unsigned char not_null[SQL_MAX_COLUMNS];
    unsigned char has_default[SQL_MAX_COLUMNS];
    unsigned char unique[SQL_MAX_COLUMNS];
    unsigned char primary_key[SQL_MAX_COLUMNS];
    unsigned int defaults[SQL_MAX_COLUMNS];
    unsigned int row_count;
    unsigned int row_capacity;
    SqlRow *rows;
} SqlTable;

typedef struct {
    unsigned int table_count;
    unsigned int table_capacity;
    unsigned int value_used;
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
    int aggregate_index;
    SqlColumnRef column;
    char value[SQL_VALUE_SIZE];
} SqlConditionValue;

typedef struct {
    int present;
    int negated;
    int operator_kind;
    SqlConditionValue left;
    SqlConditionValue right;
    SqlConditionValue values[SQL_MAX_IN_VALUES];
    unsigned int value_count;
} SqlCondition;

typedef struct {
    int kind;
    int left;
    int right;
    SqlCondition condition;
} SqlConditionNode;

typedef struct {
    SqlConditionNode nodes[SQL_MAX_CONDITION_NODES];
    unsigned int count;
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
    const SqlRow *rows[SQL_MAX_QUERY_TABLES];
    unsigned int values[SQL_MAX_COLUMNS];
    unsigned int aggregates[SQL_MAX_COLUMNS];
    unsigned int count;
} SqlResultRow;

typedef struct {
    SqlQuerySource sources[SQL_MAX_QUERY_TABLES];
    unsigned int source_count;
    SqlSelectItem items[SQL_MAX_COLUMNS];
    unsigned int item_count;
    int select_all;
    SqlCondition joins[SQL_MAX_QUERY_TABLES - 1U];
    int join_types[SQL_MAX_QUERY_TABLES - 1U];
    unsigned int join_count;
    SqlConditionList where;
    SqlColumnRef group_by[SQL_MAX_GROUP_KEYS];
    unsigned int group_count;
    SqlConditionList having;
    SqlOrderKey order_by[SQL_MAX_ORDER_KEYS];
    unsigned int order_count;
    int has_limit;
    unsigned int limit;
    int has_offset;
    unsigned int offset;
    SqlAggregate aggregates[SQL_MAX_COLUMNS];
    unsigned int aggregate_count;
    int distinct;
} SqlSelectQuery;

typedef struct {
    int fd;
    char buffer[SQL_IO_BUFFER_SIZE];
    size_t pos;
    size_t used;
    int eof;
} SqlLineReader;

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
