#include "parser.h"

#include "runtime.h"

typedef struct {
    char name[COMPILER_TYPEDEF_NAME_CAPACITY];
    int is_function;
    int is_array;
    int pointer_depth;
    char parameter_names[64][COMPILER_TYPEDEF_NAME_CAPACITY];
    size_t parameter_count;
} CompilerDeclarator;

static int parse_expression(CompilerParser *parser);
static int parse_assignment_expression(CompilerParser *parser);
static int parse_statement(CompilerParser *parser);
static int parse_compound_statement(CompilerParser *parser);
static int parse_declaration_or_function(CompilerParser *parser, int allow_function_body, int emit_summary);
static int parse_declarator(CompilerParser *parser, CompilerDeclarator *declarator, int allow_abstract);

static int token_text_equals(const CompilerToken *token, const char *text) {
    size_t i = 0;

    while (i < token->length && text[i] != '\0') {
        if (token->start[i] != text[i]) {
            return 0;
        }
        i += 1;
    }

    return i == token->length && text[i] == '\0';
}

static int current_is_punct(const CompilerParser *parser, const char *text) {
    return parser->current.kind == COMPILER_TOKEN_PUNCTUATOR && token_text_equals(&parser->current, text);
}

static int current_is_keyword(const CompilerParser *parser, const char *text) {
    return parser->current.kind == COMPILER_TOKEN_KEYWORD && token_text_equals(&parser->current, text);
}

static int current_is_identifier(const CompilerParser *parser) {
    return parser->current.kind == COMPILER_TOKEN_IDENTIFIER;
}

static int current_is_assignment_op(const CompilerParser *parser) {
    return current_is_punct(parser, "=") ||
           current_is_punct(parser, "+=") ||
           current_is_punct(parser, "-=") ||
           current_is_punct(parser, "*=") ||
           current_is_punct(parser, "/=") ||
           current_is_punct(parser, "%=") ||
           current_is_punct(parser, "<<=") ||
           current_is_punct(parser, ">>=") ||
           current_is_punct(parser, "&=") ||
           current_is_punct(parser, "^=") ||
           current_is_punct(parser, "|=");
}

static void copy_token_text(const CompilerToken *token, char *buffer, size_t buffer_size) {
    size_t count = token->length;

    if (buffer_size == 0) {
        return;
    }

    if (count + 1 > buffer_size) {
        count = buffer_size - 1;
    }

    memcpy(buffer, token->start, count);
    buffer[count] = '\0';
}

static void set_error(CompilerParser *parser, const char *message) {
    rt_copy_string(parser->error_message, sizeof(parser->error_message), message);
    parser->error_line = parser->current.line;
    parser->error_column = parser->current.column;
}

static int semantic_error(CompilerParser *parser) {
    set_error(parser, compiler_semantic_error_message(&parser->semantic));
    return -1;
}

static int ir_error(CompilerParser *parser) {
    set_error(parser, compiler_ir_error_message(&parser->ir));
    return -1;
}

static int emit_ir_status(CompilerParser *parser, int status) {
    return status == 0 ? 0 : ir_error(parser);
}

static void copy_normalized_span(const char *start, const char *end, char *buffer, size_t buffer_size, const char *fallback) {
    size_t out = 0;
    int in_space = 1;

    if (buffer_size == 0) {
        return;
    }

    if (start == 0 || end == 0 || end < start) {
        rt_copy_string(buffer, buffer_size, fallback != 0 ? fallback : "");
        return;
    }

    while (start < end && rt_is_space(*start)) {
        start += 1;
    }
    while (end > start && rt_is_space(end[-1])) {
        end -= 1;
    }

    while (start < end && out + 1 < buffer_size) {
        char ch = *start++;

        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == '\v' || ch == '\f') {
            ch = ' ';
        }

        if (ch == ' ') {
            if (in_space) {
                continue;
            }
            in_space = 1;
        } else {
            in_space = 0;
        }

        buffer[out++] = ch;
    }

    buffer[out] = '\0';
    if (out == 0 && fallback != 0) {
        rt_copy_string(buffer, buffer_size, fallback);
    }
}

static int advance(CompilerParser *parser) {
    if (compiler_lexer_next(&parser->lexer, &parser->current) != 0) {
        rt_copy_string(parser->error_message, sizeof(parser->error_message), compiler_lexer_error_message(&parser->lexer));
        parser->error_line = parser->lexer.line;
        parser->error_column = parser->lexer.column;
        return -1;
    }

    return 0;
}

static int peek_token(const CompilerParser *parser, CompilerToken *token_out) {
    CompilerLexer snapshot = parser->lexer;
    return compiler_lexer_next(&snapshot, token_out);
}

static int emit_ast_line(CompilerParser *parser, const char *kind, const char *name) {
    if (!parser->dump_ast || name == 0 || name[0] == '\0') {
        return 0;
    }

    if (rt_write_cstr(parser->output_fd, kind) != 0 ||
        rt_write_char(parser->output_fd, ' ') != 0 ||
        rt_write_line(parser->output_fd, name) != 0) {
        set_error(parser, "failed while writing AST output");
        return -1;
    }

    return 0;
}

static int add_typedef_name(CompilerParser *parser, const char *name) {
    size_t i;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }

    for (i = 0; i < parser->typedef_count; ++i) {
        if (rt_strcmp(parser->typedef_names[i], name) == 0) {
            return 0;
        }
    }

    if (parser->typedef_count >= COMPILER_MAX_TYPEDEF_NAMES) {
        return -1;
    }

    rt_copy_string(parser->typedef_names[parser->typedef_count], sizeof(parser->typedef_names[parser->typedef_count]), name);
    parser->typedef_count += 1U;
    return 0;
}

static int is_typedef_name(const CompilerParser *parser, const CompilerToken *token) {
    char name[COMPILER_TYPEDEF_NAME_CAPACITY];
    size_t i;

    if (token->kind != COMPILER_TOKEN_IDENTIFIER) {
        return 0;
    }

    copy_token_text(token, name, sizeof(name));
    for (i = 0; i < parser->typedef_count; ++i) {
        if (rt_strcmp(parser->typedef_names[i], name) == 0) {
            return 1;
        }
    }

    return 0;
}

static int maybe_type_identifier(const CompilerParser *parser, int allow_unknown_identifiers) {
    CompilerToken next;

    if (!current_is_identifier(parser)) {
        return 0;
    }

    if (is_typedef_name(parser, &parser->current)) {
        return 1;
    }

    if (!allow_unknown_identifiers) {
        return 0;
    }

    if (peek_token(parser, &next) != 0) {
        return 0;
    }

    if (next.kind == COMPILER_TOKEN_IDENTIFIER) {
        return 1;
    }

    if (next.kind == COMPILER_TOKEN_PUNCTUATOR && token_text_equals(&next, "*")) {
        return 1;
    }

    return 0;
}

static int token_starts_decl_specifier(const CompilerParser *parser) {
    if (current_is_keyword(parser, "typedef") ||
        current_is_keyword(parser, "extern") ||
        current_is_keyword(parser, "static") ||
        current_is_keyword(parser, "auto") ||
        current_is_keyword(parser, "register") ||
        current_is_keyword(parser, "inline") ||
        current_is_keyword(parser, "const") ||
        current_is_keyword(parser, "volatile") ||
        current_is_keyword(parser, "restrict") ||
        current_is_keyword(parser, "void") ||
        current_is_keyword(parser, "char") ||
        current_is_keyword(parser, "short") ||
        current_is_keyword(parser, "int") ||
        current_is_keyword(parser, "long") ||
        current_is_keyword(parser, "signed") ||
        current_is_keyword(parser, "unsigned") ||
        current_is_keyword(parser, "float") ||
        current_is_keyword(parser, "double") ||
        current_is_keyword(parser, "struct") ||
        current_is_keyword(parser, "union") ||
        current_is_keyword(parser, "enum")) {
        return 1;
    }

    return maybe_type_identifier(parser, 1);
}

static int token_starts_known_type_specifier(const CompilerParser *parser) {
    if (current_is_keyword(parser, "const") ||
        current_is_keyword(parser, "volatile") ||
        current_is_keyword(parser, "restrict") ||
        current_is_keyword(parser, "void") ||
        current_is_keyword(parser, "char") ||
        current_is_keyword(parser, "short") ||
        current_is_keyword(parser, "int") ||
        current_is_keyword(parser, "long") ||
        current_is_keyword(parser, "signed") ||
        current_is_keyword(parser, "unsigned") ||
        current_is_keyword(parser, "float") ||
        current_is_keyword(parser, "double") ||
        current_is_keyword(parser, "struct") ||
        current_is_keyword(parser, "union") ||
        current_is_keyword(parser, "enum")) {
        return 1;
    }

    return maybe_type_identifier(parser, 0);
}

static int looks_like_declaration(const CompilerParser *parser) {
    return token_starts_decl_specifier(parser);
}

static int consume_punct(CompilerParser *parser, const char *text) {
    if (!current_is_punct(parser, text)) {
        return 0;
    }

    return advance(parser) == 0 ? 1 : -1;
}

static int expect_punct(CompilerParser *parser, const char *text) {
    int consumed = consume_punct(parser, text);
    if (consumed == 1) {
        return 0;
    }
    if (consumed < 0) {
        return -1;
    }
    set_error(parser, "expected punctuation");
    return -1;
}

static int expect_identifier(CompilerParser *parser, char *name_out, size_t name_size, int allow_missing) {
    if (current_is_identifier(parser)) {
        if (name_out != 0) {
            copy_token_text(&parser->current, name_out, name_size);
        }
        return advance(parser);
    }

    if (allow_missing) {
        if (name_out != 0 && name_size > 0) {
            name_out[0] = '\0';
        }
        return 0;
    }

    set_error(parser, "expected identifier");
    return -1;
}

static int skip_balanced_group(CompilerParser *parser, const char *open_text, const char *close_text) {
    int depth = 1;

    if (expect_punct(parser, open_text) != 0) {
        return -1;
    }

    while (parser->current.kind != COMPILER_TOKEN_EOF && depth > 0) {
        if (current_is_punct(parser, open_text)) {
            depth += 1;
        } else if (current_is_punct(parser, close_text)) {
            depth -= 1;
        }

        if (advance(parser) != 0) {
            return -1;
        }
    }

    if (depth != 0) {
        set_error(parser, "unterminated balanced token group");
        return -1;
    }

    return 0;
}

static int parse_declaration_specifiers(CompilerParser *parser, int *is_typedef_out, CompilerType *type_out) {
    int saw_any = 0;
    int saw_explicit_base = 0;

    if (is_typedef_out != 0) {
        *is_typedef_out = 0;
    }
    if (type_out != 0) {
        compiler_type_init(type_out);
    }

    while (token_starts_decl_specifier(parser)) {
        saw_any = 1;

        if (current_is_keyword(parser, "typedef") && is_typedef_out != 0) {
            *is_typedef_out = 1;
        } else if (type_out != 0) {
            if (current_is_keyword(parser, "void")) {
                type_out->base = COMPILER_BASE_VOID;
                saw_explicit_base = 1;
            } else if (current_is_keyword(parser, "char")) {
                type_out->base = COMPILER_BASE_CHAR;
                saw_explicit_base = 1;
            } else if (current_is_keyword(parser, "short") ||
                       current_is_keyword(parser, "int") ||
                       current_is_keyword(parser, "long") ||
                       current_is_keyword(parser, "signed") ||
                       current_is_keyword(parser, "float") ||
                       current_is_keyword(parser, "double")) {
                type_out->base = COMPILER_BASE_INT;
                saw_explicit_base = 1;
            } else if (current_is_keyword(parser, "unsigned")) {
                type_out->base = COMPILER_BASE_INT;
                type_out->is_unsigned = 1;
                saw_explicit_base = 1;
            }
        }

        if (current_is_keyword(parser, "struct") ||
            current_is_keyword(parser, "union") ||
            current_is_keyword(parser, "enum")) {
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

            if (advance(parser) != 0) {
                return -1;
            }

            if (current_is_identifier(parser) && advance(parser) != 0) {
                return -1;
            }

            if (current_is_punct(parser, "{") && skip_balanced_group(parser, "{", "}") != 0) {
                return -1;
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

static int looks_like_type_name_after_lparen(const CompilerParser *parser) {
    CompilerParser snapshot = *parser;

    if (!current_is_punct(parser, "(")) {
        return 0;
    }

    if (advance(&snapshot) != 0) {
        return 0;
    }

    return token_starts_known_type_specifier(&snapshot);
}

static int parse_type_name(CompilerParser *parser) {
    CompilerType type;
    int saw;
    CompilerDeclarator declarator;

    compiler_type_init(&type);
    saw = parse_declaration_specifiers(parser, 0, &type);

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

static int parse_initializer(CompilerParser *parser) {
    if (current_is_punct(parser, "{")) {
        if (advance(parser) != 0) {
            return -1;
        }

        if (!current_is_punct(parser, "}")) {
            for (;;) {
                while (current_is_punct(parser, ".") || current_is_punct(parser, "[")) {
                    if (current_is_punct(parser, ".")) {
                        if (advance(parser) != 0 || expect_identifier(parser, 0, 0, 0) != 0) {
                            return -1;
                        }
                    } else {
                        if (advance(parser) != 0 || parse_expression(parser) != 0 || expect_punct(parser, "]") != 0) {
                            return -1;
                        }
                    }
                }

                if (current_is_punct(parser, "=") && advance(parser) != 0) {
                    return -1;
                }

                if (parse_initializer(parser) != 0) {
                    return -1;
                }

                if (!current_is_punct(parser, ",")) {
                    break;
                }
                if (advance(parser) != 0) {
                    return -1;
                }
                if (current_is_punct(parser, "}")) {
                    break;
                }
            }
        }

        return expect_punct(parser, "}");
    }

    return parse_assignment_expression(parser);
}

static int parse_primary_expression(CompilerParser *parser) {
    if (current_is_identifier(parser) ||
        parser->current.kind == COMPILER_TOKEN_NUMBER ||
        parser->current.kind == COMPILER_TOKEN_CHAR ||
        parser->current.kind == COMPILER_TOKEN_STRING) {
        CompilerTokenKind kind = parser->current.kind;
        CompilerToken next;
        char name[COMPILER_TYPEDEF_NAME_CAPACITY];
        int as_call = 0;

        if (kind == COMPILER_TOKEN_IDENTIFIER) {
            copy_token_text(&parser->current, name, sizeof(name));
            if (peek_token(parser, &next) == 0 && next.kind == COMPILER_TOKEN_PUNCTUATOR && token_text_equals(&next, "(")) {
                as_call = 1;
            }
            if (compiler_semantic_use_identifier(&parser->semantic, name, as_call) != 0) {
                return semantic_error(parser);
            }
        }

        if (advance(parser) != 0) {
            return -1;
        }

        while (kind == COMPILER_TOKEN_STRING && parser->current.kind == COMPILER_TOKEN_STRING) {
            if (advance(parser) != 0) {
                return -1;
            }
        }

        return 0;
    }

    if (current_is_punct(parser, "(")) {
        if (advance(parser) != 0 || parse_expression(parser) != 0 || expect_punct(parser, ")") != 0) {
            return -1;
        }
        return 0;
    }

    set_error(parser, "expected primary expression");
    return -1;
}

static int parse_postfix_expression(CompilerParser *parser) {
    if (parse_primary_expression(parser) != 0) {
        return -1;
    }

    while (current_is_punct(parser, "[") ||
           current_is_punct(parser, "(") ||
           current_is_punct(parser, ".") ||
           current_is_punct(parser, "->") ||
           current_is_punct(parser, "++") ||
           current_is_punct(parser, "--")) {
        if (current_is_punct(parser, "[")) {
            if (advance(parser) != 0) {
                return -1;
            }
            if (!current_is_punct(parser, "]") && parse_expression(parser) != 0) {
                return -1;
            }
            if (expect_punct(parser, "]") != 0) {
                return -1;
            }
            continue;
        }

        if (current_is_punct(parser, "(")) {
            if (advance(parser) != 0) {
                return -1;
            }
            if (!current_is_punct(parser, ")")) {
                for (;;) {
                    if (parse_assignment_expression(parser) != 0) {
                        return -1;
                    }
                    if (!current_is_punct(parser, ",")) {
                        break;
                    }
                    if (advance(parser) != 0) {
                        return -1;
                    }
                }
            }
            if (expect_punct(parser, ")") != 0) {
                return -1;
            }
            continue;
        }

        if (current_is_punct(parser, ".") || current_is_punct(parser, "->")) {
            if (advance(parser) != 0 || expect_identifier(parser, 0, 0, 0) != 0) {
                return -1;
            }
            continue;
        }

        if (advance(parser) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_cast_expression(CompilerParser *parser);

static int parse_unary_expression(CompilerParser *parser) {
    if (current_is_punct(parser, "++") ||
        current_is_punct(parser, "--") ||
        current_is_punct(parser, "&") ||
        current_is_punct(parser, "*") ||
        current_is_punct(parser, "+") ||
        current_is_punct(parser, "-") ||
        current_is_punct(parser, "!") ||
        current_is_punct(parser, "~")) {
        if (advance(parser) != 0) {
            return -1;
        }
        return parse_cast_expression(parser);
    }

    if (current_is_keyword(parser, "sizeof")) {
        if (advance(parser) != 0) {
            return -1;
        }
        if (current_is_punct(parser, "(") && looks_like_type_name_after_lparen(parser)) {
            if (advance(parser) != 0 || parse_type_name(parser) != 0 || expect_punct(parser, ")") != 0) {
                return -1;
            }
            return 0;
        }
        return parse_cast_expression(parser);
    }

    return parse_postfix_expression(parser);
}

static int parse_cast_expression(CompilerParser *parser) {
    if (current_is_punct(parser, "(") && looks_like_type_name_after_lparen(parser)) {
        if (advance(parser) != 0 || parse_type_name(parser) != 0 || expect_punct(parser, ")") != 0) {
            return -1;
        }
        return parse_cast_expression(parser);
    }

    return parse_unary_expression(parser);
}

static int parse_binary_chain(CompilerParser *parser, int (*subexpr)(CompilerParser *), const char *const *ops, size_t op_count) {
    size_t i;

    if (subexpr(parser) != 0) {
        return -1;
    }

    for (;;) {
        int matched = 0;

        for (i = 0; i < op_count; ++i) {
            if (current_is_punct(parser, ops[i])) {
                matched = 1;
                break;
            }
        }

        if (!matched) {
            break;
        }

        if (advance(parser) != 0 || subexpr(parser) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_multiplicative_expression(CompilerParser *parser) {
    static const char *const ops[] = {"*", "/", "%"};
    return parse_binary_chain(parser, parse_cast_expression, ops, sizeof(ops) / sizeof(ops[0]));
}

static int parse_additive_expression(CompilerParser *parser) {
    static const char *const ops[] = {"+", "-"};
    return parse_binary_chain(parser, parse_multiplicative_expression, ops, sizeof(ops) / sizeof(ops[0]));
}

static int parse_shift_expression(CompilerParser *parser) {
    static const char *const ops[] = {"<<", ">>"};
    return parse_binary_chain(parser, parse_additive_expression, ops, sizeof(ops) / sizeof(ops[0]));
}

static int parse_relational_expression(CompilerParser *parser) {
    static const char *const ops[] = {"<", ">", "<=", ">="};
    return parse_binary_chain(parser, parse_shift_expression, ops, sizeof(ops) / sizeof(ops[0]));
}

static int parse_equality_expression(CompilerParser *parser) {
    static const char *const ops[] = {"==", "!="};
    return parse_binary_chain(parser, parse_relational_expression, ops, sizeof(ops) / sizeof(ops[0]));
}

static int parse_bitand_expression(CompilerParser *parser) {
    static const char *const ops[] = {"&"};
    return parse_binary_chain(parser, parse_equality_expression, ops, sizeof(ops) / sizeof(ops[0]));
}

static int parse_bitxor_expression(CompilerParser *parser) {
    static const char *const ops[] = {"^"};
    return parse_binary_chain(parser, parse_bitand_expression, ops, sizeof(ops) / sizeof(ops[0]));
}

static int parse_bitor_expression(CompilerParser *parser) {
    static const char *const ops[] = {"|"};
    return parse_binary_chain(parser, parse_bitxor_expression, ops, sizeof(ops) / sizeof(ops[0]));
}

static int parse_logical_and_expression(CompilerParser *parser) {
    static const char *const ops[] = {"&&"};
    return parse_binary_chain(parser, parse_bitor_expression, ops, sizeof(ops) / sizeof(ops[0]));
}

static int parse_logical_or_expression(CompilerParser *parser) {
    static const char *const ops[] = {"||"};
    return parse_binary_chain(parser, parse_logical_and_expression, ops, sizeof(ops) / sizeof(ops[0]));
}

static int parse_conditional_expression(CompilerParser *parser) {
    if (parse_logical_or_expression(parser) != 0) {
        return -1;
    }

    if (current_is_punct(parser, "?")) {
        if (advance(parser) != 0 ||
            parse_expression(parser) != 0 ||
            expect_punct(parser, ":") != 0 ||
            parse_conditional_expression(parser) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_assignment_expression(CompilerParser *parser) {
    if (parse_conditional_expression(parser) != 0) {
        return -1;
    }

    if (current_is_assignment_op(parser)) {
        if (advance(parser) != 0 || parse_assignment_expression(parser) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_expression(CompilerParser *parser) {
    if (parse_assignment_expression(parser) != 0) {
        return -1;
    }

    while (current_is_punct(parser, ",")) {
        if (advance(parser) != 0 || parse_assignment_expression(parser) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_parameter_declaration(CompilerParser *parser, CompilerDeclarator *owner) {
    CompilerType type;
    int saw;
    CompilerDeclarator declarator;

    compiler_type_init(&type);
    saw = parse_declaration_specifiers(parser, 0, &type);

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

    if (owner != 0 &&
        declarator.name[0] != '\0' &&
        owner->parameter_count < sizeof(owner->parameter_names) / sizeof(owner->parameter_names[0])) {
        rt_copy_string(
            owner->parameter_names[owner->parameter_count],
            sizeof(owner->parameter_names[owner->parameter_count]),
            declarator.name
        );
        owner->parameter_count += 1U;
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
        if (!current_is_punct(parser, "]") && parse_expression(parser) != 0) {
            return -1;
        }
        declarator->is_array = 1;
        if (expect_punct(parser, "]") != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_declarator(CompilerParser *parser, CompilerDeclarator *declarator, int allow_abstract) {
    rt_memset(declarator, 0, sizeof(*declarator));

    while (current_is_punct(parser, "*")) {
        declarator->pointer_depth += 1;
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
    int is_definition,
    int emit_summary
) {
    CompilerType symbol_type = *base_type;

    symbol_type.pointer_depth += declarator->pointer_depth;
    symbol_type.is_function = declarator->is_function;
    symbol_type.is_array = declarator->is_array;

    if (is_typedef) {
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
            is_definition || !declarator->is_function
        ) != 0) {
        return semantic_error(parser);
    }

    if (declarator->name[0] != '\0') {
        const char *storage = parser->semantic.in_function ? "local" : "global";
        if (emit_ir_status(parser, compiler_ir_emit_decl(&parser->ir, storage, declarator->is_function, &symbol_type, declarator->name)) != 0) {
            return -1;
        }
    }

    return emit_summary ? emit_ast_line(parser, declarator->is_function ? "function" : "declaration", declarator->name) : 0;
}

static int parse_declaration_or_function(CompilerParser *parser, int allow_function_body, int emit_summary) {
    CompilerType declared_type;
    int is_typedef = 0;
    int saw;

    compiler_type_init(&declared_type);
    saw = parse_declaration_specifiers(parser, &is_typedef, &declared_type);

    if (saw <= 0) {
        set_error(parser, "expected declaration");
        return -1;
    }

    if (current_is_punct(parser, ";")) {
        return advance(parser);
    }

    for (;;) {
        CompilerDeclarator declarator;

        if (parse_declarator(parser, &declarator, 0) != 0) {
            return -1;
        }

        if (allow_function_body && declarator.is_function && current_is_punct(parser, "{")) {
            size_t i;
            CompilerType function_type = declared_type;

            if (declare_symbol(parser, &declarator, &declared_type, 0, 1, emit_summary) != 0) {
                return -1;
            }

            function_type.pointer_depth += declarator.pointer_depth;
            function_type.is_function = 1;
            function_type.is_array = declarator.is_array;
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
            }
            return parse_compound_statement(parser);
        }

        if (declare_symbol(parser, &declarator, &declared_type, is_typedef, 0, emit_summary) != 0) {
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

static int parse_statement(CompilerParser *parser) {
    if (current_is_punct(parser, "{")) {
        return parse_compound_statement(parser);
    }

    if (current_is_keyword(parser, "if")) {
        char else_label[COMPILER_IR_NAME_CAPACITY];
        char end_label[COMPILER_IR_NAME_CAPACITY];
        char cond_text[COMPILER_IR_LINE_CAPACITY];
        const char *cond_start;
        const char *cond_end;

        if (advance(parser) != 0 || expect_punct(parser, "(") != 0) {
            return -1;
        }
        cond_start = parser->current.start;
        if (parse_expression(parser) != 0) {
            return -1;
        }
        cond_end = parser->current.start;
        if (expect_punct(parser, ")") != 0) {
            return -1;
        }
        copy_normalized_span(cond_start, cond_end, cond_text, sizeof(cond_text), "1");
        if (emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "else", else_label, sizeof(else_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "endif", end_label, sizeof(end_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_branch_zero(&parser->ir, cond_text, else_label)) != 0 ||
            parse_statement(parser) != 0) {
            return -1;
        }
        if (current_is_keyword(parser, "else")) {
            if (emit_ir_status(parser, compiler_ir_emit_jump(&parser->ir, end_label)) != 0 ||
                emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, else_label)) != 0 ||
                advance(parser) != 0 ||
                parse_statement(parser) != 0 ||
                emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, end_label)) != 0) {
                return -1;
            }
        } else if (emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, else_label)) != 0) {
            return -1;
        }
        return 0;
    }

    if (current_is_keyword(parser, "while")) {
        char loop_label[COMPILER_IR_NAME_CAPACITY];
        char end_label[COMPILER_IR_NAME_CAPACITY];
        char cond_text[COMPILER_IR_LINE_CAPACITY];
        const char *cond_start;
        const char *cond_end;

        if (advance(parser) != 0 ||
            emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "while", loop_label, sizeof(loop_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "endwhile", end_label, sizeof(end_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, loop_label)) != 0 ||
            expect_punct(parser, "(") != 0) {
            return -1;
        }
        cond_start = parser->current.start;
        if (parse_expression(parser) != 0) {
            return -1;
        }
        cond_end = parser->current.start;
        if (expect_punct(parser, ")") != 0) {
            return -1;
        }
        copy_normalized_span(cond_start, cond_end, cond_text, sizeof(cond_text), "1");
        if (emit_ir_status(parser, compiler_ir_emit_branch_zero(&parser->ir, cond_text, end_label)) != 0 ||
            parse_statement(parser) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_jump(&parser->ir, loop_label)) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, end_label)) != 0) {
            return -1;
        }
        return 0;
    }

    if (current_is_keyword(parser, "do")) {
        char loop_label[COMPILER_IR_NAME_CAPACITY];
        char end_label[COMPILER_IR_NAME_CAPACITY];
        char cond_text[COMPILER_IR_LINE_CAPACITY];
        const char *cond_start;
        const char *cond_end;

        if (advance(parser) != 0 ||
            emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "do", loop_label, sizeof(loop_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "enddo", end_label, sizeof(end_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, loop_label)) != 0 ||
            parse_statement(parser) != 0) {
            return -1;
        }
        if (!current_is_keyword(parser, "while")) {
            set_error(parser, "expected while after do statement");
            return -1;
        }
        if (advance(parser) != 0 || expect_punct(parser, "(") != 0) {
            return -1;
        }
        cond_start = parser->current.start;
        if (parse_expression(parser) != 0) {
            return -1;
        }
        cond_end = parser->current.start;
        if (expect_punct(parser, ")") != 0 || expect_punct(parser, ";") != 0) {
            return -1;
        }
        copy_normalized_span(cond_start, cond_end, cond_text, sizeof(cond_text), "1");
        if (emit_ir_status(parser, compiler_ir_emit_branch_zero(&parser->ir, cond_text, end_label)) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_jump(&parser->ir, loop_label)) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, end_label)) != 0) {
            return -1;
        }
        return 0;
    }

    if (current_is_keyword(parser, "for")) {
        char loop_label[COMPILER_IR_NAME_CAPACITY];
        char end_label[COMPILER_IR_NAME_CAPACITY];
        char init_text[COMPILER_IR_LINE_CAPACITY];
        char cond_text[COMPILER_IR_LINE_CAPACITY];
        char step_text[COMPILER_IR_LINE_CAPACITY];
        const char *segment_start;

        if (advance(parser) != 0 || expect_punct(parser, "(") != 0) {
            return -1;
        }

        if (!current_is_punct(parser, ";")) {
            if (looks_like_declaration(parser)) {
                if (parse_declaration_or_function(parser, 0, 0) != 0) {
                    return -1;
                }
            } else {
                const char *init_end;
                segment_start = parser->current.start;
                if (parse_expression(parser) != 0) {
                    return -1;
                }
                init_end = parser->current.start;
                if (expect_punct(parser, ";") != 0) {
                    return -1;
                }
                copy_normalized_span(segment_start, init_end, init_text, sizeof(init_text), "");
                if (init_text[0] != '\0' && emit_ir_status(parser, compiler_ir_emit_eval(&parser->ir, init_text)) != 0) {
                    return -1;
                }
            }
        } else if (advance(parser) != 0) {
            return -1;
        }

        if (emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "for", loop_label, sizeof(loop_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "endfor", end_label, sizeof(end_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, loop_label)) != 0) {
            return -1;
        }

        if (!current_is_punct(parser, ";")) {
            segment_start = parser->current.start;
            if (parse_expression(parser) != 0) {
                return -1;
            }
            copy_normalized_span(segment_start, parser->current.start, cond_text, sizeof(cond_text), "1");
        } else {
            rt_copy_string(cond_text, sizeof(cond_text), "1");
        }
        if (expect_punct(parser, ";") != 0) {
            return -1;
        }
        if (emit_ir_status(parser, compiler_ir_emit_branch_zero(&parser->ir, cond_text, end_label)) != 0) {
            return -1;
        }

        step_text[0] = '\0';
        if (!current_is_punct(parser, ")")) {
            segment_start = parser->current.start;
            if (parse_expression(parser) != 0) {
                return -1;
            }
            copy_normalized_span(segment_start, parser->current.start, step_text, sizeof(step_text), "");
        }
        if (expect_punct(parser, ")") != 0 || parse_statement(parser) != 0) {
            return -1;
        }
        if (step_text[0] != '\0' && emit_ir_status(parser, compiler_ir_emit_eval(&parser->ir, step_text)) != 0) {
            return -1;
        }
        if (emit_ir_status(parser, compiler_ir_emit_jump(&parser->ir, loop_label)) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, end_label)) != 0) {
            return -1;
        }
        return 0;
    }

    if (current_is_keyword(parser, "switch")) {
        char cond_text[COMPILER_IR_LINE_CAPACITY];
        const char *cond_start;
        const char *cond_end;

        if (advance(parser) != 0 || expect_punct(parser, "(") != 0) {
            return -1;
        }
        cond_start = parser->current.start;
        if (parse_expression(parser) != 0) {
            return -1;
        }
        cond_end = parser->current.start;
        if (expect_punct(parser, ")") != 0) {
            return -1;
        }
        copy_normalized_span(cond_start, cond_end, cond_text, sizeof(cond_text), "0");
        if (emit_ir_status(parser, compiler_ir_emit_note(&parser->ir, "switch", cond_text)) != 0 ||
            parse_statement(parser) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_note(&parser->ir, "endswitch", "")) != 0) {
            return -1;
        }
        return 0;
    }

    if (current_is_keyword(parser, "case")) {
        char expr_text[COMPILER_IR_LINE_CAPACITY];
        const char *expr_start;
        const char *expr_end;

        if (advance(parser) != 0) {
            return -1;
        }
        expr_start = parser->current.start;
        if (parse_expression(parser) != 0) {
            return -1;
        }
        expr_end = parser->current.start;
        if (expect_punct(parser, ":") != 0) {
            return -1;
        }
        copy_normalized_span(expr_start, expr_end, expr_text, sizeof(expr_text), "0");
        if (emit_ir_status(parser, compiler_ir_emit_case(&parser->ir, expr_text)) != 0 ||
            parse_statement(parser) != 0) {
            return -1;
        }
        return 0;
    }

    if (current_is_keyword(parser, "default")) {
        if (advance(parser) != 0 || expect_punct(parser, ":") != 0) {
            return -1;
        }
        if (emit_ir_status(parser, compiler_ir_emit_default(&parser->ir)) != 0 || parse_statement(parser) != 0) {
            return -1;
        }
        return 0;
    }

    if (current_is_keyword(parser, "return")) {
        int has_value;
        const char *expr_start = 0;
        char expr_text[COMPILER_IR_LINE_CAPACITY];

        if (advance(parser) != 0) {
            return -1;
        }
        has_value = !current_is_punct(parser, ";");
        if (has_value) {
            expr_start = parser->current.start;
        }
        if (has_value && parse_expression(parser) != 0) {
            return -1;
        }
        if (compiler_semantic_check_return(&parser->semantic, has_value) != 0) {
            return semantic_error(parser);
        }
        if (has_value) {
            copy_normalized_span(expr_start, parser->current.start, expr_text, sizeof(expr_text), "0");
        } else {
            expr_text[0] = '\0';
        }
        if (emit_ir_status(parser, compiler_ir_emit_return(&parser->ir, expr_text)) != 0) {
            return -1;
        }
        return expect_punct(parser, ";");
    }

    if (current_is_keyword(parser, "break") || current_is_keyword(parser, "continue")) {
        char keyword[16];
        copy_token_text(&parser->current, keyword, sizeof(keyword));
        if (advance(parser) != 0) {
            return -1;
        }
        if (emit_ir_status(parser, compiler_ir_emit_note(&parser->ir, keyword, "")) != 0) {
            return -1;
        }
        return expect_punct(parser, ";");
    }

    if (current_is_keyword(parser, "goto")) {
        char label[COMPILER_TYPEDEF_NAME_CAPACITY];
        if (advance(parser) != 0 || expect_identifier(parser, label, sizeof(label), 0) != 0 || expect_punct(parser, ";") != 0) {
            return -1;
        }
        if (emit_ir_status(parser, compiler_ir_emit_jump(&parser->ir, label)) != 0) {
            return -1;
        }
        return 0;
    }

    if (current_is_punct(parser, ";")) {
        return advance(parser);
    }

    if (current_is_identifier(parser)) {
        CompilerToken next;
        if (peek_token(parser, &next) == 0 && next.kind == COMPILER_TOKEN_PUNCTUATOR && token_text_equals(&next, ":")) {
            char label[COMPILER_TYPEDEF_NAME_CAPACITY];
            copy_token_text(&parser->current, label, sizeof(label));
            if (advance(parser) != 0 || expect_punct(parser, ":") != 0) {
                return -1;
            }
            if (emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, label)) != 0 || parse_statement(parser) != 0) {
                return -1;
            }
            return 0;
        }
    }

    {
        const char *expr_start = parser->current.start;
        char expr_text[COMPILER_IR_LINE_CAPACITY];

        if (parse_expression(parser) != 0) {
            return -1;
        }
        copy_normalized_span(expr_start, parser->current.start, expr_text, sizeof(expr_text), "");
        if (expr_text[0] != '\0' && emit_ir_status(parser, compiler_ir_emit_eval(&parser->ir, expr_text)) != 0) {
            return -1;
        }
    }

    return expect_punct(parser, ";");
}

static int parse_compound_statement(CompilerParser *parser) {
    int entered_function_scope = 0;
    size_t i;

    if (expect_punct(parser, "{") != 0) {
        return -1;
    }

    if (compiler_semantic_enter_scope(&parser->semantic) != 0) {
        return semantic_error(parser);
    }

    if (parser->pending_function_scope) {
        CompilerType parameter_type;

        entered_function_scope = 1;
        parser->pending_function_scope = 0;
        compiler_semantic_begin_function(&parser->semantic, &parser->pending_function_type);
        compiler_type_init(&parameter_type);

        for (i = 0; i < parser->pending_parameter_count; ++i) {
            if (compiler_semantic_declare(
                    &parser->semantic,
                    parser->pending_parameter_names[i],
                    COMPILER_SYMBOL_OBJECT,
                    &parameter_type,
                    1
                ) != 0) {
                compiler_semantic_end_function(&parser->semantic);
                compiler_semantic_exit_scope(&parser->semantic);
                return semantic_error(parser);
            }
            if (emit_ir_status(parser, compiler_ir_emit_decl(&parser->ir, "param", 0, &parameter_type, parser->pending_parameter_names[i])) != 0) {
                compiler_semantic_end_function(&parser->semantic);
                compiler_semantic_exit_scope(&parser->semantic);
                return -1;
            }
        }
        parser->pending_parameter_count = 0;
    }

    while (!current_is_punct(parser, "}") && parser->current.kind != COMPILER_TOKEN_EOF) {
        if (looks_like_declaration(parser)) {
            if (parse_declaration_or_function(parser, 0, 0) != 0) {
                if (entered_function_scope) {
                    compiler_semantic_end_function(&parser->semantic);
                }
                compiler_semantic_exit_scope(&parser->semantic);
                return -1;
            }
        } else if (parse_statement(parser) != 0) {
            if (entered_function_scope) {
                compiler_semantic_end_function(&parser->semantic);
            }
            compiler_semantic_exit_scope(&parser->semantic);
            return -1;
        }
    }

    if (expect_punct(parser, "}") != 0) {
        if (entered_function_scope) {
            compiler_semantic_end_function(&parser->semantic);
        }
        compiler_semantic_exit_scope(&parser->semantic);
        return -1;
    }

    if (entered_function_scope) {
        if (emit_ir_status(parser, compiler_ir_emit_function_end(&parser->ir, parser->pending_function_name)) != 0) {
            compiler_semantic_end_function(&parser->semantic);
            compiler_semantic_exit_scope(&parser->semantic);
            return -1;
        }
        compiler_semantic_end_function(&parser->semantic);
        parser->pending_function_name[0] = '\0';
    }
    compiler_semantic_exit_scope(&parser->semantic);
    return 0;
}

void compiler_parser_init(CompilerParser *parser, const CompilerSource *source, int dump_ast, int dump_ir, int output_fd) {
    static const char *const builtin_types[] = {
        "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
        "pid_t", "uid_t", "gid_t", "mode_t", "off_t", "time_t",
        "FILE", "DIR", "socklen_t", "sa_family_t", "in_addr_t",
        "nfds_t", "speed_t", "tcflag_t", "sigset_t"
    };
    static const char *const builtin_objects[] = {
        "errno", "stdin", "stdout", "stderr", "environ"
    };
    size_t i;

    rt_memset(parser, 0, sizeof(*parser));
    parser->source = source;
    parser->dump_ast = dump_ast;
    parser->dump_ir = dump_ir;
    parser->output_fd = output_fd;
    compiler_lexer_init(&parser->lexer, source);
    compiler_ir_init(&parser->ir);
    compiler_semantic_init(&parser->semantic);

    for (i = 0; i < sizeof(builtin_types) / sizeof(builtin_types[0]); ++i) {
        CompilerType type;
        (void)add_typedef_name(parser, builtin_types[i]);
        compiler_type_init(&type);
        (void)compiler_semantic_declare(&parser->semantic, builtin_types[i], COMPILER_SYMBOL_TYPEDEF, &type, 1);
    }

    for (i = 0; i < sizeof(builtin_objects) / sizeof(builtin_objects[0]); ++i) {
        CompilerType type;
        compiler_type_init(&type);
        (void)compiler_semantic_declare(&parser->semantic, builtin_objects[i], COMPILER_SYMBOL_OBJECT, &type, 1);
    }

    (void)advance(parser);
}

int compiler_parse_translation_unit(CompilerParser *parser) {
    while (parser->current.kind != COMPILER_TOKEN_EOF) {
        if (parse_declaration_or_function(parser, 1, 1) != 0) {
            return -1;
        }
    }

    return 0;
}

const char *compiler_parser_error_message(const CompilerParser *parser) {
    return parser->error_message;
}

unsigned long long compiler_parser_error_line(const CompilerParser *parser) {
    return parser->error_line;
}

unsigned long long compiler_parser_error_column(const CompilerParser *parser) {
    return parser->error_column;
}
