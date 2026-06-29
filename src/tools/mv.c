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

static int move_one_path(const char *source_path, const char *dest_path, const MvOptions *options) {
    char target_path[MV_PATH_CAPACITY];
    char path_scratch[MV_PATH_CAPACITY * 2U];
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

    if (source_is_dir && tool_path_is_same_or_child(target_path, source_path, path_scratch, sizeof(path_scratch))) {
        rt_write_cstr(2, "mv: cannot move directory into itself: ");
        rt_write_line(2, source_path);
        return 1;
    }

    if (!tool_should_replace_path(source_path, target_path, source_is_dir, options->no_clobber, options->update_only, options->interactive, "mv: overwrite ")) {
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
