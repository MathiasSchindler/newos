#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[file ...]");
}

int main(int argc, char **argv) {
    int exit_code = 0;
    int i;

    if (argc == 1) {
        return platform_sync_all() == 0 ? 0 : 1;
    }

    if (argc > 1 && rt_strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    for (i = 1; i < argc; ++i) {
        if (platform_sync_path(argv[i]) != 0) {
            tool_write_error("sync", "cannot sync ", argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
