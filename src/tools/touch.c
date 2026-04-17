#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int update_access;
    int update_modify;
    int no_create;
    int explicit_time;
    long long atime;
    long long mtime;
} TouchOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-acm] [-d DATETIME | -t STAMP | -r FILE] path ...");
}

static int parse_fixed_digits(const char *text, size_t start, size_t digits, unsigned int *value_out) {
    unsigned long long value = 0;
    size_t i;

    if (text == 0 || value_out == 0) {
        return -1;
    }

    for (i = 0; i < digits; ++i) {
        char ch = text[start + i];
        if (ch < '0' || ch > '9') {
            return -1;
        }
        value = (value * 10ULL) + (unsigned long long)(ch - '0');
    }

    *value_out = (unsigned int)value;
    return 0;
}

static int is_leap_year(int year) {
    if ((year % 400) == 0) return 1;
    if ((year % 100) == 0) return 0;
    return (year % 4) == 0;
}

static int days_in_month(int year, unsigned int month) {
    static const unsigned char days[] = { 31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U };
    if (month == 0U || month > 12U) {
        return 0;
    }
    if (month == 2U && is_leap_year(year)) {
        return 29;
    }
    return (int)days[month - 1U];
}

static long long days_from_civil(int year, unsigned int month, unsigned int day) {
    int adjusted_year = year - (month <= 2U ? 1 : 0);
    int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    unsigned int year_of_era = (unsigned int)(adjusted_year - (era * 400));
    unsigned int shifted_month = month + (month > 2U ? (unsigned int)-3 : 9U);
    unsigned int day_of_year = ((153U * shifted_month) + 2U) / 5U + day - 1U;
    unsigned int day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return (long long)era * 146097LL + (long long)day_of_era - 719468LL;
}

static void civil_from_days(long long days, int *year_out, unsigned int *month_out, unsigned int *day_out) {
    long long z = days + 719468LL;
    long long era = (z >= 0 ? z : z - 146096LL) / 146097LL;
    unsigned int day_of_era = (unsigned int)(z - era * 146097LL);
    unsigned int year_of_era = (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    int year = (int)year_of_era + (int)(era * 400LL);
    unsigned int day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    unsigned int mp = (5U * day_of_year + 2U) / 153U;
    unsigned int day = day_of_year - (153U * mp + 2U) / 5U + 1U;
    unsigned int month = mp + (mp < 10U ? 3U : (unsigned int)-9);

    year += (month <= 2U) ? 1 : 0;
    *year_out = year;
    *month_out = month;
    *day_out = day;
}

static int build_epoch_timestamp(int year, unsigned int month, unsigned int day, unsigned int hour, unsigned int minute, unsigned int second, long long *epoch_out) {
    long long days;

    if (epoch_out == 0 || month < 1U || month > 12U || day < 1U || day > (unsigned int)days_in_month(year, month) ||
        hour > 23U || minute > 59U || second > 59U) {
        return -1;
    }

    days = days_from_civil(year, month, day);
    *epoch_out = days * 86400LL + (long long)hour * 3600LL + (long long)minute * 60LL + (long long)second;
    return 0;
}

static int parse_datetime_text(const char *text, long long *epoch_out) {
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour = 0U;
    unsigned int minute = 0U;
    unsigned int second = 0U;
    size_t len;
    long long explicit_epoch;

    if (text == 0 || epoch_out == 0 || text[0] == '\0') {
        return -1;
    }

    if (text[0] == '@') {
        if (tool_parse_int_arg(text + 1, &explicit_epoch, "touch", "epoch time") != 0) {
            return -1;
        }
        *epoch_out = explicit_epoch;
        return 0;
    }

    len = rt_strlen(text);
    if (len < 10U ||
        parse_fixed_digits(text, 0U, 4U, &year) != 0 ||
        text[4] != '-' ||
        parse_fixed_digits(text, 5U, 2U, &month) != 0 ||
        text[7] != '-' ||
        parse_fixed_digits(text, 8U, 2U, &day) != 0) {
        return -1;
    }

    if (len > 10U) {
        size_t offset = 10U;
        if (text[offset] != ' ' && text[offset] != 'T') {
            return -1;
        }
        offset += 1U;
        if (len < offset + 5U ||
            parse_fixed_digits(text, offset, 2U, &hour) != 0 ||
            text[offset + 2U] != ':' ||
            parse_fixed_digits(text, offset + 3U, 2U, &minute) != 0) {
            return -1;
        }
        offset += 5U;
        if (text[offset] == ':') {
            offset += 1U;
            if (parse_fixed_digits(text, offset, 2U, &second) != 0) {
                return -1;
            }
            offset += 2U;
        }
        if (text[offset] == 'Z') {
            offset += 1U;
        }
        if (text[offset] != '\0') {
            return -1;
        }
    }

    return build_epoch_timestamp((int)year, month, day, hour, minute, second, epoch_out);
}

static int current_year(void) {
    long long now = platform_get_epoch_time();
    long long days = now / 86400LL;
    int year = 1970;
    unsigned int month = 1U;
    unsigned int day = 1U;

    if (now < 0) {
        return 1970;
    }

    civil_from_days(days, &year, &month, &day);
    return year;
}

static int parse_touch_stamp(const char *text, long long *epoch_out) {
    char digits[16];
    size_t input_index = 0;
    size_t digit_count = 0;
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second = 0U;

    if (text == 0 || epoch_out == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[input_index] != '\0' && text[input_index] != '.') {
        if (digit_count + 1U >= sizeof(digits) || text[input_index] < '0' || text[input_index] > '9') {
            return -1;
        }
        digits[digit_count++] = text[input_index++];
    }
    digits[digit_count] = '\0';

    if (text[input_index] == '.') {
        if (parse_fixed_digits(text, input_index + 1U, 2U, &second) != 0 || text[input_index + 3U] != '\0') {
            return -1;
        }
    }

    if (digit_count == 12U) {
        if (parse_fixed_digits(digits, 0U, 4U, &year) != 0 ||
            parse_fixed_digits(digits, 4U, 2U, &month) != 0 ||
            parse_fixed_digits(digits, 6U, 2U, &day) != 0 ||
            parse_fixed_digits(digits, 8U, 2U, &hour) != 0 ||
            parse_fixed_digits(digits, 10U, 2U, &minute) != 0) {
            return -1;
        }
    } else if (digit_count == 10U) {
        unsigned int short_year;
        if (parse_fixed_digits(digits, 0U, 2U, &short_year) != 0 ||
            parse_fixed_digits(digits, 2U, 2U, &month) != 0 ||
            parse_fixed_digits(digits, 4U, 2U, &day) != 0 ||
            parse_fixed_digits(digits, 6U, 2U, &hour) != 0 ||
            parse_fixed_digits(digits, 8U, 2U, &minute) != 0) {
            return -1;
        }
        year = (short_year >= 69U) ? (1900U + short_year) : (2000U + short_year);
    } else if (digit_count == 8U) {
        year = (unsigned int)current_year();
        if (parse_fixed_digits(digits, 0U, 2U, &month) != 0 ||
            parse_fixed_digits(digits, 2U, 2U, &day) != 0 ||
            parse_fixed_digits(digits, 4U, 2U, &hour) != 0 ||
            parse_fixed_digits(digits, 6U, 2U, &minute) != 0) {
            return -1;
        }
    } else {
        return -1;
    }

    return build_epoch_timestamp((int)year, month, day, hour, minute, second, epoch_out);
}

static int load_reference_times(const char *path, TouchOptions *options) {
    PlatformDirEntry entry;
    char resolved[1024];
    const char *lookup_path = path;

    if (tool_canonicalize_path(path, 1, 0, resolved, sizeof(resolved)) == 0) {
        lookup_path = resolved;
    }

    if (platform_get_path_info(lookup_path, &entry) != 0) {
        return -1;
    }

    options->explicit_time = 1;
    options->atime = entry.atime;
    options->mtime = entry.mtime;
    return 0;
}

int main(int argc, char **argv) {
    TouchOptions options;
    PlatformDirEntry existing_entry;
    int exit_code = 0;
    int argi = 1;
    int i;
    long long timestamp = 0;

    rt_memset(&options, 0, sizeof(options));

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "--no-create") == 0) {
            options.no_create = 1;
            argi += 1;
            continue;
        }

        while (*flag != '\0') {
            char option = *flag;

            if (option == 'a') {
                options.update_access = 1;
                flag += 1;
            } else if (option == 'm') {
                options.update_modify = 1;
                flag += 1;
            } else if (option == 'c') {
                options.no_create = 1;
                flag += 1;
            } else if (option == 'd' || option == 't' || option == 'r') {
                const char *value_text = 0;

                if (flag[1] != '\0') {
                    value_text = flag + 1;
                    flag += rt_strlen(flag);
                } else {
                    if (argi + 1 >= argc) {
                        print_usage(argv[0]);
                        return 1;
                    }
                    value_text = argv[argi + 1];
                    argi += 1;
                    flag += 1;
                }

                if (option == 'd') {
                    if (parse_datetime_text(value_text, &timestamp) != 0) {
                        tool_write_error("touch", "invalid date ", value_text);
                        return 1;
                    }
                    options.explicit_time = 1;
                    options.atime = timestamp;
                    options.mtime = timestamp;
                } else if (option == 't') {
                    if (parse_touch_stamp(value_text, &timestamp) != 0) {
                        tool_write_error("touch", "invalid timestamp ", value_text);
                        return 1;
                    }
                    options.explicit_time = 1;
                    options.atime = timestamp;
                    options.mtime = timestamp;
                } else {
                    if (load_reference_times(value_text, &options) != 0) {
                        tool_write_error("touch", "cannot read reference ", value_text);
                        return 1;
                    }
                }
            } else {
                print_usage(argv[0]);
                return 1;
            }
        }

        argi += 1;
    }

    if (!options.update_access && !options.update_modify) {
        options.update_access = 1;
        options.update_modify = 1;
    }

    if (argi >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        long long desired_atime = options.atime;
        long long desired_mtime = options.mtime;
        int have_existing_times = (platform_get_path_info(argv[i], &existing_entry) == 0);

        if (!options.explicit_time) {
            timestamp = platform_get_epoch_time();
            if (timestamp < 0) {
                tool_write_error("touch", "cannot get current time for ", argv[i]);
                exit_code = 1;
                continue;
            }
            desired_atime = timestamp;
            desired_mtime = timestamp;
        }

        if (options.no_create && !tool_path_exists(argv[i])) {
            continue;
        }

        if (have_existing_times) {
            if (!options.update_access) {
                desired_atime = existing_entry.atime;
            } else if (!options.explicit_time && desired_atime <= existing_entry.atime) {
                desired_atime = existing_entry.atime + 1LL;
            }

            if (!options.update_modify) {
                desired_mtime = existing_entry.mtime;
            } else if (!options.explicit_time && desired_mtime <= existing_entry.mtime) {
                desired_mtime = existing_entry.mtime + 1LL;
            }
        }

        if (platform_set_path_times(
                argv[i],
                desired_atime,
                desired_mtime,
                options.no_create ? 0 : 1,
                options.update_access,
                options.update_modify
            ) != 0) {
            rt_write_cstr(2, "touch: cannot touch ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
