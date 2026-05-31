#include "ir_internal.h"

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
    int base = 10;

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
    } else {
        if (text[0] == '0' && text[1] >= '0' && text[1] <= '7') {
            base = 8;
        }
        if (ir_parse_unsigned_value(text, base, &magnitude) != 0) {
            return -1;
        }
    }

    *value_out = negative ? -(long long)magnitude : (long long)magnitude;
    return 0;
}

void ir_const_next(IrConstParser *parser) {
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
            else if (*cursor == 'v') ch = '\v';
            else if (*cursor == 'f') ch = '\f';
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

int ir_lookup_value(const IrOptimizerState *state, const char *name, long long *value_out) {
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

int ir_apply_binary_op(const char *op, long long lhs, long long rhs, long long *value_out) {
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
        *value_out = (long long)(((unsigned long long)lhs) >> (unsigned int)rhs);
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

int ir_evaluate_constant_expression(const char *expr, const IrOptimizerState *state, long long *value_out) {
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

