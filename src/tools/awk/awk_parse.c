/*
 * awk_parse.c - awk program parser.
 *
 * Covers: tokenisation helpers (skip_spaces, parse_uint_token,
 * parse_string_literal, parse_regex_literal, parse_compare_operator),
 * expression and statement parsers, and the top-level parse_program entry.
 */

#include "awk_impl.h"

static int is_identifier_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}

static int is_identifier_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
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

static int parse_identifier(const char *text, size_t *index, char *buffer, size_t buffer_size) {
    size_t out = 0;

    if (!is_identifier_start(text[*index]) || buffer == 0 || buffer_size == 0) {
        return -1;
    }

    while (is_identifier_char(text[*index])) {
        if (out + 1 >= buffer_size) {
            return -1;
        }
        buffer[out++] = text[*index];
        *index += 1;
    }

    buffer[out] = '\0';
    return 0;
}

static int looks_like_assignment_statement(const char *text, size_t index) {
    char identifier[AWK_MAX_TEXT];
    size_t probe = index;

    if (parse_identifier(text, &probe, identifier, sizeof(identifier)) != 0) {
        return 0;
    }

    skip_spaces(text, &probe);
    return text[probe] == '=' && text[probe + 1] != '=';
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

        if (expression.type == AWK_EXPR_NR || expression.type == AWK_EXPR_NF || expression.type == AWK_EXPR_FNR) {
            clause->pattern_type = (expression.type == AWK_EXPR_NR)
                                     ? AWK_PATTERN_NR
                                     : ((expression.type == AWK_EXPR_NF) ? AWK_PATTERN_NF : AWK_PATTERN_FNR);

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

    if (starts_with_keyword(text, *index, "FNR")) {
        expression->type = AWK_EXPR_FNR;
        *index += 3;
        return 0;
    }

    if (starts_with_keyword(text, *index, "FILENAME")) {
        expression->type = AWK_EXPR_FILENAME;
        *index += 8;
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

    if (starts_with_keyword(text, *index, "RS")) {
        expression->type = AWK_EXPR_RS;
        *index += 2;
        return 0;
    }

    if (starts_with_keyword(text, *index, "ORS")) {
        expression->type = AWK_EXPR_ORS;
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

    if (is_identifier_start(text[*index])) {
        expression->type = AWK_EXPR_VARIABLE;
        return parse_identifier(text, index, expression->text, sizeof(expression->text));
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
    size_t saved = *index;
    char identifier[AWK_MAX_TEXT];

    if (parse_identifier(text, index, identifier, sizeof(identifier)) != 0) {
        return -1;
    }

    rt_memset(statement, 0, sizeof(*statement));
    statement->kind = AWK_STATEMENT_ASSIGN;
    rt_copy_string(statement->variable_name, sizeof(statement->variable_name), identifier);
    if (rt_strcmp(identifier, "FS") == 0) {
        statement->variable = AWK_VARIABLE_FS;
    } else if (rt_strcmp(identifier, "OFS") == 0) {
        statement->variable = AWK_VARIABLE_OFS;
    } else if (rt_strcmp(identifier, "RS") == 0) {
        statement->variable = AWK_VARIABLE_RS;
    } else if (rt_strcmp(identifier, "ORS") == 0) {
        statement->variable = AWK_VARIABLE_ORS;
    } else {
        statement->variable = AWK_VARIABLE_USER;
    }

    skip_spaces(text, index);
    if (text[*index] != '=' || text[*index + 1] == '=') {
        *index = saved;
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
        looks_like_assignment_statement(text, *index)) {
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

int parse_program(const char *program_text, AwkProgram *program) {
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
                   !looks_like_assignment_statement(program_text, index)) {
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
