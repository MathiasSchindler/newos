#ifndef NEWOS_MATH_H
#define NEWOS_MATH_H

#define MATH_PI 3.14159265358979323846264338327950288419716939937510
#define MATH_E 2.71828182845904523536028747135266249775724709369995
#define MATH_LN2 0.69314718055994530941723212145817656807550013436026
#define MATH_LN10 2.30258509299404568401799145468436420760110148862877

int math_is_nan(double value);
int math_is_infinite(double value);
int math_is_finite(double value);
int math_sign_bit(double value);
double math_copy_sign(double magnitude, double sign);
double math_nan(void);
double math_infinity(void);
double math_abs(double value);
double math_sqrt(double value);
double math_exp(double value);
double math_exp2(double value);
double math_log(double value);
double math_log2(double value);
double math_log10(double value);
double math_atan(double value);
double math_atan2(double y, double x);
double math_hypot(double x, double y);
double math_sin(double value);
double math_cos(double value);
double math_tan(double value);
double math_asin(double value);
double math_acos(double value);
double math_sinh(double value);
double math_cosh(double value);
double math_tanh(double value);
double math_trunc(double value);
double math_modf(double value, double *integer_part);
double math_fmod(double value, double divisor);
double math_frexp(double value, int *exponent_out);
double math_scalbn(double value, int exponent);
double math_next_after(double value, double toward);
double math_min(double left, double right);
double math_max(double left, double right);
double math_clamp(double value, double lower, double upper);
double math_floor(double value);
double math_ceil(double value);
double math_round(double value);
double math_pow_int(double base, long long exponent);
double math_pow(double base, double exponent);

#endif
