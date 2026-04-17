#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int main(int argc, char **argv) {
    int argi = 1;
    unsigned long long signal_number = 15;
    int exit_code = 0;

    if (argc < 2) {
        tool_write_usage("kill", "[-SIGNAL] PID...");
        return 1;
    }

    if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (tool_parse_uint_arg(argv[argi] + 1, &signal_number, "kill", "signal") != 0) {
            return 1;
        }
        argi += 1;
    }

    if (argi >= argc) {
        tool_write_usage("kill", "[-SIGNAL] PID...");
        return 1;
    }

    for (; argi < argc; ++argi) {
        unsigned long long pid_value = 0;
        if (tool_parse_uint_arg(argv[argi], &pid_value, "kill", "pid") != 0) {
            exit_code = 1;
            continue;
        }
        if (platform_send_signal((int)pid_value, (int)signal_number) != 0) {
            tool_write_error("kill", "cannot signal ", argv[argi]);
            exit_code = 1;
        }
    }

    return exit_code;
}
