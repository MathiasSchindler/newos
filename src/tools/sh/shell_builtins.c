#define _POSIX_C_SOURCE 200809L

#include "shell_shared.h"
#include "runtime.h"

static int builtin_jobs(void) {
    int i;

    for (i = 0; i < SH_MAX_JOBS; ++i) {
        if (shell_jobs[i].active) {
            rt_write_char(1, '[');
            rt_write_uint(1, (unsigned long long)shell_jobs[i].job_id);
            rt_write_cstr(1, "] running ");
            rt_write_line(1, shell_jobs[i].command);
        }
    }

    return 0;
}

static int builtin_history(void) {
    int i;

    for (i = 0; i < shell_history_count; ++i) {
        rt_write_uint(1, (unsigned long long)(i + 1));
        rt_write_cstr(1, "  ");
        rt_write_line(1, shell_history[i]);
    }

    return 0;
}

static int builtin_fg(int argc, char **argv) {
    unsigned long long value = 0;
    ShJob *job;
    int last_status = 0;
    int i;

    if (argc >= 2 && rt_parse_uint(argv[1], &value) != 0) {
        rt_write_line(2, "sh: fg requires numeric job id");
        return 2;
    }

    job = sh_find_job_by_id((argc >= 2) ? (int)value : 0);
    if (job == 0) {
        rt_write_line(2, "sh: no such job");
        return 1;
    }

    for (i = 0; i < job->pid_count; ++i) {
        int status = 0;
        if (platform_wait_process(job->pids[i], &status) != 0) {
            return 1;
        }
        last_status = status;
    }

    job->active = 0;
    return last_status;
}

static int builtin_bg(int argc, char **argv) {
    unsigned long long value = 0;
    ShJob *job;

    if (argc >= 2 && rt_parse_uint(argv[1], &value) != 0) {
        rt_write_line(2, "sh: bg requires numeric job id");
        return 2;
    }

    job = sh_find_job_by_id((argc >= 2) ? (int)value : 0);
    if (job == 0) {
        rt_write_line(2, "sh: no such job");
        return 1;
    }

    return 0;
}

static int builtin_cd(const ShCommand *cmd) {
    const char *path = (cmd->argc >= 2) ? cmd->argv[1] : ".";

    if (platform_change_directory(path) != 0) {
        rt_write_cstr(2, "sh: cd failed: ");
        rt_write_line(2, path);
        return 1;
    }

    return 0;
}

static int builtin_exit_command(const ShCommand *cmd) {
    unsigned long long code = 0;

    if (cmd->argc >= 2 && rt_parse_uint(cmd->argv[1], &code) != 0) {
        rt_write_line(2, "sh: exit requires numeric status");
        return 2;
    }

    shell_should_exit = 1;
    shell_exit_status = (cmd->argc >= 2) ? (int)code : 0;
    return shell_exit_status;
}

static int builtin_export_command(const ShCommand *cmd) {
    int i;

    for (i = 1; i < cmd->argc; ++i) {
        char *arg = cmd->argv[i];
        char *eq = arg;

        while (*eq != '\0' && *eq != '=') {
            eq += 1;
        }

        if (*eq == '=') {
            *eq = '\0';
            (void)platform_setenv(arg, eq + 1, 1);
            *eq = '=';
        }
    }

    return 0;
}

static int builtin_unset_command(const ShCommand *cmd) {
    int i;

    for (i = 1; i < cmd->argc; ++i) {
        (void)platform_unsetenv(cmd->argv[i]);
    }

    return 0;
}

static int builtin_alias_command(const ShCommand *cmd) {
    int i;

    if (cmd->argc == 1) {
        for (i = 0; i < SH_MAX_ALIASES; ++i) {
            if (shell_aliases[i].active) {
                rt_write_cstr(1, shell_aliases[i].name);
                rt_write_cstr(1, "='");
                rt_write_cstr(1, shell_aliases[i].value);
                rt_write_line(1, "'");
            }
        }
        return 0;
    }

    for (i = 1; i < cmd->argc; ++i) {
        if (sh_set_shell_alias(cmd->argv[i]) != 0) {
            const char *value = sh_lookup_shell_alias(cmd->argv[i]);
            if (value != 0) {
                rt_write_line(1, value);
            } else {
                rt_write_cstr(2, "sh: invalid alias: ");
                rt_write_line(2, cmd->argv[i]);
                return 1;
            }
        }
    }

    return 0;
}

static int builtin_command_command(const ShCommand *cmd) {
    int i;
    int exit_code = 0;

    if (cmd->argc < 3 || rt_strcmp(cmd->argv[1], "-v") != 0) {
        rt_write_line(2, "Usage: command -v NAME...");
        return 2;
    }

    for (i = 2; i < cmd->argc; ++i) {
        char path[SH_MAX_LINE];
        if (sh_is_shell_builtin_name(cmd->argv[i]) || sh_lookup_shell_alias(cmd->argv[i]) != 0 || sh_lookup_shell_function(cmd->argv[i]) != 0) {
            rt_write_line(1, cmd->argv[i]);
        } else if (sh_resolve_shell_command_path(cmd->argv[i], path, sizeof(path)) == 0) {
            rt_write_line(1, path);
        } else {
            exit_code = 1;
        }
    }

    return exit_code;
}

int sh_is_shell_builtin_name(const char *name) {
    static const char *names[] = {
        "cd", "exit", "jobs", "history", "fg", "bg", "export", "unset", "command", "alias"
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (rt_strcmp(name, names[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

int sh_try_run_builtin(const ShPipeline *pipeline, int *status_out) {
    ShCommand *cmd;

    if (pipeline->count != 1 || pipeline->commands[0].argc == 0) {
        return 0;
    }

    cmd = (ShCommand *)&pipeline->commands[0];

    if (rt_strcmp(cmd->argv[0], "cd") == 0) {
        *status_out = builtin_cd(cmd);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "exit") == 0) {
        *status_out = builtin_exit_command(cmd);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "jobs") == 0) {
        *status_out = builtin_jobs();
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "history") == 0) {
        *status_out = builtin_history();
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "fg") == 0) {
        *status_out = builtin_fg(cmd->argc, cmd->argv);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "bg") == 0) {
        *status_out = builtin_bg(cmd->argc, cmd->argv);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "export") == 0) {
        *status_out = builtin_export_command(cmd);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "unset") == 0) {
        *status_out = builtin_unset_command(cmd);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "alias") == 0) {
        *status_out = builtin_alias_command(cmd);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "command") == 0) {
        *status_out = builtin_command_command(cmd);
        return 1;
    }

    return 0;
}
