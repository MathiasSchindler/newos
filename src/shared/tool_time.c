#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int tool_parse_numeric_timezone_offset(const char *text, size_t *index_io, int *offset_seconds_out) {
    size_t index;
    int sign = 1;
    unsigned int hour = 0U;
    unsigned int minute = 0U;

    if (text == 0 || index_io == 0 || offset_seconds_out == 0) {
        return -1;
    }
    index = *index_io;
    if (text[index] != '+' && text[index] != '-') {
        return -1;
    }
    if (text[index] == '-') {
        sign = -1;
    }
    index += 1U;
    if (tool_parse_fixed_digits(text, index, 2U, &hour) != 0) {
        return -1;
    }
    index += 2U;
    if (text[index] == ':') {
        index += 1U;
        if (tool_parse_fixed_digits(text, index, 2U, &minute) != 0) {
            return -1;
        }
        index += 2U;
    } else if (text[index] >= '0' && text[index] <= '9') {
        if (tool_parse_fixed_digits(text, index, 2U, &minute) != 0) {
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

static int tool_timezone_word_at(const char *text, size_t index, const char *name, size_t *end_out) {
    size_t offset = 0U;

    while (name[offset] != '\0') {
        if (tool_ascii_tolower(text[index + offset]) != tool_ascii_tolower(name[offset])) {
            return 0;
        }
        offset += 1U;
    }
    *end_out = index + offset;
    return 1;
}

int tool_parse_timezone_suffix(const char *text, size_t *index_io, int *offset_seconds_out, int accept_empty) {
    size_t index;
    size_t end = 0U;

    if (text == 0 || index_io == 0 || offset_seconds_out == 0) {
        return -1;
    }
    index = *index_io;
    while (rt_is_space(text[index])) {
        index += 1U;
    }
    if (text[index] == '\0') {
        if (!accept_empty) {
            return -1;
        }
        *offset_seconds_out = 0;
        *index_io = index;
        return 0;
    }
    if (tool_ascii_tolower(text[index]) == 'z') {
        *offset_seconds_out = 0;
        *index_io = index + 1U;
        return 0;
    }
    if (tool_timezone_word_at(text, index, "utc", &end) || tool_timezone_word_at(text, index, "gmt", &end)) {
        index = end;
        if (text[index] == '+' || text[index] == '-') {
            if (tool_parse_numeric_timezone_offset(text, &index, offset_seconds_out) != 0) {
                return -1;
            }
        } else {
            *offset_seconds_out = 0;
        }
        *index_io = index;
        return 0;
    }
    if ((text[index] == '+' || text[index] == '-') && text[index + 1U] >= '0' && text[index + 1U] <= '9') {
        if (tool_parse_numeric_timezone_offset(text, &index, offset_seconds_out) != 0) {
            return -1;
        }
        *index_io = index;
        return 0;
    }
    return -1;
}

int tool_parse_duration_ms(const char *text, unsigned long long *milliseconds_out) {
    unsigned long long whole = 0;
    unsigned long long fraction = 0;
    unsigned long long divisor = 1;
    unsigned long long unit_ms = 1000ULL;
    int saw_digit = 0;
    int saw_fraction = 0;

    if (text == 0 || text[0] == '\0' || milliseconds_out == 0) {
        return -1;
    }

    while (*text >= '0' && *text <= '9') {
        saw_digit = 1;
        whole = (whole * 10ULL) + (unsigned long long)(*text - '0');
        text += 1;
    }

    if (*text == '.') {
        text += 1;
        while (*text >= '0' && *text <= '9') {
            saw_digit = 1;
            saw_fraction = 1;
            if (divisor < 1000000ULL) {
                fraction = (fraction * 10ULL) + (unsigned long long)(*text - '0');
                divisor *= 10ULL;
            }
            text += 1;
        }
    }

    if (!saw_digit) {
        return -1;
    }

    if (text[0] == '\0' || (text[0] == 's' && text[1] == '\0')) {
        unit_ms = 1000ULL;
    } else if (text[0] == 'm' && text[1] == 's' && text[2] == '\0') {
        unit_ms = 1ULL;
    } else if (text[0] == 'm' && text[1] == '\0') {
        unit_ms = 60ULL * 1000ULL;
    } else if (text[0] == 'h' && text[1] == '\0') {
        unit_ms = 60ULL * 60ULL * 1000ULL;
    } else if (text[0] == 'd' && text[1] == '\0') {
        unit_ms = 24ULL * 60ULL * 60ULL * 1000ULL;
    } else {
        return -1;
    }

    *milliseconds_out = whole * unit_ms;
    if (saw_fraction) {
        unsigned long long fraction_ms = ((fraction * unit_ms) + (divisor / 2ULL)) / divisor;
        if (fraction_ms == 0ULL && fraction > 0ULL) {
            fraction_ms = 1ULL;
        }
        *milliseconds_out += fraction_ms;
    }

    return 0;
}

void tool_format_uptime_compact(unsigned long long total_seconds, char *buffer, size_t buffer_size) {
    unsigned long long days = total_seconds / 86400ULL;
    unsigned long long hours = (total_seconds % 86400ULL) / 3600ULL;
    unsigned long long minutes = (total_seconds % 3600ULL) / 60ULL;
    unsigned long long seconds = total_seconds % 60ULL;
    size_t length = 0U;

    if (buffer_size == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (days > 0ULL) {
        length = tool_buffer_append_uint(buffer, buffer_size, length, days);
        length = tool_buffer_append_char(buffer, buffer_size, length, 'd');
        length = tool_buffer_append_char(buffer, buffer_size, length, ' ');
        length = tool_buffer_append_uint(buffer, buffer_size, length, hours);
        length = tool_buffer_append_char(buffer, buffer_size, length, 'h');
        length = tool_buffer_append_char(buffer, buffer_size, length, ' ');
        length = tool_buffer_append_uint(buffer, buffer_size, length, minutes);
        (void)tool_buffer_append_char(buffer, buffer_size, length, 'm');
    } else if (hours > 0ULL) {
        length = tool_buffer_append_uint(buffer, buffer_size, length, hours);
        length = tool_buffer_append_char(buffer, buffer_size, length, 'h');
        length = tool_buffer_append_char(buffer, buffer_size, length, ' ');
        length = tool_buffer_append_uint(buffer, buffer_size, length, minutes);
        (void)tool_buffer_append_char(buffer, buffer_size, length, 'm');
    } else if (minutes > 0ULL) {
        length = tool_buffer_append_uint(buffer, buffer_size, length, minutes);
        (void)tool_buffer_append_char(buffer, buffer_size, length, 'm');
    } else {
        length = tool_buffer_append_uint(buffer, buffer_size, length, seconds);
        (void)tool_buffer_append_char(buffer, buffer_size, length, 's');
    }
}

static int tool_is_leap_year(int year) {
    if ((year % 400) == 0) return 1;
    if ((year % 100) == 0) return 0;
    return (year % 4) == 0;
}

int tool_days_in_month(int year, unsigned int month) {
    static const unsigned char days[] = { 31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U };

    if (month == 0U || month > 12U) {
        return 0;
    }
    if (month == 2U && tool_is_leap_year(year)) {
        return 29;
    }
    return (int)days[month - 1U];
}

long long tool_days_from_civil(int year, unsigned int month, unsigned int day) {
    int adjusted_year = year - (month <= 2U ? 1 : 0);
    int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    unsigned int year_of_era = (unsigned int)(adjusted_year - (era * 400));
    unsigned int shifted_month = month + (month > 2U ? (unsigned int)-3 : 9U);
    unsigned int day_of_year = ((153U * shifted_month) + 2U) / 5U + day - 1U;
    unsigned int day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return (long long)era * 146097LL + (long long)day_of_era - 719468LL;
}

void tool_civil_from_days(long long days, int *year_out, unsigned int *month_out, unsigned int *day_out) {
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
    if (year_out != 0) {
        *year_out = year;
    }
    if (month_out != 0) {
        *month_out = month;
    }
    if (day_out != 0) {
        *day_out = day;
    }
}

int tool_build_epoch_timestamp(int year, unsigned int month, unsigned int day, unsigned int hour, unsigned int minute, unsigned int second, long long *epoch_out) {
    long long days;

    if (epoch_out == 0 || month < 1U || month > 12U || day < 1U || day > (unsigned int)tool_days_in_month(year, month) ||
        hour > 23U || minute > 59U || second > 59U) {
        return -1;
    }

    days = tool_days_from_civil(year, month, day);
    *epoch_out = days * 86400LL + (long long)hour * 3600LL + (long long)minute * 60LL + (long long)second;
    return 0;
}
