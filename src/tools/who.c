#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if __STDC_HOSTED__
#include <time.h>
#include <utmpx.h>
#endif

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
#if __STDC_HOSTED__
    struct utmpx *entry;
    int quick = 0;
    unsigned int count = 0;

    if (argc == 2 && rt_strcmp(argv[1], "-q") == 0) {
        quick = 1;
    } else if (argc != 1) {
        tool_write_usage(argv[0], "[-q]");
        return 1;
    }

    setutxent();
    while ((entry = getutxent()) != 0) {
        if (entry->ut_type == USER_PROCESS && entry->ut_user[0] != '\0') {
            char user[sizeof(entry->ut_user) + 1];
            count += 1;

            copy_field(user, sizeof(user), entry->ut_user, sizeof(entry->ut_user));
            if (quick) {
                if (count > 1) {
                    rt_write_char(1, ' ');
                }
                rt_write_cstr(1, user);
                continue;
            }

            char line[sizeof(entry->ut_line) + 1];
            char host[sizeof(entry->ut_host) + 1];
            char time_text[64];
            time_t when = (time_t)entry->ut_tv.tv_sec;
            struct tm *local = localtime(&when);

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

    if (quick) {
        rt_write_char(1, '\n');
        rt_write_cstr(1, "# users=");
        rt_write_uint(1, count);
        rt_write_char(1, '\n');
    }

    return 0;
#else
    PlatformIdentity identity;

    if (argc == 2 && rt_strcmp(argv[1], "-q") == 0) {
        if (platform_get_identity(&identity) != 0) {
            tool_write_error("who", "user information unavailable", 0);
            return 1;
        }
        rt_write_line(1, identity.username[0] != '\0' ? identity.username : "unknown");
        rt_write_line(1, "# users=1");
        return 0;
    }

    if (argc != 1) {
        tool_write_usage(argv[0], "[-q]");
        return 1;
    }

    if (platform_get_identity(&identity) != 0) {
        tool_write_error("who", "user information unavailable", 0);
        return 1;
    }

    rt_write_line(1, identity.username[0] != '\0' ? identity.username : "unknown");
    return 0;
#endif
}
