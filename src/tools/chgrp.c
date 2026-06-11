#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define CHGRP_ENTRY_CAPACITY 1024
#define CHGRP_PATH_CAPACITY 1024

typedef struct {
    int recursive;
    int no_dereference;
    int use_reference;
    const char *reference_path;
    unsigned int gid;
} ChgrpOptions;

static void print_usage(void) {
    tool_write_usage("chgrp", "[-R] [-h] [--reference=FILE | GROUP] PATH...");
}

static int load_reference_group(const ChgrpOptions *options, unsigned int *gid_out) {
    PlatformDirEntry entry;

    if ((options->no_dereference
             ? platform_get_path_info(options->reference_path, &entry)
             : platform_get_path_info_follow(options->reference_path, &entry)) != 0) {
        return -1;
    }

    *gid_out = entry.gid;
    return 0;
}

static int chgrp_one_path(const char *path, const ChgrpOptions *options) {
    PlatformDirEntry entry;
    PlatformDirEntry entries[CHGRP_ENTRY_CAPACITY];
    size_t count = 0U;
    int is_directory = 0;

    if (platform_get_path_info(path, &entry) != 0) {
        return -1;
    }
    if (platform_change_owner_ex(path, (unsigned int)-1, options->gid, options->no_dereference ? 0 : 1) != 0) {
        return -1;
    }

    if (options->recursive && entry.is_dir) {
        size_t i;

        if (platform_collect_entries(path, 1, entries, CHGRP_ENTRY_CAPACITY, &count, &is_directory) != 0 || !is_directory) {
            return -1;
        }

        for (i = 0U; i < count; ++i) {
            char child_path[CHGRP_PATH_CAPACITY];

            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
                continue;
            }
            if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0 ||
                chgrp_one_path(child_path, options) != 0) {
                platform_free_entries(entries, count);
                return -1;
            }
        }
        platform_free_entries(entries, count);
    }

    return 0;
}

int main(int argc, char **argv) {
    ChgrpOptions options;
    int argi = 1;
    int i;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));
    options.gid = (unsigned int)-1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *arg = argv[argi];

        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        }
        if (rt_strcmp(arg, "--recursive") == 0) {
            options.recursive = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "--no-dereference") == 0) {
            options.no_dereference = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "--dereference") == 0) {
            options.no_dereference = 0;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "--reference") == 0) {
            if (argi + 1 >= argc) {
                print_usage();
                return 1;
            }
            options.use_reference = 1;
            options.reference_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (tool_starts_with(arg, "--reference=")) {
            options.use_reference = 1;
            options.reference_path = arg + 12;
            if (options.reference_path[0] == '\0') {
                print_usage();
                return 1;
            }
            argi += 1;
            continue;
        }

        {
            const char *flag = arg + 1;

            while (*flag != '\0') {
                if (*flag == 'R') {
                    options.recursive = 1;
                } else if (*flag == 'h') {
                    options.no_dereference = 1;
                } else {
                    print_usage();
                    return 1;
                }
                flag += 1;
            }
        }
        argi += 1;
    }

    if ((!options.use_reference && argc - argi < 2) || (options.use_reference && argc - argi < 1)) {
        print_usage();
        return 1;
    }

    if (options.use_reference) {
        if (load_reference_group(&options, &options.gid) != 0) {
            tool_write_error("chgrp", "cannot read reference ", options.reference_path);
            return 1;
        }
    } else {
        if (tool_resolve_group_id(argv[argi], &options.gid) != 0) {
            tool_write_error("chgrp", "invalid group ", argv[argi]);
            return 1;
        }
        argi += 1;
    }

    for (i = argi; i < argc; ++i) {
        if (chgrp_one_path(argv[i], &options) != 0) {
            tool_write_error("chgrp", "cannot change group for ", argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
