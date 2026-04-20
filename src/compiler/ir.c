#include "ir.h"

#include "runtime.h"

#define COMPILER_IR_TRACKED_VALUE_CAPACITY 256

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    long long value;
} IrTrackedValue;

typedef struct {
    IrTrackedValue global_constants[COMPILER_IR_TRACKED_VALUE_CAPACITY];
    size_t global_constant_count;
    IrTrackedValue local_values[COMPILER_IR_TRACKED_VALUE_CAPACITY];
    size_t local_value_count;
} IrOptimizerState;

typedef enum {
    IR_CONST_TOKEN_EOF = 0,
    IR_CONST_TOKEN_IDENTIFIER,
    IR_CONST_TOKEN_NUMBER,
    IR_CONST_TOKEN_CHAR,
    IR_CONST_TOKEN_PUNCT,
    IR_CONST_TOKEN_INVALID
} IrConstTokenKind;

typedef struct {
    IrConstTokenKind kind;
    char text[COMPILER_IR_NAME_CAPACITY];
    long long number_value;
} IrConstToken;

typedef struct {
    const char *cursor;
    IrConstToken current;
    const IrOptimizerState *state;
} IrConstParser;

static int ir_text_equals(const char *lhs, const char *rhs);
static int ir_starts_with(const char *text, const char *prefix);
static const char *ir_skip_spaces(const char *text);
static int ir_parse_punctuator_width(const char *cursor);
static int ir_parse_unsigned_value(const char *text, int base, unsigned long long *value_out);
static int ir_parse_number_value(const char *text, long long *value_out);
static void ir_const_next(IrConstParser *parser);
static int ir_lookup_value(const IrOptimizerState *state, const char *name, long long *value_out);
static int ir_apply_binary_op(const char *op, long long lhs, long long rhs, long long *value_out);
static int ir_parse_conditional_expr(IrConstParser *parser, long long *value_out);
static int ir_parse_logical_or_expr(IrConstParser *parser, long long *value_out);
static int ir_parse_logical_and_expr(IrConstParser *parser, long long *value_out);
static int ir_parse_bitor_expr(IrConstParser *parser, long long *value_out);
static int ir_parse_bitxor_expr(IrConstParser *parser, long long *value_out);
static int ir_parse_bitand_expr(IrConstParser *parser, long long *value_out);
static int ir_parse_equality_expr(IrConstParser *parser, long long *value_out);
static int ir_parse_relational_expr(IrConstParser *parser, long long *value_out);
static int ir_parse_shift_expr(IrConstParser *parser, long long *value_out);
static int ir_parse_additive_expr(IrConstParser *parser, long long *value_out);
static int ir_parse_multiplicative_expr(IrConstParser *parser, long long *value_out);
static int ir_parse_unary_expr(IrConstParser *parser, long long *value_out);
static int ir_parse_primary_expr(IrConstParser *parser, long long *value_out);
static int ir_evaluate_constant_expression(const char *expr, const IrOptimizerState *state, long long *value_out);
static void ir_format_signed_value(long long value, char *buffer, size_t buffer_size);
static int ir_copy_identifier(char *buffer, size_t buffer_size, const char *start, const char *end);
static void ir_clear_local_values(IrOptimizerState *state);
static void ir_invalidate_local_value(IrOptimizerState *state, const char *name);
static int ir_set_tracked_value(IrTrackedValue *values, size_t *count, const char *name, long long value);
static int ir_rewrite_expr_line(CompilerIr *ir, size_t index, const char *prefix, const char *expr);
static int ir_rewrite_jump_line(CompilerIr *ir, size_t index, const char *label);
static void ir_remove_line(CompilerIr *ir, size_t index);
static int ir_extract_store_parts(const char *line, char *name, size_t name_size, const char **expr_out);
static int ir_extract_assignment_parts(const char *expr, char *name, size_t name_size, char *op, size_t op_size, const char **rhs_out);
static int ir_extract_branch_parts(const char *line, char *label, size_t label_size, char *expr, size_t expr_size);
static int ir_extract_const_parts(const char *line, char *name, size_t name_size, const char **expr_out);

static void set_error(CompilerIr *ir, const char *message) {
    rt_copy_string(ir->error_message, sizeof(ir->error_message), message != 0 ? message : "IR error");
}

static int ir_text_equals(const char *lhs, const char *rhs) {
    size_t i = 0;

    while (lhs[i] != '\0' && rhs[i] != '\0') {
        if (lhs[i] != rhs[i]) {
            return 0;
        }
        i += 1U;
    }

    return lhs[i] == rhs[i];
}

static int ir_starts_with(const char *text, const char *prefix) {
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }

    return 1;
}

static const char *ir_skip_spaces(const char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text += 1;
    }
    return text;
}

static int ir_parse_punctuator_width(const char *cursor) {
    if ((cursor[0] == '<' && cursor[1] == '<' && cursor[2] == '=') ||
        (cursor[0] == '>' && cursor[1] == '>' && cursor[2] == '=')) {
        return 3;
    }
    if ((cursor[0] == '&' && cursor[1] == '&') ||
        (cursor[0] == '|' && cursor[1] == '|') ||
        (cursor[0] == '=' && cursor[1] == '=') ||
        (cursor[0] == '!' && cursor[1] == '=') ||
        (cursor[0] == '<' && cursor[1] == '=') ||
        (cursor[0] == '>' && cursor[1] == '=') ||
        (cursor[0] == '<' && cursor[1] == '<') ||
        (cursor[0] == '>' && cursor[1] == '>') ||
        (cursor[0] == '&' && cursor[1] == '=') ||
        (cursor[0] == '|' && cursor[1] == '=') ||
        (cursor[0] == '^' && cursor[1] == '=') ||
        (cursor[0] == '+' && cursor[1] == '=') ||
        (cursor[0] == '-' && cursor[1] == '=') ||
        (cursor[0] == '*' && cursor[1] == '=') ||
        (cursor[0] == '/' && cursor[1] == '=') ||
        (cursor[0] == '%' && cursor[1] == '=') ||
        (cursor[0] == '+' && cursor[1] == '+') ||
        (cursor[0] == '-' && cursor[1] == '-')) {
        return 2;
    }
    return 0;
}

static int ir_parse_unsigned_value(const char *text, int base, unsigned long long *value_out) {
    unsigned long long value = 0;
    size_t i = 0;

    while (text[i] != '\0') {
        unsigned int digit;
        char ch = text[i];
        if (ch >= '0' && ch <= '9') {
            digit = (unsigned int)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = 10U + (unsigned int)(ch - 'a');
        } else if (ch >= 'A' && ch <= 'F') {
            digit = 10U + (unsigned int)(ch - 'A');
        } else {
            break;
        }
        if ((int)digit >= base) {
            return -1;
        }
        value = value * (unsigned long long)base + (unsigned long long)digit;
        i += 1U;
    }

    *value_out = value;
    return 0;
}

static int ir_parse_number_value(const char *text, long long *value_out) {
    unsigned long long magnitude = 0;
    int negative = 0;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }
    if (*text == '-') {
        negative = 1;
        text += 1;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        if (ir_parse_unsigned_value(text + 2, 16, &magnitude) != 0) {
            return -1;
        }
    } else if (ir_parse_unsigned_value(text, 10, &magnitude) != 0) {
        return -1;
    }

    *value_out = negative ? -(long long)magnitude : (long long)magnitude;
    return 0;
}

static void ir_const_next(IrConstParser *parser) {
    const char *cursor = ir_skip_spaces(parser->cursor);
    size_t length = 0;

    parser->cursor = cursor;
    parser->current.kind = IR_CONST_TOKEN_EOF;
    parser->current.text[0] = '\0';
    parser->current.number_value = 0;

    if (*cursor == '\0') {
        return;
    }

    if ((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') || *cursor == '_') {
        parser->current.kind = IR_CONST_TOKEN_IDENTIFIER;
        while (((*cursor >= 'a' && *cursor <= 'z') ||
                (*cursor >= 'A' && *cursor <= 'Z') ||
                (*cursor >= '0' && *cursor <= '9') ||
                *cursor == '_') &&
               length + 1U < sizeof(parser->current.text)) {
            parser->current.text[length++] = *cursor++;
        }
        parser->current.text[length] = '\0';
        parser->cursor = cursor;
        return;
    }

    if (*cursor >= '0' && *cursor <= '9') {
        parser->current.kind = IR_CONST_TOKEN_NUMBER;
        if (*cursor == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
            if (length + 2U < sizeof(parser->current.text)) {
                parser->current.text[length++] = *cursor++;
                parser->current.text[length++] = *cursor++;
            } else {
                cursor += 2;
            }
            while (((*cursor >= '0' && *cursor <= '9') ||
                    (*cursor >= 'a' && *cursor <= 'f') ||
                    (*cursor >= 'A' && *cursor <= 'F')) &&
                   length + 1U < sizeof(parser->current.text)) {
                parser->current.text[length++] = *cursor++;
            }
        } else {
            while (*cursor >= '0' && *cursor <= '9' && length + 1U < sizeof(parser->current.text)) {
                parser->current.text[length++] = *cursor++;
            }
        }
        parser->current.text[length] = '\0';
        while (*cursor == 'u' || *cursor == 'U' || *cursor == 'l' || *cursor == 'L') {
            cursor += 1;
        }
        if (ir_parse_number_value(parser->current.text, &parser->current.number_value) != 0) {
            parser->current.kind = IR_CONST_TOKEN_INVALID;
        }
        parser->cursor = cursor;
        return;
    }

    if (*cursor == '\'') {
        char ch = 0;
        parser->current.kind = IR_CONST_TOKEN_CHAR;
        cursor += 1;
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor += 1;
            if (*cursor == 'n') ch = '\n';
            else if (*cursor == 't') ch = '\t';
            else if (*cursor == 'r') ch = '\r';
            else if (*cursor == '0') ch = '\0';
            else ch = *cursor;
            cursor += 1;
        } else {
            ch = *cursor;
            if (*cursor != '\0') {
                cursor += 1;
            }
        }
        while (*cursor != '\0' && *cursor != '\'') {
            cursor += 1;
        }
        if (*cursor == '\'') {
            cursor += 1;
        }
        parser->current.number_value = (long long)(unsigned char)ch;
        parser->cursor = cursor;
        return;
    }

    if (*cursor == '"') {
        parser->current.kind = IR_CONST_TOKEN_INVALID;
        parser->cursor = cursor + 1;
        return;
    }

    parser->current.kind = IR_CONST_TOKEN_PUNCT;
    {
        int width = ir_parse_punctuator_width(cursor);
        if (width == 3) {
            parser->current.text[0] = cursor[0];
            parser->current.text[1] = cursor[1];
            parser->current.text[2] = cursor[2];
            parser->current.text[3] = '\0';
            parser->cursor = cursor + 3;
            return;
        }
        if (width == 2) {
            parser->current.text[0] = cursor[0];
            parser->current.text[1] = cursor[1];
            parser->current.text[2] = '\0';
            parser->cursor = cursor + 2;
            return;
        }
    }

    parser->current.text[0] = *cursor;
    parser->current.text[1] = '\0';
    parser->cursor = cursor + 1;
}

static int ir_lookup_value(const IrOptimizerState *state, const char *name, long long *value_out) {
    size_t i;

    if (ir_text_equals(name, "NULL")) {
        *value_out = 0;
        return 0;
    }

    for (i = state->local_value_count; i > 0; --i) {
        if (ir_text_equals(state->local_values[i - 1U].name, name)) {
            *value_out = state->local_values[i - 1U].value;
            return 0;
        }
    }
    for (i = state->global_constant_count; i > 0; --i) {
        if (ir_text_equals(state->global_constants[i - 1U].name, name)) {
            *value_out = state->global_constants[i - 1U].value;
            return 0;
        }
    }

    return -1;
}

static int ir_apply_binary_op(const char *op, long long lhs, long long rhs, long long *value_out) {
    if (ir_text_equals(op, "*")) {
        *value_out = lhs * rhs;
    } else if (ir_text_equals(op, "/")) {
        if (rhs == 0) {
            return -1;
        }
        *value_out = lhs / rhs;
    } else if (ir_text_equals(op, "%")) {
        if (rhs == 0) {
            return -1;
        }
        *value_out = lhs % rhs;
    } else if (ir_text_equals(op, "+")) {
        *value_out = lhs + rhs;
    } else if (ir_text_equals(op, "-")) {
        *value_out = lhs - rhs;
    } else if (ir_text_equals(op, "<<")) {
        *value_out = lhs << rhs;
    } else if (ir_text_equals(op, ">>")) {
        *value_out = lhs >> rhs;
    } else if (ir_text_equals(op, "<")) {
        *value_out = lhs < rhs;
    } else if (ir_text_equals(op, ">")) {
        *value_out = lhs > rhs;
    } else if (ir_text_equals(op, "<=")) {
        *value_out = lhs <= rhs;
    } else if (ir_text_equals(op, ">=")) {
        *value_out = lhs >= rhs;
    } else if (ir_text_equals(op, "==")) {
        *value_out = lhs == rhs;
    } else if (ir_text_equals(op, "!=")) {
        *value_out = lhs != rhs;
    } else if (ir_text_equals(op, "&")) {
        *value_out = lhs & rhs;
    } else if (ir_text_equals(op, "^")) {
        *value_out = lhs ^ rhs;
    } else if (ir_text_equals(op, "|")) {
        *value_out = lhs | rhs;
    } else if (ir_text_equals(op, "&&")) {
        *value_out = (lhs != 0 && rhs != 0) ? 1 : 0;
    } else if (ir_text_equals(op, "||")) {
        *value_out = (lhs != 0 || rhs != 0) ? 1 : 0;
    } else {
        return -1;
    }

    return 0;
}

static int ir_parse_primary_expr(IrConstParser *parser, long long *value_out) {
    if (parser->current.kind == IR_CONST_TOKEN_NUMBER || parser->current.kind == IR_CONST_TOKEN_CHAR) {
        *value_out = parser->current.number_value;
        ir_const_next(parser);
        return 0;
    }
    if (parser->current.kind == IR_CONST_TOKEN_IDENTIFIER) {
        if (ir_lookup_value(parser->state, parser->current.text, value_out) != 0) {
            return -1;
        }
        ir_const_next(parser);
        return 0;
    }
    if (parser->current.kind == IR_CONST_TOKEN_PUNCT && ir_text_equals(parser->current.text, "(")) {
        ir_const_next(parser);
        if (ir_parse_conditional_expr(parser, value_out) != 0) {
            return -1;
        }
        if (parser->current.kind != IR_CONST_TOKEN_PUNCT || !ir_text_equals(parser->current.text, ")")) {
            return -1;
        }
        ir_const_next(parser);
        return 0;
    }
    return -1;
}

static int ir_parse_unary_expr(IrConstParser *parser, long long *value_out) {
    char op[4];

    if (parser->current.kind == IR_CONST_TOKEN_PUNCT &&
        (ir_text_equals(parser->current.text, "+") ||
         ir_text_equals(parser->current.text, "-") ||
         ir_text_equals(parser->current.text, "!") ||
         ir_text_equals(parser->current.text, "~"))) {
        rt_copy_string(op, sizeof(op), parser->current.text);
        ir_const_next(parser);
        if (ir_parse_unary_expr(parser, value_out) != 0) {
            return -1;
        }
        if (ir_text_equals(op, "-")) {
            *value_out = -*value_out;
        } else if (ir_text_equals(op, "!")) {
            *value_out = (*value_out == 0) ? 1 : 0;
        } else if (ir_text_equals(op, "~")) {
            *value_out = ~*value_out;
        }
        return 0;
    }

    return ir_parse_primary_expr(parser, value_out);
}

static int ir_parse_multiplicative_expr(IrConstParser *parser, long long *value_out) {
    if (ir_parse_unary_expr(parser, value_out) != 0) {
        return -1;
    }

    while (parser->current.kind == IR_CONST_TOKEN_PUNCT &&
           (ir_text_equals(parser->current.text, "*") ||
            ir_text_equals(parser->current.text, "/") ||
            ir_text_equals(parser->current.text, "%"))) {
        char op[4];
        long long rhs;
        rt_copy_string(op, sizeof(op), parser->current.text);
        ir_const_next(parser);
        if (ir_parse_unary_expr(parser, &rhs) != 0 || ir_apply_binary_op(op, *value_out, rhs, value_out) != 0) {
            return -1;
        }
    }

    return 0;
}

static int ir_parse_additive_expr(IrConstParser *parser, long long *value_out) {
    if (ir_parse_multiplicative_expr(parser, value_out) != 0) {
        return -1;
    }

    while (parser->current.kind == IR_CONST_TOKEN_PUNCT &&
           (ir_text_equals(parser->current.text, "+") || ir_text_equals(parser->current.text, "-"))) {
        char op[4];
        long long rhs;
        rt_copy_string(op, sizeof(op), parser->current.text);
        ir_const_next(parser);
        if (ir_parse_multiplicative_expr(parser, &rhs) != 0 || ir_apply_binary_op(op, *value_out, rhs, value_out) != 0) {
            return -1;
        }
    }

    return 0;
}

static int ir_parse_shift_expr(IrConstParser *parser, long long *value_out) {
    if (ir_parse_additive_expr(parser, value_out) != 0) {
        return -1;
    }

    while (parser->current.kind == IR_CONST_TOKEN_PUNCT &&
           (ir_text_equals(parser->current.text, "<<") || ir_text_equals(parser->current.text, ">>"))) {
        char op[4];
        long long rhs;
        rt_copy_string(op, sizeof(op), parser->current.text);
        ir_const_next(parser);
        if (ir_parse_additive_expr(parser, &rhs) != 0 || ir_apply_binary_op(op, *value_out, rhs, value_out) != 0) {
            return -1;
        }
    }

    return 0;
}

static int ir_parse_relational_expr(IrConstParser *parser, long long *value_out) {
    if (ir_parse_shift_expr(parser, value_out) != 0) {
        return -1;
    }

    while (parser->current.kind == IR_CONST_TOKEN_PUNCT &&
           (ir_text_equals(parser->current.text, "<") ||
            ir_text_equals(parser->current.text, ">") ||
            ir_text_equals(parser->current.text, "<=") ||
            ir_text_equals(parser->current.text, ">="))) {
        char op[4];
        long long rhs;
        rt_copy_string(op, sizeof(op), parser->current.text);
        ir_const_next(parser);
        if (ir_parse_shift_expr(parser, &rhs) != 0 || ir_apply_binary_op(op, *value_out, rhs, value_out) != 0) {
            return -1;
        }
    }

    return 0;
}

static int ir_parse_equality_expr(IrConstParser *parser, long long *value_out) {
    if (ir_parse_relational_expr(parser, value_out) != 0) {
        return -1;
    }

    while (parser->current.kind == IR_CONST_TOKEN_PUNCT &&
           (ir_text_equals(parser->current.text, "==") || ir_text_equals(parser->current.text, "!="))) {
        char op[4];
        long long rhs;
        rt_copy_string(op, sizeof(op), parser->current.text);
        ir_const_next(parser);
        if (ir_parse_relational_expr(parser, &rhs) != 0 || ir_apply_binary_op(op, *value_out, rhs, value_out) != 0) {
            return -1;
        }
    }

    return 0;
}

static int ir_parse_bitand_expr(IrConstParser *parser, long long *value_out) {
    if (ir_parse_equality_expr(parser, value_out) != 0) {
        return -1;
    }

    while (parser->current.kind == IR_CONST_TOKEN_PUNCT && ir_text_equals(parser->current.text, "&")) {
        long long rhs;
        ir_const_next(parser);
        if (ir_parse_equality_expr(parser, &rhs) != 0 || ir_apply_binary_op("&", *value_out, rhs, value_out) != 0) {
            return -1;
        }
    }

    return 0;
}

static int ir_parse_bitxor_expr(IrConstParser *parser, long long *value_out) {
    if (ir_parse_bitand_expr(parser, value_out) != 0) {
        return -1;
    }

    while (parser->current.kind == IR_CONST_TOKEN_PUNCT && ir_text_equals(parser->current.text, "^")) {
        long long rhs;
        ir_const_next(parser);
        if (ir_parse_bitand_expr(parser, &rhs) != 0 || ir_apply_binary_op("^", *value_out, rhs, value_out) != 0) {
            return -1;
        }
    }

    return 0;
}

static int ir_parse_bitor_expr(IrConstParser *parser, long long *value_out) {
    if (ir_parse_bitxor_expr(parser, value_out) != 0) {
        return -1;
    }

    while (parser->current.kind == IR_CONST_TOKEN_PUNCT &&
           ir_text_equals(parser->current.text, "|") &&
           !ir_text_equals(parser->current.text, "||")) {
        long long rhs;
        ir_const_next(parser);
        if (ir_parse_bitxor_expr(parser, &rhs) != 0 || ir_apply_binary_op("|", *value_out, rhs, value_out) != 0) {
            return -1;
        }
    }

    return 0;
}

static int ir_parse_logical_and_expr(IrConstParser *parser, long long *value_out) {
    if (ir_parse_bitor_expr(parser, value_out) != 0) {
        return -1;
    }

    while (parser->current.kind == IR_CONST_TOKEN_PUNCT && ir_text_equals(parser->current.text, "&&")) {
        long long rhs;
        ir_const_next(parser);
        if (ir_parse_bitor_expr(parser, &rhs) != 0 || ir_apply_binary_op("&&", *value_out, rhs, value_out) != 0) {
            return -1;
        }
    }

    return 0;
}

static int ir_parse_logical_or_expr(IrConstParser *parser, long long *value_out) {
    if (ir_parse_logical_and_expr(parser, value_out) != 0) {
        return -1;
    }

    while (parser->current.kind == IR_CONST_TOKEN_PUNCT && ir_text_equals(parser->current.text, "||")) {
        long long rhs;
        ir_const_next(parser);
        if (ir_parse_logical_and_expr(parser, &rhs) != 0 || ir_apply_binary_op("||", *value_out, rhs, value_out) != 0) {
            return -1;
        }
    }

    return 0;
}

static int ir_parse_conditional_expr(IrConstParser *parser, long long *value_out) {
    long long true_value;
    long long false_value;

    if (ir_parse_logical_or_expr(parser, value_out) != 0) {
        return -1;
    }

    if (parser->current.kind == IR_CONST_TOKEN_PUNCT && ir_text_equals(parser->current.text, "?")) {
        ir_const_next(parser);
        if (ir_parse_conditional_expr(parser, &true_value) != 0) {
            return -1;
        }
        if (parser->current.kind != IR_CONST_TOKEN_PUNCT || !ir_text_equals(parser->current.text, ":")) {
            return -1;
        }
        ir_const_next(parser);
        if (ir_parse_conditional_expr(parser, &false_value) != 0) {
            return -1;
        }
        *value_out = (*value_out != 0) ? true_value : false_value;
    }

    return 0;
}

static int ir_evaluate_constant_expression(const char *expr, const IrOptimizerState *state, long long *value_out) {
    IrConstParser parser;

    parser.cursor = expr != 0 ? expr : "";
    parser.state = state;
    ir_const_next(&parser);

    if (parser.current.kind == IR_CONST_TOKEN_EOF ||
        parser.current.kind == IR_CONST_TOKEN_INVALID ||
        ir_parse_conditional_expr(&parser, value_out) != 0) {
        return -1;
    }

    return parser.current.kind == IR_CONST_TOKEN_EOF ? 0 : -1;
}

static int append_text(char *buffer, size_t buffer_size, size_t *offset, const char *text) {
    size_t i = 0;

    while (text[i] != '\0') {
        if (*offset + 1 >= buffer_size) {
            return -1;
        }
        buffer[*offset] = text[i];
        *offset += 1U;
        i += 1U;
    }

    buffer[*offset] = '\0';
    return 0;
}

static int append_uint(char *buffer, size_t buffer_size, size_t *offset, unsigned long long value) {
    char scratch[32];
    rt_unsigned_to_string(value, scratch, sizeof(scratch));
    return append_text(buffer, buffer_size, offset, scratch);
}

static int emit_line(CompilerIr *ir, const char *lhs, const char *mid, const char *rhs, const char *tail) {
    size_t offset = 0;
    char *line;

    if (ir->count >= COMPILER_MAX_IR_LINES) {
        set_error(ir, "IR instruction capacity exceeded");
        return -1;
    }

    line = ir->lines[ir->count];
    line[0] = '\0';

    if ((lhs != 0 && append_text(line, COMPILER_IR_LINE_CAPACITY, &offset, lhs) != 0) ||
        (mid != 0 && append_text(line, COMPILER_IR_LINE_CAPACITY, &offset, mid) != 0) ||
        (rhs != 0 && append_text(line, COMPILER_IR_LINE_CAPACITY, &offset, rhs) != 0) ||
        (tail != 0 && append_text(line, COMPILER_IR_LINE_CAPACITY, &offset, tail) != 0)) {
        set_error(ir, "IR instruction text exceeded line capacity");
        return -1;
    }

    ir->count += 1U;
    return 0;
}

static void format_type(const CompilerType *type, char *buffer, size_t buffer_size) {
    size_t i;
    const char *base = "int";

    if (type->base == COMPILER_BASE_VOID) {
        base = "void";
    } else if (type->base == COMPILER_BASE_CHAR) {
        base = "char";
    } else if (type->base == COMPILER_BASE_STRUCT) {
        base = "struct";
    } else if (type->base == COMPILER_BASE_UNION) {
        base = "union";
    } else if (type->base == COMPILER_BASE_ENUM) {
        base = "enum";
    }

    buffer[0] = '\0';
    if (type->is_unsigned) {
        rt_copy_string(buffer, buffer_size, "unsigned ");
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), base);
    } else {
        rt_copy_string(buffer, buffer_size, base);
    }

    if (type->is_array && rt_strlen(buffer) + 2U < buffer_size) {
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "[]");
    }

    for (i = 0; i < (size_t)type->pointer_depth && rt_strlen(buffer) + 2U < buffer_size; ++i) {
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "*");
    }
}

void compiler_ir_init(CompilerIr *ir) {
    rt_memset(ir, 0, sizeof(*ir));
}

static void ir_format_signed_value(long long value, char *buffer, size_t buffer_size) {
    if (value < 0) {
        buffer[0] = '-';
        rt_unsigned_to_string((unsigned long long)(-value), buffer + 1, buffer_size > 0 ? buffer_size - 1U : 0U);
    } else {
        rt_unsigned_to_string((unsigned long long)value, buffer, buffer_size);
    }
}

static int ir_copy_identifier(char *buffer, size_t buffer_size, const char *start, const char *end) {
    size_t length = 0;

    while (start < end && length + 1U < buffer_size) {
        buffer[length++] = *start++;
    }
    buffer[length] = '\0';
    return (start == end && length > 0U) ? 0 : -1;
}

static void ir_clear_local_values(IrOptimizerState *state) {
    state->local_value_count = 0;
}

static void ir_invalidate_local_value(IrOptimizerState *state, const char *name) {
    size_t i;
    size_t j = 0;

    for (i = 0; i < state->local_value_count; ++i) {
        if (!ir_text_equals(state->local_values[i].name, name)) {
            if (j != i) {
                state->local_values[j] = state->local_values[i];
            }
            j += 1U;
        }
    }
    state->local_value_count = j;
}

static int ir_set_tracked_value(IrTrackedValue *values, size_t *count, const char *name, long long value) {
    size_t i;

    for (i = 0; i < *count; ++i) {
        if (ir_text_equals(values[i].name, name)) {
            values[i].value = value;
            return 0;
        }
    }

    if (*count >= COMPILER_IR_TRACKED_VALUE_CAPACITY) {
        return -1;
    }

    rt_copy_string(values[*count].name, sizeof(values[*count].name), name);
    values[*count].value = value;
    *count += 1U;
    return 0;
}

static int ir_rewrite_expr_line(CompilerIr *ir, size_t index, const char *prefix, const char *expr) {
    size_t offset = 0;
    char line[COMPILER_IR_LINE_CAPACITY];

    line[0] = '\0';
    if (append_text(line, sizeof(line), &offset, prefix) != 0 ||
        append_text(line, sizeof(line), &offset, expr) != 0) {
        set_error(ir, "IR instruction text exceeded line capacity");
        return -1;
    }
    rt_copy_string(ir->lines[index], sizeof(ir->lines[index]), line);
    return 0;
}

static int ir_rewrite_jump_line(CompilerIr *ir, size_t index, const char *label) {
    return ir_rewrite_expr_line(ir, index, "jump ", label);
}

static void ir_remove_line(CompilerIr *ir, size_t index) {
    size_t i;

    if (index >= ir->count) {
        return;
    }
    for (i = index + 1U; i < ir->count; ++i) {
        memcpy(ir->lines[i - 1U], ir->lines[i], sizeof(ir->lines[i - 1U]));
    }
    if (ir->count > 0) {
        ir->count -= 1U;
    }
}

static int ir_extract_store_parts(const char *line, char *name, size_t name_size, const char **expr_out) {
    const char *cursor = line + 6;
    const char *name_end = cursor;

    while ((*name_end >= 'a' && *name_end <= 'z') ||
           (*name_end >= 'A' && *name_end <= 'Z') ||
           (*name_end >= '0' && *name_end <= '9') ||
           *name_end == '_') {
        name_end += 1;
    }
    if (ir_copy_identifier(name, name_size, cursor, name_end) != 0) {
        return -1;
    }
    if (!ir_starts_with(name_end, " <- ")) {
        return -1;
    }
    *expr_out = name_end + 4;
    return 0;
}

static int ir_extract_branch_parts(const char *line, char *label, size_t label_size, char *expr, size_t expr_size) {
    const char *expr_start = line + 8;
    const char *arrow = line + 8;

    while (*arrow != '\0') {
        if (arrow[0] == ' ' && arrow[1] == '-' && arrow[2] == '>' && arrow[3] == ' ') {
            if (ir_copy_identifier(label, label_size, arrow + 4, arrow + 4 + rt_strlen(arrow + 4)) != 0) {
                return -1;
            }
            if (ir_copy_identifier(expr, expr_size, expr_start, arrow) != 0) {
                return -1;
            }
            return 0;
        }
        arrow += 1;
    }

    return -1;
}

static int ir_extract_const_parts(const char *line, char *name, size_t name_size, const char **expr_out) {
    const char *cursor = line + 6;
    const char *name_end = cursor;

    while ((*name_end >= 'a' && *name_end <= 'z') ||
           (*name_end >= 'A' && *name_end <= 'Z') ||
           (*name_end >= '0' && *name_end <= '9') ||
           *name_end == '_') {
        name_end += 1;
    }
    if (ir_copy_identifier(name, name_size, cursor, name_end) != 0) {
        return -1;
    }
    if (!ir_starts_with(name_end, " = ")) {
        return -1;
    }
    *expr_out = name_end + 3;
    return 0;
}

static int ir_extract_assignment_parts(const char *expr, char *name, size_t name_size, char *op, size_t op_size, const char **rhs_out) {
    IrConstParser parser;

    parser.cursor = expr != 0 ? expr : "";
    parser.state = 0;
    ir_const_next(&parser);
    if (parser.current.kind != IR_CONST_TOKEN_IDENTIFIER) {
        return -1;
    }
    rt_copy_string(name, name_size, parser.current.text);
    ir_const_next(&parser);
    if (parser.current.kind != IR_CONST_TOKEN_PUNCT ||
        !(ir_text_equals(parser.current.text, "=") ||
          ir_text_equals(parser.current.text, "+=") ||
          ir_text_equals(parser.current.text, "-=") ||
          ir_text_equals(parser.current.text, "*=") ||
          ir_text_equals(parser.current.text, "/=") ||
          ir_text_equals(parser.current.text, "%=") ||
          ir_text_equals(parser.current.text, "<<=") ||
          ir_text_equals(parser.current.text, ">>=") ||
          ir_text_equals(parser.current.text, "&=") ||
          ir_text_equals(parser.current.text, "^=") ||
          ir_text_equals(parser.current.text, "|="))) {
        return -1;
    }
    rt_copy_string(op, op_size, parser.current.text);
    *rhs_out = ir_skip_spaces(parser.cursor);
    return **rhs_out == '\0' ? -1 : 0;
}

int compiler_ir_optimize(CompilerIr *ir) {
    IrOptimizerState state;
    size_t i = 0;

    rt_memset(&state, 0, sizeof(state));

    while (i < ir->count) {
        char *line = ir->lines[i];

        if (ir_starts_with(line, "func ") || ir_starts_with(line, "endfunc ") ||
            ir_starts_with(line, "label ") || ir_starts_with(line, "jump ") ||
            ir_starts_with(line, "switch ") || ir_starts_with(line, "case ") ||
            ir_text_equals(line, "default") || ir_starts_with(line, "endswitch")) {
            ir_clear_local_values(&state);
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "const ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            const char *expr = 0;
            long long value = 0;

            if (ir_extract_const_parts(line, name, sizeof(name), &expr) == 0 &&
                ir_evaluate_constant_expression(expr, &state, &value) == 0 &&
                ir_set_tracked_value(state.global_constants, &state.global_constant_count, name, value) == 0) {
                char number[32];
                char rewritten[COMPILER_IR_LINE_CAPACITY];
                size_t offset = 0;

                ir_format_signed_value(value, number, sizeof(number));
                rewritten[0] = '\0';
                if (append_text(rewritten, sizeof(rewritten), &offset, "const ") != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, name) != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, " = ") != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, number) != 0) {
                    set_error(ir, "IR instruction text exceeded line capacity");
                    return -1;
                }
                rt_copy_string(line, sizeof(ir->lines[i]), rewritten);
            }
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "decl ")) {
            const char *cursor = line + rt_strlen(line);
            while (cursor > line && cursor[-1] != ' ') {
                cursor -= 1;
            }
            if (*cursor != '\0') {
                ir_invalidate_local_value(&state, cursor);
            }
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "store ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            const char *expr = 0;
            long long value = 0;
            int parsed_store = ir_extract_store_parts(line, name, sizeof(name), &expr) == 0;

            if (parsed_store && ir_evaluate_constant_expression(expr, &state, &value) == 0) {
                char number[32];
                char rewritten[COMPILER_IR_LINE_CAPACITY];
                size_t offset = 0;

                ir_format_signed_value(value, number, sizeof(number));
                rewritten[0] = '\0';
                if (append_text(rewritten, sizeof(rewritten), &offset, "store ") != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, name) != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, " <- ") != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, number) != 0) {
                    set_error(ir, "IR instruction text exceeded line capacity");
                    return -1;
                }
                rt_copy_string(line, sizeof(ir->lines[i]), rewritten);
                if (ir_set_tracked_value(state.local_values, &state.local_value_count, name, value) != 0) {
                    set_error(ir, "IR optimizer tracking capacity exceeded");
                    return -1;
                }
            } else if (parsed_store) {
                ir_invalidate_local_value(&state, name);
            }
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "eval ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            char op[4];
            const char *expr = line + 5;
            const char *rhs = 0;
            long long value = 0;

            if (ir_extract_assignment_parts(expr, name, sizeof(name), op, sizeof(op), &rhs) == 0) {
                if (ir_text_equals(op, "=")) {
                    if (ir_evaluate_constant_expression(rhs, &state, &value) == 0) {
                        char number[32];
                        char rewritten[COMPILER_IR_LINE_CAPACITY];
                        size_t offset = 0;

                        ir_format_signed_value(value, number, sizeof(number));
                        rewritten[0] = '\0';
                        if (append_text(rewritten, sizeof(rewritten), &offset, "store ") != 0 ||
                            append_text(rewritten, sizeof(rewritten), &offset, name) != 0 ||
                            append_text(rewritten, sizeof(rewritten), &offset, " <- ") != 0 ||
                            append_text(rewritten, sizeof(rewritten), &offset, number) != 0) {
                            set_error(ir, "IR instruction text exceeded line capacity");
                            return -1;
                        }
                        rt_copy_string(line, sizeof(ir->lines[i]), rewritten);
                        if (ir_set_tracked_value(state.local_values, &state.local_value_count, name, value) != 0) {
                            set_error(ir, "IR optimizer tracking capacity exceeded");
                            return -1;
                        }
                        i += 1U;
                        continue;
                    }
                } else {
                    long long lhs = 0;
                    long long rhs_value = 0;
                    const char *binary_op = 0;
                    if (ir_lookup_value(&state, name, &lhs) == 0 &&
                        ir_evaluate_constant_expression(rhs, &state, &rhs_value) == 0) {
                        if (ir_text_equals(op, "+=")) binary_op = "+";
                        else if (ir_text_equals(op, "-=")) binary_op = "-";
                        else if (ir_text_equals(op, "*=")) binary_op = "*";
                        else if (ir_text_equals(op, "/=")) binary_op = "/";
                        else if (ir_text_equals(op, "%=")) binary_op = "%";
                        else if (ir_text_equals(op, "<<=")) binary_op = "<<";
                        else if (ir_text_equals(op, ">>=")) binary_op = ">>";
                        else if (ir_text_equals(op, "&=")) binary_op = "&";
                        else if (ir_text_equals(op, "^=")) binary_op = "^";
                        else if (ir_text_equals(op, "|=")) binary_op = "|";
                        if (binary_op != 0 && ir_apply_binary_op(binary_op, lhs, rhs_value, &value) == 0) {
                            char number[32];
                            char rewritten[COMPILER_IR_LINE_CAPACITY];
                            size_t offset = 0;

                            ir_format_signed_value(value, number, sizeof(number));
                            rewritten[0] = '\0';
                            if (append_text(rewritten, sizeof(rewritten), &offset, "store ") != 0 ||
                                append_text(rewritten, sizeof(rewritten), &offset, name) != 0 ||
                                append_text(rewritten, sizeof(rewritten), &offset, " <- ") != 0 ||
                                append_text(rewritten, sizeof(rewritten), &offset, number) != 0) {
                                set_error(ir, "IR instruction text exceeded line capacity");
                                return -1;
                            }
                            rt_copy_string(line, sizeof(ir->lines[i]), rewritten);
                            if (ir_set_tracked_value(state.local_values, &state.local_value_count, name, value) != 0) {
                                set_error(ir, "IR optimizer tracking capacity exceeded");
                                return -1;
                            }
                            i += 1U;
                            continue;
                        }
                    }
                }
                ir_invalidate_local_value(&state, name);
                i += 1U;
                continue;
            }

            if (ir_evaluate_constant_expression(expr, &state, &value) == 0) {
                char number[32];
                ir_format_signed_value(value, number, sizeof(number));
                if (ir_rewrite_expr_line(ir, i, "eval ", number) != 0) {
                    return -1;
                }
            } else {
                ir_clear_local_values(&state);
            }
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "ret ")) {
            long long value = 0;
            if (ir_evaluate_constant_expression(line + 4, &state, &value) == 0) {
                char number[32];
                ir_format_signed_value(value, number, sizeof(number));
                if (ir_rewrite_expr_line(ir, i, "ret ", number) != 0) {
                    return -1;
                }
            }
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "brfalse ")) {
            char label[COMPILER_IR_NAME_CAPACITY];
            char expr[COMPILER_IR_LINE_CAPACITY];
            long long value = 0;

            if (ir_extract_branch_parts(line, label, sizeof(label), expr, sizeof(expr)) == 0 &&
                ir_evaluate_constant_expression(expr, &state, &value) == 0) {
                if (value == 0) {
                    if (ir_rewrite_jump_line(ir, i, label) != 0) {
                        return -1;
                    }
                    ir_clear_local_values(&state);
                    i += 1U;
                    continue;
                }
                ir_remove_line(ir, i);
                continue;
            }

            ir_clear_local_values(&state);
            i += 1U;
            continue;
        }

        i += 1U;
    }

    return 0;
}

int compiler_ir_make_label(CompilerIr *ir, const char *prefix, char *buffer, size_t buffer_size) {
    size_t offset = 0;

    if (append_text(buffer, buffer_size, &offset, prefix != 0 ? prefix : "L") != 0 ||
        append_uint(buffer, buffer_size, &offset, (unsigned long long)ir->label_counter) != 0) {
        set_error(ir, "failed to format IR label");
        return -1;
    }

    ir->label_counter += 1U;
    return 0;
}

int compiler_ir_emit_function_begin(CompilerIr *ir, const char *name, const CompilerType *type) {
    char type_text[64];
    format_type(type, type_text, sizeof(type_text));
    return emit_line(ir, "func ", name, " : ", type_text);
}

int compiler_ir_emit_function_end(CompilerIr *ir, const char *name) {
    return emit_line(ir, "endfunc ", name, 0, 0);
}

int compiler_ir_emit_constant(CompilerIr *ir, const char *name, long long value) {
    char number_text[32];

    if (value < 0) {
        number_text[0] = '-';
        rt_unsigned_to_string((unsigned long long)(-value), number_text + 1, sizeof(number_text) - 1);
    } else {
        rt_unsigned_to_string((unsigned long long)value, number_text, sizeof(number_text));
    }

    return emit_line(ir, "const ", name, " = ", number_text);
}

int compiler_ir_emit_decl(CompilerIr *ir, const char *storage, int is_function, const CompilerType *type, const char *name) {
    char prefix[128];
    char type_text[64];
    size_t offset = 0;

    format_type(type, type_text, sizeof(type_text));
    if (append_text(prefix, sizeof(prefix), &offset, "decl ") != 0 ||
        append_text(prefix, sizeof(prefix), &offset, storage) != 0 ||
        append_text(prefix, sizeof(prefix), &offset, " ") != 0 ||
        append_text(prefix, sizeof(prefix), &offset, is_function ? "func" : "obj") != 0 ||
        append_text(prefix, sizeof(prefix), &offset, " ") != 0 ||
        append_text(prefix, sizeof(prefix), &offset, type_text) != 0 ||
        append_text(prefix, sizeof(prefix), &offset, " ") != 0) {
        set_error(ir, "failed to format declaration IR");
        return -1;
    }

    return emit_line(ir, prefix, name, 0, 0);
}

int compiler_ir_emit_assign(CompilerIr *ir, const char *name, const char *expr) {
    return emit_line(ir, "store ", name, " <- ", expr);
}

int compiler_ir_emit_eval(CompilerIr *ir, const char *expr) {
    return emit_line(ir, "eval ", expr, 0, 0);
}

int compiler_ir_emit_return(CompilerIr *ir, const char *expr) {
    if (expr == 0 || expr[0] == '\0') {
        return emit_line(ir, "ret", 0, 0, 0);
    }
    return emit_line(ir, "ret ", expr, 0, 0);
}

int compiler_ir_emit_branch_zero(CompilerIr *ir, const char *expr, const char *label) {
    return emit_line(ir, "brfalse ", expr, " -> ", label);
}

int compiler_ir_emit_jump(CompilerIr *ir, const char *label) {
    return emit_line(ir, "jump ", label, 0, 0);
}

int compiler_ir_emit_label(CompilerIr *ir, const char *label) {
    return emit_line(ir, "label ", label, 0, 0);
}

int compiler_ir_emit_case(CompilerIr *ir, const char *expr) {
    return emit_line(ir, "case ", expr, 0, 0);
}

int compiler_ir_emit_default(CompilerIr *ir) {
    return emit_line(ir, "default", 0, 0, 0);
}

int compiler_ir_emit_note(CompilerIr *ir, const char *keyword, const char *detail) {
    if (detail == 0 || detail[0] == '\0') {
        return emit_line(ir, keyword, 0, 0, 0);
    }
    return emit_line(ir, keyword, " ", detail, 0);
}

int compiler_ir_write_dump(const CompilerIr *ir, int fd) {
    size_t i;

    for (i = 0; i < ir->count; ++i) {
        if (rt_write_line(fd, ir->lines[i]) != 0) {
            return -1;
        }
    }

    return 0;
}

const char *compiler_ir_error_message(const CompilerIr *ir) {
    return ir->error_message;
}
