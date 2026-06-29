#if !defined(SOLVE_FROM_SOLVE_C)
#ifndef SOLVE_PRIVATE_CONTEXT
#define SOLVE_PRIVATE_CONTEXT 1
#endif
#include "../solve.c"
#else
static int solve_relation_satisfied(double value, SolveRelation relation, double tolerance) {
    switch (relation) {
        case SOLVE_RELATION_LT: return value < -tolerance;
        case SOLVE_RELATION_LE: return value <= tolerance;
        case SOLVE_RELATION_GT: return value > tolerance;
        case SOLVE_RELATION_GE: return value >= -tolerance;
        default: return 0;
    }
}

static int solve_relation_is_inclusive(SolveRelation relation) {
    return relation == SOLVE_RELATION_LE || relation == SOLVE_RELATION_GE;
}

static void solve_sort_breakpoints(SolveBreakpoint *points, int *count_io, double tolerance) {
    int count = *count_io;
    int i;
    for (i = 0; i < count; ++i) {
        int j;
        for (j = i + 1; j < count; ++j) {
            if (points[j].value < points[i].value) {
                SolveBreakpoint temp = points[i];
                points[i] = points[j];
                points[j] = temp;
            }
        }
    }
    for (i = 0; i < count; ++i) {
        int out_count;
        if (i == 0 || solve_abs(points[i].value - points[i - 1].value) > tolerance * 10.0) continue;
        points[i - 1].pole = points[i - 1].pole || points[i].pole;
        for (out_count = i; out_count + 1 < count; ++out_count) points[out_count] = points[out_count + 1];
        count -= 1;
        i -= 1;
    }
    *count_io = count;
}

static void solve_write_interval_endpoint(const char *label, double value, int has_endpoint) {
    char text[96];
    if (!has_endpoint) {
        solve_sp_cstr(1, value < 0.0 ? "-inf" : "inf");
    } else if (label[0] != '\0') {
        solve_sp_cstr(1, label);
    } else {
        solve_format_compact_decimal(value, SOLVE_DEFAULT_SCALE, text, sizeof(text));
        solve_sp_cstr(1, text);
    }
}

static void solve_print_intervals(const SolveOptions *options, const SolveInterval *intervals, int count, int bounded) {
    int i;
    (void)options;
    if (count == 1 && !intervals[0].has_left && !intervals[0].has_right) {
        solve_sp_line(1, "solution = all real x");
        return;
    }
    solve_sp_cstr(1, bounded ? "solution (within scan range) = " : "solution = ");
    for (i = 0; i < count; ++i) {
        if (i > 0) solve_sp_cstr(1, " U ");
        solve_sp_char(1, intervals[i].left_closed ? '[' : '(');
        solve_write_interval_endpoint(intervals[i].left_label, intervals[i].left, intervals[i].has_left);
        solve_sp_cstr(1, ", ");
        solve_write_interval_endpoint(intervals[i].right_label, intervals[i].right, intervals[i].has_right);
        solve_sp_char(1, intervals[i].right_closed ? ']' : ')');
    }
    solve_sp_char(1, '\n');
}

static int solve_add_interval(SolveInterval *intervals, int *count_io, const SolveInterval *interval) {
    int count = *count_io;
    if (count > 0 && intervals[count - 1].has_right && interval->has_left && solve_abs(intervals[count - 1].right - interval->left) <= SOLVE_DEFAULT_TOLERANCE * 10.0 && intervals[count - 1].right_closed && interval->left_closed) {
        intervals[count - 1].has_right = interval->has_right;
        intervals[count - 1].right = interval->right;
        intervals[count - 1].right_closed = interval->right_closed;
        rt_copy_string(intervals[count - 1].right_label, sizeof(intervals[count - 1].right_label), interval->right_label);
        return 0;
    }
    if (count >= (int)SOLVE_MAX_RESULTS) return -1;
    intervals[count] = *interval;
    *count_io = count + 1;
    return 0;
}

static int solve_collect_rat_poly_roots(const SolveRatPoly *input, SolveBreakpoint *points, int *count_out) {
    SolveRatPoly poly = *input;
    int degree = solve_rat_poly_degree(&poly);
    int count = 0;
    while (degree > 2) {
        SolveRat root;
        SolveRatPoly quotient;
        if (solve_find_exact_rational_root(&poly, degree, &root) != 0) break;
        if (solve_rat_poly_divide_linear(&poly, degree, root, &quotient) != 0) return -1;
        points[count].value = solve_rat_to_double(root);
        points[count].rat_value = root;
        points[count].exact = 1;
        points[count].pole = 0;
        if (solve_rat_format(root, points[count].label, sizeof(points[count].label)) != 0) return -1;
        count += 1;
        poly = quotient;
        degree -= 1;
    }
    if (degree == 2) {
        SolveRat a = poly.coeff[2];
        SolveRat b = poly.coeff[1];
        SolveRat c = poly.coeff[0];
        SolveRat disc;
        SolveRat four;
        SolveRat temp;
        SolveRat sqrt_disc;
        if (solve_rat_make(4, 1, &four) != 0 || solve_rat_mul(a, c, &temp) != 0 || solve_rat_mul(four, temp, &temp) != 0) return -1;
        if (solve_rat_mul(b, b, &disc) != 0 || solve_rat_sub(disc, temp, &disc) != 0) return -1;
        if (disc.num > 0) {
            SolveRat minus_b;
            SolveRat two_a;
            if (solve_rat_neg(b, &minus_b) != 0 || solve_rat_make_i128((__int128)2 * a.num, a.den, &two_a) != 0) return -1;
            if (solve_rat_sqrt_exact(disc, &sqrt_disc) == 0) {
                SolveRat roots[2];
                int i;
                if (solve_rat_sub(minus_b, sqrt_disc, &roots[0]) != 0 || solve_rat_div(roots[0], two_a, &roots[0]) != 0) return -1;
                if (solve_rat_add(minus_b, sqrt_disc, &roots[1]) != 0 || solve_rat_div(roots[1], two_a, &roots[1]) != 0) return -1;
                for (i = 0; i < 2; ++i) {
                    points[count].value = solve_rat_to_double(roots[i]);
                    points[count].rat_value = roots[i];
                    points[count].exact = 1;
                    points[count].pole = 0;
                    if (solve_rat_format(roots[i], points[count].label, sizeof(points[count].label)) != 0) return -1;
                    count += 1;
                }
            } else {
                double da = solve_rat_to_double(a);
                double db = solve_rat_to_double(b);
                double dd = solve_rat_to_double(disc);
                double root1 = (-db - solve_sqrt(dd)) / (2.0 * da);
                double root2 = (-db + solve_sqrt(dd)) / (2.0 * da);
                solve_format_compact_decimal(root1, SOLVE_DEFAULT_SCALE, points[count].label, sizeof(points[count].label));
                points[count].value = root1; points[count].exact = 0; points[count].pole = 0; (void)solve_rat_make(0, 1, &points[count].rat_value); count += 1;
                solve_format_compact_decimal(root2, SOLVE_DEFAULT_SCALE, points[count].label, sizeof(points[count].label));
                points[count].value = root2; points[count].exact = 0; points[count].pole = 0; (void)solve_rat_make(0, 1, &points[count].rat_value); count += 1;
            }
        } else if (disc.num == 0) {
            SolveRat root;
            SolveRat minus_b;
            SolveRat two_a;
            if (solve_rat_neg(b, &minus_b) != 0 || solve_rat_make_i128((__int128)2 * a.num, a.den, &two_a) != 0 || solve_rat_div(minus_b, two_a, &root) != 0) return -1;
            points[count].value = solve_rat_to_double(root);
            points[count].rat_value = root;
            points[count].exact = 1;
            points[count].pole = 0;
            if (solve_rat_format(root, points[count].label, sizeof(points[count].label)) != 0) return -1;
            count += 1;
        }
    } else if (degree == 1) {
        SolveRat root;
        if (solve_rat_neg(poly.coeff[0], &root) != 0 || solve_rat_div(root, poly.coeff[1], &root) != 0) return -1;
        points[count].value = solve_rat_to_double(root);
        points[count].rat_value = root;
        points[count].exact = 1;
        points[count].pole = 0;
        if (solve_rat_format(root, points[count].label, sizeof(points[count].label)) != 0) return -1;
        count += 1;
    }
    *count_out = count;
    return 0;
}

static int solve_run_exact_poly_inequality(const SolveEquation *equation, const SolveOptions *options, const SolveRatPoly *poly) {
    SolveBreakpoint points[SOLVE_MAX_RESULTS];
    SolveInterval intervals[SOLVE_MAX_RESULTS];
    int point_count = 0;
    int interval_count = 0;
    int degree = solve_rat_poly_degree(poly);
    int inclusive = solve_relation_is_inclusive(equation->relation);
    int segment;

    if (solve_should_explain(options)) {
        solve_explain_working_function("inequality", equation, options);
        solve_explain_rat_poly_line("zero-function polynomial: ", poly, options);
        solve_sp_cstr(1, "target sign: f(x) ");
        solve_sp_line(1, solve_relation_text(equation->relation));
        solve_sp_line(1, "method detail: exact roots split the real line; one exact test value decides each interval");
    }

    if (degree < 0) {
        if (solve_relation_satisfied(0.0, equation->relation, 0.0)) {
            solve_sp_line(1, "solution = all real x");
            return 0;
        }
        solve_sp_line(1, "solution = empty set");
        return 1;
    }
    if (solve_collect_rat_poly_roots(poly, points, &point_count) != 0) return -1;
    solve_sort_breakpoints(points, &point_count, options->tolerance);
    if (solve_should_explain(options)) solve_print_rat_roots_line("boundary roots:", points, point_count);
    if (point_count == 0) {
        SolveRat zero;
        SolveRat value;
        (void)solve_rat_make(0, 1, &zero);
        if (solve_rat_poly_eval(poly, degree, zero, &value) != 0) return -1;
        if (solve_relation_satisfied(solve_rat_to_double(value), equation->relation, 0.0)) {
            solve_sp_line(1, "solution = all real x");
            return 0;
        }
        solve_sp_line(1, "solution = empty set");
        return 1;
    }
    for (segment = 0; segment <= point_count; ++segment) {
        SolveRat sample_rat;
        SolveRat value;
        SolveInterval interval;
        if ((segment > 0 && !points[segment - 1].exact) || (segment < point_count && !points[segment].exact)) return -1;
        if (segment == 0) {
            SolveRat one;
            if (solve_rat_make(1, 1, &one) != 0 || solve_rat_sub(points[0].rat_value, one, &sample_rat) != 0) return -1;
        } else if (segment == point_count) {
            SolveRat one;
            if (solve_rat_make(1, 1, &one) != 0 || solve_rat_add(points[point_count - 1].rat_value, one, &sample_rat) != 0) return -1;
        } else {
            SolveRat sum;
            SolveRat two;
            if (solve_rat_add(points[segment - 1].rat_value, points[segment].rat_value, &sum) != 0 || solve_rat_make(2, 1, &two) != 0 || solve_rat_div(sum, two, &sample_rat) != 0) return -1;
        }
        if (solve_rat_poly_eval(poly, degree, sample_rat, &value) != 0) return -1;
        if (!solve_relation_satisfied(solve_rat_to_double(value), equation->relation, 0.0)) continue;
        rt_memset(&interval, 0, sizeof(interval));
        interval.has_left = segment > 0;
        interval.has_right = segment < point_count;
        if (interval.has_left) {
            interval.left = points[segment - 1].value;
            interval.left_closed = inclusive;
            rt_copy_string(interval.left_label, sizeof(interval.left_label), points[segment - 1].label);
        } else {
            interval.left = -1.0;
        }
        if (interval.has_right) {
            interval.right = points[segment].value;
            interval.right_closed = inclusive;
            rt_copy_string(interval.right_label, sizeof(interval.right_label), points[segment].label);
        } else {
            interval.right = 1.0;
        }
        if (solve_add_interval(intervals, &interval_count, &interval) != 0) return -1;
    }
    if (interval_count == 0) {
        solve_sp_line(1, "solution = empty set");
        return 1;
    }
    solve_print_intervals(options, intervals, interval_count, 0);
    return 0;
}

static int solve_run_numeric_inequality(const SolveEquation *equation, const SolveOptions *options) {
    SolveEquation zero_equation = *equation;
    SolveOptions scan_options = *options;
    SolveResultSet roots;
    SolveBreakpoint points[SOLVE_MAX_RESULTS];
    SolveInterval intervals[SOLVE_MAX_RESULTS];
    int point_count = 0;
    int interval_count = 0;
    double lo = options->scan_lo < options->scan_hi ? options->scan_lo : options->scan_hi;
    double hi = options->scan_lo < options->scan_hi ? options->scan_hi : options->scan_lo;
    double step = (hi - lo) / (double)options->scan_steps;
    int i;
    const char *message = 0;

    if (solve_should_explain(options)) {
        solve_explain_working_function("numeric inequality", equation, options);
        solve_sp_cstr(1, "target sign: f(x) ");
        solve_sp_line(1, solve_relation_text(equation->relation));
        solve_explain_scan_window_line(options);
        solve_sp_line(1, "method detail: scan for zero crossings and invalid sample points, then test each interval midpoint");
    }

    zero_equation.relation = SOLVE_RELATION_EQ;
    scan_options.all = 1;
    rt_memset(&roots, 0, sizeof(roots));
    solve_scan(&zero_equation, &scan_options, &roots);
    for (i = 0; i < (int)roots.count && point_count < (int)SOLVE_MAX_RESULTS; ++i) {
        points[point_count].value = roots.results[i].root;
        points[point_count].exact = roots.results[i].exact_value[0] != '\0';
        points[point_count].pole = 0;
        (void)solve_rat_make(0, 1, &points[point_count].rat_value);
        solve_format_compact_decimal(points[point_count].value, options->scale, points[point_count].label, sizeof(points[point_count].label));
        point_count += 1;
    }
    for (i = 0; i <= options->scan_steps && point_count < (int)SOLVE_MAX_RESULTS; ++i) {
        double x = lo + step * (double)i;
        double y;
        if (i == options->scan_steps) x = hi;
        if (solve_eval_function(equation, options, x, &y, &message) != 0) {
            points[point_count].value = x;
            (void)solve_rat_make(0, 1, &points[point_count].rat_value);
            points[point_count].pole = 1;
            points[point_count].exact = 0;
            solve_format_compact_decimal(x, options->scale, points[point_count].label, sizeof(points[point_count].label));
            point_count += 1;
        }
    }
    solve_sort_breakpoints(points, &point_count, options->tolerance);
    for (i = 0; i <= point_count; ++i) {
        double left = i == 0 ? lo : points[i - 1].value;
        double right = i == point_count ? hi : points[i].value;
        double sample = (left + right) * 0.5;
        double value;
        SolveInterval interval;
        if (right <= left || solve_eval_function(equation, options, sample, &value, &message) != 0) continue;
        if (!solve_relation_satisfied(value, equation->relation, options->tolerance)) continue;
        rt_memset(&interval, 0, sizeof(interval));
        interval.has_left = 1;
        interval.has_right = 1;
        interval.left = left;
        interval.right = right;
        interval.left_closed = i == 0 || (!points[i - 1].pole && solve_relation_is_inclusive(equation->relation));
        interval.right_closed = i == point_count || (!points[i].pole && solve_relation_is_inclusive(equation->relation));
        if (i == 0) solve_format_compact_decimal(left, options->scale, interval.left_label, sizeof(interval.left_label));
        else rt_copy_string(interval.left_label, sizeof(interval.left_label), points[i - 1].label);
        if (i == point_count) solve_format_compact_decimal(right, options->scale, interval.right_label, sizeof(interval.right_label));
        else rt_copy_string(interval.right_label, sizeof(interval.right_label), points[i].label);
        if (solve_add_interval(intervals, &interval_count, &interval) != 0) return 3;
    }
    if (interval_count == 0) {
        solve_sp_line(1, "solution = empty set");
        return 1;
    }
    solve_print_intervals(options, intervals, interval_count, 1);
    return 0;
}

static int solve_run_inequality_mode(const SolveEquation *equation, const SolveOptions *options) {
    SolveRatPoly poly;
    if (solve_equation_rat_poly(equation, options, &poly) == 0) {
        int rc = solve_run_exact_poly_inequality(equation, options, &poly);
        if (rc >= 0) return rc;
    }
    return solve_run_numeric_inequality(equation, options);
}


#endif
