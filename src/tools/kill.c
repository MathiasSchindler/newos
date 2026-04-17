#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int main(int argc, char **argv) {
    int argi = 1;
    int signal_number = 15;
    int exit_code = 0;

    if (argc < 2) {
        tool_write_usage("kill", "[-l [SIGNAL]] [-s SIGNAL | -SIGNAL] PID...");
        return 1;
    }

    if (rt_strcmp(argv[argi], "-l") == 0) {
        if (argi + 1 >= argc) {
            tool_write_signal_list(1);
            return 0;
        }

        for (argi += 1; argi < argc; ++argi) {
            int value = 0;
            if (tool_parse_signal_name(argv[argi], &value) != 0) {
                tool_write_error("kill", "unknown signal ", argv[argi]);
                exit_code = 1;
                continue;
            }
            if (rt_write_line(1, tool_signal_name(value)) != 0) {
                return 1;
            }
        }
        return exit_code;
    }

    if (rt_strcmp(argv[argi], "-s") == 0) {
        if (argi + 1 >= argc || tool_parse_signal_name(argv[argi + 1], &signal_number) != 0) {
            tool_write_error("kill", "unknown signal ", argi + 1 < argc ? argv[argi + 1] : "");
            return 1;
        }
        argi += 2;
    } else if (argv[argi][0] == '-' && argv[argi][1] != '\0' &&
               rt_strcmp(argv[argi], "--") != 0) {
        if (tool_parse_signal_name(argv[argi] + 1, &signal_number) != 0) {
            tool_write_error("kill", "unknown signal ", argv[argi] + 1);
            return 1;
        }
        argi += 1;
    }

    if (argi < argc && rt_strcmp(argv[argi], "--") == 0) {
        argi += 1;
    }

    if (argi >= argc) {
        tool_write_usage("kill", "[-l [SIGNAL]] [-s SIGNAL | -SIGNAL] PID...");
        return 1;
    }

    for (; argi < argc; ++argi) {
        long long pid_value = 0;
        if (tool_parse_int_arg(argv[argi], &pid_value, "kill", "pid") != 0) {
            exit_code = 1;
            continue;
        }
        if (platform_send_signal((int)pid_value, signal_number) != 0) {
            tool_write_error("kill", "cannot signal ", argv[argi]);
            exit_code = 1;
        }
    }

    return exit_code;
}
