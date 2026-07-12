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

double math_abs(double value);
double math_sqrt(double value);
double math_exp(double value);
double math_log(double value);
double math_pow(double base, double exponent);
double math_pow_int(double base, long long exponent);

double math_sin(double value);
double math_cos(double value);
double math_tan(double value);
double math_asin(double value);
double math_acos(double value);
double math_atan(double value);

double math_sinh(double value);
double math_cosh(double value);
double math_tanh(double value);

double math_floor(double value);
double math_ceil(double value);
double math_round(double value);
```

The implementation uses Newton iteration, range reduction, and convergent series. It does not call a host math library.

## CURRENT USERS

- `solve` uses the elementary, trigonometric, hyperbolic, rounding, and power functions
- `printf` uses shared absolute-value and integer-power helpers for floating conversion and formatting
- the TrueType rasterizer uses shared floor and ceiling operations while computing quadratic outline bounds
- `bc` intentionally does not use this binary64 layer; its calculations use decimal, arbitrary-precision `BcValue` and `bignum` operations with user-controlled scale

## NUMERICAL PROFILE

The supported profile is finite binary64 input in the ordinary ranges used by current project tools. Results are approximate and tests use operation-appropriate tolerances rather than bit-for-bit comparison.

Domain errors such as `sqrt(-1)`, `log(0)`, `asin(2)`, or a non-integral power of a negative base return NaN. The API does not set `errno`, raise project-level diagnostics, or expose floating-point exception state.

Halfway cases are rounded away from zero. General powers recognize near-integral exponents in the range -1024 through 1024 and otherwise evaluate the exponential of the exponent multiplied by the logarithm of the base.

## LIMITATIONS

- there are no `float` or `long double` variants
- subnormal, signed-zero, infinity, NaN payload, and floating-environment behavior are not specified
- argument reduction for very large trigonometric inputs is simple and can lose precision or take excessive time
- overflow and underflow are left to the target's hardware binary64 behavior
- integral rounding requires values representable as `long long`
- correctly rounded results and libm-level error bounds are not guaranteed
- decimal parsing and formatting remain tool/runtime concerns rather than part of this mathematical API
- ncc currently parses `double` as an 8-byte integer type and cannot execute this API correctly; GCC or Clang builds provide the current floating-point implementation path

## TESTING

The standalone math fixture covers representative values, identities, rounding rules, integer powers, and domain errors. `make test` runs the fixture in hosted form and, on Linux, as a static no-libc freestanding binary. Tool-level behavior remains covered by the `solve` and `printf` Phase 1 tests.

## SEE ALSO

runtime, compiler, testing, bc, solve, printf
