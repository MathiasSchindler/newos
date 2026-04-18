#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MV_PATH_CAPACITY 2048

typedef struct {
    int interactive;
    int no_clobber;
    int update_only;
    int verbose;
} MvOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-i] [-f] [-n] [-u] [-v] source... destination");
}

static int path_is_same_or_child(const char *path, const char *prefix) {
    char normalized_path[MV_PATH_CAPACITY];
    char normalized_prefix[MV_PATH_CAPACITY];
    size_t i = 0U;

    if (tool_canonicalize_path(path, 0, 1, normalized_path, sizeof(normalized_path)) != 0) {
        rt_copy_string(normalized_path, sizeof(normalized_path), path);
    }
    if (tool_canonicalize_path(prefix, 0, 1, normalized_prefix, sizeof(normalized_prefix)) != 0) {
        rt_copy_string(normalized_prefix, sizeof(normalized_prefix), prefix);
    }

    if (normalized_prefix[0] == '/' && normalized_prefix[1] == '\0') {
        return 1;
    }

    while (normalized_prefix[i] != '\0' && normalized_path[i] == normalized_prefix[i]) {
        i += 1U;
    }
    if (normalized_prefix[i] != '\0') {
        return 0;
    }
    return normalized_path[i] == '\0' || normalized_path[i] == '/';
}

static int should_replace(const char *source_path, const char *target_path, int source_is_dir, const MvOptions *options) {
    PlatformDirEntry source_info;
    PlatformDirEntry target_info;

    if (!tool_path_exists(target_path)) {
        return 1;
    }
    if (options->no_clobber) {
        return 0;
    }
    if (options->update_only &&
        !source_is_dir &&
        platform_get_path_info(source_path, &source_info) == 0 &&
        platform_get_path_info(target_path, &target_info) == 0 &&
        source_info.mtime <= target_info.mtime) {
        return 0;
    }
    if (options->interactive) {
        return tool_prompt_yes_no("mv: overwrite ", target_path);
    }
    return 1;
}

static int move_one_path(const char *source_path, const char *dest_path, const MvOptions *options) {
    char target_path[MV_PATH_CAPACITY];
    int source_is_dir;
    int path_status;

    if (tool_resolve_destination(source_path, dest_path, target_path, sizeof(target_path)) != 0) {
        rt_write_line(2, "mv: destination path too long");
        return 1;
    }

    if (tool_paths_equal(source_path, target_path)) {
        return 0;
    }

    path_status = platform_path_is_directory(source_path, &source_is_dir);
    if (path_status != 0) {
        rt_write_cstr(2, "mv: cannot access ");
        rt_write_line(2, source_path);
        return 1;
    }

    if (source_is_dir && path_is_same_or_child(target_path, source_path)) {
        rt_write_cstr(2, "mv: cannot move directory into itself: ");
        rt_write_line(2, source_path);
        return 1;
    }

    if (!should_replace(source_path, target_path, source_is_dir, options)) {
        return 0;
    }

    if (platform_rename_path(source_path, target_path) == 0) {
        if (options->verbose) {
            rt_write_cstr(1, source_path);
            rt_write_cstr(1, " -> ");
            rt_write_line(1, target_path);
        }
        return 0;
    }

    if (tool_copy_path(source_path, target_path, 1, 1, 1) == 0) {
        if (tool_remove_path(source_path, 1) == 0) {
            if (options->verbose) {
                rt_write_cstr(1, source_path);
                rt_write_cstr(1, " -> ");
                rt_write_line(1, target_path);
            }
            return 0;
        }
    }

    rt_write_cstr(2, "mv: failed to move ");
    rt_write_cstr(2, source_path);
    rt_write_cstr(2, " to ");
    rt_write_line(2, target_path);
    return 1;
}

int main(int argc, char **argv) {
    MvOptions options;
    int argi = 1;
    int i;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 'i') {
                options.interactive = 1;
                options.no_clobber = 0;
            } else if (*flag == 'f') {
                options.interactive = 0;
                options.no_clobber = 0;
            } else if (*flag == 'n') {
                options.no_clobber = 1;
                options.interactive = 0;
            } else if (*flag == 'u') {
                options.update_only = 1;
            } else if (*flag == 'v') {
                options.verbose = 1;
            } else {
                print_usage(argv[0]);
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (argc - argi < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc - argi > 2) {
        int dest_is_directory = 0;
        if (platform_path_is_directory(argv[argc - 1], &dest_is_directory) != 0 || !dest_is_directory) {
            rt_write_line(2, "mv: target for multiple sources must be an existing directory");
            return 1;
        }
    }

    for (i = argi; i < argc - 1; ++i) {
        if (move_one_path(argv[i], argv[argc - 1], &options) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
