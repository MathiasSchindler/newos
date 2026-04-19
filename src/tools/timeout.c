#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static int starts_with(const char *text, const char *prefix) {
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }

    return 1;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[--preserve-status] [-s SIGNAL|--signal SIGNAL] [-k DURATION|--kill-after DURATION] DURATION COMMAND [ARG ...]");
}

int main(int argc, char **argv) {
    unsigned long long timeout_milliseconds = 0;
    unsigned long long kill_after_milliseconds = 0;
    int signal_number = 15;
    int preserve_status = 0;
    int argi = 1;
    int pid;
    int status = 0;

    while (argi < argc) {
        const char *arg = argv[argi];

        if (rt_strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(arg, "--preserve-status") == 0) {
            preserve_status = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-s") == 0 || rt_strcmp(arg, "--signal") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 125;
            }
            if (tool_parse_signal_name(argv[argi + 1], &signal_number) != 0) {
                tool_write_error("timeout", "invalid signal: ", argv[argi + 1]);
                return 125;
            }
            argi += 2;
            continue;
        }
        if (starts_with(arg, "--signal=")) {
            if (tool_parse_signal_name(arg + 9, &signal_number) != 0) {
                tool_write_error("timeout", "invalid signal: ", arg + 9);
                return 125;
            }
            argi += 1;
            continue;
        }
        if (arg[0] == '-' && arg[1] == 's' && arg[2] != '\0') {
            if (tool_parse_signal_name(arg + 2, &signal_number) != 0) {
                tool_write_error("timeout", "invalid signal: ", arg + 2);
                return 125;
            }
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-k") == 0 || rt_strcmp(arg, "--kill-after") == 0) {
            if (argi + 1 >= argc || tool_parse_duration_ms(argv[argi + 1], &kill_after_milliseconds) != 0) {
                print_usage(argv[0]);
                return 125;
            }
            argi += 2;
            continue;
        }
        if (starts_with(arg, "--kill-after=")) {
            if (tool_parse_duration_ms(arg + 13, &kill_after_milliseconds) != 0) {
                print_usage(argv[0]);
                return 125;
            }
            argi += 1;
            continue;
        }
        if (arg[0] == '-' && arg[1] == 'k' && arg[2] != '\0') {
            if (tool_parse_duration_ms(arg + 2, &kill_after_milliseconds) != 0) {
                print_usage(argv[0]);
                return 125;
            }
            argi += 1;
            continue;
        }
        break;
    }

    if (argc - argi < 2 || tool_parse_duration_ms(argv[argi], &timeout_milliseconds) != 0) {
        print_usage(argv[0]);
        return 125;
    }
    argi += 1;

    if (platform_spawn_process(&argv[argi], -1, -1, 0, 0, 0, &pid) != 0) {
        tool_write_error("timeout", "failed to execute ", argv[argi]);
        return 127;
    }

    if (platform_wait_process_timeout(
            pid,
            timeout_milliseconds,
            kill_after_milliseconds,
            signal_number,
            preserve_status,
            &status
        ) != 0) {
        tool_write_error("timeout", "wait failed", 0);
        return 125;
    }
    return status;
}
