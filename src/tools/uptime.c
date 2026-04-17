#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

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

static size_t append_uptime_part(
    char *buffer,
    size_t buffer_size,
    size_t length,
    unsigned long long value,
    const char *singular,
    const char *plural,
    int *parts_written
) {
    if (value == 0ULL) {
        return length;
    }

    if (*parts_written > 0) {
        length = append_cstr(buffer, buffer_size, length, ", ");
    }
    length = append_uint(buffer, buffer_size, length, value);
    length = append_char(buffer, buffer_size, length, ' ');
    length = append_cstr(buffer, buffer_size, length, value == 1ULL ? singular : plural);
    *parts_written += 1;
    return length;
}

static void format_uptime_compact(unsigned long long total_seconds, char *buffer, size_t buffer_size) {
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

static void format_uptime_pretty(unsigned long long total_seconds, char *buffer, size_t buffer_size) {
    unsigned long long days = total_seconds / 86400ULL;
    unsigned long long hours = (total_seconds % 86400ULL) / 3600ULL;
    unsigned long long minutes = (total_seconds % 3600ULL) / 60ULL;
    unsigned long long seconds = total_seconds % 60ULL;
    size_t length = 0;
    int parts_written = 0;

    if (buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    length = append_uptime_part(buffer, buffer_size, length, days, "day", "days", &parts_written);
    length = append_uptime_part(buffer, buffer_size, length, hours, "hour", "hours", &parts_written);
    length = append_uptime_part(buffer, buffer_size, length, minutes, "minute", "minutes", &parts_written);
    if (parts_written == 0) {
        length = append_uint(buffer, buffer_size, length, seconds);
        length = append_char(buffer, buffer_size, length, ' ');
        (void)append_cstr(buffer, buffer_size, length, seconds == 1ULL ? "second" : "seconds");
    }
}

int main(int argc, char **argv) {
    PlatformUptimeInfo info;
    PlatformSessionEntry sessions[128];
    size_t session_count = 0;
    char uptime_text[64];
    char now_text[32];
    int pretty = 0;
    int since = 0;
    int argi;

    for (argi = 1; argi < argc; ++argi) {
        if (rt_strcmp(argv[argi], "--help") == 0) {
            tool_write_usage(argv[0], "[-p|--pretty] [-s|--since]");
            return 0;
        } else if (rt_strcmp(argv[argi], "-p") == 0 || rt_strcmp(argv[argi], "--pretty") == 0) {
            pretty = 1;
        } else if (rt_strcmp(argv[argi], "-s") == 0 || rt_strcmp(argv[argi], "--since") == 0) {
            since = 1;
        } else {
            tool_write_usage(argv[0], "[-p|--pretty] [-s|--since]");
            return 1;
        }
    }

    if (platform_get_uptime_info(&info) != 0) {
        tool_write_error("uptime", "uptime information unavailable", 0);
        return 1;
    }

    format_uptime_compact(info.uptime_seconds, uptime_text, sizeof(uptime_text));

    if (pretty) {
        format_uptime_pretty(info.uptime_seconds, uptime_text, sizeof(uptime_text));
        rt_write_cstr(1, "up ");
        rt_write_line(1, uptime_text);
        return 0;
    }

    if (since) {
        char boot_text[64];
        long long boot_time = platform_get_epoch_time() - (long long)info.uptime_seconds;

        if (platform_format_time(boot_time, 1, "%Y-%m-%d %H:%M:%S", boot_text, sizeof(boot_text)) == 0) {
            return rt_write_line(1, boot_text) == 0 ? 0 : 1;
        }
        tool_write_error("uptime", "boot time unavailable", 0);
        return 1;
    }

    if (platform_list_sessions(sessions, sizeof(sessions) / sizeof(sessions[0]), &session_count) != 0) {
        session_count = 0;
    }

    if (platform_format_time(platform_get_epoch_time(), 1, "%H:%M:%S", now_text, sizeof(now_text)) == 0) {
        rt_write_cstr(1, now_text);
        rt_write_char(1, ' ');
    }
    rt_write_cstr(1, "up ");
    rt_write_cstr(1, uptime_text);
    rt_write_cstr(1, ", ");
    rt_write_uint(1, (unsigned long long)session_count);
    rt_write_cstr(1, session_count == 1U ? " user, load average: " : " users, load average: ");
    rt_write_line(1, info.load_average[0] != '\0' ? info.load_average : "0.00 0.00 0.00");

    return 0;
}
