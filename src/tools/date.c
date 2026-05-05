#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#include <limits.h>

typedef enum {
    DATE_FORMAT_DEFAULT,
    DATE_FORMAT_CUSTOM,
    DATE_FORMAT_ISO_DATE,
    DATE_FORMAT_ISO_HOURS,
    DATE_FORMAT_ISO_MINUTES,
    DATE_FORMAT_ISO_SECONDS,
    DATE_FORMAT_ISO_NS,
    DATE_FORMAT_RFC3339_DATE,
    DATE_FORMAT_RFC3339_SECONDS,
    DATE_FORMAT_RFC3339_NS
} DateFormatKind;

typedef struct {
    int use_utc;
    DateFormatKind format_kind;
    const char *custom_format;
    const char *date_value;
    const char *reference_path;
} DateOptions;

static char date_to_lower_ascii(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int date_is_alpha(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static int date_equals_ignore_case(const char *lhs, const char *rhs) {
    size_t index = 0;

    if (lhs == 0 || rhs == 0) {
        return 0;
    }
    while (lhs[index] != '\0' && rhs[index] != '\0') {
        if (date_to_lower_ascii(lhs[index]) != date_to_lower_ascii(rhs[index])) {
            return 0;
        }
        index += 1U;
    }
    return lhs[index] == '\0' && rhs[index] == '\0';
}

static int date_word_at(const char *text, size_t index, const char *word, size_t *end_out) {
    size_t offset = 0;

    while (word[offset] != '\0') {
        if (text[index + offset] == '\0' || date_to_lower_ascii(text[index + offset]) != date_to_lower_ascii(word[offset])) {
            return 0;
        }
        offset += 1U;
    }
    if (date_is_alpha(text[index + offset])) {
        return 0;
    }
    if (end_out != 0) {
        *end_out = index + offset;
    }
    return 1;
}

static void skip_spaces(const char *text, size_t *index_io) {
    while (rt_is_space(text[*index_io])) {
        *index_io += 1U;
    }
}

static int parse_fixed_digits(const char *text, size_t start, size_t digits, unsigned int *value_out) {
    unsigned long long value = 0ULL;
    size_t i;

    if (text == 0 || value_out == 0) {
        return -1;
    }
    for (i = 0; i < digits; ++i) {
        char ch = text[start + i];
        if (ch < '0' || ch > '9') {
            return -1;
        }
        value = value * 10ULL + (unsigned long long)(ch - '0');
    }
    *value_out = (unsigned int)value;
    return 0;
}

static int parse_unsigned_at(const char *text, size_t *index_io, unsigned long long *value_out) {
    size_t index = *index_io;
    unsigned long long value = 0ULL;
    int have_digit = 0;

    while (text[index] >= '0' && text[index] <= '9') {
        unsigned long long digit = (unsigned long long)(text[index] - '0');
        if (value > (ULLONG_MAX - digit) / 10ULL) {
            return -1;
        }
        value = value * 10ULL + digit;
        have_digit = 1;
        index += 1U;
    }
    if (!have_digit) {
        return -1;
    }
    *value_out = value;
    *index_io = index;
    return 0;
}

static int parse_signed_prefix(const char *text, size_t *index_io, long long *value_out) {
    size_t index = *index_io;
    unsigned long long magnitude;
    int negative = 0;

    if (text[index] == '-') {
        negative = 1;
        index += 1U;
    } else if (text[index] == '+') {
        index += 1U;
    }
    if (parse_unsigned_at(text, &index, &magnitude) != 0) {
        return -1;
    }
    if (!negative) {
        if (magnitude > (unsigned long long)LLONG_MAX) {
            return -1;
        }
        *value_out = (long long)magnitude;
    } else if (magnitude == (unsigned long long)LLONG_MAX + 1ULL) {
        *value_out = LLONG_MIN;
    } else {
        if (magnitude > (unsigned long long)LLONG_MAX) {
            return -1;
        }
        *value_out = -(long long)magnitude;
    }
    *index_io = index;
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
    unsigned int year_of_era = (unsigned int)(adjusted_year - era * 400);
    unsigned int shifted_month = month + (month > 2U ? (unsigned int)-3 : 9U);
    unsigned int day_of_year = ((153U * shifted_month) + 2U) / 5U + day - 1U;
    unsigned int day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return (long long)era * 146097LL + (long long)day_of_era - 719468LL;
}

static void civil_from_days(long long days, int *year_out, unsigned int *month_out, unsigned int *day_out) {
    long long z = days + 719468LL;
    long long era = (z >= 0LL ? z : z - 146096LL) / 146097LL;
    unsigned int day_of_era = (unsigned int)(z - era * 146097LL);
    unsigned int year_of_era = (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    int year = (int)year_of_era + (int)(era * 400LL);
    unsigned int day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    unsigned int month_index = (5U * day_of_year + 2U) / 153U;
    unsigned int day = day_of_year - (153U * month_index + 2U) / 5U + 1U;
    unsigned int month = month_index + (month_index < 10U ? 3U : (unsigned int)-9);

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

static void split_epoch_timestamp(long long epoch, int *year_out, unsigned int *month_out, unsigned int *day_out, unsigned int *hour_out, unsigned int *minute_out, unsigned int *second_out) {
    long long days = epoch / 86400LL;
    long long remainder = epoch % 86400LL;

    if (epoch < 0 && remainder != 0LL) {
        days -= 1LL;
        remainder += 86400LL;
    }
    civil_from_days(days, year_out, month_out, day_out);
    *hour_out = (unsigned int)(remainder / 3600LL);
    *minute_out = (unsigned int)((remainder % 3600LL) / 60LL);
    *second_out = (unsigned int)(remainder % 60LL);
}

static int checked_add_seconds(long long *epoch_io, long long delta) {
    if ((delta > 0LL && *epoch_io > LLONG_MAX - delta) || (delta < 0LL && *epoch_io < LLONG_MIN - delta)) {
        return -1;
    }
    *epoch_io += delta;
    return 0;
}

static int parse_numeric_timezone_offset(const char *text, size_t *index_io, int *offset_seconds_out) {
    size_t index = *index_io;
    int sign = 1;
    unsigned int hour = 0U;
    unsigned int minute = 0U;

    if (text[index] != '+' && text[index] != '-') {
        return -1;
    }
    if (text[index] == '-') {
        sign = -1;
    }
    index += 1U;
    if (parse_fixed_digits(text, index, 2U, &hour) != 0) {
        return -1;
    }
    index += 2U;
    if (text[index] == ':') {
        index += 1U;
        if (parse_fixed_digits(text, index, 2U, &minute) != 0) {
            return -1;
        }
        index += 2U;
    } else if (text[index] >= '0' && text[index] <= '9') {
        if (parse_fixed_digits(text, index, 2U, &minute) != 0) {
            return -1;
        }
        index += 2U;
    }
    if (hour > 23U || minute > 59U) {
        return -1;
    }
    *offset_seconds_out = sign * (int)(hour * 3600U + minute * 60U);
    *index_io = index;
    return 0;
}

static int parse_timezone_suffix(const char *text, size_t *index_io, int *offset_seconds_out) {
    size_t index = *index_io;
    size_t end;

    skip_spaces(text, &index);
    if (date_to_lower_ascii(text[index]) == 'z') {
        *offset_seconds_out = 0;
        *index_io = index + 1U;
        return 0;
    }
    if (date_word_at(text, index, "utc", &end) || date_word_at(text, index, "gmt", &end)) {
        index = end;
        if (text[index] == '+' || text[index] == '-') {
            if (parse_numeric_timezone_offset(text, &index, offset_seconds_out) != 0) {
                return -1;
            }
        } else {
            *offset_seconds_out = 0;
        }
        *index_io = index;
        return 0;
    }
    if ((text[index] == '+' || text[index] == '-') && text[index + 1U] >= '0' && text[index + 1U] <= '9') {
        if (parse_numeric_timezone_offset(text, &index, offset_seconds_out) != 0) {
            return -1;
        }
        *index_io = index;
        return 0;
    }
    return -1;
}

static int parse_calendar_value(const char *text, size_t *index_io, long long *epoch_out) {
    size_t index = *index_io;
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour = 0U;
    unsigned int minute = 0U;
    unsigned int second = 0U;
    int timezone_offset_seconds = 0;

    if (parse_fixed_digits(text, index, 4U, &year) != 0 || text[index + 4U] != '-' ||
        parse_fixed_digits(text, index + 5U, 2U, &month) != 0 || text[index + 7U] != '-' ||
        parse_fixed_digits(text, index + 8U, 2U, &day) != 0) {
        return -1;
    }
    index += 10U;
    if (text[index] == 'T' || (rt_is_space(text[index]) && text[index + 1U] >= '0' && text[index + 1U] <= '9')) {
        if (text[index] == 'T') {
            index += 1U;
        } else {
            skip_spaces(text, &index);
        }
        if (parse_fixed_digits(text, index, 2U, &hour) != 0 || text[index + 2U] != ':' || parse_fixed_digits(text, index + 3U, 2U, &minute) != 0) {
            return -1;
        }
        index += 5U;
        if (text[index] == ':') {
            index += 1U;
            if (parse_fixed_digits(text, index, 2U, &second) != 0) {
                return -1;
            }
            index += 2U;
        }
        if (text[index] == '.') {
            index += 1U;
            if (text[index] < '0' || text[index] > '9') {
                return -1;
            }
            while (text[index] >= '0' && text[index] <= '9') {
                index += 1U;
            }
        }
    }
    if (parse_timezone_suffix(text, &index, &timezone_offset_seconds) != 0) {
        timezone_offset_seconds = 0;
    }
    if (build_epoch_timestamp((int)year, month, day, hour, minute, second, epoch_out) != 0) {
        return -1;
    }
    *epoch_out -= (long long)timezone_offset_seconds;
    *index_io = index;
    return 0;
}

static int parse_base_date_value(const char *text, size_t *index_io, long long *epoch_out) {
    size_t index = *index_io;
    size_t end;

    skip_spaces(text, &index);
    if (date_word_at(text, index, "now", &end) || date_word_at(text, index, "today", &end)) {
        *epoch_out = platform_get_epoch_time();
        *index_io = end;
        return 0;
    }
    if (date_word_at(text, index, "yesterday", &end)) {
        *epoch_out = platform_get_epoch_time() - 86400LL;
        *index_io = end;
        return 0;
    }
    if (date_word_at(text, index, "tomorrow", &end)) {
        *epoch_out = platform_get_epoch_time() + 86400LL;
        *index_io = end;
        return 0;
    }
    if (text[index] == '@') {
        long long explicit_epoch;
        index += 1U;
        if (parse_signed_prefix(text, &index, &explicit_epoch) != 0) {
            return -1;
        }
        *epoch_out = explicit_epoch;
        *index_io = index;
        return 0;
    }
    if ((text[index] == '+' || text[index] == '-' || (text[index] >= '0' && text[index] <= '9')) &&
        !(text[index] >= '0' && text[index] <= '9' && text[index + 4U] == '-')) {
        long long explicit_epoch;
        if (parse_signed_prefix(text, &index, &explicit_epoch) != 0) {
            return -1;
        }
        *epoch_out = explicit_epoch;
        *index_io = index;
        return 0;
    }
    if (parse_calendar_value(text, &index, epoch_out) == 0) {
        *index_io = index;
        return 0;
    }
    return -1;
}

static int apply_month_delta(long long *epoch_io, int sign, unsigned long long amount, int months_per_unit) {
    int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    long long delta_months;
    long long total_months;
    long long new_year;
    unsigned int new_month;
    int max_day;

    if (amount > (unsigned long long)(LLONG_MAX / (long long)months_per_unit)) {
        return -1;
    }
    delta_months = (long long)amount * (long long)months_per_unit;
    if (sign < 0) {
        delta_months = -delta_months;
    }
    split_epoch_timestamp(*epoch_io, &year, &month, &day, &hour, &minute, &second);
    total_months = (long long)year * 12LL + (long long)month - 1LL + delta_months;
    new_year = total_months / 12LL;
    if (total_months < 0LL && (total_months % 12LL) != 0LL) {
        new_year -= 1LL;
    }
    if (new_year < (long long)INT_MIN || new_year > (long long)INT_MAX) {
        return -1;
    }
    new_month = (unsigned int)(total_months - new_year * 12LL) + 1U;
    max_day = days_in_month((int)new_year, new_month);
    if (day > (unsigned int)max_day) {
        day = (unsigned int)max_day;
    }
    return build_epoch_timestamp((int)new_year, new_month, day, hour, minute, second, epoch_io);
}

static int unit_matches(const char *text, size_t start, size_t length, const char *word) {
    size_t end;
    return date_word_at(text, start, word, &end) && end == start + length;
}

static int parse_relative_unit(const char *text, size_t *index_io, int sign, unsigned long long amount, long long *epoch_io) {
    size_t index = *index_io;
    size_t start;
    size_t length;
    long long multiplier = 0LL;
    int months_per_unit = 0;

    skip_spaces(text, &index);
    start = index;
    while (date_is_alpha(text[index])) {
        index += 1U;
    }
    length = index - start;
    if (length == 0U) {
        return -1;
    }
    if ((length == 1U && date_to_lower_ascii(text[start]) == 's') || unit_matches(text, start, length, "sec") || unit_matches(text, start, length, "second") || unit_matches(text, start, length, "seconds")) {
        multiplier = 1LL;
    } else if ((length == 1U && date_to_lower_ascii(text[start]) == 'm') || unit_matches(text, start, length, "min") || unit_matches(text, start, length, "minute") || unit_matches(text, start, length, "minutes")) {
        multiplier = 60LL;
    } else if ((length == 1U && date_to_lower_ascii(text[start]) == 'h') || unit_matches(text, start, length, "hr") || unit_matches(text, start, length, "hour") || unit_matches(text, start, length, "hours")) {
        multiplier = 3600LL;
    } else if ((length == 1U && date_to_lower_ascii(text[start]) == 'd') || unit_matches(text, start, length, "day") || unit_matches(text, start, length, "days")) {
        multiplier = 86400LL;
    } else if ((length == 1U && date_to_lower_ascii(text[start]) == 'w') || unit_matches(text, start, length, "week") || unit_matches(text, start, length, "weeks")) {
        multiplier = 604800LL;
    } else if (unit_matches(text, start, length, "mon") || unit_matches(text, start, length, "month") || unit_matches(text, start, length, "months")) {
        months_per_unit = 1;
    } else if ((length == 1U && date_to_lower_ascii(text[start]) == 'y') || unit_matches(text, start, length, "year") || unit_matches(text, start, length, "years")) {
        months_per_unit = 12;
    } else {
        return -1;
    }
    skip_spaces(text, &index);
    if (date_word_at(text, index, "ago", &index)) {
        sign = -sign;
    }
    if (months_per_unit != 0) {
        if (apply_month_delta(epoch_io, sign, amount, months_per_unit) != 0) {
            return -1;
        }
    } else {
        long long delta;
        if (amount > (unsigned long long)(LLONG_MAX / multiplier)) {
            return -1;
        }
        delta = (long long)amount * multiplier;
        if (sign < 0) {
            delta = -delta;
        }
        if (checked_add_seconds(epoch_io, delta) != 0) {
            return -1;
        }
    }
    *index_io = index;
    return 0;
}

static int parse_date_value(const char *text, long long *epoch_out) {
    size_t index = 0;
    long long epoch;
    int base_is_now = 0;

    if (text == 0 || epoch_out == 0) {
        return -1;
    }
    skip_spaces(text, &index);
    if (text[index] == '+' || text[index] == '-') {
        size_t lookahead = index + 1U;
        unsigned long long ignored_amount;

        if (parse_unsigned_at(text, &lookahead, &ignored_amount) == 0) {
            skip_spaces(text, &lookahead);
        }
        if (date_is_alpha(text[lookahead])) {
            base_is_now = 1;
            epoch = platform_get_epoch_time();
        } else if (parse_base_date_value(text, &index, &epoch) != 0) {
            return -1;
        }
    } else if (date_is_alpha(text[index]) && !date_word_at(text, index, "now", 0) && !date_word_at(text, index, "today", 0) && !date_word_at(text, index, "yesterday", 0) && !date_word_at(text, index, "tomorrow", 0)) {
        base_is_now = 1;
        epoch = platform_get_epoch_time();
    } else if (parse_base_date_value(text, &index, &epoch) != 0) {
        return -1;
    }
    while (1) {
        int sign = 1;
        unsigned long long amount;

        skip_spaces(text, &index);
        if (text[index] == '\0') {
            *epoch_out = epoch;
            return 0;
        }
        if (text[index] == '+') {
            sign = 1;
            index += 1U;
        } else if (text[index] == '-') {
            sign = -1;
            index += 1U;
        } else if (!base_is_now && !(text[index] >= '0' && text[index] <= '9')) {
            return -1;
        }
        skip_spaces(text, &index);
        if (parse_unsigned_at(text, &index, &amount) != 0 || parse_relative_unit(text, &index, sign, amount, &epoch) != 0) {
            return -1;
        }
        base_is_now = 0;
    }
}

static const char *format_for_kind(DateFormatKind kind, int use_utc) {
    switch (kind) {
        case DATE_FORMAT_ISO_DATE:
        case DATE_FORMAT_RFC3339_DATE:
            return "%Y-%m-%d";
        case DATE_FORMAT_ISO_HOURS:
            return use_utc ? "%Y-%m-%dT%HZ" : "%Y-%m-%dT%H";
        case DATE_FORMAT_ISO_MINUTES:
            return use_utc ? "%Y-%m-%dT%H:%MZ" : "%Y-%m-%dT%H:%M";
        case DATE_FORMAT_ISO_NS:
            return use_utc ? "%Y-%m-%dT%H:%M:%S.000000000Z" : "%Y-%m-%dT%H:%M:%S.000000000";
        case DATE_FORMAT_ISO_SECONDS:
            return use_utc ? "%Y-%m-%dT%H:%M:%SZ" : "%Y-%m-%dT%H:%M:%S";
        case DATE_FORMAT_RFC3339_NS:
            return use_utc ? "%Y-%m-%d %H:%M:%S.000000000Z" : "%Y-%m-%d %H:%M:%S.000000000";
        case DATE_FORMAT_RFC3339_SECONDS:
            return use_utc ? "%Y-%m-%d %H:%M:%SZ" : "%Y-%m-%d %H:%M:%S";
        case DATE_FORMAT_DEFAULT:
        case DATE_FORMAT_CUSTOM:
        default:
            return "%Y-%m-%d %H:%M:%S";
    }
}

static int parse_iso_precision(const char *text, DateFormatKind *kind_out) {
    if (text == 0 || text[0] == '\0' || date_equals_ignore_case(text, "date")) {
        *kind_out = DATE_FORMAT_ISO_DATE;
        return 0;
    }
    if (date_equals_ignore_case(text, "hours")) {
        *kind_out = DATE_FORMAT_ISO_HOURS;
        return 0;
    }
    if (date_equals_ignore_case(text, "minutes")) {
        *kind_out = DATE_FORMAT_ISO_MINUTES;
        return 0;
    }
    if (date_equals_ignore_case(text, "seconds")) {
        *kind_out = DATE_FORMAT_ISO_SECONDS;
        return 0;
    }
    if (date_equals_ignore_case(text, "ns") || date_equals_ignore_case(text, "nanoseconds")) {
        *kind_out = DATE_FORMAT_ISO_NS;
        return 0;
    }
    return -1;
}

static int parse_rfc3339_precision(const char *text, DateFormatKind *kind_out) {
    if (date_equals_ignore_case(text, "date")) {
        *kind_out = DATE_FORMAT_RFC3339_DATE;
        return 0;
    }
    if (date_equals_ignore_case(text, "seconds")) {
        *kind_out = DATE_FORMAT_RFC3339_SECONDS;
        return 0;
    }
    if (date_equals_ignore_case(text, "ns") || date_equals_ignore_case(text, "nanoseconds")) {
        *kind_out = DATE_FORMAT_RFC3339_NS;
        return 0;
    }
    return -1;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-u|-l] [-d TEXT|-r FILE] [--iso-8601[=PRECISION]|--rfc-3339=PRECISION] [+FORMAT]");
}

static int set_format_kind(DateOptions *options, DateFormatKind kind) {
    if (options->format_kind != DATE_FORMAT_DEFAULT) {
        return -1;
    }
    options->format_kind = kind;
    return 0;
}

static int parse_options(int argc, char **argv, DateOptions *options) {
    int argi;

    rt_memset(options, 0, sizeof(*options));
    options->use_utc = 1;
    options->format_kind = DATE_FORMAT_DEFAULT;
    for (argi = 1; argi < argc; ++argi) {
        if (rt_strcmp(argv[argi], "-u") == 0) {
            options->use_utc = 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-l") == 0) {
            options->use_utc = 0;
            continue;
        }
        if (rt_strcmp(argv[argi], "-s") == 0) {
            if (argi + 1 < argc) {
                argi += 1;
            }
            tool_write_error("date", "setting system time is not supported", 0);
            return -2;
        }
        if (tool_starts_with(argv[argi], "--set=")) {
            tool_write_error("date", "setting system time is not supported", 0);
            return -2;
        }
        if (rt_strcmp(argv[argi], "-d") == 0) {
            if (argi + 1 >= argc || options->date_value != 0) {
                return -1;
            }
            options->date_value = argv[argi + 1];
            argi += 1;
            continue;
        }
        if (tool_starts_with(argv[argi], "--date=")) {
            if (options->date_value != 0) {
                return -1;
            }
            options->date_value = argv[argi] + 7;
            continue;
        }
        if (rt_strcmp(argv[argi], "-r") == 0) {
            if (argi + 1 >= argc || options->reference_path != 0) {
                return -1;
            }
            options->reference_path = argv[argi + 1];
            argi += 1;
            continue;
        }
        if (tool_starts_with(argv[argi], "--reference=")) {
            if (options->reference_path != 0) {
                return -1;
            }
            options->reference_path = argv[argi] + 12;
            continue;
        }
        if (rt_strcmp(argv[argi], "--iso-8601") == 0 || rt_strcmp(argv[argi], "-I") == 0) {
            if (set_format_kind(options, DATE_FORMAT_ISO_DATE) != 0) {
                return -1;
            }
            continue;
        }
        if (tool_starts_with(argv[argi], "--iso-8601=")) {
            DateFormatKind kind;
            if (parse_iso_precision(argv[argi] + 11, &kind) != 0 || set_format_kind(options, kind) != 0) {
                return -1;
            }
            continue;
        }
        if (tool_starts_with(argv[argi], "-I") && argv[argi][2] != '\0') {
            DateFormatKind kind;
            if (parse_iso_precision(argv[argi] + 2, &kind) != 0 || set_format_kind(options, kind) != 0) {
                return -1;
            }
            continue;
        }
        if (tool_starts_with(argv[argi], "--rfc-3339=")) {
            DateFormatKind kind;
            if (parse_rfc3339_precision(argv[argi] + 11, &kind) != 0 || set_format_kind(options, kind) != 0) {
                return -1;
            }
            continue;
        }
        if (argv[argi][0] == '+' && argv[argi][1] != '\0') {
            if (set_format_kind(options, DATE_FORMAT_CUSTOM) != 0) {
                return -1;
            }
            options->custom_format = argv[argi] + 1;
            continue;
        }
        return -1;
    }
    if (options->date_value != 0 && options->reference_path != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    DateOptions options;
    const char *actual_format;
    int parse_status;
    char output[256];
    long long epoch_seconds;

    parse_status = parse_options(argc, argv, &options);
    if (parse_status == -2) {
        return 1;
    }
    if (parse_status != 0) {
        print_usage(argv[0]);
        return 1;
    }
    if (options.reference_path != 0) {
        PlatformDirEntry entry;

        if (platform_get_path_info(options.reference_path, &entry) != 0) {
            tool_write_error("date", "cannot read reference file ", options.reference_path);
            return 1;
        }
        epoch_seconds = entry.mtime;
    } else if (options.date_value != 0) {
        if (parse_date_value(options.date_value, &epoch_seconds) != 0) {
            tool_write_error("date", "unsupported date value: ", options.date_value);
            return 1;
        }
    } else {
        epoch_seconds = platform_get_epoch_time();
    }
    actual_format = options.format_kind == DATE_FORMAT_CUSTOM ? options.custom_format : format_for_kind(options.format_kind, options.use_utc);
    if (platform_format_time(epoch_seconds, options.use_utc ? 0 : 1, actual_format, output, sizeof(output)) != 0) {
        if (platform_format_time(epoch_seconds, 0, "%Y-%m-%d %H:%M:%S", output, sizeof(output)) != 0) {
            tool_write_error("date", "failed to format time", 0);
            return 1;
        }
        options.use_utc = 1;
        options.format_kind = DATE_FORMAT_DEFAULT;
    }
    if (rt_write_cstr(1, output) != 0) {
        return 1;
    }
    if (options.format_kind == DATE_FORMAT_DEFAULT && options.use_utc) {
        return rt_write_line(1, " UTC") == 0 ? 0 : 1;
    }
    return rt_write_char(1, '\n') == 0 ? 0 : 1;
}