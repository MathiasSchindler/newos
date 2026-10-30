#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#include <limits.h>

int tool_parse_escaped_string(const char *text, char *buffer, size_t buffer_size, size_t *length_out) {
    size_t in_index = 0;
    size_t out_index = 0;

    if (text == 0 || buffer == 0 || buffer_size == 0) {
        return -1;
    }

    while (text[in_index] != '\0') {
        char ch = text[in_index];

        if (ch == '\\' && text[in_index + 1U] != '\0') {
            in_index += 1U;
            ch = text[in_index];
            if (ch == 'n') {
                ch = '\n';
            } else if (ch == 'r') {
                ch = '\r';
            } else if (ch == 't') {
                ch = '\t';
            } else if (ch == 'f') {
                ch = '\f';
            } else if (ch == 'v') {
                ch = '\v';
            } else if (ch == '0') {
                ch = '\0';
            }
        }

        if (out_index + 1U >= buffer_size) {
            buffer[buffer_size - 1U] = '\0';
            return -1;
        }

        buffer[out_index++] = ch;
        in_index += 1U;
    }

    buffer[out_index] = '\0';
    if (length_out != 0) {
        *length_out = out_index;
    }
    return 0;
}

int tool_parse_uint_arg(const char *text, unsigned long long *value_out, const char *tool_name, const char *what) {
    if (text == 0 || rt_parse_uint(text, value_out) != 0) {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }
    return 0;
}

int tool_parse_int_arg(const char *text, long long *value_out, const char *tool_name, const char *what) {
    unsigned long long magnitude = 0;
    int negative = 0;

    if (text == 0 || value_out == 0 || text[0] == '\0') {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }

    if (text[0] == '-') {
        negative = 1;
        text += 1;
    } else if (text[0] == '+') {
        text += 1;
    }

    if (text[0] == '\0' || rt_parse_uint(text, &magnitude) != 0) {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }

    *value_out = negative ? -(long long)magnitude : (long long)magnitude;
    return 0;
}

int tool_parse_size_value(const char *text, unsigned long long *value_out) {
    char digits[32];
    size_t len = 0U;
    unsigned long long value;
    unsigned long long multiplier = 1ULL;
    char suffix;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }
    while (text[len] >= '0' && text[len] <= '9') {
        if (len + 1U >= sizeof(digits)) {
            return -1;
        }
        digits[len] = text[len];
        len += 1U;
    }
    if (len == 0U) {
        return -1;
    }
    digits[len] = '\0';
    if (rt_parse_uint(digits, &value) != 0) {
        return -1;
    }
    suffix = text[len];
    if (suffix != '\0') {
        if (text[len + 1U] != '\0') {
            return -1;
        }
        if (suffix == 'k' || suffix == 'K') {
            multiplier = 1024ULL;
        } else if (suffix == 'm' || suffix == 'M') {
            multiplier = 1024ULL * 1024ULL;
        } else if (suffix == 'g' || suffix == 'G') {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        } else {
            return -1;
        }
    }
    if (value > ULLONG_MAX / multiplier) {
        return -1;
    }
    *value_out = value * multiplier;
    return 0;
}

int tool_parse_unsigned_auto(const char *text, unsigned long long *value_out) {
    unsigned long long value = 0ULL;
    size_t index = 0U;
    int is_hex = 0;
    int saw_digit = 0;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        is_hex = 1;
        index = 2U;
    }
    for (; text[index] != '\0'; ++index) {
        int digit;

        if (is_hex) {
            digit = tool_hex_value(text[index]);
            if (digit < 0) {
                return -1;
            }
            value = (value << 4U) | (unsigned long long)digit;
        } else {
            if (text[index] < '0' || text[index] > '9') {
                return -1;
            }
            value = (value * 10ULL) + (unsigned long long)(text[index] - '0');
        }
        saw_digit = 1;
    }
    if (!saw_digit) {
        return -1;
    }
    *value_out = value;
    return 0;
}

int tool_parse_address_token(const char *text, unsigned long long *value_out) {
    size_t index;
    int has_hex_letter = 0;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        return tool_parse_unsigned_auto(text, value_out);
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        int digit = tool_hex_value(text[index]);

        if (digit < 0) {
            return -1;
        }
        if ((text[index] >= 'a' && text[index] <= 'f') || (text[index] >= 'A' && text[index] <= 'F')) {
            has_hex_letter = 1;
        }
    }
    if (!has_hex_letter && rt_strlen(text) < 9U) {
        return tool_parse_unsigned_auto(text, value_out);
    }
    *value_out = 0ULL;
    for (index = 0U; text[index] != '\0'; ++index) {
        int digit = tool_hex_value(text[index]);
        if (digit < 0) {
            return -1;
        }
        *value_out = (*value_out << 4U) | (unsigned long long)digit;
    }
    return 0;
}

int tool_parse_fixed_digits(const char *text, size_t start, size_t digits, unsigned int *value_out) {
    unsigned long long value = 0ULL;
    size_t i;

    if (text == 0 || value_out == 0) {
        return -1;
    }
    for (i = 0U; i < digits; ++i) {
        char ch = text[start + i];
        if (ch < '0' || ch > '9') {
            return -1;
        }
        value = value * 10ULL + (unsigned long long)(ch - '0');
    }
    *value_out = (unsigned int)value;
    return 0;
}

unsigned long long tool_parse_decimal_field(const char *field, size_t field_size) {
    unsigned long long value = 0ULL;
    size_t i = 0U;

    while (i < field_size && (field[i] == ' ' || field[i] == '\0')) i += 1U;
    while (i < field_size && field[i] >= '0' && field[i] <= '9') {
        value = (value * 10ULL) + (unsigned long long)(field[i] - '0');
        i += 1U;
    }
    return value;
}
