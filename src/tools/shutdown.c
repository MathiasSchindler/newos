#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-r|-H|-P|-p] [now]");
}

static int is_immediate_time_spec(const char *text) {
    return rt_strcmp(text, "now") == 0 ||
           rt_strcmp(text, "0") == 0 ||
           rt_strcmp(text, "+0") == 0;
}

static const char *action_name(int action) {
    if (action == PLATFORM_SHUTDOWN_REBOOT) {
        return "reboot";
    }
    if (action == PLATFORM_SHUTDOWN_HALT) {
        return "halt";
    }
    return "power off";
}

int main(int argc, char **argv) {
    int action = PLATFORM_SHUTDOWN_POWEROFF;
    int saw_time = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (rt_strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            rt_write_line(1, "Request an immediate system halt, power-off, or reboot.");
            return 0;
        }

        if (rt_strcmp(arg, "-r") == 0 || rt_strcmp(arg, "--reboot") == 0) {
            action = PLATFORM_SHUTDOWN_REBOOT;
            continue;
        }
        if (rt_strcmp(arg, "-H") == 0 || rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--halt") == 0) {
            action = PLATFORM_SHUTDOWN_HALT;
            continue;
        }
        if (rt_strcmp(arg, "-P") == 0 || rt_strcmp(arg, "-p") == 0 || rt_strcmp(arg, "--poweroff") == 0) {
            action = PLATFORM_SHUTDOWN_POWEROFF;
            continue;
        }

        if (arg[0] == '-' && arg[1] != '\0') {
            tool_write_error("shutdown", "unknown option: ", arg);
            print_usage(argv[0]);
            return 1;
        }

        if (!saw_time) {
            if (!is_immediate_time_spec(arg)) {
                tool_write_error("shutdown", "only immediate shutdown is supported: ", arg);
                return 1;
            }
            saw_time = 1;
            continue;
        }

        /* Optional message text is accepted but currently ignored. */
    }

    rt_write_cstr(1, "shutdown: requesting ");
    rt_write_cstr(1, action_name(action));
    rt_write_line(1, " now");

    (void)platform_sync_all();
    if (platform_shutdown_system(action) != 0) {
        tool_write_error("shutdown", "failed to change system power state", 0);
        return 1;
    }

    return 0;
}
