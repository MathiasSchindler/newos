#if !defined(SOLVE_FROM_SOLVE_C)
#ifndef SOLVE_PRIVATE_CONTEXT
#define SOLVE_PRIVATE_CONTEXT 1
#endif
#include "../solve.c"
#else
static void solve_poly_zero(SolvePoly *poly) {
    int i;
    for (i = 0; i <= SOLVE_POLY_MAX_DEGREE; ++i) {
        poly->coeff[i] = 0.0;
    }
    poly->exact = 1;
}

static SolvePoly solve_poly_constant(double value) {
    SolvePoly poly;
    solve_poly_zero(&poly);
    poly.coeff[0] = value;
    return poly;
}

static int solve_number_literal_is_exact_integer(const char *text, size_t start, size_t end, double value) {
    size_t i;

    if (value < -9007199254740992.0 || value > 9007199254740992.0) {
        return 0;
    }
    for (i = start; i < end; ++i) {
        if (text[i] == '.' || text[i] == 'e' || text[i] == 'E') {
            return 0;
        }
    }
    return 1;
}

static SolvePoly solve_poly_variable(void) {
    SolvePoly poly;
    solve_poly_zero(&poly);
    poly.coeff[1] = 1.0;
    return poly;
}

static int solve_poly_degree(const SolvePoly *poly, double tolerance) {
    int degree;
    for (degree = SOLVE_POLY_MAX_DEGREE; degree >= 0; --degree) {
        if (solve_abs(poly->coeff[degree]) > tolerance) {
            return degree;
        }
    }
    return -1;
}

static int solve_poly_is_constant(const SolvePoly *poly, double tolerance) {
    return solve_poly_degree(poly, tolerance) <= 0;
}

static SolvePoly solve_poly_add(const SolvePoly *left, const SolvePoly *right, int subtract) {
    SolvePoly result;
    int i;
    solve_poly_zero(&result);
    for (i = 0; i <= SOLVE_POLY_MAX_DEGREE; ++i) {
        result.coeff[i] = left->coeff[i] + (subtract ? -right->coeff[i] : right->coeff[i]);
    }
    result.exact = left->exact && right->exact;
    return result;
}

static SolvePoly solve_poly_scale(const SolvePoly *poly, double scale) {
    SolvePoly result;
    int i;
    solve_poly_zero(&result);
    for (i = 0; i <= SOLVE_POLY_MAX_DEGREE; ++i) {
        result.coeff[i] = poly->coeff[i] * scale;
    }
    result.exact = poly->exact && (scale == 1.0 || scale == -1.0);
    return result;
}

static int solve_poly_mul(const SolvePoly *left, const SolvePoly *right, SolvePoly *result_out) {
    int i;
    int j;
    solve_poly_zero(result_out);
    result_out->exact = left->exact && right->exact;
    for (i = 0; i <= SOLVE_POLY_MAX_DEGREE; ++i) {
        for (j = 0; j <= SOLVE_POLY_MAX_DEGREE; ++j) {
            if (left->coeff[i] != 0.0 && right->coeff[j] != 0.0) {
                if (i + j > SOLVE_POLY_MAX_DEGREE) {
                    return -1;
                }
                result_out->coeff[i + j] += left->coeff[i] * right->coeff[j];
            }
        }
    }
    return 0;
}

static double solve_poly_eval(const SolvePoly *poly, int degree, double x) {
    double value = 0.0;

    while (degree >= 0) {
        value = value * x + poly->coeff[degree];
        degree -= 1;
    }
    return value;
}

static int solve_poly_divide_linear(const SolvePoly *poly, int degree, double root, SolvePoly *quotient_out, double tolerance) {
    double remainder;
    int i;

    if (degree <= 0) {
        return -1;
    }
    solve_poly_zero(quotient_out);
    quotient_out->coeff[degree - 1] = poly->coeff[degree];
    for (i = degree - 2; i >= 0; --i) {
        quotient_out->coeff[i] = poly->coeff[i + 1] + root * quotient_out->coeff[i + 1];
    }
    remainder = poly->coeff[0] + root * quotient_out->coeff[0];
    return solve_abs(remainder) <= tolerance ? 0 : -1;
}

static int solve_find_rational_poly_root(const SolvePoly *poly, int degree, const SolveOptions *options, double *root_out) {
    double bound = 100.0;
    double tolerance = options->tolerance * 1000.0;
    int den;

    if (options->have_scan) {
        double lo_abs = solve_abs(options->scan_lo);
        double hi_abs = solve_abs(options->scan_hi);
        bound = lo_abs > hi_abs ? lo_abs : hi_abs;
        if (bound < 1.0) {
            bound = 1.0;
        }
    }
    if (bound > 1000.0) {
        bound = 1000.0;
    }
    for (den = 1; den <= SOLVE_POLY_FACTOR_DENOMINATOR_LIMIT; ++den) {
        int limit = (int)(bound * (double)den + 0.5);
        int num;
        for (num = -limit; num <= limit; ++num) {
            double candidate = (double)num / (double)den;
            if (solve_abs(solve_poly_eval(poly, degree, candidate)) <= tolerance) {
                *root_out = candidate;
                return 0;
            }
        }
    }
    return -1;
}

static void solve_poly_skip_spaces(SolvePolyParser *parser) {
    while (parser->text[parser->pos] == ' ' || parser->text[parser->pos] == '\t' || parser->text[parser->pos] == '\r' || parser->text[parser->pos] == '\n') {
        parser->pos += 1U;
    }
}

static void solve_poly_set_error(SolvePolyParser *parser) {
    parser->error = 1;
}

static SolvePoly solve_parse_poly_expr(SolvePolyParser *parser);

static SolvePoly solve_parse_poly_primary(SolvePolyParser *parser) {
    char name[SOLVE_NAME_CAPACITY];
    double value;
    SolvePoly poly;

    solve_poly_skip_spaces(parser);
    if (parser->text[parser->pos] == '(') {
        parser->pos += 1U;
        poly = solve_parse_poly_expr(parser);
        solve_poly_skip_spaces(parser);
        if (parser->text[parser->pos] != ')') {
            solve_poly_set_error(parser);
            return solve_poly_constant(0.0);
        }
        parser->pos += 1U;
        return poly;
    }
    if ((parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9') || parser->text[parser->pos] == '.') {
        size_t start = parser->pos;
        if (solve_parse_double(parser->text, &parser->pos, &value) != 0) {
            solve_poly_set_error(parser);
            return solve_poly_constant(0.0);
        }
        poly = solve_poly_constant(value);
        poly.exact = solve_number_literal_is_exact_integer(parser->text, start, parser->pos, value);
        return poly;
    }
    if (tool_ascii_is_identifier_start(parser->text[parser->pos])) {
        SolveExprParser reader;
        reader.text = parser->text;
        reader.pos = parser->pos;
        reader.var_name = parser->var_name;
        reader.var_value = 0.0;
        reader.error = 0;
        reader.message = 0;
        if (solve_read_identifier(&reader, name, sizeof(name)) != 0) {
            solve_poly_set_error(parser);
            return solve_poly_constant(0.0);
        }
        parser->pos = reader.pos;
        if (rt_strcmp(name, parser->var_name) == 0) {
            return solve_poly_variable();
        }
        if (rt_strcmp(name, "pi") == 0) {
            return solve_poly_constant(SOLVE_PI);
        }
        if (rt_strcmp(name, "e") == 0) {
            solve_poly_skip_spaces(parser);
            if (parser->text[parser->pos] != '(') {
                return solve_poly_constant(SOLVE_E);
            }
        }
        solve_poly_set_error(parser);
        return solve_poly_constant(0.0);
    }
    solve_poly_set_error(parser);
    return solve_poly_constant(0.0);
}

static SolvePoly solve_parse_poly_unary(SolvePolyParser *parser) {
    SolvePoly poly;
    solve_poly_skip_spaces(parser);
    if (parser->text[parser->pos] == '+') {
        parser->pos += 1U;
        return solve_parse_poly_unary(parser);
    }
    if (parser->text[parser->pos] == '-') {
        parser->pos += 1U;
        poly = solve_parse_poly_unary(parser);
        return solve_poly_scale(&poly, -1.0);
    }
    return solve_parse_poly_primary(parser);
}

static SolvePoly solve_parse_poly_power(SolvePolyParser *parser) {
    SolvePoly base = solve_parse_poly_unary(parser);

    solve_poly_skip_spaces(parser);
    if (parser->text[parser->pos] == '^') {
        SolvePoly exponent;
        SolvePoly result;
        int power;
        int i;
        parser->pos += 1U;
        exponent = solve_parse_poly_power(parser);
        if (parser->error || !solve_poly_is_constant(&exponent, SOLVE_DEFAULT_TOLERANCE)) {
            solve_poly_set_error(parser);
            return solve_poly_constant(0.0);
        }
        power = exponent.coeff[0] >= 0.0 ? (int)(exponent.coeff[0] + 0.5) : (int)(exponent.coeff[0] - 0.5);
        if (power < 0 || power > SOLVE_POLY_MAX_DEGREE || solve_abs(exponent.coeff[0] - (double)power) > SOLVE_DEFAULT_TOLERANCE) {
            solve_poly_set_error(parser);
            return solve_poly_constant(0.0);
        }
        result = solve_poly_constant(1.0);
        for (i = 0; i < power; ++i) {
            SolvePoly next;
            if (solve_poly_mul(&result, &base, &next) != 0) {
                solve_poly_set_error(parser);
                return solve_poly_constant(0.0);
            }
            result = next;
        }
        return result;
    }
    return base;
}

static SolvePoly solve_parse_poly_term(SolvePolyParser *parser) {
    SolvePoly value = solve_parse_poly_power(parser);

    while (!parser->error) {
        char op;
        SolvePoly right;
        SolvePoly product;
        solve_poly_skip_spaces(parser);
        op = parser->text[parser->pos];
        if (op != '*' && op != '/' && op != '%') {
            break;
        }
        parser->pos += 1U;
        right = solve_parse_poly_power(parser);
        if (op == '*') {
            if (solve_poly_mul(&value, &right, &product) != 0) {
                solve_poly_set_error(parser);
                return solve_poly_constant(0.0);
            }
            value = product;
        } else if (op == '/') {
            if (!solve_poly_is_constant(&right, SOLVE_DEFAULT_TOLERANCE) || solve_abs(right.coeff[0]) <= SOLVE_DEFAULT_TOLERANCE) {
                solve_poly_set_error(parser);
                return solve_poly_constant(0.0);
            }
            value = solve_poly_scale(&value, 1.0 / right.coeff[0]);
            if (solve_abs(right.coeff[0] - 1.0) > SOLVE_DEFAULT_TOLERANCE && solve_abs(right.coeff[0] + 1.0) > SOLVE_DEFAULT_TOLERANCE) {
                value.exact = 0;
            }
        } else {
            solve_poly_set_error(parser);
            return solve_poly_constant(0.0);
        }
    }
    return value;
}

static SolvePoly solve_parse_poly_expr(SolvePolyParser *parser) {
    SolvePoly value = solve_parse_poly_term(parser);

    while (!parser->error) {
        char op;
        SolvePoly right;
        solve_poly_skip_spaces(parser);
        op = parser->text[parser->pos];
        if (op != '+' && op != '-') {
            break;
        }
        parser->pos += 1U;
        right = solve_parse_poly_term(parser);
        value = solve_poly_add(&value, &right, op == '-');
    }
    return value;
}

static int solve_parse_poly_text(const char *expr, const char *var_name, SolvePoly *poly_out) {
    SolvePolyParser parser;
    SolvePoly poly;

    parser.text = expr;
    parser.pos = 0U;
    parser.var_name = var_name;
    parser.error = 0;
    poly = solve_parse_poly_expr(&parser);
    solve_poly_skip_spaces(&parser);
    if (parser.error || parser.text[parser.pos] != '\0') {
        return -1;
    }
    *poly_out = poly;
    return 0;
}

static int solve_equation_poly(const SolveEquation *equation, const SolveOptions *options, SolvePoly *poly_out) {
    SolvePoly left;
    SolvePoly right;

    if (solve_parse_poly_text(equation->left, options->var_name, &left) != 0 ||
        solve_parse_poly_text(equation->right, options->var_name, &right) != 0) {
        return -1;
    }
    *poly_out = solve_poly_add(&left, &right, 1);
    return 0;
}


#endif
