#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void write_session_line(const PlatformSessionEntry *entry) {
    char time_text[64];
    int has_terminal;
    int has_time;

    rt_write_cstr(1, entry->username[0] != '\0' ? entry->username : "unknown");

    has_terminal = entry->terminal[0] != '\0';
    has_time = entry->login_time > 0 &&
               platform_format_time(entry->login_time, 1, "%Y-%m-%d %H:%M", time_text, sizeof(time_text)) == 0;

    if (has_terminal) {
        rt_write_char(1, ' ');
        rt_write_cstr(1, entry->terminal);
    }

    if (has_time) {
        rt_write_char(1, ' ');
        rt_write_cstr(1, time_text);
    }

    if (entry->host[0] != '\0') {
        rt_write_cstr(1, " (");
        rt_write_cstr(1, entry->host);
        rt_write_char(1, ')');
    }

    rt_write_char(1, '\n');
}

int main(int argc, char **argv) {
    PlatformSessionEntry entries[128];
    PlatformUptimeInfo uptime;
    int quick = 0;
    int show_header = 0;
    int show_boot = 0;
    size_t count = 0;
    size_t i;
    size_t display_count;
    int argi;

    for (argi = 1; argi < argc; ++argi) {
        const char *arg = argv[argi];
        size_t j;

        if (arg[0] != '-' || arg[1] == '\0') {
            tool_write_usage(argv[0], "[-H] [-b] [-q]");
            return 1;
        }

        for (j = 1; arg[j] != '\0'; ++j) {
            if (arg[j] == 'q') {
                quick = 1;
            } else if (arg[j] == 'H') {
                show_header = 1;
            } else if (arg[j] == 'b') {
                show_boot = 1;
            } else {
                tool_write_usage(argv[0], "[-H] [-b] [-q]");
                return 1;
            }
        }
    }

    if (show_boot) {
        char boot_text[64];
        long long boot_time;

        if (platform_get_uptime_info(&uptime) != 0) {
            tool_write_error("who", "boot information unavailable", 0);
            return 1;
        }

        boot_time = platform_get_epoch_time() - (long long)uptime.uptime_seconds;
        if (platform_format_time(boot_time, 1, "%Y-%m-%d %H:%M", boot_text, sizeof(boot_text)) != 0) {
            tool_write_error("who", "boot information unavailable", 0);
            return 1;
        }

        if (show_header) {
            rt_write_line(1, "EVENT        TIME");
        }
        rt_write_cstr(1, "system boot  ");
        rt_write_line(1, boot_text);
        return 0;
    }

    if (platform_list_sessions(entries, sizeof(entries) / sizeof(entries[0]), &count) != 0) {
        tool_write_error("who", "user information unavailable", 0);
        return 1;
    }

    display_count = count < (sizeof(entries) / sizeof(entries[0])) ? count : (sizeof(entries) / sizeof(entries[0]));

    if (quick) {
        for (i = 0; i < display_count; ++i) {
            if (i > 0) {
                rt_write_char(1, ' ');
            }
            rt_write_cstr(1, entries[i].username[0] != '\0' ? entries[i].username : "unknown");
        }
        rt_write_char(1, '\n');
        rt_write_cstr(1, "# users=");
        rt_write_uint(1, (unsigned long long)count);
        rt_write_char(1, '\n');
        return 0;
    }

    if (show_header) {
        rt_write_line(1, "NAME             LINE             TIME             HOST");
    }

    for (i = 0; i < display_count; ++i) {
        write_session_line(&entries[i]);
    }

    return 0;
}
