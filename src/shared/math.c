#include "math.h"

static unsigned long long math_double_bits(double value) {
    unsigned long long bits = 0ULL;
    unsigned char *destination = (unsigned char *)&bits;
    const unsigned char *source = (const unsigned char *)&value;
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        destination[index] = source[index];
    }
    return bits;
}

static double math_bits_double(unsigned long long bits) {
    double value = 0.0;
    unsigned char *destination = (unsigned char *)&value;
    const unsigned char *source = (const unsigned char *)&bits;
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        destination[index] = source[index];
    }
    return value;
}

int math_is_nan(double value) {
    unsigned long long bits = math_double_bits(value);

    return (bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL &&
           (bits & 0x000fffffffffffffULL) != 0ULL;
}

int math_is_infinite(double value) {
    return (math_double_bits(value) & 0x7fffffffffffffffULL) == 0x7ff0000000000000ULL;
}

int math_is_finite(double value) {
    return (math_double_bits(value) & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
}

int math_sign_bit(double value) {
    return (math_double_bits(value) & 0x8000000000000000ULL) != 0ULL;
}

double math_copy_sign(double magnitude, double sign) {
    unsigned long long magnitude_bits = math_double_bits(magnitude) & 0x7fffffffffffffffULL;
    unsigned long long sign_bits = math_double_bits(sign) & 0x8000000000000000ULL;

    return math_bits_double(magnitude_bits | sign_bits);
}

double math_nan(void) {
    return math_bits_double(0x7ff8000000000000ULL);
}

double math_infinity(void) {
    return math_bits_double(0x7ff0000000000000ULL);
}

double math_abs(double value) {
    return math_bits_double(math_double_bits(value) & 0x7fffffffffffffffULL);
}

double math_sqrt(double value) {
    double fraction;
    double guess;
    int exponent;
    int i;

    if (math_is_nan(value) || value < 0.0) {
        return math_nan();
    }
    if (math_is_infinite(value)) {
        return value;
    }
    if (value == 0.0) {
        return value;
    }
    fraction = math_frexp(value, &exponent);
    if (exponent % 2 != 0) {
        fraction *= 2.0;
        exponent -= 1;
    }
    guess = 0.5 * (fraction + 1.0);
    for (i = 0; i < 6; ++i) {
        guess = 0.5 * (guess + fraction / guess);
    }
    return math_scalbn(guess, exponent / 2);
}

double math_exp(double value) {
    int exponent;
    double reduced;
    double term = 1.0;
    double sum = 1.0;
    int negative = value < 0.0;
    int n;

    if (math_is_nan(value)) {
        return math_nan();
    }
    if (math_is_infinite(value)) {
        return negative ? 0.0 : value;
    }
    if (value > 709.782712893384) {
        return math_infinity();
    }
    if (value < -745.133219101941) {
        return 0.0;
    }
    exponent = (int)(value / MATH_LN2 + (negative ? -0.5 : 0.5));
    reduced = value - (double)exponent * MATH_LN2;
    for (n = 1; n <= 24; ++n) {
        term = term * reduced / (double)n;
        sum += term;
        if (math_abs(term) < 1.0e-18) {
            break;
        }
    }
    return math_scalbn(sum, exponent);
}

double math_exp2(double value) {
    return math_exp(value * MATH_LN2);
}

double math_log(double value) {
    double fraction;
    double y;
    double y2;
    double term;
    double sum;
    int exponent;
    int n;

    if (math_is_nan(value) || value <= 0.0) {
        return math_nan();
    }
    if (math_is_infinite(value)) {
        return value;
    }
    fraction = math_frexp(value, &exponent);
    if (fraction < 0.70710678118654752440) {
        fraction *= 2.0;
        exponent -= 1;
    }
    y = (fraction - 1.0) / (fraction + 1.0);
    y2 = y * y;
    term = y;
    sum = y;
    for (n = 3; n <= 49; n += 2) {
        term *= y2;
        if (math_abs(term) < 1.0e-20) {
            break;
        }
        sum += term / (double)n;
    }
    return 2.0 * sum + (double)exponent * MATH_LN2;
}

double math_log2(double value) {
    return math_log(value) / MATH_LN2;
}

double math_log10(double value) {
    return math_log(value) / MATH_LN10;
}

static double math_atan_series(double value) {
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
        if (math_abs(term) < 1.0e-20) {
            break;
        }
    }
    return sum;
}

double math_atan(double value) {
    int negative = value < 0.0;
    double result;

    if (negative) {
        value = -value;
    }
    if (value > 1.0) {
        result = (MATH_PI / 2.0) - math_atan(1.0 / value);
    } else if (value > 0.5) {
        double reduced = value / (1.0 + math_sqrt(1.0 + value * value));
        result = 2.0 * math_atan_series(reduced);
    } else {
        result = math_atan_series(value);
    }
    return negative ? -result : result;
}

double math_atan2(double y, double x) {
    double result;

    if (math_is_nan(x) || math_is_nan(y)) {
        return math_nan();
    }
    if (y == 0.0) {
        if (math_sign_bit(x)) {
            return math_copy_sign(MATH_PI, y);
        }
        return y;
    }
    if (x == 0.0) {
        return math_copy_sign(MATH_PI / 2.0, y);
    }
    if (math_is_infinite(y)) {
        if (math_is_infinite(x)) {
            result = math_sign_bit(x) ? 3.0 * MATH_PI / 4.0 : MATH_PI / 4.0;
            return math_copy_sign(result, y);
        }
        return math_copy_sign(MATH_PI / 2.0, y);
    }
    if (math_is_infinite(x)) {
        return math_sign_bit(x) ? math_copy_sign(MATH_PI, y) : math_copy_sign(0.0, y);
    }
    result = math_atan(math_abs(y / x));
    if (x < 0.0) {
        result = MATH_PI - result;
    }
    return math_copy_sign(result, y);
}

double math_hypot(double x, double y) {
    double larger;
    double smaller;
    double ratio;

    x = math_abs(x);
    y = math_abs(y);
    if (math_is_infinite(x) || math_is_infinite(y)) {
        return math_infinity();
    }
    if (math_is_nan(x) || math_is_nan(y)) {
        return math_nan();
    }
    larger = math_max(x, y);
    smaller = math_min(x, y);
    if (larger == 0.0) {
        return 0.0;
    }
    ratio = smaller / larger;
    return larger * math_sqrt(1.0 + ratio * ratio);
}

static double math_reduce_angle(double value) {
    double two_pi = MATH_PI * 2.0;

    if (!math_is_finite(value)) {
        return math_nan();
    }
    value = math_fmod(value, two_pi);
    if (value > MATH_PI) {
        value -= two_pi;
    } else if (value < -MATH_PI) {
        value += two_pi;
    }
    return value;
}

double math_sin(double value) {
    double x = math_reduce_angle(value);
    double x2 = x * x;
    double term = x;
    double sum = x;
    int n;

    for (n = 3; n <= 39; n += 2) {
        term = -term * x2 / (double)((n - 1) * n);
        sum += term;
        if (math_abs(term) < 1.0e-18) {
            break;
        }
    }
    return sum;
}

double math_cos(double value) {
    double x = math_reduce_angle(value);
    double x2 = x * x;
    double term = 1.0;
    double sum = 1.0;
    int n;

    for (n = 2; n <= 40; n += 2) {
        term = -term * x2 / (double)((n - 1) * n);
        sum += term;
        if (math_abs(term) < 1.0e-18) {
            break;
        }
    }
    return sum;
}

double math_tan(double value) {
    double cosine = math_cos(value);

    if (cosine == 0.0) {
        return math_nan();
    }
    return math_sin(value) / cosine;
}

double math_asin(double value) {
    if (value < -1.0 || value > 1.0) {
        return math_nan();
    }
    if (value == 1.0) {
        return MATH_PI / 2.0;
    }
    if (value == -1.0) {
        return -MATH_PI / 2.0;
    }
    return math_atan(value / math_sqrt(1.0 - value * value));
}

double math_acos(double value) {
    if (value < -1.0 || value > 1.0) {
        return math_nan();
    }
    return MATH_PI / 2.0 - math_asin(value);
}

double math_sinh(double value) {
    double ex = math_exp(value);

    return 0.5 * (ex - 1.0 / ex);
}

double math_cosh(double value) {
    double ex = math_exp(value);

    return 0.5 * (ex + 1.0 / ex);
}

double math_tanh(double value) {
    double e2;

    if (value > 350.0) {
        return 1.0;
    }
    if (value < -350.0) {
        return -1.0;
    }
    e2 = math_exp(2.0 * value);
    return (e2 - 1.0) / (e2 + 1.0);
}

double math_trunc(double value) {
    unsigned long long bits = math_double_bits(value);
    int exponent = (int)((bits >> 52U) & 0x7ffULL) - 1023;
    unsigned long long fraction_mask;

    if (exponent < 0) {
        return math_copy_sign(0.0, value);
    }
    if (exponent >= 52) {
        return value;
    }
    fraction_mask = (1ULL << (unsigned int)(52 - exponent)) - 1ULL;
    return math_bits_double(bits & ~fraction_mask);
}

double math_modf(double value, double *integer_part) {
    double integer;
    double fraction;

    if (math_is_nan(value)) {
        *integer_part = value;
        return value;
    }
    if (math_is_infinite(value)) {
        *integer_part = value;
        return math_copy_sign(0.0, value);
    }
    integer = math_trunc(value);
    fraction = value - integer;
    *integer_part = integer;
    return fraction == 0.0 ? math_copy_sign(0.0, value) : fraction;
}

double math_fmod(double value, double divisor) {
    double remainder;
    double scaled_divisor;

    if (math_is_nan(value) || math_is_nan(divisor) || divisor == 0.0 || math_is_infinite(value)) {
        return math_nan();
    }
    if (math_is_infinite(divisor)) {
        return value;
    }
    remainder = math_abs(value);
    divisor = math_abs(divisor);
    if (remainder < divisor) {
        return value;
    }
    scaled_divisor = divisor;
    while (scaled_divisor <= remainder * 0.5 && math_is_finite(scaled_divisor * 2.0)) {
        scaled_divisor *= 2.0;
    }
    while (scaled_divisor >= divisor) {
        if (remainder >= scaled_divisor) {
            remainder -= scaled_divisor;
        }
        scaled_divisor *= 0.5;
    }
    return math_copy_sign(remainder, value);
}

double math_frexp(double value, int *exponent_out) {
    unsigned long long bits;
    unsigned int exponent_bits;

    *exponent_out = 0;
    if (value == 0.0 || !math_is_finite(value)) {
        return value;
    }
    bits = math_double_bits(value);
    exponent_bits = (unsigned int)((bits >> 52U) & 0x7ffULL);
    if (exponent_bits == 0U) {
        value *= 18014398509481984.0;
        bits = math_double_bits(value);
        exponent_bits = (unsigned int)((bits >> 52U) & 0x7ffULL);
        *exponent_out = (int)exponent_bits - 1022 - 54;
    } else {
        *exponent_out = (int)exponent_bits - 1022;
    }
    bits = (bits & 0x800fffffffffffffULL) | 0x3fe0000000000000ULL;
    return math_bits_double(bits);
}

static double math_power_of_two(int exponent) {
    if (exponent > 1023) {
        return math_infinity();
    }
    if (exponent >= -1022) {
        return math_bits_double((unsigned long long)(exponent + 1023) << 52U);
    }
    if (exponent >= -1074) {
        return math_bits_double(1ULL << (unsigned int)(exponent + 1074));
    }
    return 0.0;
}

double math_scalbn(double value, int exponent) {
    double result = value;

    if (value == 0.0 || !math_is_finite(value) || exponent == 0) {
        return value;
    }
    if (exponent > 4096) {
        return math_copy_sign(math_infinity(), value);
    }
    if (exponent < -4096) {
        return math_copy_sign(0.0, value);
    }
    while (exponent > 1023) {
        result *= math_power_of_two(1023);
        exponent -= 1023;
    }
    while (exponent < -1022) {
        result *= math_power_of_two(-1022);
        exponent += 1022;
    }
    return result * math_power_of_two(exponent);
}

double math_next_after(double value, double toward) {
    unsigned long long bits;

    if (math_is_nan(value) || math_is_nan(toward)) {
        return math_nan();
    }
    if (value == toward) {
        return toward;
    }
    if (value == 0.0) {
        return math_bits_double((math_sign_bit(toward) ? 0x8000000000000000ULL : 0ULL) | 1ULL);
    }
    bits = math_double_bits(value);
    if ((value > 0.0) == (toward > value)) {
        bits += 1ULL;
    } else {
        bits -= 1ULL;
    }
    return math_bits_double(bits);
}

double math_min(double left, double right) {
    if (math_is_nan(left)) {
        return right;
    }
    if (math_is_nan(right)) {
        return left;
    }
    if (left == right) {
        return math_sign_bit(left) ? left : right;
    }
    return left < right ? left : right;
}

double math_max(double left, double right) {
    if (math_is_nan(left)) {
        return right;
    }
    if (math_is_nan(right)) {
        return left;
    }
    if (left == right) {
        return math_sign_bit(left) ? right : left;
    }
    return left > right ? left : right;
}

double math_clamp(double value, double lower, double upper) {
    if (math_is_nan(value) || math_is_nan(lower) || math_is_nan(upper) || lower > upper) {
        return math_nan();
    }
    return math_min(math_max(value, lower), upper);
}

double math_floor(double value) {
    double truncated;

    if (!math_is_finite(value) || value == 0.0) {
        return value;
    }
    truncated = math_trunc(value);
    if (truncated > value) {
        truncated -= 1.0;
    }
    return truncated;
}

double math_ceil(double value) {
    double truncated;

    if (!math_is_finite(value) || value == 0.0) {
        return value;
    }
    truncated = math_trunc(value);
    if (truncated < value) {
        truncated += 1.0;
    }
    return truncated;
}

double math_round(double value) {
    double truncated;
    double fraction;

    if (!math_is_finite(value) || value == 0.0) {
        return value;
    }
    truncated = math_trunc(value);
    fraction = value - truncated;
    if (fraction >= 0.5) {
        return truncated + 1.0;
    }
    if (fraction <= -0.5) {
        return truncated - 1.0;
    }
    return truncated == 0.0 ? math_copy_sign(0.0, value) : truncated;
}

double math_pow_int(double base, long long exponent) {
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

double math_pow(double base, double exponent) {
    long long rounded;

    if (exponent >= 0.0) {
        rounded = (long long)(exponent + 0.5);
    } else {
        rounded = (long long)(exponent - 0.5);
    }
    if (math_abs(exponent - (double)rounded) < 0.000000001 &&
        rounded >= -1024 && rounded <= 1024) {
        return math_pow_int(base, rounded);
    }
    if (base <= 0.0) {
        return math_nan();
    }
    return math_exp(math_log(base) * exponent);
}
