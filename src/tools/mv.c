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

int main(int argc, char **argv) {
    char target_path[MV_PATH_CAPACITY];

    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (tool_resolve_destination(argv[1], argv[2], target_path, sizeof(target_path)) != 0) {
        rt_write_line(2, "mv: destination path too long");
        return 1;
    }

    if (rt_strcmp(argv[1], target_path) == 0) {
        return 0;
    }

    if (platform_rename_path(argv[1], target_path) == 0) {
        return 0;
    }

    if (!source_is_directory(argv[1]) && tool_copy_file(argv[1], target_path) == 0) {
        if (platform_remove_file(argv[1]) == 0) {
            return 0;
        }
        (void)platform_remove_file(target_path);
    }

    rt_write_cstr(2, "mv: failed to move ");
    rt_write_cstr(2, argv[1]);
    rt_write_cstr(2, " to ");
    rt_write_line(2, target_path);
    return 1;
}
