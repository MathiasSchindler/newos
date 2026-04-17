#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(void) {
    tool_write_usage("kill", "[-l [SIGNAL]] [-s SIGNAL | --signal SIGNAL | -SIGNAL] [--] PID...");
}

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

static int parse_signed_value(const char *text, long long *value_out) {
    unsigned long long magnitude = 0;
    int negative = 0;

    if (text == 0 || value_out == 0 || text[0] == '\0') {
        return -1;
    }

    if (text[0] == '-') {
        negative = 1;
        text += 1;
    } else if (text[0] == '+') {
        text += 1;
    }

    if (text[0] == '\0' || rt_parse_uint(text, &magnitude) != 0) {
        return -1;
    }

    *value_out = negative ? -(long long)magnitude : (long long)magnitude;
    return 0;
}

static int write_signal_lookup(const char *text) {
    long long numeric = 0;
    int signal_number = 0;
    const char *signal_name;

    if (parse_signed_value(text, &numeric) == 0) {
        if (numeric < 0) {
            numeric = -numeric;
        }
        if (numeric == 0) {
            return rt_write_line(1, "EXIT") == 0 ? 0 : 1;
        }
        signal_name = tool_signal_name((int)numeric);
        if (rt_strcmp(signal_name, "UNKNOWN") == 0 && numeric > 128) {
            numeric -= 128;
            signal_name = tool_signal_name((int)numeric);
        }
        if (rt_strcmp(signal_name, "UNKNOWN") == 0) {
            tool_write_error("kill", "unknown signal ", text);
            return 1;
        }
        return rt_write_line(1, signal_name) == 0 ? 0 : 1;
    }

    if (tool_parse_signal_name(text, &signal_number) != 0) {
        tool_write_error("kill", "unknown signal ", text);
        return 1;
    }
    if (rt_write_uint(1, (unsigned long long)signal_number) != 0 || rt_write_char(1, '\n') != 0) {
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int argi = 1;
    int signal_number = 15;
    int exit_code = 0;
    int list_mode = 0;

    if (argc < 2) {
        print_usage();
        return 1;
    }

    while (argi < argc) {
        const char *arg = argv[argi];

        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        }
        if (rt_strcmp(arg, "-l") == 0 || rt_strcmp(arg, "-L") == 0 || rt_strcmp(arg, "--list") == 0) {
            list_mode = 1;
            argi += 1;
            break;
        }
        if (starts_with(arg, "--list=")) {
            list_mode = 1;
            if (write_signal_lookup(arg + 7) != 0) {
                exit_code = 1;
            }
            argi += 1;
            break;
        }
        if (rt_strcmp(arg, "-s") == 0 || rt_strcmp(arg, "--signal") == 0) {
            if (argi + 1 >= argc || tool_parse_signal_name(argv[argi + 1], &signal_number) != 0) {
                tool_write_error("kill", "unknown signal ", argi + 1 < argc ? argv[argi + 1] : "");
                return 1;
            }
            argi += 2;
            continue;
        }
        if (starts_with(arg, "--signal=")) {
            if (tool_parse_signal_name(arg + 9, &signal_number) != 0) {
                tool_write_error("kill", "unknown signal ", arg + 9);
                return 1;
            }
            argi += 1;
            continue;
        }
        if (arg[0] == '-' && arg[1] == 's' && arg[2] != '\0') {
            if (tool_parse_signal_name(arg + 2, &signal_number) != 0) {
                tool_write_error("kill", "unknown signal ", arg + 2);
                return 1;
            }
            argi += 1;
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0' && tool_parse_signal_name(arg + 1, &signal_number) == 0) {
            argi += 1;
            continue;
        }
        break;
    }

    if (list_mode) {
        if (argi < argc && rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
        }
        if (argi >= argc && exit_code == 0) {
            tool_write_signal_list(1);
            return 0;
        }
        for (; argi < argc; ++argi) {
            if (write_signal_lookup(argv[argi]) != 0) {
                exit_code = 1;
            }
        }
        return exit_code;
    }

    if (argi >= argc) {
        print_usage();
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
