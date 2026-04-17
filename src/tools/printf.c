#include "runtime.h"

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
        int consumed_args = 0;

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
                const char *arg = (arg_index < argc) ? argv[arg_index] : "";

                i += 1;
                if (format[i] == '%') {
                    if (rt_write_char(1, '%') != 0) {
                        return 1;
                    }
                    i += 1;
                    continue;
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
                    if (arg_index < argc) {
                        arg_index += 1;
                        consumed_args += 1;
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

        ran_once = 1;
        if (consumed_args == 0) {
            break;
        }
    }

    return 0;
}
