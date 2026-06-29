#if !defined(SOLVE_FROM_SOLVE_C)
#ifndef SOLVE_PRIVATE_CONTEXT
#define SOLVE_PRIVATE_CONTEXT 1
#endif
#include "../solve.c"
#else
static void solve_rat_poly_zero(SolveRatPoly *poly) {
    int i;
    for (i = 0; i <= SOLVE_RAT_POLY_MAX_DEGREE; ++i) {
        (void)solve_rat_make(0, 1, &poly->coeff[i]);
    }
}

static SolveRatPoly solve_rat_poly_constant(SolveRat value) {
    SolveRatPoly poly;
    solve_rat_poly_zero(&poly);
    poly.coeff[0] = value;
    return poly;
}

static SolveRatPoly solve_rat_poly_variable(void) {
    SolveRat one;
    SolveRatPoly poly;
    (void)solve_rat_make(1, 1, &one);
    solve_rat_poly_zero(&poly);
    poly.coeff[1] = one;
    return poly;
}

static int solve_rat_poly_degree(const SolveRatPoly *poly) {
    int degree;
    for (degree = SOLVE_RAT_POLY_MAX_DEGREE; degree >= 0; --degree) {
        if (!solve_rat_is_zero(poly->coeff[degree])) return degree;
    }
    return -1;
}

static int solve_rat_poly_is_constant(const SolveRatPoly *poly) {
    return solve_rat_poly_degree(poly) <= 0;
}

static int solve_rat_poly_add(const SolveRatPoly *left, const SolveRatPoly *right, int subtract, SolveRatPoly *out) {
    int i;
    solve_rat_poly_zero(out);
    for (i = 0; i <= SOLVE_RAT_POLY_MAX_DEGREE; ++i) {
        if ((subtract ? solve_rat_sub(left->coeff[i], right->coeff[i], &out->coeff[i]) : solve_rat_add(left->coeff[i], right->coeff[i], &out->coeff[i])) != 0) return -1;
    }
    return 0;
}

static int solve_rat_poly_neg(const SolveRatPoly *poly, SolveRatPoly *out) {
    int i;
    solve_rat_poly_zero(out);
    for (i = 0; i <= SOLVE_RAT_POLY_MAX_DEGREE; ++i) {
        if (solve_rat_neg(poly->coeff[i], &out->coeff[i]) != 0) return -1;
    }
    return 0;
}

static int solve_rat_poly_mul(const SolveRatPoly *left, const SolveRatPoly *right, SolveRatPoly *out) {
    int i;
    int j;
    solve_rat_poly_zero(out);
    for (i = 0; i <= SOLVE_RAT_POLY_MAX_DEGREE; ++i) {
        for (j = 0; j <= SOLVE_RAT_POLY_MAX_DEGREE; ++j) {
            if (!solve_rat_is_zero(left->coeff[i]) && !solve_rat_is_zero(right->coeff[j])) {
                SolveRat product;
                SolveRat sum;
                if (i + j > SOLVE_RAT_POLY_MAX_DEGREE) return -1;
                if (solve_rat_mul(left->coeff[i], right->coeff[j], &product) != 0) return -1;
                if (solve_rat_add(out->coeff[i + j], product, &sum) != 0) return -1;
                out->coeff[i + j] = sum;
            }
        }
    }
    return 0;
}

static int solve_rat_poly_scale(const SolveRatPoly *poly, SolveRat scale, SolveRatPoly *out) {
    int i;
    solve_rat_poly_zero(out);
    for (i = 0; i <= SOLVE_RAT_POLY_MAX_DEGREE; ++i) {
        if (solve_rat_mul(poly->coeff[i], scale, &out->coeff[i]) != 0) return -1;
    }
    return 0;
}

static int solve_rat_poly_eval(const SolveRatPoly *poly, int degree, SolveRat x, SolveRat *out) {
    SolveRat value;
    (void)solve_rat_make(0, 1, &value);
    while (degree >= 0) {
        SolveRat product;
        SolveRat sum;
        if (solve_rat_mul(value, x, &product) != 0) return -1;
        if (solve_rat_add(product, poly->coeff[degree], &sum) != 0) return -1;
        value = sum;
        degree -= 1;
    }
    *out = value;
    return 0;
}

static int solve_rat_poly_divide_linear(const SolveRatPoly *poly, int degree, SolveRat root, SolveRatPoly *quotient_out) {
    SolveRat remainder;
    int i;
    if (degree <= 0) return -1;
    solve_rat_poly_zero(quotient_out);
    quotient_out->coeff[degree - 1] = poly->coeff[degree];
    for (i = degree - 2; i >= 0; --i) {
        SolveRat product;
        if (solve_rat_mul(root, quotient_out->coeff[i + 1], &product) != 0) return -1;
        if (solve_rat_add(poly->coeff[i + 1], product, &quotient_out->coeff[i]) != 0) return -1;
    }
    if (solve_rat_mul(root, quotient_out->coeff[0], &remainder) != 0) return -1;
    if (solve_rat_add(poly->coeff[0], remainder, &remainder) != 0) return -1;
    return solve_rat_is_zero(remainder) ? 0 : -1;
}

static void solve_rat_skip_spaces(SolveRatParser *parser) {
    while (parser->text[parser->pos] == ' ' || parser->text[parser->pos] == '\t' || parser->text[parser->pos] == '\r' || parser->text[parser->pos] == '\n') parser->pos += 1U;
}

static void solve_rat_set_error(SolveRatParser *parser) {
    parser->error = 1;
}

static SolveRatPoly solve_parse_rat_expr(SolveRatParser *parser);

static SolveRatPoly solve_parse_rat_primary(SolveRatParser *parser) {
    char name[SOLVE_NAME_CAPACITY];
    SolveRat value;
    SolveRatPoly poly;

    (void)solve_rat_make(0, 1, &value);

    solve_rat_skip_spaces(parser);
    if (parser->text[parser->pos] == '(') {
        parser->pos += 1U;
        poly = solve_parse_rat_expr(parser);
        solve_rat_skip_spaces(parser);
        if (parser->text[parser->pos] != ')') solve_rat_set_error(parser);
        else parser->pos += 1U;
        return poly;
    }
    if ((parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9') || parser->text[parser->pos] == '.') {
        if (solve_parse_rat_literal(parser->text, &parser->pos, &value) != 0) solve_rat_set_error(parser);
        return solve_rat_poly_constant(value);
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
            solve_rat_set_error(parser);
            return solve_rat_poly_constant(value);
        }
        parser->pos = reader.pos;
        if (rt_strcmp(name, parser->var_name) == 0) return solve_rat_poly_variable();
        solve_rat_set_error(parser);
    } else {
        solve_rat_set_error(parser);
    }
    (void)solve_rat_make(0, 1, &value);
    return solve_rat_poly_constant(value);
}

static SolveRatPoly solve_parse_rat_unary(SolveRatParser *parser) {
    SolveRatPoly poly;
    SolveRatPoly neg;
    solve_rat_skip_spaces(parser);
    if (parser->text[parser->pos] == '+') {
        parser->pos += 1U;
        return solve_parse_rat_unary(parser);
    }
    if (parser->text[parser->pos] == '-') {
        parser->pos += 1U;
        poly = solve_parse_rat_unary(parser);
        if (solve_rat_poly_neg(&poly, &neg) != 0) solve_rat_set_error(parser);
        return neg;
    }
    return solve_parse_rat_primary(parser);
}

static SolveRatPoly solve_parse_rat_power(SolveRatParser *parser) {
    SolveRatPoly base = solve_parse_rat_unary(parser);
    solve_rat_skip_spaces(parser);
    if (parser->text[parser->pos] == '^') {
        SolveRatPoly exponent;
        SolveRatPoly result;
        int power;
        int i;
        parser->pos += 1U;
        exponent = solve_parse_rat_power(parser);
        if (parser->error || !solve_rat_poly_is_constant(&exponent) || exponent.coeff[0].den != 1) {
            solve_rat_set_error(parser);
            return base;
        }
        if (exponent.coeff[0].num < 0 || exponent.coeff[0].num > SOLVE_RAT_POLY_MAX_DEGREE) {
            solve_rat_set_error(parser);
            return base;
        }
        power = (int)exponent.coeff[0].num;
        (void)solve_rat_make(1, 1, &exponent.coeff[0]);
        result = solve_rat_poly_constant(exponent.coeff[0]);
        for (i = 0; i < power; ++i) {
            SolveRatPoly next;
            if (solve_rat_poly_mul(&result, &base, &next) != 0) {
                solve_rat_set_error(parser);
                return result;
            }
            result = next;
        }
        return result;
    }
    return base;
}

static SolveRatPoly solve_parse_rat_term(SolveRatParser *parser) {
    SolveRatPoly value = solve_parse_rat_power(parser);
    while (!parser->error) {
        char op;
        SolveRatPoly right;
        SolveRatPoly next;
        solve_rat_skip_spaces(parser);
        op = parser->text[parser->pos];
        if (op != '*' && op != '/' && op != '%') break;
        parser->pos += 1U;
        right = solve_parse_rat_power(parser);
        if (op == '*') {
            if (solve_rat_poly_mul(&value, &right, &next) != 0) solve_rat_set_error(parser);
            value = next;
        } else if (op == '/') {
            if (!solve_rat_poly_is_constant(&right) || solve_rat_is_zero(right.coeff[0])) {
                solve_rat_set_error(parser);
            } else {
                SolveRat reciprocal;
                if (solve_rat_make(right.coeff[0].den, right.coeff[0].num, &reciprocal) != 0 || solve_rat_poly_scale(&value, reciprocal, &next) != 0) solve_rat_set_error(parser);
                else value = next;
            }
        } else {
            solve_rat_set_error(parser);
        }
    }
    return value;
}

static SolveRatPoly solve_parse_rat_expr(SolveRatParser *parser) {
    SolveRatPoly value = solve_parse_rat_term(parser);
    while (!parser->error) {
        char op;
        SolveRatPoly right;
        SolveRatPoly next;
        solve_rat_skip_spaces(parser);
        op = parser->text[parser->pos];
        if (op != '+' && op != '-') break;
        parser->pos += 1U;
        right = solve_parse_rat_term(parser);
        if (solve_rat_poly_add(&value, &right, op == '-', &next) != 0) solve_rat_set_error(parser);
        value = next;
    }
    return value;
}

static int solve_parse_rat_text(const char *expr, const char *var_name, SolveRatPoly *poly_out) {
    SolveRatParser parser;
    SolveRatPoly poly;
    parser.text = expr;
    parser.pos = 0U;
    parser.var_name = var_name;
    parser.error = 0;
    poly = solve_parse_rat_expr(&parser);
    solve_rat_skip_spaces(&parser);
    if (parser.error || parser.text[parser.pos] != '\0') return -1;
    *poly_out = poly;
    return 0;
}

static int solve_equation_rat_poly(const SolveEquation *equation, const SolveOptions *options, SolveRatPoly *poly_out) {
    SolveRatPoly left;
    SolveRatPoly right;
    if (solve_parse_rat_text(equation->left, options->var_name, &left) != 0 || solve_parse_rat_text(equation->right, options->var_name, &right) != 0) return -1;
    return solve_rat_poly_add(&left, &right, 1, poly_out);
}

static int solve_rat_compare(SolveRat left, SolveRat right) {
    __int128 diff = (__int128)left.num * right.den - (__int128)right.num * left.den;
    if (diff < 0) return -1;
    if (diff > 0) return 1;
    return 0;
}

static int solve_ll_lcm(long long a, long long b, long long *out) {
    unsigned long long aa = solve_abs_ll(a);
    unsigned long long bb = solve_abs_ll(b);
    unsigned long long gcd;
    __int128 value;

    if (aa == 0ULL || bb == 0ULL) return -1;
    gcd = solve_gcd_ull(aa, bb);
    value = (__int128)(aa / gcd) * bb;
    return solve_i128_to_ll(value, out);
}

static int solve_rat_poly_to_integer(const SolveRatPoly *poly, int degree, long long *out) {
    long long lcm = 1;
    long long content = 0;
    int i;

    for (i = 0; i <= degree; ++i) {
        if (solve_ll_lcm(lcm, poly->coeff[i].den, &lcm) != 0) return -1;
    }
    for (i = 0; i <= degree; ++i) {
        __int128 value = (__int128)poly->coeff[i].num * (lcm / poly->coeff[i].den);
        if (solve_i128_to_ll(value, &out[i]) != 0) return -1;
        if (out[i] != 0) {
            unsigned long long gcd = content == 0 ? solve_abs_ll(out[i]) : solve_gcd_ll(content, out[i]);
            if (gcd > (unsigned long long)SOLVE_RAT_LIMIT) return -1;
            content = (long long)gcd;
        }
    }
    if (content > 1) {
        for (i = 0; i <= degree; ++i) out[i] /= content;
    }
    if (out[degree] < 0) {
        for (i = 0; i <= degree; ++i) out[i] = -out[i];
    }
    return 0;
}

static int solve_collect_divisors(unsigned long long value, unsigned long long *divisors, int *count_out) {
    unsigned long long divisor;
    int count = 0;

    if (value > SOLVE_RAT_DIVISOR_LIMIT) return -1;
    if (value == 0ULL) {
        divisors[count++] = 0ULL;
    } else {
        for (divisor = 1ULL; divisor * divisor <= value; ++divisor) {
            if (value % divisor == 0ULL) {
                if (count >= SOLVE_RAT_MAX_DIVISORS) return -1;
                divisors[count++] = divisor;
                if (divisor != value / divisor) {
                    if (count >= SOLVE_RAT_MAX_DIVISORS) return -1;
                    divisors[count++] = value / divisor;
                }
            }
        }
    }
    *count_out = count;
    return 0;
}

static int solve_find_exact_rational_root(const SolveRatPoly *poly, int degree, SolveRat *root_out) {
    long long ints[SOLVE_RAT_POLY_MAX_DEGREE + 1];
    unsigned long long numerators[SOLVE_RAT_MAX_DIVISORS];
    unsigned long long denominators[SOLVE_RAT_MAX_DIVISORS];
    int numerator_count;
    int denominator_count;
    int numerator_index;
    int denominator_index;

    if (degree < 1 || solve_rat_poly_to_integer(poly, degree, ints) != 0) return -1;
    if (ints[0] == 0) {
        return solve_rat_make(0, 1, root_out);
    }
    if (solve_collect_divisors(solve_abs_ll(ints[0]), numerators, &numerator_count) != 0 ||
        solve_collect_divisors(solve_abs_ll(ints[degree]), denominators, &denominator_count) != 0) {
        return -1;
    }
    for (numerator_index = 0; numerator_index < numerator_count; ++numerator_index) {
        for (denominator_index = 0; denominator_index < denominator_count; ++denominator_index) {
            int sign;
            for (sign = -1; sign <= 1; sign += 2) {
                SolveRat candidate;
                SolveRat value;
                if (solve_rat_make((long long)(sign * (int)numerators[numerator_index]), (long long)denominators[denominator_index], &candidate) != 0) return -1;
                if (solve_rat_poly_eval(poly, degree, candidate, &value) != 0) continue;
                if (solve_rat_is_zero(value)) {
                    *root_out = candidate;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int solve_sqrt_ull_exact(unsigned long long value, unsigned long long *root_out) {
    unsigned long long lo = 0ULL;
    unsigned long long hi = value < 3037000499ULL ? value : 3037000499ULL;

    while (lo <= hi) {
        unsigned long long mid = lo + (hi - lo) / 2ULL;
        __int128 square = (__int128)mid * mid;
        if (square == (__int128)value) {
            *root_out = mid;
            return 0;
        }
        if (square < (__int128)value) {
            lo = mid + 1ULL;
        } else {
            if (mid == 0ULL) break;
            hi = mid - 1ULL;
        }
    }
    return -1;
}

static int solve_rat_sqrt_exact(SolveRat value, SolveRat *root_out) {
    unsigned long long num_root;
    unsigned long long den_root;

    if (value.num < 0) return -1;
    if (solve_sqrt_ull_exact(solve_abs_ll(value.num), &num_root) != 0 || solve_sqrt_ull_exact(solve_abs_ll(value.den), &den_root) != 0) return -1;
    return solve_rat_make((long long)num_root, (long long)den_root, root_out);
}

static int solve_rat_pow(SolveRat base, int power, SolveRat *out) {
    SolveRat result;
    int i;
    if (power < 0) return -1;
    if (solve_rat_make(1, 1, &result) != 0) return -1;
    for (i = 0; i < power; ++i) {
        if (solve_rat_mul(result, base, &result) != 0) return -1;
    }
    *out = result;
    return 0;
}

static int solve_rat_abs_value(SolveRat value, SolveRat *out) {
    return value.num < 0 ? solve_rat_neg(value, out) : solve_rat_make(value.num, value.den, out);
}

static int solve_rat_poly_derivative(const SolveRatPoly *poly, int order, SolveRatPoly *out) {
    SolveRatPoly current = *poly;
    int step;
    for (step = 0; step < order; ++step) {
        SolveRatPoly next;
        int i;
        solve_rat_poly_zero(&next);
        for (i = 1; i <= SOLVE_RAT_POLY_MAX_DEGREE; ++i) {
            SolveRat multiplier;
            if (solve_rat_make(i, 1, &multiplier) != 0 || solve_rat_mul(current.coeff[i], multiplier, &next.coeff[i - 1]) != 0) return -1;
        }
        current = next;
    }
    *out = current;
    return 0;
}

static int solve_rat_poly_antiderivative_eval(const SolveRatPoly *poly, SolveRat x, SolveRat *out) {
    SolveRat sum;
    int i;
    if (solve_rat_make(0, 1, &sum) != 0) return -1;
    for (i = 0; i <= SOLVE_RAT_POLY_MAX_DEGREE; ++i) {
        SolveRat power;
        SolveRat denom;
        SolveRat term;
        if (solve_rat_is_zero(poly->coeff[i])) continue;
        if (solve_rat_pow(x, i + 1, &power) != 0 || solve_rat_mul(poly->coeff[i], power, &term) != 0) return -1;
        if (solve_rat_make(i + 1, 1, &denom) != 0 || solve_rat_div(term, denom, &term) != 0) return -1;
        if (solve_rat_add(sum, term, &sum) != 0) return -1;
    }
    *out = sum;
    return 0;
}

static int solve_rat_poly_parse_bound(const char *text, const char *var_name, SolveRat *out) {
    SolveRatPoly poly;
    if (solve_parse_rat_text(text, var_name, &poly) != 0 || solve_rat_poly_degree(&poly) > 0) return -1;
    *out = poly.coeff[0];
    return 0;
}

static int solve_rat_poly_format(const SolveRatPoly *poly, const char *var_name, char *buffer, size_t buffer_size) {
    int degree = solve_rat_poly_degree(poly);
    int first = 1;
    int i;
    size_t length = 0U;

    if (buffer_size == 0U) return -1;
    buffer[0] = '\0';
    if (degree < 0) {
        return solve_append_char(buffer, buffer_size, &length, '0');
    }
    for (i = degree; i >= 0; --i) {
        SolveRat coeff = poly->coeff[i];
        SolveRat abs_coeff;
        char coeff_text[96];
        if (solve_rat_is_zero(coeff)) continue;
        if (solve_rat_abs_value(coeff, &abs_coeff) != 0 || solve_rat_format(abs_coeff, coeff_text, sizeof(coeff_text)) != 0) return -1;
        if (first) {
            if (coeff.num < 0 && solve_append_text(buffer, buffer_size, &length, "-") != 0) return -1;
        } else {
            if (solve_append_text(buffer, buffer_size, &length, coeff.num < 0 ? " - " : " + ") != 0) return -1;
        }
        if (i == 0) {
            if (solve_append_text(buffer, buffer_size, &length, coeff_text) != 0) return -1;
        } else {
            if (!(abs_coeff.num == 1 && abs_coeff.den == 1)) {
                if (solve_append_text(buffer, buffer_size, &length, coeff_text) != 0 || solve_append_char(buffer, buffer_size, &length, '*') != 0) return -1;
            }
            if (solve_append_text(buffer, buffer_size, &length, var_name) != 0) return -1;
            if (i > 1) {
                if (solve_append_char(buffer, buffer_size, &length, '^') != 0) return -1;
                if (solve_append_signed_ll(buffer, buffer_size, &length, i) != 0) return -1;
            }
        }
        first = 0;
    }
    return 0;
}

static int solve_rat_poly_antiderivative(const SolveRatPoly *poly, SolveRatPoly *out) {
    int i;
    solve_rat_poly_zero(out);
    for (i = 0; i < SOLVE_RAT_POLY_MAX_DEGREE; ++i) {
        SolveRat denom;
        if (solve_rat_is_zero(poly->coeff[i])) continue;
        if (solve_rat_make(i + 1, 1, &denom) != 0 || solve_rat_div(poly->coeff[i], denom, &out->coeff[i + 1]) != 0) return -1;
    }
    if (!solve_rat_is_zero(poly->coeff[SOLVE_RAT_POLY_MAX_DEGREE])) return -1;
    return 0;
}

static int solve_rat_poly_definite_integral(const SolveRatPoly *poly, SolveRat lo, SolveRat hi, SolveRat *out) {
    SolveRat hi_value;
    SolveRat lo_value;
    if (solve_rat_poly_antiderivative_eval(poly, hi, &hi_value) != 0 || solve_rat_poly_antiderivative_eval(poly, lo, &lo_value) != 0) return -1;
    return solve_rat_sub(hi_value, lo_value, out);
}

static int solve_rat_poly_square(const SolveRatPoly *poly, SolveRatPoly *out) {
    return solve_rat_poly_mul(poly, poly, out);
}

static int solve_rat_poly_divide(const SolveRatPoly *num, const SolveRatPoly *den, SolveRatPoly *quotient_out, SolveRatPoly *remainder_out) {
    SolveRatPoly remainder = *num;
    int den_degree = solve_rat_poly_degree(den);
    int rem_degree = solve_rat_poly_degree(&remainder);
    solve_rat_poly_zero(quotient_out);
    if (den_degree < 0) return -1;
    while (rem_degree >= den_degree && rem_degree >= 0) {
        int shift = rem_degree - den_degree;
        SolveRat factor;
        int i;
        if (solve_rat_div(remainder.coeff[rem_degree], den->coeff[den_degree], &factor) != 0) return -1;
        quotient_out->coeff[shift] = factor;
        for (i = 0; i <= den_degree; ++i) {
            SolveRat product;
            if (solve_rat_mul(factor, den->coeff[i], &product) != 0 || solve_rat_sub(remainder.coeff[i + shift], product, &remainder.coeff[i + shift]) != 0) return -1;
        }
        rem_degree = solve_rat_poly_degree(&remainder);
    }
    *remainder_out = remainder;
    return 0;
}

static void solve_write_rat_value(SolveRat value) {
    char text[96];
    if (solve_rat_format(value, text, sizeof(text)) != 0) solve_sp_cstr(1, "?");
    else solve_sp_cstr(1, text);
}

static void solve_write_point_rat(SolveRat x, SolveRat y) {
    solve_sp_cstr(1, "(");
    solve_write_rat_value(x);
    solve_sp_cstr(1, ", ");
    solve_write_rat_value(y);
    solve_sp_cstr(1, ")");
}


#endif
