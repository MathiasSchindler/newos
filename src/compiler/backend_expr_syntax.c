/* Expression tokenization and syntax classification helpers. */

#include "backend_internal.h"

static int expr_read_punctuator_width(const char *cursor) {
    if ((cursor[0] == '<' && cursor[1] == '<' && cursor[2] == '=') ||
        (cursor[0] == '>' && cursor[1] == '>' && cursor[2] == '=')) {
        return 3;
    }
    if (cursor[0] == '&' && cursor[1] == '&') {
        return 2;
    }
    if (cursor[0] == '|' && cursor[1] == '|') {
        return 2;
    }
    if (cursor[0] == '=' && cursor[1] == '=') {
        return 2;
    }
    if (cursor[0] == '!' && cursor[1] == '=') {
        return 2;
    }
    if (cursor[0] == '<' && cursor[1] == '=') {
        return 2;
    }
    if (cursor[0] == '>' && cursor[1] == '=') {
        return 2;
    }
    if (cursor[0] == '<' && cursor[1] == '<') {
        return 2;
    }
    if (cursor[0] == '>' && cursor[1] == '>') {
        return 2;
    }
    if (cursor[0] == '&' && cursor[1] == '=') {
        return 2;
    }
    if (cursor[0] == '|' && cursor[1] == '=') {
        return 2;
    }
    if (cursor[0] == '^' && cursor[1] == '=') {
        return 2;
    }
    if (cursor[0] == '+' && cursor[1] == '=') {
        return 2;
    }
    if (cursor[0] == '-' && cursor[1] == '=') {
        return 2;
    }
    if (cursor[0] == '*' && cursor[1] == '=') {
        return 2;
    }
    if (cursor[0] == '/' && cursor[1] == '=') {
        return 2;
    }
    if (cursor[0] == '%' && cursor[1] == '=') {
        return 2;
    }
    if (cursor[0] == '+' && cursor[1] == '+') {
        return 2;
    }
    if (cursor[0] == '-' && cursor[1] == '-') {
        return 2;
    }
    if (cursor[0] == '-' && cursor[1] == '>') {
        return 2;
    }
    return 0;
}

void expr_next(ExprParser *parser) {
    const char *cursor = skip_spaces(parser->cursor);
    size_t length = 0;

    parser->cursor = cursor;
    parser->current.text[0] = '\0';
    parser->current.number_value = 0;
    parser->current.number_is_unsigned = 0;
    parser->current.text_length = 0U;

    if (*cursor == '\0') {
        parser->current.kind = EXPR_TOKEN_EOF;
        return;
    }

    if ((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') || *cursor == '_') {
        parser->current.kind = EXPR_TOKEN_IDENTIFIER;
        while (((*cursor >= 'a' && *cursor <= 'z') ||
                (*cursor >= 'A' && *cursor <= 'Z') ||
                (*cursor >= '0' && *cursor <= '9') ||
                *cursor == '_') &&
               length + 1 < sizeof(parser->current.text)) {
            parser->current.text[length++] = *cursor++;
        }
        parser->current.text[length] = '\0';
        parser->cursor = cursor;
        return;
    }

    if (*cursor >= '0' && *cursor <= '9') {
        parser->current.kind = EXPR_TOKEN_NUMBER;
        if (*cursor == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
            if (length + 2 < sizeof(parser->current.text)) {
                parser->current.text[length++] = *cursor++;
                parser->current.text[length++] = *cursor++;
            } else {
                cursor += 2;
            }
            while (((*cursor >= '0' && *cursor <= '9') ||
                    (*cursor >= 'a' && *cursor <= 'f') ||
                    (*cursor >= 'A' && *cursor <= 'F')) &&
                   length + 1 < sizeof(parser->current.text)) {
                parser->current.text[length++] = *cursor++;
            }
        } else {
            while (*cursor >= '0' && *cursor <= '9' && length + 1 < sizeof(parser->current.text)) {
                parser->current.text[length++] = *cursor++;
            }
            if (*cursor == '.') {
                cursor += 1;
                while (*cursor >= '0' && *cursor <= '9') {
                    cursor += 1;
                }
            }
            if (*cursor == 'e' || *cursor == 'E') {
                cursor += 1;
                if (*cursor == '+' || *cursor == '-') {
                    cursor += 1;
                }
                while (*cursor >= '0' && *cursor <= '9') {
                    cursor += 1;
                }
            }
        }
        parser->current.text[length] = '\0';
        while (*cursor == 'u' || *cursor == 'U' || *cursor == 'l' || *cursor == 'L' ||
               *cursor == 'f' || *cursor == 'F') {
            if (*cursor == 'u' || *cursor == 'U') {
                parser->current.number_is_unsigned = 1;
            }
            if (length + 1 < sizeof(parser->current.text)) {
                parser->current.text[length++] = *cursor;
                parser->current.text[length] = '\0';
            }
            cursor += 1;
        }
        (void)parse_signed_value(parser->current.text, &parser->current.number_value);
        parser->cursor = cursor;
        return;
    }

    if (*cursor == '\'') {
        char ch = 0;
        parser->current.kind = EXPR_TOKEN_CHAR;
        cursor += 1;
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor += 1;
            ch = backend_decode_escaped_char(&cursor);
        } else {
            ch = *cursor;
            if (*cursor != '\0') {
                cursor += 1;
            }
        }
        parser->current.number_value = (long long)(unsigned char)ch;
        while (*cursor != '\0' && *cursor != '\'') {
            cursor += 1;
        }
        if (*cursor == '\'') {
            cursor += 1;
        }
        parser->cursor = cursor;
        return;
    }

    if (*cursor == '"') {
        parser->current.kind = EXPR_TOKEN_STRING;
        do {
            cursor += 1;
            while (*cursor != '\0' && *cursor != '"' && length + 1 < sizeof(parser->current.text)) {
                if (*cursor == '\\' && cursor[1] != '\0') {
                    cursor += 1;
                    parser->current.text[length++] = backend_decode_escaped_char(&cursor);
                    continue;
                }
                parser->current.text[length++] = *cursor++;
            }
            if (*cursor == '"') {
                cursor += 1;
            }
            cursor = skip_spaces(cursor);
        } while (*cursor == '"');
        parser->current.text[length] = '\0';
        parser->current.text_length = length;
        parser->cursor = cursor;
        return;
    }

    parser->current.kind = EXPR_TOKEN_PUNCT;
    {
        int punctuator_width = expr_read_punctuator_width(cursor);
        if (punctuator_width == 3) {
            parser->current.text[0] = cursor[0];
            parser->current.text[1] = cursor[1];
            parser->current.text[2] = cursor[2];
            parser->current.text[3] = '\0';
            parser->cursor = cursor + 3;
            return;
        }
        if (punctuator_width == 2) {
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

int expr_match_punct(ExprParser *parser, const char *text) {
    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, text)) {
        expr_next(parser);
        return 1;
    }
    return 0;
}

int expr_expect_punct(ExprParser *parser, const char *text) {
    if (!expr_match_punct(parser, text)) {
        char message[96];

        rt_copy_string(message, sizeof(message), "expected `");
        rt_copy_string(message + rt_strlen(message), sizeof(message) - rt_strlen(message), text);
        rt_copy_string(message + rt_strlen(message), sizeof(message) - rt_strlen(message), "` in backend");
        backend_set_error_with_line(parser->state->backend, message, parser->current.text);
        return -1;
    }
    return 0;
}

int expr_is_assignment_operator_text(const char *text) {
    if (names_equal(text, "=")) {
        return 1;
    }
    if (names_equal(text, "&=")) {
        return 1;
    }
    if (names_equal(text, "|=")) {
        return 1;
    }
    if (names_equal(text, "^=")) {
        return 1;
    }
    if (names_equal(text, "<<=")) {
        return 1;
    }
    if (names_equal(text, ">>=")) {
        return 1;
    }
    if (names_equal(text, "+=")) {
        return 1;
    }
    if (names_equal(text, "-=")) {
        return 1;
    }
    if (names_equal(text, "*=")) {
        return 1;
    }
    if (names_equal(text, "/=")) {
        return 1;
    }
    if (names_equal(text, "%=")) {
        return 1;
    }
    return 0;
}

int expr_is_assignment_stop_text(const char *text) {
    if (names_equal(text, ",")) {
        return 1;
    }
    if (names_equal(text, ":")) {
        return 1;
    }
    if (names_equal(text, "?")) {
        return 1;
    }
    return 0;
}

const char *expr_binary_op_for_assignment(const char *op) {
    if (names_equal(op, "+=")) {
        return "+";
    }
    if (names_equal(op, "&=")) {
        return "&";
    }
    if (names_equal(op, "|=")) {
        return "|";
    }
    if (names_equal(op, "^=")) {
        return "^";
    }
    if (names_equal(op, "<<=")) {
        return "<<";
    }
    if (names_equal(op, ">>=")) {
        return ">>";
    }
    if (names_equal(op, "-=")) {
        return "-";
    }
    if (names_equal(op, "*=")) {
        return "*";
    }
    if (names_equal(op, "/=")) {
        return "/";
    }
    return "%";
}

int expr_is_incdec_text(const char *text) {
    return names_equal(text, "++") || names_equal(text, "--");
}

int expr_is_index_or_arrow_text(const char *text) {
    return names_equal(text, "[") || names_equal(text, "->");
}

int expr_is_unary_prefix_text(const char *text) {
    if (expr_is_incdec_text(text)) {
        return 1;
    }
    if (names_equal(text, "-")) {
        return 1;
    }
    if (names_equal(text, "!")) {
        return 1;
    }
    if (names_equal(text, "~")) {
        return 1;
    }
    if (names_equal(text, "&")) {
        return 1;
    }
    if (names_equal(text, "*")) {
        return 1;
    }
    if (names_equal(text, "+")) {
        return 1;
    }
    return 0;
}
