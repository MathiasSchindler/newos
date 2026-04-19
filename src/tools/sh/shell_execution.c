#include "shell_shared.h"
#include "runtime.h"

static ShJob *allocate_job_slot(void) {
    int i;
    for (i = 0; i < SH_MAX_JOBS; ++i) {
        if (!shell_jobs[i].active) {
            return &shell_jobs[i];
        }
    }
    return 0;
}

ShJob *sh_find_job_by_id(int job_id) {
    int i;
    ShJob *fallback = 0;

    for (i = 0; i < SH_MAX_JOBS; ++i) {
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
    for (i = 0; i < pid_count; ++i) {
        job->pids[i] = pids[i];
    }
    rt_copy_string(job->command, sizeof(job->command), command_text);
}

int sh_execute_pipeline(const ShPipeline *pipeline, int background, const char *command_text) {
    int pids[SH_MAX_COMMANDS];
    int prev_read_fd = -1;
    int exit_status = 0;
    size_t i;

    for (i = 0; i < pipeline->count; ++i) {
        int pipe_fds[2] = { -1, -1 };
        int stdin_fd = prev_read_fd;
        int stdout_fd = -1;

        if (i + 1 < pipeline->count) {
            if (platform_create_pipe(pipe_fds) != 0) {
                if (prev_read_fd >= 0) {
                    platform_close(prev_read_fd);
                }
                rt_write_line(2, "sh: pipe creation failed");
                return 1;
            }
            stdout_fd = pipe_fds[1];
        }

        if (platform_spawn_process(
                pipeline->commands[i].argv,
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
            rt_write_line(2, pipeline->commands[i].argv[0]);
            return 127;
        }

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
        return 0;
    }

    for (i = 0; i < pipeline->count; ++i) {
        int status = 1;
        if (platform_wait_process(pids[i], &status) != 0) {
            rt_write_line(2, "sh: wait failed");
            return 1;
        }
        if (i + 1 == pipeline->count) {
            exit_status = status;
        }
    }

    return exit_status;
}
