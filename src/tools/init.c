#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define INIT_DEFAULT_RESTART_DELAY_MS 1000ULL

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
    tool_write_usage(program_name, "[-nq] [-r DELAY] [-c COMMAND] [PROGRAM [ARG ...]]");
}

static void write_command_line(char *const argv[]) {
    size_t index = 0U;

    while (argv[index] != 0) {
        if (index > 0U) {
            rt_write_char(2, ' ');
        }
        rt_write_cstr(2, argv[index]);
        index += 1U;
    }
}

static void write_start_message(char *const argv[], unsigned long long delay_ms, int restart_count) {
    rt_write_cstr(2, "init: ");
    if (restart_count == 0) {
        rt_write_cstr(2, "starting ");
    } else {
        rt_write_cstr(2, "respawning ");
    }
    write_command_line(argv);
    if (restart_count > 0) {
        rt_write_cstr(2, " (restart ");
        rt_write_uint(2, (unsigned long long)restart_count);
        rt_write_cstr(2, ", delay ");
        rt_write_uint(2, delay_ms);
        rt_write_cstr(2, "ms)");
    }
    rt_write_char(2, '\n');
}

static void write_exit_message(int status) {
    rt_write_cstr(2, "init: child exited with status ");
    rt_write_uint(2, (unsigned long long)(status < 0 ? 1 : status));
    rt_write_char(2, '\n');
}

int main(int argc, char **argv) {
    static char *default_argv[] = { (char *)"/bin/sh", 0 };
    char *shell_argv[4];
    char *const *child_argv = default_argv;
    const char *command_text = 0;
    unsigned long long restart_delay_ms = INIT_DEFAULT_RESTART_DELAY_MS;
    int no_respawn = 0;
    int quiet = 0;
    int restart_count = 0;
    ToolOptState options;
    int parse_result;

    tool_opt_init(&options, argc, argv, tool_base_name(argv[0]), "[-nq] [-r DELAY] [-c COMMAND] [PROGRAM [ARG ...]]");

    while ((parse_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-n") == 0 || rt_strcmp(options.flag, "--no-respawn") == 0) {
            no_respawn = 1;
        } else if (rt_strcmp(options.flag, "-q") == 0 || rt_strcmp(options.flag, "--quiet") == 0) {
            quiet = 1;
        } else if (rt_strcmp(options.flag, "-r") == 0 || rt_strcmp(options.flag, "--restart-delay") == 0) {
            if (tool_opt_require_value(&options) != 0 ||
                tool_parse_duration_ms(options.value, &restart_delay_ms) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (starts_with(options.flag, "--restart-delay=")) {
            if (tool_parse_duration_ms(options.flag + 16, &restart_delay_ms) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (rt_strcmp(options.flag, "-c") == 0 || rt_strcmp(options.flag, "--command") == 0) {
            if (tool_opt_require_value(&options) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            command_text = options.value;
        } else if (starts_with(options.flag, "--command=")) {
            command_text = options.flag + 10;
        } else {
            tool_write_error("init", "unknown option: ", options.flag);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        print_usage(argv[0]);
        rt_write_line(1, "Run a command as a tiny init-style supervisor. Defaults to launching sh.");
        return 0;
    }

    if (command_text != 0 && options.argi < argc) {
        tool_write_error("init", "cannot combine -c with PROGRAM arguments", 0);
        print_usage(argv[0]);
        return 1;
    }

    if (command_text != 0) {
        shell_argv[0] = (char *)"/bin/sh";
        shell_argv[1] = (char *)"-c";
        shell_argv[2] = (char *)command_text;
        shell_argv[3] = 0;
        child_argv = shell_argv;
    } else if (options.argi < argc) {
        child_argv = &argv[options.argi];
    }

    (void)platform_ignore_signal(2);
    (void)platform_ignore_signal(3);
    (void)platform_ignore_signal(13);

    for (;;) {
        int pid = -1;
        int status = 0;

        if (!quiet) {
            write_start_message((char *const *)child_argv, restart_delay_ms, restart_count);
            if (platform_get_process_id() != 1) {
                rt_write_line(2, "init: note: not running as PID 1");
            }
        }

        if (platform_spawn_process((char *const *)child_argv, -1, -1, 0, 0, 0, &pid) != 0) {
            tool_write_error("init", "failed to execute ", child_argv[0]);
            return 127;
        }

        if (platform_wait_process(pid, &status) != 0) {
            tool_write_error("init", "wait failed", 0);
            return 1;
        }

        if (no_respawn) {
            return status;
        }

        if (!quiet) {
            write_exit_message(status);
        }
        restart_count += 1;

        if (platform_sleep_milliseconds(restart_delay_ms) != 0) {
            tool_write_error("init", "sleep failed", 0);
            return 1;
        }
    }
}
