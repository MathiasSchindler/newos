#include "bignum.h"
#include "runtime.h"
#include "tool_util.h"

#include <limits.h>

#define SEQ_TEXT_CAPACITY 2048U
#define SEQ_LITERAL_CAPACITY 128U
#define SEQ_MAX_FORMAT_NUMBER 1024U

typedef struct {
    Bignum coefficient;
    unsigned int scale;
} DecimalValue;

typedef struct {
    int enabled;
    int precision;
    int min_width;
    int left_adjust;
    int show_sign;
    int leading_space;
    int zero_pad;
    int grouping;
    char conversion;
    char prefix[SEQ_LITERAL_CAPACITY];
    char suffix[SEQ_LITERAL_CAPACITY];
} FormatSpec;

static int append_char(char *buffer, size_t buffer_size, size_t *length_io, char ch) {
    if (*length_io + 1U >= buffer_size) {
        return -1;
    }
    buffer[*length_io] = ch;
    *length_io += 1U;
    buffer[*length_io] = '\0';
    return 0;
}

static int append_text(char *buffer, size_t buffer_size, size_t *length_io, const char *text) {
    size_t index = 0U;

    while (text[index] != '\0') {
        if (append_char(buffer, buffer_size, length_io, text[index]) != 0) {
            return -1;
        }
        index += 1U;
    }
    return 0;
}

static int parse_uint_limited(const char *text, size_t *index_io, int *value_out) {
    size_t index = *index_io;
    unsigned int value = 0U;
    int have_digit = 0;

    while (text[index] >= '0' && text[index] <= '9') {
        unsigned int digit = (unsigned int)(text[index] - '0');
        if (value > (SEQ_MAX_FORMAT_NUMBER - digit) / 10U) {
            return -1;
        }
        value = value * 10U + digit;
        have_digit = 1;
        index += 1U;
    }
    if (!have_digit) {
        return -1;
    }
    *value_out = (int)value;
    *index_io = index;
    return 0;
}

static int parse_signed_exponent(const char *text, size_t *index_io, int *value_out) {
    size_t index = *index_io;
    int sign = 1;
    int value = 0;

    if (text[index] == '+' || text[index] == '-') {
        if (text[index] == '-') {
            sign = -1;
        }
        index += 1U;
    }
    if (parse_uint_limited(text, &index, &value) != 0) {
        return -1;
    }
    *value_out = sign * value;
    *index_io = index;
    return 0;
}

static int decimal_from_parts(DecimalValue *value_out, int negative, char *digits, size_t digit_count, unsigned int scale) {
    char text[SEQ_TEXT_CAPACITY];
    size_t out = 0U;
    size_t first_nonzero = 0U;

    while (first_nonzero + 1U < digit_count && digits[first_nonzero] == '0') {
        first_nonzero += 1U;
    }
    if (negative) {
        text[out++] = '-';
    }
    while (first_nonzero < digit_count) {
        if (out + 1U >= sizeof(text)) {
            return -1;
        }
        text[out++] = digits[first_nonzero++];
    }
    text[out] = '\0';
    if (bn_from_string(&value_out->coefficient, text) != 0) {
        return -1;
    }
    value_out->scale = scale;
    return 0;
}

static int parse_decimal_value(const char *text, DecimalValue *value_out) {
    char digits[SEQ_TEXT_CAPACITY];
    size_t index = 0U;
    size_t digit_count = 0U;
    unsigned int fractional_digits = 0U;
    int seen_digit = 0;
    int seen_point = 0;
    int negative = 0;
    int exponent = 0;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }
    if (text[index] == '-' || text[index] == '+') {
        negative = text[index] == '-';
        index += 1U;
    }
    while (text[index] != '\0') {
        if (text[index] >= '0' && text[index] <= '9') {
            if (digit_count + 1U >= sizeof(digits)) {
                return -1;
            }
            digits[digit_count++] = text[index];
            if (seen_point) {
                fractional_digits += 1U;
            }
            seen_digit = 1;
            index += 1U;
        } else if (text[index] == '.' && !seen_point) {
            seen_point = 1;
            index += 1U;
        } else {
            break;
        }
    }
    if (!seen_digit) {
        return -1;
    }
    if (text[index] == 'e' || text[index] == 'E') {
        index += 1U;
        if (parse_signed_exponent(text, &index, &exponent) != 0) {
            return -1;
        }
    }
    if (text[index] != '\0') {
        return -1;
    }
    if (exponent >= 0) {
        unsigned int positive_exponent = (unsigned int)exponent;
        if (positive_exponent >= fractional_digits) {
            unsigned int zeros = positive_exponent - fractional_digits;
            while (zeros > 0U) {
                if (digit_count + 1U >= sizeof(digits)) {
                    return -1;
                }
                digits[digit_count++] = '0';
                zeros -= 1U;
            }
            fractional_digits = 0U;
        } else {
            fractional_digits -= positive_exponent;
        }
    } else {
        unsigned int negative_exponent = (unsigned int)(-exponent);
        if (fractional_digits > UINT_MAX - negative_exponent) {
            return -1;
        }
        fractional_digits += negative_exponent;
    }
    return decimal_from_parts(value_out, negative, digits, digit_count, fractional_digits);
}

static int rescale_value(const DecimalValue *value, unsigned int target_scale, Bignum *result_out) {
    if (target_scale < value->scale) {
        return -1;
    }
    return bn_scale(&value->coefficient, (int)(target_scale - value->scale), result_out);
}

static int decimal_compare_scaled(const Bignum *lhs, const Bignum *rhs) {
    return bn_compare(lhs, rhs);
}

static int decimal_add_scaled(const Bignum *lhs, const Bignum *rhs, Bignum *result_out) {
    return bn_add(lhs, rhs, result_out);
}

static int copy_format_literal(char *buffer, size_t buffer_size, const char *text, size_t *index) {
    size_t length = 0U;

    if (buffer_size == 0U) {
        return -1;
    }
    while (text[*index] != '\0') {
        if (text[*index] != '%') {
            if (length + 1U >= buffer_size) {
                return -1;
            }
            buffer[length++] = text[*index];
            *index += 1U;
            continue;
        }
        if (text[*index + 1U] == '%') {
            if (length + 1U >= buffer_size) {
                return -1;
            }
            buffer[length++] = '%';
            *index += 2U;
            continue;
        }
        break;
    }
    buffer[length] = '\0';
    return 0;
}

static int parse_format_spec(const char *text, FormatSpec *spec) {
    size_t index = 0U;

    if (text == 0 || spec == 0) {
        return -1;
    }
    rt_memset(spec, 0, sizeof(*spec));
    spec->enabled = 1;
    spec->precision = -1;
    spec->conversion = 'g';
    if (copy_format_literal(spec->prefix, sizeof(spec->prefix), text, &index) != 0 || text[index] != '%') {
        return -1;
    }
    index += 1U;
    while (text[index] == '-' || text[index] == '+' || text[index] == ' ' || text[index] == '0' || text[index] == '\'') {
        if (text[index] == '-') spec->left_adjust = 1;
        if (text[index] == '+') spec->show_sign = 1;
        if (text[index] == ' ') spec->leading_space = 1;
        if (text[index] == '0') spec->zero_pad = 1;
        if (text[index] == '\'') spec->grouping = 1;
        index += 1U;
    }
    if (text[index] >= '0' && text[index] <= '9') {
        if (parse_uint_limited(text, &index, &spec->min_width) != 0) {
            return -1;
        }
    }
    if (text[index] == '.') {
        index += 1U;
        if (parse_uint_limited(text, &index, &spec->precision) != 0) {
            return -1;
        }
    }
    if (text[index] == 'f' || text[index] == 'F' || text[index] == 'e' || text[index] == 'E' || text[index] == 'g' || text[index] == 'G') {
        spec->conversion = text[index];
        index += 1U;
    } else {
        return -1;
    }
    if (copy_format_literal(spec->suffix, sizeof(spec->suffix), text, &index) != 0 || text[index] != '\0') {
        return -1;
    }
    if (spec->left_adjust) {
        spec->zero_pad = 0;
    }
    return 0;
}

static int split_signed_digits(const Bignum *value, char *digits, size_t digits_size, int *negative_out) {
    char text[SEQ_TEXT_CAPACITY];
    size_t index = 0U;
    size_t out = 0U;

    if (bn_to_string(value, text, sizeof(text)) != 0) {
        return -1;
    }
    *negative_out = text[0] == '-';
    if (*negative_out) {
        index = 1U;
    }
    while (text[index] == '0' && text[index + 1U] != '\0') {
        index += 1U;
    }
    while (text[index] != '\0') {
        if (out + 1U >= digits_size) {
            return -1;
        }
        digits[out++] = text[index++];
    }
    digits[out] = '\0';
    if (out == 1U && digits[0] == '0') {
        *negative_out = 0;
    }
    return 0;
}

static int digits_add_one(char *digits, size_t digits_size) {
    size_t length = rt_strlen(digits);
    size_t index = length;

    while (index > 0U) {
        index -= 1U;
        if (digits[index] < '9') {
            digits[index] += 1;
            return 0;
        }
        digits[index] = '0';
    }
    if (length + 1U >= digits_size) {
        return -1;
    }
    for (index = length + 1U; index > 0U; --index) {
        digits[index] = digits[index - 1U];
    }
    digits[0] = '1';
    return 0;
}

static int round_to_scaled_digits(const char *digits, unsigned int source_scale, unsigned int target_scale, char *out, size_t out_size) {
    size_t length = rt_strlen(digits);
    size_t keep;
    size_t index;

    if (source_scale > target_scale) {
        unsigned int drop = source_scale - target_scale;
        char round_digit = '0';

        keep = length > (size_t)drop ? length - (size_t)drop : 0U;
        if (keep >= out_size) {
            return -1;
        }
        for (index = 0U; index < keep; ++index) {
            out[index] = digits[index];
        }
        if (keep == 0U) {
            out[0] = '0';
            out[1] = '\0';
        } else {
            out[keep] = '\0';
        }
        if (length >= (size_t)drop) {
            round_digit = digits[keep];
        }
        if (round_digit >= '5' && digits_add_one(out, out_size) != 0) {
            return -1;
        }
        return 0;
    }
    if (length + (size_t)(target_scale - source_scale) >= out_size) {
        return -1;
    }
    for (index = 0U; index < length; ++index) {
        out[index] = digits[index];
    }
    while (source_scale < target_scale) {
        out[index++] = '0';
        source_scale += 1U;
    }
    out[index] = '\0';
    return 0;
}

static int append_grouped_whole(char *buffer, size_t buffer_size, size_t *length_io, const char *whole) {
    size_t whole_len = rt_strlen(whole);
    size_t first_group = whole_len % 3U;
    size_t index = 0U;

    if (first_group == 0U) {
        first_group = 3U;
    }
    while (index < whole_len) {
        if (index > 0U && append_char(buffer, buffer_size, length_io, ',') != 0) {
            return -1;
        }
        do {
            if (append_char(buffer, buffer_size, length_io, whole[index]) != 0) {
                return -1;
            }
            index += 1U;
            first_group -= 1U;
        } while (index < whole_len && first_group > 0U);
        first_group = 3U;
    }
    return 0;
}

static int format_fixed_number(const Bignum *value, unsigned int scale, int precision, int trim_trailing, int grouping, char *buffer, size_t buffer_size) {
    char digits[SEQ_TEXT_CAPACITY];
    char rounded[SEQ_TEXT_CAPACITY];
    char whole[SEQ_TEXT_CAPACITY];
    size_t rounded_len;
    size_t whole_len;
    size_t fraction_len;
    size_t length = 0U;
    int negative = 0;
    size_t index;

    if (precision < 0) {
        precision = (int)scale;
    }
    if (precision < 0 || precision > (int)SEQ_MAX_FORMAT_NUMBER) {
        return -1;
    }
    if (split_signed_digits(value, digits, sizeof(digits), &negative) != 0 ||
        round_to_scaled_digits(digits, scale, (unsigned int)precision, rounded, sizeof(rounded)) != 0) {
        return -1;
    }
    rounded_len = rt_strlen(rounded);
    while (rounded_len <= (size_t)precision) {
        if (rounded_len + 1U >= sizeof(rounded)) {
            return -1;
        }
        for (index = rounded_len + 1U; index > 0U; --index) {
            rounded[index] = rounded[index - 1U];
        }
        rounded[0] = '0';
        rounded_len += 1U;
    }
    whole_len = rounded_len - (size_t)precision;
    if (whole_len >= sizeof(whole)) {
        return -1;
    }
    for (index = 0U; index < whole_len; ++index) {
        whole[index] = rounded[index];
    }
    whole[whole_len] = '\0';
    fraction_len = (size_t)precision;
    if (trim_trailing) {
        while (fraction_len > 0U && rounded[whole_len + fraction_len - 1U] == '0') {
            fraction_len -= 1U;
        }
    }
    buffer[0] = '\0';
    if (negative && append_char(buffer, buffer_size, &length, '-') != 0) {
        return -1;
    }
    if (grouping) {
        if (append_grouped_whole(buffer, buffer_size, &length, whole) != 0) {
            return -1;
        }
    } else if (append_text(buffer, buffer_size, &length, whole) != 0) {
        return -1;
    }
    if (fraction_len > 0U) {
        if (append_char(buffer, buffer_size, &length, '.') != 0) {
            return -1;
        }
        for (index = 0U; index < fraction_len; ++index) {
            if (append_char(buffer, buffer_size, &length, rounded[whole_len + index]) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int format_exponential_number(const Bignum *value, unsigned int scale, int precision, int uppercase, char *buffer, size_t buffer_size) {
    char digits[SEQ_TEXT_CAPACITY];
    char significant[SEQ_TEXT_CAPACITY];
    char exponent_text[64];
    size_t digit_len;
    size_t keep;
    size_t index;
    size_t length = 0U;
    int negative = 0;
    int exponent;

    if (precision < 0) {
        precision = 6;
    }
    if (precision < 0 || precision > (int)SEQ_MAX_FORMAT_NUMBER) {
        return -1;
    }
    if (split_signed_digits(value, digits, sizeof(digits), &negative) != 0) {
        return -1;
    }
    if (digits[0] == '0' && digits[1] == '\0') {
        exponent = 0;
    } else {
        exponent = (int)rt_strlen(digits) - (int)scale - 1;
    }
    keep = (size_t)precision + 1U;
    digit_len = rt_strlen(digits);
    if (keep + 1U >= sizeof(significant)) {
        return -1;
    }
    for (index = 0U; index < keep; ++index) {
        significant[index] = index < digit_len ? digits[index] : '0';
    }
    significant[keep] = '\0';
    if (digit_len > keep && digits[keep] >= '5') {
        size_t before = rt_strlen(significant);
        if (digits_add_one(significant, sizeof(significant)) != 0) {
            return -1;
        }
        if (rt_strlen(significant) > before) {
            exponent += 1;
        }
    }
    buffer[0] = '\0';
    if (negative && append_char(buffer, buffer_size, &length, '-') != 0) {
        return -1;
    }
    if (append_char(buffer, buffer_size, &length, significant[0]) != 0) {
        return -1;
    }
    if (precision > 0) {
        if (append_char(buffer, buffer_size, &length, '.') != 0) {
            return -1;
        }
        for (index = 1U; index <= (size_t)precision; ++index) {
            if (append_char(buffer, buffer_size, &length, significant[index]) != 0) {
                return -1;
            }
        }
    }
    if (append_char(buffer, buffer_size, &length, uppercase ? 'E' : 'e') != 0 ||
        append_char(buffer, buffer_size, &length, exponent < 0 ? '-' : '+') != 0) {
        return -1;
    }
    if (exponent < 0) {
        exponent = -exponent;
    }
    rt_unsigned_to_string((unsigned long long)exponent, exponent_text, sizeof(exponent_text));
    if (exponent < 10 && append_char(buffer, buffer_size, &length, '0') != 0) {
        return -1;
    }
    return append_text(buffer, buffer_size, &length, exponent_text);
}

static int decimal_exponent(const Bignum *value, unsigned int scale) {
    char digits[SEQ_TEXT_CAPACITY];
    int negative;

    if (split_signed_digits(value, digits, sizeof(digits), &negative) != 0 || (digits[0] == '0' && digits[1] == '\0')) {
        return 0;
    }
    return (int)rt_strlen(digits) - (int)scale - 1;
}

static int format_general_number(const Bignum *value, unsigned int scale, int precision, int uppercase, char *buffer, size_t buffer_size) {
    int exponent;
    int fixed_precision;

    if (precision < 0) {
        precision = 6;
    }
    if (precision == 0) {
        precision = 1;
    }
    exponent = decimal_exponent(value, scale);
    if (exponent < -4 || exponent >= precision) {
        return format_exponential_number(value, scale, precision - 1, uppercase, buffer, buffer_size);
    }
    fixed_precision = precision - (exponent + 1);
    if (fixed_precision < 0) {
        fixed_precision = 0;
    }
    return format_fixed_number(value, scale, fixed_precision, 1, 0, buffer, buffer_size);
}

static int apply_sign_policy(char *number_text, size_t number_size, const FormatSpec *format) {
    size_t length = rt_strlen(number_text);
    size_t index;
    char sign = '\0';

    if (number_text[0] == '-') {
        return 0;
    }
    if (format->show_sign) {
        sign = '+';
    } else if (format->leading_space) {
        sign = ' ';
    }
    if (sign == '\0') {
        return 0;
    }
    if (length + 1U >= number_size) {
        return -1;
    }
    for (index = length + 1U; index > 0U; --index) {
        number_text[index] = number_text[index - 1U];
    }
    number_text[0] = sign;
    return 0;
}

static int write_with_width(const char *number_text, const FormatSpec *format) {
    int width = format->enabled ? format->min_width : 0;
    int length = (int)rt_strlen(number_text);
    int padding = width - length;
    int sign_prefix = (number_text[0] == '-' || number_text[0] == '+' || number_text[0] == ' ') ? 1 : 0;
    int i;

    if (padding < 0) {
        padding = 0;
    }
    if (format->left_adjust) {
        if (rt_write_cstr(1, number_text) != 0) {
            return -1;
        }
        for (i = 0; i < padding; ++i) {
            if (rt_write_char(1, ' ') != 0) {
                return -1;
            }
        }
        return 0;
    }
    if (format->zero_pad && sign_prefix) {
        if (rt_write_char(1, number_text[0]) != 0) {
            return -1;
        }
        for (i = 0; i < padding; ++i) {
            if (rt_write_char(1, '0') != 0) {
                return -1;
            }
        }
        return rt_write_cstr(1, number_text + 1);
    }
    for (i = 0; i < padding; ++i) {
        if (rt_write_char(1, format->zero_pad ? '0' : ' ') != 0) {
            return -1;
        }
    }
    return rt_write_cstr(1, number_text);
}

static int write_formatted_value(const Bignum *value, unsigned int scale, int default_precision, int equal_width, int width, const FormatSpec *format) {
    char number_text[SEQ_TEXT_CAPACITY];
    int precision = default_precision;
    char conversion = format->enabled ? format->conversion : 'f';

    if (format->enabled && format->precision >= 0) {
        precision = format->precision;
    }
    if (format->enabled && (conversion == 'e' || conversion == 'E')) {
        if (format_exponential_number(value, scale, precision, conversion == 'E', number_text, sizeof(number_text)) != 0) {
            return -1;
        }
    } else if (format->enabled && (conversion == 'g' || conversion == 'G')) {
        if (format_general_number(value, scale, precision, conversion == 'G', number_text, sizeof(number_text)) != 0) {
            return -1;
        }
    } else if (format_fixed_number(value, scale, precision, format->enabled && (conversion == 'g' || conversion == 'G'), format->enabled && format->grouping, number_text, sizeof(number_text)) != 0) {
        return -1;
    }
    if (format->enabled && apply_sign_policy(number_text, sizeof(number_text), format) != 0) {
        return -1;
    }
    if (format->enabled) {
        if (rt_write_cstr(1, format->prefix) != 0 || write_with_width(number_text, format) != 0 || rt_write_cstr(1, format->suffix) != 0) {
            return -1;
        }
        return 0;
    }
    if (equal_width) {
        FormatSpec width_format;
        rt_memset(&width_format, 0, sizeof(width_format));
        width_format.min_width = width;
        width_format.zero_pad = 1;
        return write_with_width(number_text, &width_format);
    }
    return rt_write_cstr(1, number_text);
}

static int should_continue(const Bignum *current, const Bignum *last, const Bignum *step) {
    int comparison = decimal_compare_scaled(current, last);
    return step->is_negative ? comparison >= 0 : comparison <= 0;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-w] [-s SEP] [-f FORMAT] [FIRST [STEP]] LAST");
}

int main(int argc, char **argv) {
    const char *separator = "\n";
    int equal_width = 0;
    int argi = 1;
    DecimalValue parsed_first;
    DecimalValue parsed_step;
    DecimalValue parsed_last;
    FormatSpec format;
    unsigned int scale = 0U;
    int number_count;
    int implicit_step = 0;
    Bignum current;
    Bignum first;
    Bignum step;
    Bignum last;
    int wrote_any = 0;
    int width = 0;
    int default_precision;
    char width_text[SEQ_TEXT_CAPACITY];

    bn_from_int(&parsed_first.coefficient, 1LL);
    parsed_first.scale = 0U;
    bn_from_int(&parsed_step.coefficient, 1LL);
    parsed_step.scale = 0U;
    bn_zero(&parsed_last.coefficient);
    parsed_last.scale = 0U;
    rt_memset(&format, 0, sizeof(format));
    format.precision = -1;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-w") == 0) {
            equal_width = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-s") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            separator = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (rt_strcmp(argv[argi], "-f") == 0) {
            if (argi + 1 >= argc || parse_format_spec(argv[argi + 1], &format) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        break;
    }

    number_count = argc - argi;
    if (number_count < 1 || number_count > 3) {
        print_usage(argv[0]);
        return 1;
    }
    if (number_count == 1) {
        if (parse_decimal_value(argv[argi], &parsed_last) != 0) {
            tool_write_error("seq", "invalid number: ", argv[argi]);
            return 1;
        }
        implicit_step = 1;
    } else if (number_count == 2) {
        if (parse_decimal_value(argv[argi], &parsed_first) != 0 || parse_decimal_value(argv[argi + 1], &parsed_last) != 0) {
            tool_write_error("seq", "invalid number", 0);
            return 1;
        }
        implicit_step = 1;
    } else if (parse_decimal_value(argv[argi], &parsed_first) != 0 ||
               parse_decimal_value(argv[argi + 1], &parsed_step) != 0 ||
               parse_decimal_value(argv[argi + 2], &parsed_last) != 0) {
        tool_write_error("seq", "invalid number", 0);
        return 1;
    }

    scale = parsed_first.scale;
    if (parsed_step.scale > scale) scale = parsed_step.scale;
    if (parsed_last.scale > scale) scale = parsed_last.scale;
    if (rescale_value(&parsed_first, scale, &first) != 0 || rescale_value(&parsed_last, scale, &last) != 0) {
        tool_write_error("seq", "number is too large", 0);
        return 1;
    }
    if (implicit_step) {
        Bignum one;
        bn_from_uint(&one, 1ULL);
        if (bn_scale(&one, (int)scale, &step) != 0) {
            tool_write_error("seq", "number is too large", 0);
            return 1;
        }
        if (decimal_compare_scaled(&last, &first) < 0) {
            bn_negate(&step);
        }
    } else if (rescale_value(&parsed_step, scale, &step) != 0) {
        tool_write_error("seq", "number is too large", 0);
        return 1;
    }
    if (bn_is_zero(&step)) {
        tool_write_error("seq", "step must not be zero", 0);
        return 1;
    }

    default_precision = (int)scale;
    if (equal_width) {
        if (format_fixed_number(&first, scale, default_precision, 0, 0, width_text, sizeof(width_text)) != 0) {
            return 1;
        }
        width = (int)rt_strlen(width_text);
        if (format_fixed_number(&last, scale, default_precision, 0, 0, width_text, sizeof(width_text)) != 0) {
            return 1;
        }
        if ((int)rt_strlen(width_text) > width) {
            width = (int)rt_strlen(width_text);
        }
    }

    current = first;
    while (should_continue(&current, &last, &step)) {
        Bignum next;

        if (wrote_any && rt_write_cstr(1, separator) != 0) {
            return 1;
        }
        if (write_formatted_value(&current, scale, default_precision, equal_width, width, &format) != 0) {
            return 1;
        }
        wrote_any = 1;
        if (decimal_compare_scaled(&current, &last) == 0) {
            break;
        }
        if (decimal_add_scaled(&current, &step, &next) != 0) {
            tool_write_error("seq", "number is too large", 0);
            return 1;
        }
        if ((step.is_negative && decimal_compare_scaled(&next, &current) > 0) || (!step.is_negative && decimal_compare_scaled(&next, &current) < 0)) {
            break;
        }
        current = next;
    }

    if (wrote_any && rt_write_char(1, '\n') != 0) {
        return 1;
    }
    return 0;
}