/* Expression lowering and initializer emission helpers. */

#include "backend_internal.h"

static int expr_match_punct(ExprParser *parser, const char *text);
static int expr_parse_postfix_suffixes(ExprParser *parser, int word_index, int current_is_address, int load_final_address, const char *base_type);
static int expr_read_punctuator_width(const char *cursor);
static int emit_binary_op(BackendState *state, const char *op);
static int expr_parse_expression(ExprParser *parser);
static int expr_parse_assignment(ExprParser *parser);
static int expr_parse_lvalue_address(ExprParser *parser, int *byte_sized);
static int expr_parse_lvalue_suffixes(ExprParser *parser,
                                      int *byte_sized,
                                      int word_index,
                                      int current_is_address,
                                      const char *base_type);
static int expr_parse_unary(ExprParser *parser);
static int expr_parse_multiplicative(ExprParser *parser);
static int expr_parse_additive(ExprParser *parser);
static int expr_parse_shift(ExprParser *parser);
static int expr_parse_relational(ExprParser *parser);
static int expr_parse_equality(ExprParser *parser);
static int expr_parse_bitand(ExprParser *parser);
static int expr_parse_bitxor(ExprParser *parser);
static int expr_parse_bitor(ExprParser *parser);
static int name_prefers_word_index(const BackendState *state, const char *name);
static int type_access_size(const char *type_text, int word_index);
static long long type_storage_bytes_text(const BackendState *state, const char *type_text);
static void copy_indexed_result_type(const char *base_type, char *buffer, size_t buffer_size);
static void copy_member_result_type(const BackendState *state,
                                    const char *base_type,
                                    const char *member_name,
                                    char *buffer,
                                    size_t buffer_size);
static void copy_dereferenced_type(const char *base_type, char *buffer, size_t buffer_size);
static int emit_named_call(ExprParser *parser, const char *name, const char *object_target_name);
static void expr_infer_result_type(ExprParser *parser, char *buffer, size_t buffer_size);
static int emit_scale_current_value(BackendState *state, int scale);
static int emit_scale_top_of_stack(BackendState *state, int scale);
static int type_is_unsigned_like(const char *type_text);

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

static int expr_operand_prefers_byte_load(ExprParser *parser) {
    ExprParser snapshot = *parser;
    int deref_depth = 1;
    char type_text[128];

    type_text[0] = '\0';

    while (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "(")) {
        expr_next(&snapshot);
    }

    while (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "*")) {
        deref_depth += 1;
        expr_next(&snapshot);
        while (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "(")) {
            expr_next(&snapshot);
        }
    }

    if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
        const char *known_type = lookup_name_type_text(snapshot.state, snapshot.current.text);
        if (known_type != 0 && known_type[0] != '\0') {
            rt_copy_string(type_text, sizeof(type_text), known_type);
            expr_next(&snapshot);
            while (snapshot.current.kind == EXPR_TOKEN_PUNCT &&
                   (names_equal(snapshot.current.text, "[") || names_equal(snapshot.current.text, ".") ||
                    names_equal(snapshot.current.text, "->"))) {
                if (names_equal(snapshot.current.text, "[")) {
                    while (snapshot.current.kind != EXPR_TOKEN_EOF && !names_equal(snapshot.current.text, "]")) {
                        expr_next(&snapshot);
                    }
                    if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "]")) {
                        char indexed[128];
                        copy_indexed_result_type(type_text, indexed, sizeof(indexed));
                        rt_copy_string(type_text, sizeof(type_text), indexed);
                        expr_next(&snapshot);
                    }
                    continue;
                }
                expr_next(&snapshot);
                if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
                    char member_type[128];
                    copy_member_result_type(snapshot.state, type_text, snapshot.current.text, member_type, sizeof(member_type));
                    rt_copy_string(type_text, sizeof(type_text), member_type);
                    expr_next(&snapshot);
                } else {
                    break;
                }
            }
        }
    }

    while (deref_depth > 0 && type_text[0] != '\0') {
        char deref_type[128];
        copy_dereferenced_type(type_text, deref_type, sizeof(deref_type));
        rt_copy_string(type_text, sizeof(type_text), deref_type);
        deref_depth -= 1;
    }

    if (type_text[0] != '\0') {
        return type_access_size(type_text, should_prefer_word_index("", type_text));
    }

    return 0;
}

static int expr_may_be_object_lvalue_source(ExprParser *parser) {
    ExprParser snapshot = *parser;

    while (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "(")) {
        expr_next(&snapshot);
    }

    if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "*")) {
        return 1;
    }

    if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
        expr_next(&snapshot);
        return !(snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "("));
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

static int type_access_size(const char *type_text, int word_index) {
    return backend_type_access_size(type_text, word_index);
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

static int member_prefers_word_index(const char *name, const char *type_text) {
    return backend_member_prefers_word_index(name, type_text);
}

static int member_result_decays_to_address(const char *type_text) {
    return backend_member_result_decays_to_address(type_text);
}

static int member_byte_offset(const BackendState *state, const char *base_type, const char *member_name) {
    return backend_member_byte_offset(state, base_type, member_name);
}

static void copy_member_result_type(const BackendState *state,
                                    const char *base_type,
                                    const char *member_name,
                                    char *buffer,
                                    size_t buffer_size) {
    backend_copy_member_result_type(state, base_type, member_name, buffer, buffer_size);
}

static int type_is_pointer_like(const char *base_type) {
    return backend_type_is_pointer_like(base_type);
}

static void copy_indexed_result_type(const char *base_type, char *buffer, size_t buffer_size) {
    backend_copy_indexed_type_text(base_type, buffer, buffer_size);
}

static int array_index_scale(const BackendState *state, const char *base_type, int word_index) {
    return backend_array_index_scale(state, base_type, word_index);
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

static void copy_dereferenced_type(const char *base_type, char *buffer, size_t buffer_size) {
    backend_copy_dereferenced_type_text(base_type, buffer, buffer_size);
}

static void append_pointer_type(char *buffer, size_t buffer_size) {
    size_t length;

    if (buffer == 0 || buffer_size == 0 || buffer[0] == '\0') {
        return;
    }
    length = rt_strlen(buffer);
    if (length + 2 >= buffer_size) {
        return;
    }
    buffer[length++] = '*';
    buffer[length] = '\0';
}

static void skip_inferred_parenthesized(ExprParser *parser) {
    int depth = 1;

    while (parser->current.kind != EXPR_TOKEN_EOF && depth > 0) {
        if (parser->current.kind == EXPR_TOKEN_PUNCT) {
            if (names_equal(parser->current.text, "(")) {
                depth += 1;
            } else if (names_equal(parser->current.text, ")")) {
                depth -= 1;
            }
        }
        expr_next(parser);
    }
}

static void expr_infer_result_type(ExprParser *parser, char *buffer, size_t buffer_size) {
    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';

    if (parser->current.kind == EXPR_TOKEN_STRING) {
        rt_copy_string(buffer, buffer_size, "char*");
        expr_next(parser);
    } else if (parser->current.kind == EXPR_TOKEN_NUMBER || parser->current.kind == EXPR_TOKEN_CHAR) {
        if (text_contains(parser->current.text, "u") || text_contains(parser->current.text, "U")) {
            rt_copy_string(buffer, buffer_size, "unsigned long");
        } else {
            rt_copy_string(buffer, buffer_size, "int");
        }
        expr_next(parser);
    } else if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "&")) {
        expr_next(parser);
        expr_infer_result_type(parser, buffer, buffer_size);
        append_pointer_type(buffer, buffer_size);
    } else if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "*")) {
        expr_next(parser);
        expr_infer_result_type(parser, buffer, buffer_size);
        if (buffer[0] != '\0') {
            char deref_type[128];
            copy_dereferenced_type(buffer, deref_type, sizeof(deref_type));
            rt_copy_string(buffer, buffer_size, deref_type);
        }
    } else if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(")) {
        if (expr_looks_like_cast(parser)) {
            expr_next(parser);
            while (parser->current.kind != EXPR_TOKEN_EOF &&
                   !(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")"))) {
                expr_next(parser);
            }
            if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")")) {
                expr_next(parser);
            }
            expr_infer_result_type(parser, buffer, buffer_size);
            return;
        }
        expr_next(parser);
        expr_infer_result_type(parser, buffer, buffer_size);
        skip_inferred_parenthesized(parser);
    } else if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
        char name[COMPILER_IR_NAME_CAPACITY];
        const char *known_type;

        rt_copy_string(name, sizeof(name), parser->current.text);
        known_type = lookup_name_type_text(parser->state, name);
        if (known_type != 0 && known_type[0] != '\0') {
            rt_copy_string(buffer, buffer_size, known_type);
        } else if (find_constant(parser->state, name) >= 0 && name_looks_like_macro_constant(name)) {
            rt_copy_string(buffer, buffer_size, "unsigned long");
        } else if (function_returns_object(parser->state, name)) {
            rt_copy_string(buffer, buffer_size, function_return_type(parser->state, name));
        } else {
            rt_copy_string(buffer, buffer_size, "int");
        }
        expr_next(parser);

        if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(")) {
            int depth = 1;
            if (!function_returns_object(parser->state, name)) {
                rt_copy_string(buffer, buffer_size, "int");
            }
            expr_next(parser);
            while (parser->current.kind != EXPR_TOKEN_EOF && depth > 0) {
                if (parser->current.kind == EXPR_TOKEN_PUNCT) {
                    if (names_equal(parser->current.text, "(")) {
                        depth += 1;
                    } else if (names_equal(parser->current.text, ")")) {
                        depth -= 1;
                    }
                }
                expr_next(parser);
            }
        }

        while (parser->current.kind == EXPR_TOKEN_PUNCT &&
               (names_equal(parser->current.text, "[") || names_equal(parser->current.text, ".") ||
                names_equal(parser->current.text, "->"))) {
            if (names_equal(parser->current.text, "[")) {
                char indexed[128];
                expr_next(parser);
                while (parser->current.kind != EXPR_TOKEN_EOF &&
                       !(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "]"))) {
                    expr_next(parser);
                }
                if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "]")) {
                    expr_next(parser);
                }
                copy_indexed_result_type(buffer, indexed, sizeof(indexed));
                rt_copy_string(buffer, buffer_size, indexed);
                continue;
            }
            expr_next(parser);
            if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
                char member_type[128];
                copy_member_result_type(parser->state, buffer, parser->current.text, member_type, sizeof(member_type));
                rt_copy_string(buffer, buffer_size, member_type);
                expr_next(parser);
            } else {
                break;
            }
        }
    }
}

static int emit_scale_current_value(BackendState *state, int scale) {
    char line[64];
    char digits[32];

    if (scale <= 1) {
        return 0;
    }
    rt_unsigned_to_string((unsigned long long)scale, digits, sizeof(digits));
    if (backend_is_aarch64(state)) {
        if (emit_load_immediate_register(state, "x9", scale) != 0) {
            return -1;
        }
        return emit_instruction(state, "mul x0, x0, x9");
    }
    rt_copy_string(line, sizeof(line), "imulq $");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rax, %rax");
    return emit_instruction(state, line);
}

static int emit_scale_top_of_stack(BackendState *state, int scale) {
    char line[64];
    char digits[32];

    if (scale <= 1) {
        return 0;
    }
    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "ldr x9, [sp]") != 0 ||
            emit_instruction(state, "add sp, sp, #16") != 0 ||
            emit_load_immediate_register(state, "x10", scale) != 0 ||
            emit_instruction(state, "mul x9, x9, x10") != 0 ||
            emit_instruction(state, "sub sp, sp, #16") != 0 ||
            emit_instruction(state, "str x9, [sp]") != 0) {
            return -1;
        }
        return 0;
    }
    rt_unsigned_to_string((unsigned long long)scale, digits, sizeof(digits));
    if (emit_instruction(state, "popq %rcx") != 0) {
        return -1;
    }
    rt_copy_string(line, sizeof(line), "imulq $");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rcx, %rcx");
    if (emit_instruction(state, line) != 0 ||
        emit_instruction(state, "pushq %rcx") != 0) {
        return -1;
    }
    return 0;
}

static int type_is_unsigned_like(const char *type_text) {
    const char *type = skip_spaces(type_text != 0 ? type_text : "");

    return text_contains(type, "unsigned") ||
           text_contains(type, "*") ||
           text_contains(type, "[") ||
           names_equal(type, "size_t") ||
           names_equal(type, "uintptr_t") ||
           names_equal(type, "usize");
}

static int expr_snapshot_looks_unsigned(ExprParser *parser) {
    ExprParser snapshot = *parser;

    if (snapshot.current.kind == EXPR_TOKEN_NUMBER) {
        return text_contains(snapshot.current.text, "u") || text_contains(snapshot.current.text, "U");
    }
    if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
        const char *known_type = lookup_name_type_text(snapshot.state, snapshot.current.text);
        if (known_type != 0 && type_is_unsigned_like(known_type)) {
            return 1;
        }
        return name_looks_like_macro_constant(snapshot.current.text) &&
               (snapshot.current.text[0] == 'U' ||
                text_contains(snapshot.current.text, "SIZE") ||
                text_contains(snapshot.current.text, "MAX"));
    }
    if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "(") &&
        expr_looks_like_cast(&snapshot)) {
        expr_next(&snapshot);
        while (snapshot.current.kind != EXPR_TOKEN_EOF &&
               !(snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, ")"))) {
            if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER &&
                (names_equal(snapshot.current.text, "unsigned") ||
                 names_equal(snapshot.current.text, "size_t") ||
                 names_equal(snapshot.current.text, "uintptr_t") ||
                 names_equal(snapshot.current.text, "usize"))) {
                return 1;
            }
            expr_next(&snapshot);
        }
    } else if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "(")) {
        expr_next(&snapshot);
        return expr_snapshot_looks_unsigned(&snapshot);
    }
    return 0;
}

static long long type_storage_bytes_text(const BackendState *state, const char *type_text) {
    return backend_type_storage_bytes(state, type_text);
}

static long long guess_identifier_size(const BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);
    long long size_guess;

    if (local_index >= 0) {
        size_guess = type_storage_bytes_text(state, state->locals[local_index].type_text);
        if (size_guess > 0) {
            return size_guess;
        }
        if (state->locals[local_index].stack_bytes > 0) {
            return state->locals[local_index].stack_bytes;
        }
        return backend_stack_slot_size(state);
    }
    if (global_index >= 0) {
        size_guess = type_storage_bytes_text(state, state->globals[global_index].type_text);
        if (size_guess > 0) {
            return size_guess;
        }
    }
    return 8;
}

static int emit_index_address(BackendState *state, int element_scale) {
    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "mov x2, x0") != 0 ||
            emit_instruction(state, "ldr x1, [sp]") != 0 ||
            emit_instruction(state, "add sp, sp, #16") != 0) {
            return -1;
        }
        if (element_scale > 1) {
            if (element_scale == 2) {
                if (emit_instruction(state, "lsl x2, x2, #1") != 0) return -1;
            } else if (element_scale == 4) {
                if (emit_instruction(state, "lsl x2, x2, #2") != 0) return -1;
            } else if (element_scale == 8) {
                if (emit_instruction(state, "lsl x2, x2, #3") != 0) return -1;
            } else {
                if (emit_load_immediate_register(state, "x3", element_scale) != 0 ||
                    emit_instruction(state, "mul x2, x2, x3") != 0) {
                    return -1;
                }
            }
        }
        return emit_instruction(state, "add x0, x1, x2");
    }

    if (emit_instruction(state, "movq %rax, %rcx") != 0 || emit_instruction(state, "popq %rax") != 0) {
        return -1;
    }
    if (element_scale == 1) {
        return emit_instruction(state, "addq %rcx, %rax");
    }
    if (element_scale == 2 || element_scale == 4 || element_scale == 8) {
        char line[64];
        char scale_text[8];

        rt_unsigned_to_string((unsigned long long)element_scale, scale_text, sizeof(scale_text));
        rt_copy_string(line, sizeof(line), "leaq (%rax,%rcx,");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), scale_text);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "), %rax");
        return emit_instruction(state, line);
    }
    if (emit_load_immediate_register(state, "%r11", element_scale) != 0 ||
        emit_instruction(state, "imulq %r11, %rcx") != 0) {
        return -1;
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
    if (return_old) {
        if (backend_is_aarch64(state)) {
            if (emit_instruction(state, "mov x11, x0") != 0) {
                return -1;
            }
        } else if (emit_instruction(state, "movq %rax, %r11") != 0) {
            return -1;
        }
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
        return backend_is_aarch64(state) ? emit_instruction(state, "mov x0, x11")
                                         : emit_instruction(state, "movq %r11, %rax");
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

static int expr_parse_lvalue_suffixes(ExprParser *parser,
                                      int *byte_sized,
                                      int word_index,
                                      int current_is_address,
                                      const char *base_type) {
    while (parser->current.kind == EXPR_TOKEN_PUNCT) {
        if (names_equal(parser->current.text, "[")) {
            int element_scale = array_index_scale(parser->state, base_type, word_index);
            int access_size = type_access_size(base_type, word_index);
            char element_type[128];

            copy_indexed_result_type(base_type, element_type, sizeof(element_type));

            if (current_is_address && type_is_pointer_like(base_type) && !member_result_decays_to_address(base_type)) {
                if (emit_load_from_address_register(parser->state,
                                                    backend_is_aarch64(parser->state) ? "x0" : "%rax",
                                                    access_size) != 0) {
                    return -1;
                }
            }
            expr_next(parser);
            if (emit_push_value(parser->state) != 0 ||
                expr_parse_expression(parser) != 0 ||
                expr_expect_punct(parser, "]") != 0 ||
                emit_index_address(parser->state, element_scale) != 0) {
                return -1;
            }
            *byte_sized = type_access_size(element_type, 0);
            word_index = 0;
            current_is_address = 1;
            base_type = element_type;
            if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                is_index_or_arrow_text(parser->current.text) &&
                type_is_pointer_like(element_type) &&
                !member_result_decays_to_address(element_type)) {
                if (emit_load_from_address_register(parser->state,
                                                    backend_is_aarch64(parser->state) ? "x0" : "%rax",
                                                    *byte_sized) != 0) {
                    return -1;
                }
                current_is_address = 0;
            }
            continue;
        }
        if (names_equal(parser->current.text, ".") || names_equal(parser->current.text, "->")) {
            char member_name[64];
            char member_type[128];
            int offset;
            expr_next(parser);
            if (parser->current.kind != EXPR_TOKEN_IDENTIFIER) {
                backend_set_error(parser->state->backend, "unsupported assignment target in backend");
                return -1;
            }
            rt_copy_string(member_name, sizeof(member_name), parser->current.text);
            offset = member_byte_offset(parser->state, base_type, member_name);
            if (offset > 0) {
                if (emit_push_value(parser->state) != 0 ||
                    emit_load_immediate(parser->state, offset) != 0 ||
                    emit_binary_op(parser->state, "+") != 0) {
                    return -1;
                }
            }
            copy_member_result_type(parser->state, base_type, member_name, member_type, sizeof(member_type));
            base_type = member_type;
            word_index = member_prefers_word_index(member_name, base_type);
            *byte_sized = type_access_size(base_type, word_index);
            current_is_address = 1;
            expr_next(parser);
            if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                is_index_or_arrow_text(parser->current.text) &&
                type_is_pointer_like(base_type) &&
                !member_result_decays_to_address(base_type)) {
                if (emit_load_from_address_register(parser->state,
                                                    backend_is_aarch64(parser->state) ? "x0" : "%rax",
                                                    *byte_sized) != 0) {
                    return -1;
                }
                current_is_address = 0;
            }
            continue;
        }
        break;
    }
    return 0;
}

static int expr_parse_postfix_suffixes(ExprParser *parser, int word_index, int current_is_address, int load_final_address, const char *base_type) {
    int byte_sized = type_access_size(base_type, word_index);

    for (;;) {
        if (expr_match_punct(parser, "[")) {
            char element_type[128];
            int element_scale = array_index_scale(parser->state, base_type, word_index);

            copy_indexed_result_type(base_type, element_type, sizeof(element_type));
            if (current_is_address && type_is_pointer_like(base_type) && !member_result_decays_to_address(base_type)) {
                if (emit_load_from_address_register(parser->state,
                                                    backend_is_aarch64(parser->state) ? "x0" : "%rax",
                                                    byte_sized) != 0) {
                    return -1;
                }
                current_is_address = 0;
            }
            if (emit_push_value(parser->state) != 0) {
                return -1;
            }
            if (expr_parse_expression(parser) != 0) {
                return -1;
            }
            if (expr_expect_punct(parser, "]") != 0) {
                return -1;
            }
            if (emit_index_address(parser->state, element_scale) != 0) {
                return -1;
            }
            current_is_address = 1;
            byte_sized = type_access_size(element_type, 0);
            word_index = 0;
            base_type = element_type;
            load_final_address = member_result_decays_to_address(element_type) ? 0 : 1;

            if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                is_index_or_arrow_text(parser->current.text)) {
                if (type_is_pointer_like(element_type)) {
                    if (emit_load_from_address_register(parser->state, backend_is_aarch64(parser->state) ? "x0" : "%rax",
                                                        byte_sized) != 0) {
                        return -1;
                    }
                    current_is_address = 0;
                }
            }
            continue;
        }

        if (parser->current.kind == EXPR_TOKEN_PUNCT &&
            (names_equal(parser->current.text, ".") || names_equal(parser->current.text, "->"))) {
            char member_name[64];
            char member_type[128];
            int offset;
            expr_next(parser);
            if (parser->current.kind != EXPR_TOKEN_IDENTIFIER) {
                backend_set_error(parser->state->backend, "unsupported expression syntax in backend");
                return -1;
            }
            rt_copy_string(member_name, sizeof(member_name), parser->current.text);
            offset = member_byte_offset(parser->state, base_type, member_name);
            if (offset > 0) {
                if (emit_push_value(parser->state) != 0 ||
                    emit_load_immediate(parser->state, offset) != 0 ||
                    emit_binary_op(parser->state, "+") != 0) {
                    return -1;
                }
            }
            copy_member_result_type(parser->state, base_type, member_name, member_type, sizeof(member_type));
            base_type = member_type;
            word_index = member_prefers_word_index(member_name, base_type);
            byte_sized = type_access_size(base_type, word_index);
            load_final_address = member_result_decays_to_address(base_type) ? 0 : 1;
            current_is_address = 1;
            expr_next(parser);
            if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                is_index_or_arrow_text(parser->current.text) &&
                type_is_pointer_like(base_type) &&
                !member_result_decays_to_address(base_type)) {
                if (emit_load_from_address_register(parser->state,
                                                    backend_is_aarch64(parser->state) ? "x0" : "%rax",
                                                    byte_sized) != 0) {
                    return -1;
                }
                current_is_address = 0;
            }
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
        if (parser->current.kind == EXPR_TOKEN_STRING) {
            size = (long long)rt_strlen(parser->current.text) + 1;
            expr_next(parser);
        } else if (parser->current.kind == EXPR_TOKEN_IDENTIFIER ||
                   (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "*"))) {
            char type_text[128];
            char identifier_name[COMPILER_IR_NAME_CAPACITY];
            int deref_count = 0;
            int had_deref = 0;
            int saw_suffix = 0;

            type_text[0] = '\0';
            identifier_name[0] = '\0';
            while (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "*")) {
                deref_count += 1;
                had_deref = 1;
                expr_next(parser);
            }
            if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
                const char *known_type = lookup_name_type_text(parser->state, parser->current.text);
                rt_copy_string(identifier_name, sizeof(identifier_name), parser->current.text);
                if (known_type != 0 && known_type[0] != '\0') {
                    rt_copy_string(type_text, sizeof(type_text), known_type);
                } else {
                    rt_copy_string(type_text, sizeof(type_text), parser->current.text);
                }
                expr_next(parser);
                while (parser->current.kind == EXPR_TOKEN_PUNCT &&
                       (names_equal(parser->current.text, "[") || names_equal(parser->current.text, ".") ||
                        names_equal(parser->current.text, "->"))) {
                    saw_suffix = 1;
                    if (names_equal(parser->current.text, "[")) {
                        while (parser->current.kind != EXPR_TOKEN_EOF && !names_equal(parser->current.text, "]")) {
                            expr_next(parser);
                        }
                        if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "]")) {
                            char indexed[128];
                            copy_indexed_result_type(type_text, indexed, sizeof(indexed));
                            rt_copy_string(type_text, sizeof(type_text), indexed);
                            expr_next(parser);
                        }
                        continue;
                    }
                    expr_next(parser);
                    if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
                        char member_type[128];
                        copy_member_result_type(parser->state, type_text, parser->current.text, member_type, sizeof(member_type));
                        rt_copy_string(type_text, sizeof(type_text), member_type);
                        expr_next(parser);
                    } else {
                        break;
                    }
                }
            }
            while (deref_count > 0 && type_text[0] != '\0') {
                char deref_type[128];
                copy_dereferenced_type(type_text, deref_type, sizeof(deref_type));
                rt_copy_string(type_text, sizeof(type_text), deref_type);
                deref_count -= 1;
            }
            if (type_text[0] != '\0' && identifier_name[0] != '\0' && !saw_suffix && !had_deref) {
                size = guess_identifier_size(parser->state, identifier_name);
            } else if (type_text[0] != '\0') {
                size = type_storage_bytes_text(parser->state, type_text);
            }
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

    if (allocate_local(parser->state, temp_name, "int", 0, 0, 0, 0, 0) != 0) {
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

static int emit_named_call(ExprParser *parser, const char *name, const char *object_target_name) {
    static const char *const x86_arg_regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
    static const char *const aarch64_arg_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
    int arg_count = 0;
    int hidden_arg_count = 0;
    int register_arg_count = backend_register_arg_limit(parser->state);
    int stack_arg_count = 0;
    int stack_slot_size = backend_stack_slot_size(parser->state);
    int returns_object = function_returns_object(parser->state, name);
    const char *return_type = function_return_type(parser->state, name);
    const char *result_object = object_target_name;

    if (returns_object && (result_object == 0 || result_object[0] == '\0')) {
        if (find_local(parser->state, "__callret") >= 0) {
            result_object = "__callret";
        } else {
            backend_set_error(parser->state->backend, "object return call requires target storage");
            return -1;
        }
    }

    (void)expr_match_punct(parser, "(");
    if (returns_object) {
        if (emit_address_of_name(parser->state, result_object) != 0 ||
            emit_push_value(parser->state) != 0) {
            return -1;
        }
        hidden_arg_count = 1;
    }
    if (!(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")"))) {
        if (expr_parse_call_arguments(parser, &arg_count, 32 - hidden_arg_count) != 0) {
            return -1;
        }
    }
    if (expr_expect_punct(parser, ")") != 0) {
        return -1;
    }

    arg_count += hidden_arg_count;
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

    {
        char line[96];
        char symbol[COMPILER_IR_NAME_CAPACITY];
        format_symbol_name(parser->state, name, symbol, sizeof(symbol));
        if (!backend_is_aarch64(parser->state) &&
            emit_instruction(parser->state, "xor %eax, %eax") != 0) {
            return -1;
        }
        rt_copy_string(line, sizeof(line), backend_is_aarch64(parser->state) ? "bl " : "call ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
        if (emit_instruction(parser->state, line) != 0) {
            return -1;
        }
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

    if (returns_object) {
        if (emit_address_of_name(parser->state, result_object) != 0) {
            return -1;
        }
        return expr_parse_postfix_suffixes(parser, 0, 1, 0, return_type);
    }

    return expr_parse_postfix_suffixes(parser, 0, 0, 0, 0);
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
        return expr_parse_postfix_suffixes(parser, 0, 1, 0, 0);
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
            if (is_function_name(parser->state, name) ||
                (find_local(parser->state, name) < 0 &&
                 find_global(parser->state, name) < 0 &&
                 find_constant(parser->state, name) < 0)) {
                return emit_named_call(parser, name, 0);
            }

            {
                int arg_count = 0;
                int register_arg_count = backend_register_arg_limit(parser->state);
                int stack_arg_count = 0;
                int stack_slot_size = backend_stack_slot_size(parser->state);
                static const char *const x86_arg_regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
                static const char *const aarch64_arg_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};

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

                if (emit_load_name_into_register(parser->state, name, backend_is_aarch64(parser->state) ? "x16" : "%r11") != 0) {
                    return -1;
                }
                if (!backend_is_aarch64(parser->state) &&
                    emit_instruction(parser->state, "xor %eax, %eax") != 0) {
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
            }

            return expr_parse_postfix_suffixes(parser, 0, 0, 0, 0);
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
                                           saw_structish_suffix ? 1 : 0,
                                           lookup_name_type_text(parser->state, name));
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
        return expr_parse_postfix_suffixes(parser, 0, 0, 0, 0);
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
        int byte_load = 0;
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

        if (names_equal(op, "*")) {
            byte_load = expr_operand_prefers_byte_load(parser);
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
            return emit_load_from_address_register(parser->state,
                                                   backend_is_aarch64(parser->state) ? "x0" : "%rax",
                                                   byte_load);
        }
    }

    return expr_parse_primary(parser);
}

static int emit_binary_op_mode(BackendState *state, const char *op, int use_unsigned) {
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
        if (names_equal(op, ">>")) return emit_instruction(state, "lsr x0, x1, x2");
        if (names_equal(op, "/") || names_equal(op, "%")) {
            if (emit_instruction(state, use_unsigned ? "udiv x3, x1, x2" : "sdiv x3, x1, x2") != 0) {
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
        if (use_unsigned && names_equal(op, "<")) return emit_set_condition(state, "lo");
        if (use_unsigned && names_equal(op, "<=")) return emit_set_condition(state, "ls");
        if (use_unsigned && names_equal(op, ">")) return emit_set_condition(state, "hi");
        if (use_unsigned && names_equal(op, ">=")) return emit_set_condition(state, "hs");
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
        if (names_equal(op, ">>")) return emit_instruction(state, "shrq %cl, %rax");
        if (names_equal(op, "/") || names_equal(op, "%")) {
            if (use_unsigned) {
                if (emit_instruction(state, "xor %edx, %edx") != 0 || emit_instruction(state, "divq %rcx") != 0) {
                    return -1;
                }
            } else {
                if (emit_instruction(state, "cqto") != 0 || emit_instruction(state, "idivq %rcx") != 0) {
                    return -1;
                }
            }
            if (names_equal(op, "%")) {
                return emit_instruction(state, "movq %rdx, %rax");
            }
            return 0;
        }

        if (emit_instruction(state, "cmpq %rcx, %rax") != 0) {
            return -1;
        }
        if (use_unsigned && names_equal(op, "<")) return emit_set_condition(state, "b");
        if (use_unsigned && names_equal(op, "<=")) return emit_set_condition(state, "be");
        if (use_unsigned && names_equal(op, ">")) return emit_set_condition(state, "a");
        if (use_unsigned && names_equal(op, ">=")) return emit_set_condition(state, "ae");
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

static int emit_binary_op(BackendState *state, const char *op) {
    return emit_binary_op_mode(state, op, 0);
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
    char lhs_type[128];
    ExprParser lhs_snapshot = *parser;
    int lhs_unsigned = expr_snapshot_looks_unsigned(&lhs_snapshot);

    expr_infer_result_type(&lhs_snapshot, lhs_type, sizeof(lhs_type));
    if (expr_parse_unary(parser) != 0) {
        return -1;
    }

    while (parser->current.kind == EXPR_TOKEN_PUNCT &&
           (names_equal(parser->current.text, "*") || names_equal(parser->current.text, "/") ||
            names_equal(parser->current.text, "%"))) {
        char op[4];
        char rhs_type[128];
        ExprParser rhs_snapshot;
        int use_unsigned;

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        if (emit_push_value(parser->state) != 0) {
            return -1;
        }
        rhs_snapshot = *parser;
        use_unsigned = lhs_unsigned || expr_snapshot_looks_unsigned(&rhs_snapshot);
        expr_infer_result_type(&rhs_snapshot, rhs_type, sizeof(rhs_type));
        if (expr_parse_unary(parser) != 0) {
            return -1;
        }
        use_unsigned = use_unsigned || type_is_unsigned_like(lhs_type) || type_is_unsigned_like(rhs_type);
        if (emit_binary_op_mode(parser->state, op, use_unsigned) != 0) {
            return -1;
        }
        rt_copy_string(lhs_type, sizeof(lhs_type), use_unsigned ? "unsigned long" : "int");
        lhs_unsigned = use_unsigned;
    }
    return 0;
}

static int expr_parse_additive(ExprParser *parser) {
    char lhs_type[128];
    ExprParser lhs_snapshot = *parser;

    expr_infer_result_type(&lhs_snapshot, lhs_type, sizeof(lhs_type));
    if (expr_parse_multiplicative(parser) != 0) {
        return -1;
    }

    while (parser->current.kind == EXPR_TOKEN_PUNCT &&
           (names_equal(parser->current.text, "+") || names_equal(parser->current.text, "-"))) {
        char op[4];
        char rhs_type[128];
        ExprParser rhs_snapshot;
        int lhs_pointer_like;
        int rhs_pointer_like;

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        if (emit_push_value(parser->state) != 0) {
            return -1;
        }

        rhs_snapshot = *parser;
        expr_infer_result_type(&rhs_snapshot, rhs_type, sizeof(rhs_type));
        if (expr_parse_multiplicative(parser) != 0) {
            return -1;
        }

        lhs_pointer_like = lhs_type[0] != '\0' && (text_contains(lhs_type, "*") || text_contains(lhs_type, "["));
        rhs_pointer_like = rhs_type[0] != '\0' && (text_contains(rhs_type, "*") || text_contains(rhs_type, "["));

        if (lhs_pointer_like && !rhs_pointer_like) {
            int scale = array_index_scale(parser->state, lhs_type, should_prefer_word_index("", lhs_type));
            if (emit_scale_current_value(parser->state, scale) != 0) {
                return -1;
            }
        } else if (names_equal(op, "+") && !lhs_pointer_like && rhs_pointer_like) {
            int scale = array_index_scale(parser->state, rhs_type, should_prefer_word_index("", rhs_type));
            if (emit_scale_top_of_stack(parser->state, scale) != 0) {
                return -1;
            }
            rt_copy_string(lhs_type, sizeof(lhs_type), rhs_type);
        } else if (!lhs_pointer_like || rhs_pointer_like) {
            lhs_type[0] = '\0';
        }

        if (emit_binary_op(parser->state, op) != 0) {
            return -1;
        }
    }
    return 0;
}

static int expr_parse_shift(ExprParser *parser) {
    static const char *const ops[] = {"<<", ">>"};
    return expr_parse_chain(parser, 2, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_relational(ExprParser *parser) {
    char lhs_type[128];
    ExprParser lhs_snapshot = *parser;
    int lhs_unsigned = expr_snapshot_looks_unsigned(&lhs_snapshot);

    expr_infer_result_type(&lhs_snapshot, lhs_type, sizeof(lhs_type));
    if (expr_parse_shift(parser) != 0) {
        return -1;
    }

    while (parser->current.kind == EXPR_TOKEN_PUNCT &&
           (names_equal(parser->current.text, "<") || names_equal(parser->current.text, ">") ||
            names_equal(parser->current.text, "<=") || names_equal(parser->current.text, ">="))) {
        char op[4];
        char rhs_type[128];
        ExprParser rhs_snapshot;
        int use_unsigned;

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        if (emit_push_value(parser->state) != 0) {
            return -1;
        }
        rhs_snapshot = *parser;
        use_unsigned = lhs_unsigned || expr_snapshot_looks_unsigned(&rhs_snapshot);
        expr_infer_result_type(&rhs_snapshot, rhs_type, sizeof(rhs_type));
        if (expr_parse_shift(parser) != 0) {
            return -1;
        }
        use_unsigned = use_unsigned || type_is_unsigned_like(lhs_type) || type_is_unsigned_like(rhs_type);
        if (emit_binary_op_mode(parser->state, op, use_unsigned) != 0) {
            return -1;
        }
        rt_copy_string(lhs_type, sizeof(lhs_type), "int");
        lhs_unsigned = 0;
    }
    return 0;
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
        char cast_base_type[128];
        int cast_is_pointer = 0;
        cast_base_type[0] = '\0';
        expr_next(parser);
        if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
            char probe[128];
            size_t tlen = rt_strlen(parser->current.text);
            if (tlen + 8 < sizeof(probe)) {
                rt_copy_string(probe, sizeof(probe), "struct:");
                rt_copy_string(probe + 7, sizeof(probe) - 7, parser->current.text);
                if (lookup_aggregate_size(parser->state, probe) > 0) {
                    rt_copy_string(cast_base_type, sizeof(cast_base_type), probe);
                }
            }
        }
        while (parser->current.kind != EXPR_TOKEN_EOF &&
               !(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")"))) {
            if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "*")) {
                cast_is_pointer = 1;
            }
            expr_next(parser);
        }
        if (cast_base_type[0] != '\0' && cast_is_pointer) {
            size_t blen = rt_strlen(cast_base_type);
            if (blen + 1 < sizeof(cast_base_type)) {
                cast_base_type[blen] = '*';
                cast_base_type[blen + 1] = '\0';
            }
        }
        if (expr_expect_punct(parser, ")") != 0 || expr_parse_unary(parser) != 0) {
            return -1;
        }
        return expr_parse_lvalue_suffixes(parser, byte_sized, 0, 0,
                                          cast_base_type[0] != '\0' ? cast_base_type : 0);
    }

    if (expr_match_punct(parser, "(")) {
        if (expr_parse_lvalue_address(parser, byte_sized) != 0 || expr_expect_punct(parser, ")") != 0) {
            return -1;
        }
        return expr_parse_lvalue_suffixes(parser, byte_sized, 0, 1, 0);
    }

    if (expr_match_punct(parser, "*")) {
        *byte_sized = expr_operand_prefers_byte_load(parser);
        if (expr_parse_unary(parser) != 0) {
            return -1;
        }
        return 0;
    }

    if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
        char name[COMPILER_IR_NAME_CAPACITY];
        int word_index;

        rt_copy_string(name, sizeof(name), parser->current.text);
        expr_next(parser);
        word_index = name_prefers_word_index(parser->state, name);

        {
            int current_is_address = 1;

            if (parser->current.kind == EXPR_TOKEN_PUNCT &&
            (names_equal(parser->current.text, "[") || names_equal(parser->current.text, "->"))) {
                if (emit_load_name(parser->state, name) != 0) {
                    return -1;
                }
                current_is_address = 0;
            } else if (emit_address_of_name(parser->state, name) != 0) {
                return -1;
            }

            return expr_parse_lvalue_suffixes(parser,
                                              byte_sized,
                                              word_index,
                                              current_is_address,
                                              lookup_name_type_text(parser->state, name));
        }
    }

    backend_set_error(parser->state->backend, "unsupported assignment target in backend");
    return -1;
}

static int expr_parse_assignment(ExprParser *parser) {
    char op[4];
    ExprParser snapshot = *parser;
    ExprParser lhs_type_snapshot = *parser;
    char lhs_type[128];

    expr_infer_result_type(&lhs_type_snapshot, lhs_type, sizeof(lhs_type));

    if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
        char name[COMPILER_IR_NAME_CAPACITY];
        rt_copy_string(name, sizeof(name), snapshot.current.text);
        expr_next(&snapshot);
        if (snapshot.current.kind == EXPR_TOKEN_PUNCT &&
            is_assignment_operator_text(snapshot.current.text)) {
            rt_copy_string(op, sizeof(op), snapshot.current.text);
            expr_next(parser);
            expr_next(parser);

            if (names_equal(op, "=")) {
                ExprParser rhs_snapshot = *parser;

                if (rhs_snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
                    char function_name[COMPILER_IR_NAME_CAPACITY];
                    rt_copy_string(function_name, sizeof(function_name), rhs_snapshot.current.text);
                    expr_next(&rhs_snapshot);
                    if (rhs_snapshot.current.kind == EXPR_TOKEN_PUNCT &&
                        names_equal(rhs_snapshot.current.text, "(") &&
                        function_returns_object(parser->state, function_name)) {
                        expr_next(parser);
                        return emit_named_call(parser, function_name, name);
                    }
                }

                int word_index = 0;
                if (lookup_array_storage(parser->state, name, &word_index)) {
                    int rhs_byte_sized = 0;
                    int local_index = find_local(parser->state, name);
                    int bytes = 0;
                    int direct_store_size = -1;
                    ExprParser object_rhs_snapshot = *parser;

                    if (local_index >= 0) {
                        bytes = parser->state->locals[local_index].stack_bytes;
                    }
                    if (bytes == 1) {
                        direct_store_size = 1;
                    } else if (bytes == 2) {
                        direct_store_size = 2;
                    } else if (bytes == 4) {
                        direct_store_size = 4;
                    } else if (bytes > 0 && bytes <= backend_stack_slot_size(parser->state)) {
                        direct_store_size = 0;
                    }
                    if (object_rhs_snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
                        char function_name[COMPILER_IR_NAME_CAPACITY];
                        rt_copy_string(function_name, sizeof(function_name), object_rhs_snapshot.current.text);
                        expr_next(&object_rhs_snapshot);
                        if (object_rhs_snapshot.current.kind == EXPR_TOKEN_PUNCT &&
                            names_equal(object_rhs_snapshot.current.text, "(") &&
                            function_returns_object(parser->state, function_name)) {
                            expr_next(parser);
                            return emit_named_call(parser, function_name, name);
                        }
                    }
                    if (expr_may_be_object_lvalue_source(parser)) {
                        if (expr_parse_lvalue_address(parser, &rhs_byte_sized) != 0) {
                            return -1;
                        }
                    } else if (expr_parse_assignment(parser) != 0) {
                        return -1;
                    } else if (local_index >= 0 && direct_store_size >= 0) {
                        return emit_local_address(parser->state,
                                                  parser->state->locals[local_index].offset,
                                                  backend_is_aarch64(parser->state) ? "x9" : "%rcx") == 0 &&
                               emit_store_to_address_register(parser->state,
                                                              backend_is_aarch64(parser->state) ? "x9" : "%rcx",
                                                              direct_store_size) == 0 ? 0 : -1;
                    }
                    return emit_copy_object_to_name(parser->state, name);
                }
            }

            if (!names_equal(op, "=")) {
                if (emit_load_name(parser->state, name) != 0 || emit_push_value(parser->state) != 0) {
                    return -1;
                }
            }

            if (expr_parse_assignment(parser) != 0) {
                return -1;
            }

            if (!names_equal(op, "=") &&
                emit_binary_op_mode(parser->state,
                                    binary_op_for_assignment(op),
                                    type_is_unsigned_like(lookup_name_type_text(parser->state, name))) != 0) {
                return -1;
            }

            return emit_store_name(parser->state, name);
        }
    }

    if (expr_assignment_operator(snapshot, op, sizeof(op))) {
        int byte_sized = 0;
        int target_is_object = lhs_type[0] != '\0' &&
                               !text_contains(lhs_type, "*") &&
                               (text_contains(lhs_type, "[") ||
                                starts_with(skip_spaces(lhs_type), "struct:") ||
                                starts_with(skip_spaces(lhs_type), "union:"));
        int target_bytes = target_is_object ? (int)type_storage_bytes_text(parser->state, lhs_type) : 0;

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

        if (target_is_object && byte_sized == 0 && names_equal(op, "=")) {
            int rhs_byte_sized = 0;

            if (expr_may_be_object_lvalue_source(parser)) {
                if (expr_parse_lvalue_address(parser, &rhs_byte_sized) != 0) {
                    return -1;
                }
            } else if (expr_parse_assignment(parser) != 0) {
                return -1;
            }
            return emit_copy_object_to_pushed_address(parser->state,
                                                      target_bytes > 0 ? target_bytes : backend_stack_slot_size(parser->state));
        }

        if (expr_parse_assignment(parser) != 0) {
            return -1;
        }

        if (!names_equal(op, "=") &&
            emit_binary_op_mode(parser->state,
                                binary_op_for_assignment(op),
                                type_is_unsigned_like(lhs_type)) != 0) {
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
                                      int element_scale,
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
    if (emit_index_address(state, element_scale) != 0) {
        return -1;
    }
    return emit_push_value(state);
}

static unsigned long long initializer_array_length_limit(const char *type_text, int element_scale) {
    const char *open = type_text != 0 ? type_text : "";
    unsigned long long length = 0ULL;

    while (*open != '\0' && *open != '[') {
        open += 1;
    }
    if (*open == '[') {
        open += 1;
        while (*open >= '0' && *open <= '9') {
            length = length * 10ULL + (unsigned long long)(*open - '0');
            open += 1;
        }
    }
    if (length == 0ULL) {
        int scale = element_scale > 0 ? element_scale : 1;
        return (unsigned long long)BACKEND_ARRAY_STACK_BYTES / (unsigned long long)scale;
    }
    return length;
}

static int emit_offset_address(BackendState *state, const char *name, int offset_bytes) {
    if (emit_address_of_name(state, name) != 0) {
        return -1;
    }
    if (offset_bytes > 0) {
        if (emit_push_value(state) != 0 ||
            emit_load_immediate(state, offset_bytes) != 0 ||
            emit_binary_op(state, "+") != 0) {
            return -1;
        }
    }
    return 0;
}

static int emit_initializer_value_at_offset(ExprParser *parser,
                                            BackendState *state,
                                            const char *name,
                                            int offset_bytes,
                                            const char *type_text) {
    const char *type = skip_spaces(type_text != 0 ? type_text : "");

    if (text_contains(type, "[")) {
        char element_type[128];
        int element_word_index = should_prefer_word_index(name, type);
        int element_scale = array_index_scale(state, type, element_word_index);
        unsigned long long slot_limit = initializer_array_length_limit(type, element_scale);
        unsigned long long index = 0ULL;

        copy_indexed_result_type(type, element_type, sizeof(element_type));
        if (parser->current.kind == EXPR_TOKEN_STRING) {
            size_t i;
            size_t length = rt_strlen(parser->current.text);
            for (i = 0; i <= length && index < slot_limit; ++i, ++index) {
                if (emit_offset_address(state, name, offset_bytes + (int)(index * (unsigned long long)element_scale)) != 0 ||
                    emit_push_value(state) != 0 ||
                    emit_load_immediate(state, (long long)(unsigned char)parser->current.text[i]) != 0 ||
                    emit_pop_address_and_store(state, 1) != 0) {
                    return -1;
                }
            }
            expr_next(parser);
            return 0;
        }
        if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "{")) {
            expr_next(parser);
            while (parser->current.kind != EXPR_TOKEN_EOF &&
                   !(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "}"))) {
                if (index < slot_limit &&
                    emit_initializer_value_at_offset(parser,
                                                     state,
                                                     name,
                                                     offset_bytes + (int)(index * (unsigned long long)element_scale),
                                                     element_type) != 0) {
                    return -1;
                }
                if (index < slot_limit) {
                    index += 1ULL;
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
    }

    if (emit_offset_address(state, name, offset_bytes) != 0 ||
        emit_push_value(state) != 0 ||
        expr_parse_assignment(parser) != 0 ||
        emit_pop_address_and_store(state,
                                   type_access_size(type,
                                                    should_prefer_word_index(name, type))) != 0) {
        return -1;
    }
    return 0;
}

static int emit_flat_initializer_store(ExprParser *parser,
                                       BackendState *state,
                                       const char *name,
                                      int element_scale,
                                      int byte_sized,
                                      unsigned long long *index_inout,
                                      unsigned long long slot_limit) {
    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "{")) {
        expr_next(parser);
        while (parser->current.kind != EXPR_TOKEN_EOF &&
               !(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "}"))) {
            if (emit_flat_initializer_store(parser, state, name, element_scale, byte_sized, index_inout, slot_limit) != 0) {
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
        if (emit_array_element_address(state, name, element_scale, *index_inout) != 0) {
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

static void copy_initializer_aggregate_name(const char *type_text, char *buffer, size_t buffer_size) {
    const char *cursor;
    size_t out = 0;

    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    type_text = skip_spaces(type_text != 0 ? type_text : "");
    if (starts_with(type_text, "struct:")) {
        cursor = type_text + 7;
    } else if (starts_with(type_text, "union:")) {
        cursor = type_text + 6;
    } else {
        return;
    }
    while (*cursor != '\0' && *cursor != '[' && *cursor != '*' && *cursor != ' ' && out + 1 < buffer_size) {
        buffer[out++] = *cursor++;
    }
    buffer[out] = '\0';
}

static int emit_named_object_initializer_store(BackendState *state, const char *name, const char *type_text, const char *expr) {
    ExprParser parser;
    char aggregate_name[COMPILER_IR_NAME_CAPACITY];
    size_t i;
    int matched = 0;

    copy_initializer_aggregate_name(type_text, aggregate_name, sizeof(aggregate_name));
    if (aggregate_name[0] == '\0') {
        return -1;
    }

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);

    if (parser.current.kind == EXPR_TOKEN_PUNCT && names_equal(parser.current.text, "{")) {
        expr_next(&parser);
    }

    for (i = 0; i < state->aggregate_member_count; ++i) {
        int access_size;

        if (!names_equal(state->aggregate_members[i].aggregate_name, aggregate_name)) {
            continue;
        }
        matched = 1;
        if (parser.current.kind == EXPR_TOKEN_PUNCT && names_equal(parser.current.text, "}")) {
            break;
        }

        access_size = type_access_size(state->aggregate_members[i].type_text,
                                       should_prefer_word_index(state->aggregate_members[i].name,
                                                                state->aggregate_members[i].type_text));
        (void)access_size;
        if (emit_initializer_value_at_offset(&parser,
                                             state,
                                             name,
                                             state->aggregate_members[i].offset_bytes,
                                             state->aggregate_members[i].type_text) != 0) {
            return -1;
        }
        if (parser.current.kind == EXPR_TOKEN_PUNCT && names_equal(parser.current.text, ",")) {
            expr_next(&parser);
        }
    }

    if (parser.current.kind == EXPR_TOKEN_PUNCT && names_equal(parser.current.text, "}")) {
        expr_next(&parser);
    }

    if (!matched || parser.current.kind != EXPR_TOKEN_EOF) {
        return -1;
    }
    return 0;
}

int emit_array_initializer_store(BackendState *state, const char *name, const char *expr) {
    ExprParser parser;
    unsigned long long index = 0;
    unsigned long long slot_limit;
    int word_index = 0;
    int element_scale;
    int byte_sized;
    int local_index = find_local(state, name);
    int storage_bytes = (int)type_storage_bytes_text(state, lookup_name_type_text(state, name));

    if (storage_bytes <= 0 && local_index >= 0) {
        storage_bytes = state->locals[local_index].stack_bytes;
    }

    if (!lookup_array_storage(state, name, &word_index)) {
        backend_set_error(state->backend, "unsupported assignment target in backend");
        return -1;
    }

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);
    element_scale = array_index_scale(state, lookup_name_type_text(state, name), word_index);
    byte_sized = (element_scale == 1 || element_scale == 2 || element_scale == 4) ? element_scale : 0;
    if (storage_bytes <= 0) {
        storage_bytes = BACKEND_ARRAY_STACK_BYTES;
    }
    slot_limit = element_scale > 1 ? ((unsigned long long)storage_bytes / (unsigned long long)element_scale)
                                   : (unsigned long long)storage_bytes;
    if (slot_limit == 0ULL) {
        slot_limit = 1ULL;
    }

    for (index = 0ULL; index < slot_limit; ++index) {
        if (emit_array_element_address(state, name, element_scale, index) != 0 ||
            emit_load_immediate(state, 0) != 0 ||
            emit_pop_address_and_store(state, byte_sized) != 0) {
            return -1;
        }
    }
    index = 0ULL;

    if (parser.current.kind == EXPR_TOKEN_STRING) {
        size_t i;
        size_t length = rt_strlen(parser.current.text);

        for (i = 0; i <= length; ++i) {
            if (emit_array_element_address(state, name, element_scale, index) != 0) {
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

    if (emit_flat_initializer_store(&parser, state, name, element_scale, byte_sized, &index, slot_limit) != 0) {
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
    const char *object_type = lookup_name_type_text(state, name);
    unsigned long long index;
    unsigned long long slot_count = 1ULL;
    int byte_sized = 0;
    int element_scale;
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index < 0 && global_index < 0) {
        backend_set_error(state->backend, "unsupported assignment target in backend");
        return -1;
    }

    if (local_index >= 0) {
        if (state->locals[local_index].stack_bytes <= 1) {
            byte_sized = 1;
        } else if (state->locals[local_index].stack_bytes <= 2) {
            byte_sized = 2;
        } else if (state->locals[local_index].stack_bytes <= 4) {
            byte_sized = 4;
        } else {
            slot_count = (unsigned long long)(state->locals[local_index].stack_bytes / backend_stack_slot_size(state));
            if (slot_count == 0ULL) {
                slot_count = 1ULL;
            }
        }
    }

    element_scale = byte_sized != 0 ? byte_sized : backend_stack_slot_size(state);

    for (index = 0; index < slot_count; ++index) {
        if (emit_array_element_address(state, name, element_scale, index) != 0 ||
            emit_load_immediate(state, 0) != 0 ||
            emit_pop_address_and_store(state, byte_sized) != 0) {
            return -1;
        }
    }

    if ((starts_with(skip_spaces(object_type), "struct:") || starts_with(skip_spaces(object_type), "union:")) &&
        !text_contains(skip_spaces(object_type), "*") &&
        emit_named_object_initializer_store(state, name, object_type, expr) == 0) {
        return 0;
    }

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);

    index = 0ULL;
    if (emit_flat_initializer_store(&parser, state, name, element_scale, byte_sized, &index, slot_count) != 0) {
        return -1;
    }

    if (parser.current.kind != EXPR_TOKEN_EOF) {
        backend_set_error(state->backend, "unsupported primary expression in backend");
        return -1;
    }

    return 0;
}

static int emit_object_call_into_name(BackendState *state, const char *name, const char *expr) {
    ExprParser parser;
    char function_name[COMPILER_IR_NAME_CAPACITY];

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);
    if (parser.current.kind != EXPR_TOKEN_IDENTIFIER) {
        return -1;
    }
    rt_copy_string(function_name, sizeof(function_name), parser.current.text);
    expr_next(&parser);
    if (parser.current.kind != EXPR_TOKEN_PUNCT ||
        !names_equal(parser.current.text, "(") ||
        !function_returns_object(state, function_name)) {
        return -1;
    }
    if (emit_named_call(&parser, function_name, name) != 0) {
        return -1;
    }
    return parser.current.kind == EXPR_TOKEN_EOF ? 0 : -1;
}

int emit_object_copy_store(BackendState *state, const char *name, const char *expr) {
    ExprParser parser;
    int byte_sized = 0;
    int local_index = find_local(state, name);
    int bytes = 0;
    int direct_store_size = -1;

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);

    if (emit_object_call_into_name(state, name, expr) == 0) {
        return 0;
    }

    if (local_index >= 0) {
        bytes = state->locals[local_index].stack_bytes;
    }
    if (bytes == 1) {
        direct_store_size = 1;
    } else if (bytes == 2) {
        direct_store_size = 2;
    } else if (bytes == 4) {
        direct_store_size = 4;
    } else if (bytes > 0 && bytes <= backend_stack_slot_size(state)) {
        direct_store_size = 0;
    }

    if (expr_may_be_object_lvalue_source(&parser)) {
        if (expr_parse_lvalue_address(&parser, &byte_sized) != 0) {
            return -1;
        }
        return emit_copy_object_to_name(state, name);
    }

    if (emit_expression(state, expr) != 0) {
        return -1;
    }

    if (local_index >= 0 && direct_store_size >= 0) {
        return emit_local_address(state, state->locals[local_index].offset, backend_is_aarch64(state) ? "x9" : "%rcx") == 0 &&
               emit_store_to_address_register(state, backend_is_aarch64(state) ? "x9" : "%rcx", direct_store_size) == 0 ? 0 : -1;
    }

    return emit_copy_object_to_name(state, name);
}
