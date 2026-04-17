#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define CHOWN_ENTRY_CAPACITY 1024
#define CHOWN_PATH_CAPACITY 1024

typedef struct {
    int recursive;
    int no_dereference;
    unsigned int uid;
    unsigned int gid;
} ChownOptions;

static int resolve_owner_name(const char *text, unsigned int *uid_out, unsigned int *primary_gid_out, int *has_primary_gid_out) {
    PlatformIdentity identity;
    unsigned long long value = 0;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }

    if (rt_parse_uint(text, &value) == 0) {
        *uid_out = (unsigned int)value;
        if (has_primary_gid_out != 0) {
            *has_primary_gid_out = 0;
        }
        return 0;
    }

    if (platform_lookup_identity(text, &identity) != 0) {
        return -1;
    }

    *uid_out = identity.uid;
    if (primary_gid_out != 0) {
        *primary_gid_out = identity.gid;
    }
    if (has_primary_gid_out != 0) {
        *has_primary_gid_out = 1;
    }
    return 0;
}

static int resolve_group_name(const char *text, unsigned int *gid_out) {
    return platform_lookup_group(text, gid_out);
}

static int parse_owner_spec(const char *text, unsigned int *uid_out, unsigned int *gid_out) {
    char left[PLATFORM_NAME_CAPACITY];
    char right[PLATFORM_NAME_CAPACITY];
    size_t i = 0U;
    size_t left_len = 0U;
    size_t right_len = 0U;
    char separator = '\0';
    unsigned int primary_gid = (unsigned int)-1;
    int has_primary_gid = 0;

    *uid_out = (unsigned int)-1;
    *gid_out = (unsigned int)-1;

    while (text[i] != '\0' && text[i] != ':' && text[i] != '.') {
        if (left_len + 1 < sizeof(left)) {
            left[left_len++] = text[i];
        }
        i += 1U;
    }
    left[left_len] = '\0';

    if (text[i] == ':' || text[i] == '.') {
        separator = text[i];
        i += 1U;
        while (text[i] != '\0') {
            if (right_len + 1 < sizeof(right)) {
                right[right_len++] = text[i];
            }
            i += 1U;
        }
    }
    right[right_len] = '\0';

    if (left_len > 0) {
        if (resolve_owner_name(left, uid_out, &primary_gid, &has_primary_gid) != 0) {
            return -1;
        }
    }

    if (right_len > 0) {
        if (resolve_group_name(right, gid_out) != 0) {
            return -1;
        }
    } else if (separator != '\0' && left_len > 0 && has_primary_gid) {
        *gid_out = primary_gid;
    }

    return (left_len == 0 && right_len == 0) ? -1 : 0;
}

static int chown_one_path(const char *path, const ChownOptions *options) {
    PlatformDirEntry entry;
    PlatformDirEntry entries[CHOWN_ENTRY_CAPACITY];
    size_t count = 0U;
    int is_directory = 0;

    if (platform_get_path_info(path, &entry) != 0) {
        return -1;
    }

    if (platform_change_owner_ex(path, options->uid, options->gid, options->no_dereference ? 0 : 1) != 0) {
        return -1;
    }

    if (options->recursive && entry.is_dir) {
        size_t i;

        if (platform_collect_entries(path, 1, entries, CHOWN_ENTRY_CAPACITY, &count, &is_directory) != 0 || !is_directory) {
            return -1;
        }

        for (i = 0U; i < count; ++i) {
            char child_path[CHOWN_PATH_CAPACITY];

            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
                continue;
            }

            if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0 ||
                chown_one_path(child_path, options) != 0) {
                platform_free_entries(entries, count);
                return -1;
            }
        }

        platform_free_entries(entries, count);
    }

    return 0;
}

int main(int argc, char **argv) {
    ChownOptions options;
    int argi = 1;
    int i;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));
    options.uid = (unsigned int)-1;
    options.gid = (unsigned int)-1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 'R') {
                options.recursive = 1;
            } else if (*flag == 'h') {
                options.no_dereference = 1;
            } else {
                tool_write_usage("chown", "[-R] [-h] OWNER[:GROUP] PATH...");
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (argc - argi < 2) {
        tool_write_usage("chown", "[-R] [-h] OWNER[:GROUP] PATH...");
        return 1;
    }

    if (parse_owner_spec(argv[argi], &options.uid, &options.gid) != 0) {
        tool_write_error("chown", "invalid owner spec ", argv[argi]);
        return 1;
    }

    for (i = argi + 1; i < argc; ++i) {
        if (chown_one_path(argv[i], &options) != 0) {
            tool_write_error("chown", "cannot change owner for ", argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
