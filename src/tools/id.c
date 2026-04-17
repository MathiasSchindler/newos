#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef enum {
    ID_MODE_FULL = 0,
    ID_MODE_UID,
    ID_MODE_GID,
    ID_MODE_GROUPS
} IdMode;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-u|-g|-G] [-n] [USER]");
}

static int print_group_list(const PlatformIdentity *identity, int names_only) {
    PlatformGroupEntry entries[256];
    size_t count = 0;
    size_t i;

    if (platform_list_groups_for_identity(identity, entries, sizeof(entries) / sizeof(entries[0]), &count) != 0) {
        return -1;
    }

    for (i = 0; i < count; ++i) {
        if (i > 0) {
            if (rt_write_char(1, ' ') != 0) {
                return -1;
            }
        }
        if (names_only && entries[i].name[0] != '\0') {
            if (rt_write_cstr(1, entries[i].name) != 0) {
                return -1;
            }
        } else if (rt_write_uint(1, (unsigned long long)entries[i].gid) != 0) {
            return -1;
        }
    }

    return 0;
}

static int write_identity_summary(const PlatformIdentity *identity) {
    if (rt_write_cstr(1, "uid=") != 0 ||
        rt_write_uint(1, identity->uid) != 0 ||
        rt_write_cstr(1, "(") != 0 ||
        rt_write_cstr(1, identity->username) != 0 ||
        rt_write_cstr(1, ") gid=") != 0 ||
        rt_write_uint(1, identity->gid) != 0 ||
        rt_write_cstr(1, "(") != 0 ||
        rt_write_cstr(1, identity->groupname) != 0 ||
        rt_write_cstr(1, ")") != 0) {
        return -1;
    }

    if (rt_write_cstr(1, " groups=") != 0) {
        return -1;
    }

    {
        PlatformGroupEntry entries[256];
        size_t count = 0;
        size_t i;

        if (platform_list_groups_for_identity(identity, entries, sizeof(entries) / sizeof(entries[0]), &count) != 0) {
            return rt_write_char(1, '\n');
        }

        for (i = 0; i < count; ++i) {
            if (i > 0 && rt_write_char(1, ',') != 0) {
                return -1;
            }
            if (rt_write_uint(1, (unsigned long long)entries[i].gid) != 0 ||
                rt_write_cstr(1, "(") != 0 ||
                rt_write_cstr(1, entries[i].name[0] != '\0' ? entries[i].name : "") != 0 ||
                rt_write_cstr(1, ")") != 0) {
                return -1;
            }
        }
    }

    return rt_write_char(1, '\n');
}

int main(int argc, char **argv) {
    PlatformIdentity identity;
    IdMode mode = ID_MODE_FULL;
    int names_only = 0;
    const char *requested_user = 0;
    int argi;

    for (argi = 1; argi < argc; ++argi) {
        const char *arg = argv[argi];
        size_t j = 0;

        if (rt_strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }

        if (arg[0] != '-' || arg[1] == '\0') {
            break;
        }

        for (j = 1; arg[j] != '\0'; ++j) {
            switch (arg[j]) {
                case 'u':
                    if (mode != ID_MODE_FULL && mode != ID_MODE_UID) {
                        print_usage(argv[0]);
                        return 1;
                    }
                    mode = ID_MODE_UID;
                    break;
                case 'g':
                    if (mode != ID_MODE_FULL && mode != ID_MODE_GID) {
                        print_usage(argv[0]);
                        return 1;
                    }
                    mode = ID_MODE_GID;
                    break;
                case 'G':
                    if (mode != ID_MODE_FULL && mode != ID_MODE_GROUPS) {
                        print_usage(argv[0]);
                        return 1;
                    }
                    mode = ID_MODE_GROUPS;
                    break;
                case 'n':
                    names_only = 1;
                    break;
                default:
                    print_usage(argv[0]);
                    return 1;
            }
        }
    }

    if (argi < argc) {
        requested_user = argv[argi];
        argi += 1;
    }
    if (argi != argc || (names_only && mode == ID_MODE_FULL)) {
        print_usage(argv[0]);
        return 1;
    }

    if ((requested_user != 0 && platform_lookup_identity(requested_user, &identity) != 0) ||
        (requested_user == 0 && platform_get_identity(&identity) != 0)) {
        tool_write_error("id", "unavailable", 0);
        return 1;
    }

    if (mode == ID_MODE_UID) {
        if (names_only) {
            return rt_write_line(1, identity.username) == 0 ? 0 : 1;
        }
        return rt_write_uint(1, identity.uid) == 0 && rt_write_char(1, '\n') == 0 ? 0 : 1;
    }

    if (mode == ID_MODE_GID) {
        if (names_only) {
            return rt_write_line(1, identity.groupname) == 0 ? 0 : 1;
        }
        return rt_write_uint(1, identity.gid) == 0 && rt_write_char(1, '\n') == 0 ? 0 : 1;
    }

    if (mode == ID_MODE_GROUPS) {
        if (print_group_list(&identity, names_only) != 0 || rt_write_char(1, '\n') != 0) {
            tool_write_error("id", "unavailable", 0);
            return 1;
        }
        return 0;
    }

    if (write_identity_summary(&identity) != 0) {
        tool_write_error("id", "failed to write output", 0);
        return 1;
    }

    return 0;
}
