#if !defined(SOLVE_FROM_SOLVE_C)
#ifndef SOLVE_PRIVATE_CONTEXT
#define SOLVE_PRIVATE_CONTEXT 1
#endif
#include "../solve.c"
#else
#define rt_write_cstr solve_sp_cstr
#define rt_write_line solve_sp_line
#define rt_write_char solve_sp_char
#define rt_write_uint solve_sp_uint
static int solve_run_diff_mode(const SolveEquation *equation, const SolveOptions *options) {
    SolveRatPoly poly;
    SolveRatPoly derivative;
    char text[SOLVE_EXPR_CAPACITY];
    SolveEquation derived;
    const char *source_text;

    if (equation->has_equation && equation->relation != SOLVE_RELATION_EQ) {
        tool_write_error("solve", "derivative solving supports equations, not inequalities", 0);
        return 2;
    }
    source_text = equation->has_equation ? equation->left : equation->left;
    if (equation->has_equation) {
        if (solve_equation_rat_poly(equation, options, &poly) != 0) {
            if (solve_symbolic_derivative_text(equation->left, options, options->diff_order, text, sizeof(text)) != 0) {
                tool_write_error("solve", "symbolic derivative unsupported for expression", 0);
                return 2;
            }
            if (solve_should_explain(options)) {
                solve_explain_working_function("symbolic derivative", equation, options);
                rt_write_line(1, "rule: symbolic sum, product, quotient, power, and chain rules");
                rt_write_cstr(1, "derivative: ");
                rt_write_line(1, text);
                rt_write_line(1, "next: solve derivative = 0");
            }
            rt_copy_string(derived.left, sizeof(derived.left), text);
            rt_copy_string(derived.right, sizeof(derived.right), "0");
            derived.has_equation = 1;
            derived.relation = SOLVE_RELATION_EQ;
            return solve_run_solver_equation(&derived, options);
        }
    } else if (solve_parse_rat_text(equation->left, options->var_name, &poly) != 0) {
        if (solve_symbolic_derivative_text(source_text, options, options->diff_order, text, sizeof(text)) != 0) {
            tool_write_error("solve", "symbolic derivative unsupported for expression", 0);
            return 2;
        }
        if (solve_should_explain(options)) {
            solve_explain_working_function("symbolic derivative", equation, options);
            rt_write_line(1, "rule: symbolic sum, product, quotient, power, and chain rules");
            rt_write_cstr(1, "derivative: ");
            rt_write_line(1, text);
        }
        if (tool_json_is_enabled()) solve_emit_kv("derivative", text);
        else rt_write_line(1, text);
        return 0;
    }
    if (solve_rat_poly_derivative(&poly, options->diff_order, &derivative) != 0 || solve_rat_poly_format(&derivative, options->var_name, text, sizeof(text)) != 0) {
        tool_write_error("solve", "derivative overflow", 0);
        return 3;
    }
    if (solve_should_explain(options)) {
        solve_explain_working_function("derivative", equation, options);
        solve_explain_rat_poly_line("polynomial: ", &poly, options);
        rt_write_cstr(1, "order: ");
        rt_write_uint(1, (unsigned long long)options->diff_order);
        rt_write_char(1, '\n');
        rt_write_line(1, "rule: d/dx a*x^n = a*n*x^(n-1)");
        rt_write_cstr(1, "derivative: ");
        rt_write_line(1, text);
        if (equation->has_equation) rt_write_line(1, "next: solve derivative = 0");
    }
    if (!equation->has_equation) {
        if (tool_json_is_enabled()) solve_emit_kv("derivative", text);
        else rt_write_line(1, text);
        return 0;
    }
    rt_copy_string(derived.left, sizeof(derived.left), text);
    rt_copy_string(derived.right, sizeof(derived.right), "0");
    derived.has_equation = 1;
    derived.relation = SOLVE_RELATION_EQ;
    return solve_run_solver_equation(&derived, options);
}

static int solve_split_integral_bounds(const char *text, char *left, size_t left_size, char *right, size_t right_size) {
    size_t index;
    int depth = 0;

    for (index = 0U; text[index] != '\0'; ++index) {
        if (text[index] == '(') depth += 1;
        else if (text[index] == ')' && depth > 0) depth -= 1;
        else if (text[index] == ':' && depth == 0) {
            return solve_copy_range(left, left_size, text, 0U, index) == 0 && solve_copy_range(right, right_size, text, index + 1U, rt_strlen(text)) == 0 ? 0 : -1;
        }
    }
    return -1;
}

static int solve_eval_bound_expr(const char *text, const SolveOptions *options, double *out) {
    const char *message = 0;
    return solve_eval_expr(text, options->var_name, 0.0, out, &message);
}

static int solve_parse_assignment_spec(const char *text, char *name, size_t name_size, char *value, size_t value_size) {
    size_t pos = 0U;
    size_t start;
    size_t end;
    solve_skip_text_spaces(text, &pos);
    start = pos;
    if (!tool_ascii_is_identifier_start(text[pos])) return -1;
    while (tool_ascii_is_identifier_char(text[pos])) pos += 1U;
    end = pos;
    solve_skip_text_spaces(text, &pos);
    if (text[pos] != '=') return -1;
    if (solve_copy_range(name, name_size, text, start, end) != 0) return -1;
    pos += 1U;
    solve_skip_text_spaces(text, &pos);
    if (text[pos] == '\0' || rt_strlen(text + pos) >= value_size) return -1;
    rt_copy_string(value, value_size, text + pos);
    return 0;
}

static int solve_eval_expr_at(const char *expr, const SolveOptions *options, double at, double *out) {
    const char *message = 0;
    return solve_eval_expr(expr, options->var_name, at, out, &message);
}

static int solve_run_eval_mode(const SolveOptions *options, const char *expr) {
    SolveOptions local = *options;
    char name[SOLVE_NAME_CAPACITY];
    char value_text[128];
    double at = 0.0;
    double value;
    char text[96];

    if (options->have_at) {
        if (solve_contains_char(options->at_spec, '=')) {
            if (solve_parse_assignment_spec(options->at_spec, name, sizeof(name), value_text, sizeof(value_text)) != 0) {
                tool_write_error("solve", "invalid --at assignment", options->at_spec);
                return 2;
            }
            if (rt_strcmp(name, options->var_name) != 0) {
                tool_write_error("solve", "--at variable does not match --var", name);
                return 2;
            }
            if (solve_eval_bound_expr(value_text, options, &at) != 0) {
                tool_write_error("solve", "invalid --at value", value_text);
                return 2;
            }
        } else if (solve_eval_bound_expr(options->at_spec, options, &at) != 0) {
            tool_write_error("solve", "invalid --at value", options->at_spec);
            return 2;
        }
    }
    if (solve_eval_expr_at(expr, &local, at, &value) != 0) {
        tool_write_error("solve", "evaluation failed", 0);
        return 3;
    }
    if (solve_should_explain(options)) {
        rt_write_line(1, "explain: evaluation");
        rt_write_cstr(1, "expression: ");
        rt_write_line(1, expr);
        if (options->have_at) {
            rt_write_cstr(1, "substitution: ");
            rt_write_cstr(1, options->var_name);
            rt_write_cstr(1, " = ");
            solve_write_double_value(at, options->scale);
            rt_write_char(1, '\n');
        }
    }
    solve_format_double(value, options->scale, text, sizeof(text));
    solve_emit_kv("value", text);
    solve_emit_kv("method", "direct-evaluation");
    if (!options->quiet) solve_emit_kv("status", "approximate");
    return 0;
}

static int solve_identifier_substitute(const char *expr, const char *old_name, const char *new_text, char *out, size_t out_size) {
    size_t in = 0U;
    size_t used = 0U;
    size_t old_len = rt_strlen(old_name);
    out[0] = '\0';
    while (expr[in] != '\0') {
        if (tool_ascii_is_identifier_start(expr[in])) {
            size_t start = in;
            while (tool_ascii_is_identifier_char(expr[in])) in += 1U;
            if (in - start == old_len && rt_strncmp(expr + start, old_name, old_len) == 0) {
                if (solve_append_text(out, out_size, &used, new_text) != 0) return -1;
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

static int solve_run_subst_mode(const SolveOptions *options, const char *expr) {
    char name[SOLVE_NAME_CAPACITY];
    char replacement[SOLVE_EXPR_CAPACITY];
    char out[SOLVE_EXPR_CAPACITY];
    if (solve_parse_assignment_spec(options->subst_spec, name, sizeof(name), replacement, sizeof(replacement)) != 0) {
        tool_write_error("solve", "invalid --subst assignment", options->subst_spec);
        return 2;
    }
    if (solve_identifier_substitute(expr, name, replacement, out, sizeof(out)) != 0) {
        tool_write_error("solve", "substitution output too large", 0);
        return 2;
    }
    if (tool_json_is_enabled()) {
        solve_emit_kv("expression", out);
    } else {
        rt_write_line(1, out);
    }
    return 0;
}

static int solve_run_average_rate_mode(const SolveEquation *equation, const SolveOptions *options) {
    char lo_text[128];
    char hi_text[128];
    double lo;
    double hi;
    double y_lo;
    double y_hi;
    double rate;
    char text[96];
    const char *message = 0;
    if (solve_split_integral_bounds(options->range_spec, lo_text, sizeof(lo_text), hi_text, sizeof(hi_text)) != 0) return 2;
    if (solve_eval_bound_expr(lo_text, options, &lo) != 0 || solve_eval_bound_expr(hi_text, options, &hi) != 0 || hi == lo) return 2;
    if (solve_eval_function(equation, options, lo, &y_lo, &message) != 0 || solve_eval_function(equation, options, hi, &y_hi, &message) != 0) return 3;
    rate = (y_hi - y_lo) / (hi - lo);
    if (solve_should_explain(options)) {
        solve_explain_working_function("average rate", equation, options);
        solve_explain_double_value_line("f(a) = ", y_lo, options);
        solve_explain_double_value_line("f(b) = ", y_hi, options);
        rt_write_line(1, "formula: average rate = (f(b)-f(a))/(b-a)");
    }
    solve_format_double(rate, options->scale, text, sizeof(text));
    solve_emit_kv("average rate approximate", text);
    solve_emit_kv("method", "endpoint-evaluation");
    solve_emit_kv("status", "approximate");
    return 0;
}

static int solve_simpson_eval(const SolveEquation *equation, const SolveOptions *options, double a, double b, int n, double *out) {
    double h = (b - a) / (double)n;
    double sum = 0.0;
    int i;
    const char *message = 0;

    if ((n % 2) != 0 || n <= 0) return -1;
    for (i = 0; i <= n; ++i) {
        double x = a + h * (double)i;
        double y;
        if (i == n) x = b;
        if (solve_eval_function(equation, options, x, &y, &message) != 0 || solve_is_bad(y)) return -1;
        if (i == 0 || i == n) sum += y;
        else sum += (i % 2 == 0 ? 2.0 : 4.0) * y;
    }
    *out = sum * h / 3.0;
    return solve_is_bad(*out) ? -1 : 0;
}

static int solve_simpson_square_eval(const SolveEquation *equation, const SolveOptions *options, double a, double b, int n, double *out) {
    double h = (b - a) / (double)n;
    double sum = 0.0;
    int i;
    const char *message = 0;

    if ((n % 2) != 0 || n <= 0) return -1;
    for (i = 0; i <= n; ++i) {
        double x = a + h * (double)i;
        double y;
        if (i == n) x = b;
        if (solve_eval_function(equation, options, x, &y, &message) != 0 || solve_is_bad(y)) return -1;
        y *= y;
        if (solve_is_bad(y)) return -1;
        if (i == 0 || i == n) sum += y;
        else sum += (i % 2 == 0 ? 2.0 : 4.0) * y;
    }
    *out = sum * h / 3.0;
    return solve_is_bad(*out) ? -1 : 0;
}

static void solve_numeric_analysis_bounds(const SolveOptions *options, double *lo_out, double *hi_out) {
    *lo_out = options->default_scan ? -10.0 : options->scan_lo;
    *hi_out = options->default_scan ? 10.0 : options->scan_hi;
}

static int solve_run_integrate_mode(const SolveEquation *equation, const SolveOptions *options) {
    char lo_text[128];
    char hi_text[128];
    SolveRatPoly poly;
    SolveRat lo_rat;
    SolveRat hi_rat;
    double lo;
    double hi;

    if (solve_split_integral_bounds(options->integrate_spec, lo_text, sizeof(lo_text), hi_text, sizeof(hi_text)) != 0) {
        tool_write_error("solve", "invalid --integrate bounds", options->integrate_spec);
        return 2;
    }
    if (solve_eval_bound_expr(lo_text, options, &lo) != 0 || solve_eval_bound_expr(hi_text, options, &hi) != 0) {
        tool_write_error("solve", "invalid integration bound", options->integrate_spec);
        return 2;
    }

    if ((equation->has_equation ? solve_equation_rat_poly(equation, options, &poly) : solve_parse_rat_text(equation->left, options->var_name, &poly)) == 0 &&
        solve_rat_poly_parse_bound(lo_text, options->var_name, &lo_rat) == 0 && solve_rat_poly_parse_bound(hi_text, options->var_name, &hi_rat) == 0) {
        SolveRat hi_value;
        SolveRat lo_value;
        SolveRat result;
        char text[96];
        if (solve_rat_poly_antiderivative_eval(&poly, hi_rat, &hi_value) != 0 || solve_rat_poly_antiderivative_eval(&poly, lo_rat, &lo_value) != 0 || solve_rat_sub(hi_value, lo_value, &result) != 0 || solve_rat_format(result, text, sizeof(text)) != 0) {
            if (solve_should_explain(options)) rt_write_line(1, "status reason: exact rational integration overflowed; falling back to numeric Simpson integration");
        } else {
            if (solve_should_explain(options)) {
                SolveRatPoly anti;
                char anti_text[SOLVE_EXPR_CAPACITY];
                solve_explain_working_function("definite integral", equation, options);
                solve_explain_rat_poly_line("integrand polynomial: ", &poly, options);
                rt_write_cstr(1, "bounds: ");
                solve_write_rat_value(lo_rat);
                rt_write_cstr(1, " to ");
                solve_write_rat_value(hi_rat);
                rt_write_char(1, '\n');
                if (solve_rat_poly_antiderivative(&poly, &anti) == 0 && solve_rat_poly_format_antiderivative(&anti, options->var_name, anti_text, sizeof(anti_text)) == 0) {
                    rt_write_cstr(1, "antiderivative: ");
                    rt_write_line(1, anti_text);
                }
                solve_explain_rat_value_line("F(upper) = ", hi_value, options);
                solve_explain_rat_value_line("F(lower) = ", lo_value, options);
                rt_write_line(1, "rule: integral from a to b = F(b) - F(a)");
            }
            if (options->quiet && !tool_json_is_enabled()) {
                rt_write_line(1, text);
            } else {
                solve_emit_kv("integral", text);
                if (!options->quiet) solve_emit_kv("method", "exact-polynomial");
            }
            return 0;
        }
    }

    {
        double coarse;
        double fine;
        double error;
        char value[96];
        if (solve_simpson_eval(equation, options, lo, hi, 1000, &coarse) != 0 || solve_simpson_eval(equation, options, lo, hi, 2000, &fine) != 0) {
            if (!options->quiet) solve_emit_kv("status", "improper integral over a discontinuity or invalid point");
            return 3;
        }
        error = solve_abs(fine - coarse) / 15.0;
        if (solve_should_explain(options)) {
            solve_explain_working_function("numeric definite integral", equation, options);
            solve_explain_double_value_line("lower bound = ", lo, options);
            solve_explain_double_value_line("upper bound = ", hi, options);
            rt_write_line(1, "method detail: composite Simpson rule with 1000 and 2000 subintervals");
            solve_explain_double_value_line("coarse estimate = ", coarse, options);
            solve_explain_double_value_line("fine estimate = ", fine, options);
            rt_write_line(1, "status reason: numeric integration uses sampled double values");
        }
        if (options->quiet && !tool_json_is_enabled()) {
            solve_format_double(fine, options->scale, value, sizeof(value));
            rt_write_line(1, value);
        } else {
            solve_format_double(fine, options->scale, value, sizeof(value));
            solve_emit_kv("integral", value);
            if (!options->quiet) {
                solve_format_double(error, options->scale, value, sizeof(value));
                solve_emit_kv("estimated error", value);
                solve_emit_kv("method", "simpson");
                solve_emit_kv("status", "approximate");
            }
        }
        return 0;
    }
}


#endif
