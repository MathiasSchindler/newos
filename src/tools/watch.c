#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define WATCH_DEFAULT_INTERVAL_MS 2000ULL

static int starts_with(const char *text, const char *prefix) {
    size_t index = 0;

    while (prefix[index] != '\0') {
        if (text[index] != prefix[index]) {
            return 0;
        }
        index += 1U;
    }

    return 1;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-n INTERVAL] [-c COUNT] [-t] COMMAND [ARG ...]");
}

static void write_interval_value(unsigned long long milliseconds) {
    unsigned long long whole = milliseconds / 1000ULL;
    unsigned long long fraction = milliseconds % 1000ULL;

    rt_write_uint(1, whole);
    if (fraction != 0ULL) {
        rt_write_char(1, '.');
        rt_write_char(1, (char)('0' + (fraction / 100ULL)));
        rt_write_char(1, (char)('0' + ((fraction / 10ULL) % 10ULL)));
        rt_write_char(1, (char)('0' + (fraction % 10ULL)));
    }
    rt_write_char(1, 's');
}

static int write_command_line(char *const argv[]) {
    size_t index = 0;

    while (argv[index] != 0) {
        if (index > 0U && rt_write_char(1, ' ') != 0) {
            return -1;
        }
        if (rt_write_cstr(1, argv[index]) != 0) {
            return -1;
        }
        index += 1U;
    }

    return 0;
}

static int write_header(unsigned long long interval_ms, char *const command_argv[], int use_clear_screen) {
    char timestamp[128];
    long long now = platform_get_epoch_time();

    if (use_clear_screen) {
        if (rt_write_cstr(1, "\033[H\033[J") != 0) {
            return -1;
        }
    }

    if (rt_write_cstr(1, "Every ") != 0) {
        return -1;
    }
    write_interval_value(interval_ms);
    if (rt_write_cstr(1, ": ") != 0) {
        return -1;
    }
    if (write_command_line(command_argv) != 0) {
        return -1;
    }

    if (platform_format_time(now, 1, "%Y-%m-%d %H:%M:%S", timestamp, sizeof(timestamp)) == 0) {
        if (rt_write_cstr(1, "    ") != 0 ||
            rt_write_cstr(1, timestamp) != 0) {
            return -1;
        }
    }

    return rt_write_cstr(1, "\n\n");
}

static int run_command(char *const argv[], int *exit_status_out) {
    int pid = -1;

    if (platform_spawn_process(argv, -1, -1, 0, 0, 0, &pid) != 0) {
        return -1;
    }

    return platform_wait_process(pid, exit_status_out);
}

int main(int argc, char **argv) {
    ToolOptState options;
    unsigned long long interval_ms = WATCH_DEFAULT_INTERVAL_MS;
    unsigned long long repeat_count = 0ULL;
    int no_title = 0;
    int parse_result;
    int last_status = 0;
    unsigned long long iteration = 0ULL;
    int clear_screen = 0;

    tool_opt_init(&options, argc, argv, argv[0], "[-n INTERVAL] [-c COUNT] [-t] COMMAND [ARG ...]");

    while ((parse_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-n") == 0 || rt_strcmp(options.flag, "--interval") == 0) {
            if (tool_opt_require_value(&options) != 0 || tool_parse_duration_ms(options.value, &interval_ms) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (starts_with(options.flag, "--interval=")) {
            if (tool_parse_duration_ms(options.flag + 11, &interval_ms) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (rt_strcmp(options.flag, "-c") == 0 || rt_strcmp(options.flag, "--count") == 0) {
            if (tool_opt_require_value(&options) != 0 || tool_parse_uint_arg(options.value, &repeat_count, "watch", "count") != 0 || repeat_count == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (starts_with(options.flag, "--count=")) {
            if (tool_parse_uint_arg(options.flag + 8, &repeat_count, "watch", "count") != 0 || repeat_count == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (rt_strcmp(options.flag, "-t") == 0 || rt_strcmp(options.flag, "--no-title") == 0) {
            no_title = 1;
        } else {
            tool_write_error("watch", "unknown option: ", options.flag);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        print_usage(argv[0]);
        return 0;
    }

    if (options.argi >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    clear_screen = !no_title && platform_isatty(1);

    for (;;) {
        if (!no_title && write_header(interval_ms, &argv[options.argi], clear_screen) != 0) {
            return 1;
        }

        if (run_command(&argv[options.argi], &last_status) != 0) {
            tool_write_error("watch", "failed to execute ", argv[options.argi]);
            return 127;
        }

        iteration += 1ULL;
        if (repeat_count != 0ULL && iteration >= repeat_count) {
            break;
        }
        if (!no_title && !clear_screen) {
            if (rt_write_char(1, '\n') != 0) {
                return 1;
            }
        }
        if (platform_sleep_milliseconds(interval_ms) != 0) {
            tool_write_error("watch", "sleep failed", 0);
            return 1;
        }
    }

    return last_status;
}
