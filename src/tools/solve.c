#include "runtime.h"
#include "tool_util.h"

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
    int have_limit;
    int have_asymptotes;
    char point_spec[128];
    char range_spec[128];
    char limit_spec[128];
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
static int solve_rat_poly_format_antiderivative(const SolveRatPoly *poly, const char *var_name, char *buffer, size_t buffer_size);
static void solve_numeric_analysis_bounds(const SolveOptions *options, double *lo_out, double *hi_out);
static double solve_numeric_derivative_value(const SolveEquation *equation, const SolveOptions *options, double x, int order, int *ok_out);
static int solve_numeric_derivative_roots(const SolveEquation *equation, const SolveOptions *options, int order, double *roots, int *count_out);

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

static double solve_sqrt(double value) {
    double guess;
    int i;

    if (value < 0.0) {
        return 0.0 / 0.0;
    }
    if (value == 0.0) {
        return 0.0;
    }
    guess = value >= 1.0 ? value : 1.0;
    for (i = 0; i < 40; ++i) {
        guess = 0.5 * (guess + value / guess);
    }
    return guess;
}

static double solve_exp(double value) {
    double term = 1.0;
    double sum = 1.0;
    int negative = value < 0.0;
    int halvings = 0;
    int n;

    if (negative) {
        value = -value;
    }
    while (value > 1.0 && halvings < 32) {
        value *= 0.5;
        halvings += 1;
    }
    for (n = 1; n <= 80; ++n) {
        term = term * value / (double)n;
        sum += term;
        if (solve_abs(term) < 1.0e-18) {
            break;
        }
    }
    while (halvings > 0) {
        sum *= sum;
        halvings -= 1;
    }
    return negative ? 1.0 / sum : sum;
}

static double solve_log(double value) {
    double lower;
    double upper;
    double y;
    double y2;
    double term;
    double sum;
    int multiplier = 1;
    int n;

    if (value <= 0.0) {
        return 0.0 / 0.0;
    }
    lower = 0.75;
    upper = 1.5;
    while ((value < lower || value > upper) && multiplier < 1024) {
        value = solve_sqrt(value);
        multiplier *= 2;
    }
    y = (value - 1.0) / (value + 1.0);
    y2 = y * y;
    term = y;
    sum = y;
    for (n = 3; n <= 399; n += 2) {
        term *= y2;
        if (solve_abs(term) < 1.0e-20) {
            break;
        }
        sum += term / (double)n;
    }
    return 2.0 * sum * (double)multiplier;
}

static double solve_atan_series(double value) {
    double term = value;
    double sum = value;
    double square = value * value;
    int n;

    for (n = 3; n <= 399; n += 2) {
        term *= square;
        if ((n / 2) % 2) {
            sum -= term / (double)n;
        } else {
            sum += term / (double)n;
        }
        if (solve_abs(term) < 1.0e-20) {
            break;
        }
    }
    return sum;
}

static double solve_atan(double value) {
    int negative = value < 0.0;
    double result;

    if (negative) {
        value = -value;
    }
    if (value > 1.0) {
        result = (SOLVE_PI / 2.0) - solve_atan(1.0 / value);
    } else if (value > 0.5) {
        double reduced = value / (1.0 + solve_sqrt(1.0 + value * value));
        result = 2.0 * solve_atan_series(reduced);
    } else {
        result = solve_atan_series(value);
    }
    return negative ? -result : result;
}

static double solve_reduce_angle(double value) {
    double two_pi = SOLVE_PI * 2.0;

    while (value > SOLVE_PI) {
        value -= two_pi;
    }
    while (value < -SOLVE_PI) {
        value += two_pi;
    }
    return value;
}

static double solve_sin(double value) {
    double x = solve_reduce_angle(value);
    double x2 = x * x;
    double term = x;
    double sum = x;
    int n;

    for (n = 3; n <= 39; n += 2) {
        term = -term * x2 / (double)((n - 1) * n);
        sum += term;
        if (solve_abs(term) < 1.0e-18) {
            break;
        }
    }
    return sum;
}

static double solve_cos(double value) {
    double x = solve_reduce_angle(value);
    double x2 = x * x;
    double term = 1.0;
    double sum = 1.0;
    int n;

    for (n = 2; n <= 40; n += 2) {
        term = -term * x2 / (double)((n - 1) * n);
        sum += term;
        if (solve_abs(term) < 1.0e-18) {
            break;
        }
    }
    return sum;
}

static double solve_pow_int(double base, long long exponent) {
    double result = 1.0;
    unsigned long long power;
    int negative = exponent < 0;

    if (negative) {
        power = (unsigned long long)(-(exponent + 1)) + 1ULL;
    } else {
        power = (unsigned long long)exponent;
    }
    while (power > 0ULL) {
        if ((power & 1ULL) != 0ULL) {
            result *= base;
        }
        power >>= 1U;
        if (power != 0ULL) {
            base *= base;
        }
    }
    return negative ? 1.0 / result : result;
}

static double solve_pow(double base, double exponent) {
    long long rounded;

    if (exponent >= 0.0) {
        rounded = (long long)(exponent + 0.5);
    } else {
        rounded = (long long)(exponent - 0.5);
    }
    if (solve_abs(exponent - (double)rounded) < 0.000000001 && rounded >= -1024 && rounded <= 1024) {
        return solve_pow_int(base, rounded);
    }
    if (base <= 0.0) {
        return 0.0 / 0.0;
    }
    return solve_exp(solve_log(base) * exponent);
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
           rt_strcmp(name, "atan") == 0 || rt_strcmp(name, "a") == 0 ||
           rt_strcmp(name, "log") == 0 || rt_strcmp(name, "ln") == 0 || rt_strcmp(name, "l") == 0 ||
           rt_strcmp(name, "exp") == 0 || rt_strcmp(name, "e") == 0 ||
           rt_strcmp(name, "min") == 0 || rt_strcmp(name, "max") == 0;
}

static int solve_validate_identifiers(const char *expr, const char *var_name, const char **message_out) {
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
            if (rt_strcmp(name, var_name) == 0 || rt_strcmp(name, "pi") == 0 || rt_strcmp(name, "e") == 0) {
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

static int solve_eval_function(const SolveEquation *equation, const SolveOptions *options, double x, double *value_out, const char **message_out) {
    double left;
    double right = 0.0;

    if (solve_eval_expr(equation->left, options->var_name, x, &left, message_out) != 0) {
        return -1;
    }
    if (equation->has_equation) {
        if (solve_eval_expr(equation->right, options->var_name, x, &right, message_out) != 0) {
            return -1;
        }
    }
    *value_out = left - right;
    return solve_is_bad(*value_out) ? -1 : 0;
}

static int solve_eval_y(const SolveEquation *equation, const SolveOptions *options, double x, double *value_out) {
    const char *message = 0;
    (void)equation;
    return solve_eval_expr(equation->left, options->var_name, x, value_out, &message);
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

    if (!options->have_scan) {
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
    if (solve_rat_format(value, text, sizeof(text)) != 0) rt_write_cstr(1, "?");
    else rt_write_cstr(1, text);
}

static void solve_write_point_rat(SolveRat x, SolveRat y) {
    rt_write_cstr(1, "(");
    solve_write_rat_value(x);
    rt_write_cstr(1, ", ");
    solve_write_rat_value(y);
    rt_write_cstr(1, ")");
}

static void solve_write_double_value(double value, int scale) {
    char text[96];
    solve_format_compact_decimal(value, scale, text, sizeof(text));
    rt_write_cstr(1, text);
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
        rt_write_line(1, "constant zero");
        return;
    }
    if (point_count == 0) {
        SolveRat zero;
        SolveRat value;
        (void)solve_rat_make(0, 1, &zero);
        if (solve_rat_poly_eval(poly, degree, zero, &value) != 0) return;
        rt_write_cstr(1, solve_rat_sign(value) >= 0 ? positive_label : negative_label);
        rt_write_line(1, " = (-inf, inf)");
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
        rt_write_cstr(1, solve_rat_sign(value) > 0 ? positive_label : negative_label);
        rt_write_cstr(1, " = ");
        rt_write_char(1, '(');
        if (segment == 0) rt_write_cstr(1, "-inf"); else rt_write_cstr(1, points[segment - 1].label);
        rt_write_cstr(1, ", ");
        if (segment == point_count) rt_write_cstr(1, "inf"); else rt_write_cstr(1, points[segment].label);
        rt_write_line(1, ")");
    }
}

static void solve_write_poly_end_behavior(const SolveRatPoly *poly) {
    int degree = solve_rat_poly_degree(poly);
    int lead_sign;
    if (degree < 0) {
        rt_write_line(1, "limit x->-inf: 0");
        rt_write_line(1, "limit x->inf: 0");
        return;
    }
    lead_sign = solve_rat_sign(poly->coeff[degree]);
    rt_write_cstr(1, "limit x->-inf: ");
    rt_write_line(1, (degree % 2 == 0 ? lead_sign : -lead_sign) > 0 ? "+inf" : "-inf");
    rt_write_cstr(1, "limit x->inf: ");
    rt_write_line(1, lead_sign > 0 ? "+inf" : "-inf");
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
    if (results->no_real_solutions) {
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

static int solve_run_diff_mode(const SolveEquation *equation, const SolveOptions *options) {
    SolveRatPoly poly;
    SolveRatPoly derivative;
    char text[SOLVE_EXPR_CAPACITY];
    SolveEquation derived;

    if (equation->has_equation && equation->relation != SOLVE_RELATION_EQ) {
        tool_write_error("solve", "derivative solving supports equations, not inequalities", 0);
        return 2;
    }
    if (equation->has_equation) {
        if (solve_equation_rat_poly(equation, options, &poly) != 0) {
            tool_write_error("solve", "derivative supported only for polynomials", 0);
            return 2;
        }
    } else if (solve_parse_rat_text(equation->left, options->var_name, &poly) != 0) {
        tool_write_error("solve", "derivative supported only for polynomials", 0);
        return 2;
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
        rt_write_line(1, text);
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
            tool_write_error("solve", "exact integration overflow", 0);
            return 3;
        }
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
        if (options->quiet) {
            rt_write_line(1, text);
        } else {
            rt_write_cstr(1, "integral = ");
            rt_write_line(1, text);
            rt_write_line(1, "method = exact-polynomial");
        }
        return 0;
    }

    {
        double coarse;
        double fine;
        double error;
        char value[96];
        if (solve_simpson_eval(equation, options, lo, hi, 1000, &coarse) != 0 || solve_simpson_eval(equation, options, lo, hi, 2000, &fine) != 0) {
            if (!options->quiet) rt_write_line(1, "improper integral over a discontinuity or invalid point");
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
        if (options->quiet) {
            solve_format_double(fine, options->scale, value, sizeof(value));
            rt_write_line(1, value);
        } else {
            solve_format_double(fine, options->scale, value, sizeof(value));
            rt_write_cstr(1, "integral = ");
            rt_write_line(1, value);
            solve_format_double(error, options->scale, value, sizeof(value));
            rt_write_cstr(1, "estimated error = ");
            rt_write_line(1, value);
            rt_write_line(1, "method = simpson");
            rt_write_line(1, "status = approximate");
        }
        return 0;
    }
}

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
        rt_write_cstr(1, value < 0.0 ? "-inf" : "inf");
    } else if (label[0] != '\0') {
        rt_write_cstr(1, label);
    } else {
        solve_format_compact_decimal(value, SOLVE_DEFAULT_SCALE, text, sizeof(text));
        rt_write_cstr(1, text);
    }
}

static void solve_print_intervals(const SolveOptions *options, const SolveInterval *intervals, int count, int bounded) {
    int i;
    (void)options;
    if (count == 1 && !intervals[0].has_left && !intervals[0].has_right) {
        rt_write_line(1, "solution = all real x");
        return;
    }
    rt_write_cstr(1, bounded ? "solution (within scan range) = " : "solution = ");
    for (i = 0; i < count; ++i) {
        if (i > 0) rt_write_cstr(1, " U ");
        rt_write_char(1, intervals[i].left_closed ? '[' : '(');
        solve_write_interval_endpoint(intervals[i].left_label, intervals[i].left, intervals[i].has_left);
        rt_write_cstr(1, ", ");
        solve_write_interval_endpoint(intervals[i].right_label, intervals[i].right, intervals[i].has_right);
        rt_write_char(1, intervals[i].right_closed ? ']' : ')');
    }
    rt_write_char(1, '\n');
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
        rt_write_cstr(1, "target sign: f(x) ");
        rt_write_line(1, solve_relation_text(equation->relation));
        rt_write_line(1, "method detail: exact roots split the real line; one exact test value decides each interval");
    }

    if (degree < 0) {
        if (solve_relation_satisfied(0.0, equation->relation, 0.0)) {
            rt_write_line(1, "solution = all real x");
            return 0;
        }
        rt_write_line(1, "solution = empty set");
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
            rt_write_line(1, "solution = all real x");
            return 0;
        }
        rt_write_line(1, "solution = empty set");
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
        rt_write_line(1, "solution = empty set");
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
        rt_write_cstr(1, "target sign: f(x) ");
        rt_write_line(1, solve_relation_text(equation->relation));
        solve_explain_scan_window_line(options);
        rt_write_line(1, "method detail: scan for zero crossings and invalid sample points, then test each interval midpoint");
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
        rt_write_line(1, "solution = empty set");
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

static int solve_run_antiderivative_mode(const SolveEquation *equation, const SolveOptions *options) {
    SolveRatPoly poly;
    SolveRatPoly anti;
    char text[SOLVE_EXPR_CAPACITY];
    if (solve_get_rat_poly_for_mode(equation, options, &poly) != 0) {
        tool_write_error("solve", "derivative supported only for polynomials", 0);
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
            rt_write_cstr(1, "polynomial: ");
            rt_write_line(1, poly_text);
        }
        rt_write_line(1, "rule: integral a*x^n dx = a*x^(n+1)/(n+1)");
        rt_write_cstr(1, "exact antiderivative: ");
        rt_write_line(1, text);
    }
    rt_write_cstr(1, "F(");
    rt_write_cstr(1, options->var_name);
    rt_write_cstr(1, ") = ");
    rt_write_line(1, text);
    rt_write_line(1, "method = exact-polynomial");
    return 0;
}

static int solve_run_monotonicity_mode(const SolveEquation *equation, const SolveOptions *options, int curvature) {
    SolveRatPoly poly;
    SolveRatPoly derivative;
    if (solve_get_rat_poly_for_mode(equation, options, &poly) != 0) {
        double roots[SOLVE_MAX_RESULTS];
        int count = 0;
        double lo = options->default_scan ? -10.0 : options->scan_lo;
        double hi = options->default_scan ? 10.0 : options->scan_hi;
        int i;
        if (solve_should_explain(options)) {
            solve_explain_working_function(curvature ? "numeric curvature" : "numeric monotonicity", equation, options);
            solve_explain_scan_window_line(options);
            rt_write_line(1, curvature ? "method detail: finite-difference f'' sign changes split curvature intervals" : "method detail: finite-difference f' sign changes split monotonicity intervals");
            rt_write_line(1, "status reason: derivative signs are sampled numerically");
        }
        solve_numeric_derivative_roots(equation, options, curvature ? 2 : 1, roots, &count);
        for (i = 0; i <= count; ++i) {
            double left = i == 0 ? lo : roots[i - 1];
            double right = i == count ? hi : roots[i];
            double sample = (left + right) * 0.5;
            int ok;
            double value = solve_numeric_derivative_value(equation, options, sample, curvature ? 2 : 1, &ok);
            if (!ok || right <= left) continue;
            rt_write_cstr(1, value >= 0.0 ? (curvature ? "left-curved" : "increasing") : (curvature ? "right-curved" : "decreasing"));
            rt_write_cstr(1, " (within scan range) = (");
            solve_write_double_value(left, options->scale);
            rt_write_cstr(1, ", ");
            solve_write_double_value(right, options->scale);
            rt_write_line(1, ")");
        }
        rt_write_line(1, "status = approximate");
        return 0;
    }
    if (solve_rat_poly_derivative(&poly, curvature ? 2 : 1, &derivative) != 0) return 3;
    if (solve_should_explain(options)) {
        solve_explain_working_function(curvature ? "curvature" : "monotonicity", equation, options);
        solve_explain_rat_poly_line("polynomial: ", &poly, options);
        solve_explain_rat_poly_line(curvature ? "second derivative: " : "first derivative: ", &derivative, options);
        rt_write_line(1, curvature ? "rule: f'' > 0 means left-curved; f'' < 0 means right-curved" : "rule: f' > 0 means increasing; f' < 0 means decreasing");
        rt_write_line(1, "method detail: exact derivative roots split the real line; exact signs decide intervals");
    }
    solve_print_labeled_intervals(curvature ? "left-curved" : "increasing", curvature ? "right-curved" : "decreasing", options, &derivative);
    rt_write_line(1, "method = exact-polynomial");
    return 0;
}

static int solve_write_exact_line(const char *prefix, SolveRat slope, SolveRat intercept, const SolveOptions *options) {
    rt_write_cstr(1, prefix);
    if (solve_rat_is_zero(slope)) {
        rt_write_cstr(1, "y = ");
        solve_write_rat_value(intercept);
        rt_write_char(1, '\n');
        return 0;
    }
    rt_write_cstr(1, "y = ");
    if (slope.num < 0) rt_write_cstr(1, "-");
    {
        SolveRat abs_slope;
        if (solve_rat_abs_value(slope, &abs_slope) != 0) return -1;
        if (!(abs_slope.num == 1 && abs_slope.den == 1)) {
            solve_write_rat_value(abs_slope);
            rt_write_cstr(1, "*");
        }
    }
    rt_write_cstr(1, options->var_name);
    if (!solve_rat_is_zero(intercept)) {
        SolveRat abs_intercept;
        if (solve_rat_abs_value(intercept, &abs_intercept) != 0) return -1;
        rt_write_cstr(1, intercept.num < 0 ? " - " : " + ");
        solve_write_rat_value(abs_intercept);
    }
    rt_write_char(1, '\n');
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
        int ok;
        const char *message = 0;
        if (solve_parse_double_arg(options->point_spec, &point) != 0 || solve_eval_function(equation, options, point, &y_value, &message) != 0) {
            tool_write_error("solve", "invalid tangent point", options->point_spec);
            return 2;
        }
        slope_value = solve_numeric_derivative_value(equation, options, point, 1, &ok);
        if (!ok) return 3;
        if (normal) {
            if (solve_abs(slope_value) <= options->tolerance) {
                if (solve_should_explain(options)) {
                    solve_explain_working_function("numeric normal", equation, options);
                    solve_explain_double_value_line("point a = ", point, options);
                    solve_explain_double_value_line("f(a) = ", y_value, options);
                    solve_explain_double_value_line("numeric f'(a) = ", slope_value, options);
                    rt_write_line(1, "rule: tangent slope is 0, so the normal line is vertical");
                }
                rt_write_cstr(1, "normal approximate: x = ");
                solve_write_double_value(point, options->scale);
                rt_write_char(1, '\n');
                rt_write_line(1, "status = approximate");
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
            rt_write_line(1, normal ? "rule: normal slope is -1/f'(a)" : "rule: tangent line is y - f(a) = f'(a)*(x - a)");
            rt_write_line(1, "status reason: slope is computed by finite differences");
        }
        rt_write_cstr(1, normal ? "normal approximate: y = " : "tangent approximate: y = ");
        solve_write_double_value(slope_value, options->scale);
        rt_write_cstr(1, "*");
        rt_write_cstr(1, options->var_name);
        if (intercept_value < 0.0) {
            rt_write_cstr(1, " - ");
            solve_write_double_value(-intercept_value, options->scale);
        } else {
            rt_write_cstr(1, " + ");
            solve_write_double_value(intercept_value, options->scale);
        }
        rt_write_char(1, '\n');
        rt_write_line(1, "status = approximate");
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
            if (solve_should_explain(options)) rt_write_line(1, "rule: tangent slope is 0, so the normal line is vertical");
            rt_write_cstr(1, "normal: x = ");
            solve_write_rat_value(x);
            rt_write_char(1, '\n');
            rt_write_line(1, "method = exact-polynomial");
            return 0;
        }
        if (solve_rat_make(-slope.den, slope.num, &slope) != 0) return 3;
        solve_explain_rat_value_line("normal slope = -1/f'(a) = ", slope, options);
    }
    if (solve_rat_mul(slope, x, &product) != 0 || solve_rat_sub(y, product, &intercept) != 0) return 3;
    if (solve_should_explain(options)) {
        rt_write_line(1, normal ? "rule: normal line is y - f(a) = m_normal*(x - a)" : "rule: tangent line is y - f(a) = f'(a)*(x - a)");
        solve_explain_rat_value_line("intercept f(a) - m*a = ", intercept, options);
    }
    if (solve_write_exact_line(normal ? "normal: " : "tangent: ", slope, intercept, options) != 0) return 3;
    rt_write_line(1, "method = exact-polynomial");
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
        rt_write_line(1, "rule: the leading nonzero term controls behavior as x approaches +/-inf");
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
    rt_write_cstr(1, label);
    if (count == 0) {
        rt_write_line(1, " none");
        return;
    }
    rt_write_cstr(1, " ");
    for (i = 0; i < count; ++i) {
        if (i > 0) rt_write_cstr(1, ", ");
        rt_write_cstr(1, points[i].label);
    }
    rt_write_char(1, '\n');
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
    rt_write_char(1, '(');
    solve_write_double_value(x, scale);
    rt_write_cstr(1, ", ");
    solve_write_double_value(y, scale);
    rt_write_char(1, ')');
}

static void solve_write_sample_window(const SolveOptions *options) {
    double lo;
    double hi;
    solve_numeric_analysis_bounds(options, &lo, &hi);
    rt_write_cstr(1, "sample window: [");
    solve_write_double_value(lo, options->scale);
    rt_write_cstr(1, ", ");
    solve_write_double_value(hi, options->scale);
    rt_write_line(1, "]");
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
    rt_write_cstr(1, label);
    for (i = 0; i < count; ++i) {
        if (i > 0) rt_write_cstr(1, ", ");
        else rt_write_char(1, ' ');
        solve_write_double_value(roots[i], options->scale);
    }
    rt_write_char(1, '\n');
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
        rt_write_cstr(1, value >= 0.0 ? positive_label : negative_label);
        rt_write_cstr(1, " (within scan range) = (");
        solve_write_double_value(left, options->scale);
        rt_write_cstr(1, ", ");
        solve_write_double_value(right, options->scale);
        rt_write_line(1, ")");
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
            rt_write_cstr(1, "limit x->-inf approximate: 0");
            rt_write_line(1, ys[2] < 0.0 ? " from below" : " from above");
            rt_write_line(1, "horizontal asymptote approximate: y = 0");
        } else if (solve_abs(ys[2]) > solve_abs(ys[1]) && solve_abs(ys[1]) > solve_abs(ys[0]) && solve_abs(ys[2]) > 1000000.0) {
            rt_write_line(1, ys[2] < 0.0 ? "limit x->-inf approximate: -inf" : "limit x->-inf approximate: +inf");
        }
    }
    if (ok[3] && ok[4] && ok[5]) {
        if (solve_abs(ys[5]) < solve_abs(ys[4]) && solve_abs(ys[4]) < solve_abs(ys[3]) && solve_abs(ys[5]) <= 0.000001) {
            rt_write_cstr(1, "limit x->inf approximate: 0");
            rt_write_line(1, ys[5] < 0.0 ? " from below" : " from above");
            rt_write_line(1, "horizontal asymptote approximate: y = 0");
        } else if (solve_abs(ys[5]) > solve_abs(ys[4]) && solve_abs(ys[4]) > solve_abs(ys[3]) && solve_abs(ys[5]) > 1000000.0) {
            rt_write_line(1, ys[5] < 0.0 ? "limit x->inf approximate: -inf" : "limit x->inf approximate: +inf");
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
            rt_write_line(1, "domain rule: every polynomial is defined for all real x");
            rt_write_line(1, "symmetry rule: only even powers -> y-axis; only odd powers -> origin");
        }
        rt_write_line(1, "domain: all real x");
        rt_write_cstr(1, "symmetry: ");
        rt_write_line(1, solve_symmetry_label(&poly));
        (void)solve_collect_rat_poly_roots(&poly, zeros, &zero_count);
        solve_sort_breakpoints(zeros, &zero_count, options->tolerance);
        solve_print_rat_roots_line("zeros:", zeros, zero_count);
        if (solve_rat_poly_derivative(&poly, 1, &first) != 0 || solve_rat_poly_derivative(&poly, 2, &second) != 0) return 3;
        if (solve_should_explain(options)) {
            solve_explain_rat_poly_line("first derivative: ", &first, options);
            solve_explain_rat_poly_line("second derivative: ", &second, options);
            rt_write_line(1, "extremum rule: f' sign change + to - gives maximum; - to + gives minimum; no sign change gives saddle");
            rt_write_line(1, "inflection rule: roots of f'' where curvature changes sign are inflection points");
        }
        (void)solve_collect_rat_poly_roots(&first, critical, &critical_count);
        solve_sort_breakpoints(critical, &critical_count, options->tolerance);
        for (i = 0; i < critical_count; ++i) {
            SolveRat y;
            const char *label = "saddle";
            if (!critical[i].exact) continue;
            if (solve_rat_poly_eval(&poly, solve_rat_poly_degree(&poly), critical[i].rat_value, &y) != 0 || solve_classify_critical_rat(&first, &second, critical[i].rat_value, &label) != 0) return 3;
            rt_write_cstr(1, label);
            rt_write_cstr(1, ": ");
            solve_write_point_rat(critical[i].rat_value, y);
            rt_write_char(1, '\n');
        }
        (void)solve_collect_rat_poly_roots(&second, inflections, &inflection_count);
        solve_sort_breakpoints(inflections, &inflection_count, options->tolerance);
        for (i = 0; i < inflection_count; ++i) {
            SolveRat y;
            if (!inflections[i].exact) continue;
            if (solve_rat_poly_eval(&poly, solve_rat_poly_degree(&poly), inflections[i].rat_value, &y) != 0) return 3;
            rt_write_cstr(1, "inflection: ");
            solve_write_point_rat(inflections[i].rat_value, y);
            rt_write_char(1, '\n');
        }
        solve_print_labeled_intervals("increasing", "decreasing", options, &first);
        solve_print_labeled_intervals("left-curved", "right-curved", options, &second);
        solve_write_poly_end_behavior(&poly);
        rt_write_line(1, "method = exact-polynomial");
        return 0;
    }
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
            rt_write_line(1, "zero rule: zeros are found by scan plus bisection inside the sampled window");
            rt_write_line(1, "critical-point rule: finite-difference f' sign changes are classified by signs on either side");
            rt_write_line(1, "inflection rule: finite-difference f'' sign changes are reported as approximate inflections");
            rt_write_line(1, "end-behavior rule: far samples provide only approximate asymptote and infinity hints");
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
            if (left < 0.0 && right > 0.0) rt_write_cstr(1, "minimum approximate: ");
            else if (left > 0.0 && right < 0.0) rt_write_cstr(1, "maximum approximate: ");
            else rt_write_cstr(1, "saddle approximate: ");
            solve_write_point_double(x, y, options->scale);
            rt_write_char(1, '\n');
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
            rt_write_cstr(1, "inflection approximate: ");
            solve_write_point_double(x, y, options->scale);
            rt_write_char(1, '\n');
        }
        solve_print_numeric_derivative_intervals(equation, options, 1, "increasing", "decreasing");
        solve_print_numeric_derivative_intervals(equation, options, 2, "left-curved", "right-curved");
        solve_write_numeric_end_behavior(equation, options);
        rt_write_line(1, "status = approximate");
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
        rt_write_line(1, "rule: matching left and right samples indicate a two-sided limit; divergent magnitude indicates a pole; finite mismatch indicates a jump");
        rt_write_line(1, "status reason: this limit path is numeric sampling unless another exact mode handles the expression");
    }
    if (!left_ok || !right_ok || solve_abs(left) > 1.0e12 || solve_abs(right) > 1.0e12 || (left * right < 0.0 && solve_abs(left) > 1000000.0 && solve_abs(right) > 1000000.0)) {
        rt_write_line(1, "limit: no two-sided limit (pole)");
        return 1;
    }
    if (solve_abs(left - right) <= 0.000001 * (solve_abs(left) + solve_abs(right) + 1.0)) {
        rt_write_cstr(1, "limit = ");
        solve_write_double_value((left + right) * 0.5, options->scale);
        rt_write_char(1, '\n');
        return 0;
    }
    rt_write_line(1, "limit: no two-sided limit");
    rt_write_cstr(1, "left = "); solve_write_double_value(left, options->scale); rt_write_char(1, '\n');
    rt_write_cstr(1, "right = "); solve_write_double_value(right, options->scale); rt_write_char(1, '\n');
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

static int solve_split_rational_expr(const char *expr, char *num, size_t num_size, char *den, size_t den_size) {
    size_t index;
    int depth = 0;
    for (index = 0U; expr[index] != '\0'; ++index) {
        if (expr[index] == '(') depth += 1;
        else if (expr[index] == ')' && depth > 0) depth -= 1;
        else if (expr[index] == '/' && depth == 0) {
            char left[SOLVE_EXPR_CAPACITY];
            char right[SOLVE_EXPR_CAPACITY];
            if (solve_copy_range(left, sizeof(left), expr, 0U, index) != 0 || solve_copy_range(right, sizeof(right), expr, index + 1U, rt_strlen(expr)) != 0) return -1;
            return solve_copy_unwrapped(num, num_size, left) == 0 && solve_copy_unwrapped(den, den_size, right) == 0 ? 0 : -1;
        }
    }
    return -1;
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

    if (solve_contains_char(options->range_spec, ':')) {
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
    if (solve_should_explain(options)) {
        rt_write_line(1, "explain: area between curves");
        rt_write_cstr(1, "upper/lower difference h(x) = (");
        rt_write_cstr(1, first_expr);
        rt_write_cstr(1, ") - (");
        rt_write_cstr(1, second_expr);
        rt_write_line(1, ")");
        if (lo_text[0] != '\0') {
            rt_write_cstr(1, "bounds: ");
            rt_write_cstr(1, lo_text);
            rt_write_cstr(1, " to ");
            rt_write_line(1, hi_text);
        } else {
            rt_write_line(1, "bounds: omitted; using leftmost and rightmost exact intersections when available");
        }
        rt_write_line(1, "rule: area is integral |h(x)| dx, so sign changes split the interval into absolute pieces");
    }
    if (solve_parse_rat_text(first_expr, options->var_name, &first) == 0 && solve_parse_rat_text(second_expr, options->var_name, &second) == 0 && solve_rat_poly_add(&first, &second, 1, &diff) == 0 &&
        ((lo_text[0] != '\0' && solve_rat_poly_parse_bound(lo_text, options->var_name, &lo) == 0 && solve_rat_poly_parse_bound(hi_text, options->var_name, &hi) == 0) || lo_text[0] == '\0')) {
        if (lo_text[0] == '\0') {
            SolveBreakpoint roots[SOLVE_MAX_RESULTS];
            int root_count = 0;
            if (solve_collect_rat_poly_roots(&diff, roots, &root_count) != 0 || root_count < 2) {
                tool_write_error("solve", "area bounds omitted but fewer than two intersections were found", 0);
                return 2;
            }
            solve_sort_breakpoints(roots, &root_count, options->tolerance);
            if (solve_should_explain(options)) solve_print_rat_roots_line("intersections:", roots, root_count);
            lo = roots[0].rat_value;
            hi = roots[root_count - 1].rat_value;
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
            rt_write_line(1, "method detail: exact polynomial roots inside the bounds split positive and negative lobes");
        }
        if (solve_exact_area_poly(&diff, lo, hi, &area) != 0) return 3;
        rt_write_cstr(1, "area = ");
        solve_write_rat_value(area);
        rt_write_char(1, '\n');
        rt_write_line(1, "method = exact-polynomial");
        return 0;
    }
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
            rt_write_line(1, "method detail: composite Simpson rule samples |f(x)-g(x)| directly with 2000 subintervals");
            rt_write_line(1, "status reason: non-polynomial area is numeric and approximate");
        }
        solve_format_double(sum * h / 3.0, options->scale, value, sizeof(value));
        rt_write_cstr(1, "area = "); rt_write_line(1, value);
        rt_write_line(1, "method = simpson");
        rt_write_line(1, "status = approximate");
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
    if (solve_should_explain(options)) {
        rt_write_line(1, volume ? "explain: volume of rotation" : "explain: mean value");
        rt_write_cstr(1, "working function: f(x) = ");
        rt_write_line(1, expr);
        rt_write_cstr(1, "bounds: ");
        rt_write_cstr(1, lo_text);
        rt_write_cstr(1, " to ");
        rt_write_line(1, hi_text);
        rt_write_line(1, volume ? "formula: volume = pi * integral f(x)^2 dx" : "formula: mean = (1/(b-a)) * integral f(x) dx");
    }
    if (solve_parse_rat_text(expr, options->var_name, &poly) == 0 && solve_rat_poly_parse_bound(lo_text, options->var_name, &lo) == 0 && solve_rat_poly_parse_bound(hi_text, options->var_name, &hi) == 0) {
        integrand = poly;
        if (volume && solve_rat_poly_square(&poly, &integrand) != 0) return 3;
        if (solve_rat_poly_definite_integral(&integrand, lo, hi, &value) != 0) return 3;
        if (solve_should_explain(options)) {
            solve_explain_rat_poly_line("exact integrand polynomial: ", &integrand, options);
            solve_explain_rat_value_line("exact integral = ", value, options);
            if (!volume) rt_write_line(1, "next: divide by interval width b-a");
        }
        if (!volume) {
            SolveRat width;
            if (solve_rat_sub(hi, lo, &width) != 0 || solve_rat_div(value, width, &value) != 0) return 3;
            solve_explain_rat_value_line("interval width = ", width, options);
            rt_write_cstr(1, "mean = "); solve_write_rat_value(value); rt_write_char(1, '\n');
        } else {
            rt_write_cstr(1, "volume = pi*("); solve_write_rat_value(value); rt_write_line(1, ")");
        }
        rt_write_line(1, "method = exact-polynomial");
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
                rt_write_line(1, "method detail: composite Simpson rule integrates f(x)^2 with 1000 and 2000 subintervals");
                solve_explain_double_value_line("coarse integral = ", coarse, options);
                solve_explain_double_value_line("fine integral = ", fine, options);
                rt_write_line(1, "status reason: non-polynomial rotation volume is numeric and approximate");
            }
            solve_format_double(fine, options->scale, text, sizeof(text));
            rt_write_cstr(1, "volume approximate = pi*(");
            rt_write_cstr(1, text);
            rt_write_line(1, ")");
        } else {
            if (solve_simpson_eval(&equation, options, lo_d, hi_d, 1000, &coarse) != 0 || solve_simpson_eval(&equation, options, lo_d, hi_d, 2000, &fine) != 0 || hi_d == lo_d) return 3;
            result = fine / (hi_d - lo_d);
            if (solve_should_explain(options)) {
                rt_write_line(1, "method detail: composite Simpson rule integrates f(x), then divides by b-a");
                solve_explain_double_value_line("fine integral = ", fine, options);
                solve_explain_double_value_line("interval width = ", hi_d - lo_d, options);
                rt_write_line(1, "status reason: non-polynomial mean value is numeric and approximate");
            }
            solve_format_double(result, options->scale, text, sizeof(text));
            rt_write_cstr(1, "mean approximate = ");
            rt_write_line(1, text);
        }
        rt_write_line(1, "method = simpson");
        rt_write_line(1, "status = approximate");
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
        rt_write_line(1, "explain: rational asymptotes");
        solve_explain_rat_poly_line("numerator: ", &num, options);
        solve_explain_rat_poly_line("denominator: ", &den, options);
        rt_write_line(1, "vertical rule: denominator root with nonzero numerator gives a vertical asymptote");
        rt_write_line(1, "end-behavior rule: polynomial division gives horizontal or oblique asymptote when the remainder tends to 0");
    }
    if (solve_collect_rat_poly_roots(&den, roots, &root_count) != 0) return 3;
    solve_sort_breakpoints(roots, &root_count, options->tolerance);
    if (solve_should_explain(options)) solve_print_rat_roots_line("denominator roots:", roots, root_count);
    for (i = 0; i < root_count; ++i) {
        SolveRat value;
        if (!roots[i].exact) continue;
        if (solve_should_explain(options) && solve_rat_poly_eval(&num, solve_rat_poly_degree(&num), roots[i].rat_value, &value) == 0) {
            rt_write_cstr(1, "numerator at ");
            rt_write_cstr(1, roots[i].label);
            rt_write_cstr(1, " = ");
            solve_write_rat_value(value);
            rt_write_char(1, '\n');
        }
        if (solve_rat_poly_eval(&num, solve_rat_poly_degree(&num), roots[i].rat_value, &value) == 0 && !solve_rat_is_zero(value)) {
            rt_write_cstr(1, "vertical: x = "); rt_write_line(1, roots[i].label);
        }
    }
    if (solve_rat_poly_divide(&num, &den, &quotient, &remainder) != 0) return 3;
    if (solve_should_explain(options)) {
        solve_explain_rat_poly_line("polynomial quotient: ", &quotient, options);
        solve_explain_rat_poly_line("remainder: ", &remainder, options);
        rt_write_line(1, "division form: numerator/denominator = quotient + remainder/denominator");
    }
    if (solve_rat_poly_degree(&quotient) <= 1) {
        if (solve_rat_poly_format(&quotient, options->var_name, text, sizeof(text)) != 0) return 3;
        rt_write_cstr(1, solve_rat_poly_degree(&quotient) == 1 ? "oblique: y = " : "horizontal: y = ");
        rt_write_line(1, text);
    } else {
        if (solve_rat_poly_format(&quotient, options->var_name, text, sizeof(text)) != 0) return 3;
        rt_write_cstr(1, "polynomial quotient: y = "); rt_write_line(1, text);
    }
    rt_write_line(1, "method = exact-polynomial");
    return 0;
}

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
    options->have_limit = 0;
    options->have_asymptotes = 0;
    options->point_spec[0] = '\0';
    options->range_spec[0] = '\0';
    options->limit_spec[0] = '\0';
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
        } else if (rt_strcmp(opt.flag, "--volume") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.range_spec)) return 2;
            options.have_volume = 1;
            rt_copy_string(options.range_spec, sizeof(options.range_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--mean") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.range_spec)) return 2;
            options.have_mean = 1;
            rt_copy_string(options.range_spec, sizeof(options.range_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--limit") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_strlen(opt.value) >= sizeof(options.limit_spec)) return 2;
            options.have_limit = 1;
            rt_copy_string(options.limit_spec, sizeof(options.limit_spec), opt.value);
        } else if (rt_strcmp(opt.flag, "--asymptotes") == 0) {
            options.have_asymptotes = 1;
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
    if (options.have_area) {
        return solve_run_area_mode(&options, opt.argi, argc, argv);
    }
    if (solve_join_expression(opt.argi, argc, argv, expression, sizeof(expression)) != 0) {
        tool_write_error("solve", "missing or too large expression", 0);
        return 2;
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
        if (solve_validate_identifiers(equation.left, options.var_name, &validation_message) != 0 ||
            solve_validate_identifiers(equation.right, options.var_name, &validation_message) != 0) {
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
