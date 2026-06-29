#if !defined(SOLVE_FROM_SOLVE_C)
#ifndef SOLVE_PRIVATE_CONTEXT
#define SOLVE_PRIVATE_CONTEXT 1
#endif
#include "../solve.c"
#else
static int solve_add_direct_rat_root(const SolveEquation *equation, const SolveOptions *options, SolveResultSet *set, SolveRat root, const char *method) {
    SolveResult result;
    const char *message = 0;

    if (!solve_root_in_scan_range(options, solve_rat_to_double(root))) return 0;
    rt_memset(&result, 0, sizeof(result));
    result.root = solve_rat_to_double(root);
    result.lo = result.root;
    result.hi = result.root;
    result.iterations = 0;
    result.status = SOLVE_STATUS_ROOT;
    result.method = method;
    if (solve_rat_format(root, result.exact_value, sizeof(result.exact_value)) != 0) return -1;
    if (solve_eval_function(equation, options, result.root, &result.residual, &message) != 0) result.residual = 0.0;
    if (solve_eval_y(equation, options, result.root, &result.y) != 0) result.y = 0.0;
    return solve_add_result(set, &result, 1, options->tolerance);
}

static int solve_add_direct_approx_root(const SolveEquation *equation, const SolveOptions *options, SolveResultSet *set, double root, const char *method) {
    SolveResult result;
    const char *message = 0;

    if (!solve_root_in_scan_range(options, root)) return 0;
    rt_memset(&result, 0, sizeof(result));
    result.root = root;
    result.lo = root;
    result.hi = root;
    result.iterations = 0;
    result.status = SOLVE_STATUS_ROOT;
    result.method = method;
    result.approximate = 1;
    if (solve_eval_function(equation, options, root, &result.residual, &message) != 0) result.residual = 0.0;
    if (solve_eval_y(equation, options, root, &result.y) != 0) result.y = 0.0;
    return solve_add_result(set, &result, 1, options->tolerance);
}

static int solve_should_explain(const SolveOptions *options) {
    return options->explain && !tool_json_is_enabled() && !options->quiet;
}

static void solve_explain_working_function(const char *mode, const SolveEquation *equation, const SolveOptions *options) {
    rt_write_cstr(1, "explain: ");
    rt_write_line(1, mode);
    rt_write_cstr(1, "working function: f(");
    rt_write_cstr(1, options->var_name);
    rt_write_cstr(1, ") = ");
    rt_write_cstr(1, equation->left);
    if (equation->has_equation) {
        rt_write_cstr(1, " - (");
        rt_write_cstr(1, equation->right);
        rt_write_cstr(1, ")");
    }
    rt_write_char(1, '\n');
}

static void solve_explain_linear(const SolveEquation *equation, const SolveOptions *options, double slope, double intercept, double root) {
    char value[96];
    int unit_slope = solve_abs(slope - 1.0) <= options->tolerance * 2.0;
    int negative_unit_slope = solve_abs(slope + 1.0) <= options->tolerance * 2.0;

    if (!solve_should_explain(options)) {
        return;
    }
    rt_write_line(1, "linear equation detected");
    rt_write_cstr(1, "rewrite: ");
    rt_write_cstr(1, equation->left);
    rt_write_cstr(1, " = ");
    rt_write_line(1, equation->right);
    rt_write_cstr(1, "as: ");
    if (unit_slope) {
        rt_write_cstr(1, options->var_name);
    } else if (negative_unit_slope) {
        rt_write_cstr(1, "-");
        rt_write_cstr(1, options->var_name);
    } else {
        solve_format_answer(slope, options->scale, value, sizeof(value));
        rt_write_cstr(1, value);
        rt_write_cstr(1, "*");
        rt_write_cstr(1, options->var_name);
    }
    if (intercept < 0.0) {
        rt_write_cstr(1, " - ");
        solve_format_answer(-intercept, options->scale, value, sizeof(value));
    } else {
        rt_write_cstr(1, " + ");
        solve_format_answer(intercept, options->scale, value, sizeof(value));
    }
    rt_write_cstr(1, value);
    rt_write_line(1, " = 0");
    rt_write_cstr(1, "move constant term: ");
    if (unit_slope) {
        rt_write_cstr(1, options->var_name);
    } else if (negative_unit_slope) {
        rt_write_cstr(1, "-");
        rt_write_cstr(1, options->var_name);
    } else {
        solve_format_answer(slope, options->scale, value, sizeof(value));
        rt_write_cstr(1, value);
        rt_write_cstr(1, "*");
        rt_write_cstr(1, options->var_name);
    }
    rt_write_cstr(1, " = ");
    solve_format_answer(-intercept, options->scale, value, sizeof(value));
    rt_write_line(1, value);
    rt_write_cstr(1, "divide by coefficient: ");
    rt_write_cstr(1, options->var_name);
    rt_write_cstr(1, " = ");
    solve_format_answer(root, options->scale, value, sizeof(value));
    rt_write_line(1, value);
}

static int solve_try_linear(const SolveEquation *equation, const SolveOptions *options, SolveResultSet *set) {
    SolvePoly polynomial_guard;
    double f0;
    double f1;
    double f2;
    double slope;
    double second_difference;
    double root;
    double residual;
    const char *message = 0;
    SolveResult result;

    if (rt_strcmp(options->method, "auto") != 0) {
        return 0;
    }
    if (solve_equation_poly(equation, options, &polynomial_guard) != 0 || solve_poly_degree(&polynomial_guard, options->tolerance * 10.0) != 1) {
        return 0;
    }
    if (solve_eval_function(equation, options, 0.0, &f0, &message) != 0 ||
        solve_eval_function(equation, options, 1.0, &f1, &message) != 0 ||
        solve_eval_function(equation, options, 2.0, &f2, &message) != 0) {
        return 0;
    }
    slope = f1 - f0;
    second_difference = f2 - 2.0 * f1 + f0;
    if (solve_abs(slope) <= options->tolerance || solve_abs(second_difference) > options->tolerance * 100.0) {
        return 0;
    }
    root = -f0 / slope;
    if (solve_eval_function(equation, options, root, &residual, &message) != 0 || solve_abs(residual) > options->tolerance * 10.0) {
        return 0;
    }

    rt_memset(&result, 0, sizeof(result));
    result.root = root;
    result.residual = residual;
    result.lo = root;
    result.hi = root;
    result.iterations = 0;
    result.status = SOLVE_STATUS_ROOT;
    result.method = "linear";
    if (solve_eval_y(equation, options, root, &result.y) != 0) {
        result.y = 0.0;
    }
    solve_explain_linear(equation, options, slope, f0, root);
    (void)solve_add_result(set, &result, 1, options->tolerance);
    return set->count > 0U ? 1 : 0;
}

static int solve_root_in_scan_range(const SolveOptions *options, double root) {
    double lo = options->scan_lo < options->scan_hi ? options->scan_lo : options->scan_hi;
    double hi = options->scan_lo < options->scan_hi ? options->scan_hi : options->scan_lo;
    double slack = options->tolerance * 10.0;

    if (!options->have_scan || options->default_scan) {
        return 1;
    }
    return root >= lo - slack && root <= hi + slack;
}

static int solve_root_is_simple_rational(double root, const SolveOptions *options) {
    char rational[96];
    long long nearest_integer = root >= 0.0 ? (long long)(root + 0.5) : (long long)(root - 0.5);
    if (solve_abs(root - (double)nearest_integer) <= options->tolerance * 2.0) {
        return 1;
    }
    return solve_format_rational(root, rational, sizeof(rational)) == 0;
}

static void solve_sort_rat_roots(SolveRat *roots, double *values, int count) {
    int i;
    for (i = 0; i < count; ++i) {
        int j;
        for (j = i + 1; j < count; ++j) {
            if (solve_rat_compare(roots[j], roots[i]) < 0) {
                SolveRat root_temp = roots[i];
                double value_temp = values[i];
                roots[i] = roots[j];
                values[i] = values[j];
                roots[j] = root_temp;
                values[j] = value_temp;
            }
        }
    }
}

static void solve_write_double_value(double value, int scale) {
    char text[96];
    solve_format_compact_decimal(value, scale, text, sizeof(text));
    solve_sp_cstr(1, text);
}

static void solve_explain_rat_poly_line(const char *label, const SolveRatPoly *poly, const SolveOptions *options) {
    char text[SOLVE_EXPR_CAPACITY];
    if (!solve_should_explain(options)) return;
    if (solve_rat_poly_format(poly, options->var_name, text, sizeof(text)) != 0) return;
    rt_write_cstr(1, label);
    rt_write_line(1, text);
}

static void solve_explain_rat_value_line(const char *label, SolveRat value, const SolveOptions *options) {
    if (!solve_should_explain(options)) return;
    rt_write_cstr(1, label);
    solve_write_rat_value(value);
    rt_write_char(1, '\n');
}

static void solve_explain_double_value_line(const char *label, double value, const SolveOptions *options) {
    if (!solve_should_explain(options)) return;
    rt_write_cstr(1, label);
    solve_write_double_value(value, options->scale);
    rt_write_char(1, '\n');
}

static const char *solve_relation_text(SolveRelation relation) {
    switch (relation) {
        case SOLVE_RELATION_LT: return "< 0";
        case SOLVE_RELATION_LE: return "<= 0";
        case SOLVE_RELATION_GT: return "> 0";
        case SOLVE_RELATION_GE: return ">= 0";
        case SOLVE_RELATION_EQ: return "= 0";
        default: return "";
    }
}

static void solve_explain_scan_window_line(const SolveOptions *options) {
    double lo;
    double hi;
    if (!solve_should_explain(options)) return;
    solve_numeric_analysis_bounds(options, &lo, &hi);
    rt_write_cstr(1, "numeric scan window: [");
    solve_write_double_value(lo, options->scale);
    rt_write_cstr(1, ", ");
    solve_write_double_value(hi, options->scale);
    rt_write_line(1, "]");
}

static int solve_get_rat_poly_for_mode(const SolveEquation *equation, const SolveOptions *options, SolveRatPoly *poly_out) {
    if (equation->has_equation && equation->relation != SOLVE_RELATION_EQ) return -1;
    return equation->has_equation ? solve_equation_rat_poly(equation, options, poly_out) : solve_parse_rat_text(equation->left, options->var_name, poly_out);
}

static int solve_rat_sign(SolveRat value) {
    if (value.num < 0) return -1;
    if (value.num > 0) return 1;
    return 0;
}

static void solve_print_labeled_intervals(const char *positive_label, const char *negative_label, const SolveOptions *options, const SolveRatPoly *poly) {
    SolveBreakpoint points[SOLVE_MAX_RESULTS];
    int point_count = 0;
    int segment;
    int degree = solve_rat_poly_degree(poly);

    if (solve_collect_rat_poly_roots(poly, points, &point_count) != 0) return;
    solve_sort_breakpoints(points, &point_count, options->tolerance);
    if (degree < 0) {
        solve_sp_line(1, "constant zero");
        return;
    }
    if (point_count == 0) {
        SolveRat zero;
        SolveRat value;
        (void)solve_rat_make(0, 1, &zero);
        if (solve_rat_poly_eval(poly, degree, zero, &value) != 0) return;
        solve_sp_cstr(1, solve_rat_sign(value) >= 0 ? positive_label : negative_label);
        solve_sp_line(1, " = (-inf, inf)");
        return;
    }
    for (segment = 0; segment <= point_count; ++segment) {
        SolveRat sample;
        SolveRat value;
        if (segment == 0) {
            SolveRat one;
            if (solve_rat_make(1, 1, &one) != 0 || solve_rat_sub(points[0].rat_value, one, &sample) != 0) return;
        } else if (segment == point_count) {
            SolveRat one;
            if (solve_rat_make(1, 1, &one) != 0 || solve_rat_add(points[point_count - 1].rat_value, one, &sample) != 0) return;
        } else {
            SolveRat sum;
            SolveRat two;
            if (solve_rat_add(points[segment - 1].rat_value, points[segment].rat_value, &sum) != 0 || solve_rat_make(2, 1, &two) != 0 || solve_rat_div(sum, two, &sample) != 0) return;
        }
        if (solve_rat_poly_eval(poly, degree, sample, &value) != 0 || solve_rat_sign(value) == 0) continue;
        solve_sp_cstr(1, solve_rat_sign(value) > 0 ? positive_label : negative_label);
        solve_sp_cstr(1, " = ");
        solve_sp_char(1, '(');
        if (segment == 0) solve_sp_cstr(1, "-inf"); else solve_sp_cstr(1, points[segment - 1].label);
        solve_sp_cstr(1, ", ");
        if (segment == point_count) solve_sp_cstr(1, "inf"); else solve_sp_cstr(1, points[segment].label);
        solve_sp_line(1, ")");
    }
}

static void solve_write_poly_end_behavior(const SolveRatPoly *poly) {
    int degree = solve_rat_poly_degree(poly);
    int lead_sign;
    if (degree < 0) {
        solve_sp_line(1, "limit x->-inf: 0");
        solve_sp_line(1, "limit x->inf: 0");
        return;
    }
    lead_sign = solve_rat_sign(poly->coeff[degree]);
    solve_sp_cstr(1, "limit x->-inf: ");
    solve_sp_line(1, (degree % 2 == 0 ? lead_sign : -lead_sign) > 0 ? "+inf" : "-inf");
    solve_sp_cstr(1, "limit x->inf: ");
    solve_sp_line(1, lead_sign > 0 ? "+inf" : "-inf");
}

static int solve_rat_poly_roots_in_range(const SolveRatPoly *poly, SolveRat lo, SolveRat hi, SolveBreakpoint *points, int *count_out) {
    SolveBreakpoint all[SOLVE_MAX_RESULTS];
    int all_count = 0;
    int count = 0;
    int i;
    if (solve_collect_rat_poly_roots(poly, all, &all_count) != 0) return -1;
    solve_sort_breakpoints(all, &all_count, SOLVE_DEFAULT_TOLERANCE);
    for (i = 0; i < all_count; ++i) {
        if (!all[i].exact) continue;
        if (solve_rat_compare(all[i].rat_value, lo) >= 0 && solve_rat_compare(all[i].rat_value, hi) <= 0) points[count++] = all[i];
    }
    *count_out = count;
    return 0;
}

static int solve_add_direct_root(const SolveEquation *equation, const SolveOptions *options, SolveResultSet *set, double root, const char *method) {
    SolveResult result;
    const char *message = 0;

    if (!solve_root_in_scan_range(options, root)) {
        return 0;
    }
    rt_memset(&result, 0, sizeof(result));
    result.root = root;
    result.lo = root;
    result.hi = root;
    result.iterations = 0;
    result.status = SOLVE_STATUS_ROOT;
    result.method = method;
    if (solve_eval_function(equation, options, root, &result.residual, &message) != 0) {
        result.residual = 0.0;
    }
    if (solve_eval_y(equation, options, root, &result.y) != 0) {
        result.y = 0.0;
    }
    return solve_add_result(set, &result, 1, options->tolerance);
}

static void solve_write_linear_term(const SolveOptions *options, double root) {
    char value[96];

    rt_write_cstr(1, "(");
    rt_write_cstr(1, options->var_name);
    if (root < 0.0) {
        rt_write_cstr(1, " + ");
        solve_format_answer(-root, options->scale, value, sizeof(value));
    } else {
        rt_write_cstr(1, " - ");
        solve_format_answer(root, options->scale, value, sizeof(value));
    }
    rt_write_cstr(1, value);
    rt_write_cstr(1, ")");
}

static void solve_explain_identity(const SolveOptions *options, int exact) {
    if (!solve_should_explain(options)) {
        return;
    }
    rt_write_line(1, exact ? "polynomial identity detected" : "approximate polynomial identity detected");
    rt_write_line(1, exact ? "all exact coefficients reduce to 0, so the equation is true for every real x" : "floating-point coefficients reduce to 0 within tolerance, so the equation is numerically true across the supported polynomial form");
}

static void solve_explain_quadratic(const SolveOptions *options, double a, double b, double c, double discriminant, double root1, double root2, const char *method) {
    char value[96];

    if (!solve_should_explain(options)) {
        return;
    }
    rt_write_line(1, "quadratic polynomial detected");
    rt_write_cstr(1, "standard form: a=");
    solve_format_answer(a, options->scale, value, sizeof(value));
    rt_write_cstr(1, value);
    rt_write_cstr(1, " b=");
    solve_format_answer(b, options->scale, value, sizeof(value));
    rt_write_cstr(1, value);
    rt_write_cstr(1, " c=");
    solve_format_answer(c, options->scale, value, sizeof(value));
    rt_write_line(1, value);
    rt_write_cstr(1, "discriminant: ");
    solve_format_answer(discriminant, options->scale, value, sizeof(value));
    rt_write_line(1, value);
    if (discriminant < 0.0) {
        rt_write_line(1, "discriminant < 0, so there are no real roots");
        return;
    }
    rt_write_line(1, "quadratic formula: x = (-b +/- sqrt(discriminant)) / (2a)");
    if (rt_strcmp(method, "factoring") == 0) {
        rt_write_cstr(1, "factor: ");
        if (solve_abs(a - 1.0) > options->tolerance * 2.0) {
            solve_format_answer(a, options->scale, value, sizeof(value));
            rt_write_cstr(1, value);
            rt_write_cstr(1, "*");
        }
        solve_write_linear_term(options, root1);
        if (solve_abs(root1 - root2) <= options->tolerance * 2.0) {
            rt_write_line(1, "^2 = 0");
        } else {
            rt_write_cstr(1, "*");
            solve_write_linear_term(options, root2);
            rt_write_line(1, " = 0");
        }
    }
}

static void solve_explain_higher_polynomial(const SolveOptions *options, int degree, const double *roots, int root_count, int remaining_degree) {
    int i;

    if (!solve_should_explain(options)) {
        return;
    }
    rt_write_line(1, "polynomial factoring detected");
    rt_write_cstr(1, "degree: ");
    rt_write_uint(1, (unsigned long long)degree);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "rational roots: ");
    for (i = 0; i < root_count; ++i) {
        char value[96];
        if (i > 0) {
            rt_write_cstr(1, ", ");
        }
        solve_format_answer(roots[i], options->scale, value, sizeof(value));
        rt_write_cstr(1, value);
    }
    rt_write_char(1, '\n');
    rt_write_cstr(1, "factor: ");
    for (i = 0; i < root_count; ++i) {
        if (i > 0) {
            rt_write_cstr(1, "*");
        }
        solve_write_linear_term(options, roots[i]);
    }
    if (remaining_degree == 2) {
        rt_write_cstr(1, "*(remaining quadratic)");
    } else if (remaining_degree == 1) {
        rt_write_cstr(1, "*(remaining linear factor)");
    }
    rt_write_line(1, " = 0");
}

static int solve_try_rational_quadratic(const SolveEquation *equation, const SolveOptions *options, SolveResultSet *set, const SolveRatPoly *poly, int explain) {
    SolveRat a = poly->coeff[2];
    SolveRat b = poly->coeff[1];
    SolveRat c = poly->coeff[0];
    SolveRat b_squared;
    SolveRat four;
    SolveRat ac;
    SolveRat four_ac;
    SolveRat discriminant;
    SolveRat sqrt_discriminant;
    SolveRat minus_b;
    SolveRat two_a;
    SolveRat root1;
    SolveRat root2;
    int rc;

    if (solve_rat_is_zero(a)) return -1;
    if (solve_rat_mul(b, b, &b_squared) != 0 || solve_rat_make(4, 1, &four) != 0 ||
        solve_rat_mul(a, c, &ac) != 0 || solve_rat_mul(four, ac, &four_ac) != 0 ||
        solve_rat_sub(b_squared, four_ac, &discriminant) != 0) {
        return -1;
    }
    if (explain) {
        double da = solve_rat_to_double(a);
        double db = solve_rat_to_double(b);
        double dc = solve_rat_to_double(c);
        double dd = solve_rat_to_double(discriminant);
        double sr = dd < 0.0 ? 0.0 : solve_sqrt(dd);
        double r1 = dd < 0.0 ? 0.0 : (-db - sr) / (2.0 * da);
        double r2 = dd < 0.0 ? 0.0 : (-db + sr) / (2.0 * da);
        solve_explain_quadratic(options, da, db, dc, dd, r1 < r2 ? r1 : r2, r1 < r2 ? r2 : r1, solve_rat_sqrt_exact(discriminant, &sqrt_discriminant) == 0 ? "factoring" : "quadratic-formula");
    }
    if (discriminant.num < 0) {
        set->no_real_solutions = 1;
        return 1;
    }
    if (solve_rat_sqrt_exact(discriminant, &sqrt_discriminant) == 0) {
        if (solve_rat_neg(b, &minus_b) != 0 || solve_rat_make_i128((__int128)2 * a.num, a.den, &two_a) != 0) return -1;
        if (solve_rat_sub(minus_b, sqrt_discriminant, &root1) != 0 || solve_rat_div(root1, two_a, &root1) != 0) return -1;
        if (solve_rat_add(minus_b, sqrt_discriminant, &root2) != 0 || solve_rat_div(root2, two_a, &root2) != 0) return -1;
        if (solve_rat_compare(root2, root1) < 0) {
            SolveRat temp = root1;
            root1 = root2;
            root2 = temp;
        }
        rc = solve_add_direct_rat_root(equation, options, set, root1, "factoring");
        if (rc > 0) return 1;
        if (solve_rat_compare(root1, root2) != 0) {
            rc = solve_add_direct_rat_root(equation, options, set, root2, "factoring");
            if (rc > 0) return 1;
        }
    } else {
        double da = solve_rat_to_double(a);
        double db = solve_rat_to_double(b);
        double dd = solve_rat_to_double(discriminant);
        double root_low = (-db - solve_sqrt(dd)) / (2.0 * da);
        double root_high = (-db + solve_sqrt(dd)) / (2.0 * da);
        if (root_high < root_low) {
            double temp = root_low;
            root_low = root_high;
            root_high = temp;
        }
        rc = solve_add_direct_approx_root(equation, options, set, root_low, "quadratic-formula");
        if (rc > 0) return 1;
        if (solve_abs(root_low - root_high) > options->tolerance * 2.0) {
            rc = solve_add_direct_approx_root(equation, options, set, root_high, "quadratic-formula");
            if (rc > 0) return 1;
        }
    }
    return set->count > 0U ? 1 : 0;
}

static int solve_try_rational_polynomial(const SolveEquation *equation, const SolveOptions *options, SolveResultSet *set) {
    SolveRatPoly poly;
    SolveRatPoly reduced;
    SolveRat rational_roots[SOLVE_RAT_POLY_MAX_DEGREE];
    double root_values[SOLVE_RAT_POLY_MAX_DEGREE];
    int rational_root_count = 0;
    int degree;
    int original_degree;

    if (rt_strcmp(options->method, "auto") != 0) return 0;
    if (solve_equation_rat_poly(equation, options, &poly) != 0) return 0;
    degree = solve_rat_poly_degree(&poly);
    if (degree < 0) {
        set->identity = 1;
        solve_explain_identity(options, 1);
        return 1;
    }
    if (degree == 0) {
        return 1;
    }
    original_degree = degree;
    if (degree == 1) {
        SolveRat root;
        if (solve_rat_is_zero(poly.coeff[1])) return 0;
        if (solve_rat_neg(poly.coeff[0], &root) != 0 || solve_rat_div(root, poly.coeff[1], &root) != 0) return 0;
        solve_explain_linear(equation, options, solve_rat_to_double(poly.coeff[1]), solve_rat_to_double(poly.coeff[0]), solve_rat_to_double(root));
        (void)solve_add_direct_rat_root(equation, options, set, root, "linear");
        return set->count > 0U ? 1 : 0;
    }
    reduced = poly;
    while (original_degree > 2 && degree > 0) {
        SolveRat root;
        SolveRatPoly quotient;
        if (solve_find_exact_rational_root(&reduced, degree, &root) != 0) break;
        if (solve_rat_poly_divide_linear(&reduced, degree, root, &quotient) != 0) return 0;
        rational_roots[rational_root_count] = root;
        root_values[rational_root_count] = solve_rat_to_double(root);
        rational_root_count += 1;
        reduced = quotient;
        degree -= 1;
    }
    if (original_degree > 2) {
        int root_index;
        if (rational_root_count == 0) return 0;
        solve_sort_rat_roots(rational_roots, root_values, rational_root_count);
        solve_explain_higher_polynomial(options, original_degree, root_values, rational_root_count, degree);
        for (root_index = 0; root_index < rational_root_count; ++root_index) {
            int rc = solve_add_direct_rat_root(equation, options, set, rational_roots[root_index], "polynomial-factoring");
            if (rc > 0) return 1;
        }
        poly = reduced;
        if (degree == 0) {
            return set->count > 0U ? 1 : 0;
        }
    }
    if (degree == 2) {
        return solve_try_rational_quadratic(equation, options, set, &poly, original_degree <= 2);
    }
    if (degree == 1) {
        SolveRat root;
        if (solve_rat_is_zero(poly.coeff[1])) return set->count > 0U ? 1 : 0;
        if (solve_rat_neg(poly.coeff[0], &root) != 0 || solve_rat_div(root, poly.coeff[1], &root) != 0) return set->count > 0U ? 1 : 0;
        (void)solve_add_direct_rat_root(equation, options, set, root, original_degree > 2 ? "polynomial-factoring" : "linear");
        return set->count > 0U ? 1 : 0;
    }
    return set->count > 0U ? 1 : 0;
}

static int solve_try_polynomial(const SolveEquation *equation, const SolveOptions *options, SolveResultSet *set) {
    SolvePoly poly;
    SolvePoly reduced;
    int degree;
    int original_degree;
    double rational_roots[SOLVE_POLY_MAX_DEGREE];
    int rational_root_count = 0;

    if (rt_strcmp(options->method, "auto") != 0) {
        return 0;
    }
    if (solve_equation_poly(equation, options, &poly) != 0) {
        return 0;
    }
    degree = solve_poly_degree(&poly, options->tolerance * 10.0);
    if (degree < 0) {
        set->identity = poly.exact ? 1 : 2;
        solve_explain_identity(options, poly.exact);
        return 1;
    }
    original_degree = degree;
    if (degree == 1) {
        double slope = poly.coeff[1];
        double intercept = poly.coeff[0];
        double root;
        if (solve_abs(slope) <= options->tolerance) {
            return 0;
        }
        root = -intercept / slope;
        solve_explain_linear(equation, options, slope, intercept, root);
        (void)solve_add_direct_root(equation, options, set, root, "linear");
        return set->count > 0U ? 1 : 0;
    }
    reduced = poly;
    while (degree > 2 && reduced.exact) {
        SolvePoly quotient;
        double root;
        if (solve_find_rational_poly_root(&reduced, degree, options, &root) != 0) {
            break;
        }
        if (solve_poly_divide_linear(&reduced, degree, root, &quotient, options->tolerance * 1000.0) != 0) {
            break;
        }
        rational_roots[rational_root_count++] = root;
        reduced = quotient;
        reduced.exact = 1;
        degree -= 1;
    }
    if (original_degree > 2) {
        int root_index;
        if (rational_root_count == 0) {
            return 0;
        }
        solve_explain_higher_polynomial(options, original_degree, rational_roots, rational_root_count, degree);
        for (root_index = 0; root_index < rational_root_count; ++root_index) {
            int rc = solve_add_direct_root(equation, options, set, rational_roots[root_index], "polynomial-factoring");
            if (rc > 0) return 1;
        }
        poly = reduced;
    }
    if (degree == 2) {
        double a = poly.coeff[2];
        double b = poly.coeff[1];
        double c = poly.coeff[0];
        double discriminant = b * b - 4.0 * a * c;
        double root1;
        double root2;
        const char *method;
        int rc;

        if (solve_abs(a) <= options->tolerance || discriminant < -options->tolerance * 10.0) {
            return set->count > 0U ? 1 : 0;
        }
        if (discriminant < 0.0) {
            discriminant = 0.0;
        }
        root1 = (-b - solve_sqrt(discriminant)) / (2.0 * a);
        root2 = (-b + solve_sqrt(discriminant)) / (2.0 * a);
        if (root2 < root1) {
            double temp = root1;
            root1 = root2;
            root2 = temp;
        }
        method = solve_root_is_simple_rational(root1, options) && solve_root_is_simple_rational(root2, options) ? "factoring" : "quadratic-formula";
        if (original_degree <= 2) {
            solve_explain_quadratic(options, a, b, c, discriminant, root1, root2, method);
        }
        rc = solve_add_direct_root(equation, options, set, root1, method);
        if (rc > 0) return 1;
        if (solve_abs(root1 - root2) > options->tolerance * 2.0) {
            rc = solve_add_direct_root(equation, options, set, root2, method);
            if (rc > 0) return 1;
        }
        return set->count > 0U ? 1 : 0;
    }
    if (degree == 1) {
        double slope = poly.coeff[1];
        double intercept = poly.coeff[0];
        double root;
        if (solve_abs(slope) <= options->tolerance) {
            return set->count > 0U ? 1 : 0;
        }
        root = -intercept / slope;
        (void)solve_add_direct_root(equation, options, set, root, original_degree > 2 ? "polynomial-factoring" : "linear");
        return set->count > 0U ? 1 : 0;
    }
    return 0;
}


#endif
