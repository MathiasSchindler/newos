#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define UMOUNT_USAGE "[-flv] TARGET..."

static void print_help(const char *program_name) {
    rt_write_cstr(1, "Usage: ");
    rt_write_cstr(1, program_name);
    rt_write_cstr(1, " " UMOUNT_USAGE "\n");
    rt_write_line(1, "Unmount one or more targets. This is a small Linux-first umount implementation.");
}

int main(int argc, char **argv) {
    ToolOptState options;
    int force = 0;
    int lazy = 0;
    int verbose = 0;
    int parse_result;
    int exit_status = 0;

    tool_opt_init(&options, argc, argv, tool_base_name(argv[0]), UMOUNT_USAGE);

    while ((parse_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-f") == 0 || rt_strcmp(options.flag, "--force") == 0) {
            force = 1;
        } else if (rt_strcmp(options.flag, "-l") == 0 || rt_strcmp(options.flag, "--lazy") == 0) {
            lazy = 1;
        } else if (rt_strcmp(options.flag, "-v") == 0 || rt_strcmp(options.flag, "--verbose") == 0) {
            verbose = 1;
        } else {
            tool_write_error("umount", "unknown option: ", options.flag);
            print_help(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        print_help(argv[0]);
        return 0;
    }
    if (options.argi >= argc) {
        print_help(argv[0]);
        return 1;
    }

    while (options.argi < argc) {
        const char *target = argv[options.argi++];

        if (platform_unmount_filesystem(target, force, lazy) != 0) {
            tool_write_error("umount", "unmount failed for ", target);
            exit_status = 1;
            continue;
        }
        if (verbose) {
            rt_write_cstr(1, "unmounted ");
            rt_write_line(1, target);
        }
    }

    return exit_status;
}
