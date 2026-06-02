#include "internal.h"

static const char *sql_value_at(unsigned int offset) {
    if (offset == 0U) {
        return "";
    }
    return sql_database.values != 0 && offset < sql_database.value_capacity ? sql_database.values + offset : "";
}

static int sql_offset_is_null(unsigned int offset) {
    return offset == SQL_NULL_OFFSET;
}

static const char *sql_row_value(const SqlRow *row, unsigned int column) {
    if (row == 0 || row->values == 0 || sql_offset_is_null(row->values[column])) {
        return "";
    }
    return sql_value_at(row->values[column]);
}

static int sql_row_value_is_null(const SqlRow *row, unsigned int column) {
    return row == 0 || row->values == 0 || sql_offset_is_null(row->values[column]);
}

static const char *sql_row_display_value(const SqlRow *row, unsigned int column) {
    return sql_row_value_is_null(row, column) ? "NULL" : sql_row_value(row, column);
}

static int sql_store_value(SqlDatabase *db, const char *value, unsigned int *offset_out) {
    size_t length;
    unsigned int offset;

    if (db == 0 || value == 0 || offset_out == 0) {
        return -1;
    }
    if (value[0] == '\0') {
        *offset_out = 0U;
        return 0;
    }
    length = rt_strlen(value);
    if (length >= SQL_VALUE_SIZE || (size_t)db->value_used + length + 1U > (size_t)SQL_MAX_VALUE_BYTES) {
        return -1;
    }
    if (sql_ensure_value_capacity(db, (unsigned int)((size_t)db->value_used + length + 1U)) != 0) {
        return -1;
    }
    offset = db->value_used;
    memcpy(db->values + offset, value, length + 1U);
    db->value_used += (unsigned int)(length + 1U);
    *offset_out = offset;
    return 0;
}

static int sql_store_row_value(SqlDatabase *db, SqlRow *row, unsigned int column, const char *value) {
    if (row == 0 || row->values == 0) {
        return -1;
    }
    return sql_store_value(db, value, &row->values[column]);
}

static int sql_store_row_null(SqlRow *row, unsigned int column) {
    if (row == 0 || row->values == 0 || column >= SQL_MAX_COLUMNS) {
        return -1;
    }
    row->values[column] = SQL_NULL_OFFSET;
    return 0;
}

static int sql_read_value_or_null(SqlParser *parser, char *buffer, size_t buffer_size, int *is_null_out) {
    if (sql_next_token(parser) != 0 || (parser->token_type != SQL_TOKEN_WORD && parser->token_type != SQL_TOKEN_STRING)) {
        return -1;
    }
    *is_null_out = parser->token_type == SQL_TOKEN_WORD && sql_equal_ignore_case(parser->token, "null");
    if (*is_null_out) {
        buffer[0] = '\0';
        return 0;
    }
    return sql_copy_checked(buffer, buffer_size, parser->token);
}

static int sql_store_row_value_or_null(SqlDatabase *db, SqlRow *row, unsigned int column, const char *value, int is_null) {
    return is_null ? sql_store_row_null(row, column) : sql_store_row_value(db, row, column, value);
}

static int sql_parse_decimal_scaled(const char *text, long long *scaled_out, int *integer_out) {
    size_t i = 0U;
    int negative = 0;
    int saw_digit = 0;
    int saw_dot = 0;
    long long value = 0LL;
    long long fraction_scale = SQL_DECIMAL_SCALE / 10LL;

    if (text == 0 || text[0] == '\0' || scaled_out == 0) {
        return -1;
    }
    if (text[i] == '-' || text[i] == '+') {
        negative = text[i] == '-';
        i += 1U;
    }
    for (; text[i] != '\0'; ++i) {
        char ch = text[i];
        if (ch == '.') {
            if (saw_dot) {
                return -1;
            }
            saw_dot = 1;
            continue;
        }
        if (ch < '0' || ch > '9') {
            return -1;
        }
        saw_digit = 1;
        if (!saw_dot) {
            if (value > (SQL_DECIMAL_LIMIT - ((long long)(ch - '0') * SQL_DECIMAL_SCALE)) / 10LL) {
                return -1;
            }
            value = (value * 10LL) + ((long long)(ch - '0') * SQL_DECIMAL_SCALE);
        } else if (fraction_scale > 0LL) {
            value += (long long)(ch - '0') * fraction_scale;
            fraction_scale /= 10LL;
        }
    }
    if (!saw_digit) {
        return -1;
    }
    *scaled_out = negative ? -value : value;
    if (integer_out != 0) {
        *integer_out = !saw_dot;
    }
    return 0;
}

static int sql_store_decimal_scaled(long long scaled, char *buffer, size_t buffer_size) {
    unsigned long long whole;
    unsigned long long fraction;
    char whole_text[32];
    char fraction_text[8];
    size_t out = 0U;
    int digit;

    if (buffer_size == 0U) {
        return -1;
    }
    if (scaled < 0LL) {
        if (out + 1U >= buffer_size) {
            return -1;
        }
        buffer[out++] = '-';
        scaled = -scaled;
    }
    whole = (unsigned long long)(scaled / SQL_DECIMAL_SCALE);
    fraction = (unsigned long long)(scaled % SQL_DECIMAL_SCALE);
    rt_unsigned_to_string(whole, whole_text, sizeof(whole_text));
    if (out + rt_strlen(whole_text) + 1U > buffer_size) {
        return -1;
    }
    memcpy(buffer + out, whole_text, rt_strlen(whole_text));
    out += rt_strlen(whole_text);
    if (fraction == 0ULL) {
        buffer[out] = '\0';
        return 0;
    }
    if (out + 2U >= buffer_size) {
        return -1;
    }
    buffer[out++] = '.';
    for (digit = 5; digit >= 0; --digit) {
        fraction_text[digit] = (char)('0' + (fraction % 10ULL));
        fraction /= 10ULL;
    }
    fraction_text[6] = '\0';
    while (fraction_text[0] != '\0' && fraction_text[rt_strlen(fraction_text) - 1U] == '0') {
        fraction_text[rt_strlen(fraction_text) - 1U] = '\0';
    }
    if (out + rt_strlen(fraction_text) + 1U > buffer_size) {
        return -1;
    }
    memcpy(buffer + out, fraction_text, rt_strlen(fraction_text) + 1U);
    return 0;
}

static int sql_compare_values(const char *left, const char *right) {
    long long left_number;
    long long right_number;

    if (sql_parse_decimal_scaled(left, &left_number, 0) == 0 && sql_parse_decimal_scaled(right, &right_number, 0) == 0) {
        if (left_number < right_number) {
            return -1;
        }
        if (left_number > right_number) {
            return 1;
        }
        return 0;
    }
    return rt_strcmp(left, right);
}

static int sql_column_type_from_name(const char *name) {
    if (sql_equal_ignore_case(name, "text")) {
        return SQL_TYPE_TEXT;
    }
    if (sql_equal_ignore_case(name, "integer") || sql_equal_ignore_case(name, "int")) {
        return SQL_TYPE_INTEGER;
    }
    if (sql_equal_ignore_case(name, "real")) {
        return SQL_TYPE_REAL;
    }
    return -1;
}

static const char *sql_column_type_name(unsigned int type) {
    if (type == SQL_TYPE_INTEGER) {
        return "INTEGER";
    }
    if (type == SQL_TYPE_REAL) {
        return "REAL";
    }
    return "TEXT";
}

static int sql_validate_typed_value(unsigned int column_type, int is_null, const char *value) {
    long long scaled;
    int integer = 0;

    if (is_null || column_type == SQL_TYPE_TEXT) {
        return 0;
    }
    if (sql_parse_decimal_scaled(value, &scaled, &integer) != 0) {
        return -1;
    }
    if (column_type == SQL_TYPE_INTEGER && !integer) {
        return -1;
    }
    return 0;
}

static void sql_store_uint_text(unsigned long long value, char *buffer, size_t buffer_size) {
    rt_unsigned_to_string(value, buffer, buffer_size);
}

static int sql_prepare_new_row(const SqlTable *table, SqlRow *row) {
    unsigned int column_index;

    for (column_index = 0U; column_index < table->column_count; ++column_index) {
        row->values[column_index] = table->has_default[column_index] ? table->defaults[column_index] : 0U;
        if (table->not_null[column_index] && sql_offset_is_null(row->values[column_index])) {
            return -1;
        }
    }
    return 0;
}

static int sql_validate_row_constraints(const SqlTable *table, const SqlRow *row) {
    unsigned int column_index;

    for (column_index = 0U; column_index < table->column_count; ++column_index) {
        if (table->not_null[column_index] && sql_row_value_is_null(row, column_index)) {
            return -1;
        }
        if (sql_validate_typed_value(table->column_types[column_index], sql_row_value_is_null(row, column_index), sql_row_value(row, column_index)) != 0) {
            return -1;
        }
        if (table->primary_key[column_index] && sql_row_value_is_null(row, column_index)) {
            return -1;
        }
        if (table->unique[column_index] || table->primary_key[column_index]) {
            unsigned int row_index;
            if (sql_row_value_is_null(row, column_index)) {
                continue;
            }
            for (row_index = 0U; row_index < table->row_count; ++row_index) {
                const SqlRow *other = &table->rows[row_index];
                if (other == row || sql_row_value_is_null(other, column_index)) {
                    continue;
                }
                if (rt_strcmp(sql_row_value(other, column_index), sql_row_value(row, column_index)) == 0) {
                    return -1;
                }
            }
        }
    }
    return 0;
}
