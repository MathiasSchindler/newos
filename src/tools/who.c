#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "tool_util.h"

#include <time.h>
#include <utmpx.h>

static void copy_field(char *dst, size_t dst_size, const char *src, size_t src_size) {
    size_t i = 0;

    if (dst_size == 0) {
        return;
    }

    while (i + 1 < dst_size && i < src_size && src[i] != '\0') {
        dst[i] = src[i];
        i += 1;
    }
    dst[i] = '\0';
}

int main(int argc, char **argv) {
    struct utmpx *entry;

    if (argc != 1) {
        tool_write_usage(argv[0], "");
        return 1;
    }

    setutxent();
    while ((entry = getutxent()) != 0) {
        if (entry->ut_type == USER_PROCESS && entry->ut_user[0] != '\0') {
            char user[sizeof(entry->ut_user) + 1];
            char line[sizeof(entry->ut_line) + 1];
            char host[sizeof(entry->ut_host) + 1];
            char time_text[64];
            time_t when = (time_t)entry->ut_tv.tv_sec;
            struct tm *local = localtime(&when);

            copy_field(user, sizeof(user), entry->ut_user, sizeof(entry->ut_user));
            copy_field(line, sizeof(line), entry->ut_line, sizeof(entry->ut_line));
            copy_field(host, sizeof(host), entry->ut_host, sizeof(entry->ut_host));

            if (local != 0 && strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M", local) > 0) {
                rt_write_cstr(1, user);
                rt_write_char(1, ' ');
                rt_write_cstr(1, line);
                rt_write_char(1, ' ');
                rt_write_cstr(1, time_text);
            } else {
                rt_write_cstr(1, user);
                rt_write_char(1, ' ');
                rt_write_cstr(1, line);
            }

            if (host[0] != '\0') {
                rt_write_cstr(1, " (");
                rt_write_cstr(1, host);
                rt_write_char(1, ')');
            }

            rt_write_char(1, '\n');
        }
    }
    endutxent();

    return 0;
}
