#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "tool_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <utmpx.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

static int get_uptime_seconds(unsigned long long *seconds_out) {
#if defined(__APPLE__)
    struct timeval boot_time;
    size_t size = sizeof(boot_time);
    int mib[2] = { CTL_KERN, KERN_BOOTTIME };
    time_t now = time(0);

    if (sysctl(mib, 2, &boot_time, &size, 0, 0) == 0 && now >= boot_time.tv_sec) {
        *seconds_out = (unsigned long long)(now - boot_time.tv_sec);
        return 0;
    }
#endif

#if defined(CLOCK_BOOTTIME)
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
            *seconds_out = (unsigned long long)ts.tv_sec;
            return 0;
        }
    }
#endif

    return -1;
}

static unsigned int count_logged_in_users(void) {
    struct utmpx *entry;
    unsigned int count = 0;

    setutxent();
    while ((entry = getutxent()) != 0) {
        if (entry->ut_type == USER_PROCESS && entry->ut_user[0] != '\0') {
            count += 1;
        }
    }
    endutxent();

    return count;
}

static void format_uptime(unsigned long long total_seconds, char *buffer, size_t buffer_size) {
    unsigned long long days = total_seconds / 86400ULL;
    unsigned long long hours = (total_seconds % 86400ULL) / 3600ULL;
    unsigned long long minutes = (total_seconds % 3600ULL) / 60ULL;
    unsigned long long seconds = total_seconds % 60ULL;

    if (days > 0) {
        (void)snprintf(buffer, buffer_size, "%llud %lluh %llum", days, hours, minutes);
    } else if (hours > 0) {
        (void)snprintf(buffer, buffer_size, "%lluh %llum", hours, minutes);
    } else if (minutes > 0) {
        (void)snprintf(buffer, buffer_size, "%llum", minutes);
    } else {
        (void)snprintf(buffer, buffer_size, "%llus", seconds);
    }
}

int main(int argc, char **argv) {
    unsigned long long uptime_seconds = 0;
    double loads[3] = { 0.0, 0.0, 0.0 };
    char uptime_text[64];
    char load_text[96];
    unsigned int user_count;

    if (argc != 1) {
        tool_write_usage(argv[0], "");
        return 1;
    }

    if (get_uptime_seconds(&uptime_seconds) != 0) {
        tool_write_error("uptime", "uptime information unavailable", 0);
        return 1;
    }

    if (getloadavg(loads, 3) < 0) {
        loads[0] = 0.0;
        loads[1] = 0.0;
        loads[2] = 0.0;
    }

    user_count = count_logged_in_users();
    format_uptime(uptime_seconds, uptime_text, sizeof(uptime_text));
    (void)snprintf(load_text, sizeof(load_text), "%.2f %.2f %.2f", loads[0], loads[1], loads[2]);

    rt_write_cstr(1, "up ");
    rt_write_cstr(1, uptime_text);
    rt_write_cstr(1, ", ");
    rt_write_uint(1, user_count);
    rt_write_cstr(1, user_count == 1 ? " user, load average: " : " users, load average: ");
    rt_write_line(1, load_text);

    return 0;
}
