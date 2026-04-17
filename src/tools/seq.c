#include "runtime.h"
#include "tool_util.h"

typedef struct {
    long long value;
    unsigned int scale;
} DecimalValue;

typedef struct {
    int enabled;
    int precision;
    int min_width;
    int trim_trailing;
    char prefix[64];
    char suffix[64];
} FormatSpec;

static unsigned long long decimal_pow10(unsigned int scale) {
    unsigned long long result = 1ULL;
    while (scale > 0U) {
        result *= 10ULL;
        scale -= 1U;
    }
    return result;
}

static int parse_decimal_value(const char *text, DecimalValue *value_out) {
    unsigned long long magnitude = 0ULL;
    int negative = 0;
    int seen_digit = 0;
    int seen_point = 0;
    unsigned int scale = 0U;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }

    if (*text == '-' || *text == '+') {
        negative = (*text == '-') ? 1 : 0;
        text += 1;
    }

    while (*text != '\0') {
        if (*text >= '0' && *text <= '9') {
            magnitude = (magnitude * 10ULL) + (unsigned long long)(*text - '0');
            if (seen_point) {
                scale += 1U;
            }
            seen_digit = 1;
        } else if (*text == '.' && !seen_point) {
            seen_point = 1;
        } else {
            return -1;
        }
        text += 1;
    }

    if (!seen_digit) {
        return -1;
    }

    value_out->value = negative ? -(long long)magnitude : (long long)magnitude;
    value_out->scale = scale;
    return 0;
}

static long long rescale_value(const DecimalValue *value, unsigned int target_scale) {
    long long result = value->value;
    unsigned int scale = value->scale;

    while (scale < target_scale) {
        result *= 10LL;
        scale += 1U;
    }

    return result;
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
    unsigned long long width_value = 0ULL;
    unsigned long long precision_value = 0ULL;

    if (text == 0 || spec == 0) {
        return -1;
    }

    rt_memset(spec, 0, sizeof(*spec));
    spec->enabled = 1;
    spec->precision = -1;

    if (copy_format_literal(spec->prefix, sizeof(spec->prefix), text, &index) != 0 || text[index] != '%') {
        return -1;
    }
    index += 1U;

    while (text[index] >= '0' && text[index] <= '9') {
        width_value = (width_value * 10ULL) + (unsigned long long)(text[index] - '0');
        index += 1U;
    }

    if (text[index] == '.') {
        index += 1U;
        if (text[index] < '0' || text[index] > '9') {
            return -1;
        }
        while (text[index] >= '0' && text[index] <= '9') {
            precision_value = (precision_value * 10ULL) + (unsigned long long)(text[index] - '0');
            index += 1U;
        }
        spec->precision = (int)precision_value;
    }

    if (text[index] == 'f') {
        spec->trim_trailing = 0;
    } else if (text[index] == 'g') {
        spec->trim_trailing = 1;
    } else {
        return -1;
    }
    spec->min_width = (int)width_value;
    index += 1U;

    if (copy_format_literal(spec->suffix, sizeof(spec->suffix), text, &index) != 0 || text[index] != '\0') {
        return -1;
    }

    return 0;
}

static int format_decimal_text(long long value, unsigned int scale, int precision, int trim_trailing, char *buffer, size_t buffer_size) {
    unsigned long long magnitude;
    unsigned long long whole_part;
    unsigned long long fractional_part = 0ULL;
    unsigned long long scale_factor = 1ULL;
    char whole_text[64];
    char fraction_text[64];
    size_t length = 0U;
    unsigned int active_scale;
    int negative = value < 0;
    unsigned int i;

    if (buffer == 0 || buffer_size == 0U) {
        return -1;
    }

    if (precision < 0) {
        precision = (int)scale;
    }

    magnitude = negative ? (unsigned long long)(-(value + 1LL)) + 1ULL : (unsigned long long)value;
    if ((unsigned int)precision < scale) {
        unsigned int trim_scale = scale - (unsigned int)precision;
        unsigned long long divisor = decimal_pow10(trim_scale);
        unsigned long long remainder = magnitude % divisor;
        magnitude /= divisor;
        if (remainder * 2ULL >= divisor) {
            magnitude += 1ULL;
        }
        scale = (unsigned int)precision;
    } else {
        while (scale < (unsigned int)precision) {
            magnitude *= 10ULL;
            scale += 1U;
        }
    }

    active_scale = scale;
    if (trim_trailing) {
        while (active_scale > 0U && (magnitude % 10ULL) == 0ULL) {
            magnitude /= 10ULL;
            active_scale -= 1U;
        }
    }
    if (magnitude == 0ULL) {
        negative = 0;
    }

    if (active_scale > 0U) {
        scale_factor = decimal_pow10(active_scale);
        whole_part = magnitude / scale_factor;
        fractional_part = magnitude % scale_factor;
    } else {
        whole_part = magnitude;
    }

    buffer[0] = '\0';
    if (negative) {
        if (length + 1U >= buffer_size) {
            return -1;
        }
        buffer[length++] = '-';
    }

    rt_unsigned_to_string(whole_part, whole_text, sizeof(whole_text));
    for (i = 0U; whole_text[i] != '\0'; ++i) {
        if (length + 1U >= buffer_size) {
            return -1;
        }
        buffer[length++] = whole_text[i];
    }

    if (active_scale > 0U) {
        if (length + 1U >= buffer_size) {
            return -1;
        }
        buffer[length++] = '.';
        for (i = 0U; i < active_scale; ++i) {
            fraction_text[active_scale - i - 1U] = (char)('0' + (fractional_part % 10ULL));
            fractional_part /= 10ULL;
        }
        for (i = 0U; i < active_scale; ++i) {
            if (length + 1U >= buffer_size) {
                return -1;
            }
            buffer[length++] = fraction_text[i];
        }
    }

    buffer[length] = '\0';
    return 0;
}

static int write_zero_padded_text(const char *text, int width) {
    char buffer[128];
    int prefix = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    int length = (int)rt_strlen(text);
    int padding = width - length;
    int out = 0;
    int i;

    if (padding < 0) {
        padding = 0;
    }

    if (prefix > 0) {
        buffer[out++] = text[0];
    }
    for (i = 0; i < padding && out < (int)sizeof(buffer) - 1; ++i) {
        buffer[out++] = '0';
    }
    for (i = prefix; text[i] != '\0' && out < (int)sizeof(buffer) - 1; ++i) {
        buffer[out++] = text[i];
    }
    buffer[out] = '\0';
    return rt_write_cstr(1, buffer);
}

static int write_formatted_value(long long value, unsigned int scale, int default_precision, int equal_width, int width, const FormatSpec *format) {
    char number_text[128];
    int precision = default_precision;
    int trim_trailing = 0;
    int padding;

    if (format->enabled && format->precision >= 0) {
        precision = format->precision;
    }
    if (format->enabled) {
        trim_trailing = format->trim_trailing;
    }

    if (format_decimal_text(value, scale, precision, trim_trailing, number_text, sizeof(number_text)) != 0) {
        return -1;
    }

    if (format->enabled) {
        if (rt_write_cstr(1, format->prefix) != 0) {
            return -1;
        }
        padding = format->min_width - (int)rt_strlen(number_text);
        while (padding > 0) {
            if (rt_write_char(1, ' ') != 0) {
                return -1;
            }
            padding -= 1;
        }
        if (rt_write_cstr(1, number_text) != 0 || rt_write_cstr(1, format->suffix) != 0) {
            return -1;
        }
        return 0;
    }

    if (equal_width) {
        return write_zero_padded_text(number_text, width);
    }
    return rt_write_cstr(1, number_text);
}

static int should_continue(long long current, long long last, long long step) {
    return (step > 0) ? (current <= last) : (current >= last);
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-w] [-s SEP] [-f FORMAT] [FIRST [STEP]] LAST");
}

int main(int argc, char **argv) {
    const char *separator = "\n";
    int equal_width = 0;
    int argi = 1;
    DecimalValue parsed_first = { 1LL, 0U };
    DecimalValue parsed_step = { 1LL, 0U };
    DecimalValue parsed_last = { 0LL, 0U };
    FormatSpec format = { 0, -1, 0, 0, "", "" };
    unsigned int scale = 0U;
    int number_count;
    int implicit_step = 0;
    long long current;
    long long first;
    long long step;
    long long last;
    int wrote_any = 0;
    int width = 0;
    int default_precision;
    char width_text[128];

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
    } else {
        if (parse_decimal_value(argv[argi], &parsed_first) != 0 ||
            parse_decimal_value(argv[argi + 1], &parsed_step) != 0 ||
            parse_decimal_value(argv[argi + 2], &parsed_last) != 0) {
            tool_write_error("seq", "invalid number", 0);
            return 1;
        }
    }

    scale = parsed_first.scale;
    if (parsed_step.scale > scale) {
        scale = parsed_step.scale;
    }
    if (parsed_last.scale > scale) {
        scale = parsed_last.scale;
    }

    first = rescale_value(&parsed_first, scale);
    last = rescale_value(&parsed_last, scale);
    step = implicit_step ? ((last >= first) ? (long long)decimal_pow10(scale) : -(long long)decimal_pow10(scale))
                         : rescale_value(&parsed_step, scale);

    if (step == 0) {
        tool_write_error("seq", "step must not be zero", 0);
        return 1;
    }

    default_precision = (int)scale;
    if (equal_width) {
        if (format_decimal_text(first, scale, default_precision, 0, width_text, sizeof(width_text)) != 0) {
            return 1;
        }
        width = (int)rt_strlen(width_text);
        if (format_decimal_text(last, scale, default_precision, 0, width_text, sizeof(width_text)) != 0) {
            return 1;
        }
        if ((int)rt_strlen(width_text) > width) {
            width = (int)rt_strlen(width_text);
        }
    }

    current = first;
    while (should_continue(current, last, step)) {
        long long next = current + step;

        if (wrote_any) {
            if (rt_write_cstr(1, separator) != 0) {
                return 1;
            }
        }

        if (write_formatted_value(current, scale, default_precision, equal_width, width, &format) != 0) {
            return 1;
        }

        wrote_any = 1;
        if (current == last) {
            break;
        }

        if ((step > 0 && next < current) || (step < 0 && next > current)) {
            break;
        }
        current = next;
    }

    if (wrote_any && rt_write_char(1, '\n') != 0) {
        return 1;
    }

    return 0;
}
