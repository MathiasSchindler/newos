/* Expression lowering and initializer emission helpers. */

#include "backend_internal.h"

static int expr_match_punct(ExprParser *parser, const char *text);
static int expr_parse_postfix_suffixes(ExprParser *parser, int word_index, int current_is_address, int load_final_address);
static int expr_read_punctuator_width(const char *cursor);
static int emit_binary_op(BackendState *state, const char *op);
static int expr_parse_expression(ExprParser *parser);
static int expr_parse_assignment(ExprParser *parser);
static int expr_parse_lvalue_address(ExprParser *parser, int *byte_sized);
static int expr_parse_lvalue_suffixes(ExprParser *parser, int *byte_sized, int word_index);
static int expr_parse_unary(ExprParser *parser);
static int expr_parse_multiplicative(ExprParser *parser);
static int expr_parse_additive(ExprParser *parser);
static int expr_parse_shift(ExprParser *parser);
static int expr_parse_relational(ExprParser *parser);
static int expr_parse_equality(ExprParser *parser);
static int expr_parse_bitand(ExprParser *parser);
static int expr_parse_bitxor(ExprParser *parser);
static int expr_parse_bitor(ExprParser *parser);

static void expr_next(ExprParser *parser) {
    const char *cursor = skip_spaces(parser->cursor);
    size_t length = 0;

    parser->cursor = cursor;
    parser->current.text[0] = '\0';
    parser->current.number_value = 0;

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
                    if (*cursor == 'n') parser->current.text[length++] = '\n';
                    else if (*cursor == 't') parser->current.text[length++] = '\t';
                    else if (*cursor == 'r') parser->current.text[length++] = '\r';
                    else if (*cursor == '0') parser->current.text[length++] = '\0';
                    else parser->current.text[length++] = *cursor;
                    cursor += 1;
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

static int expr_match_punct(ExprParser *parser, const char *text) {
    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, text)) {
        expr_next(parser);
        return 1;
    }
    return 0;
}

static int expr_expect_punct(ExprParser *parser, const char *text) {
    if (!expr_match_punct(parser, text)) {
        backend_set_error(parser->state->backend, "unsupported expression syntax in backend");
        return -1;
    }
    return 0;
}

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

static int is_assignment_operator_text(const char *text) {
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

static int is_assignment_stop_text(const char *text) {
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

static const char *binary_op_for_assignment(const char *op) {
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

static int is_incdec_text(const char *text) {
    return names_equal(text, "++") || names_equal(text, "--");
}

static int is_index_or_arrow_text(const char *text) {
    return names_equal(text, "[") || names_equal(text, "->");
}

static int is_unary_prefix_text(const char *text) {
    if (is_incdec_text(text)) {
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

static int name_prefers_word_index(const BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0) {
        return state->locals[local_index].prefers_word_index;
    }
    if (global_index >= 0) {
        return state->globals[global_index].prefers_word_index;
    }
    return names_equal(name, "argv") || names_equal(name, "envp");
}

static int identifier_looks_like_type(const char *name) {
    size_t length = rt_strlen(name);

    if ((name[0] >= 'A' && name[0] <= 'Z') && !name_looks_like_macro_constant(name)) {
        return 1;
    }
    if (names_equal(name, "void") || names_equal(name, "char") || names_equal(name, "short") ||
        names_equal(name, "int") || names_equal(name, "long") || names_equal(name, "signed") ||
        names_equal(name, "unsigned") || names_equal(name, "float") || names_equal(name, "double") ||
        names_equal(name, "const") || names_equal(name, "volatile") ||
        names_equal(name, "struct") || names_equal(name, "union") || names_equal(name, "enum") ||
        names_equal(name, "size_t") || names_equal(name, "usize") || names_equal(name, "__int128")) {
        return 1;
    }
    if (length > 2 && name[length - 2] == '_' && name[length - 1] == 't') {
        return 1;
    }
    if ((name[0] == 'u' || name[0] == 'i') && name[1] != '\0') {
        size_t i = 1;
        while (name[i] >= '0' && name[i] <= '9') {
            i += 1U;
        }
        if (name[i] == '\0' && i > 1U) {
            return 1;
        }
    }
    return 0;
}

static int member_prefers_word_index(const char *name) {
    if (names_equal(name, "argv") || names_equal(name, "envp") || names_equal(name, "commands") ||
        names_equal(name, "jobs") || names_equal(name, "aliases") || names_equal(name, "functions") ||
        names_equal(name, "entries") || names_equal(name, "fields")) {
        return 1;
    }
    return 0;
}

static int member_decays_to_address(const char *name) {
    if (names_equal(name, "name") || names_equal(name, "path") || names_equal(name, "self_dir") ||
        names_equal(name, "text") || names_equal(name, "pattern") || names_equal(name, "pattern_text") ||
        names_equal(name, "buffer") || names_equal(name, "line") || names_equal(name, "data") ||
        names_equal(name, "value") || names_equal(name, "body") || names_equal(name, "argv") ||
        names_equal(name, "envp")) {
        return 1;
    }
    return 0;
}

static int expr_looks_like_compound_literal(ExprParser *parser) {
    ExprParser snapshot = *parser;
    int saw_typeish = 0;
    int saw_token = 0;
    int nested_parens = 0;
    int expect_tag_name = 0;

    if (snapshot.current.kind != EXPR_TOKEN_PUNCT || !names_equal(snapshot.current.text, "(")) {
        return 0;
    }
    expr_next(&snapshot);
    while (snapshot.current.kind != EXPR_TOKEN_EOF) {
        if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, ")")) {
            if (nested_parens > 0) {
                nested_parens -= 1;
                saw_token = 1;
                expr_next(&snapshot);
                continue;
            }
            expr_next(&snapshot);
            return saw_token && saw_typeish &&
                   snapshot.current.kind == EXPR_TOKEN_PUNCT &&
                   names_equal(snapshot.current.text, "{");
        }
        if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
            if (identifier_looks_like_type(snapshot.current.text) || expect_tag_name) {
                saw_typeish = 1;
                expect_tag_name = names_equal(snapshot.current.text, "struct") ||
                                  names_equal(snapshot.current.text, "union") ||
                                  names_equal(snapshot.current.text, "enum");
            } else {
                return 0;
            }
            saw_token = 1;
        } else if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "(")) {
            nested_parens += 1;
            saw_token = 1;
        } else if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "*")) {
            if (!saw_typeish) {
                return 0;
            }
            saw_token = 1;
        } else {
            return 0;
        }
        expr_next(&snapshot);
    }
    return 0;
}

static int expr_looks_like_cast(ExprParser *parser) {
    ExprParser snapshot = *parser;
    int saw_typeish = 0;
    int saw_token = 0;
    int nested_parens = 0;
    int expect_tag_name = 0;

    if (snapshot.current.kind != EXPR_TOKEN_PUNCT || !names_equal(snapshot.current.text, "(")) {
        return 0;
    }
    expr_next(&snapshot);
    while (snapshot.current.kind != EXPR_TOKEN_EOF) {
        if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, ")")) {
            if (nested_parens > 0) {
                nested_parens -= 1;
                saw_token = 1;
                expr_next(&snapshot);
                continue;
            }
            expr_next(&snapshot);
            if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "{")) {
                return 0;
            }
            return saw_token && saw_typeish;
        }
        if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
            if (identifier_looks_like_type(snapshot.current.text) || expect_tag_name) {
                saw_typeish = 1;
                expect_tag_name = names_equal(snapshot.current.text, "struct") ||
                                  names_equal(snapshot.current.text, "union") ||
                                  names_equal(snapshot.current.text, "enum");
            } else {
                return 0;
            }
            saw_token = 1;
        } else if (snapshot.current.kind == EXPR_TOKEN_PUNCT &&
                   names_equal(snapshot.current.text, "(")) {
            nested_parens += 1;
            saw_token = 1;
        } else if (snapshot.current.kind == EXPR_TOKEN_PUNCT &&
                   names_equal(snapshot.current.text, "*")) {
            if (!saw_typeish) {
                return 0;
            }
            saw_token = 1;
        } else {
            return 0;
        }
        expr_next(&snapshot);
    }
    return 0;
}

static long long guess_identifier_size(const BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0) {
        return (long long)state->locals[local_index].stack_bytes;
    }
    if (global_index >= 0 && state->globals[global_index].is_array) {
        return BACKEND_ARRAY_STACK_BYTES;
    }
    return 8;
}

static int emit_index_address(BackendState *state, int word_index) {
    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "mov x2, x0") != 0 ||
            emit_instruction(state, "ldr x1, [sp]") != 0 ||
            emit_instruction(state, "add sp, sp, #16") != 0) {
            return -1;
        }
        if (word_index && emit_instruction(state, "lsl x2, x2, #3") != 0) {
            return -1;
        }
        return emit_instruction(state, "add x0, x1, x2");
    }

    if (emit_instruction(state, "movq %rax, %rcx") != 0 || emit_instruction(state, "popq %rax") != 0) {
        return -1;
    }
    if (word_index) {
        return emit_instruction(state, "leaq (%rax,%rcx,8), %rax");
    }
    return emit_instruction(state, "addq %rcx, %rax");
}

static int emit_identifier_incdec(BackendState *state, const char *name, int delta, int return_old) {
    const char *result_register = backend_is_aarch64(state) ? "x0" : "%rax";
    const char *op = delta > 0 ? "+" : "-";

    if (emit_load_name(state, name) != 0) {
        return -1;
    }
    if (return_old && emit_push_value(state) != 0) {
        return -1;
    }
    if (emit_push_value(state) != 0) {
        return -1;
    }
    if (emit_load_immediate(state, 1) != 0) {
        return -1;
    }
    if (emit_binary_op(state, op) != 0) {
        return -1;
    }
    if (emit_store_name(state, name) != 0) {
        return -1;
    }
    if (return_old) {
        return emit_pop_to_register(state, result_register);
    }
    return 0;
}

static int emit_address_incdec(BackendState *state, int byte_sized, int delta, int return_old) {
    const char *result_register = backend_is_aarch64(state) ? "x0" : "%rax";
    const char *op = delta > 0 ? "+" : "-";

    if (emit_push_value(state) != 0) {
        return -1;
    }
    if (emit_load_from_address_register(state, result_register, byte_sized) != 0) {
        return -1;
    }
    if (return_old && emit_push_value(state) != 0) {
        return -1;
    }
    if (emit_push_value(state) != 0) {
        return -1;
    }
    if (emit_load_immediate(state, 1) != 0) {
        return -1;
    }
    if (emit_binary_op(state, op) != 0) {
        return -1;
    }
    if (emit_pop_address_and_store(state, byte_sized) != 0) {
        return -1;
    }
    if (return_old) {
        return emit_pop_to_register(state, result_register);
    }
    return 0;
}

static int expr_group_has_postfix_incdec(ExprParser *parser) {
    ExprParser snapshot = *parser;
    int depth = 0;

    while (snapshot.current.kind != EXPR_TOKEN_EOF) {
        if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "(")) {
            depth += 1;
        } else if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, ")")) {
            if (depth == 0) {
                expr_next(&snapshot);
                return snapshot.current.kind == EXPR_TOKEN_PUNCT &&
                       (names_equal(snapshot.current.text, "++") || names_equal(snapshot.current.text, "--"));
            }
            depth -= 1;
        }
        expr_next(&snapshot);
    }
    return 0;
}

static int expr_parse_lvalue_suffixes(ExprParser *parser, int *byte_sized, int word_index) {
    while (parser->current.kind == EXPR_TOKEN_PUNCT) {
        if (names_equal(parser->current.text, "[")) {
            expr_next(parser);
            if (emit_push_value(parser->state) != 0 ||
                expr_parse_expression(parser) != 0 ||
                expr_expect_punct(parser, "]") != 0 ||
                emit_index_address(parser->state, word_index) != 0) {
                return -1;
            }
            *byte_sized = word_index ? 0 : 1;
            word_index = 0;
            continue;
        }
        if (names_equal(parser->current.text, ".") || names_equal(parser->current.text, "->")) {
            expr_next(parser);
            if (parser->current.kind != EXPR_TOKEN_IDENTIFIER) {
                backend_set_error(parser->state->backend, "unsupported assignment target in backend");
                return -1;
            }
            word_index = member_prefers_word_index(parser->current.text);
            *byte_sized = word_index ? 0 : 1;
            expr_next(parser);
            continue;
        }
        break;
    }
    return 0;
}

static int expr_parse_postfix_suffixes(ExprParser *parser, int word_index, int current_is_address, int load_final_address) {
    int byte_sized = word_index ? 0 : 1;

    for (;;) {
        if (expr_match_punct(parser, "[")) {
            if (emit_push_value(parser->state) != 0) {
                return -1;
            }
            if (expr_parse_expression(parser) != 0) {
                return -1;
            }
            if (expr_expect_punct(parser, "]") != 0) {
                return -1;
            }
            if (emit_index_address(parser->state, word_index) != 0) {
                return -1;
            }
            current_is_address = 1;
            load_final_address = 1;
            byte_sized = word_index ? 0 : 1;
            word_index = 0;

            if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                is_index_or_arrow_text(parser->current.text)) {
                if (emit_load_from_address_register(parser->state, backend_is_aarch64(parser->state) ? "x0" : "%rax",
                                                    byte_sized) != 0) {
                    return -1;
                }
                current_is_address = 0;
            }
            continue;
        }

        if (parser->current.kind == EXPR_TOKEN_PUNCT &&
            (names_equal(parser->current.text, ".") || names_equal(parser->current.text, "->"))) {
            expr_next(parser);
            if (parser->current.kind != EXPR_TOKEN_IDENTIFIER) {
                backend_set_error(parser->state->backend, "unsupported expression syntax in backend");
                return -1;
            }
            word_index = member_prefers_word_index(parser->current.text);
            byte_sized = word_index ? 0 : 1;
            load_final_address = member_decays_to_address(parser->current.text) ? 0 : 1;
            current_is_address = 1;
            expr_next(parser);
            continue;
        }
        break;
    }

    if (parser->current.kind == EXPR_TOKEN_PUNCT && is_incdec_text(parser->current.text)) {
        int delta;

        if (!current_is_address) {
            backend_set_error(parser->state->backend, "unsupported expression syntax in backend");
            return -1;
        }

        delta = names_equal(parser->current.text, "++") ? 1 : -1;
        expr_next(parser);
        return emit_address_incdec(parser->state, byte_sized, delta, 1);
    }

    if (current_is_address && load_final_address) {
        return emit_load_from_address_register(parser->state, backend_is_aarch64(parser->state) ? "x0" : "%rax",
                                               byte_sized);
    }
    return 0;
}

static int expr_parse_sizeof(ExprParser *parser) {
    long long size = 8;

    if (expr_match_punct(parser, "(")) {
        if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
            size = guess_identifier_size(parser->state, parser->current.text);
            expr_next(parser);
        } else if (parser->current.kind == EXPR_TOKEN_STRING) {
            size = (long long)rt_strlen(parser->current.text) + 1;
            expr_next(parser);
        }
        if (parser->current.kind != EXPR_TOKEN_PUNCT || !names_equal(parser->current.text, ")")) {
            int depth = 0;
            while (parser->current.kind != EXPR_TOKEN_EOF) {
                if (parser->current.kind == EXPR_TOKEN_PUNCT) {
                    if (names_equal(parser->current.text, "(") || names_equal(parser->current.text, "[")) {
                        depth += 1;
                    } else if (names_equal(parser->current.text, ")")) {
                        if (depth == 0) {
                            break;
                        }
                        depth -= 1;
                    } else if (names_equal(parser->current.text, "]")) {
                        if (depth > 0) {
                            depth -= 1;
                        }
                    }
                }
                expr_next(parser);
            }
        }
        if (expr_expect_punct(parser, ")") != 0) {
            return -1;
        }
        return emit_load_immediate(parser->state, size);
    }

    if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
        size = guess_identifier_size(parser->state, parser->current.text);
        expr_next(parser);
    }
    return emit_load_immediate(parser->state, size);
}

static int expr_parse_compound_literal(ExprParser *parser, int want_address, int *byte_sized_out) {
    char temp_name[COMPILER_IR_NAME_CAPACITY];
    char digits[32];
    int byte_sized = 0;

    if (expr_expect_punct(parser, "(") != 0) {
        return -1;
    }

    while (parser->current.kind != EXPR_TOKEN_EOF) {
        if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")")) {
            break;
        }
        if (parser->current.kind == EXPR_TOKEN_IDENTIFIER && names_equal(parser->current.text, "char")) {
            byte_sized = 1;
        }
        if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "*")) {
            byte_sized = 0;
        }
        expr_next(parser);
    }

    if (expr_expect_punct(parser, ")") != 0 || expr_expect_punct(parser, "{") != 0) {
        return -1;
    }

    rt_copy_string(temp_name, sizeof(temp_name), "__compound");
    rt_unsigned_to_string((unsigned long long)parser->state->label_counter, digits, sizeof(digits));
    parser->state->label_counter += 1U;
    rt_copy_string(temp_name + rt_strlen(temp_name), sizeof(temp_name) - rt_strlen(temp_name), digits);

    if (allocate_local(parser->state, temp_name, 0, 0) != 0) {
        return -1;
    }

    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "}")) {
        if (emit_address_of_name(parser->state, temp_name) != 0 ||
            emit_push_value(parser->state) != 0 ||
            emit_load_immediate(parser->state, 0) != 0 ||
            emit_pop_address_and_store(parser->state, byte_sized) != 0) {
            return -1;
        }
    } else {
        if (emit_address_of_name(parser->state, temp_name) != 0 ||
            emit_push_value(parser->state) != 0 ||
            expr_parse_assignment(parser) != 0 ||
            emit_pop_address_and_store(parser->state, byte_sized) != 0) {
            return -1;
        }
        while (expr_match_punct(parser, ",")) {
            if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "}")) {
                break;
            }
            if (expr_parse_assignment(parser) != 0) {
                return -1;
            }
        }
    }

    if (expr_expect_punct(parser, "}") != 0) {
        return -1;
    }

    if (byte_sized_out != 0) {
        *byte_sized_out = byte_sized;
    }

    if (want_address) {
        return emit_address_of_name(parser->state, temp_name);
    }

    return 0;
}

static int expr_parse_call_arguments(ExprParser *parser, int *arg_count_out, int max_arg_count) {
    int tail_count = 0;

    if (expr_parse_assignment(parser) != 0) {
        return -1;
    }

    if (emit_push_value(parser->state) != 0) {
        return -1;
    }

    if (expr_match_punct(parser, ",")) {
        if (expr_parse_call_arguments(parser, &tail_count, max_arg_count) != 0) {
            return -1;
        }
        if (tail_count + 1 > max_arg_count) {
            backend_set_error(parser->state->backend,
                      "backend only supports up to 32 call arguments");
            return -1;
        }
    }

    *arg_count_out = tail_count + 1;
    return 0;
}

static int expr_parse_primary(ExprParser *parser) {
    if (parser->current.kind == EXPR_TOKEN_NUMBER || parser->current.kind == EXPR_TOKEN_CHAR) {
        long long value = parser->current.number_value;
        expr_next(parser);
        return emit_load_immediate(parser->state, value);
    }

    if (parser->current.kind == EXPR_TOKEN_STRING) {
        int result = emit_load_string_literal(parser->state, parser->current.text);
        expr_next(parser);
        if (result != 0) {
            return -1;
        }
        return expr_parse_postfix_suffixes(parser, 0, 1, 0);
    }

    if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
        char name[COMPILER_IR_NAME_CAPACITY];
        int saw_structish_suffix = 0;

        rt_copy_string(name, sizeof(name), parser->current.text);
        expr_next(parser);

        if (names_equal(name, "sizeof")) {
            return expr_parse_sizeof(parser);
        }

        if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(")) {
            static const char *const x86_arg_regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
            static const char *const aarch64_arg_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
            int arg_count = 0;
            int register_arg_count = backend_register_arg_limit(parser->state);
            int stack_arg_count = 0;
            int stack_slot_size = backend_stack_slot_size(parser->state);

            (void)expr_match_punct(parser, "(");
            if (!(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")"))) {
                if (expr_parse_call_arguments(parser, &arg_count, 32) != 0) {
                    return -1;
                }
            }
            if (expr_expect_punct(parser, ")") != 0) {
                return -1;
            }

            if (arg_count < register_arg_count) {
                register_arg_count = arg_count;
            }
            stack_arg_count = arg_count - register_arg_count;

            {
                int reg_index;
                for (reg_index = 0; reg_index < register_arg_count; ++reg_index) {
                    char line[64];
                    unsigned long long offset_bytes =
                        (unsigned long long)(stack_arg_count + (register_arg_count - 1 - reg_index)) *
                        (unsigned long long)stack_slot_size;
                    char offset_text[32];
                    const char *reg = backend_is_aarch64(parser->state) ? aarch64_arg_regs[reg_index] : x86_arg_regs[reg_index];
                    rt_unsigned_to_string(offset_bytes, offset_text, sizeof(offset_text));
                    if (backend_is_aarch64(parser->state)) {
                        rt_copy_string(line, sizeof(line), "ldr ");
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", [sp, #");
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
                    } else {
                        rt_copy_string(line, sizeof(line), "movq ");
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rsp), ");
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
                    }
                    if (emit_instruction(parser->state, line) != 0) {
                        return -1;
                    }
                }
            }
            if (!backend_is_aarch64(parser->state) && stack_arg_count > 0) {
                int stack_index;
                for (stack_index = 0; stack_index < stack_arg_count; ++stack_index) {
                    char reload[64];
                    char offset_text[32];
                    unsigned long long offset_bytes = (unsigned long long)(stack_index * 2) *
                                                      (unsigned long long)stack_slot_size;
                    rt_unsigned_to_string(offset_bytes, offset_text, sizeof(offset_text));
                    rt_copy_string(reload, sizeof(reload), "movq ");
                    rt_copy_string(reload + rt_strlen(reload), sizeof(reload) - rt_strlen(reload), offset_text);
                    rt_copy_string(reload + rt_strlen(reload), sizeof(reload) - rt_strlen(reload), "(%rsp), %rax");
                    if (emit_instruction(parser->state, reload) != 0 ||
                        emit_instruction(parser->state, "pushq %rax") != 0) {
                        return -1;
                    }
                }
            }
            if (is_function_name(parser->state, name) ||
                (find_local(parser->state, name) < 0 &&
                 find_global(parser->state, name) < 0 &&
                 find_constant(parser->state, name) < 0)) {
                char line[96];
                char symbol[COMPILER_IR_NAME_CAPACITY];
                int call_result;
                format_symbol_name(parser->state, name, symbol, sizeof(symbol));
                rt_copy_string(line, sizeof(line), backend_is_aarch64(parser->state) ? "bl " : "call ");
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
                call_result = emit_instruction(parser->state, line);
                if (call_result != 0) {
                    return -1;
                }
                if (arg_count > 0) {
                    char cleanup[64];
                    unsigned long long cleanup_bytes =
                        (unsigned long long)(arg_count + (backend_is_aarch64(parser->state) ? 0 : stack_arg_count)) *
                        (unsigned long long)stack_slot_size;
                    char digits[32];
                    rt_unsigned_to_string(cleanup_bytes, digits, sizeof(digits));
                    if (backend_is_aarch64(parser->state)) {
                        rt_copy_string(cleanup, sizeof(cleanup), "add sp, sp, #");
                        rt_copy_string(cleanup + rt_strlen(cleanup), sizeof(cleanup) - rt_strlen(cleanup), digits);
                    } else {
                        rt_copy_string(cleanup, sizeof(cleanup), "addq $");
                        rt_copy_string(cleanup + rt_strlen(cleanup), sizeof(cleanup) - rt_strlen(cleanup), digits);
                        rt_copy_string(cleanup + rt_strlen(cleanup), sizeof(cleanup) - rt_strlen(cleanup), ", %rsp");
                    }
                    if (emit_instruction(parser->state, cleanup) != 0) {
                        return -1;
                    }
                }
                return expr_parse_postfix_suffixes(parser, 0, 0, 0);
            }

            if (emit_load_name_into_register(parser->state, name, backend_is_aarch64(parser->state) ? "x16" : "%r11") != 0) {
                return -1;
            }
            if (emit_instruction(parser->state, backend_is_aarch64(parser->state) ? "blr x16" : "call *%r11") != 0) {
                return -1;
            }
            if (arg_count > 0) {
                char cleanup[64];
                unsigned long long cleanup_bytes =
                    (unsigned long long)(arg_count + (backend_is_aarch64(parser->state) ? 0 : stack_arg_count)) *
                    (unsigned long long)stack_slot_size;
                char digits[32];
                rt_unsigned_to_string(cleanup_bytes, digits, sizeof(digits));
                if (backend_is_aarch64(parser->state)) {
                    rt_copy_string(cleanup, sizeof(cleanup), "add sp, sp, #");
                    rt_copy_string(cleanup + rt_strlen(cleanup), sizeof(cleanup) - rt_strlen(cleanup), digits);
                } else {
                    rt_copy_string(cleanup, sizeof(cleanup), "addq $");
                    rt_copy_string(cleanup + rt_strlen(cleanup), sizeof(cleanup) - rt_strlen(cleanup), digits);
                    rt_copy_string(cleanup + rt_strlen(cleanup), sizeof(cleanup) - rt_strlen(cleanup), ", %rsp");
                }
                if (emit_instruction(parser->state, cleanup) != 0) {
                    return -1;
                }
            }
            return expr_parse_postfix_suffixes(parser, 0, 0, 0);
        }

        if (parser->current.kind == EXPR_TOKEN_PUNCT &&
            (names_equal(parser->current.text, "++") || names_equal(parser->current.text, "--"))) {
            int delta = names_equal(parser->current.text, "++") ? 1 : -1;
            expr_next(parser);
            return emit_identifier_incdec(parser->state, name, delta, 1);
        }

        if (parser->current.kind == EXPR_TOKEN_PUNCT &&
            (names_equal(parser->current.text, ".") || names_equal(parser->current.text, "->"))) {
            saw_structish_suffix = 1;
        }

        if (saw_structish_suffix) {
            if (names_equal(parser->current.text, ".") && emit_address_of_name(parser->state, name) != 0) {
                return -1;
            }
            if (names_equal(parser->current.text, "->") && emit_load_name(parser->state, name) != 0) {
                return -1;
            }
        } else if (emit_load_name(parser->state, name) != 0) {
            return -1;
        }
        return expr_parse_postfix_suffixes(parser,
                                           name_prefers_word_index(parser->state, name),
                                           saw_structish_suffix ? 1 : (find_local(parser->state, name) >= 0 &&
                                                                       parser->state->locals[find_local(parser->state, name)].is_array),
                                           saw_structish_suffix ? 1 : 0);
    }

    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(") &&
        expr_looks_like_compound_literal(parser)) {
        return expr_parse_compound_literal(parser, 0, 0);
    }

    if (expr_match_punct(parser, "(")) {
        if (expr_group_has_postfix_incdec(parser)) {
            int byte_sized = 0;
            int delta;

            if (expr_parse_lvalue_address(parser, &byte_sized) != 0 || expr_expect_punct(parser, ")") != 0) {
                return -1;
            }
            if (parser->current.kind != EXPR_TOKEN_PUNCT ||
                (!names_equal(parser->current.text, "++") && !names_equal(parser->current.text, "--"))) {
                backend_set_error(parser->state->backend, "unsupported expression syntax in backend");
                return -1;
            }
            delta = names_equal(parser->current.text, "++") ? 1 : -1;
            expr_next(parser);
            return emit_address_incdec(parser->state, byte_sized, delta, 1);
        }
        if (expr_parse_expression(parser) != 0 || expr_expect_punct(parser, ")") != 0) {
            return -1;
        }
        return expr_parse_postfix_suffixes(parser, 0, 0, 0);
    }

    backend_set_error(parser->state->backend, "unsupported primary expression in backend");
    return -1;
}

static int expr_parse_unary(ExprParser *parser) {
    char op[4];

    if (expr_looks_like_cast(parser)) {
        expr_next(parser);
        while (parser->current.kind != EXPR_TOKEN_EOF &&
               !(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")"))) {
            expr_next(parser);
        }
        if (expr_expect_punct(parser, ")") != 0) {
            return -1;
        }
        return expr_parse_unary(parser);
    }

    if (parser->current.kind == EXPR_TOKEN_PUNCT &&
        is_unary_prefix_text(parser->current.text)) {
        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);

        if (names_equal(op, "+")) {
            return expr_parse_unary(parser);
        }

        if (is_incdec_text(op)) {
            int delta = names_equal(op, "++") ? 1 : -1;
            if (parser->current.kind != EXPR_TOKEN_IDENTIFIER) {
                backend_set_error(parser->state->backend, "backend only supports ++/-- on identifiers");
                return -1;
            }
            if (emit_identifier_incdec(parser->state, parser->current.text, delta, 0) != 0) {
                return -1;
            }
            expr_next(parser);
            return 0;
        }

        if (names_equal(op, "&")) {
            int byte_sized = 0;
            return expr_parse_lvalue_address(parser, &byte_sized);
        }

        if (expr_parse_unary(parser) != 0) {
            return -1;
        }

        if (names_equal(op, "-")) {
            return emit_instruction(parser->state, backend_is_aarch64(parser->state) ? "neg x0, x0" : "negq %rax");
        }
        if (names_equal(op, "!")) {
            return emit_cmp_zero(parser->state) == 0 && emit_set_condition(parser->state, "eq") == 0 ? 0 : -1;
        }
        if (names_equal(op, "~")) {
            return emit_instruction(parser->state, backend_is_aarch64(parser->state) ? "mvn x0, x0" : "notq %rax");
        }
        if (names_equal(op, "*")) {
            return emit_load_from_address_register(parser->state, backend_is_aarch64(parser->state) ? "x0" : "%rax", 0);
        }
    }

    return expr_parse_primary(parser);
}

static int emit_binary_op(BackendState *state, const char *op) {
    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "mov x2, x0") != 0 ||
            emit_instruction(state, "ldr x1, [sp]") != 0 ||
            emit_instruction(state, "add sp, sp, #16") != 0) {
            return -1;
        }

        if (names_equal(op, "+")) return emit_instruction(state, "add x0, x1, x2");
        if (names_equal(op, "-")) return emit_instruction(state, "sub x0, x1, x2");
        if (names_equal(op, "*")) return emit_instruction(state, "mul x0, x1, x2");
        if (names_equal(op, "&")) return emit_instruction(state, "and x0, x1, x2");
        if (names_equal(op, "|")) return emit_instruction(state, "orr x0, x1, x2");
        if (names_equal(op, "^")) return emit_instruction(state, "eor x0, x1, x2");
        if (names_equal(op, "<<")) return emit_instruction(state, "lsl x0, x1, x2");
        if (names_equal(op, ">>")) return emit_instruction(state, "asr x0, x1, x2");
        if (names_equal(op, "/") || names_equal(op, "%")) {
            if (emit_instruction(state, "sdiv x3, x1, x2") != 0) {
                return -1;
            }
            if (names_equal(op, "%")) {
                return emit_instruction(state, "msub x0, x3, x2, x1");
            }
            return emit_instruction(state, "mov x0, x3");
        }

        if (emit_instruction(state, "cmp x1, x2") != 0) {
            return -1;
        }
        if (names_equal(op, "<")) return emit_set_condition(state, "lt");
        if (names_equal(op, "<=")) return emit_set_condition(state, "le");
        if (names_equal(op, ">")) return emit_set_condition(state, "gt");
        if (names_equal(op, ">=")) return emit_set_condition(state, "ge");
        if (names_equal(op, "==")) return emit_set_condition(state, "eq");
        if (names_equal(op, "!=")) return emit_set_condition(state, "ne");
    } else {
        if (emit_instruction(state, "movq %rax, %rcx") != 0 || emit_instruction(state, "popq %rax") != 0) {
            return -1;
        }

        if (names_equal(op, "+")) return emit_instruction(state, "addq %rcx, %rax");
        if (names_equal(op, "-")) return emit_instruction(state, "subq %rcx, %rax");
        if (names_equal(op, "*")) return emit_instruction(state, "imulq %rcx, %rax");
        if (names_equal(op, "&")) return emit_instruction(state, "andq %rcx, %rax");
        if (names_equal(op, "|")) return emit_instruction(state, "orq %rcx, %rax");
        if (names_equal(op, "^")) return emit_instruction(state, "xorq %rcx, %rax");
        if (names_equal(op, "<<")) return emit_instruction(state, "salq %cl, %rax");
        if (names_equal(op, ">>")) return emit_instruction(state, "sarq %cl, %rax");
        if (names_equal(op, "/") || names_equal(op, "%")) {
            if (emit_instruction(state, "movq %rax, %r11") != 0 ||
                emit_instruction(state, "movq %rcx, %r10") != 0 ||
                emit_instruction(state, "movq %r11, %rcx") != 0) {
                return -1;
            }
            if (emit_instruction(state, "cqto") != 0 || emit_instruction(state, "idivq %rcx") != 0) {
                return -1;
            }
            if (names_equal(op, "%")) {
                return emit_instruction(state, "movq %rdx, %rax");
            }
            return 0;
        }

        if (emit_instruction(state, "cmpq %rcx, %rax") != 0) {
            return -1;
        }
        if (names_equal(op, "<")) return emit_set_condition(state, "l");
        if (names_equal(op, "<=")) return emit_set_condition(state, "le");
        if (names_equal(op, ">")) return emit_set_condition(state, "g");
        if (names_equal(op, ">=")) return emit_set_condition(state, "ge");
        if (names_equal(op, "==")) return emit_set_condition(state, "e");
        if (names_equal(op, "!=")) return emit_set_condition(state, "ne");
    }

    backend_set_error(state->backend, "unsupported binary operation in backend");
    return -1;
}

static int expr_parse_chain(ExprParser *parser, int level, const char *const *ops, size_t op_count) {
    size_t i;

    switch (level) {
        case 0:
            if (expr_parse_unary(parser) != 0) {
                return -1;
            }
            break;
        case 1:
            if (expr_parse_multiplicative(parser) != 0) {
                return -1;
            }
            break;
        case 2:
            if (expr_parse_additive(parser) != 0) {
                return -1;
            }
            break;
        case 3:
            if (expr_parse_shift(parser) != 0) {
                return -1;
            }
            break;
        case 4:
            if (expr_parse_relational(parser) != 0) {
                return -1;
            }
            break;
        case 5:
            if (expr_parse_equality(parser) != 0) {
                return -1;
            }
            break;
        case 6:
            if (expr_parse_bitand(parser) != 0) {
                return -1;
            }
            break;
        case 7:
            if (expr_parse_bitxor(parser) != 0) {
                return -1;
            }
            break;
        default:
            backend_set_error(parser->state->backend, "unsupported expression precedence in backend");
            return -1;
    }

    for (;;) {
        int matched = 0;
        char op[4];

        if (parser->current.kind != EXPR_TOKEN_PUNCT) {
            break;
        }

        for (i = 0; i < op_count; ++i) {
            if (names_equal(parser->current.text, ops[i])) {
                matched = 1;
                break;
            }
        }
        if (!matched) {
            break;
        }

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        if (emit_push_value(parser->state) != 0) {
            return -1;
        }
        switch (level) {
            case 0:
                if (expr_parse_unary(parser) != 0) {
                    return -1;
                }
                break;
            case 1:
                if (expr_parse_multiplicative(parser) != 0) {
                    return -1;
                }
                break;
            case 2:
                if (expr_parse_additive(parser) != 0) {
                    return -1;
                }
                break;
            case 3:
                if (expr_parse_shift(parser) != 0) {
                    return -1;
                }
                break;
            case 4:
                if (expr_parse_relational(parser) != 0) {
                    return -1;
                }
                break;
            case 5:
                if (expr_parse_equality(parser) != 0) {
                    return -1;
                }
                break;
            case 6:
                if (expr_parse_bitand(parser) != 0) {
                    return -1;
                }
                break;
            case 7:
                if (expr_parse_bitxor(parser) != 0) {
                    return -1;
                }
                break;
            default:
                backend_set_error(parser->state->backend, "unsupported expression precedence in backend");
                return -1;
        }
        if (emit_binary_op(parser->state, op) != 0) {
            return -1;
        }
    }

    return 0;
}

static int expr_parse_multiplicative(ExprParser *parser) {
    static const char *const ops[] = {"*", "/", "%"};
    return expr_parse_chain(parser, 0, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_additive(ExprParser *parser) {
    static const char *const ops[] = {"+", "-"};
    return expr_parse_chain(parser, 1, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_shift(ExprParser *parser) {
    static const char *const ops[] = {"<<", ">>"};
    return expr_parse_chain(parser, 2, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_relational(ExprParser *parser) {
    static const char *const ops[] = {"<", ">", "<=", ">="};
    return expr_parse_chain(parser, 3, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_equality(ExprParser *parser) {
    static const char *const ops[] = {"==", "!="};
    return expr_parse_chain(parser, 4, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_bitand(ExprParser *parser) {
    static const char *const ops[] = {"&"};
    return expr_parse_chain(parser, 5, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_bitxor(ExprParser *parser) {
    static const char *const ops[] = {"^"};
    return expr_parse_chain(parser, 6, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_bitor(ExprParser *parser) {
    static const char *const ops[] = {"|"};
    return expr_parse_chain(parser, 7, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_make_logic_label(BackendState *state, const char *prefix, char *buffer, size_t buffer_size) {
    char digits[32];
    rt_copy_string(buffer, buffer_size, prefix);
    rt_unsigned_to_string((unsigned long long)state->label_counter, digits, sizeof(digits));
    state->label_counter += 1U;
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
    return 0;
}

static int expr_parse_logical_and(ExprParser *parser) {
    if (expr_parse_bitor(parser) != 0) {
        return -1;
    }

    while (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "&&")) {
        char false_label[32];
        char end_label[32];
        char asm_label[64];

        expr_make_logic_label(parser->state, "land_false", false_label, sizeof(false_label));
        expr_make_logic_label(parser->state, "land_end", end_label, sizeof(end_label));
        expr_next(parser);
        if (emit_cmp_zero(parser->state) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b.eq" : "je");
        if (emit_jump_to_label(parser->state, asm_label, false_label) != 0 || expr_parse_bitor(parser) != 0) {
            return -1;
        }
        if (emit_cmp_zero(parser->state) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b.eq" : "je");
        if (emit_jump_to_label(parser->state, asm_label, false_label) != 0 ||
            emit_load_immediate(parser->state, 1) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b" : "jmp");
        if (emit_jump_to_label(parser->state, asm_label, end_label) != 0) {
            return -1;
        }
        write_label_name(parser->state, asm_label, sizeof(asm_label), false_label);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
        if (emit_line(parser->state, asm_label) != 0 || emit_load_immediate(parser->state, 0) != 0) {
            return -1;
        }
        write_label_name(parser->state, asm_label, sizeof(asm_label), end_label);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
        if (emit_line(parser->state, asm_label) != 0) {
            return -1;
        }
    }

    return 0;
}

static int expr_parse_logical_or(ExprParser *parser) {
    if (expr_parse_logical_and(parser) != 0) {
        return -1;
    }

    while (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "||")) {
        char true_label[32];
        char end_label[32];
        char asm_label[64];

        expr_make_logic_label(parser->state, "lor_true", true_label, sizeof(true_label));
        expr_make_logic_label(parser->state, "lor_end", end_label, sizeof(end_label));
        expr_next(parser);
        if (emit_cmp_zero(parser->state) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b.ne" : "jne");
        if (emit_jump_to_label(parser->state, asm_label, true_label) != 0 || expr_parse_logical_and(parser) != 0) {
            return -1;
        }
        if (emit_cmp_zero(parser->state) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b.ne" : "jne");
        if (emit_jump_to_label(parser->state, asm_label, true_label) != 0 ||
            emit_load_immediate(parser->state, 0) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b" : "jmp");
        if (emit_jump_to_label(parser->state, asm_label, end_label) != 0) {
            return -1;
        }
        write_label_name(parser->state, asm_label, sizeof(asm_label), true_label);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
        if (emit_line(parser->state, asm_label) != 0 || emit_load_immediate(parser->state, 1) != 0) {
            return -1;
        }
        write_label_name(parser->state, asm_label, sizeof(asm_label), end_label);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
        if (emit_line(parser->state, asm_label) != 0) {
            return -1;
        }
    }

    return 0;
}

static int expr_parse_conditional(ExprParser *parser) {
    if (expr_parse_logical_or(parser) != 0) {
        return -1;
    }

    if (expr_match_punct(parser, "?")) {
        char false_label[32];
        char end_label[32];
        char asm_label[64];

        expr_make_logic_label(parser->state, "cond_false", false_label, sizeof(false_label));
        expr_make_logic_label(parser->state, "cond_end", end_label, sizeof(end_label));
        if (emit_cmp_zero(parser->state) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b.eq" : "je");
        if (emit_jump_to_label(parser->state, asm_label, false_label) != 0 ||
            expr_parse_assignment(parser) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b" : "jmp");
        if (emit_jump_to_label(parser->state, asm_label, end_label) != 0 ||
            expr_expect_punct(parser, ":") != 0) {
            return -1;
        }
        write_label_name(parser->state, asm_label, sizeof(asm_label), false_label);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
        if (emit_line(parser->state, asm_label) != 0 || expr_parse_assignment(parser) != 0) {
            return -1;
        }
        write_label_name(parser->state, asm_label, sizeof(asm_label), end_label);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
        if (emit_line(parser->state, asm_label) != 0) {
            return -1;
        }
    }

    return 0;
}

static int expr_assignment_operator(ExprParser snapshot, char *op, size_t op_size) {
    int depth = 0;

    while (snapshot.current.kind != EXPR_TOKEN_EOF) {
        if (snapshot.current.kind == EXPR_TOKEN_PUNCT) {
            if (names_equal(snapshot.current.text, "(") || names_equal(snapshot.current.text, "[")) {
                depth += 1;
            } else if (names_equal(snapshot.current.text, ")") || names_equal(snapshot.current.text, "]")) {
                if (depth == 0) {
                    break;
                }
                depth -= 1;
            } else if (depth == 0 && is_assignment_operator_text(snapshot.current.text)) {
                rt_copy_string(op, op_size, snapshot.current.text);
                return 1;
            } else if (depth == 0 && is_assignment_stop_text(snapshot.current.text)) {
                break;
            }
        }
        expr_next(&snapshot);
    }

    return 0;
}

static int expr_parse_lvalue_address(ExprParser *parser, int *byte_sized) {
    *byte_sized = 0;

    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(") &&
        expr_looks_like_compound_literal(parser)) {
        return expr_parse_compound_literal(parser, 1, byte_sized);
    }

    if (expr_looks_like_cast(parser)) {
        expr_next(parser);
        while (parser->current.kind != EXPR_TOKEN_EOF &&
               !(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")"))) {
            expr_next(parser);
        }
        if (expr_expect_punct(parser, ")") != 0 || expr_parse_unary(parser) != 0) {
            return -1;
        }
        return expr_parse_lvalue_suffixes(parser, byte_sized, 0);
    }

    if (expr_match_punct(parser, "(")) {
        if (expr_parse_lvalue_address(parser, byte_sized) != 0 || expr_expect_punct(parser, ")") != 0) {
            return -1;
        }
        return expr_parse_lvalue_suffixes(parser, byte_sized, 0);
    }

    if (expr_match_punct(parser, "*")) {
        if (expr_parse_unary(parser) != 0) {
            return -1;
        }
        *byte_sized = 0;
        return 0;
    }

    if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
        char name[COMPILER_IR_NAME_CAPACITY];
        int word_index;

        rt_copy_string(name, sizeof(name), parser->current.text);
        expr_next(parser);
        word_index = name_prefers_word_index(parser->state, name);

        if (parser->current.kind == EXPR_TOKEN_PUNCT &&
            (names_equal(parser->current.text, "[") || names_equal(parser->current.text, "->"))) {
            if (emit_load_name(parser->state, name) != 0) {
                return -1;
            }
        } else if (emit_address_of_name(parser->state, name) != 0) {
            return -1;
        }

        return expr_parse_lvalue_suffixes(parser, byte_sized, word_index);
    }

    backend_set_error(parser->state->backend, "unsupported assignment target in backend");
    return -1;
}

static int expr_parse_assignment(ExprParser *parser) {
    char op[4];
    ExprParser snapshot = *parser;

    if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
        char name[COMPILER_IR_NAME_CAPACITY];
        rt_copy_string(name, sizeof(name), snapshot.current.text);
        expr_next(&snapshot);
        if (snapshot.current.kind == EXPR_TOKEN_PUNCT &&
            is_assignment_operator_text(snapshot.current.text)) {
            rt_copy_string(op, sizeof(op), snapshot.current.text);
            expr_next(parser);
            expr_next(parser);

            if (!names_equal(op, "=")) {
                if (emit_load_name(parser->state, name) != 0 || emit_push_value(parser->state) != 0) {
                    return -1;
                }
            }

            if (expr_parse_assignment(parser) != 0) {
                return -1;
            }

            if (!names_equal(op, "=") && emit_binary_op(parser->state, binary_op_for_assignment(op)) != 0) {
                return -1;
            }

            return emit_store_name(parser->state, name);
        }
    }

    if (expr_assignment_operator(snapshot, op, sizeof(op))) {
        int byte_sized = 0;

        if (expr_parse_lvalue_address(parser, &byte_sized) != 0) {
            return -1;
        }
        if (parser->current.kind != EXPR_TOKEN_PUNCT || !names_equal(parser->current.text, op)) {
            backend_set_error(parser->state->backend, "unsupported expression syntax in backend");
            return -1;
        }
        expr_next(parser);
        if (emit_push_value(parser->state) != 0) {
            return -1;
        }

        if (!names_equal(op, "=")) {
            if (emit_load_from_address_register(parser->state, backend_is_aarch64(parser->state) ? "x0" : "%rax", byte_sized) != 0 ||
                emit_push_value(parser->state) != 0) {
                return -1;
            }
        }

        if (expr_parse_assignment(parser) != 0) {
            return -1;
        }

        if (!names_equal(op, "=") && emit_binary_op(parser->state, binary_op_for_assignment(op)) != 0) {
            return -1;
        }

        return emit_pop_address_and_store(parser->state, byte_sized);
    }

    return expr_parse_conditional(parser);
}

static int expr_parse_expression(ExprParser *parser) {
    if (expr_parse_assignment(parser) != 0) {
        return -1;
    }

    while (expr_match_punct(parser, ",")) {
        if (expr_parse_assignment(parser) != 0) {
            return -1;
        }
    }

    return 0;
}

int emit_expression(BackendState *state, const char *expr) {
    ExprParser parser;

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);
    if (expr_parse_expression(&parser) != 0) {
        if (starts_with(state->backend->error_message, "unsupported ")) {
            char message[256];
            size_t used = 0;
            size_t i = 0;
            rt_copy_string(message, sizeof(message), state->backend->error_message);
            used = rt_strlen(message);
            rt_copy_string(message + used, sizeof(message) - used, " near `");
            used = rt_strlen(message);
            while (expr[i] != '\0' && expr[i] != '\n' && used + 4 < sizeof(message)) {
                message[used++] = expr[i++];
            }
            if (expr[i] != '\0' && used + 4 < sizeof(message)) {
                message[used++] = '.';
                message[used++] = '.';
                message[used++] = '.';
            }
            message[used++] = '`';
            message[used] = '\0';
            backend_set_error(state->backend, message);
        }
        return -1;
    }
    return 0;
}

static int emit_array_element_address(BackendState *state,
                                      const char *name,
                                      int word_index,
                                      unsigned long long index) {
    if (emit_address_of_name(state, name) != 0) {
        return -1;
    }
    if (emit_push_value(state) != 0) {
        return -1;
    }
    if (emit_load_immediate(state, (long long)index) != 0) {
        return -1;
    }
    if (emit_index_address(state, word_index) != 0) {
        return -1;
    }
    return emit_push_value(state);
}

static int emit_flat_initializer_store(ExprParser *parser,
                                      BackendState *state,
                                      const char *name,
                                      int word_index,
                                      int byte_sized,
                                      unsigned long long *index_inout,
                                      unsigned long long slot_limit) {
    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "{")) {
        expr_next(parser);
        while (parser->current.kind != EXPR_TOKEN_EOF &&
               !(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "}"))) {
            if (emit_flat_initializer_store(parser, state, name, word_index, byte_sized, index_inout, slot_limit) != 0) {
                return -1;
            }
            if (!expr_match_punct(parser, ",")) {
                break;
            }
        }
        if (!expr_match_punct(parser, "}")) {
            backend_set_error(state->backend, "unsupported primary expression in backend");
            return -1;
        }
        return 0;
    }

    if (*index_inout < slot_limit) {
        if (emit_array_element_address(state, name, word_index, *index_inout) != 0) {
            return -1;
        }
        if (expr_parse_assignment(parser) != 0) {
            return -1;
        }
        if (emit_pop_address_and_store(state, byte_sized) != 0) {
            return -1;
        }
    } else {
        if (expr_parse_assignment(parser) != 0) {
            return -1;
        }
    }

    *index_inout += 1ULL;
    return 0;
}

int emit_array_initializer_store(BackendState *state, const char *name, const char *expr) {
    ExprParser parser;
    unsigned long long index = 0;
    unsigned long long slot_limit;
    int word_index = 0;
    int byte_sized;

    if (!lookup_array_storage(state, name, &word_index)) {
        backend_set_error(state->backend, "unsupported assignment target in backend");
        return -1;
    }

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);
    byte_sized = word_index ? 0 : 1;
    slot_limit = word_index ? (BACKEND_ARRAY_STACK_BYTES / 8U) : BACKEND_ARRAY_STACK_BYTES;

    if (parser.current.kind == EXPR_TOKEN_STRING) {
        size_t i;
        size_t length = rt_strlen(parser.current.text);

        for (i = 0; i <= length; ++i) {
            if (emit_array_element_address(state, name, word_index, index) != 0) {
                return -1;
            }
            if (emit_load_immediate(state, (long long)(unsigned char)parser.current.text[i]) != 0) {
                return -1;
            }
            if (emit_pop_address_and_store(state, byte_sized) != 0) {
                return -1;
            }
            index += 1U;
        }
        expr_next(&parser);
        if (parser.current.kind != EXPR_TOKEN_EOF) {
            backend_set_error(state->backend, "unsupported primary expression in backend");
            return -1;
        }
        return 0;
    }

    if (emit_flat_initializer_store(&parser, state, name, word_index, byte_sized, &index, slot_limit) != 0) {
        return -1;
    }

    if (parser.current.kind != EXPR_TOKEN_EOF) {
        backend_set_error(state->backend, "unsupported primary expression in backend");
        return -1;
    }

    return 0;
}

int emit_object_initializer_store(BackendState *state, const char *name, const char *expr) {
    ExprParser parser;
    unsigned long long index;
    unsigned long long slot_count = 1ULL;
    int byte_sized = 0;
    int word_index = 1;
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index < 0 && global_index < 0) {
        backend_set_error(state->backend, "unsupported assignment target in backend");
        return -1;
    }

    if (local_index >= 0) {
        if (state->locals[local_index].stack_bytes <= 1) {
            byte_sized = 1;
            word_index = 0;
        } else {
            slot_count = (unsigned long long)(state->locals[local_index].stack_bytes / 8);
            if (slot_count == 0ULL) {
                slot_count = 1ULL;
            }
        }
    }

    for (index = 0; index < slot_count; ++index) {
        if (emit_array_element_address(state, name, word_index, index) != 0 ||
            emit_load_immediate(state, 0) != 0 ||
            emit_pop_address_and_store(state, byte_sized) != 0) {
            return -1;
        }
    }

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);

    index = 0ULL;
    if (emit_flat_initializer_store(&parser, state, name, word_index, byte_sized, &index, slot_count) != 0) {
        return -1;
    }

    if (parser.current.kind != EXPR_TOKEN_EOF) {
        backend_set_error(state->backend, "unsupported primary expression in backend");
        return -1;
    }

    return 0;
}
