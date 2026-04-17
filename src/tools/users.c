#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    char name[256];
} UserEntry;

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
    PlatformSessionEntry sessions[128];
    UserEntry entries[256];
    size_t count = 0;
    size_t session_count = 0;
    size_t display_count;
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

    if (platform_list_sessions(sessions, sizeof(sessions) / sizeof(sessions[0]), &session_count) != 0) {
        tool_write_error("users", "user information unavailable", 0);
        return 1;
    }

    display_count = session_count < sizeof(sessions) / sizeof(sessions[0]) ? session_count : sizeof(sessions) / sizeof(sessions[0]);

    for (i = 0; i < display_count && count < sizeof(entries) / sizeof(entries[0]); ++i) {
        if (sessions[i].username[0] != '\0') {
            rt_copy_string(entries[count].name, sizeof(entries[count].name), sessions[i].username);
            count += 1;
        }
    }

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
}
