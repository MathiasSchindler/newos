#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define AWK_LINE_CAPACITY 8192
#define AWK_MAX_CLAUSES 16
#define AWK_MAX_STATEMENTS 16
#define AWK_MAX_EXPRESSIONS 16
#define AWK_MAX_TEXT 256
#define AWK_MAX_FIELDS 256

typedef enum {
    AWK_CLAUSE_MAIN = 0,
    AWK_CLAUSE_BEGIN = 1,
    AWK_CLAUSE_END = 2
} AwkClauseKind;

typedef enum {
    AWK_PATTERN_NONE = 0,
    AWK_PATTERN_REGEX = 1,
    AWK_PATTERN_NR = 2,
    AWK_PATTERN_NF = 3,
    AWK_PATTERN_EXPR_REGEX = 4
} AwkPatternType;

typedef enum {
    AWK_COMPARE_EQ = 0,
    AWK_COMPARE_NE = 1,
    AWK_COMPARE_LT = 2,
    AWK_COMPARE_LE = 3,
    AWK_COMPARE_GT = 4,
    AWK_COMPARE_GE = 5
} AwkCompareOp;

typedef enum {
    AWK_EXPR_WHOLE_LINE = 0,
    AWK_EXPR_FIELD = 1,
    AWK_EXPR_NR = 2,
    AWK_EXPR_NF = 3,
    AWK_EXPR_FS = 4,
    AWK_EXPR_OFS = 5,
    AWK_EXPR_STRING = 6,
    AWK_EXPR_NUMBER = 7
} AwkExprType;

typedef enum {
    AWK_STATEMENT_PRINT = 0,
    AWK_STATEMENT_PRINTF = 1,
    AWK_STATEMENT_ASSIGN = 2
} AwkStatementKind;

typedef enum {
    AWK_VARIABLE_NONE = 0,
    AWK_VARIABLE_FS = 1,
    AWK_VARIABLE_OFS = 2
} AwkVariableName;

typedef struct {
    AwkExprType type;
    unsigned long long number;
    char text[AWK_MAX_TEXT];
} AwkExpression;

typedef struct {
    size_t expression_count;
    AwkExpression expressions[AWK_MAX_EXPRESSIONS];
    AwkStatementKind kind;
    AwkVariableName variable;
} AwkStatement;

typedef struct {
    AwkClauseKind kind;
    AwkPatternType pattern_type;
    AwkCompareOp compare_op;
    unsigned long long compare_value;
    int match_negated;
    char pattern_text[AWK_MAX_TEXT];
    AwkExpression match_expression;
    size_t statement_count;
    AwkStatement statements[AWK_MAX_STATEMENTS];
} AwkClause;

typedef struct {
    size_t clause_count;
    AwkClause clauses[AWK_MAX_CLAUSES];
} AwkProgram;

typedef struct {
    const char *line;
    unsigned long long nr;
    unsigned long long nf;
    const char *field_starts[AWK_MAX_FIELDS];
    size_t field_lengths[AWK_MAX_FIELDS];
} AwkRecord;

typedef struct {
    char fs[AWK_MAX_TEXT];
    char ofs[AWK_MAX_TEXT];
} AwkState;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " 'BEGIN { FS=\":\"; OFS=\"|\" } /pattern/ { printf \"%s\\n\", $1 } END { print NR }' [file ...]");
}

static int is_identifier_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}

static int starts_with_keyword(const char *text, size_t index, const char *keyword) {
    size_t i = 0;

    while (keyword[i] != '\0') {
        if (text[index + i] != keyword[i]) {
            return 0;
        }
        i += 1;
    }

    return !is_identifier_char(text[index + i]);
}

static void skip_spaces(const char *text, size_t *index) {
    while (text[*index] != '\0' && rt_is_space(text[*index])) {
        *index += 1;
    }
}

static int parse_uint_token(const char *text, size_t *index, unsigned long long *value_out) {
    char digits[32];
    size_t length = 0;

    while (text[*index] >= '0' && text[*index] <= '9' && length + 1 < sizeof(digits)) {
        digits[length++] = text[*index];
        *index += 1;
    }
    digits[length] = '\0';

    if (length == 0) {
        return -1;
    }

    return rt_parse_uint(digits, value_out);
}

static int parse_string_literal(const char *text, size_t *index, char *buffer, size_t buffer_size) {
    size_t out = 0;

    if (text[*index] != '"' || buffer_size == 0) {
        return -1;
    }

    *index += 1;
    while (text[*index] != '\0' && text[*index] != '"') {
        char ch = text[*index];

        if (ch == '\\' && text[*index + 1] != '\0') {
            *index += 1;
            ch = text[*index];
            if (ch == 'n') {
                ch = '\n';
            } else if (ch == 't') {
                ch = '\t';
            }
        }

        if (out + 1 >= buffer_size) {
            return -1;
        }

        buffer[out++] = ch;
        *index += 1;
    }

    if (text[*index] != '"') {
        return -1;
    }

    buffer[out] = '\0';
    *index += 1;
    return 0;
}

static int parse_regex_literal(const char *text, size_t *index, char *buffer, size_t buffer_size) {
    size_t text_length = rt_strlen(text);
    size_t out = 0;

    if (buffer_size == 0 || *index >= text_length || text[*index] != '/') {
        return -1;
    }

    *index += 1;
    while (*index < text_length) {
        if (text[*index] == '/') {
            break;
        }

        if (text[*index] == '\\' && *index + 1 < text_length) {
            if (out + 2 >= buffer_size) {
                return -1;
            }
            buffer[out++] = text[*index];
            *index += 1;
        }

        if (out + 1 >= buffer_size) {
            return -1;
        }
        buffer[out++] = text[*index];
        *index += 1;
    }

    if (*index >= text_length || text[*index] != '/') {
        return -1;
    }

    buffer[out] = '\0';
    *index += 1;
    return 0;
}

static int parse_compare_operator(const char *text, size_t *index, AwkCompareOp *op_out) {
    if (text[*index] == '=' && text[*index + 1] == '=') {
        *op_out = AWK_COMPARE_EQ;
        *index += 2;
        return 0;
    }
    if (text[*index] == '!' && text[*index + 1] == '=') {
        *op_out = AWK_COMPARE_NE;
        *index += 2;
        return 0;
    }
    if (text[*index] == '<' && text[*index + 1] == '=') {
        *op_out = AWK_COMPARE_LE;
        *index += 2;
        return 0;
    }
    if (text[*index] == '>' && text[*index + 1] == '=') {
        *op_out = AWK_COMPARE_GE;
        *index += 2;
        return 0;
    }
    if (text[*index] == '<') {
        *op_out = AWK_COMPARE_LT;
        *index += 1;
        return 0;
    }
    if (text[*index] == '>') {
        *op_out = AWK_COMPARE_GT;
        *index += 1;
        return 0;
    }

    return -1;
}

static int parse_expression(const char *text, size_t *index, AwkExpression *expression);

static int parse_pattern(const char *text, size_t *index, AwkClause *clause) {
    if (text[*index] == '/') {
        if (parse_regex_literal(text, index, clause->pattern_text, sizeof(clause->pattern_text)) != 0) {
            return -1;
        }
        clause->pattern_type = AWK_PATTERN_REGEX;
        return 0;
    }

    {
        size_t saved = *index;
        AwkExpression expression;

        if (parse_expression(text, index, &expression) != 0) {
            *index = saved;
            return -1;
        }

        skip_spaces(text, index);
        if (text[*index] == '~' || (text[*index] == '!' && text[*index + 1] == '~')) {
            clause->pattern_type = AWK_PATTERN_EXPR_REGEX;
            clause->match_expression = expression;
            clause->match_negated = (text[*index] == '!');
            *index += (clause->match_negated ? 2U : 1U);
            skip_spaces(text, index);
            return parse_regex_literal(text, index, clause->pattern_text, sizeof(clause->pattern_text));
        }

        if (expression.type == AWK_EXPR_NR || expression.type == AWK_EXPR_NF) {
            clause->pattern_type = (expression.type == AWK_EXPR_NR) ? AWK_PATTERN_NR : AWK_PATTERN_NF;

            if (parse_compare_operator(text, index, &clause->compare_op) != 0) {
                clause->compare_op = AWK_COMPARE_NE;
                clause->compare_value = 0;
                return 0;
            }

            skip_spaces(text, index);
            if (parse_uint_token(text, index, &clause->compare_value) != 0) {
                return -1;
            }
            return 0;
        }

        *index = saved;
    }

    return -1;
}

static int parse_expression(const char *text, size_t *index, AwkExpression *expression) {
    rt_memset(expression, 0, sizeof(*expression));
    skip_spaces(text, index);

    if (text[*index] == '$') {
        *index += 1;
        if (parse_uint_token(text, index, &expression->number) != 0) {
            return -1;
        }
        expression->type = (expression->number == 0) ? AWK_EXPR_WHOLE_LINE : AWK_EXPR_FIELD;
        return 0;
    }

    if (starts_with_keyword(text, *index, "NR")) {
        expression->type = AWK_EXPR_NR;
        *index += 2;
        return 0;
    }

    if (starts_with_keyword(text, *index, "NF")) {
        expression->type = AWK_EXPR_NF;
        *index += 2;
        return 0;
    }

    if (starts_with_keyword(text, *index, "FS")) {
        expression->type = AWK_EXPR_FS;
        *index += 2;
        return 0;
    }

    if (starts_with_keyword(text, *index, "OFS")) {
        expression->type = AWK_EXPR_OFS;
        *index += 3;
        return 0;
    }

    if (text[*index] == '"') {
        expression->type = AWK_EXPR_STRING;
        return parse_string_literal(text, index, expression->text, sizeof(expression->text));
    }

    if (text[*index] >= '0' && text[*index] <= '9') {
        expression->type = AWK_EXPR_NUMBER;
        return parse_uint_token(text, index, &expression->number);
    }

    return -1;
}

static int parse_output_statement(const char *text, size_t *index, AwkStatement *statement, int is_printf) {
    const char *keyword = is_printf ? "printf" : "print";
    size_t keyword_length = is_printf ? 6U : 5U;

    if (!starts_with_keyword(text, *index, keyword)) {
        return -1;
    }

    rt_memset(statement, 0, sizeof(*statement));
    statement->kind = is_printf ? AWK_STATEMENT_PRINTF : AWK_STATEMENT_PRINT;
    *index += keyword_length;
    skip_spaces(text, index);

    while (text[*index] != '\0' && text[*index] != ';' && text[*index] != '}') {
        if (statement->expression_count >= AWK_MAX_EXPRESSIONS) {
            return -1;
        }

        if (parse_expression(text, index, &statement->expressions[statement->expression_count]) != 0) {
            return -1;
        }
        statement->expression_count += 1;

        skip_spaces(text, index);
        if (text[*index] == ',') {
            *index += 1;
            skip_spaces(text, index);
        } else if (text[*index] != '\0' && text[*index] != ';' && text[*index] != '}') {
            return -1;
        }
    }

    return 0;
}

static int parse_assignment_statement(const char *text, size_t *index, AwkStatement *statement) {
    AwkVariableName variable;

    if (starts_with_keyword(text, *index, "FS")) {
        variable = AWK_VARIABLE_FS;
    } else if (starts_with_keyword(text, *index, "OFS")) {
        variable = AWK_VARIABLE_OFS;
    } else {
        return -1;
    }

    rt_memset(statement, 0, sizeof(*statement));
    statement->kind = AWK_STATEMENT_ASSIGN;
    statement->variable = variable;
    if (variable == AWK_VARIABLE_FS) {
        *index += 2;
    } else {
        *index += 3;
    }

    skip_spaces(text, index);
    if (text[*index] != '=' || text[*index + 1] == '=') {
        return -1;
    }
    *index += 1;
    skip_spaces(text, index);

    if (parse_expression(text, index, &statement->expressions[0]) != 0) {
        return -1;
    }
    statement->expression_count = 1;
    return 0;
}

static int parse_statement(const char *text, size_t *index, AwkStatement *statement) {
    if (parse_output_statement(text, index, statement, 1) == 0) {
        return 0;
    }
    if (parse_output_statement(text, index, statement, 0) == 0) {
        return 0;
    }
    return parse_assignment_statement(text, index, statement);
}

static void set_default_print(AwkClause *clause) {
    rt_memset(&clause->statements[0], 0, sizeof(clause->statements[0]));
    clause->statements[0].kind = AWK_STATEMENT_PRINT;
    clause->statement_count = 1;
}

static int parse_action(const char *text, size_t *index, AwkClause *clause, int allow_default_print) {
    skip_spaces(text, index);

    if (text[*index] == '{') {
        *index += 1;
        skip_spaces(text, index);

        while (text[*index] != '\0' && text[*index] != '}') {
            if (clause->statement_count >= AWK_MAX_STATEMENTS) {
                return -1;
            }

            if (parse_statement(text, index, &clause->statements[clause->statement_count]) != 0) {
                return -1;
            }
            clause->statement_count += 1;

            skip_spaces(text, index);
            if (text[*index] == ';') {
                *index += 1;
                skip_spaces(text, index);
            }
        }

        if (text[*index] != '}') {
            return -1;
        }

        *index += 1;
        if (clause->statement_count == 0) {
            set_default_print(clause);
        }
        return 0;
    }

    if (starts_with_keyword(text, *index, "print") ||
        starts_with_keyword(text, *index, "printf") ||
        starts_with_keyword(text, *index, "FS") ||
        starts_with_keyword(text, *index, "OFS")) {
        if (parse_statement(text, index, &clause->statements[0]) != 0) {
            return -1;
        }
        clause->statement_count = 1;
        return 0;
    }

    if (allow_default_print) {
        set_default_print(clause);
        return 0;
    }

    return -1;
}

static int parse_program(const char *program_text, AwkProgram *program) {
    size_t index = 0;

    rt_memset(program, 0, sizeof(*program));

    while (1) {
        AwkClause *clause;

        skip_spaces(program_text, &index);
        if (program_text[index] == '\0') {
            break;
        }

        if (program->clause_count >= AWK_MAX_CLAUSES) {
            return -1;
        }

        clause = &program->clauses[program->clause_count];
        rt_memset(clause, 0, sizeof(*clause));
        clause->kind = AWK_CLAUSE_MAIN;
        clause->pattern_type = AWK_PATTERN_NONE;

        if (starts_with_keyword(program_text, index, "BEGIN")) {
            clause->kind = AWK_CLAUSE_BEGIN;
            index += 5;
        } else if (starts_with_keyword(program_text, index, "END")) {
            clause->kind = AWK_CLAUSE_END;
            index += 3;
        } else if (program_text[index] != '{' &&
                   !starts_with_keyword(program_text, index, "print") &&
                   !starts_with_keyword(program_text, index, "printf") &&
                   !starts_with_keyword(program_text, index, "FS") &&
                   !starts_with_keyword(program_text, index, "OFS")) {
            if (parse_pattern(program_text, &index, clause) != 0) {
                return -1;
            }
        }

        if (parse_action(program_text, &index, clause, clause->kind == AWK_CLAUSE_MAIN) != 0) {
            return -1;
        }

        program->clause_count += 1;
    }

    return program->clause_count == 0 ? -1 : 0;
}

static int contains_substring(const char *text, const char *pattern) {
    size_t start = 0;
    size_t end = 0;
    return tool_regex_search(pattern, text, 0, 0, &start, &end);
}

static void init_state(AwkState *state) {
    rt_copy_string(state->fs, sizeof(state->fs), " ");
    rt_copy_string(state->ofs, sizeof(state->ofs), " ");
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

static void init_record(AwkRecord *record, const char *line, unsigned long long nr, const AwkState *state) {
    rt_memset(record, 0, sizeof(*record));
    record->line = line;
    record->nr = nr;

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
        if (statement->variable == AWK_VARIABLE_FS) {
            rt_copy_string(state->fs, sizeof(state->fs), value);
        } else if (statement->variable == AWK_VARIABLE_OFS) {
            rt_copy_string(state->ofs, sizeof(state->ofs), value);
        }
        return 0;
    }

    if (statement->kind == AWK_STATEMENT_PRINTF) {
        return execute_printf_statement(statement, record, state);
    }

    if (statement->expression_count == 0) {
        return rt_write_line(1, record->line);
    }

    for (i = 0; i < statement->expression_count; ++i) {
        if (i > 0 && rt_write_cstr(1, state->ofs) != 0) {
            return -1;
        }
        if (write_expression(&statement->expressions[i], record, state) != 0) {
            return -1;
        }
    }

    return rt_write_char(1, '\n');
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

static int execute_clauses(const AwkProgram *program, AwkClauseKind kind, const AwkRecord *record, AwkState *state) {
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

static int awk_stream(int fd,
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

            if (ch == '\n') {
                AwkRecord record;

                line[line_len] = '\0';
                *line_number += 1;
                init_record(&record, line, *line_number, state);
                *last_nf = record.nf;

                if (execute_clauses(program, AWK_CLAUSE_MAIN, &record, state) != 0) {
                    return -1;
                }
                line_len = 0;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (line_len > 0) {
        AwkRecord record;

        line[line_len] = '\0';
        *line_number += 1;
        init_record(&record, line, *line_number, state);
        *last_nf = record.nf;

        if (execute_clauses(program, AWK_CLAUSE_MAIN, &record, state) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    AwkProgram program;
    AwkState state;
    AwkRecord edge_record;
    unsigned long long line_number = 0;
    unsigned long long last_nf = 0;
    int i;
    int exit_code = 0;

    if (argc < 2 || parse_program(argv[1], &program) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    init_state(&state);
    init_record(&edge_record, "", 0, &state);
    if (execute_clauses(&program, AWK_CLAUSE_BEGIN, &edge_record, &state) != 0) {
        return 1;
    }

    if (argc == 2) {
        if (awk_stream(0, &program, &state, &line_number, &last_nf) != 0) {
            return 1;
        }
    } else {
        for (i = 2; i < argc; ++i) {
            int fd;
            int should_close;

            if (tool_open_input(argv[i], &fd, &should_close) != 0) {
                rt_write_cstr(2, "awk: cannot open ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
                continue;
            }

            if (awk_stream(fd, &program, &state, &line_number, &last_nf) != 0) {
                rt_write_cstr(2, "awk: read error on ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
            }

            tool_close_input(fd, should_close);
        }
    }

    init_record(&edge_record, "", line_number, &state);
    edge_record.nf = last_nf;
    if (execute_clauses(&program, AWK_CLAUSE_END, &edge_record, &state) != 0) {
        return 1;
    }

    return exit_code;
}
