#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define INIT_DEFAULT_RESTART_DELAY_MS 1000ULL
#define INIT_USAGE "[-nq] [-r DELAY] [-m COUNT] [-t PATH] [-e NAME=VALUE] [-c COMMAND] [PROGRAM [ARG ...]]"
#define INIT_MAX_ENV_SETTINGS 16U
#define INIT_MAX_ENV_NAME_LENGTH 64U
#define INIT_SAFE_PATH "/bin:/usr/bin"

typedef struct {
    char        name[INIT_MAX_ENV_NAME_LENGTH];
    const char *value;
} InitEnvSetting;

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

static int is_stdio_path(const char *path) {
    return path != 0 && path[0] == '-' && path[1] == '\0';
}

static int validate_program_path(const char *path) {
    if (path == 0 || path[0] != '/') {
        tool_write_error("init", "refusing non-absolute program path: ", path != 0 ? path : "(null)");
        return -1;
    }
    return 0;
}

static void resolve_host_program_path(char **argv_exec, char *buffer, size_t buffer_size) {
    PlatformDirEntry entry;
    const char *base_name;

    if (argv_exec == 0 || argv_exec[0] == 0 || buffer == 0 || buffer_size == 0U) {
        return;
    }
    if (argv_exec[0][0] != '/' || !starts_with(argv_exec[0], "/bin/")) {
        return;
    }
    if (platform_get_path_info(argv_exec[0], &entry) == 0) {
        return;
    }

    base_name = tool_base_name(argv_exec[0]);
    if (tool_join_path("/usr/bin", base_name, buffer, buffer_size) != 0) {
        return;
    }
    if (platform_get_path_info(buffer, &entry) != 0 || entry.is_dir) {
        return;
    }

    argv_exec[0] = buffer;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, INIT_USAGE);
}

static int add_env_setting(InitEnvSetting *settings, size_t capacity, size_t *count_io, const char *assignment) {
    size_t name_length = 0U;

    if (settings == 0 || count_io == 0 || assignment == 0) {
        tool_write_error("init", "expected NAME=VALUE after -e", 0);
        return -1;
    }

    while (assignment[name_length] != '\0' && assignment[name_length] != '=') {
        name_length += 1U;
    }

    if (name_length == 0U || assignment[name_length] != '=') {
        tool_write_error("init", "expected NAME=VALUE after -e: ", assignment);
        return -1;
    }
    if (*count_io >= capacity) {
        tool_write_error("init", "too many environment overrides", 0);
        return -1;
    }
    if (name_length >= sizeof(settings[*count_io].name)) {
        tool_write_error("init", "environment variable name too long: ", assignment);
        return -1;
    }

    memcpy(settings[*count_io].name, assignment, name_length);
    settings[*count_io].name[name_length] = '\0';
    settings[*count_io].value = assignment + name_length + 1U;
    *count_io += 1U;
    return 0;
}

static int apply_env_settings(const InitEnvSetting *settings, size_t count) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (platform_setenv(settings[index].name, settings[index].value, 1) != 0) {
            tool_write_error("init", "failed to set environment variable: ", settings[index].name);
            return -1;
        }
    }

    return 0;
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

static void write_start_message(char *const argv[], const char *console_path, unsigned long long delay_ms, unsigned long long restart_count) {
    rt_write_cstr(2, "init: ");
    if (restart_count == 0) {
        rt_write_cstr(2, "starting ");
    } else {
        rt_write_cstr(2, "respawning ");
    }
    write_command_line(argv);
    if (console_path != 0) {
        rt_write_cstr(2, " on ");
        rt_write_cstr(2, is_stdio_path(console_path) ? "stdio" : console_path);
    }
    if (restart_count > 0) {
        rt_write_cstr(2, " (restart ");
        rt_write_uint(2, restart_count);
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
    InitEnvSetting env_settings[INIT_MAX_ENV_SETTINGS];
    const char *command_text = 0;
    const char *console_path = 0;
    unsigned long long restart_delay_ms = INIT_DEFAULT_RESTART_DELAY_MS;
    unsigned long long max_restarts = 0ULL;
    size_t env_count = 0U;
    int no_respawn = 0;
    int quiet = 0;
    int restart_limit_enabled = 0;
    unsigned long long restart_count = 0ULL;
    ToolOptState options;
    int parse_result;

    tool_opt_init(&options, argc, argv, tool_base_name(argv[0]), INIT_USAGE);

    while ((parse_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-n") == 0 || rt_strcmp(options.flag, "--no-respawn") == 0) {
            no_respawn = 1;
        } else if (rt_strcmp(options.flag, "-q") == 0 || rt_strcmp(options.flag, "--quiet") == 0) {
            quiet = 1;
        } else if (rt_strcmp(options.flag, "-m") == 0 || rt_strcmp(options.flag, "--max-restarts") == 0) {
            if (tool_opt_require_value(&options) != 0 ||
                tool_parse_uint_arg(options.value, &max_restarts, "init", "max-restarts") != 0) {
                print_usage(argv[0]);
                return 1;
            }
            restart_limit_enabled = 1;
        } else if (starts_with(options.flag, "--max-restarts=")) {
            if (tool_parse_uint_arg(options.flag + 15, &max_restarts, "init", "max-restarts") != 0) {
                print_usage(argv[0]);
                return 1;
            }
            restart_limit_enabled = 1;
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
        } else if (rt_strcmp(options.flag, "-t") == 0 || rt_strcmp(options.flag, "--console") == 0) {
            if (tool_opt_require_value(&options) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            console_path = options.value;
        } else if (starts_with(options.flag, "--console=")) {
            console_path = options.flag + 10;
        } else if (rt_strcmp(options.flag, "-e") == 0 || rt_strcmp(options.flag, "--setenv") == 0) {
            if (tool_opt_require_value(&options) != 0 ||
                add_env_setting(env_settings, INIT_MAX_ENV_SETTINGS, &env_count, options.value) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (starts_with(options.flag, "--setenv=")) {
            if (add_env_setting(env_settings, INIT_MAX_ENV_SETTINGS, &env_count, options.flag + 9) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else {
            tool_write_error("init", "unknown option: ", options.flag);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        print_usage(argv[0]);
        rt_write_line(1, "Run a command as a tiny init-style supervisor. Defaults to launching sh and can bind the child to a console path.");
        return 0;
    }

    if (command_text != 0 && options.argi < argc) {
        tool_write_error("init", "cannot combine -c with PROGRAM arguments", 0);
        print_usage(argv[0]);
        return 1;
    }
    if (console_path != 0 && console_path[0] == '\0') {
        tool_write_error("init", "console path cannot be empty", 0);
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
    if (platform_setenv("PATH", INIT_SAFE_PATH, 1) != 0) {
        tool_write_error("init", "failed to set PATH", 0);
        return 1;
    }
    if (apply_env_settings(env_settings, env_count) != 0) {
        return 1;
    }

    for (;;) {
        int pid = -1;
        int status = 0;
        char resolved_program[256];
        char **spawn_argv = (char **)child_argv;

        if (!quiet) {
            write_start_message((char *const *)child_argv, console_path, restart_delay_ms, restart_count);
            if (platform_get_process_id() != 1) {
                rt_write_line(2, "init: note: not running as PID 1");
            }
        }

        resolved_program[0] = '\0';
        if (validate_program_path(spawn_argv[0]) != 0) {
            return 1;
        }
        resolve_host_program_path(spawn_argv, resolved_program, sizeof(resolved_program));

        if (platform_spawn_process((char *const *)spawn_argv,
                                   -1,
                                   -1,
                                   is_stdio_path(console_path) ? 0 : console_path,
                                   is_stdio_path(console_path) ? 0 : console_path,
                                   console_path != 0,
                                   &pid) != 0) {
            tool_write_error("init", "failed to execute ", spawn_argv[0]);
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
        if (restart_limit_enabled && restart_count >= max_restarts) {
            if (!quiet) {
                rt_write_line(2, "init: restart limit reached; not respawning");
            }
            return status;
        }
        restart_count += 1;

        if (platform_sleep_milliseconds(restart_delay_ms) != 0) {
            tool_write_error("init", "sleep failed", 0);
            return 1;
        }
    }
}
