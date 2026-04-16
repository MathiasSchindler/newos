#include "platform.h"
#include "runtime.h"

static void write_padded(unsigned int value, unsigned int width) {
    char digits[16];
    unsigned int i = 0;

    do {
        digits[i++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value > 0U && i < sizeof(digits));

    while (i < width) {
        rt_write_char(1, '0');
        width -= 1U;
    }

    while (i > 0U) {
        i -= 1U;
        rt_write_char(1, digits[i]);
    }
}

static void civil_from_days(long long z, int *year_out, unsigned int *month_out, unsigned int *day_out) {
    long long era;
    unsigned long long doe;
    unsigned long long yoe;
    unsigned long long doy;
    unsigned long long mp;
    int year;
    unsigned int month;

    z += 719468LL;
    era = (z >= 0LL ? z : z - 146096LL) / 146097LL;
    doe = (unsigned long long)(z - era * 146097LL);
    yoe = (doe - doe / 1460ULL + doe / 36524ULL - doe / 146096ULL) / 365ULL;
    year = (int)yoe + (int)(era * 400LL);
    doy = doe - (365ULL * yoe + yoe / 4ULL - yoe / 100ULL);
    mp = (5ULL * doy + 2ULL) / 153ULL;

    *day_out = (unsigned int)(doy - (153ULL * mp + 2ULL) / 5ULL + 1ULL);
    month = (unsigned int)(mp < 10ULL ? mp + 3ULL : mp - 9ULL);
    *month_out = month;
    *year_out = year + (month <= 2U ? 1 : 0);
}

int main(int argc, char **argv) {
    long long now;
    long long days;
    unsigned long long secs_of_day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    int year;
    unsigned int month;
    unsigned int day;

    if (argc > 2 || (argc == 2 && rt_strcmp(argv[1], "-u") != 0)) {
        rt_write_line(2, "Usage: date [-u]");
        return 1;
    }

    now = platform_get_epoch_time();
    days = now / 86400LL;
    secs_of_day = (unsigned long long)(now % 86400LL);

    if (now < 0 && secs_of_day != 0ULL) {
        days -= 1LL;
        secs_of_day += 86400ULL;
    }

    civil_from_days(days, &year, &month, &day);
    hour = (unsigned int)(secs_of_day / 3600ULL);
    minute = (unsigned int)((secs_of_day % 3600ULL) / 60ULL);
    second = (unsigned int)(secs_of_day % 60ULL);

    rt_write_int(1, year);
    rt_write_char(1, '-');
    write_padded(month, 2U);
    rt_write_char(1, '-');
    write_padded(day, 2U);
    rt_write_char(1, ' ');
    write_padded(hour, 2U);
    rt_write_char(1, ':');
    write_padded(minute, 2U);
    rt_write_char(1, ':');
    write_padded(second, 2U);
    rt_write_line(1, " UTC");
    return 0;
}