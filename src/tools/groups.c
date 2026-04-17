#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void write_group_name(const PlatformGroupEntry *entry, int numeric) {
    if (!numeric && entry->name[0] != '\0') {
        rt_write_cstr(1, entry->name);
    } else {
        rt_write_uint(1, (unsigned long long)entry->gid);
    }
}

static int print_groups_for_user(const char *requested_name, int include_name, int numeric) {
    PlatformIdentity identity;
    PlatformGroupEntry entries[256];
    size_t count = 0;
    size_t i;
    int first = 1;

    if (platform_lookup_identity(requested_name, &identity) != 0) {
        if (requested_name != 0) {
            tool_write_error("groups", "unknown user ", requested_name);
        }
        return 1;
    }

    if (platform_list_groups_for_identity(&identity, entries, sizeof(entries) / sizeof(entries[0]), &count) != 0) {
        tool_write_error("groups", "cannot inspect groups for ", identity.username);
        return 1;
    }

    if (include_name) {
        rt_write_cstr(1, identity.username);
        rt_write_cstr(1, " : ");
    }

    for (i = 0; i < count; ++i) {
        if (!first) {
            rt_write_char(1, ' ');
        }
        write_group_name(&entries[i], numeric);
        first = 0;
    }
    rt_write_char(1, '\n');
    return 0;
}

int main(int argc, char **argv) {
    int i;
    int exit_code = 0;
    int numeric = 0;
    int first_user = argc;

    for (i = 1; i < argc; ++i) {
        if (rt_strcmp(argv[i], "-n") == 0) {
            numeric = 1;
        } else {
            first_user = i;
            break;
        }
    }

    if (argc == 1 || first_user >= argc) {
        return print_groups_for_user(0, 0, numeric);
    }

    for (i = first_user; i < argc; ++i) {
        if (rt_strcmp(argv[i], "-n") == 0) {
            continue;
        }
        if (print_groups_for_user(argv[i], 1, numeric) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
