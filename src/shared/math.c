#include "math.h"

double math_abs(double value) {
    return value < 0.0 ? -value : value;
}

double math_sqrt(double value) {
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

double math_exp(double value) {
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
        if (math_abs(term) < 1.0e-18) {
            break;
        }
    }
    while (halvings > 0) {
        sum *= sum;
        halvings -= 1;
    }
    return negative ? 1.0 / sum : sum;
}

double math_log(double value) {
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
        value = math_sqrt(value);
        multiplier *= 2;
    }
    y = (value - 1.0) / (value + 1.0);
    y2 = y * y;
    term = y;
    sum = y;
    for (n = 3; n <= 399; n += 2) {
        term *= y2;
        if (math_abs(term) < 1.0e-20) {
            break;
        }
        sum += term / (double)n;
    }
    return 2.0 * sum * (double)multiplier;
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

static double math_reduce_angle(double value) {
    double two_pi = MATH_PI * 2.0;

    while (value > MATH_PI) {
        value -= two_pi;
    }
    while (value < -MATH_PI) {
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
        return 0.0 / 0.0;
    }
    return math_sin(value) / cosine;
}

double math_asin(double value) {
    if (value < -1.0 || value > 1.0) {
        return 0.0 / 0.0;
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
        return 0.0 / 0.0;
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

double math_floor(double value) {
    double truncated = (double)(long long)value;

    if (truncated > value) {
        truncated -= 1.0;
    }
    return truncated;
}

double math_ceil(double value) {
    double truncated = (double)(long long)value;

    if (truncated < value) {
        truncated += 1.0;
    }
    return truncated;
}

double math_round(double value) {
    return value >= 0.0 ? math_floor(value + 0.5) : math_ceil(value - 0.5);
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
        return 0.0 / 0.0;
    }
    return math_exp(math_log(base) * exponent);
}
