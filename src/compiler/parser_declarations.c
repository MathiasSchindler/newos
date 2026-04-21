/* Declarator, parameter, and top-level declaration parsing helpers. */

#include "parser_internal.h"

#include <limits.h>

static int checked_add_long_long(long long lhs, long long rhs, long long *result_out) {
    if (result_out == 0) {
        return -1;
    }
    if ((rhs > 0 && lhs > LLONG_MAX - rhs) || (rhs < 0 && lhs < LLONG_MIN - rhs)) {
        return -1;
    }
    *result_out = lhs + rhs;
    return 0;
}

static int checked_multiply_long_long(long long lhs, long long rhs, long long *result_out) {
    if (result_out == 0) {
        return -1;
    }
    if (lhs == 0 || rhs == 0) {
        *result_out = 0;
        return 0;
    }

    if ((lhs == LLONG_MIN && rhs == -1) || (rhs == LLONG_MIN && lhs == -1)) {
        return -1;
    }

    if (lhs > 0) {
        if (rhs > 0) {
            if (lhs > LLONG_MAX / rhs) {
                return -1;
            }
        } else if (rhs < LLONG_MIN / lhs) {
            return -1;
        }
    } else {
        if (rhs > 0) {
            if (lhs < LLONG_MIN / rhs) {
                return -1;
            }
        } else if (rhs < LLONG_MAX / lhs) {
            return -1;
        }
    }

    *result_out = lhs * rhs;
    return 0;
}

static int parse_parameter_declaration(CompilerParser *parser, CompilerDeclarator *owner) {
    CompilerType type;
    CompilerType parameter_type;
    int saw;
    CompilerDeclarator declarator;

    compiler_type_init(&type);
    saw = parse_declaration_specifiers(parser, 0, 0, 0, &type);

    if (saw <= 0) {
        set_error(parser, "expected parameter declaration");
        return -1;
    }

    rt_memset(&declarator, 0, sizeof(declarator));
    if (!current_is_punct(parser, ",") && !current_is_punct(parser, ")")) {
        if (parse_declarator(parser, &declarator, 1) != 0) {
            return -1;
        }
    }

    parameter_type = type;
    if (parser_add_pointer_depth(parser, &parameter_type.pointer_depth, declarator.pointer_depth) != 0) {
        return -1;
    }
    parameter_type.is_function = 0;
    parameter_type.is_array = declarator.is_array;
    parameter_type.array_length = declarator.array_length;
    parameter_type.array_stride = declarator.array_stride;
    if (declarator.is_function) {
        if (parser_add_pointer_depth(parser, &parameter_type.pointer_depth, 1) != 0) {
            return -1;
        }
    }

    if (owner != 0 &&
        declarator.name[0] != '\0' &&
        owner->parameter_count < sizeof(owner->parameter_names) / sizeof(owner->parameter_names[0])) {
        rt_copy_string(
            owner->parameter_names[owner->parameter_count],
            sizeof(owner->parameter_names[owner->parameter_count]),
            declarator.name
        );
        owner->parameter_types[owner->parameter_count] = parameter_type;
        owner->parameter_count += 1U;
    }

    return 0;
}

static int parse_array_length_token_value(const CompilerToken *token, unsigned long long *value_out) {
    char text[64];
    size_t length;
    unsigned long long value = 0;

    if (token == 0 || value_out == 0 || token->kind != COMPILER_TOKEN_NUMBER) {
        return -1;
    }

    copy_token_text(token, text, sizeof(text));
    length = rt_strlen(text);
    while (length > 0 &&
           (text[length - 1] == 'u' || text[length - 1] == 'U' ||
            text[length - 1] == 'l' || text[length - 1] == 'L')) {
        text[length - 1] = '\0';
        length -= 1U;
    }

    if (rt_parse_uint(text, &value) == 0 && value > 0ULL) {
        *value_out = value;
        return 0;
    }
    return -1;
}

static int maybe_capture_array_length(CompilerParser *parser, unsigned long long *value_out) {
    long long sum = 0;
    long long term = 0;
    char pending_op = '+';
    int saw_value = 0;

    if (value_out == 0) {
        return -1;
    }
    *value_out = 0ULL;

    if (parser == 0) {
        return 0;
    }

    while (parser->current.kind != COMPILER_TOKEN_EOF && !current_is_punct(parser, "]")) {
        if (parser->current.kind == COMPILER_TOKEN_NUMBER) {
            unsigned long long parsed = 0ULL;
            long long value;

            if (parse_array_length_token_value(&parser->current, &parsed) == 0) {
                long long next_sum;

                if (parsed > (unsigned long long)LLONG_MAX) {
                    set_error(parser, "array bound is too large");
                    return -1;
                }
                value = (long long)parsed;
                if (!saw_value) {
                    term = value;
                    saw_value = 1;
                } else if (pending_op == '*') {
                    if (checked_multiply_long_long(term, value, &term) != 0) {
                        set_error(parser, "array bound is too large");
                        return -1;
                    }
                } else if (pending_op == '/') {
                    if (value != 0) {
                        term /= value;
                    }
                } else if (pending_op == '-') {
                    if (checked_add_long_long(sum, term, &next_sum) != 0) {
                        set_error(parser, "array bound is too large");
                        return -1;
                    }
                    sum = next_sum;
                    term = -value;
                } else {
                    if (checked_add_long_long(sum, term, &next_sum) != 0) {
                        set_error(parser, "array bound is too large");
                        return -1;
                    }
                    sum = next_sum;
                    term = value;
                }
            }
        } else if (parser->current.kind == COMPILER_TOKEN_PUNCTUATOR) {
            char punct[8];
            copy_token_text(&parser->current, punct, sizeof(punct));
            if (rt_strcmp(punct, "+") == 0 || rt_strcmp(punct, "-") == 0 ||
                rt_strcmp(punct, "*") == 0 || rt_strcmp(punct, "/") == 0) {
                pending_op = punct[0];
            }
        }

        if (advance(parser) != 0) {
            return -1;
        }
    }

    if (saw_value) {
        long long total = 0;

        if (checked_add_long_long(sum, term, &total) != 0) {
            set_error(parser, "array bound is too large");
            return -1;
        }
        if (total > 0) {
            *value_out = (unsigned long long)total;
        }
    }
    return 0;
}

static int parse_parameter_list(CompilerParser *parser, CompilerDeclarator *declarator) {
    if (current_is_keyword(parser, "void")) {
        CompilerToken next;
        if (peek_token(parser, &next) == 0 && next.kind == COMPILER_TOKEN_PUNCTUATOR && token_text_equals(&next, ")")) {
            return advance(parser);
        }
    }

    for (;;) {
        if (current_is_punct(parser, "...")) {
            return advance(parser);
        }

        if (parse_parameter_declaration(parser, declarator) != 0) {
            return -1;
        }

        if (!current_is_punct(parser, ",")) {
            break;
        }

        if (advance(parser) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_direct_declarator(CompilerParser *parser, CompilerDeclarator *declarator, int allow_abstract) {
    if (current_is_identifier(parser)) {
        copy_token_text(&parser->current, declarator->name, sizeof(declarator->name));
        if (advance(parser) != 0) {
            return -1;
        }
    } else if (current_is_punct(parser, "(")) {
        if (advance(parser) != 0 || parse_declarator(parser, declarator, 1) != 0 || expect_punct(parser, ")") != 0) {
            return -1;
        }
    } else if (!allow_abstract) {
        set_error(parser, "expected declarator");
        return -1;
    }

    while (current_is_punct(parser, "(") || current_is_punct(parser, "[")) {
        if (current_is_punct(parser, "(")) {
            declarator->is_function = 1;
            if (advance(parser) != 0) {
                return -1;
            }

            if (!current_is_punct(parser, ")") && parse_parameter_list(parser, declarator) != 0) {
                return -1;
            }

            if (expect_punct(parser, ")") != 0) {
                return -1;
            }
            continue;
        }

        if (advance(parser) != 0) {
            return -1;
        }
        if (!current_is_punct(parser, "]")) {
            unsigned long long array_value = 0ULL;

            if (maybe_capture_array_length(parser, &array_value) != 0) {
                return -1;
            }
            if (array_value > 0ULL) {
                if (!declarator->is_array && declarator->array_length == 0ULL) {
                    declarator->array_length = array_value;
                } else if (declarator->array_length == 0ULL) {
                    declarator->array_stride = array_value;
                } else if (declarator->array_stride == 0ULL) {
                    declarator->array_stride = array_value;
                } else {
                    if (declarator->array_stride > ULLONG_MAX / array_value) {
                        set_error(parser, "array bound is too large");
                        return -1;
                    }
                    declarator->array_stride *= array_value;
                }
            }
        }
        declarator->is_array = 1;
        if (expect_punct(parser, "]") != 0) {
            return -1;
        }
    }

    return 0;
}

int parse_declarator(CompilerParser *parser, CompilerDeclarator *declarator, int allow_abstract) {
    rt_memset(declarator, 0, sizeof(*declarator));

    while (current_is_punct(parser, "*")) {
        if (parser_add_pointer_depth(parser, &declarator->pointer_depth, 1) != 0) {
            return -1;
        }
        if (advance(parser) != 0) {
            return -1;
        }

        while (current_is_keyword(parser, "const") ||
               current_is_keyword(parser, "volatile") ||
               current_is_keyword(parser, "restrict")) {
            if (advance(parser) != 0) {
                return -1;
            }
        }
    }

    return parse_direct_declarator(parser, declarator, allow_abstract);
}

static int declare_symbol(
    CompilerParser *parser,
    const CompilerDeclarator *declarator,
    const CompilerType *base_type,
    int is_typedef,
    int is_extern,
    int is_static,
    int is_definition,
    int emit_summary
) {
    CompilerType symbol_type = *base_type;

    if (parser_add_pointer_depth(parser, &symbol_type.pointer_depth, declarator->pointer_depth) != 0) {
        return -1;
    }
    symbol_type.is_function = declarator->is_function;
    symbol_type.is_array = declarator->is_array;
    symbol_type.array_length = declarator->array_length;
    symbol_type.array_stride = declarator->array_stride;

    if (is_typedef) {
        if ((symbol_type.base == COMPILER_BASE_STRUCT || symbol_type.base == COMPILER_BASE_UNION) &&
            symbol_type.aggregate_name[0] == '\0') {
            rt_copy_string(symbol_type.aggregate_name, sizeof(symbol_type.aggregate_name), declarator->name);
        }
        if (parser_emit_type_layout_notes(parser, &symbol_type) != 0) {
            return -1;
        }
        if (add_typedef_name(parser, declarator->name) != 0) {
            set_error(parser, "typedef table exhausted");
            return -1;
        }
        if (compiler_semantic_declare(&parser->semantic, declarator->name, COMPILER_SYMBOL_TYPEDEF, &symbol_type, 1) != 0) {
            return semantic_error(parser);
        }
        if (emit_ir_status(parser, compiler_ir_emit_note(&parser->ir, "typedef", declarator->name)) != 0) {
            return -1;
        }
        return emit_summary ? emit_ast_line(parser, "typedef", declarator->name) : 0;
    }

    if (compiler_semantic_declare(
            &parser->semantic,
            declarator->name,
            declarator->is_function ? COMPILER_SYMBOL_FUNCTION : COMPILER_SYMBOL_OBJECT,
            &symbol_type,
            is_definition
        ) != 0) {
        return semantic_error(parser);
    }

    if (parser_emit_type_layout_notes(parser, &symbol_type) != 0) {
        return -1;
    }

    if (declarator->name[0] != '\0') {
        const char *storage = "local";

        if (is_static) {
            storage = parser->semantic.in_function ? "local_static" : "static";
        } else if (!parser->semantic.in_function) {
            if (is_extern && !is_definition) {
                storage = "extern";
            } else {
                storage = "global";
            }
        }

        if (emit_ir_status(parser, compiler_ir_emit_decl(&parser->ir, storage, declarator->is_function, &symbol_type, declarator->name)) != 0) {
            return -1;
        }
    }

    return emit_summary ? emit_ast_line(parser, declarator->is_function ? "function" : "declaration", declarator->name) : 0;
}

int parse_declaration_or_function(CompilerParser *parser, int allow_function_body, int emit_summary) {
    CompilerType declared_type;
    int is_typedef = 0;
    int is_extern = 0;
    int is_static = 0;
    int saw;

    if ((parser->current.kind == COMPILER_TOKEN_KEYWORD || parser->current.kind == COMPILER_TOKEN_IDENTIFIER) &&
        (token_text_equals(&parser->current, "_Static_assert") || token_text_equals(&parser->current, "static_assert"))) {
        if (advance(parser) != 0 || skip_balanced_group(parser, "(", ")") != 0 || expect_punct(parser, ";") != 0) {
            return -1;
        }
        return 0;
    }

    compiler_type_init(&declared_type);
    saw = parse_declaration_specifiers(parser, &is_typedef, &is_extern, &is_static, &declared_type);

    if (saw <= 0) {
        set_error(parser, "expected declaration");
        return -1;
    }

    if (current_is_punct(parser, ";")) {
        if (parser_emit_type_layout_notes(parser, &declared_type) != 0) {
            return -1;
        }
        return advance(parser);
    }

    for (;;) {
        CompilerDeclarator declarator;
        int is_definition;

        if (parse_declarator(parser, &declarator, 0) != 0) {
            return -1;
        }

        if (allow_function_body && declarator.is_function && current_is_punct(parser, "{")) {
            size_t i;
            CompilerType function_type = declared_type;

            if (declare_symbol(parser, &declarator, &declared_type, 0, is_extern, is_static, 1, emit_summary) != 0) {
                return -1;
            }

            if (parser_add_pointer_depth(parser, &function_type.pointer_depth, declarator.pointer_depth) != 0) {
                return -1;
            }
            function_type.is_function = 1;
            function_type.is_array = declarator.is_array;
            function_type.array_length = declarator.array_length;
            function_type.array_stride = declarator.array_stride;
            parser->pending_function_type = function_type;
            rt_copy_string(parser->pending_function_name, sizeof(parser->pending_function_name), declarator.name);
            parser->pending_parameter_count = declarator.parameter_count;
            parser->pending_function_scope = 1;
            if (emit_ir_status(parser, compiler_ir_emit_function_begin(&parser->ir, declarator.name, &function_type)) != 0) {
                return -1;
            }
            for (i = 0; i < declarator.parameter_count; ++i) {
                rt_copy_string(
                    parser->pending_parameter_names[i],
                    sizeof(parser->pending_parameter_names[i]),
                    declarator.parameter_names[i]
                );
                parser->pending_parameter_types[i] = declarator.parameter_types[i];
            }
            return parse_compound_statement(parser);
        }

        is_definition = declarator.is_function ? 0 : (!is_extern || current_is_punct(parser, "="));
        if (declare_symbol(parser, &declarator, &declared_type, is_typedef, is_extern, is_static, is_definition, emit_summary) != 0) {
            return -1;
        }

        if (current_is_punct(parser, "=")) {
            const char *init_start;
            char init_text[COMPILER_IR_LINE_CAPACITY];

            if (advance(parser) != 0) {
                return -1;
            }
            init_start = parser->current.start;
            if (parse_initializer(parser) != 0) {
                return -1;
            }
            copy_normalized_span(init_start, parser->current.start, init_text, sizeof(init_text), "0");
            if (declarator.name[0] != '\0' &&
                emit_ir_status(parser, compiler_ir_emit_assign(&parser->ir, declarator.name, init_text)) != 0) {
                return -1;
            }
        }

        if (!current_is_punct(parser, ",")) {
            break;
        }

        if (advance(parser) != 0) {
            return -1;
        }
    }

    return expect_punct(parser, ";");
}
