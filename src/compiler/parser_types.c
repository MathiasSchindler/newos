/* Type, enum, and constant-expression parsing helpers. */

#include "parser_internal.h"

#include <limits.h>

/* Forward declarations for mutually-recursive static helpers. */
static int parse_constant_expression(CompilerParser *parser, long long *value_out);
static int parse_constant_unary(CompilerParser *parser, long long *value_out);
static int parse_constant_multiplicative(CompilerParser *parser, long long *value_out);
static int parse_constant_additive(CompilerParser *parser, long long *value_out);
static int parse_constant_shift(CompilerParser *parser, long long *value_out);
static int parse_constant_relational(CompilerParser *parser, long long *value_out);
static int parse_constant_equality(CompilerParser *parser, long long *value_out);
static int parse_constant_bitand(CompilerParser *parser, long long *value_out);
static int parse_constant_bitxor(CompilerParser *parser, long long *value_out);
static int parse_constant_bitor(CompilerParser *parser, long long *value_out);
static int parse_constant_logical_and(CompilerParser *parser, long long *value_out);
static int parse_constant_logical_or(CompilerParser *parser, long long *value_out);

static unsigned long long align_up(unsigned long long value, unsigned long long alignment) {
    if (alignment <= 1ULL) {
        return value;
    }
    if (value > ULLONG_MAX - (alignment - 1ULL)) {
        return ULLONG_MAX;
    }
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static int checked_size_product(const CompilerParser *parser,
                                unsigned long long lhs,
                                unsigned long long rhs,
                                unsigned long long *result_out) {
    if (result_out == 0) {
        return -1;
    }
    if (lhs == 0ULL || rhs == 0ULL) {
        *result_out = 0ULL;
        return 0;
    }
    if (lhs > ULLONG_MAX / rhs) {
        if (parser != 0) {
            set_error((CompilerParser *)parser, "array size exceeds compiler limits");
        }
        return -1;
    }
    *result_out = lhs * rhs;
    return 0;
}

static int find_aggregate_layout_by_name(const CompilerParser *parser, const char *name) {
    size_t i;

    if (name == 0 || name[0] == '\0') {
        return -1;
    }

    for (i = 0; i < parser->aggregate_layout_count; ++i) {
        if (rt_strcmp(parser->aggregate_layouts[i].name, name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int ensure_aggregate_layout(CompilerParser *parser, CompilerType *type, int is_union) {
    int index;

    if (type == 0) {
        return -1;
    }
    if (type->aggregate_layout_id > 0U) {
        return (int)type->aggregate_layout_id - 1;
    }

    index = find_aggregate_layout_by_name(parser, type->aggregate_name);
    if (index >= 0) {
        parser->aggregate_layouts[index].is_union = is_union ? 1 : 0;
        type->aggregate_layout_id = (unsigned short)(index + 1);
        return index;
    }

    if (parser->aggregate_layout_count >= COMPILER_MAX_AGGREGATE_LAYOUTS) {
        set_error(parser, "aggregate layout table exhausted");
        return -1;
    }

    index = (int)parser->aggregate_layout_count;
    rt_memset(&parser->aggregate_layouts[index], 0, sizeof(parser->aggregate_layouts[index]));
    rt_copy_string(parser->aggregate_layouts[index].name,
                   sizeof(parser->aggregate_layouts[index].name),
                   type->aggregate_name);
    parser->aggregate_layouts[index].base = type->base;
    parser->aggregate_layouts[index].is_union = is_union ? 1 : 0;
    parser->aggregate_layouts[index].align_bytes = 1U;
    parser->aggregate_layouts[index].field_start = parser->aggregate_field_count;
    parser->aggregate_layout_count += 1U;
    type->aggregate_layout_id = (unsigned short)(index + 1);
    return index;
}

static unsigned long long type_storage_bytes(const CompilerParser *parser, const CompilerType *type);

static unsigned long long type_alignment_bytes(const CompilerParser *parser, const CompilerType *type) {
    int layout_index;

    if (type == 0) {
        return 1U;
    }
    if (type->pointer_depth > 0 || type->is_function) {
        return 8U;
    }
    if (type->is_array) {
        CompilerType element_type = *type;
        if (type->array_stride > 0ULL) {
            element_type.is_array = 1;
            element_type.array_length = type->array_stride;
            element_type.array_stride = 0ULL;
        } else {
            element_type.is_array = 0;
            element_type.array_length = 0ULL;
            element_type.array_stride = 0ULL;
        }
        return type_alignment_bytes(parser, &element_type);
    }
    if (type->base == COMPILER_BASE_CHAR) {
        return 1U;
    }
    if (type->base == COMPILER_BASE_VOID) {
        return 1U;
    }
    if (type->base == COMPILER_BASE_STRUCT || type->base == COMPILER_BASE_UNION) {
        if (type->aggregate_layout_id > 0U) {
            layout_index = (int)type->aggregate_layout_id - 1;
            if (layout_index >= 0 && (size_t)layout_index < parser->aggregate_layout_count &&
                parser->aggregate_layouts[layout_index].align_bytes > 0ULL) {
                return parser->aggregate_layouts[layout_index].align_bytes;
            }
        }
        layout_index = find_aggregate_layout_by_name(parser, type->aggregate_name);
        if (layout_index >= 0 && parser->aggregate_layouts[layout_index].align_bytes > 0ULL) {
            return parser->aggregate_layouts[layout_index].align_bytes;
        }
        return 8U;
    }
    if (type->scalar_bytes >= 8U) {
        return 8U;
    }
    if (type->scalar_bytes == 2U) {
        return 2U;
    }
    if (type->scalar_bytes == 1U) {
        return 1U;
    }
    return 4U;
}

static unsigned long long type_storage_bytes(const CompilerParser *parser, const CompilerType *type) {
    int layout_index;
    unsigned long long element_size;
    unsigned long long length;

    if (type == 0) {
        return 0ULL;
    }
    if (type->is_array) {
        CompilerType element_type = *type;
        if (type->array_stride > 0ULL) {
            element_type.is_array = 1;
            element_type.array_length = type->array_stride;
            element_type.array_stride = 0ULL;
        } else {
            element_type.is_array = 0;
            element_type.array_length = 0ULL;
            element_type.array_stride = 0ULL;
        }
        element_size = type_storage_bytes(parser, &element_type);
        if (element_size == 0ULL) {
            element_size = 1ULL;
        }
        length = type->array_length > 0ULL ? type->array_length : 1ULL;
        if (checked_size_product(parser, element_size, length, &element_size) != 0) {
            return 0ULL;
        }
        return element_size;
    }
    if (type->pointer_depth > 0 || type->is_function) {
        return 8U;
    }
    if (type->base == COMPILER_BASE_CHAR) {
        return 1U;
    }
    if (type->base == COMPILER_BASE_VOID) {
        return 1U;
    }
    if (type->base == COMPILER_BASE_STRUCT || type->base == COMPILER_BASE_UNION) {
        if (type->aggregate_layout_id > 0U) {
            layout_index = (int)type->aggregate_layout_id - 1;
            if (layout_index >= 0 && (size_t)layout_index < parser->aggregate_layout_count &&
                parser->aggregate_layouts[layout_index].size_bytes > 0ULL) {
                return parser->aggregate_layouts[layout_index].size_bytes;
            }
        }
        layout_index = find_aggregate_layout_by_name(parser, type->aggregate_name);
        if (layout_index >= 0 && parser->aggregate_layouts[layout_index].size_bytes > 0ULL) {
            return parser->aggregate_layouts[layout_index].size_bytes;
        }
        return 16U;
    }
    if (type->scalar_bytes > 0U) {
        return (unsigned long long)type->scalar_bytes;
    }
    return 4U;
}

static void format_layout_type(const CompilerType *type, char *buffer, size_t buffer_size) {
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
    } else if (type->base == COMPILER_BASE_INT) {
        if (type->scalar_bytes >= 16U) {
            base = "__int128";
        } else if (type->scalar_bytes >= 8U) {
            base = "long";
        } else if (type->scalar_bytes == 2U) {
            base = "short";
        }
    }

    buffer[0] = '\0';
    if (type->is_unsigned) {
        rt_copy_string(buffer, buffer_size, "unsigned ");
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), base);
    } else {
        rt_copy_string(buffer, buffer_size, base);
    }

    if ((type->base == COMPILER_BASE_STRUCT || type->base == COMPILER_BASE_UNION || type->base == COMPILER_BASE_ENUM) &&
        type->aggregate_name[0] != '\0') {
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), ":");
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), type->aggregate_name);
    }

    for (i = 0; i < (size_t)type->pointer_depth; ++i) {
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "*");
    }

    if (type->is_array) {
        char digits[32];
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "[");
        if (type->array_length > 0ULL) {
            rt_unsigned_to_string(type->array_length, digits, sizeof(digits));
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
        }
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "]");
        if (type->array_stride > 0ULL) {
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "[");
            rt_unsigned_to_string(type->array_stride, digits, sizeof(digits));
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "]");
        }
    }
}

static int emit_pending_aggregate_layout(CompilerParser *parser, const CompilerType *type) {
    CompilerAggregateLayout *layout;
    int index;
    size_t i;
    char detail[COMPILER_IR_LINE_CAPACITY];
    char digits[32];

    if (type == 0 || !parser ||
        (type->base != COMPILER_BASE_STRUCT && type->base != COMPILER_BASE_UNION)) {
        return 0;
    }

    if (type->aggregate_layout_id > 0U) {
        index = (int)type->aggregate_layout_id - 1;
    } else {
        index = find_aggregate_layout_by_name(parser, type->aggregate_name);
    }
    if (index < 0 || (size_t)index >= parser->aggregate_layout_count) {
        return 0;
    }

    layout = &parser->aggregate_layouts[index];
    if (layout->name[0] == '\0' && type->aggregate_name[0] != '\0') {
        rt_copy_string(layout->name, sizeof(layout->name), type->aggregate_name);
    }
    if (layout->name[0] == '\0' || layout->emitted) {
        return 0;
    }

    rt_copy_string(detail, sizeof(detail), layout->is_union ? "union " : "struct ");
    rt_copy_string(detail + rt_strlen(detail), sizeof(detail) - rt_strlen(detail), layout->name);
    rt_copy_string(detail + rt_strlen(detail), sizeof(detail) - rt_strlen(detail), " ");
    rt_unsigned_to_string(layout->size_bytes, digits, sizeof(digits));
    rt_copy_string(detail + rt_strlen(detail), sizeof(detail) - rt_strlen(detail), digits);
    rt_copy_string(detail + rt_strlen(detail), sizeof(detail) - rt_strlen(detail), " ");
    rt_unsigned_to_string(layout->align_bytes, digits, sizeof(digits));
    rt_copy_string(detail + rt_strlen(detail), sizeof(detail) - rt_strlen(detail), digits);
    if (emit_ir_status(parser, compiler_ir_emit_note(&parser->ir, "aggregate", detail)) != 0) {
        return -1;
    }

    for (i = 0; i < parser->aggregate_field_count; ++i) {
        char type_text[128];
        if (parser->aggregate_fields[i].layout_id != (unsigned short)(index + 1)) {
            continue;
        }
        format_layout_type(&parser->aggregate_fields[i].type, type_text, sizeof(type_text));
        rt_copy_string(detail, sizeof(detail), layout->name);
        rt_copy_string(detail + rt_strlen(detail), sizeof(detail) - rt_strlen(detail), " ");
        rt_copy_string(detail + rt_strlen(detail), sizeof(detail) - rt_strlen(detail), parser->aggregate_fields[i].name);
        rt_copy_string(detail + rt_strlen(detail), sizeof(detail) - rt_strlen(detail), " ");
        rt_unsigned_to_string(parser->aggregate_fields[i].offset_bytes, digits, sizeof(digits));
        rt_copy_string(detail + rt_strlen(detail), sizeof(detail) - rt_strlen(detail), digits);
        rt_copy_string(detail + rt_strlen(detail), sizeof(detail) - rt_strlen(detail), " ");
        rt_copy_string(detail + rt_strlen(detail), sizeof(detail) - rt_strlen(detail), type_text);
        if (emit_ir_status(parser, compiler_ir_emit_note(&parser->ir, "member", detail)) != 0) {
            return -1;
        }
    }

    layout->emitted = 1;
    return 0;
}

int parser_emit_type_layout_notes(CompilerParser *parser, const CompilerType *type) {
    return emit_pending_aggregate_layout(parser, type);
}

static int parse_aggregate_definition(CompilerParser *parser, CompilerType *type_out, int is_union) {
    int layout_index;
    CompilerAggregateLayout *layout;

    layout_index = ensure_aggregate_layout(parser, type_out, is_union);
    if (layout_index < 0) {
        return -1;
    }

    layout = &parser->aggregate_layouts[layout_index];
    if (advance(parser) != 0) {
        return -1;
    }

    while (!current_is_punct(parser, "}")) {
        CompilerType field_base;
        int saw = 0;

        compiler_type_init(&field_base);
        saw = parse_declaration_specifiers(parser, 0, 0, 0, &field_base);
        if (saw <= 0) {
            set_error(parser, "expected aggregate member declaration");
            return -1;
        }

        if (current_is_punct(parser, ";")) {
            if (advance(parser) != 0) {
                return -1;
            }
            continue;
        }

        for (;;) {
            CompilerDeclarator declarator;
            CompilerType member_type = field_base;
            unsigned long long field_align;
            unsigned long long field_size;
            unsigned long long offset;

            if (parse_declarator(parser, &declarator, 0) != 0) {
                return -1;
            }

            if (parser_add_pointer_depth(parser, &member_type.pointer_depth, declarator.pointer_depth) != 0) {
                return -1;
            }
            member_type.is_function = declarator.is_function;
            member_type.is_array = declarator.is_array;
            member_type.array_length = declarator.array_length;
            member_type.array_stride = declarator.array_stride;

            field_align = type_alignment_bytes(parser, &member_type);
            field_size = type_storage_bytes(parser, &member_type);
            if (parser->error_message[0] != '\0') {
                return -1;
            }
            if (field_size == 0ULL) {
                field_size = 1ULL;
            }

            if (layout->is_union) {
                offset = 0ULL;
                if (field_size > layout->size_bytes) {
                    layout->size_bytes = field_size;
                }
            } else {
                offset = align_up(layout->size_bytes, field_align);
                if (offset == ULLONG_MAX || offset > ULLONG_MAX - field_size) {
                    set_error(parser, "aggregate layout exceeds compiler limits");
                    return -1;
                }
                layout->size_bytes = offset + field_size;
            }
            if (field_align > layout->align_bytes) {
                layout->align_bytes = field_align;
            }

            if (declarator.name[0] != '\0') {
                if (parser->aggregate_field_count >= COMPILER_MAX_AGGREGATE_FIELDS) {
                    set_error(parser, "aggregate member table exhausted");
                    return -1;
                }
                rt_copy_string(parser->aggregate_fields[parser->aggregate_field_count].aggregate_name,
                               sizeof(parser->aggregate_fields[parser->aggregate_field_count].aggregate_name),
                               layout->name);
                rt_copy_string(parser->aggregate_fields[parser->aggregate_field_count].name,
                               sizeof(parser->aggregate_fields[parser->aggregate_field_count].name),
                               declarator.name);
                parser->aggregate_fields[parser->aggregate_field_count].type = member_type;
                parser->aggregate_fields[parser->aggregate_field_count].layout_id = (unsigned short)(layout_index + 1);
                parser->aggregate_fields[parser->aggregate_field_count].offset_bytes = offset;
                parser->aggregate_fields[parser->aggregate_field_count].size_bytes = field_size;
                parser->aggregate_field_count += 1U;
                layout->field_count += 1U;
            }

            if (!current_is_punct(parser, ",")) {
                break;
            }
            if (advance(parser) != 0) {
                return -1;
            }
        }

        if (expect_punct(parser, ";") != 0) {
            return -1;
        }
    }

    layout->size_bytes = align_up(layout->size_bytes, layout->align_bytes);
    return expect_punct(parser, "}");
}

int parse_declaration_specifiers(CompilerParser *parser, int *is_typedef_out, int *is_extern_out, int *is_static_out, CompilerType *type_out) {
    int saw_any = 0;
    int saw_explicit_base = 0;

    if (is_typedef_out != 0) {
        *is_typedef_out = 0;
    }
    if (is_extern_out != 0) {
        *is_extern_out = 0;
    }
    if (is_static_out != 0) {
        *is_static_out = 0;
    }
    if (type_out != 0) {
        compiler_type_init(type_out);
    }

    while (token_starts_decl_specifier(parser)) {
        saw_any = 1;

        if (current_is_keyword(parser, "typedef") && is_typedef_out != 0) {
            *is_typedef_out = 1;
        } else if (current_is_keyword(parser, "extern") && is_extern_out != 0) {
            *is_extern_out = 1;
        } else if (current_is_keyword(parser, "static") && is_static_out != 0) {
            *is_static_out = 1;
        } else if (type_out != 0) {
            if (current_is_keyword(parser, "void")) {
                type_out->base = COMPILER_BASE_VOID;
                type_out->scalar_bytes = 0U;
                saw_explicit_base = 1;
            } else if (current_is_keyword(parser, "char")) {
                type_out->base = COMPILER_BASE_CHAR;
                type_out->scalar_bytes = 1U;
                saw_explicit_base = 1;
            } else if (current_is_int_family_keyword(parser) ||
                       (current_is_identifier(parser) && token_text_equals(&parser->current, "__int128"))) {
                type_out->base = COMPILER_BASE_INT;
                if (current_is_keyword(parser, "short")) {
                    type_out->scalar_bytes = 2U;
                } else if (current_is_keyword(parser, "long") ||
                           current_is_keyword(parser, "double") ||
                           (current_is_identifier(parser) && token_text_equals(&parser->current, "__int128"))) {
                    type_out->scalar_bytes = token_text_equals(&parser->current, "__int128") ? 16U : 8U;
                } else {
                    type_out->scalar_bytes = 4U;
                }
                saw_explicit_base = 1;
            } else if (current_is_keyword(parser, "unsigned")) {
                type_out->base = COMPILER_BASE_INT;
                type_out->is_unsigned = 1;
                if (type_out->scalar_bytes == 0U) {
                    type_out->scalar_bytes = 4U;
                }
                saw_explicit_base = 1;
            } else if (current_is_identifier(parser) && is_typedef_name(parser, &parser->current)) {
                char typedef_name[COMPILER_TYPEDEF_NAME_CAPACITY];
                CompilerType resolved_type;

                copy_token_text(&parser->current, typedef_name, sizeof(typedef_name));
                if (compiler_semantic_lookup_typedef(&parser->semantic, typedef_name, &resolved_type) == 0) {
                    *type_out = resolved_type;
                }
                saw_explicit_base = 1;
            }
        }

        if (current_is_aggregate_type_keyword(parser)) {
            int is_enum = current_is_keyword(parser, "enum");
            int is_union = current_is_keyword(parser, "union");

            if (type_out != 0) {
                if (current_is_keyword(parser, "struct")) {
                    type_out->base = COMPILER_BASE_STRUCT;
                } else if (current_is_keyword(parser, "union")) {
                    type_out->base = COMPILER_BASE_UNION;
                } else {
                    type_out->base = COMPILER_BASE_ENUM;
                }
                saw_explicit_base = 1;
            }

                if (is_enum) {
                    if (parse_enum_specifier(parser) != 0) {
                        return -1;
                    }
                } else {
                if (advance(parser) != 0) {
                    return -1;
                }

                if (current_is_identifier(parser)) {
                    if (type_out != 0) {
                        copy_token_text(&parser->current, type_out->aggregate_name, sizeof(type_out->aggregate_name));
                    }
                    if (advance(parser) != 0) {
                        return -1;
                    }
                }

                if (current_is_punct(parser, "{")) {
                    if (parse_aggregate_definition(parser, type_out, is_union) != 0) {
                        return -1;
                    }
                }
            }
            continue;
        }

        if (advance(parser) != 0) {
            return -1;
        }
    }

    if (type_out != 0 && !saw_explicit_base) {
        type_out->base = COMPILER_BASE_INT;
    }

    return saw_any;
}

static int identifier_looks_like_type_name(const CompilerToken *token) {
    size_t i;
    int has_lower = 0;

    if (token->length == 0 || token->start[0] < 'A' || token->start[0] > 'Z') {
        return 0;
    }

    for (i = 0; i < token->length; ++i) {
        if (token->start[i] >= 'a' && token->start[i] <= 'z') {
            has_lower = 1;
            break;
        }
    }

    return has_lower;
}

static int token_is_known_type_specifier(const CompilerParser *parser, const CompilerToken *token) {
    if (token->kind == COMPILER_TOKEN_KEYWORD &&
        (token_text_equals(token, "const") || token_text_equals(token, "volatile") ||
         token_text_equals(token, "restrict") || token_text_equals(token, "void") ||
         token_text_equals(token, "char") || token_text_equals(token, "short") ||
         token_text_equals(token, "int") || token_text_equals(token, "long") ||
         token_text_equals(token, "signed") || token_text_equals(token, "unsigned") ||
         token_text_equals(token, "float") || token_text_equals(token, "double") ||
         token_text_equals(token, "struct") || token_text_equals(token, "union") ||
         token_text_equals(token, "enum") || token_text_equals(token, "__int128"))) {
        return 1;
    }

    if (token->kind == COMPILER_TOKEN_IDENTIFIER) {
        size_t length = token->length;
        if (is_typedef_name(parser, token)) {
            return 1;
        }
        if (token_text_equals(token, "__int128") ||
            token_text_equals(token, "u8") || token_text_equals(token, "u16") ||
            token_text_equals(token, "u32") || token_text_equals(token, "u64") ||
            token_text_equals(token, "i8") || token_text_equals(token, "i16") ||
            token_text_equals(token, "i32") || token_text_equals(token, "i64") ||
            token_text_equals(token, "usize")) {
            return 1;
        }
        if (length > 2 && token->start[length - 2] == '_' && token->start[length - 1] == 't') {
            return 1;
        }
        if (identifier_looks_like_type_name(token)) {
            return 1;
        }
    }

    return 0;
}

int looks_like_type_name_after_lparen(const CompilerParser *parser) {
    CompilerToken next;

    if (!current_is_punct(parser, "(")) {
        return 0;
    }

    if (peek_token(parser, &next) != 0) {
        return 0;
    }

    return token_is_known_type_specifier(parser, &next);
}

int looks_like_compound_literal_after_lparen(const CompilerParser *parser) {
    CompilerLexer snapshot;
    CompilerToken token;
    int depth = 1;

    if (!current_is_punct(parser, "(") || peek_token(parser, &token) != 0) {
        return 0;
    }

    if (!token_is_known_type_specifier(parser, &token)) {
        return 0;
    }

    snapshot = parser->lexer;
    while (compiler_lexer_next(&snapshot, &token) == 0) {
        if (token.kind == COMPILER_TOKEN_PUNCTUATOR) {
            if (token_text_equals(&token, "(")) {
                depth += 1;
            } else if (token_text_equals(&token, ")")) {
                depth -= 1;
                if (depth == 0) {
                    break;
                }
            }
        }
    }

    if (depth != 0 || compiler_lexer_next(&snapshot, &token) != 0) {
        return 0;
    }

    return token.kind == COMPILER_TOKEN_PUNCTUATOR && token_text_equals(&token, "{");
}

int parse_type_name(CompilerParser *parser) {
    CompilerType type;
    int saw;
    CompilerDeclarator declarator;

    compiler_type_init(&type);
    saw = parse_declaration_specifiers(parser, 0, 0, 0, &type);

    if (saw <= 0) {
        set_error(parser, "expected type name");
        return -1;
    }

    if (current_is_punct(parser, "*") || current_is_punct(parser, "(") || current_is_punct(parser, "[")) {
        rt_memset(&declarator, 0, sizeof(declarator));
        if (parse_declarator(parser, &declarator, 1) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_number_token_value(const CompilerToken *token, long long *value_out) {
    char text[64];
    size_t length;
    unsigned long long value = 0;

    copy_token_text(token, text, sizeof(text));
    length = rt_strlen(text);
    while (length > 0 &&
           (text[length - 1] == 'u' || text[length - 1] == 'U' ||
            text[length - 1] == 'l' || text[length - 1] == 'L')) {
        text[length - 1] = '\0';
        length -= 1U;
    }

    if (rt_parse_uint(text, &value) != 0) {
        return -1;
    }

    *value_out = (long long)value;
    return 0;
}

static int parse_char_token_value(const CompilerToken *token, long long *value_out) {
    const char *text = token->start;
    const char *cursor;
    unsigned int value = 0U;
    int digits = 0;

    if (token->length < 3 || text[0] != '\'') {
        return -1;
    }

    if (text[1] == '\\' && token->length >= 4) {
        char escaped = text[2];
        if (escaped == 'n') {
            *value_out = '\n';
        } else if (escaped == 't') {
            *value_out = '\t';
        } else if (escaped == 'r') {
            *value_out = '\r';
        } else if (escaped == 'v') {
            *value_out = '\v';
        } else if (escaped == 'f') {
            *value_out = '\f';
        } else if (escaped == 'a') {
            *value_out = '\a';
        } else if (escaped == 'b') {
            *value_out = '\b';
        } else if (escaped == 'x' || escaped == 'X') {
            int hex = 0;
            cursor = text + 3;
            while ((*cursor >= '0' && *cursor <= '9') ||
                   (*cursor >= 'a' && *cursor <= 'f') ||
                   (*cursor >= 'A' && *cursor <= 'F')) {
                if (*cursor >= '0' && *cursor <= '9') hex = *cursor - '0';
                else if (*cursor >= 'a' && *cursor <= 'f') hex = 10 + (*cursor - 'a');
                else hex = 10 + (*cursor - 'A');
                value = (value * 16U) + (unsigned int)hex;
                cursor += 1;
                digits += 1;
            }
            *value_out = digits > 0 ? (long long)(unsigned char)value : 'x';
        } else if (escaped >= '0' && escaped <= '7') {
            cursor = text + 2;
            while (digits < 3 && *cursor >= '0' && *cursor <= '7') {
                value = (value * 8U) + (unsigned int)(*cursor - '0');
                cursor += 1;
                digits += 1;
            }
            *value_out = (long long)(unsigned char)value;
        } else {
            *value_out = (unsigned char)escaped;
        }
        return 0;
    }

    *value_out = (unsigned char)text[1];
    return 0;
}

static int apply_constant_binary_op(CompilerParser *parser, const char *op, long long lhs, long long rhs, long long *value_out) {
    if (rt_strcmp(op, "*") == 0) {
        *value_out = lhs * rhs;
    } else if (rt_strcmp(op, "/") == 0) {
        if (rhs == 0) {
            set_error(parser, "division by zero in constant expression");
            return -1;
        }
        *value_out = lhs / rhs;
    } else if (rt_strcmp(op, "%") == 0) {
        if (rhs == 0) {
            set_error(parser, "division by zero in constant expression");
            return -1;
        }
        *value_out = lhs % rhs;
    } else if (rt_strcmp(op, "+") == 0) {
        *value_out = lhs + rhs;
    } else if (rt_strcmp(op, "-") == 0) {
        *value_out = lhs - rhs;
    } else if (rt_strcmp(op, "<<") == 0) {
        *value_out = lhs << rhs;
    } else if (rt_strcmp(op, ">>") == 0) {
        *value_out = (long long)(((unsigned long long)lhs) >> (unsigned int)rhs);
    } else if (rt_strcmp(op, "<") == 0) {
        *value_out = lhs < rhs;
    } else if (rt_strcmp(op, ">") == 0) {
        *value_out = lhs > rhs;
    } else if (rt_strcmp(op, "<=") == 0) {
        *value_out = lhs <= rhs;
    } else if (rt_strcmp(op, ">=") == 0) {
        *value_out = lhs >= rhs;
    } else if (rt_strcmp(op, "==") == 0) {
        *value_out = lhs == rhs;
    } else if (rt_strcmp(op, "!=") == 0) {
        *value_out = lhs != rhs;
    } else if (rt_strcmp(op, "&") == 0) {
        *value_out = lhs & rhs;
    } else if (rt_strcmp(op, "^") == 0) {
        *value_out = lhs ^ rhs;
    } else if (rt_strcmp(op, "|") == 0) {
        *value_out = lhs | rhs;
    } else if (rt_strcmp(op, "&&") == 0) {
        *value_out = (lhs && rhs) ? 1 : 0;
    } else if (rt_strcmp(op, "||") == 0) {
        *value_out = (lhs || rhs) ? 1 : 0;
    } else {
        set_error(parser, "unsupported constant expression operator");
        return -1;
    }

    return 0;
}

static int parse_constant_primary(CompilerParser *parser, long long *value_out) {
    if (parser->current.kind == COMPILER_TOKEN_NUMBER) {
        if (parse_number_token_value(&parser->current, value_out) != 0) {
            set_error(parser, "invalid integer constant");
            return -1;
        }
        return advance(parser);
    }

    if (parser->current.kind == COMPILER_TOKEN_CHAR) {
        if (parse_char_token_value(&parser->current, value_out) != 0) {
            set_error(parser, "invalid character constant");
            return -1;
        }
        return advance(parser);
    }

    if (current_is_identifier(parser)) {
        char name[COMPILER_TYPEDEF_NAME_CAPACITY];
        copy_token_text(&parser->current, name, sizeof(name));
        if (compiler_semantic_lookup_constant(&parser->semantic, name, value_out) != 0) {
            set_error(parser, "expected integer constant expression");
            return -1;
        }
        return advance(parser);
    }

    if (current_is_punct(parser, "(")) {
        if (advance(parser) != 0 || parse_constant_expression(parser, value_out) != 0 || expect_punct(parser, ")") != 0) {
            return -1;
        }
        return 0;
    }

    set_error(parser, "expected integer constant expression");
    return -1;
}

static int parse_constant_unary(CompilerParser *parser, long long *value_out) {
    if (current_is_punct(parser, "+")) {
        if (advance(parser) != 0) {
            return -1;
        }
        return parse_constant_unary(parser, value_out);
    }

    if (current_is_punct(parser, "-") || current_is_punct(parser, "!") || current_is_punct(parser, "~")) {
        char op[4];
        copy_token_text(&parser->current, op, sizeof(op));
        if (advance(parser) != 0 || parse_constant_unary(parser, value_out) != 0) {
            return -1;
        }
        if (rt_strcmp(op, "-") == 0) {
            *value_out = -*value_out;
        } else if (rt_strcmp(op, "!") == 0) {
            *value_out = !*value_out;
        } else {
            *value_out = ~*value_out;
        }
        return 0;
    }

    return parse_constant_primary(parser, value_out);
}

static const char *match_constant_binary_op(const CompilerParser *parser, int level) {
    switch (level) {
        case 0:
            if (current_is_punct(parser, "*") || current_is_punct(parser, "/") || current_is_punct(parser, "%")) {
                return parser->current.start;
            }
            break;
        case 1:
            if (current_is_punct(parser, "+") || current_is_punct(parser, "-")) {
                return parser->current.start;
            }
            break;
        case 2:
            if (current_is_punct(parser, "<<") || current_is_punct(parser, ">>")) {
                return parser->current.start;
            }
            break;
        case 3:
            if (current_is_punct(parser, "<") || current_is_punct(parser, ">") ||
                current_is_punct(parser, "<=") || current_is_punct(parser, ">=")) {
                return parser->current.start;
            }
            break;
        case 4:
            if (current_is_punct(parser, "==") || current_is_punct(parser, "!=")) {
                return parser->current.start;
            }
            break;
        case 5:
            if (current_is_punct(parser, "&")) {
                return parser->current.start;
            }
            break;
        case 6:
            if (current_is_punct(parser, "^")) {
                return parser->current.start;
            }
            break;
        case 7:
            if (current_is_punct(parser, "|")) {
                return parser->current.start;
            }
            break;
        case 8:
            if (current_is_punct(parser, "&&")) {
                return parser->current.start;
            }
            break;
        case 9:
            if (current_is_punct(parser, "||")) {
                return parser->current.start;
            }
            break;
    }

    return 0;
}

static int parse_constant_binary_chain(CompilerParser *parser, int level, long long *value_out) {
    long long lhs;
    char op[4];

    switch (level) {
        case 0:
            if (parse_constant_unary(parser, &lhs) != 0) {
                return -1;
            }
            break;
        case 1:
            if (parse_constant_multiplicative(parser, &lhs) != 0) {
                return -1;
            }
            break;
        case 2:
            if (parse_constant_additive(parser, &lhs) != 0) {
                return -1;
            }
            break;
        case 3:
            if (parse_constant_shift(parser, &lhs) != 0) {
                return -1;
            }
            break;
        case 4:
            if (parse_constant_relational(parser, &lhs) != 0) {
                return -1;
            }
            break;
        case 5:
            if (parse_constant_equality(parser, &lhs) != 0) {
                return -1;
            }
            break;
        case 6:
            if (parse_constant_bitand(parser, &lhs) != 0) {
                return -1;
            }
            break;
        case 7:
            if (parse_constant_bitxor(parser, &lhs) != 0) {
                return -1;
            }
            break;
        case 8:
            if (parse_constant_bitor(parser, &lhs) != 0) {
                return -1;
            }
            break;
        default:
            if (parse_constant_logical_and(parser, &lhs) != 0) {
                return -1;
            }
            break;
    }

    for (;;) {
        long long rhs;
        const char *matched = match_constant_binary_op(parser, level);

        if (matched == 0) {
            break;
        }

        copy_token_text(&parser->current, op, sizeof(op));
        if (advance(parser) != 0) {
            return -1;
        }

        switch (level) {
            case 0:
                if (parse_constant_unary(parser, &rhs) != 0) {
                    return -1;
                }
                break;
            case 1:
                if (parse_constant_multiplicative(parser, &rhs) != 0) {
                    return -1;
                }
                break;
            case 2:
                if (parse_constant_additive(parser, &rhs) != 0) {
                    return -1;
                }
                break;
            case 3:
                if (parse_constant_shift(parser, &rhs) != 0) {
                    return -1;
                }
                break;
            case 4:
                if (parse_constant_relational(parser, &rhs) != 0) {
                    return -1;
                }
                break;
            case 5:
                if (parse_constant_equality(parser, &rhs) != 0) {
                    return -1;
                }
                break;
            case 6:
                if (parse_constant_bitand(parser, &rhs) != 0) {
                    return -1;
                }
                break;
            case 7:
                if (parse_constant_bitxor(parser, &rhs) != 0) {
                    return -1;
                }
                break;
            case 8:
                if (parse_constant_bitor(parser, &rhs) != 0) {
                    return -1;
                }
                break;
            default:
                if (parse_constant_logical_and(parser, &rhs) != 0) {
                    return -1;
                }
                break;
        }

        if (apply_constant_binary_op(parser, op, lhs, rhs, &lhs) != 0) {
            return -1;
        }
    }

    *value_out = lhs;
    return 0;
}

static int parse_constant_multiplicative(CompilerParser *parser, long long *value_out) {
    return parse_constant_binary_chain(parser, 0, value_out);
}

static int parse_constant_additive(CompilerParser *parser, long long *value_out) {
    return parse_constant_binary_chain(parser, 1, value_out);
}

static int parse_constant_shift(CompilerParser *parser, long long *value_out) {
    return parse_constant_binary_chain(parser, 2, value_out);
}

static int parse_constant_relational(CompilerParser *parser, long long *value_out) {
    return parse_constant_binary_chain(parser, 3, value_out);
}

static int parse_constant_equality(CompilerParser *parser, long long *value_out) {
    return parse_constant_binary_chain(parser, 4, value_out);
}

static int parse_constant_bitand(CompilerParser *parser, long long *value_out) {
    return parse_constant_binary_chain(parser, 5, value_out);
}

static int parse_constant_bitxor(CompilerParser *parser, long long *value_out) {
    return parse_constant_binary_chain(parser, 6, value_out);
}

static int parse_constant_bitor(CompilerParser *parser, long long *value_out) {
    return parse_constant_binary_chain(parser, 7, value_out);
}

static int parse_constant_logical_and(CompilerParser *parser, long long *value_out) {
    return parse_constant_binary_chain(parser, 8, value_out);
}

static int parse_constant_logical_or(CompilerParser *parser, long long *value_out) {
    return parse_constant_binary_chain(parser, 9, value_out);
}

static int parse_constant_expression(CompilerParser *parser, long long *value_out) {
    if (parse_constant_logical_or(parser, value_out) != 0) {
        return -1;
    }

    if (current_is_punct(parser, "?")) {
        long long true_value;
        long long false_value;

        if (advance(parser) != 0 ||
            parse_constant_expression(parser, &true_value) != 0 ||
            expect_punct(parser, ":") != 0 ||
            parse_constant_expression(parser, &false_value) != 0) {
            return -1;
        }
        *value_out = *value_out ? true_value : false_value;
    }

    return 0;
}

int parse_enum_specifier(CompilerParser *parser) {
    long long next_value = 0;

    if (advance(parser) != 0) {
        return -1;
    }

    if (current_is_identifier(parser) && advance(parser) != 0) {
        return -1;
    }

    if (!current_is_punct(parser, "{")) {
        return 0;
    }

    if (advance(parser) != 0) {
        return -1;
    }

    while (!current_is_punct(parser, "}") && parser->current.kind != COMPILER_TOKEN_EOF) {
        char name[COMPILER_TYPEDEF_NAME_CAPACITY];
        long long value = next_value;

        if (expect_identifier(parser, name, sizeof(name), 0) != 0) {
            return -1;
        }

        if (current_is_punct(parser, "=")) {
            if (advance(parser) != 0 || parse_constant_expression(parser, &value) != 0) {
                return -1;
            }
        }

        if (compiler_semantic_declare_constant(&parser->semantic, name, value) != 0) {
            return semantic_error(parser);
        }
        if (emit_ir_status(parser, compiler_ir_emit_constant(&parser->ir, name, value)) != 0) {
            return -1;
        }

        next_value = value + 1;
        if (!current_is_punct(parser, ",")) {
            break;
        }
        if (advance(parser) != 0) {
            return -1;
        }
    }

    return expect_punct(parser, "}");
}
