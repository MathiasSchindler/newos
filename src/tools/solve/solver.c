#if !defined(SOLVE_FROM_SOLVE_C)
#ifndef SOLVE_PRIVATE_CONTEXT
#define SOLVE_PRIVATE_CONTEXT 1
#endif
#include "../solve.c"
#else
static int solve_add_result(SolveResultSet *set, const SolveResult *result, int all, double tolerance) {
    size_t index;
    double duplicate_window = tolerance < 0.00000001 ? 0.00000001 : tolerance * 10.0;

    for (index = 0U; index < set->count; ++index) {
        if (solve_abs(set->results[index].root - result->root) <= duplicate_window) {
            if (result->status == SOLVE_STATUS_ROOT && set->results[index].status != SOLVE_STATUS_ROOT) {
                set->results[index] = *result;
            }
            return 0;
        }
    }
    if (set->count >= SOLVE_MAX_RESULTS) {
        return -1;
    }
    set->results[set->count++] = *result;
    return all ? 0 : 1;
}

static void solve_keep_preferred_scan_result(SolveResultSet *set) {
    size_t index;
    size_t best = 0U;

    if (set->count <= 1U) {
        return;
    }
    for (index = 1U; index < set->count; ++index) {
        double best_abs;
        double current_abs;
        if (set->results[index].status == SOLVE_STATUS_ROOT && set->results[best].status != SOLVE_STATUS_ROOT) {
            best = index;
            continue;
        }
        if (set->results[index].status != set->results[best].status) {
            continue;
        }
        best_abs = solve_abs(set->results[best].root);
        current_abs = solve_abs(set->results[index].root);
        if (current_abs < best_abs || (solve_abs(current_abs - best_abs) <= 0.00000001 && set->results[index].root < set->results[best].root)) {
            best = index;
        }
    }
    if (best != 0U) {
        set->results[0] = set->results[best];
    }
    set->count = 1U;
}

static void solve_explain_start(const SolveEquation *equation, const SolveOptions *options) {
    if (!solve_should_explain(options)) {
        return;
    }
    rt_write_cstr(1, "function: f(");
    rt_write_cstr(1, options->var_name);
    rt_write_cstr(1, ") = (");
    rt_write_cstr(1, equation->left);
    rt_write_cstr(1, ") - (");
    rt_write_cstr(1, equation->right);
    rt_write_line(1, ")");
}

static void solve_explain_step(const SolveOptions *options, int iteration, double lo, double hi, double mid, double fmid) {
    char buffer[64];

    if (!solve_should_explain(options)) {
        return;
    }
    rt_write_cstr(1, "step ");
    rt_write_uint(1, (unsigned long long)iteration);
    rt_write_cstr(1, ": lo=");
    solve_format_double(lo, options->scale, buffer, sizeof(buffer));
    rt_write_cstr(1, buffer);
    rt_write_cstr(1, " hi=");
    solve_format_double(hi, options->scale, buffer, sizeof(buffer));
    rt_write_cstr(1, buffer);
    rt_write_cstr(1, " mid=");
    solve_format_double(mid, options->scale, buffer, sizeof(buffer));
    rt_write_cstr(1, buffer);
    rt_write_cstr(1, " f(mid)=");
    solve_format_double(fmid, options->scale, buffer, sizeof(buffer));
    rt_write_line(1, buffer);
}

static int solve_bisect(const SolveEquation *equation, const SolveOptions *options, double lo, double hi, SolveResult *result) {
    double flo;
    double fhi;
    double mid = lo;
    double fmid = 0.0;
    const char *message = 0;
    int iteration;
    int exact_sample = 0;

    rt_memset(result, 0, sizeof(*result));
    if (solve_eval_function(equation, options, lo, &flo, &message) != 0 || solve_eval_function(equation, options, hi, &fhi, &message) != 0) {
        return -1;
    }
    if (solve_abs(flo) <= options->tolerance) {
        mid = lo;
        fmid = flo;
        iteration = 0;
        exact_sample = flo == 0.0;
    } else if (solve_abs(fhi) <= options->tolerance) {
        mid = hi;
        fmid = fhi;
        iteration = 0;
        exact_sample = fhi == 0.0;
    } else if ((flo < 0.0 && fhi < 0.0) || (flo > 0.0 && fhi > 0.0)) {
        return -1;
    } else {
        for (iteration = 1; iteration <= options->max_iterations; ++iteration) {
            mid = (lo + hi) * 0.5;
            if (solve_eval_function(equation, options, mid, &fmid, &message) != 0) {
                return -2;
            }
            solve_explain_step(options, iteration, lo, hi, mid, fmid);
            if (solve_abs(fmid) <= options->tolerance || solve_abs(hi - lo) <= options->tolerance) {
                break;
            }
            if ((flo < 0.0 && fmid < 0.0) || (flo > 0.0 && fmid > 0.0)) {
                lo = mid;
                flo = fmid;
            } else {
                hi = mid;
                fhi = fmid;
            }
        }
    }

    result->root = mid;
    result->residual = fmid;
    result->lo = lo;
    result->hi = hi;
    result->iterations = iteration;
    result->status = solve_abs(fmid) <= options->tolerance ? SOLVE_STATUS_ROOT : SOLVE_STATUS_SUSPECT_DISCONTINUITY;
    result->method = exact_sample ? "exact-sample" : "bisection";
    result->approximate = !exact_sample;
    if (solve_eval_y(equation, options, mid, &result->y) != 0) {
        result->y = 0.0;
    }
    return 0;
}

static int solve_scan(const SolveEquation *equation, const SolveOptions *options, SolveResultSet *set) {
    double lo = options->scan_lo;
    double hi = options->scan_hi;
    double step = (hi - lo) / (double)options->scan_steps;
    double prev_x = lo;
    double prev_value = 0.0;
    double curr_x = lo;
    double curr_value = 0.0;
    double next_x;
    double next_value;
    int prev_ok;
    int curr_ok;
    int i;
    const char *message = 0;
    double touch_tolerance = options->tolerance < 0.000001 ? 0.000001 : options->tolerance * 1000.0;

    if (options->explain && !tool_json_is_enabled() && !options->quiet) {
        char buffer[64];
        rt_write_cstr(1, "scan: ");
        solve_format_double(lo, options->scale, buffer, sizeof(buffer));
        rt_write_cstr(1, buffer);
        rt_write_cstr(1, " to ");
        solve_format_double(hi, options->scale, buffer, sizeof(buffer));
        rt_write_cstr(1, buffer);
        rt_write_cstr(1, " in ");
        rt_write_uint(1, (unsigned long long)options->scan_steps);
        rt_write_line(1, " steps");
    }

    prev_ok = solve_eval_function(equation, options, prev_x, &prev_value, &message) == 0;
    curr_ok = prev_ok;
    curr_value = prev_value;
    for (i = 1; i <= options->scan_steps; ++i) {
        SolveResult result;
        next_x = lo + step * (double)i;
        if (i == options->scan_steps) {
            next_x = hi;
        }
        if (solve_eval_function(equation, options, next_x, &next_value, &message) != 0) {
            prev_x = next_x;
            prev_ok = 0;
            curr_ok = 0;
            continue;
        }
        if (prev_ok && ((prev_value <= 0.0 && next_value >= 0.0) || (prev_value >= 0.0 && next_value <= 0.0))) {
            int rc = solve_bisect(equation, options, prev_x, next_x, &result);
            if (rc == 0) {
                (void)solve_add_result(set, &result, 1, options->tolerance);
            } else if (rc == -2) {
                set->suspected_discontinuity = 1;
                if (options->explain && !tool_json_is_enabled() && !options->quiet) {
                    rt_write_line(1, "suspected discontinuity: sign change did not converge to a root with a valid residual");
                }
            }
        } else if (i > 1 && curr_ok && solve_abs(curr_value) <= touch_tolerance && solve_abs(curr_value) <= solve_abs(prev_value) && solve_abs(curr_value) <= solve_abs(next_value)) {
            rt_memset(&result, 0, sizeof(result));
            result.root = curr_x;
            result.residual = curr_value;
            result.lo = curr_x;
            result.hi = curr_x;
            result.iterations = 0;
            result.status = solve_abs(curr_value) <= options->tolerance ? SOLVE_STATUS_ROOT : SOLVE_STATUS_CANDIDATE;
            result.method = curr_value == 0.0 ? "exact-sample" : "scan";
            if (solve_eval_y(equation, options, curr_x, &result.y) != 0) {
                result.y = 0.0;
            }
            (void)solve_add_result(set, &result, 1, options->tolerance);
        }
        prev_x = next_x;
        prev_value = next_value;
        prev_ok = 1;
        curr_x = next_x;
        curr_value = next_value;
        curr_ok = 1;
    }
    if (!options->all) {
        solve_keep_preferred_scan_result(set);
    }
    return 0;
}

static int solve_explicit_bracket(const SolveEquation *equation, const SolveOptions *options, SolveResultSet *set) {
    SolveResult result;
    int rc = solve_bisect(equation, options, options->lo, options->hi, &result);

    if (rc == 0) {
        (void)solve_add_result(set, &result, 1, options->tolerance);
        return 0;
    }
    if (rc == -2) {
        set->suspected_discontinuity = 1;
        if (options->explain && !tool_json_is_enabled() && !options->quiet) {
            rt_write_line(1, "suspected discontinuity: interval sign change did not converge to a root with a valid residual");
        }
    }
    return 0;
}

static double solve_display_residual(const SolveEquation *equation, const SolveOptions *options, const SolveResult *result, const char *display_value) {
    size_t pos = 0U;
    double display_root;
    double residual;
    const char *message = 0;

    if (!result->approximate || solve_parse_double(display_value, &pos, &display_root) != 0) {
        return result->residual;
    }
    if (solve_eval_function(equation, options, display_root, &residual, &message) != 0) {
        return result->residual;
    }
    return residual;
}

static int solve_write_json_result(const SolveEquation *equation, const SolveOptions *options, const SolveResult *result) {
    char value[96];
    const char *root_text;
    double residual;

    if (tool_json_begin_event(1, "solve", "stdout", result->status == SOLVE_STATUS_CANDIDATE ? "solve_candidate" : "solve_result") != 0) return -1;
    rt_write_cstr(1, ",\"data\":{\"variable\":");
    tool_json_write_string(1, options->var_name);
    rt_write_cstr(1, ",\"root\":");
    if (result->exact_value[0] != '\0') {
        root_text = result->exact_value;
    } else {
        solve_format_double(result->root, options->scale, value, sizeof(value));
        root_text = value;
    }
    tool_json_write_string(1, root_text);
    if (options->report_y) {
        rt_write_cstr(1, ",\"y\":");
        solve_format_double(result->y, options->scale, value, sizeof(value));
        tool_json_write_string(1, value);
    }
    rt_write_cstr(1, ",\"residual\":");
    residual = solve_display_residual(equation, options, result, root_text);
    solve_format_double(residual, result->approximate ? SOLVE_MAX_SCALE : options->scale, value, sizeof(value));
    tool_json_write_string(1, value);
    rt_write_cstr(1, ",\"method\":");
    tool_json_write_string(1, result->method != 0 ? result->method : "bisection");
    rt_write_cstr(1, ",\"iterations\":");
    rt_write_uint(1, (unsigned long long)result->iterations);
    rt_write_cstr(1, ",\"candidate\":");
    rt_write_cstr(1, result->status == SOLVE_STATUS_CANDIDATE ? "true" : "false");
    if (result->exact_value[0] != '\0' || result->approximate) {
        rt_write_cstr(1, ",\"exact\":");
        rt_write_cstr(1, result->exact_value[0] != '\0' ? "true" : "false");
    }
    rt_write_char(1, '}');
    tool_json_end_event(1);
    return 0;
}

static void solve_print_result(const SolveEquation *equation, const SolveOptions *options, const SolveResult *result) {
    char value[96];
    char root_display[96];
    double residual;

    if (tool_json_is_enabled()) {
        (void)solve_write_json_result(equation, options, result);
        return;
    }
    if (result->exact_value[0] != '\0') {
        rt_copy_string(value, sizeof(value), result->exact_value);
    } else {
        solve_format_result_answer(equation, options, result, value, sizeof(value));
    }
    rt_copy_string(root_display, sizeof(root_display), value);
    if (options->quiet) {
        rt_write_line(1, value);
        return;
    }
    rt_write_cstr(1, options->var_name);
    rt_write_cstr(1, " = ");
    rt_write_line(1, value);
    if (options->report_y) {
        solve_format_answer(result->y, options->scale, value, sizeof(value));
        rt_write_cstr(1, "y = ");
        rt_write_line(1, value);
    }
    residual = solve_display_residual(equation, options, result, root_display);
    solve_format_double(residual, result->approximate ? SOLVE_MAX_SCALE : options->scale, value, sizeof(value));
    rt_write_cstr(1, "residual = ");
    rt_write_line(1, value);
    rt_write_cstr(1, "method = ");
    rt_write_line(1, result->method != 0 ? result->method : "bisection");
    rt_write_cstr(1, "iterations = ");
    rt_write_uint(1, (unsigned long long)result->iterations);
    rt_write_char(1, '\n');
    if (result->status == SOLVE_STATUS_CANDIDATE) {
        rt_write_line(1, "status = touching-root-candidate");
    } else if (result->status == SOLVE_STATUS_SUSPECT_DISCONTINUITY) {
        rt_write_line(1, "status = suspected-discontinuity");
    } else if (result->approximate) {
        rt_write_line(1, "status = approximate");
    }
}

static void solve_print_identity(const SolveOptions *options, int exact) {
    if (tool_json_is_enabled()) {
        if (tool_json_begin_event(1, "solve", "stdout", "solve_identity") != 0) return;
        rt_write_cstr(1, ",\"data\":{\"variable\":");
        tool_json_write_string(1, options->var_name);
        rt_write_cstr(1, ",\"exact\":");
        rt_write_cstr(1, exact ? "true" : "false");
        rt_write_cstr(1, ",\"method\":");
        tool_json_write_string(1, exact ? "polynomial-identity" : "polynomial-identity-approx");
        rt_write_char(1, '}');
        tool_json_end_event(1);
        return;
    }
    if (options->quiet) {
        rt_write_line(1, exact ? "all real values" : "all real values (approximate)");
        return;
    }
    rt_write_line(1, exact ? "identity = true" : "identity = approximate");
    rt_write_cstr(1, options->var_name);
    rt_write_line(1, exact ? " = all real values" : " = all real values (within tolerance)");
    rt_write_line(1, exact ? "method = polynomial-identity" : "method = polynomial-identity-approx");
}

static void solve_write_summary_json(size_t count, int status) {
    if (!tool_json_is_enabled()) {
        return;
    }
    if (tool_json_begin_event(1, "solve", "stdout", "solve_summary") != 0) return;
    rt_write_cstr(1, ",\"data\":{\"count\":");
    rt_write_uint(1, (unsigned long long)count);
    rt_write_cstr(1, ",\"status\":");
    tool_json_write_string(1, status == 0 ? "found" : "not_found");
    rt_write_char(1, '}');
    tool_json_end_event(1);
}

static int solve_finish_results(const SolveEquation *equation, const SolveOptions *options, const SolveResultSet *results) {
    size_t index;

    if (results->identity) {
        solve_print_identity(options, results->identity == 1);
        solve_write_summary_json(1U, 0);
        return 0;
    }
    if (results->no_real_solutions && results->count == 0U) {
        if (tool_json_is_enabled()) {
            solve_write_summary_json(0U, 1);
        } else if (!options->quiet) {
            rt_write_line(1, "no real solutions");
        }
        return 1;
    }

    for (index = 0U; index < results->count; ++index) {
        if (index > 0U && !tool_json_is_enabled() && !options->quiet) {
            rt_write_char(1, '\n');
        }
        solve_print_result(equation, options, &results->results[index]);
    }
    if (results->count == 0U) {
        if (tool_json_is_enabled()) {
            solve_write_summary_json(0U, 1);
        } else if (!options->quiet) {
            if (options->have_bracket) {
                rt_write_line(1, "no solution found in requested interval");
            } else if (options->default_scan) {
                rt_write_line(1, "no solution found in default scan range");
            } else {
                rt_write_line(1, "no solution found in requested range");
            }
        }
        return results->suspected_discontinuity ? 3 : 1;
    }
    solve_write_summary_json(results->count, 0);
    return 0;
}

static int solve_run_solver_equation(const SolveEquation *equation, const SolveOptions *options) {
    SolveResultSet results;

    solve_explain_start(equation, options);
    rt_memset(&results, 0, sizeof(results));
    if (!options->have_bracket && solve_try_rational_polynomial(equation, options, &results)) {
        /* solved exactly by the rational polynomial front-end */
    } else if (!options->have_bracket && solve_try_polynomial(equation, options, &results)) {
        /* solved directly */
    } else if (!options->have_bracket && solve_try_linear(equation, options, &results)) {
        /* solved directly */
    } else if (options->have_bracket) {
        solve_explicit_bracket(equation, options, &results);
    } else {
        solve_scan(equation, options, &results);
    }
    return solve_finish_results(equation, options, &results);
}


#endif
