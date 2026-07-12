#if !defined(SOLVE_FROM_SOLVE_C)
#ifndef SOLVE_PRIVATE_CONTEXT
#define SOLVE_PRIVATE_CONTEXT 1
#endif
#include "../solve.c"
#else
static int solve_parse_double(const char *text, size_t *index_io, double *value_out) {
    size_t index = *index_io;
    double value = 0.0;
    double place = 0.1;
    int negative = 0;
    int saw_digit = 0;
    int exponent = 0;
    int exponent_negative = 0;

    if (text[index] == '+' || text[index] == '-') {
        negative = text[index] == '-';
        index += 1U;
    }
    while (text[index] >= '0' && text[index] <= '9') {
        value = value * 10.0 + (double)(text[index] - '0');
        saw_digit = 1;
        index += 1U;
    }
    if (text[index] == '.') {
        index += 1U;
        while (text[index] >= '0' && text[index] <= '9') {
            value += (double)(text[index] - '0') * place;
            place *= 0.1;
            saw_digit = 1;
            index += 1U;
        }
    }
    if (!saw_digit) {
        return -1;
    }
    if (text[index] == 'e' || text[index] == 'E') {
        index += 1U;
        if (text[index] == '+' || text[index] == '-') {
            exponent_negative = text[index] == '-';
            index += 1U;
        }
        if (text[index] < '0' || text[index] > '9') {
            return -1;
        }
        while (text[index] >= '0' && text[index] <= '9') {
            if (exponent < 1000) {
                exponent = exponent * 10 + (text[index] - '0');
            }
            index += 1U;
        }
        while (exponent > 0) {
            if (exponent_negative) {
                value /= 10.0;
            } else {
                value *= 10.0;
            }
            exponent -= 1;
        }
    }
    *value_out = negative ? -value : value;
    *index_io = index;
    return 0;
}

static int solve_parse_double_arg(const char *text, double *value_out) {
    size_t index = 0U;
    double value;

    if (text == 0 || solve_parse_double(text, &index, &value) != 0 || text[index] != '\0') {
        return -1;
    }
    *value_out = value;
    return 0;
}

static void solve_skip_spaces(SolveExprParser *parser) {
    while (parser->text[parser->pos] == ' ' || parser->text[parser->pos] == '\t' || parser->text[parser->pos] == '\r' || parser->text[parser->pos] == '\n') {
        parser->pos += 1U;
    }
}

static void solve_set_expr_error(SolveExprParser *parser, const char *message) {
    if (!parser->error) {
        parser->error = 1;
        parser->message = message;
    }
}

static double solve_parse_expr(SolveExprParser *parser);

static int solve_read_identifier(SolveExprParser *parser, char *name, size_t name_size) {
    size_t used = 0U;

    if (!tool_ascii_is_identifier_start(parser->text[parser->pos])) {
        return -1;
    }
    while (tool_ascii_is_identifier_char(parser->text[parser->pos])) {
        if (used + 1U >= name_size) {
            solve_set_expr_error(parser, "identifier too long");
            return -1;
        }
        name[used++] = parser->text[parser->pos++];
    }
    name[used] = '\0';
    return 0;
}

static int solve_is_known_function(const char *name) {
    return rt_strcmp(name, "sqrt") == 0 || rt_strcmp(name, "q") == 0 ||
           rt_strcmp(name, "abs") == 0 ||
           rt_strcmp(name, "sin") == 0 || rt_strcmp(name, "s") == 0 ||
           rt_strcmp(name, "cos") == 0 || rt_strcmp(name, "c") == 0 ||
           rt_strcmp(name, "tan") == 0 || rt_strcmp(name, "t") == 0 ||
           rt_strcmp(name, "asin") == 0 || rt_strcmp(name, "acos") == 0 ||
           rt_strcmp(name, "sinh") == 0 || rt_strcmp(name, "cosh") == 0 || rt_strcmp(name, "tanh") == 0 ||
           rt_strcmp(name, "floor") == 0 || rt_strcmp(name, "ceil") == 0 || rt_strcmp(name, "round") == 0 ||
           rt_strcmp(name, "atan") == 0 || rt_strcmp(name, "a") == 0 ||
           rt_strcmp(name, "log") == 0 || rt_strcmp(name, "ln") == 0 || rt_strcmp(name, "l") == 0 ||
           rt_strcmp(name, "exp") == 0 || rt_strcmp(name, "e") == 0 ||
           rt_strcmp(name, "min") == 0 || rt_strcmp(name, "max") == 0;
}

static int solve_validate_identifiers(const char *expr, const SolveOptions *options, const char **message_out) {
    size_t pos = 0U;

    while (expr[pos] != '\0') {
        if (tool_ascii_is_identifier_start(expr[pos])) {
            char name[SOLVE_NAME_CAPACITY];
            size_t used = 0U;
            size_t lookahead;

            while (tool_ascii_is_identifier_char(expr[pos])) {
                if (used + 1U >= sizeof(name)) {
                    *message_out = "identifier too long";
                    return -1;
                }
                name[used++] = expr[pos++];
            }
            name[used] = '\0';
            if (rt_strcmp(name, options->var_name) == 0 || solve_is_param_name(options, name) || rt_strcmp(name, "pi") == 0 || rt_strcmp(name, "e") == 0) {
                continue;
            }
            lookahead = pos;
            while (expr[lookahead] == ' ' || expr[lookahead] == '\t' || expr[lookahead] == '\r' || expr[lookahead] == '\n') {
                lookahead += 1U;
            }
            if (expr[lookahead] == '(' && solve_is_known_function(name)) {
                continue;
            }
            *message_out = "unknown identifier";
            return -1;
        }
        pos += 1U;
    }
    return 0;
}

static double solve_call_function(SolveExprParser *parser, const char *name) {
    double first;
    double second = 0.0;
    int have_second = 0;

    solve_skip_spaces(parser);
    if (parser->text[parser->pos] != '(') {
        solve_set_expr_error(parser, "unknown identifier");
        return 0.0;
    }
    parser->pos += 1U;
    first = solve_parse_expr(parser);
    solve_skip_spaces(parser);
    if (parser->text[parser->pos] == ',') {
        parser->pos += 1U;
        have_second = 1;
        second = solve_parse_expr(parser);
        solve_skip_spaces(parser);
    }
    if (parser->text[parser->pos] != ')') {
        solve_set_expr_error(parser, "missing ')'");
        return 0.0;
    }
    parser->pos += 1U;

    if ((rt_strcmp(name, "sqrt") == 0 || rt_strcmp(name, "q") == 0) && !have_second) return solve_sqrt(first);
    if ((rt_strcmp(name, "abs") == 0) && !have_second) return solve_abs(first);
    if ((rt_strcmp(name, "sin") == 0 || rt_strcmp(name, "s") == 0) && !have_second) return solve_sin(first);
    if ((rt_strcmp(name, "cos") == 0 || rt_strcmp(name, "c") == 0) && !have_second) return solve_cos(first);
    if ((rt_strcmp(name, "tan") == 0 || rt_strcmp(name, "t") == 0) && !have_second) return solve_tan(first);
    if (rt_strcmp(name, "asin") == 0 && !have_second) return solve_asin(first);
    if (rt_strcmp(name, "acos") == 0 && !have_second) return solve_acos(first);
    if (rt_strcmp(name, "sinh") == 0 && !have_second) return solve_sinh(first);
    if (rt_strcmp(name, "cosh") == 0 && !have_second) return solve_cosh(first);
    if (rt_strcmp(name, "tanh") == 0 && !have_second) return solve_tanh(first);
    if (rt_strcmp(name, "floor") == 0 && !have_second) return solve_floor(first);
    if (rt_strcmp(name, "ceil") == 0 && !have_second) return solve_ceil(first);
    if (rt_strcmp(name, "round") == 0 && !have_second) return solve_round(first);
    if ((rt_strcmp(name, "atan") == 0 || rt_strcmp(name, "a") == 0) && !have_second) return solve_atan(first);
    if ((rt_strcmp(name, "log") == 0 || rt_strcmp(name, "ln") == 0 || rt_strcmp(name, "l") == 0) && !have_second) return solve_log(first);
    if ((rt_strcmp(name, "exp") == 0 || rt_strcmp(name, "e") == 0) && !have_second) return solve_exp(first);
    if (rt_strcmp(name, "min") == 0 && have_second) return first <= second ? first : second;
    if (rt_strcmp(name, "max") == 0 && have_second) return first >= second ? first : second;

    solve_set_expr_error(parser, have_second ? "unknown function" : "wrong function arity");
    return 0.0;
}

static double solve_parse_primary(SolveExprParser *parser) {
    char name[SOLVE_NAME_CAPACITY];
    double value;

    solve_skip_spaces(parser);
    if (parser->text[parser->pos] == '(') {
        parser->pos += 1U;
        value = solve_parse_expr(parser);
        solve_skip_spaces(parser);
        if (parser->text[parser->pos] != ')') {
            solve_set_expr_error(parser, "missing ')'");
            return 0.0;
        }
        parser->pos += 1U;
        return value;
    }
    if ((parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9') || parser->text[parser->pos] == '.') {
        if (solve_parse_double(parser->text, &parser->pos, &value) != 0) {
            solve_set_expr_error(parser, "invalid number");
            return 0.0;
        }
        return value;
    }
    if (tool_ascii_is_identifier_start(parser->text[parser->pos])) {
        if (solve_read_identifier(parser, name, sizeof(name)) != 0) {
            return 0.0;
        }
        if (rt_strcmp(name, parser->var_name) == 0) {
            return parser->var_value;
        }
        if (rt_strcmp(name, "pi") == 0) {
            return SOLVE_PI;
        }
        if (rt_strcmp(name, "e") == 0) {
            solve_skip_spaces(parser);
            if (parser->text[parser->pos] != '(') {
                return SOLVE_E;
            }
        }
        return solve_call_function(parser, name);
    }
    solve_set_expr_error(parser, "syntax error");
    return 0.0;
}

static double solve_parse_unary(SolveExprParser *parser) {
    solve_skip_spaces(parser);
    if (parser->text[parser->pos] == '+') {
        parser->pos += 1U;
        return solve_parse_unary(parser);
    }
    if (parser->text[parser->pos] == '-') {
        parser->pos += 1U;
        return -solve_parse_unary(parser);
    }
    return solve_parse_primary(parser);
}

static double solve_parse_power(SolveExprParser *parser) {
    double value = solve_parse_unary(parser);

    solve_skip_spaces(parser);
    if (parser->text[parser->pos] == '^') {
        double exponent;
        parser->pos += 1U;
        exponent = solve_parse_power(parser);
        value = solve_pow(value, exponent);
    }
    return value;
}

static double solve_parse_term(SolveExprParser *parser) {
    double value = solve_parse_power(parser);

    while (!parser->error) {
        char op;
        double right;
        solve_skip_spaces(parser);
        op = parser->text[parser->pos];
        if (op != '*' && op != '/' && op != '%') {
            break;
        }
        parser->pos += 1U;
        right = solve_parse_power(parser);
        if (op == '*') {
            value *= right;
        } else if (op == '/') {
            if (right == 0.0) {
                solve_set_expr_error(parser, "division by zero");
                return 0.0;
            }
            value /= right;
        } else {
            double quotient;
            long long truncated;
            if (right == 0.0) {
                solve_set_expr_error(parser, "division by zero");
                return 0.0;
            }
            quotient = value / right;
            truncated = quotient >= 0.0 ? (long long)quotient : -(long long)(-quotient);
            value = value - (double)truncated * right;
        }
    }
    return value;
}

static double solve_parse_expr(SolveExprParser *parser) {
    double value = solve_parse_term(parser);

    while (!parser->error) {
        char op;
        double right;
        solve_skip_spaces(parser);
        op = parser->text[parser->pos];
        if (op != '+' && op != '-') {
            break;
        }
        parser->pos += 1U;
        right = solve_parse_term(parser);
        if (op == '+') {
            value += right;
        } else {
            value -= right;
        }
    }
    return value;
}

static void solve_skip_text_spaces(const char *text, size_t *pos_io) {
    while (tool_ascii_is_space(text[*pos_io])) {
        *pos_io += 1U;
    }
}

static int solve_match_name_at(const char *text, size_t *pos_io, const char *name) {
    size_t length = rt_strlen(name);
    if (rt_strncmp(text + *pos_io, name, length) != 0 || tool_ascii_is_identifier_char(text[*pos_io + length])) {
        return 0;
    }
    *pos_io += length;
    return 1;
}

static int solve_eval_unary_function_fast(const char *name, double argument, double *value_out) {
    if (rt_strcmp(name, "sqrt") == 0 || rt_strcmp(name, "q") == 0) {
        *value_out = solve_sqrt(argument);
        return 0;
    }
    if (rt_strcmp(name, "abs") == 0) {
        *value_out = solve_abs(argument);
        return 0;
    }
    if (rt_strcmp(name, "sin") == 0 || rt_strcmp(name, "s") == 0) {
        *value_out = solve_sin(argument);
        return 0;
    }
    if (rt_strcmp(name, "cos") == 0 || rt_strcmp(name, "c") == 0) {
        *value_out = solve_cos(argument);
        return 0;
    }
    if (rt_strcmp(name, "tan") == 0 || rt_strcmp(name, "t") == 0) {
        *value_out = solve_tan(argument);
        return 0;
    }
    if (rt_strcmp(name, "asin") == 0) { *value_out = solve_asin(argument); return 0; }
    if (rt_strcmp(name, "acos") == 0) { *value_out = solve_acos(argument); return 0; }
    if (rt_strcmp(name, "sinh") == 0) { *value_out = solve_sinh(argument); return 0; }
    if (rt_strcmp(name, "cosh") == 0) { *value_out = solve_cosh(argument); return 0; }
    if (rt_strcmp(name, "tanh") == 0) { *value_out = solve_tanh(argument); return 0; }
    if (rt_strcmp(name, "floor") == 0) { *value_out = solve_floor(argument); return 0; }
    if (rt_strcmp(name, "ceil") == 0) { *value_out = solve_ceil(argument); return 0; }
    if (rt_strcmp(name, "round") == 0) { *value_out = solve_round(argument); return 0; }
    if (rt_strcmp(name, "atan") == 0 || rt_strcmp(name, "a") == 0) {
        *value_out = solve_atan(argument);
        return 0;
    }
    if (rt_strcmp(name, "log") == 0 || rt_strcmp(name, "ln") == 0 || rt_strcmp(name, "l") == 0) {
        *value_out = solve_log(argument);
        return 0;
    }
    if (rt_strcmp(name, "exp") == 0 || rt_strcmp(name, "e") == 0) {
        *value_out = solve_exp(argument);
        return 0;
    }
    return -1;
}

static int solve_eval_expr_fast(const char *expr, const char *var_name, double var_value, double *value_out) {
    char name[SOLVE_NAME_CAPACITY];
    size_t pos = 0U;
    size_t used = 0U;

    solve_skip_text_spaces(expr, &pos);
    if (!tool_ascii_is_identifier_start(expr[pos])) {
        return -1;
    }
    while (tool_ascii_is_identifier_char(expr[pos])) {
        if (used + 1U >= sizeof(name)) {
            return -1;
        }
        name[used++] = expr[pos++];
    }
    name[used] = '\0';
    solve_skip_text_spaces(expr, &pos);
    if (expr[pos] == '\0') {
        if (rt_strcmp(name, var_name) == 0) {
            *value_out = var_value;
            return 0;
        }
        if (rt_strcmp(name, "pi") == 0) {
            *value_out = SOLVE_PI;
            return 0;
        }
        if (rt_strcmp(name, "e") == 0) {
            *value_out = SOLVE_E;
            return 0;
        }
        return -1;
    }
    if (expr[pos] != '(') {
        return -1;
    }
    pos += 1U;
    solve_skip_text_spaces(expr, &pos);
    if (!solve_match_name_at(expr, &pos, var_name)) {
        return -1;
    }
    solve_skip_text_spaces(expr, &pos);
    if (expr[pos] != ')') {
        return -1;
    }
    pos += 1U;
    solve_skip_text_spaces(expr, &pos);
    if (expr[pos] != '\0') {
        return -1;
    }
    return solve_eval_unary_function_fast(name, var_value, value_out);
}

static int solve_eval_expr(const char *expr, const char *var_name, double var_value, double *value_out, const char **message_out) {
    SolveExprParser parser;
    double value;

    if (solve_eval_expr_fast(expr, var_name, var_value, value_out) == 0) {
        return solve_is_bad(*value_out) ? -1 : 0;
    }
    parser.text = expr;
    parser.pos = 0U;
    parser.var_name = var_name;
    parser.var_value = var_value;
    parser.error = 0;
    parser.message = 0;
    value = solve_parse_expr(&parser);
    solve_skip_spaces(&parser);
    if (!parser.error && parser.text[parser.pos] != '\0') {
        solve_set_expr_error(&parser, "syntax error");
    }
    if (parser.error || solve_is_bad(value)) {
        if (message_out != 0) {
            *message_out = parser.message != 0 ? parser.message : "numeric error";
        }
        return -1;
    }
    *value_out = value;
    return 0;
}

static int solve_substitute_bound_params(const char *expr, const SolveOptions *options, char *out, size_t out_size) {
    size_t in = 0U;
    size_t used = 0U;
    out[0] = '\0';
    while (expr[in] != '\0') {
        if (tool_ascii_is_identifier_start(expr[in])) {
            size_t start = in;
            char name[SOLVE_NAME_CAPACITY];
            size_t name_len;
            int param_index;
            while (tool_ascii_is_identifier_char(expr[in])) in += 1U;
            name_len = in - start;
            if (name_len >= sizeof(name)) return -1;
            if (solve_copy_range(name, sizeof(name), expr, start, in) != 0) return -1;
            param_index = solve_find_param_index(options, name);
            if (param_index >= 0 && options->param_has_value[param_index]) {
                char value[96];
                solve_format_double(options->param_values[param_index], SOLVE_MAX_SCALE, value, sizeof(value));
                if (options->param_values[param_index] < 0.0 && solve_append_char(out, out_size, &used, '(') != 0) return -1;
                if (solve_append_text(out, out_size, &used, value) != 0) return -1;
                if (options->param_values[param_index] < 0.0 && solve_append_char(out, out_size, &used, ')') != 0) return -1;
            } else {
                while (start < in) {
                    if (solve_append_char(out, out_size, &used, expr[start++]) != 0) return -1;
                }
            }
        } else {
            if (solve_append_char(out, out_size, &used, expr[in++]) != 0) return -1;
        }
    }
    return 0;
}

static int solve_eval_options_expr(const char *expr, const SolveOptions *options, double var_value, double *value_out, const char **message_out) {
    char substituted[SOLVE_EXPR_CAPACITY];
    if (solve_substitute_bound_params(expr, options, substituted, sizeof(substituted)) == 0) {
        return solve_eval_expr(substituted, options->var_name, var_value, value_out, message_out);
    }
    return solve_eval_expr(expr, options->var_name, var_value, value_out, message_out);
}

static int solve_eval_function(const SolveEquation *equation, const SolveOptions *options, double x, double *value_out, const char **message_out) {
    double left;
    double right = 0.0;

    if (solve_eval_options_expr(equation->left, options, x, &left, message_out) != 0) {
        return -1;
    }
    if (equation->has_equation) {
        if (solve_eval_options_expr(equation->right, options, x, &right, message_out) != 0) {
            return -1;
        }
    }
    *value_out = left - right;
    return solve_is_bad(*value_out) ? -1 : 0;
}

static int solve_eval_y(const SolveEquation *equation, const SolveOptions *options, double x, double *value_out) {
    const char *message = 0;
    (void)equation;
    return solve_eval_options_expr(equation->left, options, x, value_out, &message);
}


#endif
