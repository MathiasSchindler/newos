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

static int run_rule_commands(MakeProgram *program, MakeRule *rule) {
    size_t i;

    for (i = 0; i < rule->command_count; ++i) {
        char expanded[MAKE_LINE_CAPACITY];
        char *command_ptr = expanded;
        int silent = 0;
        int ignore_error = 0;
        char *argv_exec[4];
        int pid;
        int exit_status;

        if (expand_text(program, rule, rule->commands[i], expanded, sizeof(expanded)) != 0) {
            return -1;
        }

        while (*command_ptr == '@' || *command_ptr == '-' || *command_ptr == '+') {
            if (*command_ptr == '@') {
                silent = 1;
            } else if (*command_ptr == '-') {
                ignore_error = 1;
            }
            command_ptr += 1;
        }
        command_ptr = trim_leading_whitespace(command_ptr);

        if (!silent && !program->silent) {
            rt_write_line(1, command_ptr);
        }

        if (program->dry_run) {
            continue;
        }

        argv_exec[0] = "/bin/sh";
        argv_exec[1] = "-c";
        argv_exec[2] = command_ptr;
        argv_exec[3] = 0;

        if (platform_spawn_process(argv_exec, 0, 1, 0, 0, 0, &pid) != 0 || platform_wait_process(pid, &exit_status) != 0) {
            return -1;
        }

        if (exit_status != 0 && !ignore_error) {
            return exit_status;
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

    for (i = 0; i < rule->dep_count; ++i) {
        long long dep_mtime = 0;
        if (build_target(program, rule->deps[i]) != 0) {
            rule->building = 0;
            pop_active_target(program);
            return 1;
        }
        if (!target_exists || (path_exists_and_mtime(rule->deps[i], &dep_mtime) && dep_mtime > target_mtime)) {
            need_run = 1;
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
