#ifndef NEWOS_MATH_H
#define NEWOS_MATH_H

#define MATH_PI 3.14159265358979323846264338327950288419716939937510
#define MATH_E 2.71828182845904523536028747135266249775724709369995

double math_abs(double value);
double math_sqrt(double value);
double math_exp(double value);
double math_log(double value);
double math_atan(double value);
double math_sin(double value);
double math_cos(double value);
double math_tan(double value);
double math_asin(double value);
double math_acos(double value);
double math_sinh(double value);
double math_cosh(double value);
double math_tanh(double value);
double math_floor(double value);
double math_ceil(double value);
double math_round(double value);
double math_pow_int(double base, long long exponent);
double math_pow(double base, double exponent);

#endif
