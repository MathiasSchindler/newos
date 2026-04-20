/* Type, enum, and constant-expression parsing helpers. */

#include "parser_internal.h"

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
                saw_explicit_base = 1;
            } else if (current_is_keyword(parser, "char")) {
                type_out->base = COMPILER_BASE_CHAR;
                saw_explicit_base = 1;
            } else if (current_is_int_family_keyword(parser) ||
                       (current_is_identifier(parser) && token_text_equals(&parser->current, "__int128"))) {
                type_out->base = COMPILER_BASE_INT;
                saw_explicit_base = 1;
            } else if (current_is_keyword(parser, "unsigned")) {
                type_out->base = COMPILER_BASE_INT;
                type_out->is_unsigned = 1;
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

                if (current_is_identifier(parser) && advance(parser) != 0) {
                    return -1;
                }

                if (current_is_punct(parser, "{") && skip_balanced_group(parser, "{", "}") != 0) {
                    return -1;
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
        } else if (escaped == '0') {
            *value_out = '\0';
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
        *value_out = lhs >> rhs;
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
