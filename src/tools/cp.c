#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define CP_PATH_CAPACITY 1024

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " source destination");
}

int main(int argc, char **argv) {
    char target_path[CP_PATH_CAPACITY];

    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (tool_resolve_destination(argv[1], argv[2], target_path, sizeof(target_path)) != 0) {
        rt_write_line(2, "cp: destination path too long");
        return 1;
    }

    if (rt_strcmp(argv[1], target_path) == 0) {
        rt_write_line(2, "cp: source and destination are the same");
        return 1;
    }

    if (tool_copy_file(argv[1], target_path) != 0) {
        rt_write_cstr(2, "cp: failed to copy ");
        rt_write_cstr(2, argv[1]);
        rt_write_cstr(2, " to ");
        rt_write_line(2, target_path);
        return 1;
    }

    return 0;
}
