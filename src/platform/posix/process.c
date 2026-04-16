#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int platform_create_pipe(int pipe_fds[2]) {
    return pipe(pipe_fds);
}

int platform_spawn_process(
    char *const argv[],
    int stdin_fd,
    int stdout_fd,
    const char *input_path,
    const char *output_path,
    int output_append,
    int *pid_out
) {
    pid_t pid;

    if (argv == NULL || argv[0] == NULL || pid_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        int fd;

        if (input_path != NULL) {
            fd = open(input_path, O_RDONLY);
            if (fd < 0) {
                _exit(126);
            }
            if (fd != STDIN_FILENO) {
                if (dup2(fd, STDIN_FILENO) < 0) {
                    _exit(126);
                }
                close(fd);
            }
        } else if (stdin_fd >= 0 && stdin_fd != STDIN_FILENO) {
            if (dup2(stdin_fd, STDIN_FILENO) < 0) {
                _exit(126);
            }
        }

        if (output_path != NULL) {
            int flags = O_WRONLY | O_CREAT | (output_append ? O_APPEND : O_TRUNC);
            fd = open(output_path, flags, 0644);
            if (fd < 0) {
                _exit(126);
            }
            if (fd != STDOUT_FILENO) {
                if (dup2(fd, STDOUT_FILENO) < 0) {
                    _exit(126);
                }
                close(fd);
            }
        } else if (stdout_fd >= 0 && stdout_fd != STDOUT_FILENO) {
            if (dup2(stdout_fd, STDOUT_FILENO) < 0) {
                _exit(126);
            }
        }

        if (stdin_fd > STDERR_FILENO) {
            close(stdin_fd);
        }
        if (stdout_fd > STDERR_FILENO) {
            close(stdout_fd);
        }

        execvp(argv[0], argv);
        _exit(127);
    }

    *pid_out = (int)pid;
    return 0;
}

int platform_wait_process(int pid, int *exit_status_out) {
    int status;

    if (exit_status_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (waitpid((pid_t)pid, &status, 0) < 0) {
        return -1;
    }

    if (WIFEXITED(status)) {
        *exit_status_out = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        *exit_status_out = 128 + WTERMSIG(status);
    } else {
        *exit_status_out = 1;
    }

    return 0;
}

int platform_list_processes(PlatformProcessEntry *entries_out, size_t entry_capacity, size_t *count_out) {
    size_t count = 0;

    if (entries_out == NULL || count_out == NULL) {
        errno = EINVAL;
        return -1;
    }

#ifdef __APPLE__
    if (entry_capacity > 0) {
        entries_out[0].pid = (int)getpid();
        posix_copy_string(entries_out[0].name, sizeof(entries_out[0].name), "self");
        count = 1;
    }
#else
    {
        DIR *dir = opendir("/proc");
        struct dirent *de;

        if (dir == NULL) {
            return -1;
        }

        while ((de = readdir(dir)) != NULL && count < entry_capacity) {
            char proc_dir[1024];
            char comm_path[1024];
            int fd;
            ssize_t bytes_read;

            if (!posix_is_digit_string(de->d_name)) {
                continue;
            }

            entries_out[count].pid = posix_parse_pid_value(de->d_name);
            posix_copy_string(entries_out[count].name, sizeof(entries_out[count].name), de->d_name);

            if (posix_join_path("/proc", de->d_name, proc_dir, sizeof(proc_dir)) == 0 &&
                posix_join_path(proc_dir, "comm", comm_path, sizeof(comm_path)) == 0) {
                fd = open(comm_path, O_RDONLY);
                if (fd >= 0) {
                    bytes_read = read(fd, entries_out[count].name, sizeof(entries_out[count].name) - 1);
                    if (bytes_read > 0) {
                        entries_out[count].name[bytes_read] = '\0';
                        posix_trim_newline(entries_out[count].name);
                    }
                    close(fd);
                }
            }

            count += 1;
        }

        closedir(dir);
    }
#endif

    *count_out = count;
    return 0;
}
