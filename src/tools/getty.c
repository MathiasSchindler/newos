#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define GETTY_USAGE "[-nqi] [-r DELAY] [-l PROGRAM | -c COMMAND] [-t TERM] [-f ISSUE] [-p PROMPT] TTY [ARG ...]"
#define GETTY_DEFAULT_DELAY_MS 1000ULL
#define GETTY_DEFAULT_TERM "linux"
#define GETTY_MAX_LOGIN_NAME 64U
#define GETTY_SAFE_PATH "/bin:/usr/bin"

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

static int is_stdio_path(const char *path) {
    return path != 0 && path[0] == '-' && path[1] == '\0';
}

static int validate_program_path(const char *path) {
    if (path == 0 || path[0] != '/') {
        tool_write_error("getty", "refusing non-absolute program path: ", path != 0 ? path : "(null)");
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

static void print_help(const char *program_name) {
    rt_write_cstr(1, "Usage: ");
    rt_write_cstr(1, program_name);
    rt_write_cstr(1, " " GETTY_USAGE "\n");
    rt_write_line(1, "Open a tty-like path, optionally prompt for a login name, and run a shell, command, or login program on it.");
}

static void copy_file_to_fd(const char *path, int fd) {
    int input_fd;

    if (path == 0 || path[0] == '\0') {
        return;
    }

    input_fd = platform_open_read(path);
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

static void write_text_to_fd(int fd, const char *text) {
    if (text != 0 && text[0] != '\0') {
        (void)platform_write(fd, text, rt_strlen(text));
    }
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

static void write_banner(const char *tty_path,
                         char *const argv[],
                         int quiet,
                         int print_issue,
                         const char *issue_path,
                         int restart_count) {
    int fd;

    fd = platform_open_append(tty_path, 0600U);
    if (fd < 0) {
        return;
    }

    if (print_issue) {
        copy_file_to_fd(issue_path, fd);
    }

    if (!quiet) {
        (void)platform_write(fd, "\r\nnewos getty on ", 17U);
        write_text_to_fd(fd, is_stdio_path(tty_path) ? "stdio" : tty_path);
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

static void trim_whitespace(char *text) {
    size_t start = 0U;
    size_t length;

    while (text[start] != '\0' && rt_is_space(text[start])) {
        start += 1U;
    }
    if (start > 0U) {
        memmove(text, text + start, rt_strlen(text + start) + 1U);
    }

    length = rt_strlen(text);
    while (length > 0U && rt_is_space(text[length - 1U])) {
        length -= 1U;
        text[length] = '\0';
    }
}

static int prompt_for_login_name(const char *tty_path, const char *prompt_text, char *buffer, size_t buffer_size) {
    int input_fd;
    int output_fd;
    size_t length = 0U;

    if (buffer == 0 || buffer_size == 0U) {
        return -1;
    }

    buffer[0] = '\0';
    output_fd = platform_open_append(tty_path, 0600U);
    if (output_fd >= 0) {
        write_text_to_fd(output_fd, prompt_text != 0 ? prompt_text : "login: ");
        (void)platform_close(output_fd);
    }

    input_fd = platform_open_read(tty_path);
    if (input_fd < 0) {
        return -1;
    }

    for (;;) {
        char ch = '\0';
        long bytes = platform_read(input_fd, &ch, 1U);

        if (bytes <= 0) {
            break;
        }
        if (ch == '\n' || ch == '\r') {
            break;
        }
        if ((ch == '\b' || ch == 127) && length > 0U) {
            length -= 1U;
            continue;
        }
        if (length + 1U < buffer_size) {
            buffer[length++] = ch;
        }
    }

    buffer[length] = '\0';
    (void)platform_close(input_fd);
    trim_whitespace(buffer);

    output_fd = platform_open_append(tty_path, 0600U);
    if (output_fd >= 0) {
        (void)platform_write(output_fd, "\r\n", 2U);
        (void)platform_close(output_fd);
    }

    return 0;
}

static int sync_login_environment(const char *login_name) {
    if (login_name != 0 && login_name[0] != '\0') {
        if (platform_setenv("GETTY_USER", login_name, 1) != 0 ||
            platform_setenv("USER", login_name, 1) != 0 ||
            platform_setenv("LOGNAME", login_name, 1) != 0) {
            return -1;
        }
    } else {
        (void)platform_unsetenv("GETTY_USER");
        (void)platform_unsetenv("USER");
        (void)platform_unsetenv("LOGNAME");
    }

    return 0;
}

int main(int argc, char **argv) {
    static char *default_argv[] = { (char *)"/bin/sh", 0 };
    char *program_argv[64];
    size_t program_capacity = sizeof(program_argv) / sizeof(program_argv[0]);
    size_t program_count = 0U;
    const char *tty_path = 0;
    const char *login_program = 0;
    const char *command_text = 0;
    const char *term_name = GETTY_DEFAULT_TERM;
    const char *issue_path = "/etc/issue";
    const char *prompt_text = 0;
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
        } else if (rt_strcmp(options.flag, "-c") == 0 || rt_strcmp(options.flag, "--command") == 0) {
            if (tool_opt_require_value(&options) != 0) {
                print_help(argv[0]);
                return 1;
            }
            command_text = options.value;
        } else if (starts_with(options.flag, "--command=")) {
            command_text = options.flag + 10;
        } else if (rt_strcmp(options.flag, "-t") == 0 || rt_strcmp(options.flag, "--term") == 0) {
            if (tool_opt_require_value(&options) != 0) {
                print_help(argv[0]);
                return 1;
            }
            term_name = options.value;
        } else if (starts_with(options.flag, "--term=")) {
            term_name = options.flag + 7;
        } else if (rt_strcmp(options.flag, "-f") == 0 || rt_strcmp(options.flag, "--issue-file") == 0) {
            if (tool_opt_require_value(&options) != 0) {
                print_help(argv[0]);
                return 1;
            }
            issue_path = options.value;
            no_issue = 0;
        } else if (starts_with(options.flag, "--issue-file=")) {
            issue_path = options.flag + 13;
            no_issue = 0;
        } else if (rt_strcmp(options.flag, "-p") == 0 || rt_strcmp(options.flag, "--prompt") == 0) {
            if (tool_opt_require_value(&options) != 0) {
                print_help(argv[0]);
                return 1;
            }
            prompt_text = options.value;
        } else if (starts_with(options.flag, "--prompt=")) {
            prompt_text = options.flag + 9;
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
    if (term_name[0] == '\0') {
        tool_write_error("getty", "TERM value cannot be empty", 0);
        return 1;
    }
    if (command_text != 0 && login_program != 0) {
        tool_write_error("getty", "cannot combine -c with -l", 0);
        print_help(argv[0]);
        return 1;
    }
    if (command_text != 0 && options.argi < argc) {
        tool_write_error("getty", "cannot combine -c with PROGRAM arguments", 0);
        print_help(argv[0]);
        return 1;
    }

    if (command_text != 0) {
        program_argv[program_count++] = (char *)"/bin/sh";
        program_argv[program_count++] = (char *)"-c";
        program_argv[program_count++] = (char *)command_text;
        program_argv[program_count] = 0;
    } else if (login_program != 0) {
        size_t reserve = prompt_text != 0 ? 2U : 1U;
        program_argv[program_count++] = (char *)login_program;
        while (options.argi < argc && program_count + reserve < program_capacity) {
            program_argv[program_count++] = argv[options.argi++];
        }
        if (options.argi < argc) {
            tool_write_error("getty", "too many program arguments", 0);
            return 1;
        }
        program_argv[program_count] = 0;
    } else if (options.argi < argc) {
        while (options.argi < argc && program_count + 1U < program_capacity) {
            program_argv[program_count++] = argv[options.argi++];
        }
        if (options.argi < argc) {
            tool_write_error("getty", "too many program arguments", 0);
            return 1;
        }
        program_argv[program_count] = 0;
    } else {
        program_argv[0] = 0;
    }

    (void)platform_ignore_signal(2);
    (void)platform_ignore_signal(3);
    (void)platform_ignore_signal(13);
    if (platform_setenv("PATH", GETTY_SAFE_PATH, 1) != 0 ||
        platform_setenv("TERM", term_name, 1) != 0 ||
        platform_setenv("GETTY_TTY", tty_path, 1) != 0) {
        tool_write_error("getty", "failed to initialize session environment", 0);
        return 1;
    }

    for (;;) {
        int pid = -1;
        int status = 0;
        char login_name[GETTY_MAX_LOGIN_NAME];
        char *const *child_argv = program_count > 0U ? program_argv : default_argv;
        char resolved_program[256];
        char **spawn_argv = (char **)child_argv;

        write_banner(tty_path, (char *const *)child_argv, quiet, !no_issue, issue_path, restart_count);
        login_name[0] = '\0';

        if (prompt_text != 0) {
            if (prompt_for_login_name(tty_path, prompt_text, login_name, sizeof(login_name)) != 0) {
                tool_write_error("getty", "failed to read login name from ", tty_path);
                return 1;
            }
            if (sync_login_environment(login_name[0] != '\0' ? login_name : 0) != 0) {
                tool_write_error("getty", "failed to update login environment", 0);
                return 1;
            }
        }
        if (login_program != 0 && login_name[0] != '\0') {
            program_argv[program_count] = login_name;
            program_argv[program_count + 1U] = 0;
            child_argv = program_argv;
        } else if (program_count > 0U) {
            program_argv[program_count] = 0;
            child_argv = program_argv;
        }

        spawn_argv = (char **)child_argv;
        resolved_program[0] = '\0';
        if (validate_program_path(spawn_argv[0]) != 0) {
            return 1;
        }
        resolve_host_program_path(spawn_argv, resolved_program, sizeof(resolved_program));

        if (platform_spawn_process((char *const *)spawn_argv,
                                   -1,
                                   -1,
                                   is_stdio_path(tty_path) ? 0 : tty_path,
                                   is_stdio_path(tty_path) ? 0 : tty_path,
                                   1,
                                   &pid) != 0) {
            tool_write_error("getty", "failed to start ", spawn_argv[0]);
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
