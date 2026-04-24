#include "runtime.h"

static int ascii_is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

static int parse_signed_value(const char *text, long long *value_out) {
    long long value = 0;
    long long sign = 1;

    if (text == 0 || value_out == 0) {
        return -1;
    }

    while (*text == ' ' || *text == '\t') {
        text += 1;
    }

    if (*text == '-') {
        sign = -1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }

    while (*text >= '0' && *text <= '9') {
        value = (value * 10) + (long long)(*text - '0');
        text += 1;
    }

    *value_out = value * sign;
    return 0;
}

static int parse_unsigned_value(const char *text, unsigned long long *value_out) {
    unsigned long long value = 0;

    if (text == 0 || value_out == 0) {
        return -1;
    }

    while (*text == ' ' || *text == '\t' || *text == '+') {
        text += 1;
    }

    if (*text == '-') {
        long long signed_value = 0;
        if (parse_signed_value(text, &signed_value) != 0) {
            return -1;
        }
        *value_out = (unsigned long long)signed_value;
        return 0;
    }

    while (*text >= '0' && *text <= '9') {
        value = (value * 10ULL) + (unsigned long long)(*text - '0');
        text += 1;
    }

    *value_out = value;
    return 0;
}

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length += 1U;
    }
    return length;
}

static double absolute_double(double value) {
    return value < 0.0 ? -value : value;
}

static double power_of_ten(int exponent) {
    double result = 1.0;
    int i;

    if (exponent >= 0) {
        for (i = 0; i < exponent; ++i) {
            result *= 10.0;
        }
    } else {
        for (i = 0; i < -exponent; ++i) {
            result /= 10.0;
        }
    }

    return result;
}

static int parse_float_value(const char *text, double *value_out) {
    double value = 0.0;
    double fraction_scale = 0.1;
    int sign = 1;
    int exponent_sign = 1;
    int exponent = 0;

    if (text == 0 || value_out == 0) {
        return -1;
    }

    while (*text == ' ' || *text == '\t') {
        text += 1;
    }

    if (*text == '-') {
        sign = -1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }

    while (ascii_is_digit(*text)) {
        value = (value * 10.0) + (double)(*text - '0');
        text += 1;
    }

    if (*text == '.') {
        text += 1;
        while (ascii_is_digit(*text)) {
            value += (double)(*text - '0') * fraction_scale;
            fraction_scale /= 10.0;
            text += 1;
        }
    }

    if (*text == 'e' || *text == 'E') {
        text += 1;
        if (*text == '-') {
            exponent_sign = -1;
            text += 1;
        } else if (*text == '+') {
            text += 1;
        }
        while (ascii_is_digit(*text)) {
            exponent = (exponent * 10) + (int)(*text - '0');
            text += 1;
        }
    }

    *value_out = (double)sign * value * power_of_ten(exponent_sign * exponent);
    return 0;
}

static void format_unsigned(unsigned long long value, unsigned int base, int uppercase, char *buffer, size_t buffer_size) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char scratch[64];
    size_t length = 0;
    size_t i;

    if (buffer_size == 0U) {
        return;
    }

    if (value == 0ULL) {
        if (buffer_size > 1U) {
            buffer[0] = '0';
            buffer[1] = '\0';
        } else {
            buffer[0] = '\0';
        }
        return;
    }

    while (value != 0ULL && length + 1U < sizeof(scratch)) {
        scratch[length++] = digits[value % base];
        value /= base;
    }

    for (i = 0; i < length && i + 1U < buffer_size; ++i) {
        buffer[i] = scratch[length - 1U - i];
    }
    buffer[i] = '\0';
}

static int append_char_to_buffer(char *buffer, size_t buffer_size, size_t *length_io, char ch) {
    if (*length_io + 1U >= buffer_size) {
        return -1;
    }
    buffer[*length_io] = ch;
    *length_io += 1U;
    buffer[*length_io] = '\0';
    return 0;
}

static int append_text_to_buffer(char *buffer, size_t buffer_size, size_t *length_io, const char *text) {
    size_t index = 0;

    while (text[index] != '\0') {
        if (append_char_to_buffer(buffer, buffer_size, length_io, text[index]) != 0) {
            return -1;
        }
        index += 1U;
    }

    return 0;
}

static int write_repeated_char(char ch, int count) {
    while (count > 0) {
        if (rt_write_char(1, ch) != 0) {
            return -1;
        }
        count -= 1;
    }
    return 0;
}

static int write_padded_chunk(const char *text, size_t length, int width, int left_align, char pad) {
    if (!left_align && width > (int)length) {
        if (write_repeated_char(pad, width - (int)length) != 0) {
            return -1;
        }
    }

    if (length > 0U && rt_write_all(1, text, length) != 0) {
        return -1;
    }

    if (left_align && width > (int)length) {
        if (write_repeated_char(' ', width - (int)length) != 0) {
            return -1;
        }
    }

    return 0;
}

static int write_padded_numeric_text(const char *text, int width, int left_align, char pad) {
    size_t length = text_length(text);
    size_t body_offset = 0U;
    int total_length = (int)length;

    if (!left_align && pad == '0' && text[0] == '-') {
        if (rt_write_char(1, '-') != 0) {
            return -1;
        }
        body_offset = 1U;
        length -= 1U;
        if (width > total_length && write_repeated_char('0', width - total_length) != 0) {
            return -1;
        }
        if (length > 0U && rt_write_all(1, text + body_offset, length) != 0) {
            return -1;
        }
        return 0;
    }

    return write_padded_chunk(text, length, width, left_align, pad);
}

static int shell_quote_argument(const char *text, char *buffer, size_t buffer_size) {
    size_t out = 0U;
    size_t i = 0U;

    if (buffer_size == 0U) {
        return -1;
    }
    if (append_char_to_buffer(buffer, buffer_size, &out, '\'') != 0) {
        return -1;
    }
    while (text != 0 && text[i] != '\0') {
        if (text[i] == '\'') {
            if (append_text_to_buffer(buffer, buffer_size, &out, "'\\''") != 0) {
                return -1;
            }
        } else if (append_char_to_buffer(buffer, buffer_size, &out, text[i]) != 0) {
            return -1;
        }
        i += 1U;
    }
    return append_char_to_buffer(buffer, buffer_size, &out, '\'');
}

static int parse_escape_sequence(const char *text, char *out_char, size_t *consumed_out, int *stop_output) {
    unsigned int value = 0U;
    size_t consumed = 1U;

    if (text[0] != '\\') {
        *out_char = text[0];
        *consumed_out = 1U;
        *stop_output = 0;
        return 0;
    }

    *stop_output = 0;
    if (text[1] == '\0') {
        *out_char = '\\';
        *consumed_out = 1U;
        return 0;
    }

    consumed = 2U;
    switch (text[1]) {
        case 'n':
            *out_char = '\n';
            break;
        case 't':
            *out_char = '\t';
            break;
        case 'r':
            *out_char = '\r';
            break;
        case '\\':
            *out_char = '\\';
            break;
        case 'a':
            *out_char = '\a';
            break;
        case 'b':
            *out_char = '\b';
            break;
        case 'f':
            *out_char = '\f';
            break;
        case 'v':
            *out_char = '\v';
            break;
        case 'c':
            *out_char = '\0';
            *stop_output = 1;
            break;
        case '0':
            value = 0U;
            while (consumed < 5U && text[consumed] >= '0' && text[consumed] <= '7') {
                value = (value * 8U) + (unsigned int)(text[consumed] - '0');
                consumed += 1U;
            }
            *out_char = (char)value;
            break;
        default:
            *out_char = text[1];
            break;
    }

    *consumed_out = consumed;
    return 0;
}

static int format_escaped_argument(const char *text, char *buffer, size_t buffer_size, int *stop_output) {
    size_t index = 0;
    size_t length = 0;

    if (buffer_size == 0U) {
        return -1;
    }

    *stop_output = 0;
    while (text[index] != '\0') {
        char ch;
        size_t consumed = 0U;

        if (parse_escape_sequence(text + index, &ch, &consumed, stop_output) != 0) {
            return -1;
        }
        if (*stop_output) {
            break;
        }
        if (length + 1U >= buffer_size) {
            return -1;
        }
        buffer[length++] = ch;
        index += consumed;
    }

    buffer[length] = '\0';
    return 0;
}

static int write_formatted_number(
    unsigned long long value,
    int negative,
    unsigned int base,
    int uppercase,
    int width,
    int left_align,
    char pad,
    int precision
) {
    char digits[128];
    size_t digit_length;
    int zero_padding = 0;
    int total_length;

    format_unsigned(value, base, uppercase, digits, sizeof(digits));
    if (precision == 0 && value == 0ULL) {
        digits[0] = '\0';
    }

    digit_length = text_length(digits);
    if (precision >= 0 && (size_t)precision > digit_length) {
        zero_padding = precision - (int)digit_length;
    }

    if (precision >= 0) {
        pad = ' ';
    }

    total_length = (int)digit_length + zero_padding + (negative ? 1 : 0);
    if (!left_align && width > total_length) {
        if (pad == '0' && negative) {
            if (rt_write_char(1, '-') != 0) {
                return -1;
            }
            negative = 0;
        }
        if (write_repeated_char(pad, width - total_length) != 0) {
            return -1;
        }
    }

    if (negative) {
        if (rt_write_char(1, '-') != 0) {
            return -1;
        }
    }
    if (zero_padding > 0 && write_repeated_char('0', zero_padding) != 0) {
        return -1;
    }
    if (digit_length > 0U && rt_write_all(1, digits, digit_length) != 0) {
        return -1;
    }

    if (left_align && width > total_length) {
        if (write_repeated_char(' ', width - total_length) != 0) {
            return -1;
        }
    }

    return 0;
}

static int format_fixed_double(double value, int precision, char *buffer, size_t buffer_size) {
    char whole_buffer[128];
    size_t length = 0;
    unsigned long long scale = 1ULL;
    unsigned long long whole;
    unsigned long long fraction;
    int negative = value < 0.0;
    int i;

    if (buffer_size == 0U) {
        return -1;
    }

    if (precision < 0) {
        precision = 6;
    }
    if (precision > 18) {
        precision = 18;
    }

    value = absolute_double(value);
    for (i = 0; i < precision; ++i) {
        scale *= 10ULL;
    }

    {
        double rounded = value * (double)scale + 0.5;
        unsigned long long scaled = (unsigned long long)rounded;
        whole = scale == 0ULL ? 0ULL : (scaled / scale);
        fraction = scale == 0ULL ? 0ULL : (scaled % scale);
    }

    buffer[0] = '\0';
    if (negative && append_char_to_buffer(buffer, buffer_size, &length, '-') != 0) {
        return -1;
    }

    format_unsigned(whole, 10U, 0, whole_buffer, sizeof(whole_buffer));
    if (append_text_to_buffer(buffer, buffer_size, &length, whole_buffer) != 0) {
        return -1;
    }

    if (precision > 0) {
        char fraction_buffer[32];
        int digit_index;

        if (append_char_to_buffer(buffer, buffer_size, &length, '.') != 0) {
            return -1;
        }

        for (digit_index = precision - 1; digit_index >= 0; --digit_index) {
            fraction_buffer[digit_index] = (char)('0' + (fraction % 10ULL));
            fraction /= 10ULL;
        }
        fraction_buffer[precision] = '\0';

        if (append_text_to_buffer(buffer, buffer_size, &length, fraction_buffer) != 0) {
            return -1;
        }
    }

    return 0;
}

static void trim_fractional_zeros(char *buffer) {
    size_t length = text_length(buffer);

    while (length > 0U && buffer[length - 1U] == '0') {
        buffer[length - 1U] = '\0';
        length -= 1U;
    }

    if (length > 0U && buffer[length - 1U] == '.') {
        buffer[length - 1U] = '\0';
    }
}

static int format_exponential_double(double value, int precision, int uppercase, char *buffer, size_t buffer_size) {
    int exponent = 0;
    double normalized;
    char mantissa[128];
    size_t length = 0;
    char exponent_digits[32];
    int exponent_negative = 0;
    unsigned long long exponent_value;

    if (precision < 0) {
        precision = 6;
    }

    if (value == 0.0) {
        if (format_fixed_double(0.0, precision, mantissa, sizeof(mantissa)) != 0) {
            return -1;
        }
    } else {
        normalized = absolute_double(value);
        while (normalized >= 10.0) {
            normalized /= 10.0;
            exponent += 1;
        }
        while (normalized > 0.0 && normalized < 1.0) {
            normalized *= 10.0;
            exponent -= 1;
        }

        if (value < 0.0) {
            normalized = -normalized;
        }

        if (format_fixed_double(normalized, precision, mantissa, sizeof(mantissa)) != 0) {
            return -1;
        }

        if (mantissa[0] == '1' && mantissa[1] == '0' && mantissa[2] == '.') {
            if (format_fixed_double((value < 0.0) ? -1.0 : 1.0, precision, mantissa, sizeof(mantissa)) != 0) {
                return -1;
            }
            exponent += 1;
        }
    }

    buffer[0] = '\0';
    if (append_text_to_buffer(buffer, buffer_size, &length, mantissa) != 0) {
        return -1;
    }
    if (append_char_to_buffer(buffer, buffer_size, &length, uppercase ? 'E' : 'e') != 0) {
        return -1;
    }
    if (append_char_to_buffer(buffer, buffer_size, &length, exponent < 0 ? '-' : '+') != 0) {
        return -1;
    }

    exponent_negative = exponent < 0;
    exponent_value = (unsigned long long)(exponent_negative ? -exponent : exponent);
    format_unsigned(exponent_value, 10U, 0, exponent_digits, sizeof(exponent_digits));
    if (exponent_value < 10ULL && append_char_to_buffer(buffer, buffer_size, &length, '0') != 0) {
        return -1;
    }
    if (append_text_to_buffer(buffer, buffer_size, &length, exponent_digits) != 0) {
        return -1;
    }

    return 0;
}

static int format_general_double(double value, int precision, int uppercase, char *buffer, size_t buffer_size) {
    double normalized = absolute_double(value);
    int exponent = 0;

    if (precision == 0) {
        precision = 1;
    } else if (precision < 0) {
        precision = 6;
    }

    if (normalized > 0.0) {
        while (normalized >= 10.0) {
            normalized /= 10.0;
            exponent += 1;
        }
        while (normalized < 1.0) {
            normalized *= 10.0;
            exponent -= 1;
        }
    }

    if (exponent < -4 || exponent >= precision) {
        if (format_exponential_double(value, precision - 1, uppercase, buffer, buffer_size) != 0) {
            return -1;
        }
        {
            char *marker = buffer;
            while (*marker != '\0' && *marker != 'e' && *marker != 'E') {
                marker += 1;
            }
            if (*marker == 'e' || *marker == 'E') {
                char suffix[32];
                size_t length = 0;
                rt_copy_string(suffix, sizeof(suffix), marker);
                *marker = '\0';
                trim_fractional_zeros(buffer);
                length = text_length(buffer);
                if (append_text_to_buffer(buffer, buffer_size, &length, suffix) != 0) {
                    return -1;
                }
            }
        }
    } else {
        int digits_after_decimal = precision - (exponent + 1);
        if (digits_after_decimal < 0) {
            digits_after_decimal = 0;
        }
        if (format_fixed_double(value, digits_after_decimal, buffer, buffer_size) != 0) {
            return -1;
        }
        trim_fractional_zeros(buffer);
    }

    return 0;
}

int main(int argc, char **argv) {
    const char *format;
    int arg_index = 2;
    int ran_once = 0;

    if (argc < 2) {
        rt_write_line(2, "Usage: printf FORMAT [ARG ...]");
        return 1;
    }

    format = argv[1];
    while (!ran_once || arg_index < argc) {
        size_t i = 0;
        int cycle_base = arg_index;
        int consumed_args = 0;
        unsigned long long max_position = 0ULL;

        while (format[i] != '\0') {
            if (format[i] == '\\') {
                char ch = '\0';
                size_t consumed = 0U;
                int stop_output = 0;

                if (parse_escape_sequence(format + i, &ch, &consumed, &stop_output) != 0) {
                    return 1;
                }
                if (stop_output) {
                    return 0;
                }
                if (rt_write_char(1, ch) != 0) {
                    return 1;
                }
                i += consumed;
                continue;
            }

            if (format[i] == '%') {
                int left_align = 0;
                char pad = ' ';
                int width = 0;
                int precision = -1;
                char spec;
                unsigned long long position = 0ULL;
                const char *arg = "";
                size_t spec_start;
                size_t scan;

                spec_start = i + 1U;
                scan = spec_start;
                if (format[spec_start] == '%') {
                    if (rt_write_char(1, '%') != 0) {
                        return 1;
                    }
                    i = spec_start + 1U;
                    continue;
                }

                while (ascii_is_digit(format[scan])) {
                    position = (position * 10ULL) + (unsigned long long)(format[scan] - '0');
                    scan += 1U;
                }
                if (scan > spec_start && format[scan] == '$' && position > 0ULL) {
                    i = scan + 1U;
                } else {
                    position = 0ULL;
                    i = spec_start;
                }

                while (format[i] == '-' || format[i] == '0') {
                    if (format[i] == '-') {
                        left_align = 1;
                        pad = ' ';
                    } else if (!left_align) {
                        pad = '0';
                    }
                    i += 1;
                }

                while (format[i] >= '0' && format[i] <= '9') {
                    width = (width * 10) + (int)(format[i] - '0');
                    i += 1;
                }

                if (format[i] == '.') {
                    precision = 0;
                    i += 1;
                    while (format[i] >= '0' && format[i] <= '9') {
                        precision = (precision * 10) + (int)(format[i] - '0');
                        i += 1;
                    }
                }

                spec = format[i];
                if (spec == '\0') {
                    break;
                }

                if (spec != '%') {
                    if (position > 0ULL) {
                        unsigned long long arg_offset = position - 1ULL;
                        if (position > max_position) {
                            max_position = position;
                        }
                        if ((unsigned long long)cycle_base + arg_offset < (unsigned long long)argc) {
                            arg = argv[cycle_base + (int)arg_offset];
                        }
                    } else {
                        arg = (arg_index < argc) ? argv[arg_index] : "";
                        if (arg_index < argc) {
                            arg_index += 1;
                            consumed_args += 1;
                        }
                    }
                }

                if (spec == 's') {
                    size_t length = text_length(arg);
                    if (precision >= 0 && (size_t)precision < length) {
                        length = (size_t)precision;
                    }
                    if (write_padded_chunk(arg, length, width, left_align, pad) != 0) {
                        return 1;
                    }
                } else if (spec == 'c') {
                    char ch = (arg[0] != '\0') ? arg[0] : '\0';
                    if (write_padded_chunk(&ch, 1U, width, left_align, pad) != 0) {
                        return 1;
                    }
                } else if (spec == 'b') {
                    char decoded[512];
                    size_t length;
                    int stop_output = 0;

                    if (format_escaped_argument(arg, decoded, sizeof(decoded), &stop_output) != 0) {
                        return 1;
                    }
                    length = text_length(decoded);
                    if (precision >= 0 && (size_t)precision < length) {
                        length = (size_t)precision;
                    }
                    if (write_padded_chunk(decoded, length, width, left_align, pad) != 0) {
                        return 1;
                    }
                    if (stop_output) {
                        return 0;
                    }
                } else if (spec == 'q') {
                    char quoted[512];
                    size_t length;

                    if (shell_quote_argument(arg, quoted, sizeof(quoted)) != 0) {
                        return 1;
                    }
                    length = text_length(quoted);
                    if (precision >= 0 && (size_t)precision < length) {
                        length = (size_t)precision;
                    }
                    if (write_padded_chunk(quoted, length, width, left_align, pad) != 0) {
                        return 1;
                    }
                } else if (spec == 'd' || spec == 'i') {
                    long long signed_value = 0;
                    unsigned long long abs_value;
                    int negative = 0;

                    parse_signed_value(arg, &signed_value);
                    if (signed_value < 0) {
                        negative = 1;
                        abs_value = (unsigned long long)(0ULL - (unsigned long long)signed_value);
                    } else {
                        abs_value = (unsigned long long)signed_value;
                    }

                    if (write_formatted_number(abs_value, negative, 10U, 0, width, left_align, pad, precision) != 0) {
                        return 1;
                    }
                } else if (spec == 'u') {
                    unsigned long long value = 0;
                    parse_unsigned_value(arg, &value);
                    if (write_formatted_number(value, 0, 10U, 0, width, left_align, pad, precision) != 0) {
                        return 1;
                    }
                } else if (spec == 'x') {
                    unsigned long long value = 0;
                    parse_unsigned_value(arg, &value);
                    if (write_formatted_number(value, 0, 16U, 0, width, left_align, pad, precision) != 0) {
                        return 1;
                    }
                } else if (spec == 'X') {
                    unsigned long long value = 0;
                    parse_unsigned_value(arg, &value);
                    if (write_formatted_number(value, 0, 16U, 1, width, left_align, pad, precision) != 0) {
                        return 1;
                    }
                } else if (spec == 'o') {
                    unsigned long long value = 0;
                    parse_unsigned_value(arg, &value);
                    if (write_formatted_number(value, 0, 8U, 0, width, left_align, pad, precision) != 0) {
                        return 1;
                    }
                } else if (spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' || spec == 'g' || spec == 'G') {
                    char float_buffer[256];
                    double value = 0.0;

                    parse_float_value(arg, &value);
                    if (spec == 'f' || spec == 'F') {
                        if (format_fixed_double(value, precision, float_buffer, sizeof(float_buffer)) != 0) {
                            return 1;
                        }
                    } else if (spec == 'e' || spec == 'E') {
                        if (format_exponential_double(value, precision, spec == 'E', float_buffer, sizeof(float_buffer)) != 0) {
                            return 1;
                        }
                    } else {
                        if (format_general_double(value, precision, spec == 'G', float_buffer, sizeof(float_buffer)) != 0) {
                            return 1;
                        }
                    }

                    if (write_padded_numeric_text(float_buffer, width, left_align, pad) != 0) {
                        return 1;
                    }
                } else {
                    if (rt_write_char(1, spec) != 0) {
                        return 1;
                    }
                }

                i += 1;
                continue;
            }

            if (rt_write_char(1, format[i]) != 0) {
                return 1;
            }
            i += 1;
        }

        if (max_position > (unsigned long long)consumed_args) {
            consumed_args = (int)max_position;
        }
        if (arg_index < cycle_base + consumed_args) {
            arg_index = cycle_base + consumed_args;
        }
        ran_once = 1;
        if (consumed_args == 0) {
            break;
        }
    }

    return 0;
}
