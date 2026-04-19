#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define GETTY_USAGE "[-nqi] [-r DELAY] [-l PROGRAM] TTY [ARG ...]"
#define GETTY_DEFAULT_DELAY_MS 1000ULL

static int starts_with(const char *text, const char *prefix) {
    size_t i = 0U;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }

    return 1;
}

static void print_help(const char *program_name) {
    rt_write_cstr(1, "Usage: ");
    rt_write_cstr(1, program_name);
    rt_write_cstr(1, " " GETTY_USAGE "\n");
    rt_write_line(1, "Open a tty-like path, print a simple login banner, and run a shell or login program on it.");
}

static void copy_file_to_fd(const char *path, int fd) {
    int input_fd = platform_open_read(path);

    if (input_fd < 0) {
        return;
    }

    for (;;) {
        char buffer[512];
        long bytes = platform_read(input_fd, buffer, sizeof(buffer));

        if (bytes <= 0) {
            break;
        }
        (void)platform_write(fd, buffer, (size_t)bytes);
    }

    (void)platform_close(input_fd);
}

static void write_command_line(int fd, char *const argv[]) {
    size_t index = 0U;

    while (argv[index] != 0) {
        if (index > 0U) {
            (void)platform_write(fd, " ", 1U);
        }
        (void)platform_write(fd, argv[index], rt_strlen(argv[index]));
        index += 1U;
    }
}

static void write_banner(const char *tty_path, char *const argv[], int quiet, int print_issue, int restart_count) {
    int fd;

    fd = platform_open_append(tty_path, 0600U);
    if (fd < 0) {
        return;
    }

    if (print_issue) {
        copy_file_to_fd("/etc/issue", fd);
    }

    if (!quiet) {
        (void)platform_write(fd, "\r\nnewos getty on ", 17U);
        (void)platform_write(fd, tty_path, rt_strlen(tty_path));
        if (restart_count > 0) {
            (void)platform_write(fd, " (restart ", 10U);
            rt_write_uint(fd, (unsigned long long)restart_count);
            (void)platform_write(fd, ")", 1U);
        }
        (void)platform_write(fd, "\r\nstarting ", 11U);
        write_command_line(fd, argv);
        (void)platform_write(fd, "\r\n\r\n", 4U);
    }

    (void)platform_close(fd);
}

int main(int argc, char **argv) {
    static char *default_argv[] = { (char *)"/bin/sh", 0 };
    char *program_argv[64];
    const char *tty_path = 0;
    const char *login_program = 0;
    unsigned long long restart_delay_ms = GETTY_DEFAULT_DELAY_MS;
    int no_respawn = 0;
    int quiet = 0;
    int no_issue = 0;
    int restart_count = 0;
    ToolOptState options;
    int parse_result;

    tool_opt_init(&options, argc, argv, tool_base_name(argv[0]), GETTY_USAGE);

    while ((parse_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-n") == 0 || rt_strcmp(options.flag, "--no-respawn") == 0) {
            no_respawn = 1;
        } else if (rt_strcmp(options.flag, "-q") == 0 || rt_strcmp(options.flag, "--quiet") == 0) {
            quiet = 1;
        } else if (rt_strcmp(options.flag, "-i") == 0 || rt_strcmp(options.flag, "--no-issue") == 0) {
            no_issue = 1;
        } else if (rt_strcmp(options.flag, "-r") == 0 || rt_strcmp(options.flag, "--restart-delay") == 0) {
            if (tool_opt_require_value(&options) != 0 ||
                tool_parse_duration_ms(options.value, &restart_delay_ms) != 0) {
                print_help(argv[0]);
                return 1;
            }
        } else if (starts_with(options.flag, "--restart-delay=")) {
            if (tool_parse_duration_ms(options.flag + 16, &restart_delay_ms) != 0) {
                print_help(argv[0]);
                return 1;
            }
        } else if (rt_strcmp(options.flag, "-l") == 0 || rt_strcmp(options.flag, "--login") == 0) {
            if (tool_opt_require_value(&options) != 0) {
                print_help(argv[0]);
                return 1;
            }
            login_program = options.value;
        } else if (starts_with(options.flag, "--login=")) {
            login_program = options.flag + 8;
        } else {
            tool_write_error("getty", "unknown option: ", options.flag);
            print_help(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        print_help(argv[0]);
        return 0;
    }

    if (options.argi >= argc) {
        tool_write_error("getty", "missing tty path", 0);
        print_help(argv[0]);
        return 1;
    }

    tty_path = argv[options.argi++];
    if (tty_path[0] == '\0') {
        tool_write_error("getty", "tty path cannot be empty", 0);
        return 1;
    }

    if (login_program != 0) {
        size_t count = 0U;
        program_argv[count++] = (char *)login_program;
        while (options.argi < argc && count + 1U < sizeof(program_argv) / sizeof(program_argv[0])) {
            program_argv[count++] = argv[options.argi++];
        }
        program_argv[count] = 0;
    } else if (options.argi < argc) {
        size_t count = 0U;
        while (options.argi < argc && count + 1U < sizeof(program_argv) / sizeof(program_argv[0])) {
            program_argv[count++] = argv[options.argi++];
        }
        program_argv[count] = 0;
    } else {
        program_argv[0] = default_argv[0];
        program_argv[1] = 0;
    }

    (void)platform_ignore_signal(2);
    (void)platform_ignore_signal(3);
    (void)platform_ignore_signal(13);
    (void)platform_setenv("TERM", "linux", 0);

    for (;;) {
        int pid = -1;
        int status = 0;

        write_banner(tty_path, program_argv[0] != 0 ? program_argv : default_argv, quiet, !no_issue, restart_count);

        if (platform_spawn_process(program_argv[0] != 0 ? program_argv : default_argv,
                                   -1,
                                   -1,
                                   tty_path,
                                   tty_path,
                                   1,
                                   &pid) != 0) {
            tool_write_error("getty", "failed to start ", program_argv[0] != 0 ? program_argv[0] : default_argv[0]);
            return 127;
        }

        if (platform_wait_process(pid, &status) != 0) {
            tool_write_error("getty", "wait failed", 0);
            return 1;
        }

        if (no_respawn) {
            return status;
        }

        restart_count += 1;
        if (platform_sleep_milliseconds(restart_delay_ms) != 0) {
            tool_write_error("getty", "sleep failed", 0);
            return 1;
        }
    }
}
