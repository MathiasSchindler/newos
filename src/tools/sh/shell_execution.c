#include "shell_shared.h"
#include "runtime.h"

static ShJob *allocate_job_slot(void) {
    size_t i;
    ShJob *next_jobs;
    size_t next_capacity;

    for (i = 0; i < shell_jobs_count; ++i) {
        if (!shell_jobs[i].active) {
            return &shell_jobs[i];
        }
    }

    if (shell_jobs_count >= shell_jobs_capacity) {
        next_capacity = shell_jobs_capacity == 0U ? 16U : shell_jobs_capacity * 2U;
        next_jobs = (ShJob *)rt_realloc(shell_jobs, next_capacity * sizeof(*next_jobs));
        if (next_jobs == 0) {
            return 0;
        }
        rt_memset(next_jobs + shell_jobs_capacity, 0, (next_capacity - shell_jobs_capacity) * sizeof(*next_jobs));
        shell_jobs = next_jobs;
        shell_jobs_capacity = next_capacity;
    }

    return &shell_jobs[shell_jobs_count++];
}

ShJob *sh_find_job_by_id(int job_id) {
    size_t i;
    ShJob *fallback = 0;

    for (i = 0; i < shell_jobs_count; ++i) {
        if (shell_jobs[i].active) {
            fallback = &shell_jobs[i];
            if (shell_jobs[i].job_id == job_id) {
                return &shell_jobs[i];
            }
        }
    }

    return (job_id == 0) ? fallback : 0;
}

static void remember_job(const int *pids, size_t pid_count, const char *command_text) {
    ShJob *job = allocate_job_slot();
    size_t i;

    if (job == 0) {
        return;
    }

    job->active = 1;
    job->job_id = shell_next_job_id++;
    job->pid_count = (int)pid_count;
    rt_free(job->pids);
    job->pids = (int *)rt_malloc(pid_count * sizeof(*job->pids));
    if (job->pids == 0) {
        job->active = 0;
        return;
    }
    for (i = 0; i < pid_count; ++i) {
        job->pids[i] = pids[i];
    }
    rt_copy_string(job->command, sizeof(job->command), command_text);
}

int sh_execute_pipeline(const ShPipeline *pipeline, int background, const char *command_text) {
    int *pids;
    char (*resolved_paths)[SH_MAX_LINE];
    int prev_read_fd = -1;
    int exit_status = 0;
    size_t i;

    pids = (int *)rt_malloc(pipeline->count * sizeof(*pids));
    resolved_paths = (char (*)[SH_MAX_LINE])rt_malloc(pipeline->count * sizeof(*resolved_paths));
    if (pids == 0 || resolved_paths == 0) {
        rt_free(pids);
        rt_free(resolved_paths);
        rt_write_line(2, "sh: out of memory");
        return 1;
    }

    for (i = 0; i < pipeline->count; ++i) {
        int pipe_fds[2] = { -1, -1 };
        int stdin_fd = prev_read_fd;
        int stdout_fd = -1;
        char **argv_copy;
        char *original_argv0 = pipeline->commands[i].argv[0];
        int argi;

        if (i + 1 < pipeline->count) {
            if (platform_create_pipe(pipe_fds) != 0) {
                if (prev_read_fd >= 0) {
                    platform_close(prev_read_fd);
                }
                rt_write_line(2, "sh: pipe creation failed");
                rt_free(pids);
                rt_free(resolved_paths);
                return 1;
            }
            stdout_fd = pipe_fds[1];
        }

        if (sh_resolve_shell_command_path(original_argv0, resolved_paths[i], sizeof(resolved_paths[i])) != 0) {
            if (stdin_fd >= 0) {
                platform_close(stdin_fd);
            }
            if (pipe_fds[0] >= 0) {
                platform_close(pipe_fds[0]);
            }
            if (pipe_fds[1] >= 0) {
                platform_close(pipe_fds[1]);
            }
            rt_write_cstr(2, "sh: failed to execute ");
            rt_write_line(2, original_argv0);
            rt_free(pids);
            rt_free(resolved_paths);
            return 127;
        }

        argv_copy = (char **)rt_malloc(((size_t)pipeline->commands[i].argc + 1U) * sizeof(*argv_copy));
        if (argv_copy == 0) {
            if (stdin_fd >= 0) {
                platform_close(stdin_fd);
            }
            if (pipe_fds[0] >= 0) {
                platform_close(pipe_fds[0]);
            }
            if (pipe_fds[1] >= 0) {
                platform_close(pipe_fds[1]);
            }
            rt_write_line(2, "sh: out of memory");
            rt_free(pids);
            rt_free(resolved_paths);
            return 1;
        }
        for (argi = 0; argi <= pipeline->commands[i].argc; ++argi) {
            argv_copy[argi] = pipeline->commands[i].argv[argi];
        }
        argv_copy[0] = resolved_paths[i];

        if (platform_spawn_process(
                argv_copy,
                stdin_fd,
                stdout_fd,
                pipeline->commands[i].input_path,
                pipeline->commands[i].output_path,
                pipeline->commands[i].output_append,
                &pids[i]) != 0) {
            if (stdin_fd >= 0) {
                platform_close(stdin_fd);
            }
            if (pipe_fds[0] >= 0) {
                platform_close(pipe_fds[0]);
            }
            if (pipe_fds[1] >= 0) {
                platform_close(pipe_fds[1]);
            }
            rt_write_cstr(2, "sh: failed to execute ");
            rt_write_line(2, original_argv0);
            rt_free(argv_copy);
            rt_free(pids);
            rt_free(resolved_paths);
            return 127;
        }
        rt_free(argv_copy);

        if (stdin_fd >= 0) {
            platform_close(stdin_fd);
        }
        if (stdout_fd >= 0) {
            platform_close(stdout_fd);
        }

        prev_read_fd = (i + 1 < pipeline->count) ? pipe_fds[0] : -1;
    }

    if (prev_read_fd >= 0) {
        platform_close(prev_read_fd);
    }

    if (background) {
        remember_job(pids, pipeline->count, command_text);
        rt_free(pids);
        rt_free(resolved_paths);
        return 0;
    }

    for (i = 0; i < pipeline->count; ++i) {
        int status = 1;
        if (platform_wait_process(pids[i], &status) != 0) {
            rt_write_line(2, "sh: wait failed");
            rt_free(pids);
            rt_free(resolved_paths);
            return 1;
        }
        if (i + 1 == pipeline->count) {
            exit_status = status;
        }
    }

    rt_free(pids);
    rt_free(resolved_paths);
    return exit_status;
}
