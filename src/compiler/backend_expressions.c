/* Expression lowering and initializer emission helpers. */

#include "backend_internal.h"

static int expr_parse_postfix_suffixes(ExprParser *parser, int word_index, int current_is_address, int load_final_address, const char *base_type);
static int emit_binary_op(BackendState *state, const char *op);
static int expr_parse_expression(ExprParser *parser);
static int expr_parse_assignment(ExprParser *parser);
static int expr_parse_lvalue_address(ExprParser *parser, int *byte_sized);
static int emit_offset_address(BackendState *state, const char *name, int offset_bytes);
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
static int emit_named_call(ExprParser *parser, const char *name, const char *object_target_name);
static int emit_move_call_arguments(BackendState *state, int arg_count);
static int emit_cleanup_call_arguments(BackendState *state, int arg_count);
static int emit_indirect_postfix_call(ExprParser *parser, int current_is_address, int byte_sized);

static int sizeof_token_is_type_specifier(const char *text) {
    return names_equal(text, "void") ||
           names_equal(text, "char") ||
           names_equal(text, "short") ||
           names_equal(text, "int") ||
           names_equal(text, "long") ||
           names_equal(text, "signed") ||
           names_equal(text, "unsigned") ||
           names_equal(text, "__int128") ||
           names_equal(text, "struct") ||
           names_equal(text, "union") ||
           names_equal(text, "enum");
}

static void append_sizeof_type_token(char *buffer, size_t buffer_size, const char *separator, const char *token) {
    size_t length;

    if (buffer_size == 0 || token == 0 || token[0] == '\0') {
        return;
    }
    length = rt_strlen(buffer);
    if (separator != 0 && separator[0] != '\0' && length + 1U < buffer_size) {
        rt_copy_string(buffer + length, buffer_size - length, separator);
        length = rt_strlen(buffer);
    }
    if (length < buffer_size) {
        rt_copy_string(buffer + length, buffer_size - length, token);
    }
}

static int emit_identifier_incdec(BackendState *state, const char *name, int delta, int return_old) {
    const char *result_register = backend_is_aarch64(state) ? "x0" : "%rax";
    const char *op = delta > 0 ? "+" : "-";
    int local_index = find_local(state, name);

    if (!backend_is_aarch64(state) && local_index >= 0 && state->locals[local_index].cached_register >= 0) {
        char line[64];
        const char *reg = backend_x86_cached_register_name(state->locals[local_index].cached_register);

        if (reg == 0) {
            return -1;
        }
        if (return_old && emit_load_name(state, name) != 0) {
            return -1;
        }
        rt_copy_string(line, sizeof(line), delta > 0 ? "addq $1, " : "subq $1, ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
        return return_old ? 0 : emit_load_name(state, name);
    }

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
    char current_type[128];

    rt_copy_string(current_type, sizeof(current_type), base_type != 0 ? base_type : "");
    base_type = current_type;

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
            rt_copy_string(current_type, sizeof(current_type), element_type);
            base_type = current_type;
            if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                expr_is_index_or_arrow_text(parser->current.text) &&
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
            rt_copy_string(current_type, sizeof(current_type), member_type);
            base_type = current_type;
            word_index = member_prefers_word_index(member_name, base_type);
            *byte_sized = type_access_size(base_type, word_index);
            current_is_address = 1;
            expr_next(parser);
            if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                names_equal(parser->current.text, "[") &&
                member_result_decays_to_address(member_type)) {
                continue;
            }
            if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                expr_is_index_or_arrow_text(parser->current.text) &&
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
    char current_type[128];

    rt_copy_string(current_type, sizeof(current_type), base_type != 0 ? base_type : "");
    base_type = current_type;

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
            rt_copy_string(current_type, sizeof(current_type), element_type);
            base_type = current_type;
            load_final_address = member_result_decays_to_address(element_type) ? 0 : 1;

            if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                expr_is_index_or_arrow_text(parser->current.text)) {
                if (type_is_pointer_like(element_type) && !member_result_decays_to_address(element_type)) {
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
            rt_copy_string(current_type, sizeof(current_type), member_type);
            base_type = current_type;
            word_index = member_prefers_word_index(member_name, base_type);
            byte_sized = type_access_size(base_type, word_index);
            load_final_address = member_result_decays_to_address(base_type) ? 0 : 1;
            current_is_address = 1;
            expr_next(parser);
            if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                names_equal(parser->current.text, "[") &&
                member_result_decays_to_address(member_type)) {
                continue;
            }
            if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                expr_is_index_or_arrow_text(parser->current.text) &&
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

        if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(")) {
            if (emit_indirect_postfix_call(parser, current_is_address, byte_sized) != 0) {
                return -1;
            }
            current_is_address = 0;
            load_final_address = 0;
            byte_sized = 0;
            word_index = 0;
            current_type[0] = '\0';
            base_type = current_type;
            continue;
        }
        break;
    }

    if (parser->current.kind == EXPR_TOKEN_PUNCT && expr_is_incdec_text(parser->current.text)) {
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
            int known_type_found = 0;

            type_text[0] = '\0';
            identifier_name[0] = '\0';
            while (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "*")) {
                deref_count += 1;
                had_deref = 1;
                expr_next(parser);
            }
            if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
                const char *known_type = lookup_name_type_text(parser->state, parser->current.text);
                known_type_found = known_type != 0 && known_type[0] != '\0';
                rt_copy_string(identifier_name, sizeof(identifier_name), parser->current.text);
                if (known_type_found) {
                    rt_copy_string(type_text, sizeof(type_text), known_type);
                    expr_next(parser);
                } else if (sizeof_token_is_type_specifier(parser->current.text)) {
                    rt_copy_string(type_text, sizeof(type_text), parser->current.text);
                    expr_next(parser);
                    if ((names_equal(type_text, "struct") || names_equal(type_text, "union") || names_equal(type_text, "enum")) &&
                        parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
                        append_sizeof_type_token(type_text, sizeof(type_text), ":", parser->current.text);
                        expr_next(parser);
                    } else {
                        while (parser->current.kind == EXPR_TOKEN_IDENTIFIER &&
                               sizeof_token_is_type_specifier(parser->current.text)) {
                            append_sizeof_type_token(type_text, sizeof(type_text), " ", parser->current.text);
                            expr_next(parser);
                        }
                    }
                    while (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "*")) {
                        append_sizeof_type_token(type_text, sizeof(type_text), "", "*");
                        expr_next(parser);
                    }
                } else {
                    rt_copy_string(type_text, sizeof(type_text), parser->current.text);
                    expr_next(parser);
                }
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
                            backend_copy_sizeof_indexed_type_text(type_text, indexed, sizeof(indexed));
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
            if (type_text[0] != '\0' && identifier_name[0] != '\0' && !saw_suffix && !had_deref && known_type_found) {
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
    ExprParser type_snapshot = *parser;
    char compound_type[128];
    char temp_name[COMPILER_IR_NAME_CAPACITY];
    char digits[32];
    int byte_sized = 0;
    int initializer_index = 0;

    compound_type[0] = '\0';
    expr_infer_result_type(&type_snapshot, compound_type, sizeof(compound_type));
    if (compound_type[0] == '\0') {
        rt_copy_string(compound_type, sizeof(compound_type), "int");
    }

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

    if (allocate_local(parser->state,
                       temp_name,
                       compound_type,
                       decl_slot_size(parser->state, compound_type),
                       decl_requires_object_storage(compound_type),
                       decl_pointer_depth(compound_type),
                       decl_char_based(compound_type),
                       should_prefer_word_index(temp_name, compound_type)) != 0) {
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
        int offset_bytes = 0;
        int access_size = byte_sized;
        const char *member_type = 0;

        if (starts_with(skip_spaces(compound_type), "struct:") ||
            starts_with(skip_spaces(compound_type), "union:")) {
            const char *compound_base_type = skip_spaces(compound_type);
            const char *aggregate_name = compound_base_type + (starts_with(compound_base_type, "struct:") ? 7 : 6);
            int seen_member = 0;
            size_t member_index;

            for (member_index = 0; member_index < parser->state->aggregate_member_count; ++member_index) {
                if (names_equal(parser->state->aggregate_members[member_index].aggregate_name, aggregate_name)) {
                    if (seen_member == initializer_index) {
                        offset_bytes = parser->state->aggregate_members[member_index].offset_bytes;
                        member_type = parser->state->aggregate_members[member_index].type_text;
                        access_size = backend_type_access_size(member_type,
                                                               backend_member_prefers_word_index("", member_type));
                        if (decl_pointer_depth(member_type) > 0) {
                            access_size = 0;
                        }
                        break;
                    }
                    seen_member += 1;
                }
            }
        }

        if (emit_offset_address(parser->state, temp_name, offset_bytes) != 0 ||
            emit_push_value(parser->state) != 0 ||
            expr_parse_assignment(parser) != 0 ||
            emit_pop_address_and_store(parser->state, access_size) != 0) {
            return -1;
        }
        initializer_index += 1;
        while (expr_match_punct(parser, ",")) {
            if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "}")) {
                break;
            }
            offset_bytes = initializer_index * backend_stack_slot_size(parser->state);
            access_size = byte_sized;
            member_type = 0;
            if (starts_with(skip_spaces(compound_type), "struct:") ||
                starts_with(skip_spaces(compound_type), "union:")) {
                const char *compound_base_type = skip_spaces(compound_type);
                const char *aggregate_name = compound_base_type + (starts_with(compound_base_type, "struct:") ? 7 : 6);
                int seen_member = 0;
                size_t member_index;

                for (member_index = 0; member_index < parser->state->aggregate_member_count; ++member_index) {
                    if (names_equal(parser->state->aggregate_members[member_index].aggregate_name, aggregate_name)) {
                        if (seen_member == initializer_index) {
                            offset_bytes = parser->state->aggregate_members[member_index].offset_bytes;
                            member_type = parser->state->aggregate_members[member_index].type_text;
                            access_size = backend_type_access_size(member_type,
                                                                   backend_member_prefers_word_index("", member_type));
                            if (decl_pointer_depth(member_type) > 0) {
                                access_size = 0;
                            }
                            break;
                        }
                        seen_member += 1;
                    }
                }
            }
            if (emit_offset_address(parser->state, temp_name, offset_bytes) != 0 ||
                emit_push_value(parser->state) != 0 ||
                expr_parse_assignment(parser) != 0 ||
                emit_pop_address_and_store(parser->state, access_size) != 0) {
                return -1;
            }
            initializer_index += 1;
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

static int expr_argument_is_object_value(ExprParser *parser) {
    ExprParser type_parser = *parser;
    char type_text[128];
    const char *type;

    type_text[0] = '\0';
    expr_infer_result_type(&type_parser, type_text, sizeof(type_text));
    type = skip_spaces(type_text);
    return (starts_with(type, "struct:") || starts_with(type, "union:")) &&
           !text_contains(type, "*") && !text_contains(type, "[");
}

static int expr_argument_is_direct_object_call(ExprParser *parser) {
    ExprParser lookahead = *parser;
    char name[COMPILER_IR_NAME_CAPACITY];

    if (lookahead.current.kind != EXPR_TOKEN_IDENTIFIER) {
        return 0;
    }
    rt_copy_string(name, sizeof(name), lookahead.current.text);
    expr_next(&lookahead);
    return lookahead.current.kind == EXPR_TOKEN_PUNCT &&
           names_equal(lookahead.current.text, "(") &&
           function_returns_object(parser->state, name);
}

static int expr_parse_object_value_address(ExprParser *parser) {
    if (expr_argument_is_direct_object_call(parser)) {
        return expr_parse_assignment(parser);
    }
    {
        int byte_sized = 0;
        return expr_parse_lvalue_address(parser, &byte_sized);
    }
}

static int expr_parse_call_arguments(ExprParser *parser, int *arg_count_out, int max_arg_count) {
    int tail_count = 0;
    int object_argument = expr_argument_is_object_value(parser);

    if (object_argument) {
        if (expr_parse_object_value_address(parser) != 0) {
            return -1;
        }
    } else {
        if (expr_parse_assignment(parser) != 0) {
            return -1;
        }
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

static int emit_move_call_arguments(BackendState *state, int arg_count) {
    const char x86_arg_regs[][5] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
    const char aarch64_arg_regs[][3] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
    int register_arg_count = backend_register_arg_limit(state);
    int stack_arg_count;
    int stack_slot_size = backend_stack_slot_size(state);
    int reg_index;

    if (arg_count < register_arg_count) {
        register_arg_count = arg_count;
    }
    stack_arg_count = arg_count - register_arg_count;

    if (!backend_is_aarch64(state) && stack_arg_count == 0) {
        for (reg_index = register_arg_count - 1; reg_index >= 0; --reg_index) {
            char line[32];
            rt_copy_string(line, sizeof(line), "popq ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), x86_arg_regs[reg_index]);
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
        }
        return 0;
    }

    for (reg_index = 0; reg_index < register_arg_count; ++reg_index) {
        char line[64];
        unsigned long long offset_bytes =
            (unsigned long long)(stack_arg_count + (register_arg_count - 1 - reg_index)) *
            (unsigned long long)stack_slot_size;
        char offset_text[32];
        const char *reg = backend_is_aarch64(state) ? aarch64_arg_regs[reg_index] : x86_arg_regs[reg_index];

        rt_unsigned_to_string(offset_bytes, offset_text, sizeof(offset_text));
        if (backend_is_aarch64(state)) {
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
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
    }
    return 0;
}

static int parse_atomic_pointer_argument(ExprParser *parser, int *byte_sized) {
    *byte_sized = 0;
    if (expr_match_punct(parser, "&")) {
        return expr_parse_lvalue_address(parser, byte_sized);
    }
    {
        ExprParser pointer_type_parser = *parser;
        char pointer_type[128];
        char deref_type[128];

        pointer_type[0] = '\0';
        deref_type[0] = '\0';
        expr_infer_result_type(&pointer_type_parser, pointer_type, sizeof(pointer_type));
        if (pointer_type[0] != '\0') {
            copy_dereferenced_type(pointer_type, deref_type, sizeof(deref_type));
            if (deref_type[0] != '\0') {
                *byte_sized = type_access_size(deref_type, should_prefer_word_index("", deref_type));
            }
        }
    }
    return expr_parse_assignment(parser);
}

static int emit_atomic_store_to_address(BackendState *state, int byte_sized) {
    if (backend_is_aarch64(state)) {
        backend_set_error(state->backend, "__atomic_store_n lowering is not supported for this backend");
        return -1;
    }
    return emit_pop_to_register(state, "%rax") == 0 &&
           emit_pop_to_register(state, "%rcx") == 0 &&
           emit_store_to_address_register(state, "%rcx", byte_sized) == 0 ? 0 : -1;
}

static int emit_atomic_load_from_address(BackendState *state, int byte_sized) {
    if (backend_is_aarch64(state)) {
        backend_set_error(state->backend, "__atomic_load_n lowering is not supported for this backend");
        return -1;
    }
    return emit_pop_to_register(state, "%rcx") == 0 &&
           emit_load_from_address_into_register(state, "%rcx", "%rax", byte_sized) == 0 ? 0 : -1;
}

static int emit_atomic_exchange_with_address(BackendState *state, int byte_sized) {
    char line[64];
    const char *opcode = "xchgq";
    const char *reg = "%rax";
    int access_size = byte_sized;

    if (backend_is_aarch64(state)) {
        backend_set_error(state->backend, "__atomic_exchange_n lowering is not supported for this backend");
        return -1;
    }
    if (access_size == 0) {
        access_size = backend_stack_slot_size(state);
    } else if (access_size < 0) {
        access_size = -access_size;
    }
    if (access_size == 1) {
        opcode = "xchgb";
        reg = "%al";
    } else if (access_size == 2) {
        opcode = "xchgw";
        reg = "%ax";
    } else if (access_size == 4) {
        opcode = "xchgl";
        reg = "%eax";
    }

    if (emit_pop_to_register(state, "%rax") != 0 ||
        emit_pop_to_register(state, "%rcx") != 0) {
        return -1;
    }
    rt_copy_string(line, sizeof(line), opcode);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), " ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", (%rcx)");
    return emit_instruction(state, line);
}

static int emit_atomic_fetch_add_with_address(BackendState *state, int byte_sized, int negate_value) {
    char line[64];
    const char *opcode = "lock xaddq";
    const char *reg = "%rax";
    int access_size = byte_sized;

    if (backend_is_aarch64(state)) {
        backend_set_error(state->backend, "atomic fetch-add lowering is not supported for this backend");
        return -1;
    }
    if (access_size == 0) {
        access_size = backend_stack_slot_size(state);
    } else if (access_size < 0) {
        access_size = -access_size;
    }
    if (access_size == 1) {
        opcode = "lock xaddb";
        reg = "%al";
    } else if (access_size == 2) {
        opcode = "lock xaddw";
        reg = "%ax";
    } else if (access_size == 4) {
        opcode = "lock xaddl";
        reg = "%eax";
    }
    if (emit_pop_to_register(state, "%rax") != 0 ||
        emit_pop_to_register(state, "%rcx") != 0) {
        return -1;
    }
    if (negate_value && emit_instruction(state, "negq %rax") != 0) {
        return -1;
    }
    rt_copy_string(line, sizeof(line), opcode);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), " ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", (%rcx)");
    return emit_instruction(state, line);
}

static int emit_atomic_compare_exchange_with_address(BackendState *state, int byte_sized) {
    char line[64];
    const char *opcode = "lock cmpxchgq";
    const char *desired_reg = "%rdx";
    int access_size = byte_sized;

    if (backend_is_aarch64(state)) {
        backend_set_error(state->backend, "__atomic_compare_exchange_n lowering is not supported for this backend");
        return -1;
    }
    if (access_size == 0) {
        access_size = backend_stack_slot_size(state);
    } else if (access_size < 0) {
        access_size = -access_size;
    }
    if (access_size == 1) {
        opcode = "lock cmpxchgb";
        desired_reg = "%dl";
    } else if (access_size == 2) {
        opcode = "lock cmpxchgw";
        desired_reg = "%dx";
    } else if (access_size == 4) {
        opcode = "lock cmpxchgl";
        desired_reg = "%edx";
    }
    if (emit_pop_to_register(state, "%rdx") != 0 ||
        emit_pop_to_register(state, "%rsi") != 0 ||
        emit_pop_to_register(state, "%rcx") != 0 ||
        emit_load_from_address_into_register(state, "%rsi", "%rax", byte_sized) != 0) {
        return -1;
    }
    rt_copy_string(line, sizeof(line), opcode);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), " ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), desired_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", (%rcx)");
    if (emit_instruction(state, line) != 0 ||
        emit_instruction(state, "sete %r11b") != 0 ||
        emit_store_to_address_register(state, "%rsi", byte_sized) != 0 ||
        emit_instruction(state, "movzbq %r11b, %rax") != 0) {
        return -1;
    }
    return 0;
}

static int emit_atomic_address_value_builtin(ExprParser *parser, const char *name) {
    int byte_sized = 0;
    int is_store = names_equal(name, "__atomic_store_n");
    int is_exchange = names_equal(name, "__atomic_exchange_n");
    int is_fetch_add = names_equal(name, "__atomic_fetch_add");
    int is_fetch_sub = names_equal(name, "__atomic_fetch_sub");

    if (!is_store && !is_exchange && !is_fetch_add && !is_fetch_sub) {
        return 0;
    }
    if (expr_expect_punct(parser, "(") != 0) {
        return -1;
    }
    if (parse_atomic_pointer_argument(parser, &byte_sized) != 0 ||
        emit_push_value(parser->state) != 0 ||
        expr_expect_punct(parser, ",") != 0 ||
        expr_parse_assignment(parser) != 0 ||
        emit_push_value(parser->state) != 0 ||
        expr_expect_punct(parser, ",") != 0 ||
        expr_parse_assignment(parser) != 0 ||
        expr_expect_punct(parser, ")") != 0) {
        return -1;
    }
    backend_invalidate_block_cache(parser->state);
    if (is_store) {
        return emit_atomic_store_to_address(parser->state, byte_sized) == 0 ? 1 : -1;
    }
    if (is_exchange) {
        return emit_atomic_exchange_with_address(parser->state, byte_sized) == 0 ? 1 : -1;
    }
    return emit_atomic_fetch_add_with_address(parser->state, byte_sized, is_fetch_sub) == 0 ? 1 : -1;
}

static int emit_atomic_load_builtin(ExprParser *parser, const char *name) {
    int byte_sized = 0;

    if (!names_equal(name, "__atomic_load_n")) {
        return 0;
    }
    if (expr_expect_punct(parser, "(") != 0 ||
        parse_atomic_pointer_argument(parser, &byte_sized) != 0 ||
        emit_push_value(parser->state) != 0 ||
        expr_expect_punct(parser, ",") != 0 ||
        expr_parse_assignment(parser) != 0 ||
        expr_expect_punct(parser, ")") != 0) {
        return -1;
    }
    backend_invalidate_block_cache(parser->state);
    return emit_atomic_load_from_address(parser->state, byte_sized) == 0 ? 1 : -1;
}

static int emit_atomic_compare_exchange_builtin(ExprParser *parser, const char *name) {
    int object_byte_sized = 0;
    int expected_byte_sized = 0;

    if (!names_equal(name, "__atomic_compare_exchange_n")) {
        return 0;
    }
    if (expr_expect_punct(parser, "(") != 0 ||
        parse_atomic_pointer_argument(parser, &object_byte_sized) != 0 ||
        emit_push_value(parser->state) != 0 ||
        expr_expect_punct(parser, ",") != 0 ||
        parse_atomic_pointer_argument(parser, &expected_byte_sized) != 0 ||
        emit_push_value(parser->state) != 0 ||
        expr_expect_punct(parser, ",") != 0 ||
        expr_parse_assignment(parser) != 0 ||
        emit_push_value(parser->state) != 0 ||
        expr_expect_punct(parser, ",") != 0 ||
        expr_parse_assignment(parser) != 0 ||
        expr_expect_punct(parser, ",") != 0 ||
        expr_parse_assignment(parser) != 0 ||
        expr_expect_punct(parser, ",") != 0 ||
        expr_parse_assignment(parser) != 0 ||
        expr_expect_punct(parser, ")") != 0) {
        return -1;
    }
    backend_invalidate_block_cache(parser->state);
    return emit_atomic_compare_exchange_with_address(parser->state, object_byte_sized) == 0 ? 1 : -1;
}

static int emit_sync_synchronize_builtin(ExprParser *parser, const char *name) {
    if (!names_equal(name, "__sync_synchronize")) {
        return 0;
    }
    if (expr_expect_punct(parser, "(") != 0 ||
        expr_expect_punct(parser, ")") != 0) {
        return -1;
    }
    backend_invalidate_block_cache(parser->state);
    return emit_instruction(parser->state, backend_is_aarch64(parser->state) ? "dmb ish" : "mfence") == 0 ? 1 : -1;
}

static int emit_cleanup_call_arguments(BackendState *state, int arg_count) {
    char cleanup[64];
    int register_arg_count = backend_register_arg_limit(state);
    int stack_arg_count;
    unsigned long long cleanup_bytes;
    char digits[32];

    if (arg_count <= 0) {
        return 0;
    }
    if (arg_count < register_arg_count) {
        register_arg_count = arg_count;
    }
    stack_arg_count = arg_count - register_arg_count;
    if (!backend_is_aarch64(state) && stack_arg_count == 0) {
        return 0;
    }
    cleanup_bytes = (unsigned long long)(arg_count + (backend_is_aarch64(state) ? 0 : stack_arg_count)) *
                    (unsigned long long)backend_stack_slot_size(state);
    rt_unsigned_to_string(cleanup_bytes, digits, sizeof(digits));
    if (backend_is_aarch64(state)) {
        rt_copy_string(cleanup, sizeof(cleanup), "add sp, sp, #");
        rt_copy_string(cleanup + rt_strlen(cleanup), sizeof(cleanup) - rt_strlen(cleanup), digits);
    } else {
        rt_copy_string(cleanup, sizeof(cleanup), "addq $");
        rt_copy_string(cleanup + rt_strlen(cleanup), sizeof(cleanup) - rt_strlen(cleanup), digits);
        rt_copy_string(cleanup + rt_strlen(cleanup), sizeof(cleanup) - rt_strlen(cleanup), ", %rsp");
    }
    return emit_instruction(state, cleanup);
}

static int emit_indirect_postfix_call(ExprParser *parser, int current_is_address, int byte_sized) {
    int arg_count = 0;
    int register_arg_limit = backend_register_arg_limit(parser->state);

    if (current_is_address) {
        if (emit_load_from_address_register(parser->state,
                                            backend_is_aarch64(parser->state) ? "x0" : "%rax",
                                            byte_sized) != 0) {
            return -1;
        }
    }
    if (emit_push_value(parser->state) != 0) {
        return -1;
    }

    (void)expr_match_punct(parser, "(");
    if (!(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")"))) {
        if (expr_parse_call_arguments(parser, &arg_count, 32) != 0) {
            return -1;
        }
    }
    if (expr_expect_punct(parser, ")") != 0) {
        return -1;
    }

    if (arg_count > register_arg_limit) {
        backend_set_error(parser->state->backend, "indirect calls with stack arguments are not supported");
        return -1;
    }
    if (emit_move_call_arguments(parser->state, arg_count) != 0) {
        return -1;
    }

    if (backend_is_aarch64(parser->state)) {
        char line[64];
        char offset_text[32];
        unsigned long long offset_bytes = (unsigned long long)arg_count *
                                          (unsigned long long)backend_stack_slot_size(parser->state);
        rt_unsigned_to_string(offset_bytes, offset_text, sizeof(offset_text));
        rt_copy_string(line, sizeof(line), "ldr x16, [sp, #");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
        if (emit_instruction(parser->state, line) != 0 ||
            emit_instruction(parser->state, "blr x16") != 0 ||
            emit_cleanup_call_arguments(parser->state, arg_count + 1) != 0) {
            return -1;
        }
    } else {
        if (emit_instruction(parser->state, "popq %r11") != 0 ||
            emit_instruction(parser->state, "xor %eax, %eax") != 0 ||
            emit_instruction(parser->state, "call *%r11") != 0) {
            return -1;
        }
    }
    backend_invalidate_block_cache(parser->state);
    return 0;
}

static int emit_named_call(ExprParser *parser, const char *name, const char *object_target_name) {
    int arg_count = 0;
    int hidden_arg_count = 0;
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
    if (emit_move_call_arguments(parser->state, arg_count) != 0) {
        return -1;
    }
    if (!returns_object && !backend_is_aarch64(parser->state) &&
        ((names_equal(name, "memcpy") && arg_count == 3) ||
         (names_equal(name, "memset") && arg_count == 3))) {
        if (names_equal(name, "memcpy")) {
            if (emit_instruction(parser->state, "movq %rdi, %rax") != 0 ||
                emit_instruction(parser->state, "movq %rdx, %rcx") != 0 ||
                emit_instruction(parser->state, "rep movsb") != 0) {
                return -1;
            }
        } else {
            if (emit_instruction(parser->state, "movq %rdi, %r8") != 0 ||
                emit_instruction(parser->state, "movl %esi, %eax") != 0 ||
                emit_instruction(parser->state, "movq %rdx, %rcx") != 0 ||
                emit_instruction(parser->state, "rep stosb") != 0 ||
                emit_instruction(parser->state, "movq %r8, %rax") != 0) {
                return -1;
            }
        }
        backend_invalidate_block_cache(parser->state);
        if (emit_cleanup_call_arguments(parser->state, arg_count) != 0) {
            return -1;
        }
        return expr_parse_postfix_suffixes(parser, 0, 0, 0, 0);
    }
    if (!backend_is_aarch64(parser->state)) {
        int stack_index;
        int register_arg_count = backend_register_arg_limit(parser->state);
        int stack_arg_count;
        int stack_slot_size = backend_stack_slot_size(parser->state);

        if (arg_count < register_arg_count) {
            register_arg_count = arg_count;
        }
        stack_arg_count = arg_count - register_arg_count;
        if (stack_arg_count > 0) {
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
    }

    {
        char line[96];
        char symbol[COMPILER_IR_NAME_CAPACITY];
        format_symbol_name(parser->state, name, symbol, sizeof(symbol));
        if (!backend_is_aarch64(parser->state) &&
            emit_instruction(parser->state, "xor %eax, %eax") != 0) {
            return -1;
        }
        backend_invalidate_block_cache(parser->state);
        rt_copy_string(line, sizeof(line), backend_is_aarch64(parser->state) ? "bl " : "call ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
        if (emit_instruction(parser->state, line) != 0) {
            return -1;
        }
    }

    if (emit_cleanup_call_arguments(parser->state, arg_count) != 0) {
        return -1;
    }

    if (returns_object) {
        if (emit_address_of_name(parser->state, result_object) != 0) {
            return -1;
        }
        return expr_parse_postfix_suffixes(parser, 0, 1, 0, return_type);
    }

    return expr_parse_postfix_suffixes(parser, 0, 0, 0, return_type);
}

static int emit_named_object_call_to_pushed_address(ExprParser *parser, const char *name) {
    int arg_count = 1;

    (void)expr_match_punct(parser, "(");
    if (!(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")"))) {
        int explicit_arg_count = 0;
        if (expr_parse_call_arguments(parser, &explicit_arg_count, 31) != 0) {
            return -1;
        }
        arg_count += explicit_arg_count;
    }
    if (expr_expect_punct(parser, ")") != 0) {
        return -1;
    }

    if (emit_move_call_arguments(parser->state, arg_count) != 0) {
        return -1;
    }
    if (!backend_is_aarch64(parser->state)) {
        int stack_index;
        int register_arg_count = backend_register_arg_limit(parser->state);
        int stack_arg_count;
        int stack_slot_size = backend_stack_slot_size(parser->state);

        if (arg_count < register_arg_count) {
            register_arg_count = arg_count;
        }
        stack_arg_count = arg_count - register_arg_count;
        if (stack_arg_count > 0) {
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
    }

    {
        char line[96];
        char symbol[COMPILER_IR_NAME_CAPACITY];
        format_symbol_name(parser->state, name, symbol, sizeof(symbol));
        if (!backend_is_aarch64(parser->state) &&
            emit_instruction(parser->state, "xor %eax, %eax") != 0) {
            return -1;
        }
        backend_invalidate_block_cache(parser->state);
        rt_copy_string(line, sizeof(line), backend_is_aarch64(parser->state) ? "bl " : "call ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
        if (emit_instruction(parser->state, line) != 0) {
            return -1;
        }
    }

    return emit_cleanup_call_arguments(parser->state, arg_count);
}

static int expr_parse_primary(ExprParser *parser) {
    if (parser->current.kind == EXPR_TOKEN_NUMBER || parser->current.kind == EXPR_TOKEN_CHAR) {
        long long value = parser->current.number_value;
        expr_next(parser);
        return emit_load_immediate(parser->state, value);
    }

    if (parser->current.kind == EXPR_TOKEN_STRING) {
        int result = emit_load_string_literal_bytes(parser->state, parser->current.text, parser->current.text_length);
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
                int atomic_builtin = emit_atomic_address_value_builtin(parser, name);
                if (atomic_builtin != 0) {
                    return atomic_builtin < 0 ? -1 : 0;
                }
                atomic_builtin = emit_atomic_load_builtin(parser, name);
                if (atomic_builtin != 0) {
                    return atomic_builtin < 0 ? -1 : 0;
                }
                atomic_builtin = emit_atomic_compare_exchange_builtin(parser, name);
                if (atomic_builtin != 0) {
                    return atomic_builtin < 0 ? -1 : 0;
                }
                atomic_builtin = emit_sync_synchronize_builtin(parser, name);
                if (atomic_builtin != 0) {
                    return atomic_builtin < 0 ? -1 : 0;
                }
                return emit_named_call(parser, name, 0);
            }

            {
                int arg_count = 0;

                (void)expr_match_punct(parser, "(");
                if (!(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")"))) {
                    if (expr_parse_call_arguments(parser, &arg_count, 32) != 0) {
                        return -1;
                    }
                }
                if (expr_expect_punct(parser, ")") != 0) {
                    return -1;
                }

                if (emit_move_call_arguments(parser->state, arg_count) != 0) {
                    return -1;
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
                if (emit_cleanup_call_arguments(parser->state, arg_count) != 0) {
                    return -1;
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

        if (!saw_structish_suffix && parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "[")) {
            char element_type[128];
            const char *base_type = lookup_name_type_text(parser->state, name);
            int handled = expr_try_cached_identifier_index(parser,
                                                           name,
                                                           base_type,
                                                           name_prefers_word_index(parser->state, name),
                                                           element_type,
                                                           sizeof(element_type));

            if (handled < 0) {
                return -1;
            }
            if (handled > 0) {
                return expr_parse_postfix_suffixes(parser,
                                                   0,
                                                   1,
                                                   member_result_decays_to_address(element_type) ? 0 : 1,
                                                   element_type);
            }
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
        ExprParser grouped_type_parser = *parser;
        char grouped_type[128];

        grouped_type[0] = '\0';
        expr_infer_result_type(&grouped_type_parser, grouped_type, sizeof(grouped_type));
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
        return expr_parse_postfix_suffixes(parser, 0, 0, 0, grouped_type[0] != '\0' ? grouped_type : 0);
    }

    backend_set_error(parser->state->backend, "unsupported primary expression in backend");
    return -1;
}

static int expr_parse_unary(ExprParser *parser) {
    char op[4];

    if (expr_looks_like_cast(parser)) {
        int saw_unsigned = 0;
        int saw_char = 0;
        int saw_short = 0;
        int saw_int = 0;
        int saw_pointer = 0;
        int cast_depth = 1;
        expr_next(parser);
        while (parser->current.kind != EXPR_TOKEN_EOF && cast_depth > 0) {
            if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(")) {
                cast_depth += 1;
            } else if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")")) {
                cast_depth -= 1;
            } else if (cast_depth == 1 && parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
                if (names_equal(parser->current.text, "unsigned")) {
                    saw_unsigned = 1;
                } else if (names_equal(parser->current.text, "char")) {
                    saw_char = 1;
                } else if (names_equal(parser->current.text, "short")) {
                    saw_short = 1;
                } else if (names_equal(parser->current.text, "int")) {
                    saw_int = 1;
                }
            } else if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                       names_equal(parser->current.text, "*")) {
                saw_pointer = 1;
            }
            expr_next(parser);
        }
        if (cast_depth != 0) {
            backend_set_error(parser->state->backend, "unterminated cast expression in backend");
            return -1;
        }
        if (expr_parse_unary(parser) != 0) {
            return -1;
        }
        if (!saw_pointer && saw_unsigned && saw_char) {
            return emit_instruction(parser->state, backend_is_aarch64(parser->state) ? "and x0, x0, #0xff" : "movzbl %al, %eax");
        }
        if (!saw_pointer && saw_unsigned && saw_short) {
            return emit_instruction(parser->state, backend_is_aarch64(parser->state) ? "and x0, x0, #0xffff" : "movzwl %ax, %eax");
        }
        if (!saw_pointer && saw_unsigned && saw_int) {
            return emit_instruction(parser->state, backend_is_aarch64(parser->state) ? "and x0, x0, #0xffffffff" : "movl %eax, %eax");
        }
        return 0;
    }

    if (parser->current.kind == EXPR_TOKEN_PUNCT &&
        expr_is_unary_prefix_text(parser->current.text)) {
        int byte_load = 0;
        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);

        if (names_equal(op, "+")) {
            return expr_parse_unary(parser);
        }

        if (expr_is_incdec_text(op)) {
            int byte_sized = 0;
            int delta = names_equal(op, "++") ? 1 : -1;
            if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
                char name[COMPILER_IR_NAME_CAPACITY];
                int local_index;

                rt_copy_string(name, sizeof(name), parser->current.text);
                local_index = find_local(parser->state, name);
                if (!backend_is_aarch64(parser->state) && local_index >= 0 &&
                    parser->state->locals[local_index].cached_register >= 0) {
                    expr_next(parser);
                    return emit_identifier_incdec(parser->state, name, delta, 0);
                }
            }
            if (expr_parse_lvalue_address(parser, &byte_sized) != 0) {
                return -1;
            }
            return emit_address_incdec(parser->state, byte_sized, delta, 0);
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

static int emit_binary_op_mode(BackendState *state, const char *op, int use_unsigned, int unsigned_32bit_result) {
    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "mov x2, x0") != 0 ||
            emit_instruction(state, "ldr x1, [sp]") != 0 ||
            emit_instruction(state, "add sp, sp, #16") != 0) {
            return -1;
        }

        if (names_equal(op, "+")) return emit_instruction(state, "add x0, x1, x2") == 0 && (!unsigned_32bit_result || emit_instruction(state, "uxtw x0, w0") == 0) ? 0 : -1;
        if (names_equal(op, "-")) return emit_instruction(state, "sub x0, x1, x2") == 0 && (!unsigned_32bit_result || emit_instruction(state, "uxtw x0, w0") == 0) ? 0 : -1;
        if (names_equal(op, "*")) return emit_instruction(state, "mul x0, x1, x2") == 0 && (!unsigned_32bit_result || emit_instruction(state, "uxtw x0, w0") == 0) ? 0 : -1;
        if (names_equal(op, "&")) return emit_instruction(state, "and x0, x1, x2") == 0 && (!unsigned_32bit_result || emit_instruction(state, "uxtw x0, w0") == 0) ? 0 : -1;
        if (names_equal(op, "|")) return emit_instruction(state, "orr x0, x1, x2") == 0 && (!unsigned_32bit_result || emit_instruction(state, "uxtw x0, w0") == 0) ? 0 : -1;
        if (names_equal(op, "^")) return emit_instruction(state, "eor x0, x1, x2") == 0 && (!unsigned_32bit_result || emit_instruction(state, "uxtw x0, w0") == 0) ? 0 : -1;
        if (names_equal(op, "<<")) return emit_instruction(state, "lsl x0, x1, x2") == 0 && (!unsigned_32bit_result || emit_instruction(state, "uxtw x0, w0") == 0) ? 0 : -1;
        if (names_equal(op, ">>")) return emit_instruction(state, "lsr x0, x1, x2") == 0 && (!unsigned_32bit_result || emit_instruction(state, "uxtw x0, w0") == 0) ? 0 : -1;
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
        if (emit_instruction(state, "movq %rax, %rcx") != 0 || emit_pop_to_register(state, "%rax") != 0) {
            return -1;
        }

        if (names_equal(op, "+")) return emit_instruction(state, "addq %rcx, %rax") == 0 && (!unsigned_32bit_result || emit_instruction(state, "movl %eax, %eax") == 0) ? 0 : -1;
        if (names_equal(op, "-")) return emit_instruction(state, "subq %rcx, %rax") == 0 && (!unsigned_32bit_result || emit_instruction(state, "movl %eax, %eax") == 0) ? 0 : -1;
        if (names_equal(op, "*")) return emit_instruction(state, "imulq %rcx, %rax") == 0 && (!unsigned_32bit_result || emit_instruction(state, "movl %eax, %eax") == 0) ? 0 : -1;
        if (names_equal(op, "&")) return emit_instruction(state, "andq %rcx, %rax") == 0 && (!unsigned_32bit_result || emit_instruction(state, "movl %eax, %eax") == 0) ? 0 : -1;
        if (names_equal(op, "|")) return emit_instruction(state, "orq %rcx, %rax") == 0 && (!unsigned_32bit_result || emit_instruction(state, "movl %eax, %eax") == 0) ? 0 : -1;
        if (names_equal(op, "^")) return emit_instruction(state, "xorq %rcx, %rax") == 0 && (!unsigned_32bit_result || emit_instruction(state, "movl %eax, %eax") == 0) ? 0 : -1;
        if (names_equal(op, "<<")) return emit_instruction(state, "salq %cl, %rax") == 0 && (!unsigned_32bit_result || emit_instruction(state, "movl %eax, %eax") == 0) ? 0 : -1;
        if (names_equal(op, ">>")) return emit_instruction(state, "shrq %cl, %rax") == 0 && (!unsigned_32bit_result || emit_instruction(state, "movl %eax, %eax") == 0) ? 0 : -1;
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
    return emit_binary_op_mode(state, op, 0, 0);
}

static int emit_compare_immediate(BackendState *state, const char *op, long long value, int use_unsigned) {
    char line[96];
    char digits[32];

    if (backend_is_aarch64(state) || value < -2147483648LL || value > 2147483647LL) {
        return 0;
    }
    if (value < 0) {
        rt_copy_string(digits, sizeof(digits), "-");
        rt_unsigned_to_string((unsigned long long)(-(value + 1LL)) + 1ULL, digits + 1, sizeof(digits) - 1U);
    } else {
        rt_unsigned_to_string((unsigned long long)value, digits, sizeof(digits));
    }
    rt_copy_string(line, sizeof(line), "cmpq $");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rax");
    if (emit_instruction(state, line) != 0) {
        return -1;
    }
    if (use_unsigned && names_equal(op, "<")) return emit_set_condition(state, "b") == 0 ? 1 : -1;
    if (use_unsigned && names_equal(op, "<=")) return emit_set_condition(state, "be") == 0 ? 1 : -1;
    if (use_unsigned && names_equal(op, ">")) return emit_set_condition(state, "a") == 0 ? 1 : -1;
    if (use_unsigned && names_equal(op, ">=")) return emit_set_condition(state, "ae") == 0 ? 1 : -1;
    if (names_equal(op, "<")) return emit_set_condition(state, "l") == 0 ? 1 : -1;
    if (names_equal(op, "<=")) return emit_set_condition(state, "le") == 0 ? 1 : -1;
    if (names_equal(op, ">")) return emit_set_condition(state, "g") == 0 ? 1 : -1;
    if (names_equal(op, ">=")) return emit_set_condition(state, "ge") == 0 ? 1 : -1;
    return 0;
}

static int emit_shift_immediate(BackendState *state, const char *op, long long amount) {
    char line[64];
    char digits[32];

    if (amount < 0) {
        return -1;
    }
    amount &= 63LL;
    rt_unsigned_to_string((unsigned long long)amount, digits, sizeof(digits));
    if (backend_is_aarch64(state)) {
        rt_copy_string(line, sizeof(line), names_equal(op, "<<") ? "lsl x0, x0, #" : "lsr x0, x0, #");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        return emit_instruction(state, line);
    }
    rt_copy_string(line, sizeof(line), names_equal(op, "<<") ? "salq $" : "shrq $");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rax");
    return emit_instruction(state, line);
}

static int emit_multiply_immediate(BackendState *state, long long amount, int unsigned_32bit_result) {
    char line[96];
    char digits[32];
    unsigned long long magnitude;
    int shift = 0;

    if (amount == 0) {
        return emit_instruction(state, backend_is_aarch64(state) ? "mov x0, #0" : "xor %eax, %eax");
    }
    if (amount == 1) {
        return unsigned_32bit_result
                   ? emit_instruction(state, backend_is_aarch64(state) ? "uxtw x0, w0" : "movl %eax, %eax")
                   : 0;
    }
    if (amount > 0) {
        magnitude = (unsigned long long)amount;
        while (((1ULL << (unsigned int)shift) < magnitude) && shift < 62) {
            shift += 1;
        }
        if ((1ULL << (unsigned int)shift) == magnitude) {
            rt_unsigned_to_string((unsigned long long)shift, digits, sizeof(digits));
            if (backend_is_aarch64(state)) {
                rt_copy_string(line, sizeof(line), "lsl x0, x0, #");
            } else {
                rt_copy_string(line, sizeof(line), "salq $");
            }
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
            if (!backend_is_aarch64(state)) {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rax");
            }
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            if (unsigned_32bit_result) {
                return emit_instruction(state, backend_is_aarch64(state) ? "uxtw x0, w0" : "movl %eax, %eax");
            }
            return 0;
        }
    }

    if (amount < 0) {
        char magnitude_text[32];
        rt_unsigned_to_string((unsigned long long)(-(amount + 1LL)) + 1ULL, magnitude_text, sizeof(magnitude_text));
        rt_copy_string(digits, sizeof(digits), "-");
        rt_copy_string(digits + rt_strlen(digits), sizeof(digits) - rt_strlen(digits), magnitude_text);
    } else {
        rt_unsigned_to_string((unsigned long long)amount, digits, sizeof(digits));
    }
    if (backend_is_aarch64(state)) {
        if (emit_load_immediate_register(state, "x2", amount) != 0) {
            return -1;
        }
        if (emit_instruction(state, "mul x0, x0, x2") != 0) {
            return -1;
        }
        return unsigned_32bit_result ? emit_instruction(state, "uxtw x0, w0") : 0;
    }
    if (amount < -2147483648LL || amount > 2147483647LL) {
        if (emit_load_immediate_register(state, "%rcx", amount) != 0) {
            return -1;
        }
        if (emit_instruction(state, "imulq %rcx, %rax") != 0) {
            return -1;
        }
    } else {
        rt_copy_string(line, sizeof(line), "imulq $");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rax, %rax");
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
    }
    return unsigned_32bit_result ? emit_instruction(state, "movl %eax, %eax") : 0;
}

static int emit_bitand_immediate(BackendState *state, long long amount) {
    char line[96];
    char digits[32];

    if (backend_is_aarch64(state)) {
        if (emit_load_immediate_register(state, "x2", amount) != 0) {
            return -1;
        }
        return emit_instruction(state, "and x0, x0, x2");
    }

    if (amount >= -2147483648LL && amount <= 2147483647LL) {
        if (amount < 0) {
            char magnitude_text[32];
            rt_unsigned_to_string((unsigned long long)(-(amount + 1LL)) + 1ULL, magnitude_text, sizeof(magnitude_text));
            rt_copy_string(digits, sizeof(digits), "-");
            rt_copy_string(digits + rt_strlen(digits), sizeof(digits) - rt_strlen(digits), magnitude_text);
        } else {
            rt_unsigned_to_string((unsigned long long)amount, digits, sizeof(digits));
        }
        rt_copy_string(line, sizeof(line), "andq $");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rax");
        return emit_instruction(state, line);
    }

    if (emit_load_immediate_register(state, "%rcx", amount) != 0) {
        return -1;
    }
    return emit_instruction(state, "andq %rcx, %rax");
}

static int emit_add_immediate(BackendState *state, long long amount, int unsigned_32bit_result) {
    char line[96];
    char digits[32];
    long long magnitude = amount;
    const char *op;

    if (amount == 0) {
        return unsigned_32bit_result
                   ? emit_instruction(state, backend_is_aarch64(state) ? "uxtw x0, w0" : "movl %eax, %eax")
                   : 0;
    }
    if (backend_is_aarch64(state)) {
        if (emit_load_immediate_register(state, "x2", amount) != 0 ||
            emit_instruction(state, "add x0, x0, x2") != 0) {
            return -1;
        }
        return unsigned_32bit_result ? emit_instruction(state, "uxtw x0, w0") : 0;
    }

    op = amount < 0 ? "subq $" : "addq $";
    if (magnitude < 0) {
        magnitude = -(magnitude + 1LL) + 1LL;
    }
    if (magnitude <= 2147483647LL) {
        rt_unsigned_to_string((unsigned long long)magnitude, digits, sizeof(digits));
        rt_copy_string(line, sizeof(line), op);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rax");
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
    } else {
        if (emit_load_immediate_register(state, "%rcx", amount) != 0 ||
            emit_instruction(state, "addq %rcx, %rax") != 0) {
            return -1;
        }
    }
    return unsigned_32bit_result ? emit_instruction(state, "movl %eax, %eax") : 0;
}

static int expr_try_match_punct(ExprParser *parser, const char *text) {
    if (parser->current.kind != EXPR_TOKEN_PUNCT || !names_equal(parser->current.text, text)) {
        return -1;
    }
    expr_next(parser);
    return 0;
}

static int expr_try_parse_grouped_identifier(ExprParser *parser, char *name, size_t name_size) {
    int grouped = 0;

    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(")) {
        ExprParser snapshot = *parser;
        expr_next(&snapshot);
        if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
            expr_next(&snapshot);
            if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, ")")) {
                grouped = 1;
                expr_next(parser);
            }
        }
    }
    if (parser->current.kind != EXPR_TOKEN_IDENTIFIER) {
        return -1;
    }
    rt_copy_string(name, name_size, parser->current.text);
    expr_next(parser);
    if (grouped && expr_try_match_punct(parser, ")") != 0) {
        return -1;
    }
    return 0;
}

static int expr_try_parse_grouped_number(ExprParser *parser, long long *value_out) {
    int grouped = 0;

    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(")) {
        grouped = 1;
        expr_next(parser);
    }
    if (parser->current.kind != EXPR_TOKEN_NUMBER) {
        return -1;
    }
    *value_out = parser->current.number_value;
    expr_next(parser);
    if (grouped && expr_try_match_punct(parser, ")") != 0) {
        return -1;
    }
    return 0;
}

static int expr_try_parse_rotr32_identifier(ExprParser *parser) {
    ExprParser snapshot = *parser;
    char name[COMPILER_IR_NAME_CAPACITY];
    char right_name[COMPILER_IR_NAME_CAPACITY];
    long long amount;
    long long width;
    long long right_amount;
    char line[64];
    char digits[32];

    if (expr_try_match_punct(&snapshot, "(") != 0 ||
        expr_try_match_punct(&snapshot, "(") != 0 ||
        expr_try_parse_grouped_identifier(&snapshot, name, sizeof(name)) != 0) {
        return 0;
    }
    if (expr_try_match_punct(&snapshot, ")") != 0 ||
        expr_try_match_punct(&snapshot, ">>") != 0 ||
        expr_try_parse_grouped_number(&snapshot, &amount) != 0) {
        return 0;
    }
    if (amount <= 0 || amount >= 32) {
        return 0;
    }
    if (expr_try_match_punct(&snapshot, ")") != 0 ||
        expr_try_match_punct(&snapshot, "|") != 0 ||
        expr_try_match_punct(&snapshot, "(") != 0 ||
        expr_try_match_punct(&snapshot, "(") != 0 ||
        expr_try_parse_grouped_identifier(&snapshot, right_name, sizeof(right_name)) != 0 ||
        !names_equal(right_name, name)) {
        return 0;
    }
    if (expr_try_match_punct(&snapshot, ")") != 0 ||
        expr_try_match_punct(&snapshot, "<<") != 0 ||
        expr_try_match_punct(&snapshot, "(") != 0 ||
        expr_try_parse_grouped_number(&snapshot, &width) != 0 ||
        width != 32) {
        return 0;
    }
    if (expr_try_match_punct(&snapshot, "-") != 0 ||
        expr_try_parse_grouped_number(&snapshot, &right_amount) != 0 ||
        right_amount != amount) {
        return 0;
    }
    if (expr_try_match_punct(&snapshot, ")") != 0 ||
        expr_try_match_punct(&snapshot, ")") != 0) {
        return 0;
    }
    if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "^")) {
        return 0;
    }

    if (emit_load_name(parser->state, name) != 0) {
        return -1;
    }
    rt_unsigned_to_string((unsigned long long)amount, digits, sizeof(digits));
    if (backend_is_aarch64(parser->state)) {
        rt_copy_string(line, sizeof(line), "ror w0, w0, #");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        if (emit_instruction(parser->state, line) != 0 ||
            emit_instruction(parser->state, "uxtw x0, w0") != 0) {
            return -1;
        }
    } else {
        rt_copy_string(line, sizeof(line), "rorl $");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %eax");
        if (emit_instruction(parser->state, line) != 0) {
            return -1;
        }
    }
    *parser = snapshot;
    return 1;
}

static int expr_parse_constant_additive(ExprParser *parser, long long *value_out);

static int expr_parse_constant_primary(ExprParser *parser, long long *value_out) {
    if (parser->current.kind == EXPR_TOKEN_NUMBER || parser->current.kind == EXPR_TOKEN_CHAR) {
        *value_out = parser->current.number_value;
        expr_next(parser);
        return 0;
    }
    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(")) {
        expr_next(parser);
        if (expr_parse_constant_additive(parser, value_out) != 0) {
            return -1;
        }
        if (parser->current.kind != EXPR_TOKEN_PUNCT || !names_equal(parser->current.text, ")")) {
            return -1;
        }
        expr_next(parser);
        return 0;
    }
    return -1;
}

static int expr_parse_constant_unary(ExprParser *parser, long long *value_out) {
    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "+")) {
        expr_next(parser);
        return expr_parse_constant_unary(parser, value_out);
    }
    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "-")) {
        expr_next(parser);
        if (expr_parse_constant_unary(parser, value_out) != 0) {
            return -1;
        }
        *value_out = -*value_out;
        return 0;
    }
    return expr_parse_constant_primary(parser, value_out);
}

static int expr_parse_constant_multiplicative(ExprParser *parser, long long *value_out) {
    if (expr_parse_constant_unary(parser, value_out) != 0) {
        return -1;
    }
    while (parser->current.kind == EXPR_TOKEN_PUNCT &&
           (names_equal(parser->current.text, "*") || names_equal(parser->current.text, "/") ||
            names_equal(parser->current.text, "%"))) {
        char op[4];
        long long rhs;

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        if (expr_parse_constant_unary(parser, &rhs) != 0) {
            return -1;
        }
        if (names_equal(op, "*")) {
            *value_out *= rhs;
        } else {
            if (rhs == 0) {
                return -1;
            }
            if (names_equal(op, "/")) {
                *value_out /= rhs;
            } else {
                *value_out %= rhs;
            }
        }
    }
    return 0;
}

static int expr_parse_constant_additive(ExprParser *parser, long long *value_out) {
    if (expr_parse_constant_multiplicative(parser, value_out) != 0) {
        return -1;
    }
    while (parser->current.kind == EXPR_TOKEN_PUNCT &&
           (names_equal(parser->current.text, "+") || names_equal(parser->current.text, "-"))) {
        char op[4];
        long long rhs;

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        if (expr_parse_constant_multiplicative(parser, &rhs) != 0) {
            return -1;
        }
        if (names_equal(op, "+")) {
            *value_out += rhs;
        } else {
            *value_out -= rhs;
        }
    }
    return 0;
}

static int expr_parse_chain(ExprParser *parser, int level, const char ops[][4], size_t op_count) {
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
        ExprParser const_parser;
        int use_unsigned;
        int unsigned_32bit_result;
        long long immediate;

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        if (names_equal(op, "*")) {
            const_parser = *parser;
            if (expr_parse_constant_unary(&const_parser, &immediate) == 0) {
                rhs_snapshot = *parser;
                expr_infer_result_type(&rhs_snapshot, rhs_type, sizeof(rhs_type));
                use_unsigned = lhs_unsigned ||
                               expr_snapshot_looks_unsigned(&rhs_snapshot) ||
                               type_is_unsigned_like(lhs_type) ||
                               type_is_unsigned_like(rhs_type);
                unsigned_32bit_result = use_unsigned &&
                                        lhs_type[0] != '\0' &&
                                        rhs_type[0] != '\0' &&
                                        !text_contains(lhs_type, "long") &&
                                        !text_contains(rhs_type, "long") &&
                                        !text_contains(lhs_type, "64") &&
                                        !text_contains(rhs_type, "64") &&
                                        !text_contains(lhs_type, "*") &&
                                        !text_contains(rhs_type, "*") &&
                                        !text_contains(lhs_type, "[") &&
                                        !text_contains(rhs_type, "[");
                *parser = const_parser;
                if (emit_multiply_immediate(parser->state, immediate, unsigned_32bit_result) != 0) {
                    return -1;
                }
                rt_copy_string(lhs_type, sizeof(lhs_type), unsigned_32bit_result ? "unsigned int" : (use_unsigned ? "unsigned long" : "int"));
                lhs_unsigned = use_unsigned;
                continue;
            }
        }
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
        unsigned_32bit_result = use_unsigned &&
                                lhs_type[0] != '\0' &&
                                rhs_type[0] != '\0' &&
                                !text_contains(lhs_type, "long") &&
                                !text_contains(rhs_type, "long") &&
                                !text_contains(lhs_type, "64") &&
                                !text_contains(rhs_type, "64") &&
                                !text_contains(lhs_type, "*") &&
                                !text_contains(rhs_type, "*") &&
                                !text_contains(lhs_type, "[") &&
                                !text_contains(rhs_type, "[");
        if (emit_binary_op_mode(parser->state, op, use_unsigned, unsigned_32bit_result) != 0) {
            return -1;
        }
        rt_copy_string(lhs_type, sizeof(lhs_type), unsigned_32bit_result ? "unsigned int" : (use_unsigned ? "unsigned long" : "int"));
        lhs_unsigned = use_unsigned;
    }
    return 0;
}

static int expr_parse_additive(ExprParser *parser) {
    char lhs_type[128];
    ExprParser lhs_snapshot = *parser;
    int lhs_unsigned = expr_snapshot_looks_unsigned(&lhs_snapshot);

    expr_infer_result_type(&lhs_snapshot, lhs_type, sizeof(lhs_type));
    if (expr_parse_multiplicative(parser) != 0) {
        return -1;
    }

    while (parser->current.kind == EXPR_TOKEN_PUNCT &&
           (names_equal(parser->current.text, "+") || names_equal(parser->current.text, "-"))) {
        char op[4];
        char rhs_type[128];
        ExprParser rhs_snapshot;
        ExprParser const_parser;
        int lhs_pointer_like;
        int rhs_pointer_like;
        int use_unsigned;
        int unsigned_32bit_result;
        long long immediate;

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        lhs_pointer_like = lhs_type[0] != '\0' && (text_contains(lhs_type, "*") || text_contains(lhs_type, "["));
        const_parser = *parser;
        if (!lhs_pointer_like && expr_parse_constant_multiplicative(&const_parser, &immediate) == 0) {
            rhs_snapshot = *parser;
            expr_infer_result_type(&rhs_snapshot, rhs_type, sizeof(rhs_type));
            use_unsigned = lhs_unsigned ||
                           expr_snapshot_looks_unsigned(&rhs_snapshot) ||
                           type_is_unsigned_like(lhs_type) ||
                           type_is_unsigned_like(rhs_type);
            unsigned_32bit_result = use_unsigned &&
                                    lhs_type[0] != '\0' &&
                                    rhs_type[0] != '\0' &&
                                    !text_contains(lhs_type, "long") &&
                                    !text_contains(rhs_type, "long") &&
                                    !text_contains(lhs_type, "64") &&
                                    !text_contains(rhs_type, "64") &&
                                    !text_contains(lhs_type, "*") &&
                                    !text_contains(rhs_type, "*") &&
                                    !text_contains(lhs_type, "[") &&
                                    !text_contains(rhs_type, "[");
            *parser = const_parser;
            if (names_equal(op, "-")) {
                immediate = -immediate;
            }
            if (emit_add_immediate(parser->state, immediate, unsigned_32bit_result) != 0) {
                return -1;
            }
            rt_copy_string(lhs_type, sizeof(lhs_type), unsigned_32bit_result ? "unsigned int" : (use_unsigned ? "unsigned long" : "int"));
            lhs_unsigned = use_unsigned;
            continue;
        }
        if (emit_push_value(parser->state) != 0) {
            return -1;
        }

        rhs_snapshot = *parser;
        use_unsigned = lhs_unsigned || expr_snapshot_looks_unsigned(&rhs_snapshot);
        expr_infer_result_type(&rhs_snapshot, rhs_type, sizeof(rhs_type));
        if (expr_parse_multiplicative(parser) != 0) {
            return -1;
        }
        use_unsigned = use_unsigned || type_is_unsigned_like(lhs_type) || type_is_unsigned_like(rhs_type);
        unsigned_32bit_result = use_unsigned &&
                                lhs_type[0] != '\0' &&
                                rhs_type[0] != '\0' &&
                                !text_contains(lhs_type, "long") &&
                                !text_contains(rhs_type, "long") &&
                                !text_contains(lhs_type, "64") &&
                                !text_contains(rhs_type, "64") &&
                                !text_contains(lhs_type, "*") &&
                                !text_contains(rhs_type, "*") &&
                                !text_contains(lhs_type, "[") &&
                                !text_contains(rhs_type, "[");

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
            rt_copy_string(lhs_type, sizeof(lhs_type), unsigned_32bit_result ? "unsigned int" : (use_unsigned ? "unsigned long" : "int"));
        }
        lhs_unsigned = use_unsigned;

        if (emit_binary_op_mode(parser->state, op, use_unsigned, unsigned_32bit_result && !lhs_pointer_like && !rhs_pointer_like) != 0) {
            return -1;
        }
    }
    return 0;
}

static int expr_parse_shift(ExprParser *parser) {
    char lhs_type[128];
    ExprParser lhs_snapshot = *parser;
    int lhs_unsigned = expr_snapshot_looks_unsigned(&lhs_snapshot);

    expr_infer_result_type(&lhs_snapshot, lhs_type, sizeof(lhs_type));
    if (expr_parse_additive(parser) != 0) {
        return -1;
    }

    while (parser->current.kind == EXPR_TOKEN_PUNCT &&
           (names_equal(parser->current.text, "<<") || names_equal(parser->current.text, ">>"))) {
        char op[4];
        ExprParser const_parser;
        long long shift_amount = 0;
        int unsigned_32bit_result;

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        lhs_unsigned = lhs_unsigned || type_is_unsigned_like(lhs_type);
        unsigned_32bit_result = lhs_unsigned &&
                                lhs_type[0] != '\0' &&
                                !text_contains(lhs_type, "long") &&
                                !text_contains(lhs_type, "64") &&
                                !text_contains(lhs_type, "*") &&
                                !text_contains(lhs_type, "[");
        const_parser = *parser;
        if (expr_parse_constant_additive(&const_parser, &shift_amount) == 0 &&
            shift_amount >= 0 && shift_amount < 64) {
            *parser = const_parser;
            if (emit_shift_immediate(parser->state, op, shift_amount) != 0) {
                return -1;
            }
            if (unsigned_32bit_result &&
                emit_instruction(parser->state, backend_is_aarch64(parser->state) ? "uxtw x0, w0" : "movl %eax, %eax") != 0) {
                return -1;
            }
            if (unsigned_32bit_result) {
                rt_copy_string(lhs_type, sizeof(lhs_type), "unsigned int");
            }
            continue;
        }
        if (emit_push_value(parser->state) != 0 ||
            expr_parse_additive(parser) != 0 ||
            emit_binary_op_mode(parser->state, op, lhs_unsigned, unsigned_32bit_result) != 0) {
            return -1;
        }
        if (unsigned_32bit_result) {
            rt_copy_string(lhs_type, sizeof(lhs_type), "unsigned int");
        }
    }
    return 0;
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
        ExprParser const_parser;
        long long immediate;
        int use_unsigned;

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        const_parser = *parser;
        use_unsigned = lhs_unsigned || type_is_unsigned_like(lhs_type);
        if (expr_parse_constant_additive(&const_parser, &immediate) == 0) {
            int compared;
            compared = emit_compare_immediate(parser->state, op, immediate, use_unsigned);
            if (compared < 0) {
                return -1;
            }
            if (compared > 0) {
                *parser = const_parser;
                rt_copy_string(lhs_type, sizeof(lhs_type), "int");
                lhs_unsigned = 0;
                continue;
            }
        }
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
        if (emit_binary_op_mode(parser->state, op, use_unsigned, 0) != 0) {
            return -1;
        }
        rt_copy_string(lhs_type, sizeof(lhs_type), "int");
        lhs_unsigned = 0;
    }
    return 0;
}

static int expr_parse_equality(ExprParser *parser) {
    const char ops[][4] = {"==", "!="};
    return expr_parse_chain(parser, 4, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_bitand(ExprParser *parser) {
    if (expr_parse_equality(parser) != 0) {
        return -1;
    }
    while (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "&")) {
        ExprParser const_parser;
        long long immediate;

        expr_next(parser);
        const_parser = *parser;
        if (expr_parse_constant_unary(&const_parser, &immediate) == 0) {
            *parser = const_parser;
            if (emit_bitand_immediate(parser->state, immediate) != 0) {
                return -1;
            }
            continue;
        }
        if (emit_push_value(parser->state) != 0 ||
            expr_parse_equality(parser) != 0 ||
            emit_binary_op(parser->state, "&") != 0) {
            return -1;
        }
    }
    return 0;
}

static int expr_parse_bitxor(ExprParser *parser) {
    const char ops[][4] = {"^"};
    return expr_parse_chain(parser, 6, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_bitor(ExprParser *parser) {
    const char ops[][4] = {"|"};
    int rotated = expr_try_parse_rotr32_identifier(parser);
    if (rotated != 0) {
        return rotated < 0 ? -1 : 0;
    }
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
        char asm_label[128];

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
        char asm_label[128];

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
        char asm_label[128];
        ExprParser branch_type_parser = *parser;
        char branch_type[128];
        const char *branch_base;
        int object_conditional;

        branch_type[0] = '\0';
        expr_infer_result_type(&branch_type_parser, branch_type, sizeof(branch_type));
        branch_base = skip_spaces(branch_type);
        object_conditional = (starts_with(branch_base, "struct:") || starts_with(branch_base, "union:")) &&
                             !text_contains(branch_base, "*") && !text_contains(branch_base, "[");
        expr_make_logic_label(parser->state, "cond_false", false_label, sizeof(false_label));
        expr_make_logic_label(parser->state, "cond_end", end_label, sizeof(end_label));
        if (emit_cmp_zero(parser->state) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b.eq" : "je");
        if (emit_jump_to_label(parser->state, asm_label, false_label) != 0 ||
            (object_conditional ? expr_parse_object_value_address(parser) : expr_parse_assignment(parser)) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b" : "jmp");
        if (emit_jump_to_label(parser->state, asm_label, end_label) != 0 ||
            expr_expect_punct(parser, ":") != 0) {
            return -1;
        }
        write_label_name(parser->state, asm_label, sizeof(asm_label), false_label);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
        if (emit_line(parser->state, asm_label) != 0 ||
            (object_conditional ? expr_parse_object_value_address(parser) : expr_parse_assignment(parser)) != 0) {
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
            } else if (depth == 0 && expr_is_assignment_operator_text(snapshot.current.text)) {
                rt_copy_string(op, op_size, snapshot.current.text);
                return 1;
            } else if (depth == 0 && expr_is_assignment_stop_text(snapshot.current.text)) {
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

    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(")) {
        ExprParser grouped_type_parser = *parser;
        char grouped_type[128];
        char grouped_base_type[128];

        grouped_type[0] = '\0';
        grouped_base_type[0] = '\0';
        expr_infer_result_type(&grouped_type_parser, grouped_type, sizeof(grouped_type));
        expr_next(parser);
        if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "&")) {
            const char *inner_type = 0;

            expr_next(parser);
            if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
                inner_type = lookup_name_type_text(parser->state, parser->current.text);
                if (inner_type != 0 && inner_type[0] != '\0') {
                    rt_copy_string(grouped_base_type, sizeof(grouped_base_type), inner_type);
                }
            }
            if (expr_parse_lvalue_address(parser, byte_sized) != 0 || expr_expect_punct(parser, ")") != 0) {
                return -1;
            }
            if (grouped_base_type[0] == '\0' && grouped_type[0] != '\0') {
                copy_dereferenced_type(grouped_type, grouped_base_type, sizeof(grouped_base_type));
            }
            return expr_parse_lvalue_suffixes(parser,
                                              byte_sized,
                                              0,
                                              0,
                                              grouped_base_type[0] != '\0' ? grouped_base_type : 0);
        }
        if (expr_parse_lvalue_address(parser, byte_sized) != 0 || expr_expect_punct(parser, ")") != 0) {
            return -1;
        }
        return expr_parse_lvalue_suffixes(parser,
                                          byte_sized,
                                          0,
                                          1,
                                          grouped_type[0] != '\0' ? grouped_type : 0);
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
        *byte_sized = type_access_size(lookup_name_type_text(parser->state, name), word_index);

        if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "[")) {
            char element_type[128];
            int handled = expr_try_cached_identifier_index(parser,
                                                           name,
                                                           lookup_name_type_text(parser->state, name),
                                                           word_index,
                                                           element_type,
                                                           sizeof(element_type));
            if (handled < 0) {
                return -1;
            }
            if (handled > 0) {
                *byte_sized = type_access_size(element_type, 0);
                return expr_parse_lvalue_suffixes(parser,
                                                  byte_sized,
                                                  0,
                                                  1,
                                                  element_type);
            }
        }

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

    {
        char name[COMPILER_IR_NAME_CAPACITY];
        ExprParser direct_snapshot = snapshot;
        int direct_assignment = 0;

        name[0] = '\0';
        if (direct_snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
            rt_copy_string(name, sizeof(name), direct_snapshot.current.text);
            expr_next(&direct_snapshot);
        } else if (direct_snapshot.current.kind == EXPR_TOKEN_PUNCT &&
                   names_equal(direct_snapshot.current.text, "(")) {
            expr_next(&direct_snapshot);
            if (direct_snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
                rt_copy_string(name, sizeof(name), direct_snapshot.current.text);
                expr_next(&direct_snapshot);
                if (direct_snapshot.current.kind == EXPR_TOKEN_PUNCT &&
                    names_equal(direct_snapshot.current.text, ")")) {
                    expr_next(&direct_snapshot);
                } else {
                    name[0] = '\0';
                }
            }
        }
        if (name[0] != '\0' &&
            direct_snapshot.current.kind == EXPR_TOKEN_PUNCT &&
            expr_is_assignment_operator_text(direct_snapshot.current.text)) {
            direct_assignment = 1;
            rt_copy_string(op, sizeof(op), direct_snapshot.current.text);
        }

        if (direct_assignment) {
            *parser = direct_snapshot;
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
                int local_index = find_local(parser->state, name);
                int global_index = find_global(parser->state, name);
                int target_pointer_depth = 0;
                const char *target_type = lookup_name_type_text(parser->state, name);
                const char *target_base = skip_spaces(target_type);
                int target_is_object;

                if (local_index >= 0) {
                    target_pointer_depth = parser->state->locals[local_index].pointer_depth;
                } else if (global_index >= 0) {
                    target_pointer_depth = parser->state->globals[global_index].pointer_depth;
                }
                target_is_object = target_pointer_depth == 0 &&
                                       target_base[0] != '\0' &&
                                       !text_contains(target_base, "*") &&
                                       (text_contains(target_base, "[") ||
                                        starts_with(target_base, "struct:") ||
                                        starts_with(target_base, "union:"));
                if (lookup_array_storage(parser->state, name, &word_index) || target_is_object) {
                    int rhs_byte_sized = 0;
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
                                    expr_binary_op_for_assignment(op),
                                    type_is_unsigned_like(lookup_name_type_text(parser->state, name)),
                                    0) != 0) {
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
            ExprParser object_rhs_snapshot = *parser;

            if (object_rhs_snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
                char function_name[COMPILER_IR_NAME_CAPACITY];
                rt_copy_string(function_name, sizeof(function_name), object_rhs_snapshot.current.text);
                expr_next(&object_rhs_snapshot);
                if (object_rhs_snapshot.current.kind == EXPR_TOKEN_PUNCT &&
                    names_equal(object_rhs_snapshot.current.text, "(") &&
                    function_returns_object(parser->state, function_name)) {
                    expr_next(parser);
                    return emit_named_object_call_to_pushed_address(parser, function_name);
                }
            }

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
                                expr_binary_op_for_assignment(op),
                                type_is_unsigned_like(lhs_type),
                                0) != 0) {
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

static int u128_name_is_scalar(BackendState *state, const char *name) {
    const char *type = skip_spaces(lookup_name_type_text(state, name));

    return type[0] != '\0' && text_contains(type, "__int128") && !text_contains(type, "*") &&
           !text_contains(type, "[");
}

static int u128_parser_mentions_value(ExprParser *parser) {
    ExprParser snapshot = *parser;

    while (snapshot.current.kind != EXPR_TOKEN_EOF) {
        if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
            if (names_equal(snapshot.current.text, "__int128") ||
                u128_name_is_scalar(snapshot.state, snapshot.current.text)) {
                return 1;
            }
        }
        expr_next(&snapshot);
    }
    return 0;
}

static int u128_expr_mentions_value(BackendState *state, const char *expr) {
    ExprParser parser;

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);
    return u128_parser_mentions_value(&parser);
}

static int emit_u128_load_name(BackendState *state, const char *name) {
    if (!u128_name_is_scalar(state, name) || backend_is_aarch64(state)) {
        return -1;
    }
    backend_invalidate_block_cache(state);
    return emit_address_of_name(state, name) == 0 &&
           emit_instruction(state, "movq %rax, %rcx") == 0 &&
           emit_instruction(state, "movq (%rcx), %rax") == 0 &&
           emit_instruction(state, "movq 8(%rcx), %rdx") == 0 ? 0 : -1;
}

static int emit_u128_store_name(BackendState *state, const char *name) {
    if (!u128_name_is_scalar(state, name) || backend_is_aarch64(state)) {
        return -1;
    }
    backend_invalidate_block_cache_name(state, name);
    return emit_instruction(state, "pushq %rdx") == 0 &&
           emit_instruction(state, "pushq %rax") == 0 &&
           emit_address_of_name(state, name) == 0 &&
           emit_instruction(state, "movq %rax, %rcx") == 0 &&
           emit_instruction(state, "popq %rax") == 0 &&
           emit_instruction(state, "popq %rdx") == 0 &&
           emit_instruction(state, "movq %rax, (%rcx)") == 0 &&
           emit_instruction(state, "movq %rdx, 8(%rcx)") == 0 ? 0 : -1;
}

static int emit_u128_zero_extend_rax(BackendState *state) {
    return emit_instruction(state, "xor %edx, %edx");
}

static int emit_u128_push(BackendState *state) {
    return emit_instruction(state, "pushq %rdx") == 0 &&
           emit_instruction(state, "pushq %rax") == 0 ? 0 : -1;
}

static int emit_u128_pop_lhs(BackendState *state) {
    return emit_instruction(state, "popq %r10") == 0 &&
           emit_instruction(state, "popq %r11") == 0 ? 0 : -1;
}

static int emit_u128_add_lhs(BackendState *state) {
    return emit_u128_pop_lhs(state) == 0 &&
           emit_instruction(state, "addq %r10, %rax") == 0 &&
           emit_instruction(state, "adcq %r11, %rdx") == 0 ? 0 : -1;
}

static int emit_u128_sub_from_lhs(BackendState *state) {
    return emit_instruction(state, "movq %rax, %r8") == 0 &&
           emit_instruction(state, "movq %rdx, %r9") == 0 &&
           emit_u128_pop_lhs(state) == 0 &&
           emit_instruction(state, "movq %r10, %rax") == 0 &&
           emit_instruction(state, "movq %r11, %rdx") == 0 &&
           emit_instruction(state, "subq %r8, %rax") == 0 &&
           emit_instruction(state, "sbbq %r9, %rdx") == 0 ? 0 : -1;
}

static int emit_u128_and_lhs(BackendState *state) {
    return emit_u128_pop_lhs(state) == 0 &&
           emit_instruction(state, "andq %r10, %rax") == 0 &&
           emit_instruction(state, "andq %r11, %rdx") == 0 ? 0 : -1;
}

static int emit_u128_mul_lhs(BackendState *state) {
    return emit_instruction(state, "movq %rax, %r8") == 0 &&
           emit_instruction(state, "movq %rdx, %r9") == 0 &&
           emit_u128_pop_lhs(state) == 0 &&
           emit_instruction(state, "movq %r10, %rax") == 0 &&
           emit_instruction(state, "mulq %r8") == 0 &&
           emit_instruction(state, "movq %rdx, %rcx") == 0 &&
           emit_instruction(state, "movq %r10, %rdx") == 0 &&
           emit_instruction(state, "imulq %r9, %rdx") == 0 &&
           emit_instruction(state, "addq %rdx, %rcx") == 0 &&
           emit_instruction(state, "movq %r11, %rdx") == 0 &&
           emit_instruction(state, "imulq %r8, %rdx") == 0 &&
           emit_instruction(state, "addq %rdx, %rcx") == 0 &&
           emit_instruction(state, "movq %rcx, %rdx") == 0 ? 0 : -1;
}

static int emit_u128_shift_right_immediate(BackendState *state, unsigned int amount) {
    char line[64];
    char digits[32];

    if (amount == 0U) {
        return 0;
    }
    if (amount >= 128U) {
        return emit_instruction(state, "xor %eax, %eax") == 0 &&
               emit_instruction(state, "xor %edx, %edx") == 0 ? 0 : -1;
    }
    if (amount >= 64U) {
        rt_unsigned_to_string((unsigned long long)(amount - 64U), digits, sizeof(digits));
        rt_copy_string(line, sizeof(line), "shrq $");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rdx");
        return emit_instruction(state, line) == 0 &&
               emit_instruction(state, "movq %rdx, %rax") == 0 &&
               emit_instruction(state, "xor %edx, %edx") == 0 ? 0 : -1;
    }

    rt_unsigned_to_string((unsigned long long)amount, digits, sizeof(digits));
    rt_copy_string(line, sizeof(line), "shrdq $");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rdx, %rax");
    if (emit_instruction(state, line) != 0) {
        return -1;
    }
    rt_copy_string(line, sizeof(line), "shrq $");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rdx");
    return emit_instruction(state, line);
}

static int emit_u128_shift_left_immediate(BackendState *state, unsigned int amount) {
    char line[64];
    char digits[32];

    if (amount == 0U) {
        return 0;
    }
    if (amount >= 128U) {
        return emit_instruction(state, "xor %eax, %eax") == 0 &&
               emit_instruction(state, "xor %edx, %edx") == 0 ? 0 : -1;
    }
    if (amount >= 64U) {
        rt_unsigned_to_string((unsigned long long)(amount - 64U), digits, sizeof(digits));
        rt_copy_string(line, sizeof(line), "shlq $");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rax");
        return emit_instruction(state, line) == 0 &&
               emit_instruction(state, "movq %rax, %rdx") == 0 &&
               emit_instruction(state, "xor %eax, %eax") == 0 ? 0 : -1;
    }

    rt_unsigned_to_string((unsigned long long)amount, digits, sizeof(digits));
    rt_copy_string(line, sizeof(line), "shldq $");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rax, %rdx");
    if (emit_instruction(state, line) != 0) {
        return -1;
    }
    rt_copy_string(line, sizeof(line), "shlq $");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rax");
    return emit_instruction(state, line);
}

static int u128_parse_assignment(ExprParser *parser);

static int u128_parse_shift_amount(ExprParser *parser, unsigned int *amount_out) {
    if (parser->current.kind != EXPR_TOKEN_NUMBER && parser->current.kind != EXPR_TOKEN_CHAR) {
        backend_set_error(parser->state->backend, "unsupported __int128 shift count in backend");
        return -1;
    }
    if (parser->current.number_value < 0) {
        return -1;
    }
    *amount_out = (unsigned int)parser->current.number_value;
    expr_next(parser);
    return 0;
}

static int u128_parse_primary(ExprParser *parser) {
    if (parser->current.kind == EXPR_TOKEN_NUMBER || parser->current.kind == EXPR_TOKEN_CHAR) {
        if (emit_load_immediate(parser->state, parser->current.number_value) != 0 ||
            emit_u128_zero_extend_rax(parser->state) != 0) {
            return -1;
        }
        expr_next(parser);
        return 0;
    }

    if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
        ExprParser snapshot = *parser;
        char name[COMPILER_IR_NAME_CAPACITY];

        rt_copy_string(name, sizeof(name), parser->current.text);
        expr_next(&snapshot);
        if (u128_name_is_scalar(parser->state, name) &&
            !(snapshot.current.kind == EXPR_TOKEN_PUNCT &&
              (names_equal(snapshot.current.text, "[") ||
               names_equal(snapshot.current.text, ".") ||
               names_equal(snapshot.current.text, "->") ||
               names_equal(snapshot.current.text, "(")))) {
            *parser = snapshot;
            return emit_u128_load_name(parser->state, name);
        }

        if (expr_parse_unary(parser) != 0) {
            return -1;
        }
        return emit_u128_zero_extend_rax(parser->state);
    }

    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "(")) {
        ExprParser cast_snapshot = *parser;
        int is_cast = expr_looks_like_cast(&cast_snapshot);
        int saw_int128 = 0;

        if (is_cast) {
            int depth = 0;
            expr_next(parser);
            depth = 1;
            while (parser->current.kind != EXPR_TOKEN_EOF && depth > 0) {
                if (parser->current.kind == EXPR_TOKEN_IDENTIFIER &&
                    names_equal(parser->current.text, "__int128")) {
                    saw_int128 = 1;
                } else if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                           names_equal(parser->current.text, "(")) {
                    depth += 1;
                } else if (parser->current.kind == EXPR_TOKEN_PUNCT &&
                           names_equal(parser->current.text, ")")) {
                    depth -= 1;
                    if (depth == 0) {
                        expr_next(parser);
                        break;
                    }
                }
                if (depth > 0) {
                    expr_next(parser);
                }
            }
            if (depth == 0 && saw_int128 && !u128_parser_mentions_value(parser)) {
                int operand_unsigned = expr_snapshot_looks_unsigned(parser);

                if (expr_parse_unary(parser) != 0) {
                    return -1;
                }
                return operand_unsigned ? emit_u128_zero_extend_rax(parser->state) :
                                          emit_instruction(parser->state, "cqto");
            }
            if (depth != 0 || u128_parse_primary(parser) != 0) {
                return -1;
            }
            return saw_int128 ? 0 : emit_u128_zero_extend_rax(parser->state);
        }

        if (!u128_parser_mentions_value(parser)) {
            int operand_unsigned = expr_snapshot_looks_unsigned(parser);

            if (expr_parse_unary(parser) != 0) {
                return -1;
            }
            return operand_unsigned ? emit_u128_zero_extend_rax(parser->state) :
                                      emit_instruction(parser->state, "cqto");
        }

        expr_next(parser);
        if (u128_parse_assignment(parser) != 0 || expr_expect_punct(parser, ")") != 0) {
            return -1;
        }
        return 0;
    }

    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "-")) {
        expr_next(parser);
        if (u128_parse_primary(parser) != 0) {
            return -1;
        }
        return emit_instruction(parser->state, "negq %rax") == 0 &&
               emit_instruction(parser->state, "adcq $0, %rdx") == 0 &&
               emit_instruction(parser->state, "negq %rdx") == 0 ? 0 : -1;
    }

    return expr_parse_unary(parser) == 0 && emit_u128_zero_extend_rax(parser->state) == 0 ? 0 : -1;
}

static int u128_parse_multiplicative(ExprParser *parser) {
    if (u128_parse_primary(parser) != 0) {
        return -1;
    }
    while (parser->current.kind == EXPR_TOKEN_PUNCT &&
           (names_equal(parser->current.text, "*") ||
            names_equal(parser->current.text, "/") ||
            names_equal(parser->current.text, "%"))) {
        char op[4];

        rt_copy_string(op, sizeof(op), parser->current.text);
        if (!names_equal(op, "*")) {
            backend_set_error(parser->state->backend, "unsupported __int128 binary operation in backend");
            return -1;
        }
        expr_next(parser);
        if (emit_u128_push(parser->state) != 0 ||
            u128_parse_primary(parser) != 0 ||
            emit_u128_mul_lhs(parser->state) != 0) {
            return -1;
        }
    }
    return 0;
}

static int u128_parse_additive(ExprParser *parser) {
    if (u128_parse_multiplicative(parser) != 0) {
        return -1;
    }
    while (parser->current.kind == EXPR_TOKEN_PUNCT &&
           (names_equal(parser->current.text, "+") || names_equal(parser->current.text, "-"))) {
        char op[4];

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        if (emit_u128_push(parser->state) != 0 ||
            u128_parse_multiplicative(parser) != 0) {
            return -1;
        }
        if (names_equal(op, "+")) {
            if (emit_u128_add_lhs(parser->state) != 0) {
                return -1;
            }
        } else if (emit_u128_sub_from_lhs(parser->state) != 0) {
            return -1;
        }
    }
    return 0;
}

static int u128_parse_shift(ExprParser *parser) {
    if (u128_parse_additive(parser) != 0) {
        return -1;
    }
    while (parser->current.kind == EXPR_TOKEN_PUNCT &&
           (names_equal(parser->current.text, "<<") || names_equal(parser->current.text, ">>"))) {
        char op[4];
        unsigned int amount = 0;

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        if (u128_parse_shift_amount(parser, &amount) != 0) {
            return -1;
        }
        if (names_equal(op, "<<")) {
            if (emit_u128_shift_left_immediate(parser->state, amount) != 0) {
                return -1;
            }
        } else if (emit_u128_shift_right_immediate(parser->state, amount) != 0) {
            return -1;
        }
    }
    return 0;
}

static int u128_parse_bitand(ExprParser *parser) {
    if (u128_parse_shift(parser) != 0) {
        return -1;
    }
    while (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "&")) {
        expr_next(parser);
        if (emit_u128_push(parser->state) != 0 ||
            u128_parse_shift(parser) != 0 ||
            emit_u128_and_lhs(parser->state) != 0) {
            return -1;
        }
    }
    return 0;
}

static int u128_parse_assignment(ExprParser *parser) {
    return u128_parse_bitand(parser);
}

static int emit_u128_parse_expression(BackendState *state, const char *expr) {
    ExprParser parser;

    backend_invalidate_block_cache(state);
    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);
    if (u128_parse_assignment(&parser) != 0) {
        return -1;
    }
    if (parser.current.kind != EXPR_TOKEN_EOF) {
        backend_set_error(state->backend, "unsupported __int128 expression syntax in backend");
        return -1;
    }
    return 0;
}

int emit_u128_store_expression(BackendState *state, const char *name, const char *expr) {
    int target_is_u128 = u128_name_is_scalar(state, name);

    if (!target_is_u128 && !u128_expr_mentions_value(state, expr)) {
        return 0;
    }
    if (backend_is_aarch64(state)) {
        backend_set_error(state->backend, "unsupported __int128 expression on target");
        return -1;
    }
    if (emit_u128_parse_expression(state, expr) != 0) {
        return -1;
    }
    if (target_is_u128) {
        if (emit_u128_store_name(state, name) != 0) {
            return -1;
        }
    } else if (emit_store_name(state, name) != 0) {
        return -1;
    }
    return 1;
}

static int parse_direct_assignment_prefix(ExprParser *parser, char *name, size_t name_size, char *op, size_t op_size) {
    name[0] = '\0';
    op[0] = '\0';
    if (parser->current.kind != EXPR_TOKEN_IDENTIFIER) {
        return 0;
    }
    rt_copy_string(name, name_size, parser->current.text);
    expr_next(parser);
    if (parser->current.kind != EXPR_TOKEN_PUNCT || !expr_is_assignment_operator_text(parser->current.text)) {
        return 0;
    }
    rt_copy_string(op, op_size, parser->current.text);
    expr_next(parser);
    return 1;
}

int emit_u128_eval_expression(BackendState *state, const char *expr) {
    ExprParser parser;
    char name[COMPILER_IR_NAME_CAPACITY];
    char op[4];

    if (backend_is_aarch64(state)) {
        return 0;
    }
    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);
    if (!parse_direct_assignment_prefix(&parser, name, sizeof(name), op, sizeof(op))) {
        return 0;
    }
    if (!u128_name_is_scalar(state, name)) {
        if (!names_equal(op, "=") || !u128_parser_mentions_value(&parser)) {
            return 0;
        }
        if (u128_parse_assignment(&parser) != 0) {
            return -1;
        }
        if (parser.current.kind != EXPR_TOKEN_EOF) {
            backend_set_error(state->backend, "unsupported __int128 assignment syntax in backend");
            return -1;
        }
        if (emit_store_name(state, name) != 0) {
            return -1;
        }
        return 1;
    }

    if (names_equal(op, "=")) {
        if (u128_parse_assignment(&parser) != 0) {
            return -1;
        }
    } else {
        if (emit_u128_load_name(state, name) != 0 ||
            emit_u128_push(state) != 0 ||
            u128_parse_assignment(&parser) != 0) {
            return -1;
        }
        if (names_equal(op, "+=")) {
            if (emit_u128_add_lhs(state) != 0) {
                return -1;
            }
        } else if (names_equal(op, "-=")) {
            if (emit_u128_sub_from_lhs(state) != 0) {
                return -1;
            }
        } else if (names_equal(op, "&=")) {
            if (emit_u128_and_lhs(state) != 0) {
                return -1;
            }
        } else if (names_equal(op, "*=")) {
            if (emit_u128_mul_lhs(state) != 0) {
                return -1;
            }
        } else {
            backend_set_error(state->backend, "unsupported __int128 assignment operation in backend");
            return -1;
        }
    }
    if (parser.current.kind != EXPR_TOKEN_EOF) {
        backend_set_error(state->backend, "unsupported __int128 assignment syntax in backend");
        return -1;
    }
    if (emit_u128_store_name(state, name) != 0) {
        return -1;
    }
    return 1;
}

int expr_text_looks_unsigned(BackendState *state, const char *expr) {
    ExprParser parser;
    char type_text[128];

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);
    if (expr_snapshot_looks_unsigned(&parser)) {
        return 1;
    }
    expr_infer_result_type(&parser, type_text, sizeof(type_text));
    return type_is_unsigned_like(type_text);
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
    const char *target_type = lookup_name_type_text(state, name);
    int storage_bytes = (int)type_storage_bytes_text(state, target_type);

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
    element_scale = array_index_scale(state, target_type, word_index);
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

    {
        char row_type[128];
        const char *row_base;
        unsigned long long row_width;

        copy_indexed_result_type(target_type, row_type, sizeof(row_type));
        row_base = skip_spaces(row_type);
        row_width = initializer_array_length_limit(row_base, 1);
        if (parser.current.kind == EXPR_TOKEN_PUNCT &&
            names_equal(parser.current.text, "{") &&
            starts_with(row_base, "char[") &&
            !text_contains(row_base, "*") &&
            row_width > 0ULL) {
            unsigned long long row = 0ULL;

            expr_next(&parser);
            while (parser.current.kind != EXPR_TOKEN_EOF &&
                   !(parser.current.kind == EXPR_TOKEN_PUNCT && names_equal(parser.current.text, "}"))) {
                if (parser.current.kind == EXPR_TOKEN_STRING) {
                    size_t text_index;
                    size_t length = rt_strlen(parser.current.text);
                    unsigned long long limit = row_width;

                    if (limit > (unsigned long long)length) {
                        limit = (unsigned long long)length;
                    }
                    if (row < slot_limit) {
                        for (text_index = 0; text_index < (size_t)limit; ++text_index) {
                            int offset = (int)(row * row_width + (unsigned long long)text_index);
                            if (emit_offset_address(state, name, offset) != 0 ||
                                emit_push_value(state) != 0 ||
                                emit_load_immediate(state, (long long)(unsigned char)parser.current.text[text_index]) != 0 ||
                                emit_pop_address_and_store(state, 1) != 0) {
                                return -1;
                            }
                        }
                    }
                    expr_next(&parser);
                    row += 1ULL;
                } else {
                    backend_set_error(state->backend, "unsupported primary expression in backend");
                    return -1;
                }
                if (!expr_match_punct(&parser, ",")) {
                    break;
                }
            }
            if (!expr_match_punct(&parser, "}")) {
                backend_set_error(state->backend, "unsupported primary expression in backend");
                return -1;
            }
            if (parser.current.kind != EXPR_TOKEN_EOF) {
                backend_set_error(state->backend, "unsupported primary expression in backend");
                return -1;
            }
            return 0;
        }
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

int emit_object_copy_to_pointer_name(BackendState *state, const char *dst_pointer_name, const char *expr, int bytes) {
    ExprParser parser;
    int byte_sized = 0;

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);

    if (parser.current.kind == EXPR_TOKEN_IDENTIFIER) {
        char function_name[COMPILER_IR_NAME_CAPACITY];
        ExprParser call_parser = parser;

        rt_copy_string(function_name, sizeof(function_name), parser.current.text);
        expr_next(&call_parser);
        if (call_parser.current.kind == EXPR_TOKEN_PUNCT &&
            names_equal(call_parser.current.text, "(") &&
            function_returns_object(state, function_name)) {
            if (emit_load_name(state, dst_pointer_name) != 0 ||
                emit_push_value(state) != 0 ||
                emit_named_object_call_to_pushed_address(&call_parser, function_name) != 0) {
                return -1;
            }
            return call_parser.current.kind == EXPR_TOKEN_EOF ? 0 : -1;
        }
    }

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);
    if (expr_may_be_object_lvalue_source(&parser)) {
        if (emit_load_name(state, dst_pointer_name) != 0 ||
            emit_push_value(state) != 0 ||
            expr_parse_lvalue_address(&parser, &byte_sized) != 0) {
            return -1;
        }
        return emit_copy_object_to_pushed_address(state, bytes);
    }

    return -1;
}
