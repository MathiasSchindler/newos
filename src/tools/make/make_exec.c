/*
 * make_exec.c - target execution engine for make.
 *
 * Covers: mtime-based staleness checks, recipe execution, and the recursive
 * build_target driver.
 */

#include "make_impl.h"

int path_exists_and_mtime(const char *path, long long *mtime_out) {
    PlatformDirEntry entry;

    if (platform_get_path_info(path, &entry) != 0) {
        return 0;
    }

    *mtime_out = entry.mtime;
    return 1;
}

unsigned long long effective_job_count(const MakeProgram *program) {
    if (program == 0 || !program->jobs_flag_present) {
        return 1ULL;
    }
    if (program->requested_jobs > 0ULL) {
        return program->requested_jobs;
    }
    return 2ULL;
}

static int append_script_text(char *buffer, size_t buffer_size, size_t *used_io, const char *text) {
    size_t length = rt_strlen(text);

    if (*used_io + length + 1U > buffer_size) {
        return -1;
    }
    memcpy(buffer + *used_io, text, length);
    *used_io += length;
    buffer[*used_io] = '\0';
    return 0;
}

static int append_script_char(char *buffer, size_t buffer_size, size_t *used_io, char ch) {
    if (*used_io + 2U > buffer_size) {
        return -1;
    }
    buffer[*used_io] = ch;
    *used_io += 1U;
    buffer[*used_io] = '\0';
    return 0;
}

static int execute_shell_command(MakeProgram *program, const char *command_text, int silent, int ignore_error, int force_run) {
    char *argv_exec[4];
    const char *shell = get_variable_value(program, "SHELL");
    int pid;
    int exit_status;

    if (!silent && !program->silent) {
        rt_write_line(1, command_text);
    }

    if (program->dry_run && !force_run) {
        return 0;
    }
    if (sync_program_environment(program) != 0) {
        return -1;
    }
    if (shell == 0 || shell[0] == '\0') {
        shell = "/bin/sh";
    }

    argv_exec[0] = (char *)shell;
    argv_exec[1] = "-c";
    argv_exec[2] = (char *)command_text;
    argv_exec[3] = 0;

    if (platform_spawn_process(argv_exec, 0, 1, 0, 0, 0, &pid) != 0 || platform_wait_process(pid, &exit_status) != 0) {
        return -1;
    }

    if (exit_status != 0 && !ignore_error) {
        return exit_status;
    }
    return 0;
}

typedef struct {
    int pid;
    const char *target;
} MakeChildBuild;

static int spawn_submake_for_target(MakeProgram *program, const char *target, int *pid_out) {
    char jobs_flag[] = "-j1";
    char *argv_exec[8];
    int argc = 0;
    const char *program_name = (program->program_name[0] != '\0') ? program->program_name : "make";
    const char *makefile_path = (program->makefile_path[0] != '\0') ? program->makefile_path : "Makefile";

    if (sync_program_environment(program) != 0) {
        return -1;
    }

    argv_exec[argc++] = (char *)program_name;
    argv_exec[argc++] = "-f";
    argv_exec[argc++] = (char *)makefile_path;
    argv_exec[argc++] = jobs_flag;
    if (program->silent) {
        argv_exec[argc++] = "-s";
    }
    if (program->always_make) {
        argv_exec[argc++] = "-B";
    }
    argv_exec[argc++] = (char *)target;
    argv_exec[argc] = 0;

    return platform_spawn_process(argv_exec, 0, 1, 0, 0, 0, pid_out);
}

int build_targets(MakeProgram *program, const char **targets, size_t target_count) {
    unsigned long long jobs = effective_job_count(program);
    size_t next = 0U;

    if (target_count == 0U) {
        return 0;
    }

    if (jobs <= 1ULL || target_count < 2U || program->dry_run) {
        size_t i;
        for (i = 0U; i < target_count; ++i) {
            int status = build_target(program, targets[i]);
            if (status != 0) {
                return status;
            }
        }
        return 0;
    }

    while (next < target_count) {
        MakeChildBuild children[16];
        size_t started = 0U;
        size_t limit = (jobs > 16ULL) ? 16U : (size_t)jobs;
        size_t i;

        while (started < limit && next < target_count) {
            if (spawn_submake_for_target(program, targets[next], &children[started].pid) != 0) {
                return 1;
            }
            children[started].target = targets[next];
            started += 1U;
            next += 1U;
        }

        for (i = 0U; i < started; ++i) {
            int exit_status = 0;
            MakeRule *rule;

            if (platform_wait_process(children[i].pid, &exit_status) != 0) {
                return 1;
            }
            if (exit_status != 0) {
                return exit_status;
            }

            rule = find_rule(program, children[i].target);
            if (rule != 0) {
                rule->built = 1;
                rule->building = 0;
            }
        }
    }

    return 0;
}

static int run_rule_commands(MakeProgram *program, MakeRule *rule) {
    size_t i;

    if (program->oneshell && rule->command_count > 1U) {
        char script[MAKE_SCRIPT_CAPACITY];
        size_t used = 0U;
        int ignore_error = 0;
        int force_run = 0;

        script[0] = '\0';
        for (i = 0; i < rule->command_count; ++i) {
            char expanded[MAKE_LINE_CAPACITY];
            char *command_ptr = expanded;
            int silent = 0;

            if (expand_text(program, rule, rule->commands[i], expanded, sizeof(expanded)) != 0) {
                return -1;
            }

            while (*command_ptr == '@' || *command_ptr == '-' || *command_ptr == '+') {
                if (*command_ptr == '@') {
                    silent = 1;
                } else if (*command_ptr == '-') {
                    ignore_error = 1;
                } else if (*command_ptr == '+') {
                    force_run = 1;
                }
                command_ptr += 1;
            }
            command_ptr = trim_leading_whitespace(command_ptr);

            if (!silent && !program->silent) {
                rt_write_line(1, command_ptr);
            }
            if (append_script_text(script, sizeof(script), &used, command_ptr) != 0 ||
                append_script_char(script, sizeof(script), &used, '\n') != 0) {
                return -1;
            }
        }

        if (script[0] != '\0') {
            return execute_shell_command(program, script, 1, ignore_error, force_run);
        }
        return 0;
    }

    for (i = 0; i < rule->command_count; ++i) {
        char expanded[MAKE_LINE_CAPACITY];
        char *command_ptr = expanded;
        int silent = 0;
        int ignore_error = 0;
        int force_run = 0;

        if (expand_text(program, rule, rule->commands[i], expanded, sizeof(expanded)) != 0) {
            return -1;
        }

        while (*command_ptr == '@' || *command_ptr == '-' || *command_ptr == '+') {
            if (*command_ptr == '@') {
                silent = 1;
            } else if (*command_ptr == '-') {
                ignore_error = 1;
            } else if (*command_ptr == '+') {
                force_run = 1;
            }
            command_ptr += 1;
        }
        command_ptr = trim_leading_whitespace(command_ptr);

        {
            int run_status = execute_shell_command(program, command_ptr, silent, ignore_error, force_run);
            if (run_status != 0) {
                return run_status;
            }
        }
    }

    return 0;
}

int build_target(MakeProgram *program, const char *target) {
    MakeRule generated_rule;
    MakeRule *rule = find_rule(program, target);
    size_t i;
    int need_run = 0;
    long long target_mtime = 0;
    int target_exists = path_exists_and_mtime(target, &target_mtime);
    int using_generated_rule = 0;

    if (rule == 0) {
        char stem[MAKE_NAME_CAPACITY];
        MakeRule *pattern_rule = find_pattern_rule(program, target, stem, sizeof(stem));
        if (pattern_rule != 0) {
            if (instantiate_pattern_rule(pattern_rule, target, stem, &generated_rule) != 0) {
                tool_write_error("make", "cannot expand pattern rule for ", target);
                return 1;
            }
            rule = &generated_rule;
            using_generated_rule = 1;
        }
    }

    if (rule == 0) {
        if (target_exists) {
            return 0;
        }
        tool_write_error("make", "no rule to make target ", target);
        return 1;
    }

    if (is_target_active(program, target)) {
        tool_write_error("make", "dependency cycle on ", target);
        return 1;
    }
    if (push_active_target(program, target) != 0) {
        tool_write_error("make", "target stack overflow on ", target);
        return 1;
    }

    if (rule->building) {
        pop_active_target(program);
        tool_write_error("make", "dependency cycle on ", target);
        return 1;
    }
    if (rule->built) {
        pop_active_target(program);
        return 0;
    }

    rule->building = 1;

    if (rule->dep_count > 0U) {
        if (rule->dep_count > 1U && effective_job_count(program) > 1ULL && !program->dry_run) {
            const char *dep_targets[MAKE_MAX_DEPS];

            for (i = 0; i < rule->dep_count; ++i) {
                dep_targets[i] = rule->deps[i];
            }
            if (build_targets(program, dep_targets, rule->dep_count) != 0) {
                rule->building = 0;
                pop_active_target(program);
                return 1;
            }
        } else {
            for (i = 0; i < rule->dep_count; ++i) {
                if (build_target(program, rule->deps[i]) != 0) {
                    rule->building = 0;
                    pop_active_target(program);
                    return 1;
                }
            }
        }

        for (i = 0; i < rule->dep_count; ++i) {
            long long dep_mtime = 0;
            if (!target_exists || (path_exists_and_mtime(rule->deps[i], &dep_mtime) && dep_mtime > target_mtime)) {
                need_run = 1;
            }
        }
    }

    if (is_phony_target(program, target)) {
        need_run = 1;
    }

    if (program->always_make && rule->command_count > 0) {
        need_run = 1;
    }

    if (!target_exists && rule->command_count > 0) {
        need_run = 1;
    }

    if (rule->command_count == 0) {
        if (!target_exists && rule->dep_count == 0 && !is_phony_target(program, target)) {
            tool_write_error("make", "nothing to do for ", target);
            rule->building = 0;
            pop_active_target(program);
            return 1;
        }
    } else if (need_run) {
        int run_status = run_rule_commands(program, rule);
        if (run_status != 0) {
            rule->building = 0;
            pop_active_target(program);
            return run_status < 0 ? 1 : run_status;
        }
    }

    rule->building = 0;
    if (!using_generated_rule) {
        rule->built = 1;
    }
    pop_active_target(program);
    return 0;
}
