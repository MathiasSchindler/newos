#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-d] [-v] [file ...]");
}

int main(int argc, char **argv) {
    int exit_code = 0;
    int data_only = 0;
    int verbose = 0;
    int path_count = 0;
    int i;

    if (argc == 1) {
        return platform_sync_all() == 0 ? 0 : 1;
    }

    for (i = 1; i < argc; ++i) {
        int result;

        if (rt_strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (rt_strcmp(argv[i], "-d") == 0 || rt_strcmp(argv[i], "--data") == 0) {
            data_only = 1;
            continue;
        }
        if (rt_strcmp(argv[i], "-v") == 0 || rt_strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            print_usage(argv[0]);
            return 1;
        }

        path_count += 1;
        result = data_only ? platform_sync_path_data(argv[i]) : platform_sync_path(argv[i]);
        if (result != 0) {
            tool_write_error("sync", "cannot sync ", argv[i]);
            exit_code = 1;
        } else if (verbose) {
            rt_write_cstr(1, "synced ");
            rt_write_cstr(1, argv[i]);
            rt_write_char(1, '\n');
        }
    }

    if (path_count == 0) {
        if (data_only || verbose) {
            print_usage(argv[0]);
            return 1;
        }
        return platform_sync_all() == 0 ? 0 : 1;
    }

    return exit_code;
}
