#include "../../src/shared/math.h"

static int close_to(double actual, double expected, double tolerance) {
    return math_abs(actual - expected) <= tolerance;
}

int main(void) {
    double sinh_one;
    double cosh_one;
    double integer_part;
    double negative_zero = math_copy_sign(0.0, -1.0);
    double infinity = math_infinity();
    double nan = math_nan();
    double fraction;
    int binary_exponent;

    if (!math_is_nan(nan) || math_is_nan(1.0)) return 1;
    if (!math_is_infinite(infinity) || !math_is_infinite(-infinity)) return 2;
    if (!math_is_finite(1.0) || math_is_finite(infinity) || math_is_finite(nan)) return 3;
    if (!math_sign_bit(negative_zero) || math_sign_bit(0.0)) return 4;
    if (math_sign_bit(math_abs(negative_zero)) || math_copy_sign(2.0, -1.0) != -2.0) return 5;

    if (math_abs(-2.5) != 2.5 || math_abs(2.5) != 2.5) return 6;
    if (!close_to(math_sqrt(2.0), 1.4142135623730951, 1.0e-12)) return 7;
    if (!close_to(math_sqrt(1.0e300) / 1.0e150, 1.0, 1.0e-12)) return 45;
    if (!close_to(math_sqrt(1.0e-300) / 1.0e-150, 1.0, 1.0e-12)) return 46;
    if (!close_to(math_exp(1.0), MATH_E, 1.0e-12)) return 8;
    if (!close_to(math_exp(math_log(1.0e200)) / 1.0e200, 1.0, 1.0e-12)) return 47;
    if (!close_to(math_exp(math_log(1.0e-200)) / 1.0e-200, 1.0, 1.0e-12)) return 48;
    if (!close_to(math_exp2(3.0), 8.0, 1.0e-12)) return 9;
    if (!close_to(math_log(MATH_E), 1.0, 1.0e-12)) return 10;
    if (!close_to(math_log2(8.0), 3.0, 1.0e-12)) return 11;
    if (!close_to(math_log10(1000.0), 3.0, 1.0e-12)) return 12;
    if (!close_to(math_sin(MATH_PI / 2.0), 1.0, 1.0e-12)) return 13;
    if (!close_to(math_cos(MATH_PI), -1.0, 1.0e-12)) return 14;
    if (!close_to(math_atan(1.0), MATH_PI / 4.0, 1.0e-12)) return 15;
    if (!close_to(math_atan2(1.0, -1.0), 3.0 * MATH_PI / 4.0, 1.0e-12)) return 16;
    if (!math_sign_bit(math_atan2(negative_zero, 1.0))) return 17;
    if (math_hypot(3.0, 4.0) != 5.0 || !math_is_infinite(math_hypot(infinity, nan))) return 18;
    if (!close_to(math_tan(MATH_PI / 4.0), 1.0, 1.0e-12)) return 19;
    if (!close_to(math_asin(0.5), MATH_PI / 6.0, 1.0e-12)) return 20;
    if (!close_to(math_acos(0.5), MATH_PI / 3.0, 1.0e-12)) return 21;

    sinh_one = math_sinh(1.0);
    cosh_one = math_cosh(1.0);
    if (!close_to(cosh_one * cosh_one - sinh_one * sinh_one, 1.0, 1.0e-12)) return 22;
    if (!close_to(math_tanh(1.0), sinh_one / cosh_one, 1.0e-12)) return 23;

    if (math_trunc(2.75) != 2.0 || math_trunc(-2.75) != -2.0) return 24;
    if (math_modf(-2.75, &integer_part) != -0.75 || integer_part != -2.0) return 25;
    if (math_fmod(7.5, 2.0) != 1.5 || math_fmod(-7.5, 2.0) != -1.5) return 26;
    fraction = math_frexp(12.0, &binary_exponent);
    if (fraction != 0.75 || binary_exponent != 4 || math_scalbn(fraction, binary_exponent) != 12.0) return 27;
    if (math_next_after(0.0, 1.0) == 0.0 || math_next_after(1.0, 2.0) <= 1.0) return 28;
    if (math_min(3.0, -2.0) != -2.0 || math_max(3.0, -2.0) != 3.0) return 29;
    if (!math_sign_bit(math_min(0.0, negative_zero)) || math_sign_bit(math_max(0.0, negative_zero))) return 30;
    if (math_clamp(-2.0, -1.0, 1.0) != -1.0 || math_clamp(2.0, -1.0, 1.0) != 1.0) return 31;
    if (math_floor(2.75) != 2.0 || math_floor(-2.25) != -3.0) return 32;
    if (math_ceil(2.25) != 3.0 || math_ceil(-2.75) != -2.0) return 33;
    if (math_floor(1.0e20) != 1.0e20 || math_ceil(-1.0e20) != -1.0e20) return 34;
    if (math_round(2.5) != 3.0 || math_round(-2.5) != -3.0) return 35;
    if (!math_sign_bit(math_round(-0.25))) return 36;
    if (math_pow_int(-2.0, 3) != -8.0 || math_pow_int(2.0, -3) != 0.125) return 37;
    if (!close_to(math_pow(9.0, 0.5), 3.0, 1.0e-12)) return 38;

    if (!math_is_nan(math_sqrt(-1.0))) return 39;
    if (!math_is_nan(math_log(0.0))) return 40;
    if (!math_is_nan(math_asin(2.0))) return 41;
    if (!math_is_nan(math_pow(-2.0, 0.5))) return 42;
    if (!math_is_nan(math_fmod(infinity, 2.0))) return 43;
    if (!math_is_nan(math_clamp(0.0, 1.0, -1.0))) return 44;

    return 0;
}
