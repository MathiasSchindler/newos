#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define RM_MAX_ENTRIES 1024
#define RM_PATH_CAPACITY 1024

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-f] [-r] path ...");
}

static int remove_path(const char *path, int recursive, int force) {
    PlatformDirEntry entries[RM_MAX_ENTRIES];
    size_t count = 0;
    int is_directory = 0;
    size_t i;

    if (platform_collect_entries(path, 1, entries, RM_MAX_ENTRIES, &count, &is_directory) != 0) {
        return force ? 0 : -1;
    }

    if (!is_directory) {
        if (platform_remove_file(path) != 0) {
            return force ? 0 : -1;
        }
        return 0;
    }

    if (!recursive) {
        return -2;
    }

    for (i = 0; i < count; ++i) {
        char child_path[RM_PATH_CAPACITY];

        if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
            continue;
        }

        if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0) {
            return force ? 0 : -1;
        }

        if (remove_path(child_path, recursive, force) != 0 && !force) {
            return -1;
        }
    }

    if (platform_remove_directory(path) != 0) {
        return force ? 0 : -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    int recursive = 0;
    int force = 0;
    int first_path_index = 1;
    int exit_code = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        size_t j;

        if (rt_strcmp(arg, "--") == 0) {
            first_path_index = i + 1;
            break;
        }

        if (arg[0] != '-' || arg[1] == '\0') {
            first_path_index = i;
            break;
        }

        for (j = 1; arg[j] != '\0'; ++j) {
            if (arg[j] == 'r' || arg[j] == 'R') {
                recursive = 1;
            } else if (arg[j] == 'f') {
                force = 1;
            } else {
                print_usage(argv[0]);
                return 1;
            }
        }

        first_path_index = i + 1;
    }

    if (first_path_index >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    for (i = first_path_index; i < argc; ++i) {
        int result = remove_path(argv[i], recursive, force);

        if (result == -2) {
            rt_write_cstr(2, "rm: cannot remove directory without -r: ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        } else if (result != 0) {
            rt_write_cstr(2, "rm: cannot remove ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
