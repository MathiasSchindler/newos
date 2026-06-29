#include "runtime.h"
#include "tool_util.h"

#ifndef SOLVE_PRIVATE_CONTEXT
#define SOLVE_PRIVATE_CONTEXT 1
#endif
#ifndef SOLVE_FROM_SOLVE_C
#define SOLVE_FROM_SOLVE_C 1
#endif

#define SOLVE_EXPR_CAPACITY 2048U
#define SOLVE_NAME_CAPACITY 32U
#define SOLVE_MAX_RESULTS 64U
#define SOLVE_DEFAULT_SCAN_LO (-100.0)
#define SOLVE_DEFAULT_SCAN_HI 100.0
#define SOLVE_DEFAULT_SCAN_STEPS 400
#define SOLVE_DEFAULT_SCALE 10
#define SOLVE_MAX_SCALE 15
#define SOLVE_INTERNAL_SCALE 20
#define SOLVE_DEFAULT_TOLERANCE 0.0000000001
#define SOLVE_DEFAULT_MAX_ITERATIONS 128
#define SOLVE_MAX_RATIONAL_DENOMINATOR 1000
#define SOLVE_POLY_MAX_DEGREE 16
#define SOLVE_RAT_POLY_MAX_DEGREE 8
#define SOLVE_POLY_FACTOR_DENOMINATOR_LIMIT 100
#define SOLVE_RAT_LIMIT 900000000000000000LL
#define SOLVE_RAT_DIVISOR_LIMIT 1000000ULL
#define SOLVE_RAT_MAX_DIVISORS 512
#define SOLVE_MAX_PARAMS 8
#define SOLVE_PI 3.14159265358979323846264338327950288419716939937510
#define SOLVE_E 2.71828182845904523536028747135266249775724709369995
#define SOLVE_HUGE 1.0e290

typedef enum {
    SOLVE_STATUS_ROOT = 0,
    SOLVE_STATUS_CANDIDATE,
    SOLVE_STATUS_SUSPECT_DISCONTINUITY
} SolveStatus;

typedef enum {
    SOLVE_RELATION_NONE = 0,
    SOLVE_RELATION_EQ,
    SOLVE_RELATION_LT,
    SOLVE_RELATION_LE,
    SOLVE_RELATION_GT,
    SOLVE_RELATION_GE
} SolveRelation;

typedef struct {
    const char *text;
    size_t pos;
    const char *var_name;
    double var_value;
    int error;
    const char *message;
} SolveExprParser;

typedef struct {
    const char *text;
    size_t pos;
    const char *var_name;
    int error;
} SolvePolyParser;

typedef struct {
    double coeff[SOLVE_POLY_MAX_DEGREE + 1];
    int exact;
} SolvePoly;

typedef struct {
    long long num;
    long long den;
} SolveRat;

typedef struct {
    SolveRat coeff[SOLVE_RAT_POLY_MAX_DEGREE + 1];
} SolveRatPoly;

typedef struct {
    const char *text;
    size_t pos;
    const char *var_name;
    int error;
} SolveRatParser;

typedef struct {
    char left[SOLVE_EXPR_CAPACITY];
    char right[SOLVE_EXPR_CAPACITY];
    int has_equation;
    SolveRelation relation;
} SolveEquation;

typedef struct {
    char var_name[SOLVE_NAME_CAPACITY];
    int have_scan;
    int default_scan;
    int have_bracket;
    double scan_lo;
    double scan_hi;
    int scan_steps;
    double lo;
    double hi;
    int all;
    int report_y;
    int explain;
    int quiet;
    int have_diff;
    int diff_order;
    int have_integrate;
    char integrate_spec[128];
    int have_antiderivative;
    int have_monotonicity;
    int have_curvature;
    int have_tangent;
    int have_normal;
    int have_end_behavior;
    int have_discuss;
    int have_area;
    int have_volume;
    int have_mean;
    int have_eval;
    int have_at;
    int have_subst;
    int have_average_rate;
    int have_minimum;
    int have_maximum;
    int have_area_quadrant;
    int have_fit_exp_asymptote;
    int have_limit;
    int have_asymptotes;
    int param_count;
    char param_names[SOLVE_MAX_PARAMS][SOLVE_NAME_CAPACITY];
    char at_spec[128];
    char subst_spec[128];
    char fit_points_spec[128];
    char point_spec[128];
    char range_spec[128];
    char limit_spec[128];
    double fit_asymptote;
    int scale;
    double tolerance;
    int max_iterations;
    const char *method;
} SolveOptions;

typedef struct {
    double root;
    double y;
    double residual;
    double lo;
    double hi;
    int iterations;
    SolveStatus status;
    const char *method;
    int approximate;
    char exact_value[96];
} SolveResult;

typedef struct {
    SolveResult results[SOLVE_MAX_RESULTS];
    size_t count;
    int identity;
    int no_real_solutions;
    int numeric_failure;
    int suspected_discontinuity;
} SolveResultSet;

typedef struct {
    double value;
    SolveRat rat_value;
    char label[96];
    int exact;
    int pole;
} SolveBreakpoint;

typedef struct {
    int has_left;
    int has_right;
    double left;
    double right;
    char left_label[96];
    char right_label[96];
    int left_closed;
    int right_closed;
} SolveInterval;

static int solve_add_result(SolveResultSet *set, const SolveResult *result, int all, double tolerance);
static int solve_eval_function(const SolveEquation *equation, const SolveOptions *options, double x, double *value_out, const char **message_out);
static int solve_eval_y(const SolveEquation *equation, const SolveOptions *options, double x, double *value_out);
static int solve_root_in_scan_range(const SolveOptions *options, double root);
static void solve_sort_breakpoints(SolveBreakpoint *points, int *count_io, double tolerance);
static int solve_collect_rat_poly_roots(const SolveRatPoly *input, SolveBreakpoint *points, int *count_out);
static void solve_print_rat_roots_line(const char *label, SolveBreakpoint *points, int count);
static int solve_copy_range(char *dst, size_t dst_size, const char *src, size_t start, size_t end);
static int solve_rat_poly_format_antiderivative(const SolveRatPoly *poly, const char *var_name, char *buffer, size_t buffer_size);
static void solve_numeric_analysis_bounds(const SolveOptions *options, double *lo_out, double *hi_out);
static double solve_numeric_derivative_value(const SolveEquation *equation, const SolveOptions *options, double x, int order, int *ok_out);
static int solve_numeric_derivative_roots(const SolveEquation *equation, const SolveOptions *options, int order, double *roots, int *count_out);

static int solve_is_param_name(const SolveOptions *options, const char *name) {
    int i;
    for (i = 0; i < options->param_count; ++i) {
        if (rt_strcmp(options->param_names[i], name) == 0) return 1;
    }
    return 0;
}

static double solve_abs(double value) {
    return value < 0.0 ? -value : value;
}

static int solve_is_bad(double value) {
    return value != value || value > SOLVE_HUGE || value < -SOLVE_HUGE;
}

static int solve_append_char(char *buffer, size_t buffer_size, size_t *length_io, char ch) {
    if (*length_io + 1U >= buffer_size) {
        return -1;
    }
    buffer[*length_io] = ch;
    *length_io += 1U;
    buffer[*length_io] = '\0';
    return 0;
}

static int solve_append_text(char *buffer, size_t buffer_size, size_t *length_io, const char *text) {
    while (*text != '\0') {
        if (solve_append_char(buffer, buffer_size, length_io, *text) != 0) {
            return -1;
        }
        text += 1;
    }
    return 0;
}

static int solve_contains_char(const char *text, char ch) {
    while (*text != '\0') {
        if (*text == ch) {
            return 1;
        }
        text += 1;
    }
    return 0;
}

static void solve_format_double(double value, int scale, char *buffer, size_t buffer_size) {
    char whole_digits[64];
    char frac_digits[32];
    size_t length = 0U;
    unsigned long long pow10 = 1ULL;
    unsigned long long scaled;
    unsigned long long whole;
    unsigned long long fraction;
    int negative = 0;
    int i;

    if (buffer_size == 0U) {
        return;
    }
    buffer[0] = '\0';

    if (solve_is_bad(value)) {
        rt_copy_string(buffer, buffer_size, "nan");
        return;
    }
    if (scale < 0) {
        scale = SOLVE_DEFAULT_SCALE;
    }
    if (scale > SOLVE_MAX_SCALE) {
        scale = SOLVE_MAX_SCALE;
    }
    if (value < 0.0) {
        negative = 1;
        value = -value;
    }
    for (i = 0; i < scale; ++i) {
        pow10 *= 10ULL;
    }
    scaled = (unsigned long long)(value * (double)pow10 + 0.5);
    whole = scaled / pow10;
    fraction = scaled % pow10;

    if (negative && scaled != 0ULL) {
        (void)solve_append_char(buffer, buffer_size, &length, '-');
    }
    {
        size_t digit_count = 0U;
        unsigned long long temp = whole;
        if (temp == 0ULL) {
            whole_digits[digit_count++] = '0';
        } else {
            while (temp > 0ULL && digit_count < sizeof(whole_digits)) {
                whole_digits[digit_count++] = (char)('0' + (temp % 10ULL));
                temp /= 10ULL;
            }
        }
        while (digit_count > 0U) {
            (void)solve_append_char(buffer, buffer_size, &length, whole_digits[--digit_count]);
        }
    }
    if (scale > 0) {
        (void)solve_append_char(buffer, buffer_size, &length, '.');
        for (i = scale - 1; i >= 0; --i) {
            frac_digits[i] = (char)('0' + (fraction % 10ULL));
            fraction /= 10ULL;
        }
        frac_digits[scale] = '\0';
        (void)solve_append_text(buffer, buffer_size, &length, frac_digits);
    }
}

static unsigned long long solve_gcd_ull(unsigned long long a, unsigned long long b) {
    while (b != 0ULL) {
        unsigned long long r = a % b;
        a = b;
        b = r;
    }
    return a;
}

static int solve_format_rational_parts(double value, char *buffer, size_t buffer_size, double *candidate_out) {
    double absolute;
    unsigned long long best_num = 0ULL;
    unsigned long long best_den = 1ULL;
    double best_error = 1.0;
    int negative = value < 0.0;
    unsigned int den;
    unsigned long long gcd;
    unsigned long long whole;
    unsigned long long remainder;
    char number[64];
    size_t length = 0U;

    if (buffer_size == 0U || solve_is_bad(value)) {
        return -1;
    }
    absolute = negative ? -value : value;
    if (absolute > 1000000000.0) {
        return -1;
    }
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
        if (error <= SOLVE_DEFAULT_TOLERANCE * 2.0) {
            break;
        }
    }
    if (best_error > SOLVE_DEFAULT_TOLERANCE * 2.0) {
        return -1;
    }
    if (candidate_out != 0) {
        *candidate_out = (negative ? -1.0 : 1.0) * ((double)best_num / (double)best_den);
    }
    gcd = solve_gcd_ull(best_num, best_den);
    if (gcd != 0ULL) {
        best_num /= gcd;
        best_den /= gcd;
    }
    if (best_den == 1ULL) {
        return -1;
    }

    buffer[0] = '\0';
    if (negative && solve_append_char(buffer, buffer_size, &length, '-') != 0) return -1;
    whole = best_num / best_den;
    remainder = best_num % best_den;
    if (whole > 0ULL) {
        solve_format_double((double)whole, 0, number, sizeof(number));
        if (solve_append_text(buffer, buffer_size, &length, number) != 0) return -1;
        if (solve_append_char(buffer, buffer_size, &length, ' ') != 0) return -1;
    }
    solve_format_double((double)remainder, 0, number, sizeof(number));
    if (solve_append_text(buffer, buffer_size, &length, number) != 0) return -1;
    if (solve_append_char(buffer, buffer_size, &length, '/') != 0) return -1;
    solve_format_double((double)best_den, 0, number, sizeof(number));
    if (solve_append_text(buffer, buffer_size, &length, number) != 0) return -1;
    return 0;
}

static int solve_format_rational(double value, char *buffer, size_t buffer_size) {
    return solve_format_rational_parts(value, buffer, buffer_size, 0);
}

static unsigned long long solve_abs_ll(long long value) {
    if (value < 0) {
        return (unsigned long long)(-(value + 1)) + 1ULL;
    }
    return (unsigned long long)value;
}

static unsigned long long solve_gcd_ll(long long a, long long b) {
    return solve_gcd_ull(solve_abs_ll(a), solve_abs_ll(b));
}

static int solve_i128_to_ll(__int128 value, long long *out) {
    if (value < -((__int128)SOLVE_RAT_LIMIT) || value > (__int128)SOLVE_RAT_LIMIT) {
        return -1;
    }
    *out = (long long)value;
    return 0;
}

static int solve_rat_make_i128(__int128 num, __int128 den, SolveRat *out) {
    long long n;
    long long d;
    unsigned long long gcd;

    if (den == 0) {
        return -1;
    }
    if (den < 0) {
        num = -num;
        den = -den;
    }
    if (solve_i128_to_ll(num, &n) != 0 || solve_i128_to_ll(den, &d) != 0) {
        return -1;
    }
    gcd = solve_gcd_ll(n, d);
    if (gcd > 1ULL) {
        n /= (long long)gcd;
        d /= (long long)gcd;
    }
    if (d < 0) {
        n = -n;
        d = -d;
    }
    out->num = n;
    out->den = d;
    return 0;
}

static int solve_rat_make(long long num, long long den, SolveRat *out) {
    return solve_rat_make_i128((__int128)num, (__int128)den, out);
}

static int solve_rat_add(SolveRat a, SolveRat b, SolveRat *out) {
    return solve_rat_make_i128((__int128)a.num * b.den + (__int128)b.num * a.den, (__int128)a.den * b.den, out);
}

static int solve_rat_sub(SolveRat a, SolveRat b, SolveRat *out) {
    return solve_rat_make_i128((__int128)a.num * b.den - (__int128)b.num * a.den, (__int128)a.den * b.den, out);
}

static int solve_rat_mul(SolveRat a, SolveRat b, SolveRat *out) {
    return solve_rat_make_i128((__int128)a.num * b.num, (__int128)a.den * b.den, out);
}

static int solve_rat_div(SolveRat a, SolveRat b, SolveRat *out) {
    if (b.num == 0) {
        return -1;
    }
    return solve_rat_make_i128((__int128)a.num * b.den, (__int128)a.den * b.num, out);
}

static int solve_rat_neg(SolveRat value, SolveRat *out) {
    return solve_rat_make_i128(-((__int128)value.num), value.den, out);
}

static int solve_rat_is_zero(SolveRat value) {
    return value.num == 0;
}

static double solve_rat_to_double(SolveRat value) {
    return (double)value.num / (double)value.den;
}

static int solve_append_signed_ll(char *buffer, size_t buffer_size, size_t *length_io, long long value) {
    char digits[32];
    size_t count = 0U;
    unsigned long long magnitude = solve_abs_ll(value);

    if (value < 0 && solve_append_char(buffer, buffer_size, length_io, '-') != 0) return -1;
    if (magnitude == 0ULL) {
        return solve_append_char(buffer, buffer_size, length_io, '0');
    }
    while (magnitude > 0ULL && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (magnitude % 10ULL));
        magnitude /= 10ULL;
    }
    while (count > 0U) {
        if (solve_append_char(buffer, buffer_size, length_io, digits[--count]) != 0) return -1;
    }
    return 0;
}

static int solve_rat_format(SolveRat value, char *buffer, size_t buffer_size) {
    size_t length = 0U;

    if (buffer_size == 0U) return -1;
    buffer[0] = '\0';
    if (solve_append_signed_ll(buffer, buffer_size, &length, value.num) != 0) return -1;
    if (value.den != 1) {
        if (solve_append_char(buffer, buffer_size, &length, '/') != 0) return -1;
        if (solve_append_signed_ll(buffer, buffer_size, &length, value.den) != 0) return -1;
    }
    return 0;
}

static int solve_parse_rat_literal(const char *text, size_t *pos_io, SolveRat *out) {
    size_t pos = *pos_io;
    __int128 mantissa = 0;
    __int128 den = 1;
    int saw_digit = 0;
    int frac_digits = 0;
    int exp = 0;
    int exp_negative = 0;

    while (text[pos] >= '0' && text[pos] <= '9') {
        mantissa = mantissa * 10 + (text[pos] - '0');
        if (mantissa > (__int128)SOLVE_RAT_LIMIT) return -1;
        saw_digit = 1;
        pos += 1U;
    }
    if (text[pos] == '.') {
        pos += 1U;
        while (text[pos] >= '0' && text[pos] <= '9') {
            mantissa = mantissa * 10 + (text[pos] - '0');
            den *= 10;
            if (mantissa > (__int128)SOLVE_RAT_LIMIT || den > (__int128)SOLVE_RAT_LIMIT) return -1;
            saw_digit = 1;
            frac_digits += 1;
            pos += 1U;
        }
    }
    (void)frac_digits;
    if (!saw_digit) return -1;
    if (text[pos] == 'e' || text[pos] == 'E') {
        pos += 1U;
        if (text[pos] == '+' || text[pos] == '-') {
            exp_negative = text[pos] == '-';
            pos += 1U;
        }
        if (text[pos] < '0' || text[pos] > '9') return -1;
        while (text[pos] >= '0' && text[pos] <= '9') {
            exp = exp * 10 + (text[pos] - '0');
            if (exp > 18) return -1;
            pos += 1U;
        }
        while (exp > 0) {
            if (exp_negative) den *= 10;
            else mantissa *= 10;
            if (mantissa > (__int128)SOLVE_RAT_LIMIT || den > (__int128)SOLVE_RAT_LIMIT) return -1;
            exp -= 1;
        }
    }
    if (solve_rat_make_i128(mantissa, den, out) != 0) return -1;
    *pos_io = pos;
    return 0;
}

static void solve_format_compact_decimal(double value, int scale, char *buffer, size_t buffer_size) {
    long long nearest_integer;

    if (value >= 0.0) {
        nearest_integer = (long long)(value + 0.5);
    } else {
        nearest_integer = (long long)(value - 0.5);
    }
    if (solve_abs(value - (double)nearest_integer) <= SOLVE_DEFAULT_TOLERANCE * 2.0) {
        solve_format_double((double)nearest_integer, 0, buffer, buffer_size);
        return;
    }
    solve_format_double(value, scale, buffer, buffer_size);
}

static void solve_format_answer(double value, int scale, char *buffer, size_t buffer_size) {
    char decimal[96];
    char rational[96];
    size_t length = 0U;

    solve_format_compact_decimal(value, scale, decimal, sizeof(decimal));
    rt_copy_string(buffer, buffer_size, decimal);
    if (solve_format_rational(value, rational, sizeof(rational)) == 0) {
        length = rt_strlen(buffer);
        if (solve_append_text(buffer, buffer_size, &length, " (") == 0 &&
            solve_append_text(buffer, buffer_size, &length, rational) == 0) {
            (void)solve_append_char(buffer, buffer_size, &length, ')');
        }
    }
}

static void solve_format_result_answer(const SolveEquation *equation, const SolveOptions *options, const SolveResult *result, char *buffer, size_t buffer_size) {
    char decimal[96];
    char rational[96];
    double candidate;
    double residual;
    const char *message = 0;
    size_t length;

    solve_format_compact_decimal(result->root, options->scale, buffer, buffer_size);
    if (solve_contains_char(buffer, '(') || solve_format_rational_parts(result->root, rational, sizeof(rational), &candidate) != 0) {
        return;
    }
    if (solve_abs(candidate - result->root) > options->tolerance * 10.0) {
        return;
    }
    if (solve_eval_function(equation, options, candidate, &residual, &message) != 0 || solve_abs(residual) > options->tolerance * 10.0) {
        return;
    }
    solve_format_double(result->root, options->scale, decimal, sizeof(decimal));
    rt_copy_string(buffer, buffer_size, decimal);
    length = rt_strlen(buffer);
    if (solve_append_text(buffer, buffer_size, &length, " (") == 0 &&
        solve_append_text(buffer, buffer_size, &length, rational) == 0) {
        (void)solve_append_char(buffer, buffer_size, &length, ')');
    }
}

#include "solve/eval.c"

#include "solve/poly.c"

#include "solve/exact_poly.c"

#include "solve/roots.c"

static int solve_copy_range(char *dst, size_t dst_size, const char *src, size_t start, size_t end) {
    size_t used = 0U;

    while (start < end && (src[start] == ' ' || src[start] == '\t' || src[start] == '\n' || src[start] == '\r')) {
        start += 1U;
    }
    while (end > start && (src[end - 1U] == ' ' || src[end - 1U] == '\t' || src[end - 1U] == '\n' || src[end - 1U] == '\r')) {
        end -= 1U;
    }
    while (start < end) {
        if (used + 1U >= dst_size) {
            return -1;
        }
        dst[used++] = src[start++];
    }
    dst[used] = '\0';
    return used == 0U ? -1 : 0;
}

static int solve_parse_equation(const char *text, SolveEquation *equation) {
    size_t index;
    int depth = 0;
    int found = 0;
    size_t relation_pos = 0U;
    size_t relation_len = 0U;
    SolveRelation relation = SOLVE_RELATION_NONE;

    for (index = 0U; text[index] != '\0'; ++index) {
        char ch = text[index];
        if (ch == '(') {
            depth += 1;
        } else if (ch == ')' && depth > 0) {
            depth -= 1;
        } else if (depth == 0) {
            SolveRelation candidate = SOLVE_RELATION_NONE;
            size_t candidate_len = 0U;
            if (ch == '<') {
                candidate = text[index + 1U] == '=' ? SOLVE_RELATION_LE : SOLVE_RELATION_LT;
                candidate_len = text[index + 1U] == '=' ? 2U : 1U;
            } else if (ch == '>') {
                candidate = text[index + 1U] == '=' ? SOLVE_RELATION_GE : SOLVE_RELATION_GT;
                candidate_len = text[index + 1U] == '=' ? 2U : 1U;
            } else if (ch == '=') {
                char prev = index == 0U ? '\0' : text[index - 1U];
                char next = text[index + 1U];
                if (prev != '<' && prev != '>' && prev != '!' && prev != '=' && next != '=') {
                    candidate = SOLVE_RELATION_EQ;
                    candidate_len = 1U;
                }
            }
            if (candidate != SOLVE_RELATION_NONE) {
                if (found) return -1;
                found = 1;
                relation_pos = index;
                relation_len = candidate_len;
                relation = candidate;
                if (candidate_len == 2U) index += 1U;
            }
        }
    }

    if (found) {
        if (solve_copy_range(equation->left, sizeof(equation->left), text, 0U, relation_pos) != 0 ||
            solve_copy_range(equation->right, sizeof(equation->right), text, relation_pos + relation_len, rt_strlen(text)) != 0) {
            return -1;
        }
        equation->has_equation = 1;
        equation->relation = relation;
    } else {
        if (solve_copy_range(equation->left, sizeof(equation->left), text, 0U, rt_strlen(text)) != 0) {
            return -1;
        }
        rt_copy_string(equation->right, sizeof(equation->right), "0");
        equation->has_equation = 0;
        equation->relation = SOLVE_RELATION_NONE;
    }
    return 0;
}

static int solve_parse_scan(const char *text, double *lo_out, double *hi_out, int *steps_out) {
    size_t index = 0U;
    double lo;
    double hi;
    double steps_value;

    if (solve_parse_double(text, &index, &lo) != 0 || text[index] != ':') {
        return -1;
    }
    index += 1U;
    if (solve_parse_double(text, &index, &hi) != 0) {
        return -1;
    }
    if (text[index] == ':') {
        index += 1U;
        if (solve_parse_double(text, &index, &steps_value) != 0 || steps_value < 1.0 || steps_value > 100000.0) {
            return -1;
        }
        *steps_out = (int)steps_value;
    } else {
        *steps_out = SOLVE_DEFAULT_SCAN_STEPS;
    }
    if (text[index] != '\0' || lo == hi) {
        return -1;
    }
    *lo_out = lo;
    *hi_out = hi;
    return 0;
}


#include "solve/solver.c"

#include "solve/symbolic.c"

#include "solve/calculus.c"

#include "solve/inequality.c"

#include "solve/analysis.c"

static void solve_options_init(SolveOptions *options) {
    rt_copy_string(options->var_name, sizeof(options->var_name), "x");
    options->have_scan = 0;
    options->default_scan = 0;
    options->have_bracket = 0;
    options->scan_lo = SOLVE_DEFAULT_SCAN_LO;
    options->scan_hi = SOLVE_DEFAULT_SCAN_HI;
    options->scan_steps = SOLVE_DEFAULT_SCAN_STEPS;
    options->lo = 0.0;
    options->hi = 0.0;
    options->all = 0;
    options->report_y = 0;
    options->explain = 0;
    options->quiet = 0;
    options->have_diff = 0;
    options->diff_order = 1;
    options->have_integrate = 0;
    options->integrate_spec[0] = '\0';
    options->have_antiderivative = 0;
    options->have_monotonicity = 0;
    options->have_curvature = 0;
    options->have_tangent = 0;
    options->have_normal = 0;
    options->have_end_behavior = 0;
    options->have_discuss = 0;
    options->have_area = 0;
    options->have_volume = 0;
    options->have_mean = 0;
    options->have_eval = 0;
    options->have_at = 0;
    options->have_subst = 0;
    options->have_average_rate = 0;
    options->have_minimum = 0;
    options->have_maximum = 0;
    options->have_area_quadrant = 0;
    options->have_fit_exp_asymptote = 0;
    options->have_limit = 0;
    options->have_asymptotes = 0;
    options->param_count = 0;
    options->at_spec[0] = '\0';
    options->subst_spec[0] = '\0';
    options->fit_points_spec[0] = '\0';
    options->point_spec[0] = '\0';
    options->range_spec[0] = '\0';
    options->limit_spec[0] = '\0';
    options->fit_asymptote = 0.0;
    options->scale = SOLVE_DEFAULT_SCALE;
    options->tolerance = SOLVE_DEFAULT_TOLERANCE;
    options->max_iterations = SOLVE_DEFAULT_MAX_ITERATIONS;
    options->method = "auto";
}

static int solve_join_expression(int start, int argc, char **argv, char *buffer, size_t buffer_size) {
    size_t used = 0U;
    int i;

    if (start >= argc) {
        return -1;
    }
    buffer[0] = '\0';
    for (i = start; i < argc; ++i) {
        size_t len = rt_strlen(argv[i]);
        if (used + len + 2U > buffer_size) {
            return -1;
        }
        if (used > 0U) {
            buffer[used++] = ' ';
        }
        while (*argv[i] != '\0') {
            buffer[used++] = *argv[i]++;
        }
        buffer[used] = '\0';
    }
    return 0;
}

int main(int argc, char **argv) {
    SolveOptions options;
    SolveEquation equation;
    ToolOptState opt;
    char expression[SOLVE_EXPR_CAPACITY];
    int opt_result;

    solve_options_init(&options);
    tool_opt_init(&opt, argc, argv, tool_base_name(argv[0]), "[options] 'EXPRESSION = EXPRESSION'");
    while ((opt_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--var") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 2;
            if (!tool_ascii_is_identifier_start(opt.value[0]) || rt_strlen(opt.value) >= sizeof(options.var_name)) {
                tool_write_error("solve", "invalid variable name", opt.value);
                return 2;
            }
            rt_copy_string(options.var_name, sizeof(options.var_name), opt.value);
        } else if (rt_strcmp(opt.flag, "--lo") == 0) {
            if (tool_opt_require_value(&opt) != 0 || solve_parse_double_arg(opt.value, &options.lo) != 0) {
                tool_write_error("solve", "invalid --lo value", 0);
                return 2;
            }
            options.have_bracket = 1;
        } else if (rt_strcmp(opt.flag, "--hi") == 0) {
            if (tool_opt_require_value(&opt) != 0 || solve_parse_double_arg(opt.value, &options.hi) != 0) {
                tool_write_error("solve", "invalid --hi value", 0);
                return 2;
            }
            options.have_bracket = 1;
        } else if (rt_strcmp(opt.flag, "--scan") == 0) {
            if (tool_opt_require_value(&opt) != 0 || solve_parse_scan(opt.value, &options.scan_lo, &options.scan_hi, &options.scan_steps) != 0) {
                tool_write_error("solve", "invalid --scan value", opt.value);
                return 2;
            }
            options.have_scan = 1;
        } else if (rt_strcmp(opt.flag, "--all") == 0) {
            options.all = 1;
        } else if (rt_strcmp(opt.flag, "--report-y") == 0) {
            options.report_y = 1;
        } else if (rt_strcmp(opt.flag, "--explain") == 0) {
            options.explain = 1;
        } else if (rt_strcmp(opt.flag, "--quiet") == 0) {
            options.quiet = 1;
        } else if (rt_strcmp(opt.flag, "--param") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 2;
            if (options.param_count >= SOLVE_MAX_PARAMS || !tool_ascii_is_identifier_start(opt.value[0]) || rt_strlen(opt.value) >= SOLVE_NAME_CAPACITY) {
                tool_write_error("solve", "invalid parameter name", opt.value);
                return 2;
            }
            rt_copy_string(options.param_names[options.param_count++], SOLVE_NAME_CAPACITY, opt.value);
        } else if (rt_strcmp(opt.flag, "--eval") == 0) {
            options.have_eval = 1;
        } else if (rt_strcmp(opt.flag, "--at") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.at_spec)) return 2;
            options.have_at = 1;
            options.have_eval = 1;
            rt_copy_string(options.at_spec, sizeof(options.at_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--subst") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.subst_spec)) return 2;
            options.have_subst = 1;
            rt_copy_string(options.subst_spec, sizeof(options.subst_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--diff") == 0 || tool_starts_with(opt.flag, "--diff=")) {
            const char *value = 0;
            unsigned long long order = 1ULL;
            options.have_diff = 1;
            if (tool_starts_with(opt.flag, "--diff=")) {
                value = opt.flag + 7;
                if (rt_parse_uint(value, &order) != 0 || order > 64ULL) {
                    tool_write_error("solve", "invalid --diff order", value);
                    return 2;
                }
            }
            options.diff_order = (int)order;
        } else if (rt_strcmp(opt.flag, "--integrate") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.integrate_spec)) return 2;
            options.have_integrate = 1;
            rt_copy_string(options.integrate_spec, sizeof(options.integrate_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--antiderivative") == 0) {
            options.have_antiderivative = 1;
        } else if (rt_strcmp(opt.flag, "--monotonicity") == 0) {
            options.have_monotonicity = 1;
        } else if (rt_strcmp(opt.flag, "--curvature") == 0) {
            options.have_curvature = 1;
        } else if (rt_strcmp(opt.flag, "--tangent") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.point_spec)) return 2;
            options.have_tangent = 1;
            rt_copy_string(options.point_spec, sizeof(options.point_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--normal") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.point_spec)) return 2;
            options.have_normal = 1;
            rt_copy_string(options.point_spec, sizeof(options.point_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--end-behavior") == 0) {
            options.have_end_behavior = 1;
        } else if (rt_strcmp(opt.flag, "--discuss") == 0) {
            options.have_discuss = 1;
        } else if (rt_strcmp(opt.flag, "--area") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.range_spec)) return 2;
            options.have_area = 1;
            rt_copy_string(options.range_spec, sizeof(options.range_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--area-quadrant") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.range_spec)) return 2;
            options.have_area = 1;
            options.have_area_quadrant = 1;
            rt_copy_string(options.range_spec, sizeof(options.range_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--volume") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.range_spec)) return 2;
            options.have_volume = 1;
            rt_copy_string(options.range_spec, sizeof(options.range_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--mean") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.range_spec)) return 2;
            options.have_mean = 1;
            rt_copy_string(options.range_spec, sizeof(options.range_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--average-rate") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.range_spec)) return 2;
            options.have_average_rate = 1;
            rt_copy_string(options.range_spec, sizeof(options.range_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--range") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.range_spec)) return 2;
            rt_copy_string(options.range_spec, sizeof(options.range_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--max") == 0) {
            options.have_maximum = 1;
        } else if (rt_strcmp(opt.flag, "--min") == 0) {
            options.have_minimum = 1;
        } else if (rt_strcmp(opt.flag, "--limit") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.limit_spec)) return 2;
            options.have_limit = 1;
            rt_copy_string(options.limit_spec, sizeof(options.limit_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--asymptotes") == 0) {
            options.have_asymptotes = 1;
        } else if (rt_strcmp(opt.flag, "--fit-exp-asymptote") == 0) {
            if (tool_opt_require_value(&opt) != 0 || solve_parse_double_arg(opt.value, &options.fit_asymptote) != 0) return 2;
            options.have_fit_exp_asymptote = 1;
        } else if (rt_strcmp(opt.flag, "--points") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.fit_points_spec)) return 2;
            rt_copy_string(options.fit_points_spec, sizeof(options.fit_points_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--method") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 2;
            if (rt_strcmp(opt.value, "auto") != 0 && rt_strcmp(opt.value, "bisection") != 0) {
                tool_write_error("solve", "unsupported method", opt.value);
                return 2;
            }
            options.method = opt.value;
        } else if (rt_strcmp(opt.flag, "--scale") == 0) {
            unsigned long long value;
            if (tool_opt_require_value(&opt) != 0 || rt_parse_uint(opt.value, &value) != 0 || value > (unsigned long long)SOLVE_MAX_SCALE) {
                tool_write_error("solve", "invalid --scale value", opt.value);
                return 2;
            }
            options.scale = (int)value;
        } else if (rt_strcmp(opt.flag, "--tolerance") == 0) {
            if (tool_opt_require_value(&opt) != 0 || solve_parse_double_arg(opt.value, &options.tolerance) != 0 || options.tolerance <= 0.0) {
                tool_write_error("solve", "invalid --tolerance value", opt.value);
                return 2;
            }
        } else if (rt_strcmp(opt.flag, "--max-iterations") == 0) {
            unsigned long long value;
            if (tool_opt_require_value(&opt) != 0 || rt_parse_uint(opt.value, &value) != 0 || value == 0ULL || value > 100000ULL) {
                tool_write_error("solve", "invalid --max-iterations value", opt.value);
                return 2;
            }
            options.max_iterations = (int)value;
        } else {
            tool_write_error("solve", "unknown option: ", opt.flag);
            tool_write_usage("solve", "[options] 'EXPRESSION = EXPRESSION'");
            return 2;
        }
    }
    if (opt_result == TOOL_OPT_HELP) {
        tool_write_usage("solve", "[options] 'EXPRESSION = EXPRESSION'");
        return 0;
    }
    if (opt_result == TOOL_OPT_ERROR) {
        return 2;
    }
    if (options.have_scan && options.have_bracket) {
        tool_write_error("solve", "--scan cannot be combined with --lo/--hi", 0);
        return 2;
    }
    if (options.have_bracket && options.lo == options.hi) {
        tool_write_error("solve", "empty interval", 0);
        return 2;
    }
    if (!options.have_scan && !options.have_bracket) {
        options.have_scan = 1;
        options.default_scan = 1;
    }
    if (options.have_fit_exp_asymptote) {
        if (options.fit_points_spec[0] == '\0') {
            tool_write_error("solve", "--fit-exp-asymptote requires --points", 0);
            return 2;
        }
        return solve_run_fit_exp_mode(&options);
    }
    if (options.have_area) {
        return solve_run_area_mode(&options, opt.argi, argc, argv);
    }
    if (solve_join_expression(opt.argi, argc, argv, expression, sizeof(expression)) != 0) {
        tool_write_error("solve", "missing or too large expression", 0);
        return 2;
    }
    if (options.have_subst) {
        return solve_run_subst_mode(&options, expression);
    }
    if (options.have_eval) {
        return solve_run_eval_mode(&options, expression);
    }
    if (options.have_limit) {
        return solve_run_limit_mode(&options, expression);
    }
    if (options.have_volume || options.have_mean) {
        return solve_run_volume_mean_mode(&options, expression, options.have_volume);
    }
    if (options.have_asymptotes) {
        return solve_run_asymptotes_mode(&options, expression);
    }
    if (solve_parse_equation(expression, &equation) != 0) {
        tool_write_error("solve", "invalid equation", 0);
        return 2;
    }
    {
        const char *validation_message = 0;
        if (solve_validate_identifiers(equation.left, &options, &validation_message) != 0 ||
            solve_validate_identifiers(equation.right, &options, &validation_message) != 0) {
            tool_write_error("solve", validation_message != 0 ? validation_message : "invalid expression", 0);
            return 2;
        }
    }

    if (options.have_diff && options.have_integrate) {
        tool_write_error("solve", "--diff and --integrate cannot be combined", 0);
        return 2;
    }
    if (options.have_antiderivative) {
        return solve_run_antiderivative_mode(&equation, &options);
    }
    if (options.have_monotonicity) {
        return solve_run_monotonicity_mode(&equation, &options, 0);
    }
    if (options.have_curvature) {
        return solve_run_monotonicity_mode(&equation, &options, 1);
    }
    if (options.have_tangent || options.have_normal) {
        return solve_run_tangent_normal_mode(&equation, &options, options.have_normal);
    }
    if (options.have_end_behavior) {
        return solve_run_end_behavior_mode(&equation, &options);
    }
    if (options.have_discuss) {
        return solve_run_discuss_mode(&equation, &options);
    }
    if (options.have_average_rate) {
        return solve_run_average_rate_mode(&equation, &options);
    }
    if (options.have_maximum || options.have_minimum) {
        if (options.range_spec[0] == '\0') {
            rt_copy_string(options.range_spec, sizeof(options.range_spec), "-10:10");
        }
        return solve_run_extreme_mode(&equation, &options, options.have_maximum);
    }
    if (options.have_integrate) {
        return solve_run_integrate_mode(&equation, &options);
    }
    if (options.have_diff) {
        return solve_run_diff_mode(&equation, &options);
    }
    if (equation.relation == SOLVE_RELATION_LT || equation.relation == SOLVE_RELATION_LE || equation.relation == SOLVE_RELATION_GT || equation.relation == SOLVE_RELATION_GE) {
        return solve_run_inequality_mode(&equation, &options);
    }

    return solve_run_solver_equation(&equation, &options);
}
