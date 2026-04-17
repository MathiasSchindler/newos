#include "runtime.h"
#include "tool_util.h"

static int parse_signed_value(const char *text, long long *value_out) {
    long long value = 0;
    long long sign = 1;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }

    if (*text == '-') {
        sign = -1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }

    if (*text == '\0') {
        return -1;
    }

    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return -1;
        }
        value = (value * 10) + (long long)(*text - '0');
        text += 1;
    }

    *value_out = value * sign;
    return 0;
}

static int digit_width(long long value) {
    int width = 0;
    unsigned long long magnitude;

    if (value < 0) {
        width += 1;
        magnitude = (unsigned long long)(-(value + 1)) + 1ULL;
    } else {
        magnitude = (unsigned long long)value;
    }

    do {
        width += 1;
        magnitude /= 10ULL;
    } while (magnitude != 0ULL);

    return width;
}

static int write_zero_padded(long long value, int width) {
    char digits[64];
    char buffer[64];
    unsigned long long magnitude;
    int digit_count = 0;
    int prefix = 0;
    int padding;
    int i;

    if (value < 0) {
        buffer[prefix++] = '-';
        magnitude = (unsigned long long)(-(value + 1)) + 1ULL;
    } else {
        magnitude = (unsigned long long)value;
    }

    do {
        digits[digit_count++] = (char)('0' + (magnitude % 10ULL));
        magnitude /= 10ULL;
    } while (magnitude != 0ULL && digit_count < (int)sizeof(digits));

    padding = width - prefix - digit_count;
    if (padding < 0) {
        padding = 0;
    }

    for (i = 0; i < padding && prefix < (int)sizeof(buffer) - 1; ++i) {
        buffer[prefix++] = '0';
    }

    for (i = digit_count - 1; i >= 0 && prefix < (int)sizeof(buffer) - 1; --i) {
        buffer[prefix++] = digits[i];
    }

    buffer[prefix] = '\0';
    return rt_write_cstr(1, buffer);
}

static int should_continue(long long current, long long last, long long step) {
    return (step > 0) ? (current <= last) : (current >= last);
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-w] [-s SEP] [FIRST [STEP]] LAST");
}

int main(int argc, char **argv) {
    const char *separator = "\n";
    int equal_width = 0;
    int argi = 1;
    long long first = 1;
    long long step = 1;
    long long last = 0;
    int number_count;
    long long current;
    int wrote_any = 0;
    int width = 0;

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
        break;
    }

    number_count = argc - argi;
    if (number_count < 1 || number_count > 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (number_count == 1) {
        if (parse_signed_value(argv[argi], &last) != 0) {
            tool_write_error("seq", "invalid number: ", argv[argi]);
            return 1;
        }
        step = (last >= first) ? 1 : -1;
    } else if (number_count == 2) {
        if (parse_signed_value(argv[argi], &first) != 0 || parse_signed_value(argv[argi + 1], &last) != 0) {
            tool_write_error("seq", "invalid number", 0);
            return 1;
        }
        step = (last >= first) ? 1 : -1;
    } else {
        if (parse_signed_value(argv[argi], &first) != 0 ||
            parse_signed_value(argv[argi + 1], &step) != 0 ||
            parse_signed_value(argv[argi + 2], &last) != 0) {
            tool_write_error("seq", "invalid number", 0);
            return 1;
        }
    }

    if (step == 0) {
        tool_write_error("seq", "step must not be zero", 0);
        return 1;
    }

    if (equal_width) {
        int first_width = digit_width(first);
        int last_width = digit_width(last);
        width = (first_width > last_width) ? first_width : last_width;
    }

    current = first;
    while (should_continue(current, last, step)) {
        long long next = current + step;

        if (wrote_any) {
            if (rt_write_cstr(1, separator) != 0) {
                return 1;
            }
        }

        if (equal_width) {
            if (write_zero_padded(current, width) != 0) {
                return 1;
            }
        } else if (rt_write_int(1, current) != 0) {
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
