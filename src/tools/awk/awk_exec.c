/*
 * awk_exec.c - awk record handling, expression evaluation, printf formatting,
 *              and clause execution engine.
 *
 * Covers: field splitting, expression-to-value conversions, formatted output
 * (printf), statement/clause dispatch, and the stream-processing loop.
 */

#include "awk_impl.h"

static int contains_substring(const char *text, const char *pattern) {
    size_t start = 0;
    size_t end = 0;
    return tool_regex_search(pattern, text, 0, 0, &start, &end);
}

static const char *lookup_variable_value(const AwkState *state, const char *name) {
    size_t i;

    if (state == 0 || name == 0) {
        return "";
    }

    for (i = 0; i < AWK_MAX_VARIABLES; ++i) {
        if (state->variables[i].in_use && rt_strcmp(state->variables[i].name, name) == 0) {
            return state->variables[i].value;
        }
    }

    return "";
}

int awk_assign_variable(AwkState *state, const char *name, const char *value) {
    size_t i;

    if (state == 0 || name == 0 || name[0] == '\0' || value == 0) {
        return -1;
    }

    if (rt_strcmp(name, "FS") == 0) {
        rt_copy_string(state->fs, sizeof(state->fs), value);
        return 0;
    }
    if (rt_strcmp(name, "OFS") == 0) {
        rt_copy_string(state->ofs, sizeof(state->ofs), value);
        return 0;
    }
    if (rt_strcmp(name, "RS") == 0) {
        rt_copy_string(state->rs, sizeof(state->rs), value);
        return 0;
    }
    if (rt_strcmp(name, "ORS") == 0) {
        rt_copy_string(state->ors, sizeof(state->ors), value);
        return 0;
    }

    for (i = 0; i < AWK_MAX_VARIABLES; ++i) {
        if (state->variables[i].in_use && rt_strcmp(state->variables[i].name, name) == 0) {
            rt_copy_string(state->variables[i].value, sizeof(state->variables[i].value), value);
            return 0;
        }
    }

    for (i = 0; i < AWK_MAX_VARIABLES; ++i) {
        if (!state->variables[i].in_use) {
            state->variables[i].in_use = 1;
            rt_copy_string(state->variables[i].name, sizeof(state->variables[i].name), name);
            rt_copy_string(state->variables[i].value, sizeof(state->variables[i].value), value);
            return 0;
        }
    }

    return -1;
}

void awk_set_filename(AwkState *state, const char *filename) {
    if (state == 0) {
        return;
    }

    rt_copy_string(state->filename, sizeof(state->filename), filename != 0 ? filename : "");
    state->fnr = 0ULL;
}

void init_state(AwkState *state) {
    rt_memset(state, 0, sizeof(*state));
    rt_copy_string(state->fs, sizeof(state->fs), " ");
    rt_copy_string(state->ofs, sizeof(state->ofs), " ");
    rt_copy_string(state->rs, sizeof(state->rs), "\n");
    rt_copy_string(state->ors, sizeof(state->ors), "\n");
}

static int compare_values(unsigned long long lhs, AwkCompareOp op, unsigned long long rhs) {
    switch (op) {
        case AWK_COMPARE_EQ:
            return lhs == rhs;
        case AWK_COMPARE_NE:
            return lhs != rhs;
        case AWK_COMPARE_LT:
            return lhs < rhs;
        case AWK_COMPARE_LE:
            return lhs <= rhs;
        case AWK_COMPARE_GT:
            return lhs > rhs;
        case AWK_COMPARE_GE:
            return lhs >= rhs;
        default:
            return 0;
    }
}

static void add_field(AwkRecord *record, const char *start, size_t length) {
    if (record->nf < AWK_MAX_FIELDS) {
        record->field_starts[record->nf] = start;
        record->field_lengths[record->nf] = length;
    }
    record->nf += 1;
}

static size_t awk_decode_codepoint(const char *text, size_t length, size_t start, unsigned int *codepoint_out) {
    size_t index = start;

    if (start >= length) {
        if (codepoint_out != 0) {
            *codepoint_out = 0U;
        }
        return 0U;
    }

    if (rt_utf8_decode(text, length, &index, codepoint_out) != 0 || index <= start) {
        if (codepoint_out != 0) {
            *codepoint_out = (unsigned char)text[start];
        }
        return 1U;
    }

    return index - start;
}

static int awk_unicode_space_at(const char *line, size_t index, size_t *advance_out) {
    size_t length = rt_strlen(line);
    unsigned int codepoint = 0U;
    size_t advance = awk_decode_codepoint(line, length, index, &codepoint);

    if (advance_out != 0) {
        *advance_out = advance;
    }
    return advance > 0U && rt_unicode_is_space(codepoint);
}

static void split_fields_whitespace(AwkRecord *record, const char *line) {
    size_t i = 0;

    while (line[i] != '\0') {
        size_t start;
        size_t length = 0;
        size_t advance = 0U;

        while (line[i] != '\0' && awk_unicode_space_at(line, i, &advance)) {
            i += advance;
        }
        if (line[i] == '\0') {
            break;
        }

        start = i;
        while (line[i] != '\0' && !awk_unicode_space_at(line, i, &advance)) {
            i += advance;
            length += advance;
        }

        add_field(record, line + start, length);
    }
}

static void split_fields_regex(AwkRecord *record, const char *line, const char *separator) {
    size_t field_start = 0;
    size_t search_pos = 0;
    size_t line_length = rt_strlen(line);

    if (line[0] == '\0') {
        return;
    }

    while (1) {
        size_t match_start = 0;
        size_t match_end = 0;

        if (!tool_regex_search(separator, line, 0, search_pos, &match_start, &match_end)) {
            add_field(record, line + field_start, line_length - field_start);
            return;
        }

        add_field(record, line + field_start, match_start - field_start);
        if (match_end == match_start) {
            if (line[match_end] == '\0') {
                return;
            }
            match_end += 1;
        }

        search_pos = match_end;
        field_start = match_end;

        if (line[field_start] == '\0') {
            add_field(record, line + field_start, 0);
            return;
        }
    }
}

void init_record(AwkRecord *record, const char *line, unsigned long long nr, const AwkState *state) {
    rt_memset(record, 0, sizeof(*record));
    record->line = line;
    record->nr = nr;
    record->fnr = state != 0 ? state->fnr : 0ULL;

    if (state == 0 || state->fs[0] == '\0' || (state->fs[0] == ' ' && state->fs[1] == '\0')) {
        split_fields_whitespace(record, line);
    } else {
        split_fields_regex(record, line, state->fs);
    }
}

static void expression_to_string(const AwkExpression *expression,
                                 const AwkRecord *record,
                                 const AwkState *state,
                                 char *buffer,
                                 size_t buffer_size) {
    if (buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    switch (expression->type) {
        case AWK_EXPR_WHOLE_LINE:
            rt_copy_string(buffer, buffer_size, record->line);
            break;
        case AWK_EXPR_FIELD:
            if (expression->number == 0) {
                rt_copy_string(buffer, buffer_size, record->line);
            } else if (expression->number <= record->nf && expression->number <= AWK_MAX_FIELDS) {
                size_t field_index = (size_t)(expression->number - 1);
                size_t copy_length = record->field_lengths[field_index];
                if (copy_length + 1 > buffer_size) {
                    copy_length = buffer_size - 1;
                }
                if (copy_length > 0) {
                    memcpy(buffer, record->field_starts[field_index], copy_length);
                }
                buffer[copy_length] = '\0';
            }
            break;
        case AWK_EXPR_NR:
            rt_unsigned_to_string(record->nr, buffer, buffer_size);
            break;
        case AWK_EXPR_NF:
            rt_unsigned_to_string(record->nf, buffer, buffer_size);
            break;
        case AWK_EXPR_FNR:
            rt_unsigned_to_string(record->fnr, buffer, buffer_size);
            break;
        case AWK_EXPR_FS:
            if (state != 0) {
                rt_copy_string(buffer, buffer_size, state->fs);
            }
            break;
        case AWK_EXPR_OFS:
            if (state != 0) {
                rt_copy_string(buffer, buffer_size, state->ofs);
            }
            break;
        case AWK_EXPR_RS:
            if (state != 0) {
                rt_copy_string(buffer, buffer_size, state->rs);
            }
            break;
        case AWK_EXPR_ORS:
            if (state != 0) {
                rt_copy_string(buffer, buffer_size, state->ors);
            }
            break;
        case AWK_EXPR_FILENAME:
            if (state != 0) {
                rt_copy_string(buffer, buffer_size, state->filename);
            }
            break;
        case AWK_EXPR_VARIABLE:
            if (state != 0) {
                rt_copy_string(buffer, buffer_size, lookup_variable_value(state, expression->text));
            }
            break;
        case AWK_EXPR_STRING:
            rt_copy_string(buffer, buffer_size, expression->text);
            break;
        case AWK_EXPR_NUMBER:
            rt_unsigned_to_string(expression->number, buffer, buffer_size);
            break;
        default:
            break;
    }

    buffer[buffer_size - 1] = '\0';
}

static unsigned long long expression_to_unsigned(const AwkExpression *expression, const AwkRecord *record, const AwkState *state) {
    char buffer[AWK_MAX_TEXT];
    unsigned long long value = 0;

    if (expression->type == AWK_EXPR_NUMBER) {
        return expression->number;
    }
    if (expression->type == AWK_EXPR_NR) {
        return record->nr;
    }
    if (expression->type == AWK_EXPR_NF) {
        return record->nf;
    }
    if (expression->type == AWK_EXPR_FNR) {
        return record->fnr;
    }

    expression_to_string(expression, record, state, buffer, sizeof(buffer));
    if (buffer[0] == '\0' || rt_parse_uint(buffer, &value) != 0) {
        return 0;
    }
    return value;
}

static long long expression_to_signed(const AwkExpression *expression, const AwkRecord *record, const AwkState *state) {
    char buffer[AWK_MAX_TEXT];
    unsigned long long magnitude = 0;

    if (expression->type == AWK_EXPR_NUMBER) {
        return (long long)expression->number;
    }
    if (expression->type == AWK_EXPR_NR) {
        return (long long)record->nr;
    }
    if (expression->type == AWK_EXPR_NF) {
        return (long long)record->nf;
    }
    if (expression->type == AWK_EXPR_FNR) {
        return (long long)record->fnr;
    }

    expression_to_string(expression, record, state, buffer, sizeof(buffer));
    if (buffer[0] == '-') {
        if (rt_parse_uint(buffer + 1, &magnitude) != 0) {
            return 0;
        }
        return -(long long)magnitude;
    }
    if (buffer[0] == '\0' || rt_parse_uint(buffer, &magnitude) != 0) {
        return 0;
    }
    return (long long)magnitude;
}

static int write_expression(const AwkExpression *expression, const AwkRecord *record, const AwkState *state) {
    char buffer[AWK_MAX_TEXT];
    expression_to_string(expression, record, state, buffer, sizeof(buffer));
    return rt_write_cstr(1, buffer);
}

static void format_unsigned_value(unsigned long long value, unsigned int base, int uppercase, char *buffer, size_t buffer_size) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char scratch[64];
    size_t length = 0;
    size_t i;

    if (buffer_size == 0) {
        return;
    }

    if (value == 0ULL) {
        if (buffer_size > 1U) {
            buffer[0] = '0';
            buffer[1] = '\0';
        } else {
            buffer[0] = '\0';
        }
        return;
    }

    while (value != 0ULL && length + 1U < sizeof(scratch)) {
        scratch[length++] = digits[value % base];
        value /= base;
    }

    for (i = 0; i < length && i + 1U < buffer_size; ++i) {
        buffer[i] = scratch[length - 1U - i];
    }
    buffer[i] = '\0';
}

static int write_repeated_char(char ch, int count) {
    while (count > 0) {
        if (rt_write_char(1, ch) != 0) {
            return -1;
        }
        count -= 1;
    }
    return 0;
}

static int write_padded_text(const char *text, size_t length, int width, int left_align, char pad) {
    if (!left_align && width > (int)length) {
        if (write_repeated_char(pad, width - (int)length) != 0) {
            return -1;
        }
    }

    if (length > 0 && rt_write_all(1, text, length) != 0) {
        return -1;
    }

    if (left_align && width > (int)length) {
        if (write_repeated_char(' ', width - (int)length) != 0) {
            return -1;
        }
    }

    return 0;
}

static int write_formatted_number(long long signed_value,
                                  unsigned long long unsigned_value,
                                  int negative,
                                  unsigned int base,
                                  int uppercase,
                                  int width,
                                  int left_align,
                                  char pad,
                                  int precision) {
    char digits[128];
    size_t digit_length;
    int zero_padding = 0;
    int total_length;

    (void)signed_value;
    format_unsigned_value(unsigned_value, base, uppercase, digits, sizeof(digits));
    if (precision == 0 && unsigned_value == 0ULL) {
        digits[0] = '\0';
    }

    digit_length = rt_strlen(digits);
    if (precision >= 0 && (size_t)precision > digit_length) {
        zero_padding = precision - (int)digit_length;
    }
    if (precision >= 0) {
        pad = ' ';
    }

    total_length = (int)digit_length + zero_padding + (negative ? 1 : 0);
    if (!left_align && width > total_length) {
        if (pad == '0' && negative) {
            if (rt_write_char(1, '-') != 0) {
                return -1;
            }
            negative = 0;
        }
        if (write_repeated_char(pad, width - total_length) != 0) {
            return -1;
        }
    }

    if (negative && rt_write_char(1, '-') != 0) {
        return -1;
    }
    if (zero_padding > 0 && write_repeated_char('0', zero_padding) != 0) {
        return -1;
    }
    if (digit_length > 0 && rt_write_all(1, digits, digit_length) != 0) {
        return -1;
    }
    if (left_align && width > total_length) {
        if (write_repeated_char(' ', width - total_length) != 0) {
            return -1;
        }
    }

    return 0;
}

static int execute_printf_statement(const AwkStatement *statement, const AwkRecord *record, const AwkState *state) {
    char format[AWK_MAX_TEXT];
    size_t format_index = 0;
    size_t arg_index = 1;

    if (statement->expression_count == 0) {
        return 0;
    }

    expression_to_string(&statement->expressions[0], record, state, format, sizeof(format));

    while (format[format_index] != '\0') {
        if (format[format_index] != '%') {
            if (rt_write_char(1, format[format_index]) != 0) {
                return -1;
            }
            format_index += 1;
            continue;
        }

        {
            int left_align = 0;
            char pad = ' ';
            int width = 0;
            int precision = -1;
            char specifier;
            char text_value[AWK_MAX_TEXT];
            const AwkExpression *expression = arg_index < statement->expression_count ? &statement->expressions[arg_index] : 0;

            format_index += 1;
            if (format[format_index] == '%') {
                if (rt_write_char(1, '%') != 0) {
                    return -1;
                }
                format_index += 1;
                continue;
            }

            while (format[format_index] == '-' || format[format_index] == '0') {
                if (format[format_index] == '-') {
                    left_align = 1;
                    pad = ' ';
                } else if (!left_align) {
                    pad = '0';
                }
                format_index += 1;
            }

            while (format[format_index] >= '0' && format[format_index] <= '9') {
                width = (width * 10) + (format[format_index] - '0');
                format_index += 1;
            }

            if (format[format_index] == '.') {
                precision = 0;
                format_index += 1;
                while (format[format_index] >= '0' && format[format_index] <= '9') {
                    precision = (precision * 10) + (format[format_index] - '0');
                    format_index += 1;
                }
            }

            specifier = format[format_index];
            if (specifier == '\0') {
                return 0;
            }
            format_index += 1;

            if (specifier != '%' && expression != 0) {
                arg_index += 1;
            }

            switch (specifier) {
                case 's': {
                    size_t length;

                    if (expression != 0) {
                        expression_to_string(expression, record, state, text_value, sizeof(text_value));
                    } else {
                        text_value[0] = '\0';
                    }

                    length = rt_strlen(text_value);
                    if (precision >= 0 && (size_t)precision < length) {
                        length = (size_t)precision;
                    }
                    if (write_padded_text(text_value, length, width, left_align, pad) != 0) {
                        return -1;
                    }
                    break;
                }
                case 'c': {
                    char ch = '\0';
                    if (expression != 0) {
                        expression_to_string(expression, record, state, text_value, sizeof(text_value));
                        ch = text_value[0];
                        if (ch == '\0') {
                            ch = (char)expression_to_unsigned(expression, record, state);
                        }
                    }
                    if (write_padded_text(&ch, 1U, width, left_align, pad) != 0) {
                        return -1;
                    }
                    break;
                }
                case 'd':
                case 'i': {
                    long long value = expression != 0 ? expression_to_signed(expression, record, state) : 0;
                    unsigned long long magnitude = value < 0 ? (unsigned long long)(-value) : (unsigned long long)value;
                    if (write_formatted_number(value, magnitude, value < 0, 10U, 0, width, left_align, pad, precision) != 0) {
                        return -1;
                    }
                    break;
                }
                case 'u': {
                    unsigned long long value = expression != 0 ? expression_to_unsigned(expression, record, state) : 0ULL;
                    if (write_formatted_number((long long)value, value, 0, 10U, 0, width, left_align, pad, precision) != 0) {
                        return -1;
                    }
                    break;
                }
                case 'x':
                case 'X': {
                    unsigned long long value = expression != 0 ? expression_to_unsigned(expression, record, state) : 0ULL;
                    if (write_formatted_number((long long)value, value, 0, 16U, specifier == 'X', width, left_align, pad, precision) != 0) {
                        return -1;
                    }
                    break;
                }
                case 'o': {
                    unsigned long long value = expression != 0 ? expression_to_unsigned(expression, record, state) : 0ULL;
                    if (write_formatted_number((long long)value, value, 0, 8U, 0, width, left_align, pad, precision) != 0) {
                        return -1;
                    }
                    break;
                }
                default:
                    if (rt_write_char(1, '%') != 0 || rt_write_char(1, specifier) != 0) {
                        return -1;
                    }
                    break;
            }
        }
    }

    return 0;
}

static int execute_statement(const AwkStatement *statement, const AwkRecord *record, AwkState *state) {
    size_t i;

    if (statement->kind == AWK_STATEMENT_ASSIGN) {
        char value[AWK_MAX_TEXT];

        if (statement->expression_count == 0) {
            return 0;
        }
        expression_to_string(&statement->expressions[0], record, state, value, sizeof(value));
        if (statement->variable == AWK_VARIABLE_FS ||
            statement->variable == AWK_VARIABLE_OFS ||
            statement->variable == AWK_VARIABLE_RS ||
            statement->variable == AWK_VARIABLE_ORS ||
            statement->variable == AWK_VARIABLE_USER) {
            if (awk_assign_variable(state, statement->variable_name, value) != 0) {
                return -1;
            }
        }
        return 0;
    }

    if (statement->kind == AWK_STATEMENT_PRINTF) {
        return execute_printf_statement(statement, record, state);
    }

    if (statement->expression_count == 0) {
        if (rt_write_cstr(1, record->line) != 0) {
            return -1;
        }
        return rt_write_cstr(1, state->ors);
    }

    for (i = 0; i < statement->expression_count; ++i) {
        if (i > 0 && rt_write_cstr(1, state->ofs) != 0) {
            return -1;
        }
        if (write_expression(&statement->expressions[i], record, state) != 0) {
            return -1;
        }
    }

    return rt_write_cstr(1, state->ors);
}

static int clause_matches(const AwkClause *clause, const AwkRecord *record, const AwkState *state) {
    switch (clause->pattern_type) {
        case AWK_PATTERN_NONE:
            return 1;
        case AWK_PATTERN_REGEX:
            return contains_substring(record->line, clause->pattern_text);
        case AWK_PATTERN_NR:
            return compare_values(record->nr, clause->compare_op, clause->compare_value);
        case AWK_PATTERN_NF:
            return compare_values(record->nf, clause->compare_op, clause->compare_value);
        case AWK_PATTERN_FNR:
            return compare_values(record->fnr, clause->compare_op, clause->compare_value);
        case AWK_PATTERN_EXPR_REGEX: {
            char value[AWK_MAX_TEXT];
            int matched;

            expression_to_string(&clause->match_expression, record, state, value, sizeof(value));
            matched = contains_substring(value, clause->pattern_text);
            return clause->match_negated ? !matched : matched;
        }
        default:
            return 0;
    }
}

int execute_clauses(const AwkProgram *program, AwkClauseKind kind, const AwkRecord *record, AwkState *state) {
    size_t i;
    size_t j;

    for (i = 0; i < program->clause_count; ++i) {
        const AwkClause *clause = &program->clauses[i];

        if (clause->kind != kind) {
            continue;
        }
        if (kind == AWK_CLAUSE_MAIN && !clause_matches(clause, record, state)) {
            continue;
        }

        for (j = 0; j < clause->statement_count; ++j) {
            if (execute_statement(&clause->statements[j], record, state) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int line_ends_with_separator(const char *line, size_t line_length, const char *separator, size_t separator_length) {
    size_t i;

    if (separator == 0 || separator_length == 0U || line_length < separator_length) {
        return 0;
    }

    for (i = 0U; i < separator_length; ++i) {
        if (line[line_length - separator_length + i] != separator[i]) {
            return 0;
        }
    }

    return 1;
}

static int emit_record(const char *line,
                       const AwkProgram *program,
                       AwkState *state,
                       unsigned long long *line_number,
                       unsigned long long *last_nf) {
    AwkRecord record;

    *line_number += 1ULL;
    state->fnr += 1ULL;
    init_record(&record, line, *line_number, state);
    *last_nf = record.nf;

    return execute_clauses(program, AWK_CLAUSE_MAIN, &record, state);
}

int awk_stream(int fd,
               const AwkProgram *program,
               AwkState *state,
               unsigned long long *line_number,
               unsigned long long *last_nf) {
    char chunk[4096];
    char line[AWK_LINE_CAPACITY];
    size_t line_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];
            const char *separator = (state->rs[0] != '\0') ? state->rs : "\n";
            size_t separator_length = rt_strlen(separator);

            if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
                line[line_len] = '\0';
            }

            if (line_ends_with_separator(line, line_len, separator, separator_length)) {
                line_len -= separator_length;
                line[line_len] = '\0';
                if (emit_record(line, program, state, line_number, last_nf) != 0) {
                    return -1;
                }
                line_len = 0;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        if (emit_record(line, program, state, line_number, last_nf) != 0) {
            return -1;
        }
    }

    return 0;
}
