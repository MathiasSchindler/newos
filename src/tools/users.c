#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "tool_util.h"

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
    int first = 1;

    if (argc != 1) {
        tool_write_usage(argv[0], "");
        return 1;
    }

    setutxent();
    while ((entry = getutxent()) != 0) {
        if (entry->ut_type == USER_PROCESS && entry->ut_user[0] != '\0') {
            char user[sizeof(entry->ut_user) + 1];

            copy_field(user, sizeof(user), entry->ut_user, sizeof(entry->ut_user));
            if (!first) {
                rt_write_char(1, ' ');
            }
            rt_write_cstr(1, user);
            first = 0;
        }
    }
    endutxent();

    rt_write_char(1, '\n');
    return 0;
}
