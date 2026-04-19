#include "bignum.h"
#include "runtime.h"

void bn_zero(Bignum *bn) {
    bn->length = 0;
    bn->is_negative = 0;
    for (unsigned int i = 0; i < BN_MAX_DIGITS; i++) {
        bn->digits[i] = 0;
    }
}

void bn_from_uint(Bignum *bn, unsigned long long value) {
    bn_zero(bn);
    if (value == 0) {
        return;
    }
    
    while (value > 0 && bn->length < BN_MAX_DIGITS) {
        bn->digits[bn->length++] = (unsigned int)(value % BN_DIGIT_BASE);
        value /= BN_DIGIT_BASE;
    }
}

void bn_from_int(Bignum *bn, long long value) {
    if (value < 0) {
        bn_from_uint(bn, (unsigned long long)(-(value + 1LL)) + 1ULL);
        bn->is_negative = 1;
    } else {
        bn_from_uint(bn, (unsigned long long)value);
    }
}

int bn_from_string(Bignum *bn, const char *text) {
    bn_zero(bn);
    
    if (text == 0 || *text == '\0') {
        return -1;
    }
    
    const char *p = text;
    if (*p == '-') {
        bn->is_negative = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    
    if (*p == '\0') {
        return -1;
    }
    
    while (*p == '0' && *(p + 1) != '\0') {
        p++;
    }
    
    size_t text_len = rt_strlen(p);
    if (text_len == 0) {
        bn->is_negative = 0;
        return 0;
    }
    
    for (size_t i = 0; i < text_len; i++) {
        if (p[i] < '0' || p[i] > '9') {
            bn_zero(bn);
            return -1;
        }
    }
    
    size_t groups = (text_len + BN_DIGIT_DECIMALS - 1) / BN_DIGIT_DECIMALS;
    if (groups > BN_MAX_DIGITS) {
        return -1;
    }
    
    for (size_t g = 0; g < groups; g++) {
        unsigned int digit_value = 0;
        size_t start_pos = (text_len > (g + 1) * BN_DIGIT_DECIMALS) 
                          ? (text_len - (g + 1) * BN_DIGIT_DECIMALS) 
                          : 0;
        size_t end_pos = text_len - g * BN_DIGIT_DECIMALS;
        
        for (size_t i = start_pos; i < end_pos; i++) {
            digit_value = digit_value * 10 + (unsigned int)(p[i] - '0');
        }
        
        bn->digits[g] = digit_value;
        bn->length++;
    }
    
    bn_normalize(bn);
    return 0;
}

int bn_to_string(const Bignum *bn, char *buffer, size_t buffer_size) {
    if (buffer_size < 2) {
        return -1;
    }
    
    if (bn_is_zero(bn)) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return 0;
    }
    
    size_t pos = 0;
    if (bn->is_negative) {
        if (pos + 1 >= buffer_size) {
            return -1;
        }
        buffer[pos++] = '-';
    }
    
    int started = 0;
    for (unsigned int i = bn->length; i > 0; i--) {
        unsigned int digit = bn->digits[i - 1];
        
        if (!started) {
            char temp[16];
            rt_unsigned_to_string(digit, temp, sizeof(temp));
            size_t temp_len = rt_strlen(temp);
            if (pos + temp_len >= buffer_size) {
                return -1;
            }
            for (size_t j = 0; j < temp_len; j++) {
                buffer[pos++] = temp[j];
            }
            started = 1;
        } else {
            if (pos + BN_DIGIT_DECIMALS >= buffer_size) {
                return -1;
            }
            for (int j = BN_DIGIT_DECIMALS - 1; j >= 0; j--) {
                unsigned int divisor = 1;
                for (int k = 0; k < j; k++) {
                    divisor *= 10;
                }
                buffer[pos++] = '0' + (char)((digit / divisor) % 10);
            }
        }
    }
    
    buffer[pos] = '\0';
    return 0;
}

int bn_is_zero(const Bignum *bn) {
    return bn->length == 0;
}

void bn_normalize(Bignum *bn) {
    while (bn->length > 0 && bn->digits[bn->length - 1] == 0) {
        bn->length--;
    }
    if (bn->length == 0) {
        bn->is_negative = 0;
    }
}

void bn_negate(Bignum *bn) {
    if (!bn_is_zero(bn)) {
        bn->is_negative = !bn->is_negative;
    }
}

int bn_compare_abs(const Bignum *a, const Bignum *b) {
    if (a->length > b->length) {
        return 1;
    }
    if (a->length < b->length) {
        return -1;
    }
    
    for (unsigned int i = a->length; i > 0; i--) {
        if (a->digits[i - 1] > b->digits[i - 1]) {
            return 1;
        }
        if (a->digits[i - 1] < b->digits[i - 1]) {
            return -1;
        }
    }
    
    return 0;
}

int bn_compare(const Bignum *a, const Bignum *b) {
    int a_zero = bn_is_zero(a);
    int b_zero = bn_is_zero(b);
    
    if (a_zero && b_zero) {
        return 0;
    }
    
    if (a->is_negative && !b->is_negative) {
        return -1;
    }
    if (!a->is_negative && b->is_negative) {
        return 1;
    }
    
    int cmp = bn_compare_abs(a, b);
    return a->is_negative ? -cmp : cmp;
}

int bn_add_unsigned(const Bignum *a, const Bignum *b, Bignum *result) {
    bn_zero(result);
    
    unsigned int max_len = (a->length > b->length) ? a->length : b->length;
    if (max_len >= BN_MAX_DIGITS) {
        return -1;
    }
    
    unsigned long long carry = 0;
    for (unsigned int i = 0; i < max_len || carry > 0; i++) {
        if (i >= BN_MAX_DIGITS) {
            return -1;
        }
        
        unsigned long long sum = carry;
        if (i < a->length) {
            sum += a->digits[i];
        }
        if (i < b->length) {
            sum += b->digits[i];
        }
        
        result->digits[i] = (unsigned int)(sum % BN_DIGIT_BASE);
        result->length = i + 1;
        carry = sum / BN_DIGIT_BASE;
    }
    
    bn_normalize(result);
    return 0;
}

int bn_subtract_unsigned(const Bignum *a, const Bignum *b, Bignum *result) {
    if (bn_compare_abs(a, b) < 0) {
        return -1;
    }
    
    bn_zero(result);
    
    long long borrow = 0;
    for (unsigned int i = 0; i < a->length; i++) {
        long long diff = (long long)a->digits[i] - borrow;
        if (i < b->length) {
            diff -= (long long)b->digits[i];
        }
        
        if (diff < 0) {
            diff += BN_DIGIT_BASE;
            borrow = 1;
        } else {
            borrow = 0;
        }
        
        result->digits[i] = (unsigned int)diff;
        result->length = i + 1;
    }
    
    bn_normalize(result);
    return 0;
}

int bn_add(const Bignum *a, const Bignum *b, Bignum *result) {
    if (a->is_negative == b->is_negative) {
        if (bn_add_unsigned(a, b, result) != 0) {
            return -1;
        }
        result->is_negative = a->is_negative;
        return 0;
    }
    
    int cmp = bn_compare_abs(a, b);
    if (cmp == 0) {
        bn_zero(result);
        return 0;
    }
    
    if (cmp > 0) {
        if (bn_subtract_unsigned(a, b, result) != 0) {
            return -1;
        }
        result->is_negative = a->is_negative;
    } else {
        if (bn_subtract_unsigned(b, a, result) != 0) {
            return -1;
        }
        result->is_negative = b->is_negative;
    }
    
    return 0;
}

int bn_subtract(const Bignum *a, const Bignum *b, Bignum *result) {
    Bignum b_neg = *b;
    bn_negate(&b_neg);
    return bn_add(a, &b_neg, result);
}

int bn_multiply_digit(const Bignum *bn, unsigned int digit, Bignum *result) {
    bn_zero(result);
    
    if (digit == 0) {
        return 0;
    }
    
    unsigned long long carry = 0;
    for (unsigned int i = 0; i < bn->length || carry > 0; i++) {
        if (i >= BN_MAX_DIGITS) {
            return -1;
        }
        
        unsigned long long product = carry;
        if (i < bn->length) {
            product += (unsigned long long)bn->digits[i] * digit;
        }
        
        result->digits[i] = (unsigned int)(product % BN_DIGIT_BASE);
        result->length = i + 1;
        carry = product / BN_DIGIT_BASE;
    }
    
    result->is_negative = bn->is_negative;
    bn_normalize(result);
    return 0;
}

int bn_shift_left_digits(const Bignum *bn, unsigned int positions, Bignum *result) {
    if (bn_is_zero(bn)) {
        bn_zero(result);
        return 0;
    }
    
    if (bn->length + positions > BN_MAX_DIGITS) {
        return -1;
    }
    
    bn_zero(result);
    for (unsigned int i = 0; i < bn->length; i++) {
        result->digits[i + positions] = bn->digits[i];
    }
    result->length = bn->length + positions;
    result->is_negative = bn->is_negative;
    
    return 0;
}

int bn_multiply(const Bignum *a, const Bignum *b, Bignum *result) {
    Bignum temp;
    bn_zero(&temp);
    
    if (bn_is_zero(a) || bn_is_zero(b)) {
        bn_zero(result);
        return 0;
    }
    
    for (unsigned int i = 0; i < b->length; i++) {
        Bignum partial;
        if (bn_multiply_digit(a, b->digits[i], &partial) != 0) {
            return -1;
        }
        
        Bignum shifted;
        if (bn_shift_left_digits(&partial, i, &shifted) != 0) {
            return -1;
        }
        
        Bignum new_temp;
        if (bn_add_unsigned(&temp, &shifted, &new_temp) != 0) {
            return -1;
        }
        temp = new_temp;
    }
    
    *result = temp;
    result->is_negative = (a->is_negative != b->is_negative);
    bn_normalize(result);
    return 0;
}

int bn_divide_digit(const Bignum *bn, unsigned int digit,
                    Bignum *quotient, unsigned int *remainder) {
    if (digit == 0) {
        return -1;
    }
    
    bn_zero(quotient);
    
    unsigned long long rem = 0;
    for (unsigned int i = bn->length; i > 0; i--) {
        rem = rem * BN_DIGIT_BASE + bn->digits[i - 1];
        unsigned long long q = rem / digit;
        rem = rem % digit;
        
        if (q > 0 || quotient->length > 0) {
            quotient->digits[quotient->length++] = (unsigned int)q;
        }
    }
    
    if (quotient->length > 0) {
        for (unsigned int i = 0; i < quotient->length / 2; i++) {
            unsigned int temp = quotient->digits[i];
            quotient->digits[i] = quotient->digits[quotient->length - 1 - i];
            quotient->digits[quotient->length - 1 - i] = temp;
        }
    }
    
    quotient->is_negative = bn->is_negative;
    bn_normalize(quotient);
    
    if (remainder != 0) {
        *remainder = (unsigned int)rem;
    }
    
    return 0;
}

int bn_divide(const Bignum *dividend, const Bignum *divisor,
              Bignum *quotient, Bignum *remainder) {
    if (bn_is_zero(divisor)) {
        return -1;
    }
    
    if (bn_is_zero(dividend)) {
        bn_zero(quotient);
        bn_zero(remainder);
        return 0;
    }
    
    int cmp = bn_compare_abs(dividend, divisor);
    if (cmp < 0) {
        bn_zero(quotient);
        *remainder = *dividend;
        return 0;
    }
    
    if (cmp == 0) {
        bn_from_uint(quotient, 1);
        quotient->is_negative = (dividend->is_negative != divisor->is_negative);
        bn_zero(remainder);
        return 0;
    }
    
    Bignum current_dividend = *dividend;
    current_dividend.is_negative = 0;
    
    Bignum div = *divisor;
    div.is_negative = 0;
    
    bn_zero(quotient);
    bn_zero(remainder);
    
    for (int i = (int)current_dividend.length - 1; i >= 0; i--) {
        Bignum temp_remainder;
        if (bn_shift_left_digits(remainder, 1, &temp_remainder) != 0) {
            return -1;
        }
        *remainder = temp_remainder;
        
        if (remainder->length == 0) {
            remainder->length = 1;
        }
        remainder->digits[0] = current_dividend.digits[i];
        bn_normalize(remainder);
        
        unsigned int count = 0;
        unsigned int low = 0;
        unsigned int high = BN_DIGIT_BASE - 1U;

        while (low <= high) {
            unsigned int mid = low + ((high - low) / 2U);
            Bignum product;
            int cmp;

            if (bn_multiply_digit(&div, mid, &product) != 0) {
                return -1;
            }

            cmp = bn_compare_abs(&product, remainder);
            if (cmp <= 0) {
                count = mid;
                if (mid == BN_DIGIT_BASE - 1U) {
                    break;
                }
                low = mid + 1U;
            } else {
                if (mid == 0U) {
                    break;
                }
                high = mid - 1U;
            }
        }

        if (count > 0U) {
            Bignum product;
            Bignum sub_result;
            if (bn_multiply_digit(&div, count, &product) != 0) {
                return -1;
            }
            if (bn_subtract_unsigned(remainder, &product, &sub_result) != 0) {
                return -1;
            }
            *remainder = sub_result;
        }
        
        if (bn_shift_left_digits(quotient, 1, &temp_remainder) != 0) {
            return -1;
        }
        *quotient = temp_remainder;
        
        if (quotient->length == 0) {
            quotient->length = 1;
        }
        quotient->digits[0] = count;
        bn_normalize(quotient);
    }
    
    quotient->is_negative = (dividend->is_negative != divisor->is_negative);
    remainder->is_negative = dividend->is_negative;
    
    bn_normalize(quotient);
    bn_normalize(remainder);
    
    return 0;
}

int bn_power(const Bignum *base, unsigned long long exponent, Bignum *result) {
    if (exponent == 0) {
        bn_from_uint(result, 1);
        return 0;
    }
    
    Bignum current_power = *base;
    bn_from_uint(result, 1);
    
    while (exponent > 0) {
        if (exponent & 1) {
            Bignum temp;
            if (bn_multiply(result, &current_power, &temp) != 0) {
                return -1;
            }
            *result = temp;
        }
        
        exponent >>= 1;
        
        if (exponent > 0) {
            Bignum temp;
            if (bn_multiply(&current_power, &current_power, &temp) != 0) {
                return -1;
            }
            current_power = temp;
        }
    }
    
    return 0;
}

int bn_scale(const Bignum *bn, int scale_power_of_10, Bignum *result) {
    if (scale_power_of_10 == 0) {
        *result = *bn;
        return 0;
    }
    
    if (scale_power_of_10 < 0) {
        int abs_scale = -scale_power_of_10;
        Bignum divisor;
        Bignum ten;
        bn_from_uint(&ten, 10);
        
        if (bn_power(&ten, (unsigned long long)abs_scale, &divisor) != 0) {
            return -1;
        }
        
        Bignum remainder;
        return bn_divide(bn, &divisor, result, &remainder);
    } else {
        Bignum multiplier;
        Bignum ten;
        bn_from_uint(&ten, 10);
        
        if (bn_power(&ten, (unsigned long long)scale_power_of_10, &multiplier) != 0) {
            return -1;
        }
        
        return bn_multiply(bn, &multiplier, result);
    }
}
