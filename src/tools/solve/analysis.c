#if !defined(SOLVE_FROM_SOLVE_C)
#ifndef SOLVE_PRIVATE_CONTEXT
#define SOLVE_PRIVATE_CONTEXT 1
#endif
#include "../solve.c"
#else
static void solve_print_exp_poly_factor_intervals(const char *positive_label, const char *negative_label, const SolveOptions *options, const SolveRatPoly *factor);
static void solve_write_point_double(double x, double y, int scale);
static void solve_write_numeric_end_behavior(const SolveEquation *equation, const SolveOptions *options);

static int solve_format_antiderivative_term(SolveRat coeff, int power, const char *var_name, char *buffer, size_t buffer_size, size_t *length_io, int first) {
    SolveRat abs_coeff;
    if (solve_rat_abs_value(coeff, &abs_coeff) != 0) return -1;
    if (first) {
        if (coeff.num < 0 && solve_append_text(buffer, buffer_size, length_io, "-") != 0) return -1;
    } else if (solve_append_text(buffer, buffer_size, length_io, coeff.num < 0 ? " - " : " + ") != 0) return -1;
    if (power == 0) {
        char text[96];
        if (solve_rat_format(abs_coeff, text, sizeof(text)) != 0) return -1;
        return solve_append_text(buffer, buffer_size, length_io, text);
    }
    if (abs_coeff.den != 1) {
        if (abs_coeff.num != 1 && solve_append_signed_ll(buffer, buffer_size, length_io, abs_coeff.num) != 0) return -1;
        if (abs_coeff.num != 1 && solve_append_char(buffer, buffer_size, length_io, '*') != 0) return -1;
        if (solve_append_text(buffer, buffer_size, length_io, var_name) != 0) return -1;
        if (power > 1 && (solve_append_char(buffer, buffer_size, length_io, '^') != 0 || solve_append_signed_ll(buffer, buffer_size, length_io, power) != 0)) return -1;
        if (solve_append_char(buffer, buffer_size, length_io, '/') != 0) return -1;
        return solve_append_signed_ll(buffer, buffer_size, length_io, abs_coeff.den);
    }
    if (abs_coeff.num != 1 && (solve_append_signed_ll(buffer, buffer_size, length_io, abs_coeff.num) != 0 || solve_append_char(buffer, buffer_size, length_io, '*') != 0)) return -1;
    if (solve_append_text(buffer, buffer_size, length_io, var_name) != 0) return -1;
    if (power > 1 && (solve_append_char(buffer, buffer_size, length_io, '^') != 0 || solve_append_signed_ll(buffer, buffer_size, length_io, power) != 0)) return -1;
    return 0;
}

static int solve_rat_poly_format_antiderivative(const SolveRatPoly *poly, const char *var_name, char *buffer, size_t buffer_size) {
    int degree = solve_rat_poly_degree(poly);
    size_t length = 0U;
    int first = 1;
    int i;
    if (buffer_size == 0U) return -1;
    buffer[0] = '\0';
    if (degree < 0) {
        if (solve_append_text(buffer, buffer_size, &length, "C") != 0) return -1;
        return 0;
    }
    for (i = degree; i >= 0; --i) {
        if (solve_rat_is_zero(poly->coeff[i])) continue;
        if (solve_format_antiderivative_term(poly->coeff[i], i, var_name, buffer, buffer_size, &length, first) != 0) return -1;
        first = 0;
    }
    if (!first) return solve_append_text(buffer, buffer_size, &length, " + C");
    return solve_append_text(buffer, buffer_size, &length, "C");
}

static int solve_extract_unary_call_arg(const char *expr, const char *name, char *arg, size_t arg_size) {
    size_t name_len = rt_strlen(name);
    size_t pos;
    int depth = 0;
    if (rt_strncmp(expr, name, name_len) != 0 || expr[name_len] != '(') return -1;
    for (pos = name_len; expr[pos] != '\0'; ++pos) {
        if (expr[pos] == '(') depth += 1;
        else if (expr[pos] == ')') {
            depth -= 1;
            if (depth == 0 && expr[pos + 1U] != '\0') return -1;
        }
    }
    if (depth != 0) return -1;
    return solve_copy_range(arg, arg_size, expr, name_len + 1U, rt_strlen(expr) - 1U);
}

static int solve_linear_slope(const char *expr, const SolveOptions *options, SolveRat *slope_out) {
    SolveRatPoly poly;
    if (solve_parse_rat_text(expr, options->var_name, &poly) != 0 || solve_rat_poly_degree(&poly) != 1 || solve_rat_is_zero(poly.coeff[1])) return -1;
    *slope_out = poly.coeff[1];
    return 0;
}

static int solve_append_divided_by_rat(char *buffer, size_t buffer_size, size_t *length_io, SolveRat slope) {
    SolveRat abs_slope;
    char text[96];
    if (slope.den == 1 && (slope.num == 1 || slope.num == -1)) return 0;
    if (solve_rat_abs_value(slope, &abs_slope) != 0 || solve_rat_format(abs_slope, text, sizeof(text)) != 0) return -1;
    if (solve_append_char(buffer, buffer_size, length_io, '/') != 0) return -1;
    return solve_append_text(buffer, buffer_size, length_io, text);
}

static int solve_format_symbolic_antiderivative(const char *expr, const SolveOptions *options, char *out, size_t out_size) {
    char arg[SOLVE_EXPR_CAPACITY];
    SolveRat slope;
    SolveRat abs_slope;
    size_t used = 0U;
    const char *outer = 0;
    int negative = 0;
    if (solve_extract_unary_call_arg(expr, "sin", arg, sizeof(arg)) == 0 && solve_linear_slope(arg, options, &slope) == 0) {
        outer = "cos";
        negative = slope.num > 0;
    } else if (solve_extract_unary_call_arg(expr, "cos", arg, sizeof(arg)) == 0 && solve_linear_slope(arg, options, &slope) == 0) {
        outer = "sin";
        negative = slope.num < 0;
    } else if (solve_extract_unary_call_arg(expr, "exp", arg, sizeof(arg)) == 0 && solve_linear_slope(arg, options, &slope) == 0) {
        outer = "exp";
        negative = slope.num < 0;
    } else if (rt_strcmp(expr, "1/x") == 0 || rt_strcmp(expr, "1 / x") == 0) {
        if (rt_strlen("log(abs(x)) + C") >= out_size) return -1;
        rt_copy_string(out, out_size, "log(abs(x)) + C");
        return 0;
    } else {
        return -1;
    }
    out[0] = '\0';
    if (solve_rat_abs_value(slope, &abs_slope) != 0) return -1;
    if (negative && solve_append_char(out, out_size, &used, '-') != 0) return -1;
    if (abs_slope.den != 1) {
        char text[96];
        SolveRat reciprocal;
        if (solve_rat_make_i128(abs_slope.den, abs_slope.num, &reciprocal) != 0 || solve_rat_format(reciprocal, text, sizeof(text)) != 0) return -1;
        if (!(reciprocal.num == 1 && reciprocal.den == 1)) {
            if (solve_append_text(out, out_size, &used, text) != 0 || solve_append_char(out, out_size, &used, '*') != 0) return -1;
        }
    }
    if (solve_append_text(out, out_size, &used, outer) != 0 || solve_append_char(out, out_size, &used, '(') != 0 || solve_append_text(out, out_size, &used, arg) != 0 || solve_append_char(out, out_size, &used, ')') != 0) return -1;
    if (abs_slope.den == 1 && solve_append_divided_by_rat(out, out_size, &used, slope) != 0) return -1;
    return solve_append_text(out, out_size, &used, " + C");
}

static int solve_format_fraction_hint(double value, char *buffer, size_t buffer_size) {
    double absolute = math_abs(value);
    unsigned int den;
    unsigned long long best_num = 0ULL;
    unsigned long long best_den = 1ULL;
    double best_error = 1.0;
    size_t length = 0U;
    if (buffer_size == 0U || solve_is_bad(value) || absolute > 1000000.0) return -1;
    for (den = 1U; den <= SOLVE_MAX_RATIONAL_DENOMINATOR; ++den) {
        double scaled = absolute * (double)den;
        unsigned long long num = (unsigned long long)(scaled + 0.5);
        double candidate = (double)num / (double)den;
        double error = solve_abs(candidate - absolute);
        if (error < best_error) {
            best_error = error;
            best_num = num;
            best_den = (unsigned long long)den;
        }
    }
    if (best_error > 0.0000001) return -1;
    {
        unsigned long long gcd = solve_gcd_ull(best_num, best_den);
        char text[64];
        if (gcd != 0ULL) {
            best_num /= gcd;
            best_den /= gcd;
        }
        buffer[0] = '\0';
        if (value < 0.0 && solve_append_char(buffer, buffer_size, &length, '-') != 0) return -1;
        solve_format_double((double)best_num, 0, text, sizeof(text));
        if (solve_append_text(buffer, buffer_size, &length, text) != 0) return -1;
        if (best_den == 1ULL) return 0;
        if (solve_append_char(buffer, buffer_size, &length, '/') != 0) return -1;
        solve_format_double((double)best_den, 0, text, sizeof(text));
        return solve_append_text(buffer, buffer_size, &length, text);
    }
}

static int solve_run_antiderivative_mode(const SolveEquation *equation, const SolveOptions *options) {
    SolveRatPoly poly;
    SolveRatPoly anti;
    char text[SOLVE_EXPR_CAPACITY];
    if (solve_get_rat_poly_for_mode(equation, options, &poly) != 0) {
        if (!equation->has_equation && solve_format_symbolic_antiderivative(equation->left, options, text, sizeof(text)) == 0) {
            if (solve_should_explain(options)) {
                solve_explain_working_function("symbolic antiderivative", equation, options);
                solve_sp_line(1, "rule: narrow school-function table for sin(linear), cos(linear), exp(linear), and 1/x");
                solve_sp_cstr(1, "antiderivative: ");
                solve_sp_line(1, text);
            }
            solve_sp_cstr(1, "F(");
            solve_sp_cstr(1, options->var_name);
            solve_sp_cstr(1, ") = ");
            solve_sp_line(1, text);
            solve_sp_line(1, "method = symbolic-table");
            return 0;
        }
        tool_write_error("solve", "antiderivative supported only for polynomials and simple school functions", 0);
        return 2;
    }
    if (solve_rat_poly_antiderivative(&poly, &anti) != 0 || solve_rat_poly_format_antiderivative(&anti, options->var_name, text, sizeof(text)) != 0) {
        tool_write_error("solve", "exact integration overflow", 0);
        return 3;
    }
    if (solve_should_explain(options)) {
        char poly_text[SOLVE_EXPR_CAPACITY];
        solve_explain_working_function("antiderivative", equation, options);
        if (solve_rat_poly_format(&poly, options->var_name, poly_text, sizeof(poly_text)) == 0) {
            solve_sp_cstr(1, "polynomial: ");
            solve_sp_line(1, poly_text);
        }
        solve_sp_line(1, "rule: integral a*x^n dx = a*x^(n+1)/(n+1)");
        solve_sp_cstr(1, "exact antiderivative: ");
        solve_sp_line(1, text);
    }
    solve_sp_cstr(1, "F(");
    solve_sp_cstr(1, options->var_name);
    solve_sp_cstr(1, ") = ");
    solve_sp_line(1, text);
    solve_sp_line(1, "method = exact-polynomial");
    return 0;
}

static int solve_run_monotonicity_mode(const SolveEquation *equation, const SolveOptions *options, int curvature) {
    SolveRatPoly poly;
    SolveRatPoly derivative;
    if (solve_get_rat_poly_for_mode(equation, options, &poly) != 0) {
        SolveRatPoly exp_factor;
        SolveRatPoly exp_arg;
        if (!equation->has_equation && solve_sym_exp_poly_derivative_factor(equation->left, options, curvature ? 2 : 1, &exp_factor, &exp_arg) == 0) {
            if (solve_should_explain(options)) {
                solve_explain_working_function(curvature ? "exp-polynomial curvature" : "exp-polynomial monotonicity", equation, options);
                solve_explain_rat_poly_line(curvature ? "second-derivative factor: " : "first-derivative factor: ", &exp_factor, options);
                solve_sp_line(1, "rule: exp(linear) is always positive, so this exact polynomial factor decides the sign");
            }
            solve_print_exp_poly_factor_intervals(curvature ? "left-curved" : "increasing", curvature ? "right-curved" : "decreasing", options, &exp_factor);
            solve_sp_line(1, "method = exact-exp-polynomial-sign");
            return 0;
        }
        double roots[SOLVE_MAX_RESULTS];
        int count = 0;
        double lo = options->default_scan ? -10.0 : options->scan_lo;
        double hi = options->default_scan ? 10.0 : options->scan_hi;
        int i;
        if (solve_should_explain(options)) {
            solve_explain_working_function(curvature ? "numeric curvature" : "numeric monotonicity", equation, options);
            solve_explain_scan_window_line(options);
            solve_sp_line(1, curvature ? "method detail: finite-difference f'' sign changes split curvature intervals" : "method detail: finite-difference f' sign changes split monotonicity intervals");
            solve_sp_line(1, "status reason: derivative signs are sampled numerically");
        }
        solve_numeric_derivative_roots(equation, options, curvature ? 2 : 1, roots, &count);
        for (i = 0; i <= count; ++i) {
            double left = i == 0 ? lo : roots[i - 1];
            double right = i == count ? hi : roots[i];
            double sample = (left + right) * 0.5;
            int ok;
            double value = solve_numeric_derivative_value(equation, options, sample, curvature ? 2 : 1, &ok);
            if (!ok || right <= left) continue;
            solve_sp_cstr(1, value >= 0.0 ? (curvature ? "left-curved" : "increasing") : (curvature ? "right-curved" : "decreasing"));
            solve_sp_cstr(1, " (within scan range) = (");
            solve_write_double_value(left, options->scale);
            solve_sp_cstr(1, ", ");
            solve_write_double_value(right, options->scale);
            solve_sp_line(1, ")");
        }
        solve_sp_line(1, "status = approximate");
        return 0;
    }
    if (solve_rat_poly_derivative(&poly, curvature ? 2 : 1, &derivative) != 0) return 3;
    if (solve_should_explain(options)) {
        solve_explain_working_function(curvature ? "curvature" : "monotonicity", equation, options);
        solve_explain_rat_poly_line("polynomial: ", &poly, options);
        solve_explain_rat_poly_line(curvature ? "second derivative: " : "first derivative: ", &derivative, options);
        solve_sp_line(1, curvature ? "rule: f'' > 0 means left-curved; f'' < 0 means right-curved" : "rule: f' > 0 means increasing; f' < 0 means decreasing");
        solve_sp_line(1, "method detail: exact derivative roots split the real line; exact signs decide intervals");
    }
    solve_print_labeled_intervals(curvature ? "left-curved" : "increasing", curvature ? "right-curved" : "decreasing", options, &derivative);
    solve_sp_line(1, "method = exact-polynomial");
    return 0;
}

static double solve_rat_poly_eval_double_direct(const SolveRatPoly *poly, double x) {
    int degree = solve_rat_poly_degree(poly);
    double value = 0.0;
    while (degree >= 0) {
        value = value * x + solve_rat_to_double(poly->coeff[degree]);
        degree -= 1;
    }
    return value;
}

static void solve_print_exp_poly_factor_intervals(const char *positive_label, const char *negative_label, const SolveOptions *options, const SolveRatPoly *factor) {
    SolveBreakpoint points[SOLVE_MAX_RESULTS];
    int point_count = 0;
    int segment;
    if (solve_collect_rat_poly_roots(factor, points, &point_count) != 0) return;
    solve_sort_breakpoints(points, &point_count, options->tolerance);
    if (solve_rat_poly_degree(factor) < 0) {
        solve_sp_line(1, "constant zero");
        return;
    }
    for (segment = 0; segment <= point_count; ++segment) {
        double sample;
        double value;
        if (point_count == 0) sample = 0.0;
        else if (segment == 0) sample = points[0].value - 1.0;
        else if (segment == point_count) sample = points[point_count - 1].value + 1.0;
        else sample = (points[segment - 1].value + points[segment].value) * 0.5;
        value = solve_rat_poly_eval_double_direct(factor, sample);
        if (solve_abs(value) <= options->tolerance) continue;
        solve_sp_cstr(1, value > 0.0 ? positive_label : negative_label);
        solve_sp_cstr(1, " = (");
        if (segment == 0) solve_sp_cstr(1, "-inf");
        else solve_sp_cstr(1, points[segment - 1].label);
        solve_sp_cstr(1, ", ");
        if (segment == point_count) solve_sp_cstr(1, "inf");
        else solve_sp_cstr(1, points[segment].label);
        solve_sp_line(1, ")");
        if (point_count == 0) break;
    }
}

static const char *solve_classify_exp_poly_critical(const SolveRatPoly *first, const SolveRatPoly *second, double x, const SolveOptions *options) {
    double second_value = solve_rat_poly_eval_double_direct(second, x);
    if (second_value > options->tolerance) return "minimum";
    if (second_value < -options->tolerance) return "maximum";
    {
        double left = solve_rat_poly_eval_double_direct(first, x - 0.01);
        double right = solve_rat_poly_eval_double_direct(first, x + 0.01);
        if (left > 0.0 && right < 0.0) return "maximum";
        if (left < 0.0 && right > 0.0) return "minimum";
    }
    return "saddle";
}

static int solve_print_exp_poly_discussion(const SolveEquation *equation, const SolveOptions *options) {
    SolveRatPoly zero_factor;
    SolveRatPoly first;
    SolveRatPoly second;
    SolveRatPoly exp_arg;
    SolveBreakpoint zeros[SOLVE_MAX_RESULTS];
    SolveBreakpoint critical[SOLVE_MAX_RESULTS];
    SolveBreakpoint inflections[SOLVE_MAX_RESULTS];
    int zero_count = 0;
    int critical_count = 0;
    int inflection_count = 0;
    int i;
    if (equation->has_equation || solve_sym_exp_poly_derivative_factor(equation->left, options, 0, &zero_factor, &exp_arg) != 0 || solve_sym_exp_poly_derivative_factor(equation->left, options, 1, &first, &exp_arg) != 0 || solve_sym_exp_poly_derivative_factor(equation->left, options, 2, &second, &exp_arg) != 0) return -1;
    if (solve_should_explain(options)) {
        solve_explain_working_function("exp-polynomial curve discussion", equation, options);
        solve_explain_rat_poly_line("zero factor: ", &zero_factor, options);
        solve_explain_rat_poly_line("first-derivative factor: ", &first, options);
        solve_explain_rat_poly_line("second-derivative factor: ", &second, options);
        solve_sp_line(1, "rule: the exponential factor is positive, so exact polynomial factors decide zeros, critical points, and curvature changes");
    }
    solve_sp_line(1, "domain: all real x");
    solve_sp_line(1, "structure: exp(linear)*polynomial");
    (void)solve_collect_rat_poly_roots(&zero_factor, zeros, &zero_count);
    solve_sort_breakpoints(zeros, &zero_count, options->tolerance);
    solve_print_rat_roots_line("zeros:", zeros, zero_count);
    (void)solve_collect_rat_poly_roots(&first, critical, &critical_count);
    solve_sort_breakpoints(critical, &critical_count, options->tolerance);
    for (i = 0; i < critical_count; ++i) {
        double y;
        const char *message = 0;
        const char *label;
        if (solve_eval_function(equation, options, critical[i].value, &y, &message) != 0) continue;
        label = solve_classify_exp_poly_critical(&first, &second, critical[i].value, options);
        solve_sp_cstr(1, label);
        solve_sp_cstr(1, " approximate: ");
        solve_write_point_double(critical[i].value, y, options->scale);
        solve_sp_char(1, '\n');
    }
    (void)solve_collect_rat_poly_roots(&second, inflections, &inflection_count);
    solve_sort_breakpoints(inflections, &inflection_count, options->tolerance);
    for (i = 0; i < inflection_count; ++i) {
        double left = solve_rat_poly_eval_double_direct(&second, inflections[i].value - 0.01);
        double right = solve_rat_poly_eval_double_direct(&second, inflections[i].value + 0.01);
        double y;
        const char *message = 0;
        if (left * right > 0.0 || solve_eval_function(equation, options, inflections[i].value, &y, &message) != 0) continue;
        solve_sp_cstr(1, "inflection approximate: ");
        solve_write_point_double(inflections[i].value, y, options->scale);
        solve_sp_char(1, '\n');
    }
    solve_print_exp_poly_factor_intervals("increasing", "decreasing", options, &first);
    solve_print_exp_poly_factor_intervals("left-curved", "right-curved", options, &second);
    solve_write_numeric_end_behavior(equation, options);
    solve_sp_line(1, "method = exact-exp-polynomial-critical-points");
    solve_sp_line(1, "status = approximate-values");
    return 0;
}

static int solve_write_exact_line(const char *prefix, SolveRat slope, SolveRat intercept, const SolveOptions *options) {
    solve_sp_cstr(1, prefix);
    if (solve_rat_is_zero(slope)) {
        solve_sp_cstr(1, "y = ");
        solve_write_rat_value(intercept);
        solve_sp_char(1, '\n');
        return 0;
    }
    solve_sp_cstr(1, "y = ");
    if (slope.num < 0) solve_sp_cstr(1, "-");
    {
        SolveRat abs_slope;
        if (solve_rat_abs_value(slope, &abs_slope) != 0) return -1;
        if (!(abs_slope.num == 1 && abs_slope.den == 1)) {
            solve_write_rat_value(abs_slope);
            solve_sp_cstr(1, "*");
        }
    }
    solve_sp_cstr(1, options->var_name);
    if (!solve_rat_is_zero(intercept)) {
        SolveRat abs_intercept;
        if (solve_rat_abs_value(intercept, &abs_intercept) != 0) return -1;
        solve_sp_cstr(1, intercept.num < 0 ? " - " : " + ");
        solve_write_rat_value(abs_intercept);
    }
    solve_sp_char(1, '\n');
    return 0;
}

static int solve_run_tangent_normal_mode(const SolveEquation *equation, const SolveOptions *options, int normal) {
    SolveRatPoly poly;
    SolveRatPoly derivative;
    SolveRat x;
    SolveRat y;
    SolveRat slope;
    SolveRat intercept;
    SolveRat product;
    int degree;
    if (solve_get_rat_poly_for_mode(equation, options, &poly) != 0 || solve_rat_poly_parse_bound(options->point_spec, options->var_name, &x) != 0) {
        double point;
        double y_value;
        double slope_value;
        double intercept_value;
        int exact_exp_poly_slope = 0;
        int ok;
        const char *message = 0;
        if (solve_parse_double_arg(options->point_spec, &point) != 0 || solve_eval_function(equation, options, point, &y_value, &message) != 0) {
            tool_write_error("solve", "invalid tangent point", options->point_spec);
            return 2;
        }
        slope_value = solve_numeric_derivative_value(equation, options, point, 1, &ok);
        if (!ok) return 3;
        if (!equation->has_equation) {
            SolveRatPoly first_factor;
            SolveRatPoly exp_arg;
            if (solve_sym_exp_poly_derivative_factor(equation->left, options, 1, &first_factor, &exp_arg) == 0) {
                slope_value = solve_exp(solve_rat_poly_eval_double_direct(&exp_arg, point)) * solve_rat_poly_eval_double_direct(&first_factor, point);
                exact_exp_poly_slope = 1;
            }
        }
        if (normal) {
            if (solve_abs(slope_value) <= options->tolerance) {
                if (solve_should_explain(options)) {
                    solve_explain_working_function("numeric normal", equation, options);
                    solve_explain_double_value_line("point a = ", point, options);
                    solve_explain_double_value_line("f(a) = ", y_value, options);
                    solve_explain_double_value_line(exact_exp_poly_slope ? "exp-polynomial f'(a) = " : "numeric f'(a) = ", slope_value, options);
                    solve_sp_line(1, "rule: tangent slope is 0, so the normal line is vertical");
                }
                solve_sp_cstr(1, "normal approximate: x = ");
                solve_write_double_value(point, options->scale);
                solve_sp_char(1, '\n');
                solve_sp_line(1, "status = approximate");
                return 0;
            }
            slope_value = -1.0 / slope_value;
        }
        intercept_value = y_value - slope_value * point;
        if (solve_should_explain(options)) {
            solve_explain_working_function(normal ? "numeric normal" : "numeric tangent", equation, options);
            solve_explain_double_value_line("point a = ", point, options);
            solve_explain_double_value_line("f(a) = ", y_value, options);
            solve_explain_double_value_line(normal ? "normal slope = " : "tangent slope = ", slope_value, options);
            solve_sp_line(1, normal ? "rule: normal slope is -1/f'(a)" : "rule: tangent line is y - f(a) = f'(a)*(x - a)");
            solve_sp_line(1, exact_exp_poly_slope ? "status reason: slope uses an exact exp-polynomial derivative factor; values are numeric because exp is evaluated approximately" : "status reason: slope is computed by finite differences");
        }
        solve_sp_cstr(1, normal ? "normal approximate: y = " : "tangent approximate: y = ");
        solve_write_double_value(slope_value, options->scale);
        solve_sp_cstr(1, "*");
        solve_sp_cstr(1, options->var_name);
        if (intercept_value < 0.0) {
            solve_sp_cstr(1, " - ");
            solve_write_double_value(-intercept_value, options->scale);
        } else {
            solve_sp_cstr(1, " + ");
            solve_write_double_value(intercept_value, options->scale);
        }
        solve_sp_char(1, '\n');
        if (exact_exp_poly_slope) solve_sp_line(1, "method = exact-exp-polynomial-derivative-factor");
        solve_sp_line(1, "status = approximate");
        return 0;
    }
    degree = solve_rat_poly_degree(&poly);
    if (solve_rat_poly_eval(&poly, degree, x, &y) != 0 || solve_rat_poly_derivative(&poly, 1, &derivative) != 0 || solve_rat_poly_eval(&derivative, solve_rat_poly_degree(&derivative), x, &slope) != 0) return 3;
    if (solve_should_explain(options)) {
        solve_explain_working_function(normal ? "normal" : "tangent", equation, options);
        solve_explain_rat_poly_line("polynomial: ", &poly, options);
        solve_explain_rat_poly_line("first derivative: ", &derivative, options);
        solve_explain_rat_value_line("point a = ", x, options);
        solve_explain_rat_value_line("f(a) = ", y, options);
        solve_explain_rat_value_line("f'(a) = ", slope, options);
    }
    if (normal) {
        if (solve_rat_is_zero(slope)) {
            if (solve_should_explain(options)) solve_sp_line(1, "rule: tangent slope is 0, so the normal line is vertical");
            solve_sp_cstr(1, "normal: x = ");
            solve_write_rat_value(x);
            solve_sp_char(1, '\n');
            solve_sp_line(1, "method = exact-polynomial");
            return 0;
        }
        if (solve_rat_make(-slope.den, slope.num, &slope) != 0) return 3;
        solve_explain_rat_value_line("normal slope = -1/f'(a) = ", slope, options);
    }
    if (solve_rat_mul(slope, x, &product) != 0 || solve_rat_sub(y, product, &intercept) != 0) return 3;
    if (solve_should_explain(options)) {
        solve_sp_line(1, normal ? "rule: normal line is y - f(a) = m_normal*(x - a)" : "rule: tangent line is y - f(a) = f'(a)*(x - a)");
        solve_explain_rat_value_line("intercept f(a) - m*a = ", intercept, options);
    }
    if (solve_write_exact_line(normal ? "normal: " : "tangent: ", slope, intercept, options) != 0) return 3;
    solve_sp_line(1, "method = exact-polynomial");
    return 0;
}

static int solve_run_end_behavior_mode(const SolveEquation *equation, const SolveOptions *options) {
    SolveRatPoly poly;
    if (solve_get_rat_poly_for_mode(equation, options, &poly) != 0) {
        tool_write_error("solve", "end behavior supported only for polynomials", 0);
        return 2;
    }
    if (solve_should_explain(options)) {
        solve_explain_working_function("end behavior", equation, options);
        solve_explain_rat_poly_line("polynomial: ", &poly, options);
        solve_sp_line(1, "rule: the leading nonzero term controls behavior as x approaches +/-inf");
    }
    solve_write_poly_end_behavior(&poly);
    return 0;
}

static const char *solve_symmetry_label(const SolveRatPoly *poly) {
    int degree = solve_rat_poly_degree(poly);
    int has_even = 0;
    int has_odd = 0;
    int i;
    for (i = 0; i <= degree; ++i) {
        if (solve_rat_is_zero(poly->coeff[i])) continue;
        if ((i % 2) == 0) has_even = 1;
        else has_odd = 1;
    }
    if (has_even && !has_odd) return "axis-symmetric to y-axis";
    if (has_odd && !has_even) return "point-symmetric to origin";
    return "none";
}

static void solve_print_rat_roots_line(const char *label, SolveBreakpoint *points, int count) {
    int i;
    solve_sp_cstr(1, label);
    if (count == 0) {
        solve_sp_line(1, " none");
        return;
    }
    solve_sp_cstr(1, " ");
    for (i = 0; i < count; ++i) {
        if (i > 0) solve_sp_cstr(1, ", ");
        solve_sp_cstr(1, points[i].label);
    }
    solve_sp_char(1, '\n');
}

static int solve_classify_critical_rat(const SolveRatPoly *first, const SolveRatPoly *second, SolveRat x, const char **label_out) {
    SolveRat second_value;
    int second_degree = solve_rat_poly_degree(second);
    if (solve_rat_poly_eval(second, second_degree, x, &second_value) != 0) return -1;
    if (solve_rat_sign(second_value) > 0) { *label_out = "minimum"; return 0; }
    if (solve_rat_sign(second_value) < 0) { *label_out = "maximum"; return 0; }
    {
        SolveRat one;
        SolveRat left;
        SolveRat right;
        SolveRat left_value;
        SolveRat right_value;
        int first_degree = solve_rat_poly_degree(first);
        if (solve_rat_make(1, 1, &one) != 0 || solve_rat_sub(x, one, &left) != 0 || solve_rat_add(x, one, &right) != 0) return -1;
        if (solve_rat_poly_eval(first, first_degree, left, &left_value) != 0 || solve_rat_poly_eval(first, first_degree, right, &right_value) != 0) return -1;
        if (solve_rat_sign(left_value) > 0 && solve_rat_sign(right_value) < 0) *label_out = "maximum";
        else if (solve_rat_sign(left_value) < 0 && solve_rat_sign(right_value) > 0) *label_out = "minimum";
        else *label_out = "saddle";
    }
    return 0;
}

static double solve_numeric_derivative_value(const SolveEquation *equation, const SolveOptions *options, double x, int order, int *ok_out) {
    double h = 0.00001 * (solve_abs(x) + 1.0);
    double left;
    double mid;
    double right;
    const char *message = 0;
    *ok_out = 0;
    if (solve_eval_function(equation, options, x - h, &left, &message) != 0 || solve_eval_function(equation, options, x + h, &right, &message) != 0) return 0.0;
    if (order == 1) {
        *ok_out = 1;
        return (right - left) / (2.0 * h);
    }
    if (solve_eval_function(equation, options, x, &mid, &message) != 0) return 0.0;
    *ok_out = 1;
    return (right - 2.0 * mid + left) / (h * h);
}

static int solve_numeric_root_seen(const double *roots, int count, double root, double tolerance) {
    int i;
    double threshold = tolerance < 0.000001 ? 0.000001 : tolerance;
    for (i = 0; i < count; ++i) {
        if (solve_abs(roots[i] - root) <= threshold * (solve_abs(root) + 1.0)) return 1;
    }
    return 0;
}

static void solve_write_point_double(double x, double y, int scale) {
    solve_sp_char(1, '(');
    solve_write_double_value(x, scale);
    solve_sp_cstr(1, ", ");
    solve_write_double_value(y, scale);
    solve_sp_char(1, ')');
}

static void solve_write_sample_window(const SolveOptions *options) {
    double lo;
    double hi;
    solve_numeric_analysis_bounds(options, &lo, &hi);
    solve_sp_cstr(1, "sample window: [");
    solve_write_double_value(lo, options->scale);
    solve_sp_cstr(1, ", ");
    solve_write_double_value(hi, options->scale);
    solve_sp_line(1, "]");
}

static int solve_numeric_derivative_roots(const SolveEquation *equation, const SolveOptions *options, int order, double *roots, int *count_out) {
    double lo;
    double hi;
    int steps = options->scan_steps > 800 ? options->scan_steps : 800;
    double step;
    double prev_x;
    int prev_ok;
    double prev;
    int count = 0;
    int i;
    solve_numeric_analysis_bounds(options, &lo, &hi);
    step = (hi - lo) / (double)steps;
    prev_x = lo;
    prev = solve_numeric_derivative_value(equation, options, prev_x, order, &prev_ok);
    for (i = 1; i <= steps && count < (int)SOLVE_MAX_RESULTS; ++i) {
        double x = lo + step * (double)i;
        int ok;
        double value = solve_numeric_derivative_value(equation, options, x, order, &ok);
        if (ok && prev_ok && prev * value <= 0.0) {
            double a = prev_x;
            double b = x;
            int iter;
            for (iter = 0; iter < 60; ++iter) {
                double m = (a + b) * 0.5;
                int mok;
                double mv = solve_numeric_derivative_value(equation, options, m, order, &mok);
                if (!mok) break;
                if (prev * mv <= 0.0) { b = m; value = mv; }
                else { a = m; prev = mv; }
            }
            {
                double root = (a + b) * 0.5;
                if (!solve_numeric_root_seen(roots, count, root, step * 2.0)) roots[count++] = root;
            }
        }
        prev_x = x;
        prev = value;
        prev_ok = ok;
    }
    *count_out = count;
    return 0;
}

static int solve_collect_numeric_function_roots(const SolveEquation *equation, const SolveOptions *options, double *roots, int *count_out) {
    SolveOptions local = *options;
    SolveResultSet set;
    double lo;
    double hi;
    int i;
    int count = 0;
    solve_numeric_analysis_bounds(options, &lo, &hi);
    rt_memset(&set, 0, sizeof(set));
    local.all = 1;
    local.explain = 0;
    local.quiet = 1;
    local.have_bracket = 0;
    local.have_scan = 1;
    local.default_scan = 0;
    local.scan_lo = lo;
    local.scan_hi = hi;
    if (local.scan_steps < 800) local.scan_steps = 800;
    solve_scan(equation, &local, &set);
    for (i = 0; i < (int)set.count && count < (int)SOLVE_MAX_RESULTS; ++i) {
        if (set.results[i].status == SOLVE_STATUS_ROOT && !solve_numeric_root_seen(roots, count, set.results[i].root, options->tolerance)) roots[count++] = set.results[i].root;
    }
    *count_out = count;
    return 0;
}

static void solve_print_numeric_roots_line(const char *label, const double *roots, int count, const SolveOptions *options) {
    int i;
    if (count <= 0) return;
    solve_sp_cstr(1, label);
    for (i = 0; i < count; ++i) {
        if (i > 0) solve_sp_cstr(1, ", ");
        else solve_sp_char(1, ' ');
        solve_write_double_value(roots[i], options->scale);
    }
    solve_sp_char(1, '\n');
}

static void solve_print_numeric_derivative_intervals(const SolveEquation *equation, const SolveOptions *options, int order, const char *positive_label, const char *negative_label) {
    double roots[SOLVE_MAX_RESULTS];
    double lo;
    double hi;
    int count = 0;
    int i;
    solve_numeric_analysis_bounds(options, &lo, &hi);
    solve_numeric_derivative_roots(equation, options, order, roots, &count);
    for (i = 0; i <= count; ++i) {
        double left = i == 0 ? lo : roots[i - 1];
        double right = i == count ? hi : roots[i];
        double sample = (left + right) * 0.5;
        int ok;
        double value;
        if (right <= left) continue;
        value = solve_numeric_derivative_value(equation, options, sample, order, &ok);
        if (!ok) continue;
        solve_sp_cstr(1, value >= 0.0 ? positive_label : negative_label);
        solve_sp_cstr(1, " (within scan range) = (");
        solve_write_double_value(left, options->scale);
        solve_sp_cstr(1, ", ");
        solve_write_double_value(right, options->scale);
        solve_sp_line(1, ")");
    }
}

static void solve_write_numeric_end_behavior(const SolveEquation *equation, const SolveOptions *options) {
    double xs[6] = { -10.0, -20.0, -40.0, 10.0, 20.0, 40.0 };
    double ys[6];
    int ok[6];
    int i;
    const char *message = 0;
    for (i = 0; i < 6; ++i) ok[i] = solve_eval_function(equation, options, xs[i], &ys[i], &message) == 0 && !solve_is_bad(ys[i]);
    if (ok[0] && ok[1] && ok[2]) {
        if (solve_abs(ys[2]) < solve_abs(ys[1]) && solve_abs(ys[1]) < solve_abs(ys[0]) && solve_abs(ys[2]) <= 0.000001) {
            solve_sp_cstr(1, "limit x->-inf approximate: 0");
            solve_sp_line(1, ys[2] < 0.0 ? " from below" : " from above");
            solve_sp_line(1, "horizontal asymptote approximate: y = 0");
        } else if (solve_abs(ys[2]) > solve_abs(ys[1]) && solve_abs(ys[1]) > solve_abs(ys[0]) && solve_abs(ys[2]) > 1000000.0) {
            solve_sp_line(1, ys[2] < 0.0 ? "limit x->-inf approximate: -inf" : "limit x->-inf approximate: +inf");
        }
    }
    if (ok[3] && ok[4] && ok[5]) {
        if (solve_abs(ys[5]) < solve_abs(ys[4]) && solve_abs(ys[4]) < solve_abs(ys[3]) && solve_abs(ys[5]) <= 0.000001) {
            solve_sp_cstr(1, "limit x->inf approximate: 0");
            solve_sp_line(1, ys[5] < 0.0 ? " from below" : " from above");
            solve_sp_line(1, "horizontal asymptote approximate: y = 0");
        } else if (solve_abs(ys[5]) > solve_abs(ys[4]) && solve_abs(ys[4]) > solve_abs(ys[3]) && solve_abs(ys[5]) > 1000000.0) {
            solve_sp_line(1, ys[5] < 0.0 ? "limit x->inf approximate: -inf" : "limit x->inf approximate: +inf");
        }
    }
}

static int solve_run_discuss_mode(const SolveEquation *equation, const SolveOptions *options) {
    SolveRatPoly poly;
    if (solve_get_rat_poly_for_mode(equation, options, &poly) == 0) {
        SolveBreakpoint zeros[SOLVE_MAX_RESULTS];
        SolveBreakpoint critical[SOLVE_MAX_RESULTS];
        SolveBreakpoint inflections[SOLVE_MAX_RESULTS];
        SolveRatPoly first;
        SolveRatPoly second;
        int zero_count = 0;
        int critical_count = 0;
        int inflection_count = 0;
        int i;
        if (solve_should_explain(options)) {
            solve_explain_working_function("curve discussion", equation, options);
            solve_explain_rat_poly_line("polynomial: ", &poly, options);
            solve_sp_line(1, "domain rule: every polynomial is defined for all real x");
            solve_sp_line(1, "symmetry rule: only even powers -> y-axis; only odd powers -> origin");
        }
        solve_sp_line(1, "domain: all real x");
        solve_sp_cstr(1, "symmetry: ");
        solve_sp_line(1, solve_symmetry_label(&poly));
        (void)solve_collect_rat_poly_roots(&poly, zeros, &zero_count);
        solve_sort_breakpoints(zeros, &zero_count, options->tolerance);
        solve_print_rat_roots_line("zeros:", zeros, zero_count);
        if (solve_rat_poly_derivative(&poly, 1, &first) != 0 || solve_rat_poly_derivative(&poly, 2, &second) != 0) return 3;
        if (solve_should_explain(options)) {
            solve_explain_rat_poly_line("first derivative: ", &first, options);
            solve_explain_rat_poly_line("second derivative: ", &second, options);
            solve_sp_line(1, "extremum rule: f' sign change + to - gives maximum; - to + gives minimum; no sign change gives saddle");
            solve_sp_line(1, "inflection rule: roots of f'' where curvature changes sign are inflection points");
        }
        (void)solve_collect_rat_poly_roots(&first, critical, &critical_count);
        solve_sort_breakpoints(critical, &critical_count, options->tolerance);
        for (i = 0; i < critical_count; ++i) {
            SolveRat y;
            const char *label = "saddle";
            if (!critical[i].exact) continue;
            if (solve_rat_poly_eval(&poly, solve_rat_poly_degree(&poly), critical[i].rat_value, &y) != 0 || solve_classify_critical_rat(&first, &second, critical[i].rat_value, &label) != 0) return 3;
            solve_sp_cstr(1, label);
            solve_sp_cstr(1, ": ");
            solve_write_point_rat(critical[i].rat_value, y);
            solve_sp_char(1, '\n');
        }
        (void)solve_collect_rat_poly_roots(&second, inflections, &inflection_count);
        solve_sort_breakpoints(inflections, &inflection_count, options->tolerance);
        for (i = 0; i < inflection_count; ++i) {
            SolveRat y;
            if (!inflections[i].exact) continue;
            if (solve_rat_poly_eval(&poly, solve_rat_poly_degree(&poly), inflections[i].rat_value, &y) != 0) return 3;
            solve_sp_cstr(1, "inflection: ");
            solve_write_point_rat(inflections[i].rat_value, y);
            solve_sp_char(1, '\n');
        }
        solve_print_labeled_intervals("increasing", "decreasing", options, &first);
        solve_print_labeled_intervals("left-curved", "right-curved", options, &second);
        solve_write_poly_end_behavior(&poly);
        solve_sp_line(1, "method = exact-polynomial");
        return 0;
    }
    if (solve_print_exp_poly_discussion(equation, options) == 0) return 0;
    {
        double zeros[SOLVE_MAX_RESULTS];
        double roots[SOLVE_MAX_RESULTS];
        double inflections[SOLVE_MAX_RESULTS];
        int zero_count = 0;
        int count = 0;
        int inflection_count = 0;
        int i;
        solve_collect_numeric_function_roots(equation, options, zeros, &zero_count);
        solve_numeric_derivative_roots(equation, options, 1, roots, &count);
        solve_numeric_derivative_roots(equation, options, 2, inflections, &inflection_count);
        if (solve_should_explain(options)) {
            solve_explain_working_function("numeric curve discussion", equation, options);
            solve_explain_scan_window_line(options);
            solve_sp_line(1, "zero rule: zeros are found by scan plus bisection inside the sampled window");
            solve_sp_line(1, "critical-point rule: finite-difference f' sign changes are classified by signs on either side");
            solve_sp_line(1, "inflection rule: finite-difference f'' sign changes are reported as approximate inflections");
            solve_sp_line(1, "end-behavior rule: far samples provide only approximate asymptote and infinity hints");
        }
        solve_write_sample_window(options);
        solve_print_numeric_roots_line("zeros approximate:", zeros, zero_count, options);
        for (i = 0; i < count; ++i) {
            double x = roots[i];
            double y;
            double left;
            double right;
            int lok;
            int rok;
            const char *message = 0;
            if (solve_eval_function(equation, options, x, &y, &message) != 0) continue;
            left = solve_numeric_derivative_value(equation, options, x - 0.01, 1, &lok);
            right = solve_numeric_derivative_value(equation, options, x + 0.01, 1, &rok);
            if (!lok || !rok) continue;
            if (left < 0.0 && right > 0.0) solve_sp_cstr(1, "minimum approximate: ");
            else if (left > 0.0 && right < 0.0) solve_sp_cstr(1, "maximum approximate: ");
            else solve_sp_cstr(1, "saddle approximate: ");
            solve_write_point_double(x, y, options->scale);
            solve_sp_char(1, '\n');
        }
        for (i = 0; i < inflection_count; ++i) {
            double x = inflections[i];
            double y;
            double left;
            double right;
            int lok;
            int rok;
            const char *message = 0;
            if (solve_eval_function(equation, options, x, &y, &message) != 0) continue;
            left = solve_numeric_derivative_value(equation, options, x - 0.01, 2, &lok);
            right = solve_numeric_derivative_value(equation, options, x + 0.01, 2, &rok);
            if (!lok || !rok || left * right > 0.0) continue;
            solve_sp_cstr(1, "inflection approximate: ");
            solve_write_point_double(x, y, options->scale);
            solve_sp_char(1, '\n');
        }
        solve_print_numeric_derivative_intervals(equation, options, 1, "increasing", "decreasing");
        solve_print_numeric_derivative_intervals(equation, options, 2, "left-curved", "right-curved");
        solve_write_numeric_end_behavior(equation, options);
        solve_sp_line(1, "status = approximate");
        return 0;
    }
}

static int solve_run_limit_mode(const SolveOptions *options, const char *expr) {
    size_t pos = 0U;
    double at;
    SolveEquation equation;
    double left = 0.0;
    double right = 0.0;
    int left_ok = 0;
    int right_ok = 0;
    int i;
    double final_h = 0.0;
    if (!solve_match_name_at(options->limit_spec, &pos, options->var_name) || options->limit_spec[pos++] != '-' || options->limit_spec[pos++] != '>' || solve_parse_double_arg(options->limit_spec + pos, &at) != 0) {
        tool_write_error("solve", "invalid --limit spec", options->limit_spec);
        return 2;
    }
    rt_copy_string(equation.left, sizeof(equation.left), expr);
    rt_copy_string(equation.right, sizeof(equation.right), "0");
    equation.has_equation = 0;
    equation.relation = SOLVE_RELATION_NONE;
    for (i = 1; i <= 8; ++i) {
        double h = 1.0;
        int j;
        const char *message = 0;
        for (j = 0; j < i; ++j) h *= 0.1;
        final_h = h;
        left_ok = solve_eval_function(&equation, options, at - h, &left, &message) == 0;
        right_ok = solve_eval_function(&equation, options, at + h, &right, &message) == 0;
    }
    if (solve_should_explain(options)) {
        solve_explain_working_function("two-sided limit", &equation, options);
        solve_explain_double_value_line("target point = ", at, options);
        solve_explain_double_value_line("final sample distance = ", final_h, options);
        if (left_ok) solve_explain_double_value_line("left sample value = ", left, options);
        if (right_ok) solve_explain_double_value_line("right sample value = ", right, options);
        solve_sp_line(1, "rule: matching left and right samples indicate a two-sided limit; divergent magnitude indicates a pole; finite mismatch indicates a jump");
        solve_sp_line(1, "status reason: this limit path is numeric sampling unless another exact mode handles the expression");
    }
    if (!left_ok || !right_ok || solve_abs(left) > 1.0e12 || solve_abs(right) > 1.0e12 || (left * right < 0.0 && solve_abs(left) > 1000000.0 && solve_abs(right) > 1000000.0)) {
        solve_sp_line(1, "limit: no two-sided limit (pole)");
        return 1;
    }
    if (solve_abs(left - right) <= 0.000001 * (solve_abs(left) + solve_abs(right) + 1.0)) {
        solve_sp_cstr(1, "limit = ");
        solve_write_double_value((left + right) * 0.5, options->scale);
        solve_sp_char(1, '\n');
        return 0;
    }
    solve_sp_line(1, "limit: no two-sided limit");
    solve_sp_cstr(1, "left = "); solve_write_double_value(left, options->scale); solve_sp_char(1, '\n');
    solve_sp_cstr(1, "right = "); solve_write_double_value(right, options->scale); solve_sp_char(1, '\n');
    return 1;
}

static int solve_copy_unwrapped(char *dst, size_t dst_size, const char *src) {
    size_t start = 0U;
    size_t end = rt_strlen(src);
    while (tool_ascii_is_space(src[start])) start += 1U;
    while (end > start && tool_ascii_is_space(src[end - 1U])) end -= 1U;
    if (end > start + 1U && src[start] == '(' && src[end - 1U] == ')') {
        size_t i;
        int depth = 0;
        int wraps = 1;
        for (i = start; i < end; ++i) {
            if (src[i] == '(') depth += 1;
            else if (src[i] == ')') {
                depth -= 1;
                if (depth == 0 && i + 1U < end) wraps = 0;
            }
        }
        if (wraps) {
            start += 1U;
            end -= 1U;
        }
    }
    return solve_copy_range(dst, dst_size, src, start, end);
}

static int solve_has_top_level_sum(const char *expr) {
    size_t index;
    int depth = 0;
    for (index = 0U; expr[index] != '\0'; ++index) {
        char ch = expr[index];
        if (ch == '(') depth += 1;
        else if (ch == ')' && depth > 0) depth -= 1;
        else if (depth == 0 && index > 0U && (ch == '+' || ch == '-')) return 1;
    }
    return 0;
}

static int solve_build_rational_sum_num(char *out, size_t out_size, const char *left, const char *op, const char *right, const char *quot_num, const char *quot_den, int quotient_on_right) {
    size_t used = 0U;
    out[0] = '\0';
    if (quotient_on_right) {
        if (solve_append_char(out, out_size, &used, '(') != 0 || solve_append_text(out, out_size, &used, left) != 0 || solve_append_text(out, out_size, &used, ")*(") != 0 || solve_append_text(out, out_size, &used, quot_den) != 0 || solve_append_text(out, out_size, &used, ") ") != 0 || solve_append_text(out, out_size, &used, op) != 0 || solve_append_text(out, out_size, &used, " (") != 0 || solve_append_text(out, out_size, &used, quot_num) != 0 || solve_append_char(out, out_size, &used, ')') != 0) return -1;
    } else {
        if (solve_append_char(out, out_size, &used, '(') != 0 || solve_append_text(out, out_size, &used, quot_num) != 0 || solve_append_text(out, out_size, &used, ") ") != 0 || solve_append_text(out, out_size, &used, op) != 0 || solve_append_text(out, out_size, &used, " (") != 0 || solve_append_text(out, out_size, &used, right) != 0 || solve_append_text(out, out_size, &used, ")*(") != 0 || solve_append_text(out, out_size, &used, quot_den) != 0 || solve_append_char(out, out_size, &used, ')') != 0) return -1;
    }
    return 0;
}

static int solve_split_rational_expr_inner(const char *expr, char *num, size_t num_size, char *den, size_t den_size, int allow_sum) {
    size_t index;
    int depth = 0;
    for (index = 0U; expr[index] != '\0'; ++index) {
        if (expr[index] == '(') depth += 1;
        else if (expr[index] == ')' && depth > 0) depth -= 1;
        else if (expr[index] == '/' && depth == 0) {
            char left[SOLVE_EXPR_CAPACITY];
            char right[SOLVE_EXPR_CAPACITY];
            if (solve_copy_range(left, sizeof(left), expr, 0U, index) != 0 || solve_copy_range(right, sizeof(right), expr, index + 1U, rt_strlen(expr)) != 0) return -1;
            if (allow_sum && (solve_has_top_level_sum(left) || solve_has_top_level_sum(right))) continue;
            return solve_copy_unwrapped(num, num_size, left) == 0 && solve_copy_unwrapped(den, den_size, right) == 0 ? 0 : -1;
        }
    }
    if (allow_sum) {
        size_t length = rt_strlen(expr);
        for (index = length; index > 0U; --index) {
            size_t pos = index - 1U;
            char ch = expr[pos];
            if (ch == ')') depth += 1;
            else if (ch == '(' && depth > 0) depth -= 1;
            else if (depth == 0 && (ch == '+' || ch == '-') && pos > 0U) {
                char left[SOLVE_EXPR_CAPACITY];
                char right[SOLVE_EXPR_CAPACITY];
                char quot_num[SOLVE_EXPR_CAPACITY];
                char quot_den[SOLVE_EXPR_CAPACITY];
                char built[SOLVE_EXPR_CAPACITY];
                char op[2];
                op[0] = ch;
                op[1] = '\0';
                if (solve_copy_range(left, sizeof(left), expr, 0U, pos) != 0 || solve_copy_range(right, sizeof(right), expr, pos + 1U, length) != 0) return -1;
                if (solve_split_rational_expr_inner(right, quot_num, sizeof(quot_num), quot_den, sizeof(quot_den), 0) == 0 && solve_build_rational_sum_num(built, sizeof(built), left, op, right, quot_num, quot_den, 1) == 0) {
                    return solve_copy_unwrapped(num, num_size, built) == 0 && solve_copy_unwrapped(den, den_size, quot_den) == 0 ? 0 : -1;
                }
                if (ch == '+' && solve_split_rational_expr_inner(left, quot_num, sizeof(quot_num), quot_den, sizeof(quot_den), 0) == 0 && solve_build_rational_sum_num(built, sizeof(built), left, op, right, quot_num, quot_den, 0) == 0) {
                    return solve_copy_unwrapped(num, num_size, built) == 0 && solve_copy_unwrapped(den, den_size, quot_den) == 0 ? 0 : -1;
                }
            }
        }
    }
    return -1;
}

static int solve_split_rational_expr(const char *expr, char *num, size_t num_size, char *den, size_t den_size) {
    return solve_split_rational_expr_inner(expr, num, num_size, den, den_size, 1);
}

static int solve_parse_two_curves(int start, int argc, char **argv, char *first, size_t first_size, char *second, size_t second_size) {
    if (start >= argc) return -1;
    if (rt_strlen(argv[start]) >= first_size) return -1;
    rt_copy_string(first, first_size, argv[start]);
    if (start + 1 < argc) {
        if (rt_strlen(argv[start + 1]) >= second_size) return -1;
        rt_copy_string(second, second_size, argv[start + 1]);
    } else {
        rt_copy_string(second, second_size, "0");
    }
    return 0;
}

static int solve_exact_area_poly(const SolveRatPoly *poly, SolveRat lo, SolveRat hi, SolveRat *out) {
    SolveBreakpoint points[SOLVE_MAX_RESULTS];
    SolveRat cuts[SOLVE_MAX_RESULTS + 2];
    int point_count = 0;
    int cut_count = 0;
    int i;
    SolveRat total;
    if (solve_rat_make(0, 1, &total) != 0) return -1;
    if (solve_rat_compare(hi, lo) < 0) { SolveRat tmp = lo; lo = hi; hi = tmp; }
    cuts[cut_count++] = lo;
    if (solve_rat_poly_roots_in_range(poly, lo, hi, points, &point_count) != 0) return -1;
    for (i = 0; i < point_count; ++i) {
        if (solve_rat_compare(points[i].rat_value, lo) > 0 && solve_rat_compare(points[i].rat_value, hi) < 0) cuts[cut_count++] = points[i].rat_value;
    }
    cuts[cut_count++] = hi;
    for (i = 0; i + 1 < cut_count; ++i) {
        SolveRat area;
        SolveRat abs_area;
        if (solve_rat_compare(cuts[i], cuts[i + 1]) == 0) continue;
        if (solve_rat_poly_definite_integral(poly, cuts[i], cuts[i + 1], &area) != 0 || solve_rat_abs_value(area, &abs_area) != 0 || solve_rat_add(total, abs_area, &total) != 0) return -1;
    }
    *out = total;
    return 0;
}

static int solve_run_area_mode(const SolveOptions *options, int start, int argc, char **argv) {
    char first_expr[SOLVE_EXPR_CAPACITY];
    char second_expr[SOLVE_EXPR_CAPACITY];
    char lo_text[128];
    char hi_text[128];
    SolveRatPoly first;
    SolveRatPoly second;
    SolveRatPoly diff;
    SolveRat lo;
    SolveRat hi;
    SolveRat area;
    int force_numeric_area = 0;
    int have_poly_diff = 0;

    if (options->have_area_quadrant) {
        if (solve_parse_two_curves(start, argc, argv, first_expr, sizeof(first_expr), second_expr, sizeof(second_expr)) != 0) return 2;
        lo_text[0] = '\0';
        hi_text[0] = '\0';
    } else if (solve_contains_char(options->range_spec, ':')) {
        if (solve_split_integral_bounds(options->range_spec, lo_text, sizeof(lo_text), hi_text, sizeof(hi_text)) != 0) {
            tool_write_error("solve", "invalid --area bounds", options->range_spec);
            return 2;
        }
        if (solve_parse_two_curves(start, argc, argv, first_expr, sizeof(first_expr), second_expr, sizeof(second_expr)) != 0) return 2;
    } else {
        if (rt_strlen(options->range_spec) >= sizeof(first_expr)) return 2;
        rt_copy_string(first_expr, sizeof(first_expr), options->range_spec);
        if (start < argc) {
            if (rt_strlen(argv[start]) >= sizeof(second_expr)) return 2;
            rt_copy_string(second_expr, sizeof(second_expr), argv[start]);
        } else {
            rt_copy_string(second_expr, sizeof(second_expr), "0");
        }
        lo_text[0] = '\0';
        hi_text[0] = '\0';
    }
    if (solve_should_explain_student(options)) {
        SolveEquation domain_equation;
        rt_memset(&domain_equation, 0, sizeof(domain_equation));
        rt_copy_string(domain_equation.left, sizeof(domain_equation.left), first_expr);
        rt_copy_string(domain_equation.right, sizeof(domain_equation.right), second_expr);
        solve_sp_line(1, "worked solution");
        solve_sp_cstr(1, "Given: area between ");
        solve_sp_cstr(1, first_expr);
        solve_sp_cstr(1, " and ");
        solve_sp_line(1, second_expr);
        solve_sp_cstr(1, "Variable: ");
        solve_sp_line(1, options->var_name);
        solve_sp_line(1, options->have_area_quadrant ? "Goal: choose the enclosed lobe in the requested quadrant and compute its area." : "Goal: compute the area between the two curves.");
        solve_sp_cstr(1, "Rewrite: h(");
        solve_sp_cstr(1, options->var_name);
        solve_sp_cstr(1, ") = (");
        solve_sp_cstr(1, first_expr);
        solve_sp_cstr(1, ") - (");
        solve_sp_cstr(1, second_expr);
        solve_sp_cstr(1, "), then integrate |h(");
        solve_sp_cstr(1, options->var_name);
        solve_sp_line(1, ")| over the selected interval.");
        solve_student_domain_notes(&domain_equation);
        solve_sp_line(1, "Method: split at intersections or sign changes, integrate each lobe as a positive area, then add the pieces.");
    }
    if (solve_should_explain(options)) {
        solve_sp_line(1, "explain: area between curves");
        solve_sp_cstr(1, "upper/lower difference h(x) = (");
        solve_sp_cstr(1, first_expr);
        solve_sp_cstr(1, ") - (");
        solve_sp_cstr(1, second_expr);
        solve_sp_line(1, ")");
        if (lo_text[0] != '\0') {
            solve_sp_cstr(1, "bounds: ");
            solve_sp_cstr(1, lo_text);
            solve_sp_cstr(1, " to ");
            solve_sp_line(1, hi_text);
        } else {
            solve_sp_line(1, "bounds: omitted; using leftmost and rightmost exact intersections when available");
        }
        solve_sp_line(1, "rule: area is integral |h(x)| dx, so sign changes split the interval into absolute pieces");
    }
    if (solve_parse_rat_text(first_expr, options->var_name, &first) == 0 && solve_parse_rat_text(second_expr, options->var_name, &second) == 0 && solve_rat_poly_add(&first, &second, 1, &diff) == 0 &&
        ((lo_text[0] != '\0' && solve_rat_poly_parse_bound(lo_text, options->var_name, &lo) == 0 && solve_rat_poly_parse_bound(hi_text, options->var_name, &hi) == 0) || lo_text[0] == '\0')) {
        have_poly_diff = 1;
        if (lo_text[0] == '\0') {
            SolveBreakpoint roots[SOLVE_MAX_RESULTS];
            int root_count = 0;
            if (solve_collect_rat_poly_roots(&diff, roots, &root_count) != 0 || root_count < 2) {
                if (!options->have_area_quadrant) goto numeric_omitted_area_bounds;
                tool_write_error("solve", "area bounds omitted but fewer than two intersections were found", 0);
                return 2;
            }
            solve_sort_breakpoints(roots, &root_count, options->tolerance);
            if (solve_should_explain(options)) solve_print_rat_roots_line("intersections:", roots, root_count);
            if (options->have_area_quadrant) {
                int wanted_x_negative = rt_strcmp(options->range_spec, "II") == 0 || rt_strcmp(options->range_spec, "III") == 0 || rt_strcmp(options->range_spec, "2") == 0 || rt_strcmp(options->range_spec, "3") == 0;
                int wanted_y_positive = rt_strcmp(options->range_spec, "I") == 0 || rt_strcmp(options->range_spec, "II") == 0 || rt_strcmp(options->range_spec, "1") == 0 || rt_strcmp(options->range_spec, "2") == 0;
                int found = 0;
                int i;
                for (i = 0; i + 1 < root_count; ++i) {
                    double left = roots[i].value;
                    double right = roots[i + 1].value;
                    double sample = (left + right) * 0.5;
                    double y;
                    const char *message = 0;
                    SolveEquation curve;
                    rt_copy_string(curve.left, sizeof(curve.left), first_expr);
                    rt_copy_string(curve.right, sizeof(curve.right), "0");
                    curve.has_equation = 0;
                    curve.relation = SOLVE_RELATION_NONE;
                    if (solve_eval_function(&curve, options, sample, &y, &message) != 0) continue;
                    if (((sample < 0.0) == wanted_x_negative) && ((y >= 0.0) == wanted_y_positive)) {
                        if (roots[i].exact && roots[i + 1].exact) {
                            lo = roots[i].rat_value;
                            hi = roots[i + 1].rat_value;
                        } else {
                            solve_format_double(left, SOLVE_MAX_SCALE, lo_text, sizeof(lo_text));
                            solve_format_double(right, SOLVE_MAX_SCALE, hi_text, sizeof(hi_text));
                            force_numeric_area = 1;
                        }
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    tool_write_error("solve", "no exact polynomial lobe found in requested quadrant", options->range_spec);
                    return 1;
                }
                if (solve_should_explain(options)) solve_sp_line(1, "quadrant rule: choose consecutive intersections whose midpoint lies in the requested quadrant");
                if (force_numeric_area) goto numeric_area;
            } else {
                lo = roots[0].rat_value;
                hi = roots[root_count - 1].rat_value;
            }
        }
        if (solve_should_explain(options)) {
            SolveBreakpoint area_roots[SOLVE_MAX_RESULTS];
            int area_root_count = 0;
            solve_explain_rat_poly_line("exact difference polynomial: ", &diff, options);
            solve_explain_rat_value_line("effective lower bound = ", lo, options);
            solve_explain_rat_value_line("effective upper bound = ", hi, options);
            if (solve_rat_poly_roots_in_range(&diff, lo, hi, area_roots, &area_root_count) == 0) {
                solve_sort_breakpoints(area_roots, &area_root_count, options->tolerance);
                solve_print_rat_roots_line("area cut roots:", area_roots, area_root_count);
            }
            solve_sp_line(1, "method detail: exact polynomial roots inside the bounds split positive and negative lobes");
        }
        if (solve_exact_area_poly(&diff, lo, hi, &area) != 0) return 3;
        solve_sp_cstr(1, "area = ");
        solve_write_rat_value(area);
        solve_sp_char(1, '\n');
        solve_sp_line(1, "method = exact-polynomial");
        return 0;
    }
numeric_omitted_area_bounds:
    if (lo_text[0] == '\0' && !options->have_area_quadrant) {
        SolveEquation bounds_equation;
        SolveOptions scan_options = *options;
        SolveResultSet roots;
        if (options->default_scan || !options->have_scan) {
            tool_write_error("solve", "numeric omitted area bounds require --scan", 0);
            return 2;
        }
        rt_memset(&roots, 0, sizeof(roots));
        rt_copy_string(bounds_equation.left, sizeof(bounds_equation.left), first_expr);
        rt_copy_string(bounds_equation.right, sizeof(bounds_equation.right), second_expr);
        bounds_equation.has_equation = 1;
        bounds_equation.relation = SOLVE_RELATION_EQ;
        scan_options.all = 1;
        scan_options.quiet = 1;
        scan_options.explain = 0;
        scan_options.have_bracket = 0;
        scan_options.have_scan = 1;
        if (scan_options.scan_steps < 800) scan_options.scan_steps = 800;
        solve_scan(&bounds_equation, &scan_options, &roots);
        if (roots.count < 2U) {
            tool_write_error("solve", "area bounds omitted but fewer than two intersections were found", 0);
            return 2;
        }
        solve_format_double(roots.results[0].root, SOLVE_MAX_SCALE, lo_text, sizeof(lo_text));
        solve_format_double(roots.results[roots.count - 1U].root, SOLVE_MAX_SCALE, hi_text, sizeof(hi_text));
        if (solve_should_explain(options)) solve_sp_line(1, "numeric bounds: omitted bounds found by scan over the configured interval");
    }
numeric_area:
    {
        SolveEquation equation;
        SolveOptions local = *options;
        double lo_d;
        double hi_d;
        double h;
        double sum = 0.0;
        int n = 2000;
        int i;
        char value[96];
        char hint[96];
        double result;
        if (lo_text[0] == '\0' || solve_eval_bound_expr(lo_text, options, &lo_d) != 0 || solve_eval_bound_expr(hi_text, options, &hi_d) != 0) return 2;
        rt_copy_string(equation.left, sizeof(equation.left), first_expr);
        rt_copy_string(equation.right, sizeof(equation.right), second_expr);
        equation.has_equation = 1;
        equation.relation = SOLVE_RELATION_EQ;
        h = (hi_d - lo_d) / (double)n;
        for (i = 0; i <= n; ++i) {
            double x = lo_d + h * (double)i;
            double y;
            const char *message = 0;
            if (i == n) x = hi_d;
            if (solve_eval_function(&equation, &local, x, &y, &message) != 0) return 3;
            y = solve_abs(y);
            if (i == 0 || i == n) sum += y;
            else sum += (i % 2 == 0 ? 2.0 : 4.0) * y;
        }
        if (solve_should_explain(options)) {
            solve_explain_double_value_line("numeric lower bound = ", lo_d, options);
            solve_explain_double_value_line("numeric upper bound = ", hi_d, options);
            solve_sp_line(1, "method detail: composite Simpson rule samples |f(x)-g(x)| directly with 2000 subintervals");
            solve_sp_line(1, have_poly_diff ? "status reason: polynomial area has non-rational bounds here, so the printed area is numeric" : "status reason: non-polynomial area is numeric and approximate");
        }
        result = sum * h / 3.0;
        solve_format_double(result, options->scale, value, sizeof(value));
        solve_sp_cstr(1, "area = "); solve_sp_line(1, value);
        if (have_poly_diff && solve_format_fraction_hint(result, hint, sizeof(hint)) == 0) {
            solve_sp_cstr(1, "rational area hint = ");
            solve_sp_line(1, hint);
        }
        solve_sp_line(1, "method = simpson");
        solve_sp_line(1, "status = approximate");
        return 0;
    }
}

static int solve_run_volume_mean_mode(const SolveOptions *options, const char *expr, int volume) {
    char lo_text[128];
    char hi_text[128];
    SolveRatPoly poly;
    SolveRatPoly integrand;
    SolveRat lo;
    SolveRat hi;
    SolveRat value;
    double lo_d;
    double hi_d;
    if (solve_split_integral_bounds(options->range_spec, lo_text, sizeof(lo_text), hi_text, sizeof(hi_text)) != 0) return 2;
    if (solve_should_explain_student(options)) {
        SolveEquation domain_equation;
        rt_memset(&domain_equation, 0, sizeof(domain_equation));
        rt_copy_string(domain_equation.left, sizeof(domain_equation.left), expr);
        rt_copy_string(domain_equation.right, sizeof(domain_equation.right), "0");
        solve_sp_line(1, "worked solution");
        solve_sp_cstr(1, "Given: ");
        solve_sp_line(1, expr);
        solve_sp_cstr(1, "Variable: ");
        solve_sp_line(1, options->var_name);
        solve_sp_line(1, volume ? "Goal: compute the volume of rotation around the x-axis." : "Goal: compute the mean value on the interval.");
        solve_sp_cstr(1, "Interval: [");
        solve_sp_cstr(1, lo_text);
        solve_sp_cstr(1, ", ");
        solve_sp_cstr(1, hi_text);
        solve_sp_line(1, "]");
        solve_student_domain_notes(&domain_equation);
        solve_sp_line(1, volume ? "Method: square the function, integrate, then multiply by pi." : "Method: integrate the function, then divide by the interval width.");
    }
    if (solve_should_explain(options)) {
        solve_sp_line(1, volume ? "explain: volume of rotation" : "explain: mean value");
        solve_sp_cstr(1, "working function: f(x) = ");
        solve_sp_line(1, expr);
        solve_sp_cstr(1, "bounds: ");
        solve_sp_cstr(1, lo_text);
        solve_sp_cstr(1, " to ");
        solve_sp_line(1, hi_text);
        solve_sp_line(1, volume ? "formula: volume = pi * integral f(x)^2 dx" : "formula: mean = (1/(b-a)) * integral f(x) dx");
    }
    if (solve_parse_rat_text(expr, options->var_name, &poly) == 0 && solve_rat_poly_parse_bound(lo_text, options->var_name, &lo) == 0 && solve_rat_poly_parse_bound(hi_text, options->var_name, &hi) == 0) {
        integrand = poly;
        if (volume && solve_rat_poly_square(&poly, &integrand) != 0) return 3;
        if (solve_rat_poly_definite_integral(&integrand, lo, hi, &value) != 0) return 3;
        if (solve_should_explain(options)) {
            solve_explain_rat_poly_line("exact integrand polynomial: ", &integrand, options);
            solve_explain_rat_value_line("exact integral = ", value, options);
            if (!volume) solve_sp_line(1, "next: divide by interval width b-a");
        }
        if (!volume) {
            SolveRat width;
            if (solve_rat_sub(hi, lo, &width) != 0 || solve_rat_div(value, width, &value) != 0) return 3;
            solve_explain_rat_value_line("interval width = ", width, options);
            solve_sp_cstr(1, "mean = "); solve_write_rat_value(value); solve_sp_char(1, '\n');
        } else {
            solve_sp_cstr(1, "volume = pi*("); solve_write_rat_value(value); solve_sp_line(1, ")");
        }
        solve_sp_line(1, "method = exact-polynomial");
        return 0;
    }
    if (solve_eval_bound_expr(lo_text, options, &lo_d) != 0 || solve_eval_bound_expr(hi_text, options, &hi_d) != 0) return 2;
    {
        SolveEquation equation;
        double coarse;
        double fine;
        double result;
        char text[96];
        rt_copy_string(equation.left, sizeof(equation.left), expr);
        rt_copy_string(equation.right, sizeof(equation.right), "0");
        equation.has_equation = 0;
        equation.relation = SOLVE_RELATION_NONE;
        if (volume) {
            if (solve_simpson_square_eval(&equation, options, lo_d, hi_d, 1000, &coarse) != 0 || solve_simpson_square_eval(&equation, options, lo_d, hi_d, 2000, &fine) != 0) return 3;
            if (solve_should_explain(options)) {
                solve_sp_line(1, "method detail: composite Simpson rule integrates f(x)^2 with 1000 and 2000 subintervals");
                solve_explain_double_value_line("coarse integral = ", coarse, options);
                solve_explain_double_value_line("fine integral = ", fine, options);
                solve_sp_line(1, "status reason: non-polynomial rotation volume is numeric and approximate");
            }
            solve_format_double(fine, options->scale, text, sizeof(text));
            solve_sp_cstr(1, "volume approximate = pi*(");
            solve_sp_cstr(1, text);
            solve_sp_line(1, ")");
        } else {
            if (solve_simpson_eval(&equation, options, lo_d, hi_d, 1000, &coarse) != 0 || solve_simpson_eval(&equation, options, lo_d, hi_d, 2000, &fine) != 0 || hi_d == lo_d) return 3;
            result = fine / (hi_d - lo_d);
            if (solve_should_explain(options)) {
                solve_sp_line(1, "method detail: composite Simpson rule integrates f(x), then divides by b-a");
                solve_explain_double_value_line("fine integral = ", fine, options);
                solve_explain_double_value_line("interval width = ", hi_d - lo_d, options);
                solve_sp_line(1, "status reason: non-polynomial mean value is numeric and approximate");
            }
            solve_format_double(result, options->scale, text, sizeof(text));
            solve_sp_cstr(1, "mean approximate = ");
            solve_sp_line(1, text);
        }
        solve_sp_line(1, "method = simpson");
        solve_sp_line(1, "status = approximate");
        return 0;
    }
}

static int solve_run_asymptotes_mode(const SolveOptions *options, const char *expr) {
    char num_text[SOLVE_EXPR_CAPACITY];
    char den_text[SOLVE_EXPR_CAPACITY];
    SolveRatPoly num;
    SolveRatPoly den;
    SolveRatPoly quotient;
    SolveRatPoly remainder;
    SolveBreakpoint roots[SOLVE_MAX_RESULTS];
    int root_count = 0;
    int i;
    char text[SOLVE_EXPR_CAPACITY];
    if (solve_split_rational_expr(expr, num_text, sizeof(num_text), den_text, sizeof(den_text)) != 0 || solve_parse_rat_text(num_text, options->var_name, &num) != 0 || solve_parse_rat_text(den_text, options->var_name, &den) != 0) {
        tool_write_error("solve", "asymptotes supported only for rational polynomial functions", 0);
        return 2;
    }
    if (solve_should_explain(options)) {
        solve_sp_line(1, "explain: rational asymptotes");
        solve_explain_rat_poly_line("numerator: ", &num, options);
        solve_explain_rat_poly_line("denominator: ", &den, options);
        solve_sp_line(1, "vertical rule: denominator root with nonzero numerator gives a vertical asymptote");
        solve_sp_line(1, "end-behavior rule: polynomial division gives horizontal or oblique asymptote when the remainder tends to 0");
    }
    if (solve_collect_rat_poly_roots(&den, roots, &root_count) != 0) return 3;
    solve_sort_breakpoints(roots, &root_count, options->tolerance);
    if (solve_should_explain(options)) solve_print_rat_roots_line("denominator roots:", roots, root_count);
    for (i = 0; i < root_count; ++i) {
        SolveRat value;
        if (!roots[i].exact) continue;
        if (solve_should_explain(options) && solve_rat_poly_eval(&num, solve_rat_poly_degree(&num), roots[i].rat_value, &value) == 0) {
            solve_sp_cstr(1, "numerator at ");
            solve_sp_cstr(1, roots[i].label);
            solve_sp_cstr(1, " = ");
            solve_write_rat_value(value);
            solve_sp_char(1, '\n');
        }
        if (solve_rat_poly_eval(&num, solve_rat_poly_degree(&num), roots[i].rat_value, &value) == 0 && !solve_rat_is_zero(value)) {
            solve_sp_cstr(1, "vertical: x = "); solve_sp_line(1, roots[i].label);
        }
    }
    if (solve_rat_poly_divide(&num, &den, &quotient, &remainder) != 0) return 3;
    if (solve_should_explain(options)) {
        solve_explain_rat_poly_line("polynomial quotient: ", &quotient, options);
        solve_explain_rat_poly_line("remainder: ", &remainder, options);
        solve_sp_line(1, "division form: numerator/denominator = quotient + remainder/denominator");
    }
    if (solve_rat_poly_degree(&quotient) <= 1) {
        if (solve_rat_poly_format(&quotient, options->var_name, text, sizeof(text)) != 0) return 3;
        solve_sp_cstr(1, solve_rat_poly_degree(&quotient) == 1 ? "oblique: y = " : "horizontal: y = ");
        solve_sp_line(1, text);
    } else {
        if (solve_rat_poly_format(&quotient, options->var_name, text, sizeof(text)) != 0) return 3;
        solve_sp_cstr(1, "polynomial quotient: y = "); solve_sp_line(1, text);
    }
    solve_sp_line(1, "method = exact-polynomial");
    return 0;
}

static int solve_parse_range_bound(const char *text, const SolveOptions *options, double *value_out, int *is_neg_inf_out, int *is_pos_inf_out) {
    *is_neg_inf_out = 0;
    *is_pos_inf_out = 0;
    if (rt_strcmp(text, "inf") == 0 || rt_strcmp(text, "+inf") == 0) {
        *is_pos_inf_out = 1;
        *value_out = 0.0;
        return 0;
    }
    if (rt_strcmp(text, "-inf") == 0) {
        *is_neg_inf_out = 1;
        *value_out = 0.0;
        return 0;
    }
    return solve_eval_bound_expr(text, options, value_out);
}

static int solve_parse_numeric_range(const char *spec, const SolveOptions *options, double *lo_out, double *hi_out, int *lo_inf_out, int *hi_inf_out) {
    char lo_text[128];
    char hi_text[128];
    int lo_pos;
    int hi_neg;
    if (solve_split_integral_bounds(spec, lo_text, sizeof(lo_text), hi_text, sizeof(hi_text)) != 0) return -1;
    if (solve_parse_range_bound(lo_text, options, lo_out, lo_inf_out, &lo_pos) != 0) return -1;
    if (solve_parse_range_bound(hi_text, options, hi_out, &hi_neg, hi_inf_out) != 0) return -1;
    if (lo_pos || hi_neg) return -1;
    if (*lo_inf_out) *lo_out = *hi_inf_out ? -10.0 : *hi_out - 40.0;
    if (*hi_inf_out) *hi_out = *lo_inf_out ? 10.0 : *lo_out + 40.0;
    return *hi_out > *lo_out ? 0 : -1;
}

static int solve_exp_poly_limit_endpoint_safe(const SolveRatPoly *exp_arg, int lo_inf, int hi_inf) {
    int degree = solve_rat_poly_degree(exp_arg);
    int slope_sign;
    if (!lo_inf && !hi_inf) return 1;
    if (degree != 1) return 0;
    slope_sign = solve_rat_sign(exp_arg->coeff[1]);
    if (hi_inf && slope_sign >= 0) return 0;
    if (lo_inf && slope_sign <= 0) return 0;
    return 1;
}

static int solve_run_exp_poly_extreme_mode(const SolveEquation *equation, const SolveOptions *options, int want_max) {
    SolveRatPoly factor;
    SolveRatPoly exp_arg;
    SolveBreakpoint roots[SOLVE_MAX_RESULTS];
    SolveOptions local = *options;
    double lo;
    double hi;
    int lo_inf;
    int hi_inf;
    int root_count = 0;
    int i;
    int have_best = 0;
    int have_critical_candidate = 0;
    double best_x = 0.0;
    double best_y = 0.0;
    char text[96];
    const char *message = 0;

    if (equation->has_equation || solve_parse_numeric_range(options->range_spec, options, &lo, &hi, &lo_inf, &hi_inf) != 0) return -1;
    if (solve_sym_exp_poly_derivative_factor(equation->left, options, 1, &factor, &exp_arg) != 0 || !solve_exp_poly_limit_endpoint_safe(&exp_arg, lo_inf, hi_inf)) return -1;
    if (solve_collect_rat_poly_roots(&factor, roots, &root_count) != 0 || root_count <= 0) return -1;
    solve_sort_breakpoints(roots, &root_count, options->tolerance);

    local.default_scan = 0;
    local.have_scan = 1;
    local.scan_lo = lo;
    local.scan_hi = hi;
    if (solve_should_explain(options)) {
        solve_explain_working_function(want_max ? "maximum" : "minimum", equation, options);
        solve_sp_line(1, "method detail: expression recognized as exp(linear)*polynomial");
        solve_explain_rat_poly_line("critical factor after removing nonzero exponential: ", &factor, options);
        solve_sp_line(1, "rule: exp(linear) is never zero, so critical x-values come from this exact polynomial factor");
    }
    if (!lo_inf && solve_eval_function(equation, &local, lo, &best_y, &message) == 0) {
        best_x = lo;
        have_best = 1;
    }
    if (!hi_inf) {
        double y;
        if (solve_eval_function(equation, &local, hi, &y, &message) == 0 && (!have_best || (want_max ? y > best_y : y < best_y))) {
            best_x = hi;
            best_y = y;
            have_best = 1;
        }
    }
    for (i = 0; i < root_count; ++i) {
        double y;
        if (roots[i].value < lo || roots[i].value > hi) continue;
        if (solve_eval_function(equation, &local, roots[i].value, &y, &message) != 0) continue;
        have_critical_candidate = 1;
        if (!have_best || (want_max ? y > best_y : y < best_y)) {
            best_x = roots[i].value;
            best_y = y;
            have_best = 1;
        }
    }
    if (!have_best || !have_critical_candidate) return -1;
    solve_sp_cstr(1, want_max ? "maximum: " : "minimum: ");
    solve_write_point_double(best_x, best_y, options->scale);
    solve_sp_char(1, '\n');
    solve_format_double(best_y, options->scale, text, sizeof(text));
    solve_sp_cstr(1, "value approximate = ");
    solve_sp_line(1, text);
    solve_sp_line(1, "method = exact-exp-polynomial-critical-points");
    solve_sp_line(1, "status = approximate-value");
    return 0;
}

static int solve_run_extreme_mode(const SolveEquation *equation, const SolveOptions *options, int want_max) {
    SolveOptions local = *options;
    double lo;
    double hi;
    int lo_inf;
    int hi_inf;
    double roots[SOLVE_MAX_RESULTS];
    int count = 0;
    int i;
    int have_best = 0;
    double best_x = 0.0;
    double best_y = 0.0;
    char text[96];
    const char *message = 0;

    {
        int exp_poly_status = solve_run_exp_poly_extreme_mode(equation, options, want_max);
        if (exp_poly_status == 0) return 0;
    }

    if (solve_parse_numeric_range(options->range_spec, options, &lo, &hi, &lo_inf, &hi_inf) != 0) {
        tool_write_error("solve", "invalid extremum range", options->range_spec);
        return 2;
    }
    local.default_scan = 0;
    local.have_scan = 1;
    local.scan_lo = lo;
    local.scan_hi = hi;
    if (local.scan_steps < 1600) local.scan_steps = 1600;
    if (solve_should_explain(options)) {
        solve_explain_working_function(want_max ? "maximum" : "minimum", equation, options);
        solve_sp_line(1, "rule: compare endpoints and numeric critical points where f' changes sign");
        if (lo_inf || hi_inf) solve_sp_line(1, "range note: infinite endpoint is sampled over a finite exploratory window");
    }
    if (!lo_inf && solve_eval_function(equation, &local, lo, &best_y, &message) == 0) {
        best_x = lo;
        have_best = 1;
    }
    if (!hi_inf) {
        double y;
        if (solve_eval_function(equation, &local, hi, &y, &message) == 0 && (!have_best || (want_max ? y > best_y : y < best_y))) {
            best_x = hi;
            best_y = y;
            have_best = 1;
        }
    }
    solve_numeric_derivative_roots(equation, &local, 1, roots, &count);
    for (i = 0; i < count; ++i) {
        double y;
        if (roots[i] < lo || roots[i] > hi) continue;
        if (solve_eval_function(equation, &local, roots[i], &y, &message) != 0) continue;
        if (!have_best || (want_max ? y > best_y : y < best_y)) {
            best_x = roots[i];
            best_y = y;
            have_best = 1;
        }
    }
    if (!have_best) return 3;
    solve_sp_cstr(1, want_max ? "maximum approximate: " : "minimum approximate: ");
    solve_write_point_double(best_x, best_y, options->scale);
    solve_sp_char(1, '\n');
    solve_format_double(best_y, options->scale, text, sizeof(text));
    solve_sp_cstr(1, "value approximate = ");
    solve_sp_line(1, text);
    solve_sp_line(1, "method = numeric-critical-points");
    solve_sp_line(1, "status = approximate");
    return 0;
}

static int solve_parse_point_pair(const char *text, size_t *pos_io, double *x_out, double *y_out) {
    if (solve_parse_double(text, pos_io, x_out) != 0) return -1;
    if (text[*pos_io] != ':') return -1;
    *pos_io += 1U;
    if (solve_parse_double(text, pos_io, y_out) != 0) return -1;
    return 0;
}

static int solve_run_fit_exp_mode(const SolveOptions *options) {
    size_t pos = 0U;
    double t1;
    double y1;
    double t2;
    double y2;
    double d1;
    double d2;
    double ratio;
    double c;
    double b;
    char text[96];
    if (solve_parse_point_pair(options->fit_points_spec, &pos, &t1, &y1) != 0 || options->fit_points_spec[pos] != ',') return 2;
    pos += 1U;
    if (solve_parse_point_pair(options->fit_points_spec, &pos, &t2, &y2) != 0 || options->fit_points_spec[pos] != '\0' || t1 == t2) return 2;
    d1 = y1 - options->fit_asymptote;
    d2 = y2 - options->fit_asymptote;
    if (d1 == 0.0 || d2 == 0.0 || d1 * d2 <= 0.0) return 3;
    ratio = d2 / d1;
    if (ratio <= 0.0) return 3;
    c = -solve_log(ratio) / (t2 - t1);
    b = d1 * solve_exp(c * t1);
    if (solve_should_explain(options)) {
        solve_sp_line(1, "explain: exponential cooling fit");
        solve_sp_line(1, "model: a + b*exp(-c*x)");
        solve_sp_line(1, "rule: divide y-a values to eliminate b, then solve for c");
    }
    solve_sp_cstr(1, "model: ");
    solve_write_double_value(options->fit_asymptote, options->scale);
    solve_sp_cstr(1, " + ");
    solve_write_double_value(b, options->scale);
    solve_sp_cstr(1, "*exp(-");
    solve_write_double_value(c, options->scale);
    solve_sp_line(1, "*x)");
    solve_format_double(options->fit_asymptote, options->scale, text, sizeof(text));
    solve_sp_cstr(1, "a = "); solve_sp_line(1, text);
    solve_format_double(b, options->scale, text, sizeof(text));
    solve_sp_cstr(1, "b = "); solve_sp_line(1, text);
    solve_format_double(c, options->scale, text, sizeof(text));
    solve_sp_cstr(1, "c = "); solve_sp_line(1, text);
    solve_sp_line(1, "method = exponential-asymptote-fit");
    solve_sp_line(1, "status = approximate");
    return 0;
}


#endif
