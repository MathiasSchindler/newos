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

static void format_unsigned(unsigned long long value, unsigned int base, int uppercase, char *buffer, size_t buffer_size) {
    char digits[32];
    char scratch[64];
    size_t length = 0;
    size_t i;

    if (uppercase) {
        rt_copy_string(digits, sizeof(digits), "0123456789ABCDEF");
    } else {
        rt_copy_string(digits, sizeof(digits), "0123456789abcdef");
    }

    if (buffer_size == 0) {
        return;
    }

    if (value == 0ULL) {
        if (buffer_size > 1) {
            buffer[0] = '0';
            buffer[1] = '\0';
        } else {
            buffer[0] = '\0';
        }
        return;
    }

    while (value != 0ULL && length + 1 < sizeof(scratch)) {
        scratch[length++] = digits[value % base];
        value /= base;
    }

    for (i = 0; i < length && i + 1 < buffer_size; ++i) {
        buffer[i] = scratch[length - 1 - i];
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

static int write_padded_text(const char *text, int width, int left_align, char pad) {
    int length = 0;
    const char *cursor = text;

    while (*cursor != '\0') {
        length += 1;
        cursor += 1;
    }

    if (!left_align && width > length) {
        if (write_repeated_char(pad, width - length) != 0) {
            return -1;
        }
    }

    if (rt_write_cstr(1, text) != 0) {
        return -1;
    }

    if (left_align && width > length) {
        if (write_repeated_char(' ', width - length) != 0) {
            return -1;
        }
    }

    return 0;
}

static int write_escape(char ch) {
    if (ch == 'n') return rt_write_char(1, '\n');
    if (ch == 't') return rt_write_char(1, '\t');
    if (ch == 'r') return rt_write_char(1, '\r');
    if (ch == '\\') return rt_write_char(1, '\\');
    if (ch == 'a') return rt_write_char(1, '\a');
    if (ch == 'b') return rt_write_char(1, '\b');
    if (ch == 'f') return rt_write_char(1, '\f');
    if (ch == 'v') return rt_write_char(1, '\v');
    return rt_write_char(1, ch);
}

int main(int argc, char **argv) {
    const char *format;
    int arg_index = 2;
    size_t i = 0;

    if (argc < 2) {
        rt_write_line(2, "Usage: printf FORMAT [ARG ...]");
        return 1;
    }

    format = argv[1];
    while (format[i] != '\0') {
        if (format[i] == '\\' && format[i + 1] != '\0') {
            if (write_escape(format[i + 1]) != 0) {
                return 1;
            }
            i += 2;
            continue;
        }

        if (format[i] == '%') {
            int left_align = 0;
            char pad = ' ';
            int width = 0;
            char spec;
            char text_buffer[128];
            const char *arg = (arg_index < argc) ? argv[arg_index++] : "";

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

            spec = format[i];
            if (spec == '\0') {
                break;
            }

            text_buffer[0] = '\0';
            if (spec == 's') {
                rt_copy_string(text_buffer, sizeof(text_buffer), arg);
            } else if (spec == 'c') {
                text_buffer[0] = (arg[0] != '\0') ? arg[0] : '\0';
                text_buffer[1] = '\0';
            } else if (spec == 'd' || spec == 'i') {
                long long value = 0;
                parse_signed_value(arg, &value);
                if (value < 0) {
                    unsigned long long abs_value = (unsigned long long)(-value);
                    char digits[96];
                    format_unsigned(abs_value, 10U, 0, digits, sizeof(digits));
                    text_buffer[0] = '-';
                    rt_copy_string(text_buffer + 1, sizeof(text_buffer) - 1, digits);
                } else {
                    format_unsigned((unsigned long long)value, 10U, 0, text_buffer, sizeof(text_buffer));
                }
            } else if (spec == 'u') {
                unsigned long long value = 0;
                parse_unsigned_value(arg, &value);
                format_unsigned(value, 10U, 0, text_buffer, sizeof(text_buffer));
            } else if (spec == 'x') {
                unsigned long long value = 0;
                parse_unsigned_value(arg, &value);
                format_unsigned(value, 16U, 0, text_buffer, sizeof(text_buffer));
            } else if (spec == 'X') {
                unsigned long long value = 0;
                parse_unsigned_value(arg, &value);
                format_unsigned(value, 16U, 1, text_buffer, sizeof(text_buffer));
            } else if (spec == 'o') {
                unsigned long long value = 0;
                parse_unsigned_value(arg, &value);
                format_unsigned(value, 8U, 0, text_buffer, sizeof(text_buffer));
            } else {
                if (rt_write_char(1, spec) != 0) {
                    return 1;
                }
                i += 1;
                continue;
            }

            if (write_padded_text(text_buffer, width, left_align, pad) != 0) {
                return 1;
            }

            i += 1;
            continue;
        }

        if (rt_write_char(1, format[i]) != 0) {
            return 1;
        }
        i += 1;
    }

    return 0;
}
