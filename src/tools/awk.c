#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define AWK_LINE_CAPACITY 8192
#define AWK_MAX_CLAUSES 16
#define AWK_MAX_STATEMENTS 8
#define AWK_MAX_EXPRESSIONS 8
#define AWK_MAX_TEXT 128
#define AWK_MAX_FIELDS 256

typedef enum {
    AWK_CLAUSE_MAIN = 0,
    AWK_CLAUSE_BEGIN = 1,
    AWK_CLAUSE_END = 2
} AwkClauseKind;

typedef enum {
    AWK_PATTERN_NONE = 0,
    AWK_PATTERN_SUBSTRING = 1,
    AWK_PATTERN_NR = 2,
    AWK_PATTERN_NF = 3
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
    AWK_EXPR_STRING = 4,
    AWK_EXPR_NUMBER = 5
} AwkExprType;

typedef struct {
    AwkExprType type;
    unsigned long long number;
    char text[AWK_MAX_TEXT];
} AwkExpression;

typedef struct {
    size_t expression_count;
    AwkExpression expressions[AWK_MAX_EXPRESSIONS];
} AwkPrintStatement;

typedef struct {
    AwkClauseKind kind;
    AwkPatternType pattern_type;
    AwkCompareOp compare_op;
    unsigned long long compare_value;
    char pattern_text[AWK_MAX_TEXT];
    size_t statement_count;
    AwkPrintStatement statements[AWK_MAX_STATEMENTS];
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

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " 'BEGIN { print } /pattern/ { print NR, NF, $1 } END { print NR }' [file ...]");
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

static int parse_pattern(const char *text, size_t *index, AwkClause *clause) {
    if (text[*index] == '/') {
        size_t out = 0;

        *index += 1;
        while (text[*index] != '\0' && text[*index] != '/') {
            if (out + 1 >= sizeof(clause->pattern_text)) {
                return -1;
            }
            clause->pattern_text[out++] = text[*index];
            *index += 1;
        }

        if (text[*index] != '/') {
            return -1;
        }

        clause->pattern_text[out] = '\0';
        clause->pattern_type = AWK_PATTERN_SUBSTRING;
        *index += 1;
        return 0;
    }

    if (starts_with_keyword(text, *index, "NR") || starts_with_keyword(text, *index, "NF")) {
        clause->pattern_type = starts_with_keyword(text, *index, "NR") ? AWK_PATTERN_NR : AWK_PATTERN_NF;
        *index += 2;
        skip_spaces(text, index);

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

static int parse_print_statement(const char *text, size_t *index, AwkPrintStatement *statement) {
    if (!starts_with_keyword(text, *index, "print")) {
        return -1;
    }

    rt_memset(statement, 0, sizeof(*statement));
    *index += 5;
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

static void set_default_print(AwkClause *clause) {
    rt_memset(&clause->statements[0], 0, sizeof(clause->statements[0]));
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

            if (parse_print_statement(text, index, &clause->statements[clause->statement_count]) != 0) {
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

    if (starts_with_keyword(text, *index, "print")) {
        if (parse_print_statement(text, index, &clause->statements[0]) != 0) {
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
        } else if (program_text[index] != '{' && !starts_with_keyword(program_text, index, "print")) {
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

static void init_record(AwkRecord *record, const char *line, unsigned long long nr) {
    size_t i = 0;

    rt_memset(record, 0, sizeof(*record));
    record->line = line;
    record->nr = nr;

    while (line[i] != '\0') {
        size_t start;
        size_t length = 0;

        while (line[i] != '\0' && rt_is_space(line[i])) {
            i += 1;
        }
        if (line[i] == '\0') {
            break;
        }

        start = i;
        while (line[i] != '\0' && !rt_is_space(line[i])) {
            i += 1;
            length += 1;
        }

        if (record->nf < AWK_MAX_FIELDS) {
            record->field_starts[record->nf] = line + start;
            record->field_lengths[record->nf] = length;
        }
        record->nf += 1;
    }
}

static int write_expression(const AwkExpression *expression, const AwkRecord *record) {
    switch (expression->type) {
        case AWK_EXPR_WHOLE_LINE:
            return rt_write_all(1, record->line, rt_strlen(record->line));
        case AWK_EXPR_FIELD:
            if (expression->number == 0) {
                return rt_write_all(1, record->line, rt_strlen(record->line));
            }
            if (expression->number <= record->nf && expression->number <= AWK_MAX_FIELDS) {
                size_t field_index = (size_t)(expression->number - 1);
                return rt_write_all(1, record->field_starts[field_index], record->field_lengths[field_index]);
            }
            return 0;
        case AWK_EXPR_NR:
            return rt_write_uint(1, record->nr);
        case AWK_EXPR_NF:
            return rt_write_uint(1, record->nf);
        case AWK_EXPR_STRING:
            return rt_write_cstr(1, expression->text);
        case AWK_EXPR_NUMBER:
            return rt_write_uint(1, expression->number);
        default:
            return -1;
    }
}

static int execute_statement(const AwkPrintStatement *statement, const AwkRecord *record) {
    size_t i;

    if (statement->expression_count == 0) {
        return rt_write_line(1, record->line);
    }

    for (i = 0; i < statement->expression_count; ++i) {
        if (i > 0 && rt_write_char(1, ' ') != 0) {
            return -1;
        }
        if (write_expression(&statement->expressions[i], record) != 0) {
            return -1;
        }
    }

    return rt_write_char(1, '\n');
}

static int clause_matches(const AwkClause *clause, const AwkRecord *record) {
    switch (clause->pattern_type) {
        case AWK_PATTERN_NONE:
            return 1;
        case AWK_PATTERN_SUBSTRING:
            return contains_substring(record->line, clause->pattern_text);
        case AWK_PATTERN_NR:
            return compare_values(record->nr, clause->compare_op, clause->compare_value);
        case AWK_PATTERN_NF:
            return compare_values(record->nf, clause->compare_op, clause->compare_value);
        default:
            return 0;
    }
}

static int execute_clauses(const AwkProgram *program, AwkClauseKind kind, const AwkRecord *record) {
    size_t i;
    size_t j;

    for (i = 0; i < program->clause_count; ++i) {
        const AwkClause *clause = &program->clauses[i];

        if (clause->kind != kind) {
            continue;
        }
        if (kind == AWK_CLAUSE_MAIN && !clause_matches(clause, record)) {
            continue;
        }

        for (j = 0; j < clause->statement_count; ++j) {
            if (execute_statement(&clause->statements[j], record) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int awk_stream(int fd, const AwkProgram *program, unsigned long long *line_number, unsigned long long *last_nf) {
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
                init_record(&record, line, *line_number);
                *last_nf = record.nf;

                if (execute_clauses(program, AWK_CLAUSE_MAIN, &record) != 0) {
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
        init_record(&record, line, *line_number);
        *last_nf = record.nf;

        if (execute_clauses(program, AWK_CLAUSE_MAIN, &record) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    AwkProgram program;
    AwkRecord edge_record;
    unsigned long long line_number = 0;
    unsigned long long last_nf = 0;
    int i;
    int exit_code = 0;

    if (argc < 2 || parse_program(argv[1], &program) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    init_record(&edge_record, "", 0);
    if (execute_clauses(&program, AWK_CLAUSE_BEGIN, &edge_record) != 0) {
        return 1;
    }

    if (argc == 2) {
        if (awk_stream(0, &program, &line_number, &last_nf) != 0) {
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

            if (awk_stream(fd, &program, &line_number, &last_nf) != 0) {
                rt_write_cstr(2, "awk: read error on ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
            }

            tool_close_input(fd, should_close);
        }
    }

    init_record(&edge_record, "", line_number);
    edge_record.nf = last_nf;
    if (execute_clauses(&program, AWK_CLAUSE_END, &edge_record) != 0) {
        return 1;
    }

    return exit_code;
}
