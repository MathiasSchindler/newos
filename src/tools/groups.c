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

static void write_primary_group(const PlatformIdentity *identity, int numeric) {
    PlatformGroupEntry entry;
    entry.gid = identity->gid;
    rt_copy_string(entry.name, sizeof(entry.name), identity->groupname);
    write_group_name(&entry, numeric);
}

static int print_groups_for_user(const char *requested_name, int include_name, int numeric, int primary_only) {
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

    if (primary_only) {
        write_primary_group(&identity, numeric);
    } else {
        for (i = 0; i < count; ++i) {
            if (!first) {
                rt_write_char(1, ' ');
            }
            write_group_name(&entries[i], numeric);
            first = 0;
        }
    }
    rt_write_char(1, '\n');
    return 0;
}

int main(int argc, char **argv) {
    int i;
    int exit_code = 0;
    int numeric = 0;
    int primary_only = 0;
    int prefix_names = 0;
    int first_user = argc;
    int user_count = 0;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        size_t j;

        if (arg[0] != '-' || arg[1] == '\0') {
            first_user = i;
            break;
        }

        for (j = 1; arg[j] != '\0'; ++j) {
            if (arg[j] == 'n') {
                numeric = 1;
            } else if (arg[j] == 'd') {
                primary_only = 1;
            } else if (arg[j] == 'p') {
                prefix_names = 1;
            } else {
                tool_write_usage(argv[0], "[-n] [-d] [-p] [USER ...]");
                return 1;
            }
        }
    }

    if (argc == 1 || first_user >= argc) {
        return print_groups_for_user(0, prefix_names, numeric, primary_only);
    }

    user_count = argc - first_user;
    for (i = first_user; i < argc; ++i) {
        if (print_groups_for_user(argv[i], prefix_names || user_count > 1, numeric, primary_only) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
