#ifndef NEWOS_BIGNUM_H
#define NEWOS_BIGNUM_H

#include <stddef.h>

/*
 * bignum.h - freestanding arbitrary-precision signed integer arithmetic.
 *
 * Supports generic arbitrary-precision calculation primitives:
 *   - Sign handling
 *   - Parsing from decimal text
 *   - Formatting to decimal text
 *   - Normalization
 *   - Comparison
 *   - Addition/subtraction
 *   - Multiplication
 *   - Integer division/modulo
 *   - Decimal-scale operations
 *
 * Design:
 *   - Freestanding-first: no libc, no malloc, no external dependencies
 *   - Fixed-capacity stack-allocated storage with configurable limit
 *   - Little-endian digit representation (least significant first)
 *   - Base 1000000000 (9 decimal digits per digit for efficient decimal I/O)
 *   - Explicit sign flag
 *
 * Usage pattern:
 *   Bignum a, b, result;
 *   bn_from_string(&a, "12345678901234567890");
 *   bn_from_string(&b, "98765432109876543210");
 *   bn_add(&a, &b, &result);
 *   char buffer[512];
 *   bn_to_string(&result, buffer, sizeof(buffer));
 */

#define BN_DIGIT_BASE 1000000000U
#define BN_DIGIT_DECIMALS 9
#define BN_MAX_DIGITS 128

typedef struct {
    unsigned int digits[BN_MAX_DIGITS];
    unsigned int length;
    int is_negative;
} Bignum;

void bn_zero(Bignum *bn);
void bn_from_uint(Bignum *bn, unsigned long long value);
void bn_from_int(Bignum *bn, long long value);
int bn_from_string(Bignum *bn, const char *text);
int bn_to_string(const Bignum *bn, char *buffer, size_t buffer_size);

int bn_is_zero(const Bignum *bn);
int bn_compare_abs(const Bignum *a, const Bignum *b);
int bn_compare(const Bignum *a, const Bignum *b);

void bn_normalize(Bignum *bn);
void bn_negate(Bignum *bn);

int bn_add(const Bignum *a, const Bignum *b, Bignum *result);
int bn_subtract(const Bignum *a, const Bignum *b, Bignum *result);
int bn_multiply(const Bignum *a, const Bignum *b, Bignum *result);
int bn_divide(const Bignum *dividend, const Bignum *divisor,
              Bignum *quotient, Bignum *remainder);

int bn_add_unsigned(const Bignum *a, const Bignum *b, Bignum *result);
int bn_subtract_unsigned(const Bignum *a, const Bignum *b, Bignum *result);

int bn_multiply_digit(const Bignum *bn, unsigned int digit, Bignum *result);
int bn_divide_digit(const Bignum *bn, unsigned int digit,
                    Bignum *quotient, unsigned int *remainder);

int bn_shift_left_digits(const Bignum *bn, unsigned int positions, Bignum *result);

int bn_power(const Bignum *base, unsigned long long exponent, Bignum *result);
int bn_scale(const Bignum *bn, int scale_power_of_10, Bignum *result);

#endif
