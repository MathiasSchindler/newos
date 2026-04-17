#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define RM_MAX_ENTRIES 1024
#define RM_PATH_CAPACITY 1024

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-f] [-i] [-v] [-d] [-r] [--preserve-root|--no-preserve-root] path ...");
}

static void print_removed(const char *path, int is_directory) {
    rt_write_cstr(1, "removed ");
    if (is_directory) {
        rt_write_cstr(1, "directory ");
    }
    rt_write_line(1, path);
}

static int path_is_protected_name(const char *path) {
    const char *base = tool_base_name(path);
    return rt_strcmp(base, ".") == 0 || rt_strcmp(base, "..") == 0;
}

static int remove_path(const char *path, int recursive, int force, int interactive, int verbose, int allow_empty_dir) {
    PlatformDirEntry entries[RM_MAX_ENTRIES];
    size_t count = 0;
    int is_directory = 0;
    size_t i;

    if (platform_collect_entries(path, 1, entries, RM_MAX_ENTRIES, &count, &is_directory) != 0) {
        return force ? 0 : -1;
    }

    if (interactive && !tool_prompt_yes_no("rm: remove ", path)) {
        platform_free_entries(entries, count);
        return 0;
    }

    if (!is_directory) {
        if (platform_remove_file(path) != 0) {
            platform_free_entries(entries, count);
            return force ? 0 : -1;
        }
        if (verbose) {
            print_removed(path, 0);
        }
        platform_free_entries(entries, count);
        return 0;
    }

    if (allow_empty_dir && !recursive) {
        if (platform_remove_directory(path) != 0) {
            platform_free_entries(entries, count);
            return force ? 0 : -1;
        }
        if (verbose) {
            print_removed(path, 1);
        }
        platform_free_entries(entries, count);
        return 0;
    }

    if (!recursive) {
        platform_free_entries(entries, count);
        return -2;
    }

    for (i = 0; i < count; ++i) {
        char child_path[RM_PATH_CAPACITY];

        if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
            continue;
        }

        if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0) {
            platform_free_entries(entries, count);
            return force ? 0 : -1;
        }

        if (remove_path(child_path, recursive, force, interactive, verbose, allow_empty_dir) != 0 && !force) {
            platform_free_entries(entries, count);
            return -1;
        }
    }

    if (platform_remove_directory(path) != 0) {
        platform_free_entries(entries, count);
        return force ? 0 : -1;
    }

    if (verbose) {
        print_removed(path, 1);
    }

    platform_free_entries(entries, count);
    return 0;
}

int main(int argc, char **argv) {
    int recursive = 0;
    int force = 0;
    int interactive = 0;
    int verbose = 0;
    int allow_empty_dir = 0;
    int preserve_root = 1;
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

        if (rt_strcmp(arg, "--preserve-root") == 0) {
            preserve_root = 1;
            first_path_index = i + 1;
            continue;
        }

        if (rt_strcmp(arg, "--no-preserve-root") == 0) {
            preserve_root = 0;
            first_path_index = i + 1;
            continue;
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
                interactive = 0;
            } else if (arg[j] == 'i') {
                interactive = 1;
                force = 0;
            } else if (arg[j] == 'v') {
                verbose = 1;
            } else if (arg[j] == 'd') {
                allow_empty_dir = 1;
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
        if (path_is_protected_name(argv[i])) {
            rt_write_cstr(2, "rm: refusing to remove ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (recursive && preserve_root && tool_path_is_root(argv[i])) {
            rt_write_line(2, "rm: refusing to remove root directory '/'");
            exit_code = 1;
            continue;
        }

        int result = remove_path(argv[i], recursive, force, interactive, verbose, allow_empty_dir);

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
