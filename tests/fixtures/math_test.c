#include "../../src/shared/math.h"

static int close_to(double actual, double expected, double tolerance) {
    return math_abs(actual - expected) <= tolerance;
}

static int is_nan(double value) {
    return value != value;
}

int main(void) {
    double sinh_one;
    double cosh_one;

    if (math_abs(-2.5) != 2.5 || math_abs(2.5) != 2.5) return 1;
    if (!close_to(math_sqrt(2.0), 1.4142135623730951, 1.0e-12)) return 2;
    if (!close_to(math_exp(1.0), MATH_E, 1.0e-12)) return 3;
    if (!close_to(math_log(MATH_E), 1.0, 1.0e-12)) return 4;
    if (!close_to(math_sin(MATH_PI / 2.0), 1.0, 1.0e-12)) return 5;
    if (!close_to(math_cos(MATH_PI), -1.0, 1.0e-12)) return 6;
    if (!close_to(math_atan(1.0), MATH_PI / 4.0, 1.0e-12)) return 7;
    if (!close_to(math_tan(MATH_PI / 4.0), 1.0, 1.0e-12)) return 8;
    if (!close_to(math_asin(0.5), MATH_PI / 6.0, 1.0e-12)) return 9;
    if (!close_to(math_acos(0.5), MATH_PI / 3.0, 1.0e-12)) return 10;

    sinh_one = math_sinh(1.0);
    cosh_one = math_cosh(1.0);
    if (!close_to(cosh_one * cosh_one - sinh_one * sinh_one, 1.0, 1.0e-12)) return 11;
    if (!close_to(math_tanh(1.0), sinh_one / cosh_one, 1.0e-12)) return 12;

    if (math_floor(2.75) != 2.0 || math_floor(-2.25) != -3.0) return 13;
    if (math_ceil(2.25) != 3.0 || math_ceil(-2.75) != -2.0) return 14;
    if (math_round(2.5) != 3.0 || math_round(-2.5) != -3.0) return 15;
    if (math_pow_int(-2.0, 3) != -8.0 || math_pow_int(2.0, -3) != 0.125) return 16;
    if (!close_to(math_pow(9.0, 0.5), 3.0, 1.0e-12)) return 17;

    if (!is_nan(math_sqrt(-1.0))) return 18;
    if (!is_nan(math_log(0.0))) return 19;
    if (!is_nan(math_asin(2.0))) return 20;
    if (!is_nan(math_pow(-2.0, 0.5))) return 21;

    return 0;
}
