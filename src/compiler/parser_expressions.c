/* Runtime expression and initializer parsing helpers. */

#include "parser_internal.h"

/* Forward declarations for mutually-recursive static helpers. */
static int parse_multiplicative_expression(CompilerParser *parser);
static int parse_additive_expression(CompilerParser *parser);
static int parse_shift_expression(CompilerParser *parser);
static int parse_relational_expression(CompilerParser *parser);
static int parse_equality_expression(CompilerParser *parser);
static int parse_bitand_expression(CompilerParser *parser);
static int parse_bitxor_expression(CompilerParser *parser);
static int parse_bitor_expression(CompilerParser *parser);
static int parse_logical_and_expression(CompilerParser *parser);
static int parse_logical_or_expression(CompilerParser *parser);

int parse_initializer(CompilerParser *parser) {
    int result = 0;

    if (parser == 0) {
        return -1;
    }
    if (parser->initializer_depth >= COMPILER_MAX_INITIALIZER_DEPTH) {
        set_error(parser, "initializer nesting too deep");
        return -1;
    }
    parser->initializer_depth += 1U;

    if (current_is_punct(parser, "{")) {
        if (advance(parser) != 0) {
            result = -1;
            goto done;
        }

        if (!current_is_punct(parser, "}")) {
            for (;;) {
                while (current_is_punct(parser, ".") || current_is_punct(parser, "[")) {
                    if (current_is_punct(parser, ".")) {
                        if (advance(parser) != 0 || expect_identifier(parser, 0, 0, 0) != 0) {
                            result = -1;
                            goto done;
                        }
                    } else {
                        if (advance(parser) != 0 || parse_expression(parser) != 0 || expect_punct(parser, "]") != 0) {
                            result = -1;
                            goto done;
                        }
                    }
                }

                if (current_is_punct(parser, "=") && advance(parser) != 0) {
                    result = -1;
                    goto done;
                }

                if (parse_initializer(parser) != 0) {
                    result = -1;
                    goto done;
                }

                if (!current_is_punct(parser, ",")) {
                    break;
                }
                if (advance(parser) != 0) {
                    result = -1;
                    goto done;
                }
                if (current_is_punct(parser, "}")) {
                    break;
                }
            }
        }

        result = expect_punct(parser, "}");
        goto done;
    }

    result = parse_assignment_expression(parser);

done:
    parser->initializer_depth -= 1U;
    return result;
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

    if (current_is_punct(parser, "(") && looks_like_compound_literal_after_lparen(parser)) {
        if (advance(parser) != 0 || parse_type_name(parser) != 0 || expect_punct(parser, ")") != 0) {
            return -1;
        }
        return parse_initializer(parser);
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
    if (current_is_punct(parser, "(") &&
        looks_like_type_name_after_lparen(parser) &&
        !looks_like_compound_literal_after_lparen(parser)) {
        if (advance(parser) != 0 || parse_type_name(parser) != 0 || expect_punct(parser, ")") != 0) {
            return -1;
        }
        return parse_cast_expression(parser);
    }

    return parse_unary_expression(parser);
}

static int parse_level_operand(CompilerParser *parser, int level) {
    switch (level) {
        case 0: return parse_cast_expression(parser);
        case 1: return parse_multiplicative_expression(parser);
        case 2: return parse_additive_expression(parser);
        case 3: return parse_shift_expression(parser);
        case 4: return parse_relational_expression(parser);
        case 5: return parse_equality_expression(parser);
        case 6: return parse_bitand_expression(parser);
        case 7: return parse_bitxor_expression(parser);
        case 8: return parse_bitor_expression(parser);
        default: return parse_logical_and_expression(parser);
    }
}

static int current_matches_binary_level(const CompilerParser *parser, int level) {
    switch (level) {
        case 0:
            return current_is_punct(parser, "*") || current_is_punct(parser, "/") || current_is_punct(parser, "%");
        case 1:
            return current_is_punct(parser, "+") || current_is_punct(parser, "-");
        case 2:
            return current_is_punct(parser, "<<") || current_is_punct(parser, ">>");
        case 3:
            return current_is_punct(parser, "<") || current_is_punct(parser, ">") ||
                   current_is_punct(parser, "<=") || current_is_punct(parser, ">=");
        case 4:
            return current_is_punct(parser, "==") || current_is_punct(parser, "!=");
        case 5:
            return current_is_punct(parser, "&");
        case 6:
            return current_is_punct(parser, "^");
        case 7:
            return current_is_punct(parser, "|");
        case 8:
            return current_is_punct(parser, "&&");
        case 9:
            return current_is_punct(parser, "||");
    }
    return 0;
}

static int parse_binary_chain(CompilerParser *parser, int level) {
    if (parse_level_operand(parser, level) != 0) {
        return -1;
    }

    while (current_matches_binary_level(parser, level)) {
        if (advance(parser) != 0) {
            return -1;
        }
        if (parse_level_operand(parser, level) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_multiplicative_expression(CompilerParser *parser) {
    return parse_binary_chain(parser, 0);
}

static int parse_additive_expression(CompilerParser *parser) {
    return parse_binary_chain(parser, 1);
}

static int parse_shift_expression(CompilerParser *parser) {
    return parse_binary_chain(parser, 2);
}

static int parse_relational_expression(CompilerParser *parser) {
    return parse_binary_chain(parser, 3);
}

static int parse_equality_expression(CompilerParser *parser) {
    return parse_binary_chain(parser, 4);
}

static int parse_bitand_expression(CompilerParser *parser) {
    return parse_binary_chain(parser, 5);
}

static int parse_bitxor_expression(CompilerParser *parser) {
    return parse_binary_chain(parser, 6);
}

static int parse_bitor_expression(CompilerParser *parser) {
    return parse_binary_chain(parser, 7);
}

static int parse_logical_and_expression(CompilerParser *parser) {
    return parse_binary_chain(parser, 8);
}

static int parse_logical_or_expression(CompilerParser *parser) {
    return parse_binary_chain(parser, 9);
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

int parse_assignment_expression(CompilerParser *parser) {
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

int parse_expression(CompilerParser *parser) {
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
