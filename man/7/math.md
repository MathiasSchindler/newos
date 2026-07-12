# MATH

## NAME

math - dependency-free binary64 helpers shared by project tools

## DESCRIPTION

The project math layer lives in `src/shared/math.h` and `src/shared/math.c`. It provides a compact set of `double` operations for tools that need ordinary hardware floating-point calculations without libc or libm. The source is compiled directly into hosted and freestanding binaries and has no operating-system dependencies.

This is a project API rather than a complete ISO C `math.h`. Public names use the `math_` prefix so they do not collide with compiler builtins or a hosted system libm.

## CURRENT CAPABILITIES

```
#define MATH_PI ...
#define MATH_E ...
#define MATH_LN2 ...
#define MATH_LN10 ...

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
double math_pow(double base, double exponent);
double math_pow_int(double base, long long exponent);

double math_sin(double value);
double math_cos(double value);
double math_tan(double value);
double math_asin(double value);
double math_acos(double value);
double math_atan(double value);
double math_atan2(double y, double x);
double math_hypot(double x, double y);

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
```

The implementation uses Newton iteration, range reduction, and convergent series. It does not call a host math library.

## CURRENT USERS

- `solve` uses the elementary, trigonometric, hyperbolic, rounding, classification, min/max, and power functions; its expression language also exposes `atan2`, `hypot`, `exp2`, `log2`, `log10`, `trunc`, and `fmod`
- `printf` uses shared classification, sign, absolute-value, and integer-power helpers for floating conversion and formatting, including signed zero, NaN, and infinity
- the TrueType rasterizer uses shared floor and ceiling operations while computing quadratic outline bounds
- `bc` intentionally does not use this binary64 layer; its calculations use decimal, arbitrary-precision `BcValue` and `bignum` operations with user-controlled scale

## NUMERICAL PROFILE

The supported profile is finite binary64 input in the ordinary ranges used by current project tools. Results are approximate and tests use operation-appropriate tolerances rather than bit-for-bit comparison.

Domain errors such as `sqrt(-1)`, `log(0)`, `asin(2)`, division by zero in `math_fmod`, or a non-integral power of a negative base return NaN. The API does not set `errno`, raise project-level diagnostics, or expose floating-point exception state.

Classification and sign operations inspect the binary64 representation directly. `math_abs`, `math_copy_sign`, rounding, decomposition, min/max, and next-representable-value operations preserve signed zero where their contracts require it. `math_min` and `math_max` return the numeric operand when exactly one operand is NaN and choose negative or positive zero respectively when both operands compare equal. `math_clamp` returns NaN for NaN inputs or reversed bounds.

`math_frexp` and `math_scalbn` support normal and subnormal binary64 values. `math_hypot` scales its operands to avoid intermediate overflow or underflow. `math_atan2` handles signed zero, axes, quadrants, and infinite operands.

Halfway cases are rounded away from zero. General powers recognize near-integral exponents in the range -1024 through 1024 and otherwise evaluate the exponential of the exponent multiplied by the logarithm of the base.

## LIMITATIONS

- there are no `float` or `long double` variants
- NaN payload preservation and floating-environment behavior are not specified
- argument reduction for very large trigonometric inputs is bounded but remains approximate and can lose precision
- overflow and underflow are left to the target's hardware binary64 behavior
- correctly rounded results and libm-level error bounds are not guaranteed
- decimal parsing and formatting remain tool/runtime concerns rather than part of this mathematical API
- ncc supports the binary64 operations needed to compile and execute this API on x86-64

## TESTING

The standalone math fixture covers classification, signed zero, infinities, NaN, decomposition, scaling, adjacent values, representative elementary values, identities, rounding rules, integer powers, and domain errors. `make test` runs the fixture in hosted form and, on Linux, as a static no-libc freestanding binary. Tool-level behavior remains covered by the `solve` and `printf` Phase 1 tests.

## SEE ALSO

runtime, compiler, testing, bc, solve, printf
