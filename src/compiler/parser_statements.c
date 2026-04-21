/* Statement and compound-scope parsing helpers. */

#include "parser_internal.h"

static int push_loop_labels(CompilerParser *parser, const char *continue_label, const char *break_label) {
    if (parser->loop_depth >= COMPILER_MAX_LOOP_DEPTH) {
        set_error(parser, "loop nesting too deep");
        return -1;
    }
    rt_copy_string(parser->continue_labels[parser->loop_depth],
                   sizeof(parser->continue_labels[parser->loop_depth]),
                   continue_label != 0 ? continue_label : "");
    rt_copy_string(parser->break_labels[parser->loop_depth],
                   sizeof(parser->break_labels[parser->loop_depth]),
                   break_label != 0 ? break_label : "");
    parser->loop_depth += 1U;
    return 0;
}

static void pop_loop_labels(CompilerParser *parser) {
    if (parser->loop_depth > 0U) {
        parser->loop_depth -= 1U;
    }
}

int parse_statement(CompilerParser *parser) {
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
            push_loop_labels(parser, loop_label, end_label) != 0) {
            return -1;
        }
        if (parse_statement(parser) != 0) {
            pop_loop_labels(parser);
            return -1;
        }
        pop_loop_labels(parser);
        if (emit_ir_status(parser, compiler_ir_emit_jump(&parser->ir, loop_label)) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, end_label)) != 0) {
            return -1;
        }
        return 0;
    }

    if (current_is_keyword(parser, "do")) {
        char loop_label[COMPILER_IR_NAME_CAPACITY];
        char continue_label[COMPILER_IR_NAME_CAPACITY];
        char end_label[COMPILER_IR_NAME_CAPACITY];
        char cond_text[COMPILER_IR_LINE_CAPACITY];
        const char *cond_start;
        const char *cond_end;

        if (advance(parser) != 0 ||
            emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "do", loop_label, sizeof(loop_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "docont", continue_label, sizeof(continue_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "enddo", end_label, sizeof(end_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, loop_label)) != 0 ||
            push_loop_labels(parser, continue_label, end_label) != 0) {
            return -1;
        }
        if (parse_statement(parser) != 0) {
            pop_loop_labels(parser);
            return -1;
        }
        pop_loop_labels(parser);
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
        if (emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, continue_label)) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_branch_zero(&parser->ir, cond_text, end_label)) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_jump(&parser->ir, loop_label)) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, end_label)) != 0) {
            return -1;
        }
        return 0;
    }

    if (current_is_keyword(parser, "for")) {
        int status = -1;
        char loop_label[COMPILER_IR_NAME_CAPACITY];
        char continue_label[COMPILER_IR_NAME_CAPACITY];
        char end_label[COMPILER_IR_NAME_CAPACITY];
        char init_text[COMPILER_IR_LINE_CAPACITY];
        char cond_text[COMPILER_IR_LINE_CAPACITY];
        char step_text[COMPILER_IR_LINE_CAPACITY];
        const char *segment_start;

        if (advance(parser) != 0 || expect_punct(parser, "(") != 0) {
            return -1;
        }
        if (compiler_semantic_enter_scope(&parser->semantic) != 0) {
            return semantic_error(parser);
        }

        if (!current_is_punct(parser, ";")) {
            if (looks_like_declaration(parser)) {
                if (parse_declaration_or_function(parser, 0, 0) != 0) {
                    goto for_cleanup;
                }
            } else {
                const char *init_end;
                segment_start = parser->current.start;
                if (parse_expression(parser) != 0) {
                    goto for_cleanup;
                }
                init_end = parser->current.start;
                if (expect_punct(parser, ";") != 0) {
                    goto for_cleanup;
                }
                copy_normalized_span(segment_start, init_end, init_text, sizeof(init_text), "");
                if (init_text[0] != '\0' && emit_ir_status(parser, compiler_ir_emit_eval(&parser->ir, init_text)) != 0) {
                    goto for_cleanup;
                }
            }
        } else if (advance(parser) != 0) {
            goto for_cleanup;
        }

        if (emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "for", loop_label, sizeof(loop_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "forstep", continue_label, sizeof(continue_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "endfor", end_label, sizeof(end_label))) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, loop_label)) != 0) {
            goto for_cleanup;
        }

        if (!current_is_punct(parser, ";")) {
            segment_start = parser->current.start;
            if (parse_expression(parser) != 0) {
                goto for_cleanup;
            }
            copy_normalized_span(segment_start, parser->current.start, cond_text, sizeof(cond_text), "1");
        } else {
            rt_copy_string(cond_text, sizeof(cond_text), "1");
        }
        if (expect_punct(parser, ";") != 0) {
            goto for_cleanup;
        }
        if (emit_ir_status(parser, compiler_ir_emit_branch_zero(&parser->ir, cond_text, end_label)) != 0) {
            goto for_cleanup;
        }

        step_text[0] = '\0';
        if (!current_is_punct(parser, ")")) {
            segment_start = parser->current.start;
            if (parse_expression(parser) != 0) {
                goto for_cleanup;
            }
            copy_normalized_span(segment_start, parser->current.start, step_text, sizeof(step_text), "");
        }
        if (expect_punct(parser, ")") != 0 || push_loop_labels(parser, continue_label, end_label) != 0) {
            goto for_cleanup;
        }
        if (parse_statement(parser) != 0) {
            pop_loop_labels(parser);
            goto for_cleanup;
        }
        pop_loop_labels(parser);
        if (emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, continue_label)) != 0) {
            goto for_cleanup;
        }
        if (step_text[0] != '\0' && emit_ir_status(parser, compiler_ir_emit_eval(&parser->ir, step_text)) != 0) {
            goto for_cleanup;
        }
        if (emit_ir_status(parser, compiler_ir_emit_jump(&parser->ir, loop_label)) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, end_label)) != 0) {
            goto for_cleanup;
        }
        status = 0;

for_cleanup:
        compiler_semantic_exit_scope(&parser->semantic);
        return status;
    }

    if (current_is_keyword(parser, "switch")) {
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
        copy_normalized_span(cond_start, cond_end, cond_text, sizeof(cond_text), "0");
        if (emit_ir_status(parser, compiler_ir_make_label(&parser->ir, "switchend", end_label, sizeof(end_label))) != 0 ||
            push_loop_labels(parser, 0, end_label) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_note(&parser->ir, "switch", cond_text)) != 0 ||
            parse_statement(parser) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_note(&parser->ir, "endswitch", "")) != 0 ||
            emit_ir_status(parser, compiler_ir_emit_label(&parser->ir, end_label)) != 0) {
            return -1;
        }
        pop_loop_labels(parser);
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
        if (parser->loop_depth > 0U) {
            const char *target = rt_strcmp(keyword, "continue") == 0
                                     ? parser->continue_labels[parser->loop_depth - 1U]
                                     : parser->break_labels[parser->loop_depth - 1U];
            if (emit_ir_status(parser, compiler_ir_emit_jump(&parser->ir, target)) != 0) {
                return -1;
            }
        } else if (emit_ir_status(parser, compiler_ir_emit_note(&parser->ir, keyword, "")) != 0) {
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

int parse_compound_statement(CompilerParser *parser) {
    int entered_function_scope = 0;
    int status = 0;
    size_t i;

    if (expect_punct(parser, "{") != 0) {
        return -1;
    }

    if (compiler_semantic_enter_scope(&parser->semantic) != 0) {
        return semantic_error(parser);
    }

    if (parser->pending_function_scope) {
        entered_function_scope = 1;
        parser->pending_function_scope = 0;
        compiler_semantic_begin_function(&parser->semantic, &parser->pending_function_type);

        for (i = 0; i < parser->pending_parameter_count; ++i) {
            if (compiler_semantic_declare(
                    &parser->semantic,
                    parser->pending_parameter_names[i],
                    COMPILER_SYMBOL_OBJECT,
                    &parser->pending_parameter_types[i],
                    1
                ) != 0) {
                status = semantic_error(parser);
                goto compound_exit;
            }
            if (emit_ir_status(parser, compiler_ir_emit_decl(&parser->ir, "param", 0, &parser->pending_parameter_types[i], parser->pending_parameter_names[i])) != 0) {
                status = -1;
                goto compound_exit;
            }
        }
        parser->pending_parameter_count = 0;
    }

    while (!current_is_punct(parser, "}") && parser->current.kind != COMPILER_TOKEN_EOF) {
        if (looks_like_declaration(parser)) {
            if (parse_declaration_or_function(parser, 0, 0) != 0) {
                status = -1;
                goto compound_exit;
            }
        } else if (parse_statement(parser) != 0) {
            status = -1;
            goto compound_exit;
        }
    }

    if (expect_punct(parser, "}") != 0) {
        status = -1;
        goto compound_exit;
    }

    if (entered_function_scope) {
        if (emit_ir_status(parser, compiler_ir_emit_function_end(&parser->ir, parser->pending_function_name)) != 0) {
            status = -1;
            goto compound_exit;
        }
        compiler_semantic_end_function(&parser->semantic);
        parser->pending_function_name[0] = '\0';
        entered_function_scope = 0;
    }

compound_exit:
    if (entered_function_scope) {
        compiler_semantic_end_function(&parser->semantic);
    }
    compiler_semantic_exit_scope(&parser->semantic);
    return status;
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
        if (rt_strcmp(builtin_types[i], "size_t") == 0 ||
            rt_strcmp(builtin_types[i], "ssize_t") == 0 ||
            rt_strcmp(builtin_types[i], "ptrdiff_t") == 0 ||
            rt_strcmp(builtin_types[i], "intptr_t") == 0 ||
            rt_strcmp(builtin_types[i], "uintptr_t") == 0 ||
            rt_strcmp(builtin_types[i], "off_t") == 0 ||
            rt_strcmp(builtin_types[i], "time_t") == 0 ||
            rt_strcmp(builtin_types[i], "usize") == 0) {
            type.scalar_bytes = 8U;
        } else if (rt_strcmp(builtin_types[i], "FILE") == 0 ||
                   rt_strcmp(builtin_types[i], "DIR") == 0) {
            type.base = COMPILER_BASE_STRUCT;
            type.scalar_bytes = 0U;
        } else {
            type.scalar_bytes = 4U;
        }
        (void)compiler_semantic_declare(&parser->semantic, builtin_types[i], COMPILER_SYMBOL_TYPEDEF, &type, 1);
    }

    for (i = 0; i < sizeof(builtin_objects) / sizeof(builtin_objects[0]); ++i) {
        CompilerType type;
        rt_memset(&type, 0, sizeof(type));
        (void)compiler_semantic_declare(&parser->semantic, builtin_objects[i], COMPILER_SYMBOL_OBJECT, &type, 0);
    }

    (void)advance(parser);
}
