/* Expression type and address helper routines. */

#include "backend_internal.h"

int expr_operand_prefers_byte_load(ExprParser *parser) {
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

    while (snapshot.current.kind == EXPR_TOKEN_PUNCT &&
           (names_equal(snapshot.current.text, "++") || names_equal(snapshot.current.text, "--"))) {
        expr_next(&snapshot);
    }

    if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
        const char *known_type = lookup_name_type_text(snapshot.state, snapshot.current.text);
        if (known_type == 0 || known_type[0] == '\0') {
            known_type = function_return_type(snapshot.state, snapshot.current.text);
        }
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

int expr_may_be_object_lvalue_source(ExprParser *parser) {
    ExprParser snapshot = *parser;
    char type_text[128];

    while (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "(")) {
        expr_next(&snapshot);
    }

    if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "*")) {
        return 1;
    }

    if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
        type_text[0] = '\0';
        expr_infer_result_type(&snapshot, type_text, sizeof(type_text));
        type_text[sizeof(type_text) - 1] = '\0';
        return type_text[0] != '\0' &&
               !text_contains(type_text, "*") &&
               (text_contains(type_text, "[") ||
                starts_with(skip_spaces(type_text), "struct:") ||
                starts_with(skip_spaces(type_text), "union:"));
    }

    return 0;
}

int name_prefers_word_index(const BackendState *state, const char *name) {
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

int type_access_size(const char *type_text, int word_index) {
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

int member_prefers_word_index(const char *name, const char *type_text) {
    return backend_member_prefers_word_index(name, type_text);
}

int member_result_decays_to_address(const char *type_text) {
    return backend_member_result_decays_to_address(type_text);
}

int member_byte_offset(const BackendState *state, const char *base_type, const char *member_name) {
    return backend_member_byte_offset(state, base_type, member_name);
}

void copy_member_result_type(const BackendState *state,
                                    const char *base_type,
                                    const char *member_name,
                                    char *buffer,
                                    size_t buffer_size) {
    backend_copy_member_result_type(state, base_type, member_name, buffer, buffer_size);
}

int type_is_pointer_like(const char *base_type) {
    return backend_type_is_pointer_like(base_type);
}

void copy_indexed_result_type(const char *base_type, char *buffer, size_t buffer_size) {
    backend_copy_indexed_type_text(base_type, buffer, buffer_size);
}

int array_index_scale(const BackendState *state, const char *base_type, int word_index) {
    return backend_array_index_scale(state, base_type, word_index);
}

int expr_looks_like_compound_literal(ExprParser *parser) {
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

int expr_looks_like_cast(ExprParser *parser) {
    ExprParser snapshot = *parser;
    int saw_typeish = 0;
    int saw_token = 0;
    int nested_parens = 0;
    int nested_brackets = 0;
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
        if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "[")) {
            if (!saw_typeish) {
                return 0;
            }
            nested_brackets += 1;
            saw_token = 1;
            expr_next(&snapshot);
            continue;
        }
        if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "]")) {
            if (nested_brackets <= 0) {
                return 0;
            }
            nested_brackets -= 1;
            saw_token = 1;
            expr_next(&snapshot);
            continue;
        }
        if (nested_brackets > 0) {
            saw_token = 1;
            expr_next(&snapshot);
            continue;
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

void copy_dereferenced_type(const char *base_type, char *buffer, size_t buffer_size) {
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

static void expr_copy_cast_type_text(ExprParser *parser, char *buffer, size_t buffer_size) {
    char first_identifier[64];
    int saw_unsigned = 0;
    int saw_signed = 0;
    int saw_pointer = 0;
    int saw_tag_keyword = 0;

    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    first_identifier[0] = '\0';

    if (!expr_match_punct(parser, "(")) {
        return;
    }
    while (parser->current.kind != EXPR_TOKEN_EOF &&
           !(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")"))) {
        if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "*")) {
            saw_pointer = 1;
            expr_next(parser);
            continue;
        }
        if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
            if (names_equal(parser->current.text, "const") || names_equal(parser->current.text, "volatile") ||
                names_equal(parser->current.text, "register")) {
                expr_next(parser);
                continue;
            }
            if (names_equal(parser->current.text, "unsigned")) {
                saw_unsigned = 1;
                expr_next(parser);
                continue;
            }
            if (names_equal(parser->current.text, "signed")) {
                saw_signed = 1;
                expr_next(parser);
                continue;
            }
            if (names_equal(parser->current.text, "struct") || names_equal(parser->current.text, "union") ||
                names_equal(parser->current.text, "enum")) {
                saw_tag_keyword = 1;
                rt_copy_string(buffer, buffer_size, names_equal(parser->current.text, "union") ? "union:" : "struct:");
                expr_next(parser);
                if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
                    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), parser->current.text);
                    expr_next(parser);
                }
                continue;
            }
            if (first_identifier[0] == '\0') {
                rt_copy_string(first_identifier, sizeof(first_identifier), parser->current.text);
            }
            expr_next(parser);
            continue;
        }
        expr_next(parser);
    }

    if (buffer[0] == '\0' && first_identifier[0] != '\0') {
        char aggregate_name[128];
        rt_copy_string(aggregate_name, sizeof(aggregate_name), "struct:");
        rt_copy_string(aggregate_name + rt_strlen(aggregate_name), sizeof(aggregate_name) - rt_strlen(aggregate_name), first_identifier);
        if (lookup_aggregate_size(parser->state, aggregate_name) > 0) {
            rt_copy_string(buffer, buffer_size, aggregate_name);
        } else {
            if (saw_unsigned) {
                rt_copy_string(buffer, buffer_size, "unsigned ");
                rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), first_identifier);
            } else if (saw_signed) {
                rt_copy_string(buffer, buffer_size, "signed ");
                rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), first_identifier);
            } else {
                rt_copy_string(buffer, buffer_size, first_identifier);
            }
        }
    }
    if (!saw_tag_keyword && buffer[0] == '\0') {
        rt_copy_string(buffer, buffer_size, saw_unsigned ? "unsigned int" : "int");
    }
    if (saw_pointer) {
        append_pointer_type(buffer, buffer_size);
    }
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

void expr_infer_result_type(ExprParser *parser, char *buffer, size_t buffer_size) {
    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';

    if (parser->current.kind == EXPR_TOKEN_STRING) {
        rt_copy_string(buffer, buffer_size, "char*");
        expr_next(parser);
    } else if (parser->current.kind == EXPR_TOKEN_NUMBER || parser->current.kind == EXPR_TOKEN_CHAR) {
        if (parser->current.number_is_unsigned) {
            if (!text_contains(parser->current.text, "l") &&
                !text_contains(parser->current.text, "L") &&
                parser->current.number_value >= 0 &&
                (unsigned long long)parser->current.number_value <= 0xffffffffULL) {
                rt_copy_string(buffer, buffer_size, "unsigned int");
            } else {
                rt_copy_string(buffer, buffer_size, "unsigned long");
            }
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
        if (expr_looks_like_compound_literal(parser) || expr_looks_like_cast(parser)) {
            ExprParser cast_parser = *parser;
            char cast_type[128];
            int cast_depth = 1;
            expr_copy_cast_type_text(&cast_parser, cast_type, sizeof(cast_type));
            expr_next(parser);
            while (parser->current.kind != EXPR_TOKEN_EOF && cast_depth > 0) {
                if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(")) {
                    cast_depth += 1;
                } else if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")")) {
                    cast_depth -= 1;
                }
                expr_next(parser);
            }
            if (cast_type[0] != '\0') {
                rt_copy_string(buffer, buffer_size, cast_type);
            } else {
                expr_infer_result_type(parser, buffer, buffer_size);
            }
            return;
        }
        expr_next(parser);
        expr_infer_result_type(parser, buffer, buffer_size);
        skip_inferred_parenthesized(parser);
    } else if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
        char name[COMPILER_IR_NAME_CAPACITY];
        const char *known_type;
        const char *call_return_type;

        rt_copy_string(name, sizeof(name), parser->current.text);
        known_type = lookup_name_type_text(parser->state, name);
        call_return_type = function_return_type(parser->state, name);
        if (known_type != 0 && known_type[0] != '\0') {
            rt_copy_string(buffer, buffer_size, known_type);
        } else if (find_constant(parser->state, name) >= 0 && name_looks_like_macro_constant(name)) {
            rt_copy_string(buffer, buffer_size, "unsigned long");
        } else if (call_return_type[0] != '\0') {
            rt_copy_string(buffer, buffer_size, call_return_type);
        } else {
            rt_copy_string(buffer, buffer_size, "int");
        }
        expr_next(parser);

        if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(")) {
            int depth = 1;
            if (call_return_type[0] == '\0') {
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

static int scale_shift_amount(int scale) {
    int shift = 0;
    int value = 1;

    if (scale <= 1) {
        return 0;
    }
    while (shift < 30 && value < scale) {
        value <<= 1;
        shift += 1;
    }
    return value == scale ? shift : 0;
}

static int emit_shift_register_left(BackendState *state, const char *reg, int shift) {
    char line[64];
    char digits[32];

    rt_unsigned_to_string((unsigned long long)shift, digits, sizeof(digits));
    if (backend_is_aarch64(state)) {
        rt_copy_string(line, sizeof(line), "lsl ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", #");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
    } else {
        rt_copy_string(line, sizeof(line), "salq $");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
    }
    return emit_instruction(state, line);
}

int emit_scale_current_value(BackendState *state, int scale) {
    char line[64];
    char digits[32];
    int shift = scale_shift_amount(scale);

    if (scale <= 1) {
        return 0;
    }
    if (shift > 0) {
        return emit_shift_register_left(state, backend_is_aarch64(state) ? "x0" : "%rax", shift);
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

int emit_scale_top_of_stack(BackendState *state, int scale) {
    char line[64];
    char digits[32];
    int shift = scale_shift_amount(scale);

    if (scale <= 1) {
        return 0;
    }
    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "ldr x9, [sp]") != 0 ||
            emit_instruction(state, "add sp, sp, #16") != 0) {
            return -1;
        }
        if (shift > 0) {
            if (emit_shift_register_left(state, "x9", shift) != 0) {
                return -1;
            }
        } else if (emit_load_immediate_register(state, "x10", scale) != 0 ||
                   emit_instruction(state, "mul x9, x9, x10") != 0) {
            return -1;
        }
        if (emit_instruction(state, "sub sp, sp, #16") != 0 ||
            emit_instruction(state, "str x9, [sp]") != 0) {
            return -1;
        }
        return 0;
    }
    if (emit_pop_to_register(state, "%rcx") != 0) {
        return -1;
    }
    if (shift > 0) {
        if (emit_shift_register_left(state, "%rcx", shift) != 0) {
            return -1;
        }
    } else {
        rt_unsigned_to_string((unsigned long long)scale, digits, sizeof(digits));
        rt_copy_string(line, sizeof(line), "imulq $");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rcx, %rcx");
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
    }
    if (emit_instruction(state, "pushq %rcx") != 0) {
        return -1;
    }
    return 0;
}

int type_is_unsigned_like(const char *type_text) {
    const char *type = skip_spaces(type_text != 0 ? type_text : "");

    return text_contains(type, "unsigned") ||
           text_contains(type, "*") ||
           names_equal(type, "size_t") ||
           names_equal(type, "uintptr_t") ||
           names_equal(type, "usize") ||
           names_equal(type, "uint8_t") ||
           names_equal(type, "uint16_t") ||
           names_equal(type, "uint32_t") ||
           names_equal(type, "u64") ||
           names_equal(type, "u32") ||
           names_equal(type, "u16") ||
           names_equal(type, "u8") ||
           names_equal(type, "uint64_t");
}

int expr_snapshot_looks_unsigned(ExprParser *parser) {
    ExprParser snapshot = *parser;

    if (snapshot.current.kind == EXPR_TOKEN_NUMBER) {
        return snapshot.current.number_is_unsigned;
    }
    if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
        ExprParser inferred = snapshot;
        char inferred_type[128];
        const char *known_type = lookup_name_type_text(snapshot.state, snapshot.current.text);

        expr_infer_result_type(&inferred, inferred_type, sizeof(inferred_type));
        if (inferred_type[0] != '\0') {
            return type_is_unsigned_like(inferred_type);
        }
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

long long type_storage_bytes_text(const BackendState *state, const char *type_text) {
    return backend_type_storage_bytes(state, type_text);
}

long long guess_identifier_size(const BackendState *state, const char *name) {
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

int emit_index_address(BackendState *state, int element_scale) {
    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "mov x2, x0") != 0 ||
            emit_instruction(state, "ldr x1, [sp]") != 0 ||
            emit_instruction(state, "add sp, sp, #16") != 0) {
            return -1;
        }
        if (element_scale > 1) {
            int shift = scale_shift_amount(element_scale);
            if (shift > 0) {
                if (emit_shift_register_left(state, "x2", shift) != 0) return -1;
            } else {
                if (emit_load_immediate_register(state, "x3", element_scale) != 0 ||
                    emit_instruction(state, "mul x2, x2, x3") != 0) {
                    return -1;
                }
            }
        }
        return emit_instruction(state, "add x0, x1, x2");
    }

    if (emit_instruction(state, "movq %rax, %rcx") != 0 || emit_pop_to_register(state, "%rax") != 0) {
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
    {
        int shift = scale_shift_amount(element_scale);
        if (shift > 0) {
            if (emit_shift_register_left(state, "%rcx", shift) != 0) {
                return -1;
            }
            return emit_instruction(state, "addq %rcx, %rax");
        }
    }
    if (emit_load_immediate_register(state, "%r11", element_scale) != 0 ||
        emit_instruction(state, "imulq %r11, %rcx") != 0) {
        return -1;
    }
    return emit_instruction(state, "addq %rcx, %rax");
}

static int emit_x86_cached_index_address(BackendState *state, const char *base_name, const char *index_name, int element_scale) {
    int index_local = find_local(state, index_name);
    const char *index_reg;
    const char *address_index_reg;
    int index_access_size;
    char line[96];

    if (backend_is_aarch64(state) || index_local < 0 || state->locals[index_local].cached_register < 0) {
        return 0;
    }
    index_reg = backend_x86_cached_register_name(state->locals[index_local].cached_register);
    if (index_reg == 0) {
        return -1;
    }
    if (emit_load_name_into_register(state, base_name, "%rax") != 0) {
        return -1;
    }
    index_access_size = type_access_size(state->locals[index_local].type_text,
                                         state->locals[index_local].prefers_word_index);
    address_index_reg = index_reg;
    if (index_access_size != 0) {
        if (emit_load_name_into_register(state, index_name, "%r11") != 0) {
            return -1;
        }
        address_index_reg = "%r11";
    }
    if (element_scale == 1 || element_scale == 2 || element_scale == 4 || element_scale == 8) {
        char scale_text[8];

        rt_unsigned_to_string((unsigned long long)element_scale, scale_text, sizeof(scale_text));
        rt_copy_string(line, sizeof(line), "leaq (%rax,");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), address_index_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ",");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), scale_text);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "), %rax");
        return emit_instruction(state, line) == 0 ? 1 : -1;
    }
    rt_copy_string(line, sizeof(line), "movq ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), address_index_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %r11");
    if (emit_instruction(state, line) != 0) {
        return -1;
    }
    if (emit_load_immediate_register(state, "%rcx", element_scale) != 0 ||
        emit_instruction(state, "imulq %rcx, %r11") != 0) {
        return -1;
    }
    return emit_instruction(state, "addq %r11, %rax") == 0 ? 1 : -1;
}

int emit_index_address_cached_affine(BackendState *state,
                                     const char *base_name,
                                     const char *index_name,
                                     int element_scale,
                                     long long multiplier,
                                     long long offset) {
    int index_local = find_local(state, index_name);
    const char *index_reg;
    const char *address_index_reg;
    int index_access_size;
    long long total_scale = multiplier * (long long)element_scale;
    long long byte_offset = offset * (long long)element_scale;
    char line[128];
    char digits[32];
    int shift = 0;

    if (backend_is_aarch64(state) || index_local < 0 || state->locals[index_local].cached_register < 0 ||
        multiplier <= 0 || element_scale <= 0 || total_scale <= 0) {
        return 0;
    }

    index_reg = backend_x86_cached_register_name(state->locals[index_local].cached_register);
    if (index_reg == 0) {
        return -1;
    }

    if (base_name != 0 && base_name[0] != '\0') {
        if (emit_load_name_into_register(state, base_name, "%rax") != 0) {
            return -1;
        }
    }

    index_access_size = type_access_size(state->locals[index_local].type_text,
                                         state->locals[index_local].prefers_word_index);
    address_index_reg = index_reg;
    if (index_access_size != 0) {
        if (emit_load_name_into_register(state, index_name, "%r11") != 0) {
            return -1;
        }
        address_index_reg = "%r11";
    }

    if ((total_scale == 1 || total_scale == 2 || total_scale == 4 || total_scale == 8) &&
        byte_offset >= -2147483648LL && byte_offset <= 2147483647LL) {
        char scale_text[8];

        rt_unsigned_to_string((unsigned long long)total_scale, scale_text, sizeof(scale_text));
        rt_copy_string(line, sizeof(line), "leaq ");
        if (byte_offset != 0) {
            if (byte_offset < 0) {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "-");
                rt_unsigned_to_string((unsigned long long)(-(byte_offset + 1LL)) + 1ULL, digits, sizeof(digits));
            } else {
                rt_unsigned_to_string((unsigned long long)byte_offset, digits, sizeof(digits));
            }
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        }
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rax,");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), address_index_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ",");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), scale_text);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "), %rax");
        return emit_instruction(state, line) == 0 ? 1 : -1;
    }

    rt_copy_string(line, sizeof(line), "movq ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), address_index_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %r11");
    if (emit_instruction(state, line) != 0) {
        return -1;
    }
    if (total_scale != 1) {
        while (((1LL << shift) < total_scale) && shift < 62) {
            shift += 1;
        }
        if ((1LL << shift) == total_scale) {
            rt_unsigned_to_string((unsigned long long)shift, digits, sizeof(digits));
            rt_copy_string(line, sizeof(line), "salq $");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %r11");
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
        } else if (total_scale >= -2147483648LL && total_scale <= 2147483647LL) {
            rt_unsigned_to_string((unsigned long long)total_scale, digits, sizeof(digits));
            rt_copy_string(line, sizeof(line), "imulq $");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %r11, %r11");
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
        } else if (emit_load_immediate_register(state, "%rcx", total_scale) != 0 ||
                   emit_instruction(state, "imulq %rcx, %r11") != 0) {
            return -1;
        }
    }
    if (emit_instruction(state, "addq %r11, %rax") != 0) {
        return -1;
    }
    if (byte_offset != 0) {
        if (emit_load_immediate_register(state, "%rcx", byte_offset) != 0 ||
            emit_instruction(state, "addq %rcx, %rax") != 0) {
            return -1;
        }
    }
    return 1;
}

int expr_try_cached_identifier_index(ExprParser *parser,
                                            const char *base_name,
                                            const char *base_type,
                                            int word_index,
                                            char *element_type,
                                            size_t element_type_size) {
    ExprParser snapshot = *parser;
    char index_name[COMPILER_IR_NAME_CAPACITY];
    int element_scale;
    int result;
    long long multiplier = 1;
    long long offset = 0;

    if (backend_is_aarch64(parser->state) || snapshot.current.kind != EXPR_TOKEN_PUNCT ||
        !names_equal(snapshot.current.text, "[")) {
        return 0;
    }
    expr_next(&snapshot);
    if (snapshot.current.kind != EXPR_TOKEN_IDENTIFIER) {
        return 0;
    }
    rt_copy_string(index_name, sizeof(index_name), snapshot.current.text);
    expr_next(&snapshot);
    if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "*")) {
        expr_next(&snapshot);
        if (snapshot.current.kind != EXPR_TOKEN_NUMBER) {
            return 0;
        }
        multiplier = snapshot.current.number_value;
        expr_next(&snapshot);
    }
    if (snapshot.current.kind == EXPR_TOKEN_PUNCT &&
        (names_equal(snapshot.current.text, "+") || names_equal(snapshot.current.text, "-"))) {
        int negative = names_equal(snapshot.current.text, "-");

        expr_next(&snapshot);
        if (snapshot.current.kind != EXPR_TOKEN_NUMBER) {
            return 0;
        }
        offset = snapshot.current.number_value;
        if (negative) {
            offset = -offset;
        }
        expr_next(&snapshot);
    }
    if (snapshot.current.kind != EXPR_TOKEN_PUNCT || !names_equal(snapshot.current.text, "]")) {
        return 0;
    }

    copy_indexed_result_type(base_type, element_type, element_type_size);
    element_scale = array_index_scale(parser->state, base_type, word_index);
    if (multiplier != 1 || offset != 0) {
        result = emit_index_address_cached_affine(parser->state,
                                                  base_name,
                                                  index_name,
                                                  element_scale,
                                                  multiplier,
                                                  offset);
    } else {
        result = emit_x86_cached_index_address(parser->state, base_name, index_name, element_scale);
    }
    if (result <= 0) {
        return result;
    }

    *parser = snapshot;
    expr_next(parser);
    return 1;
}
