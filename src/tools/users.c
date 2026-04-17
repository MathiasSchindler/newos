#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if __STDC_HOSTED__
#include <stdlib.h>
#include <utmpx.h>
#endif

typedef struct {
    char name[256];
} UserEntry;

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

static int compare_users(const void *lhs, const void *rhs) {
    const UserEntry *left = (const UserEntry *)lhs;
    const UserEntry *right = (const UserEntry *)rhs;
    return rt_strcmp(left->name, right->name);
}

static void sort_users(UserEntry *entries, size_t count) {
    size_t i;
    size_t j;

    for (i = 0; i < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            if (compare_users(&entries[i], &entries[j]) > 0) {
                UserEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
}

int main(int argc, char **argv) {
#if __STDC_HOSTED__
    struct utmpx *entry;
    UserEntry entries[256];
    size_t count = 0;
    size_t i;
    int sort_output = 0;
    int unique_output = 0;
    int first = 1;

    for (i = 1; i < (size_t)argc; ++i) {
        if (rt_strcmp(argv[i], "-s") == 0) {
            sort_output = 1;
        } else if (rt_strcmp(argv[i], "-u") == 0) {
            unique_output = 1;
        } else {
            tool_write_usage(argv[0], "[-s] [-u]");
            return 1;
        }
    }

    setutxent();
    while ((entry = getutxent()) != 0) {
        if (entry->ut_type == USER_PROCESS && entry->ut_user[0] != '\0' && count < sizeof(entries) / sizeof(entries[0])) {
            copy_field(entries[count].name, sizeof(entries[count].name), entry->ut_user, sizeof(entry->ut_user));
            count += 1;
        }
    }
    endutxent();

    if (unique_output) {
        sort_output = 1;
    }

    if (sort_output && count > 1U) {
        sort_users(entries, count);
    }

    for (i = 0; i < count; ++i) {
        if (unique_output && i > 0 && rt_strcmp(entries[i - 1].name, entries[i].name) == 0) {
            continue;
        }
        if (!first) {
            rt_write_char(1, ' ');
        }
        rt_write_cstr(1, entries[i].name);
        first = 0;
    }
    rt_write_char(1, '\n');
    return 0;
#else
    PlatformIdentity identity;
    size_t i;

    for (i = 1; i < (size_t)argc; ++i) {
        if (rt_strcmp(argv[i], "-s") != 0 && rt_strcmp(argv[i], "-u") != 0) {
            tool_write_usage(argv[0], "[-s] [-u]");
            return 1;
        }
    }

    if (platform_get_identity(&identity) != 0) {
        tool_write_error("users", "user information unavailable", 0);
        return 1;
    }

    rt_write_line(1, identity.username[0] != '\0' ? identity.username : "unknown");
    return 0;
#endif
}
