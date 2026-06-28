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
#define SOLVE_RAT_POLY_MAX_DEGREE SOLVE_POLY_MAX_DEGREE
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
} SolveEquation;

typedef struct {
    char var_name[SOLVE_NAME_CAPACITY];
    int have_scan;
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
    int numeric_failure;
    int suspected_discontinuity;
} SolveResultSet;

static int solve_add_result(SolveResultSet *set, const SolveResult *result, int all, double tolerance);
static int solve_eval_function(const SolveEquation *equation, const SolveOptions *options, double x, double *value_out, const char **message_out);
static int solve_eval_y(const SolveEquation *equation, const SolveOptions *options, double x, double *value_out);
static int solve_root_in_scan_range(const SolveOptions *options, double root);

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

static void solve_explain_linear(const SolveEquation *equation, const SolveOptions *options, double slope, double intercept, double root) {
    char value[96];
    int unit_slope = solve_abs(slope - 1.0) <= options->tolerance * 2.0;
    int negative_unit_slope = solve_abs(slope + 1.0) <= options->tolerance * 2.0;

    if (!options->explain || tool_json_is_enabled() || options->quiet) {
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
    if (!options->explain || tool_json_is_enabled() || options->quiet) {
        return;
    }
    rt_write_line(1, exact ? "polynomial identity detected" : "approximate polynomial identity detected");
    rt_write_line(1, exact ? "all exact coefficients reduce to 0, so the equation is true for every real x" : "floating-point coefficients reduce to 0 within tolerance, so the equation is numerically true across the supported polynomial form");
}

static void solve_explain_quadratic(const SolveOptions *options, double a, double b, double c, double discriminant, double root1, double root2, const char *method) {
    char value[96];

    if (!options->explain || tool_json_is_enabled() || options->quiet) {
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

    if (!options->explain || tool_json_is_enabled() || options->quiet) {
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
    if (discriminant.num < 0) return set->count > 0U ? 0 : -1;
    if (explain) {
        double da = solve_rat_to_double(a);
        double db = solve_rat_to_double(b);
        double dc = solve_rat_to_double(c);
        double dd = solve_rat_to_double(discriminant);
        double sr = solve_sqrt(dd);
        double r1 = (-db - sr) / (2.0 * da);
        double r2 = (-db + sr) / (2.0 * da);
        solve_explain_quadratic(options, da, db, dc, dd, r1 < r2 ? r1 : r2, r1 < r2 ? r2 : r1, solve_rat_sqrt_exact(discriminant, &sqrt_discriminant) == 0 ? "factoring" : "quadratic-formula");
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
    while (degree > 2) {
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
        solve_explain_higher_polynomial(options, original_degree, root_values, rational_root_count, degree);
        for (root_index = 0; root_index < rational_root_count; ++root_index) {
            int rc = solve_add_direct_rat_root(equation, options, set, rational_roots[root_index], "polynomial-factoring");
            if (rc > 0) return 1;
        }
        poly = reduced;
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
    size_t equals_pos = 0U;

    for (index = 0U; text[index] != '\0'; ++index) {
        char ch = text[index];
        if (ch == '(') {
            depth += 1;
        } else if (ch == ')' && depth > 0) {
            depth -= 1;
        } else if (ch == '=' && depth == 0) {
            char prev = index == 0U ? '\0' : text[index - 1U];
            char next = text[index + 1U];
            if (prev == '<' || prev == '>' || prev == '!' || prev == '=' || next == '=') {
                continue;
            }
            if (found) {
                return -1;
            }
            found = 1;
            equals_pos = index;
        }
    }

    if (found) {
        if (solve_copy_range(equation->left, sizeof(equation->left), text, 0U, equals_pos) != 0 ||
            solve_copy_range(equation->right, sizeof(equation->right), text, equals_pos + 1U, rt_strlen(text)) != 0) {
            return -1;
        }
        equation->has_equation = 1;
    } else {
        if (solve_copy_range(equation->left, sizeof(equation->left), text, 0U, rt_strlen(text)) != 0) {
            return -1;
        }
        rt_copy_string(equation->right, sizeof(equation->right), "0");
        equation->has_equation = 0;
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
    if (!options->explain || tool_json_is_enabled() || options->quiet) {
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

    if (!options->explain || tool_json_is_enabled() || options->quiet) {
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

    rt_memset(result, 0, sizeof(*result));
    if (solve_eval_function(equation, options, lo, &flo, &message) != 0 || solve_eval_function(equation, options, hi, &fhi, &message) != 0) {
        return -1;
    }
    if (solve_abs(flo) <= options->tolerance) {
        mid = lo;
        fmid = flo;
        iteration = 0;
    } else if (solve_abs(fhi) <= options->tolerance) {
        mid = hi;
        fmid = fhi;
        iteration = 0;
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
    result->method = "bisection";
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
            result.method = "scan";
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

static int solve_write_json_result(const SolveOptions *options, const SolveResult *result) {
    char value[96];
    const char *root_text;

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
    solve_format_double(result->residual, options->scale, value, sizeof(value));
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

    if (tool_json_is_enabled()) {
        (void)solve_write_json_result(options, result);
        return;
    }
    if (result->exact_value[0] != '\0') {
        rt_copy_string(value, sizeof(value), result->exact_value);
    } else {
        solve_format_result_answer(equation, options, result, value, sizeof(value));
    }
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
    solve_format_double(result->residual, options->scale, value, sizeof(value));
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

static void solve_options_init(SolveOptions *options) {
    rt_copy_string(options->var_name, sizeof(options->var_name), "x");
    options->have_scan = 0;
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
    SolveResultSet results;
    ToolOptState opt;
    char expression[SOLVE_EXPR_CAPACITY];
    int opt_result;
    size_t index;

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
    }
    if (solve_join_expression(opt.argi, argc, argv, expression, sizeof(expression)) != 0) {
        tool_write_error("solve", "missing or too large expression", 0);
        return 2;
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

    solve_explain_start(&equation, &options);
    rt_memset(&results, 0, sizeof(results));
    if (!options.have_bracket && solve_try_rational_polynomial(&equation, &options, &results)) {
        /* solved exactly by the rational polynomial front-end */
    } else if (!options.have_bracket && solve_try_polynomial(&equation, &options, &results)) {
        /* solved directly */
    } else if (!options.have_bracket && solve_try_linear(&equation, &options, &results)) {
        /* solved directly */
    } else if (options.have_bracket) {
        solve_explicit_bracket(&equation, &options, &results);
    } else {
        solve_scan(&equation, &options, &results);
    }

    if (results.identity) {
        solve_print_identity(&options, results.identity == 1);
        solve_write_summary_json(1U, 0);
        return 0;
    }

    for (index = 0U; index < results.count; ++index) {
        if (index > 0U && !tool_json_is_enabled() && !options.quiet) {
            rt_write_char(1, '\n');
        }
        solve_print_result(&equation, &options, &results.results[index]);
    }
    if (results.count == 0U) {
        if (tool_json_is_enabled()) {
            solve_write_summary_json(0U, 1);
        } else if (!options.quiet) {
            rt_write_line(1, "no solution found in requested range");
        }
        return results.suspected_discontinuity ? 3 : 1;
    }
    solve_write_summary_json(results.count, 0);
    return 0;
}
