#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if __STDC_HOSTED__
#include <time.h>
#include <utmpx.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

static int read_text_file(const char *path, char *buffer, size_t buffer_size) {
    int fd;
    size_t used = 0;

    if (buffer_size == 0) {
        return -1;
    }

    fd = platform_open_read(path);
    if (fd < 0) {
        buffer[0] = '\0';
        return -1;
    }

    while (used + 1U < buffer_size) {
        long bytes_read = platform_read(fd, buffer + used, buffer_size - used - 1U);
        if (bytes_read < 0) {
            platform_close(fd);
            buffer[0] = '\0';
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }
        used += (size_t)bytes_read;
    }

    buffer[used] = '\0';
    platform_close(fd);
    return 0;
}

static size_t append_char(char *buffer, size_t buffer_size, size_t length, char ch) {
    if (buffer_size == 0) {
        return 0;
    }

    if (length + 1U < buffer_size) {
        buffer[length] = ch;
        length += 1U;
        buffer[length] = '\0';
    } else {
        buffer[buffer_size - 1U] = '\0';
    }

    return length;
}

static size_t append_cstr(char *buffer, size_t buffer_size, size_t length, const char *text) {
    size_t index = 0;

    while (text != 0 && text[index] != '\0') {
        length = append_char(buffer, buffer_size, length, text[index]);
        index += 1U;
    }

    return length;
}

static size_t append_uint(char *buffer, size_t buffer_size, size_t length, unsigned long long value) {
    char digits[32];
    rt_unsigned_to_string(value, digits, sizeof(digits));
    return append_cstr(buffer, buffer_size, length, digits);
}

static int parse_unsigned_prefix(const char *text, unsigned long long *value_out) {
    unsigned long long value = 0;
    size_t index = 0;

    if (text == 0 || value_out == 0 || text[0] < '0' || text[0] > '9') {
        return -1;
    }

    while (text[index] >= '0' && text[index] <= '9') {
        value = (value * 10ULL) + (unsigned long long)(text[index] - '0');
        index += 1U;
    }

    *value_out = value;
    return 0;
}

static int get_uptime_seconds(unsigned long long *seconds_out) {
    char uptime_text[128];

    if (read_text_file("/proc/uptime", uptime_text, sizeof(uptime_text)) == 0 &&
        parse_unsigned_prefix(uptime_text, seconds_out) == 0) {
        return 0;
    }

#if __STDC_HOSTED__
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
#else
    (void)seconds_out;
    return -1;
#endif
}

static unsigned int count_logged_in_users(void) {
#if __STDC_HOSTED__
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
#else
    return 0;
#endif
}

static int read_loadavg_text(char *buffer, size_t buffer_size) {
    char contents[128];
    size_t index = 0;
    size_t length = 0;
    int fields = 0;

    if (buffer_size == 0 || read_text_file("/proc/loadavg", contents, sizeof(contents)) != 0) {
        return -1;
    }

    buffer[0] = '\0';
    while (contents[index] != '\0' && fields < 3) {
        while (rt_is_space(contents[index])) {
            index += 1U;
        }
        if (contents[index] == '\0') {
            break;
        }
        if (fields > 0) {
            length = append_char(buffer, buffer_size, length, ' ');
        }
        while (contents[index] != '\0' && !rt_is_space(contents[index])) {
            length = append_char(buffer, buffer_size, length, contents[index]);
            index += 1U;
        }
        fields += 1;
    }

    return fields == 3 ? 0 : -1;
}

static void format_uptime(unsigned long long total_seconds, char *buffer, size_t buffer_size) {
    unsigned long long days = total_seconds / 86400ULL;
    unsigned long long hours = (total_seconds % 86400ULL) / 3600ULL;
    unsigned long long minutes = (total_seconds % 3600ULL) / 60ULL;
    unsigned long long seconds = total_seconds % 60ULL;
    size_t length = 0;

    if (buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (days > 0) {
        length = append_uint(buffer, buffer_size, length, days);
        length = append_char(buffer, buffer_size, length, 'd');
        length = append_char(buffer, buffer_size, length, ' ');
        length = append_uint(buffer, buffer_size, length, hours);
        length = append_char(buffer, buffer_size, length, 'h');
        length = append_char(buffer, buffer_size, length, ' ');
        length = append_uint(buffer, buffer_size, length, minutes);
        (void)append_char(buffer, buffer_size, length, 'm');
    } else if (hours > 0) {
        length = append_uint(buffer, buffer_size, length, hours);
        length = append_char(buffer, buffer_size, length, 'h');
        length = append_char(buffer, buffer_size, length, ' ');
        length = append_uint(buffer, buffer_size, length, minutes);
        (void)append_char(buffer, buffer_size, length, 'm');
    } else if (minutes > 0) {
        length = append_uint(buffer, buffer_size, length, minutes);
        (void)append_char(buffer, buffer_size, length, 'm');
    } else {
        length = append_uint(buffer, buffer_size, length, seconds);
        (void)append_char(buffer, buffer_size, length, 's');
    }
}

int main(int argc, char **argv) {
    unsigned long long uptime_seconds = 0;
    char uptime_text[64];
    char load_text[96];
    unsigned int user_count;
    int pretty = 0;
    int since = 0;
    int argi;

    for (argi = 1; argi < argc; ++argi) {
        if (rt_strcmp(argv[argi], "-p") == 0) {
            pretty = 1;
        } else if (rt_strcmp(argv[argi], "-s") == 0) {
            since = 1;
        } else {
            tool_write_usage(argv[0], "[-p] [-s]");
            return 1;
        }
    }

    if (get_uptime_seconds(&uptime_seconds) != 0) {
        tool_write_error("uptime", "uptime information unavailable", 0);
        return 1;
    }

    format_uptime(uptime_seconds, uptime_text, sizeof(uptime_text));

    if (pretty) {
        rt_write_cstr(1, "up ");
        rt_write_line(1, uptime_text);
        return 0;
    }

    if (since) {
#if __STDC_HOSTED__
        time_t boot = time(0) - (time_t)uptime_seconds;
        struct tm *local = localtime(&boot);
        char boot_text[64];

        if (local != 0 && strftime(boot_text, sizeof(boot_text), "%Y-%m-%d %H:%M:%S", local) > 0) {
            return rt_write_line(1, boot_text) == 0 ? 0 : 1;
        }
        tool_write_error("uptime", "boot time unavailable", 0);
        return 1;
#else
        tool_write_error("uptime", "boot time unavailable", 0);
        return 1;
#endif
    }

    if (read_loadavg_text(load_text, sizeof(load_text)) != 0) {
        rt_copy_string(load_text, sizeof(load_text), "0.00 0.00 0.00");
    }

    user_count = count_logged_in_users();

    rt_write_cstr(1, "up ");
    rt_write_cstr(1, uptime_text);
    rt_write_cstr(1, ", ");
    rt_write_uint(1, user_count);
    rt_write_cstr(1, user_count == 1 ? " user, load average: " : " users, load average: ");
    rt_write_line(1, load_text);

    return 0;
}
