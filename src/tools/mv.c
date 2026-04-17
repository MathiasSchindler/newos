#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MV_PATH_CAPACITY 1024

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " source destination");
}

static int source_is_directory(const char *path) {
    int is_directory = 0;
    return platform_path_is_directory(path, &is_directory) == 0 && is_directory;
}

static int move_one_path(const char *source_path, const char *dest_path) {
    char target_path[MV_PATH_CAPACITY];

    if (tool_resolve_destination(source_path, dest_path, target_path, sizeof(target_path)) != 0) {
        rt_write_line(2, "mv: destination path too long");
        return 1;
    }

    if (rt_strcmp(source_path, target_path) == 0) {
        return 0;
    }

    if (platform_rename_path(source_path, target_path) == 0) {
        return 0;
    }

    if (!source_is_directory(source_path) && tool_copy_file(source_path, target_path) == 0) {
        if (platform_remove_file(source_path) == 0) {
            return 0;
        }
        (void)platform_remove_file(target_path);
    }

    rt_write_cstr(2, "mv: failed to move ");
    rt_write_cstr(2, source_path);
    rt_write_cstr(2, " to ");
    rt_write_line(2, target_path);
    return 1;
}

int main(int argc, char **argv) {
    int i;
    int exit_code = 0;

    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc > 3) {
        int dest_is_directory = 0;
        if (platform_path_is_directory(argv[argc - 1], &dest_is_directory) != 0 || !dest_is_directory) {
            rt_write_line(2, "mv: target for multiple sources must be an existing directory");
            return 1;
        }
    }

    for (i = 1; i < argc - 1; ++i) {
        if (move_one_path(argv[i], argv[argc - 1]) != 0) {
            exit_code = 1;
        }
    }

    return exit_code_code = 0;

    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc > 3) {
        int dest_is_directory = 0;
        if (platform_path_is_directory(argv[argc - 1], &dest_is_directory) != 0 || !dest_is_directory) {
            rt_write_line(2, "mv: target for multiple sources must be an existing directory");
            return 1;
        }
    }

    for (i = 1; i < argc - 1; ++i) {
        if (move_one_path(argv[i], argv[argc - 1]) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
