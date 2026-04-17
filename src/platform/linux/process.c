#include "platform.h"
#include "common.h"
#include "syscall.h"

static int linux_path_has_slash(const char *path) {
    unsigned long i = 0;

    while (path[i] != '\0') {
        if (path[i] == '/') {
            return 1;
        }
        i += 1;
    }

    return 0;
}

static void linux_child_exit(int status) {
    linux_syscall1(LINUX_SYS_EXIT, status);
    for (;;) {
    }
}

static void linux_try_exec(const char *path, char *const argv[]) {
    char *const envp[] = { 0 };
    linux_syscall3(LINUX_SYS_EXECVE, (long)path, (long)argv, (long)envp);
}

int platform_create_pipe(int pipe_fds[2]) {
    return linux_syscall2(LINUX_SYS_PIPE2, (long)pipe_fds, 0) < 0 ? -1 : 0;
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
    long pid;

    if (argv == 0 || argv[0] == 0 || pid_out == 0) {
        return -1;
    }

    pid = linux_syscall5(LINUX_SYS_CLONE, LINUX_SIGCHLD, 0, 0, 0, 0);
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        if (input_path != 0) {
            long fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)input_path, LINUX_O_RDONLY, 0);
            if (fd < 0) {
                linux_child_exit(126);
            }
            if (fd != 0) {
                if (linux_syscall3(LINUX_SYS_DUP3, fd, 0, 0) < 0) {
                    linux_child_exit(126);
                }
                linux_syscall1(LINUX_SYS_CLOSE, fd);
            }
        } else if (stdin_fd >= 0 && stdin_fd != 0) {
            if (linux_syscall3(LINUX_SYS_DUP3, stdin_fd, 0, 0) < 0) {
                linux_child_exit(126);
            }
        }

        if (output_path != 0) {
            long flags = LINUX_O_WRONLY | LINUX_O_CREAT | (output_append ? LINUX_O_APPEND : LINUX_O_TRUNC);
            long fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)output_path, flags, 0644);
            if (fd < 0) {
                linux_child_exit(126);
            }
            if (fd != 1) {
                if (linux_syscall3(LINUX_SYS_DUP3, fd, 1, 0) < 0) {
                    linux_child_exit(126);
                }
                linux_syscall1(LINUX_SYS_CLOSE, fd);
            }
        } else if (stdout_fd >= 0 && stdout_fd != 1) {
            if (linux_syscall3(LINUX_SYS_DUP3, stdout_fd, 1, 0) < 0) {
                linux_child_exit(126);
            }
        }

        if (stdin_fd > 1) {
            linux_syscall1(LINUX_SYS_CLOSE, stdin_fd);
        }
        if (stdout_fd > 1) {
            linux_syscall1(LINUX_SYS_CLOSE, stdout_fd);
        }

        if (linux_path_has_slash(argv[0])) {
            linux_try_exec(argv[0], argv);
        } else {
            char candidate[512];
            const char *prefixes[] = { "./", "/bin/", "/usr/bin/" };
            unsigned long i;

            for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
                if (linux_join_path(prefixes[i], argv[0], candidate, sizeof(candidate)) == 0) {
                    linux_try_exec(candidate, argv);
                }
            }
        }

        linux_child_exit(127);
    }

    *pid_out = (int)pid;
    return 0;
}

int platform_send_signal(int pid, int signal_number) {
    return linux_syscall2(LINUX_SYS_KILL, pid, signal_number) < 0 ? -1 : 0;
}

int platform_wait_process(int pid, int *exit_status_out) {
    int status = 0;
    long result;

    if (exit_status_out == 0) {
        return -1;
    }

    result = linux_syscall4(LINUX_SYS_WAIT4, pid, (long)&status, 0, 0);
    if (result < 0) {
        return -1;
    }

    if ((status & 0x7f) == 0) {
        *exit_status_out = (status >> 8) & 0xff;
    } else {
        *exit_status_out = 128 + (status & 0x7f);
    }

    return 0;
}

int platform_list_processes(PlatformProcessEntry *entries_out, size_t entry_capacity, size_t *count_out) {
    long fd;
    size_t count = 0;
    char buffer[4096];

    if (entries_out == 0 || count_out == 0) {
        return -1;
    }

    fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)"/proc", LINUX_O_RDONLY | LINUX_O_DIRECTORY, 0);
    if (fd < 0) {
        return -1;
    }

    for (;;) {
        long bytes = linux_syscall3(LINUX_SYS_GETDENTS64, fd, (long)buffer, sizeof(buffer));
        unsigned long offset = 0;

        if (bytes == 0) {
            break;
        }

        if (bytes < 0) {
            linux_syscall1(LINUX_SYS_CLOSE, fd);
            return -1;
        }

        while (offset < (unsigned long)bytes && count < entry_capacity) {
            struct linux_dirent64 *entry = (struct linux_dirent64 *)(buffer + offset);
            const char *name = entry->d_name;

            if (linux_is_digit_string(name)) {
                char proc_dir[1024];
                char comm_path[1024];
                long comm_fd;
                long read_result;

                entries_out[count].pid = linux_parse_pid_value(name);
                linux_copy_string(entries_out[count].name, sizeof(entries_out[count].name), name);

                if (linux_join_path("/proc", name, proc_dir, sizeof(proc_dir)) == 0 &&
                    linux_join_path(proc_dir, "comm", comm_path, sizeof(comm_path)) == 0) {
                    comm_fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)comm_path, LINUX_O_RDONLY, 0);
                    if (comm_fd >= 0) {
                        read_result = linux_syscall3(LINUX_SYS_READ, comm_fd, (long)entries_out[count].name, sizeof(entries_out[count].name) - 1);
                        if (read_result > 0) {
                            entries_out[count].name[read_result] = '\0';
                            linux_trim_newline(entries_out[count].name);
                        }
                        linux_syscall1(LINUX_SYS_CLOSE, comm_fd);
                    }
                }

                count += 1;
            }

            offset += entry->d_reclen;
        }
    }

    linux_syscall1(LINUX_SYS_CLOSE, fd);
    *count_out = count;
    return 0;
}
