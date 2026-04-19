#include "platform.h"
#include "common.h"
#include "syscall.h"
#include "signal_util.h"

_Static_assert(sizeof(struct linux_termios) <= PLATFORM_TERMINAL_STATE_CAPACITY, "PlatformTerminalState is too small");

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

typedef struct {
    const char *name;
    int value;
} LinuxSignalEntry;

static const LinuxSignalEntry LINUX_SIGNAL_TABLE[] = {
    { "HUP", LINUX_SIGHUP },
    { "INT", LINUX_SIGINT },
    { "QUIT", LINUX_SIGQUIT },
    { "ILL", LINUX_SIGILL },
    { "TRAP", LINUX_SIGTRAP },
    { "ABRT", LINUX_SIGABRT },
    { "BUS", LINUX_SIGBUS },
    { "FPE", LINUX_SIGFPE },
    { "KILL", LINUX_SIGKILL },
    { "USR1", LINUX_SIGUSR1 },
    { "SEGV", LINUX_SIGSEGV },
    { "USR2", LINUX_SIGUSR2 },
    { "PIPE", LINUX_SIGPIPE },
    { "ALRM", LINUX_SIGALRM },
    { "TERM", LINUX_SIGTERM },
    { "CHLD", LINUX_SIGCHLD },
    { "CONT", LINUX_SIGCONT },
    { "STOP", LINUX_SIGSTOP },
    { "TSTP", LINUX_SIGTSTP },
    { "TTIN", LINUX_SIGTTIN },
    { "TTOU", LINUX_SIGTTOU },
};

static int linux_decode_wait_status(int status) {
    if ((status & 0x7f) == 0) {
        return (status >> 8) & 0xff;
    }
    return 128 + (status & 0x7f);
}

int platform_parse_signal_name(const char *text, int *signal_out) {
    unsigned long long numeric = 0;
    size_t i;

    if (text == 0 || signal_out == 0 || text[0] == '\0') {
        return -1;
    }

    if (rt_parse_uint(text, &numeric) == 0) {
        *signal_out = (int)numeric;
        return 0;
    }

    for (i = 0; i < sizeof(LINUX_SIGNAL_TABLE) / sizeof(LINUX_SIGNAL_TABLE[0]); ++i) {
        if (signal_name_matches(text, LINUX_SIGNAL_TABLE[i].name)) {
            *signal_out = LINUX_SIGNAL_TABLE[i].value;
            return 0;
        }
    }

    return -1;
}

const char *platform_signal_name(int signal_number) {
    size_t i;

    for (i = 0; i < sizeof(LINUX_SIGNAL_TABLE) / sizeof(LINUX_SIGNAL_TABLE[0]); ++i) {
        if (LINUX_SIGNAL_TABLE[i].value == signal_number) {
            return LINUX_SIGNAL_TABLE[i].name;
        }
    }

    return "UNKNOWN";
}

void platform_write_signal_list(int fd) {
    size_t i;

    for (i = 0; i < sizeof(LINUX_SIGNAL_TABLE) / sizeof(LINUX_SIGNAL_TABLE[0]); ++i) {
        if (i > 0) {
            (void)platform_write(fd, " ", 1U);
        }
        (void)platform_write(fd, LINUX_SIGNAL_TABLE[i].name, rt_strlen(LINUX_SIGNAL_TABLE[i].name));
    }
    (void)platform_write(fd, "\n", 1U);
}

const char *platform_getenv(const char *name) {
    (void)name;
    return 0;
}

const char *platform_getenv_entry(size_t index) {
    (void)index;
    return 0;
}

int platform_setenv(const char *name, const char *value, int overwrite) {
    (void)name;
    (void)value;
    (void)overwrite;
    return 0;
}

int platform_unsetenv(const char *name) {
    (void)name;
    return 0;
}

int platform_clearenv(void) {
    return 0;
}

int platform_isatty(int fd) {
    struct linux_winsize winsize;

    return linux_syscall3(LINUX_SYS_IOCTL, fd, LINUX_TIOCGWINSZ, (long)&winsize) < 0 ? 0 : 1;
}

int platform_get_process_id(void) {
    long pid = linux_syscall0(LINUX_SYS_GETPID);
    return pid < 0 ? -1 : (int)pid;
}

int platform_random_bytes(unsigned char *buffer, size_t count) {
    long fd;
    size_t offset = 0;

    if (buffer == 0 && count != 0U) {
        return -1;
    }
    if (count == 0U) {
        return 0;
    }

    fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)"/dev/urandom", LINUX_O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }

    while (offset < count) {
        long bytes = linux_syscall3(LINUX_SYS_READ, fd, (long)(buffer + offset), (long)(count - offset));
        if (bytes <= 0) {
            linux_syscall1(LINUX_SYS_CLOSE, fd);
            return -1;
        }
        offset += (size_t)bytes;
    }

    linux_syscall1(LINUX_SYS_CLOSE, fd);
    return 0;
}

int platform_terminal_enable_raw_mode(int fd, PlatformTerminalState *state_out) {
    struct linux_termios saved;
    struct linux_termios raw;

    if (state_out == 0) {
        return -1;
    }

    if (linux_syscall3(LINUX_SYS_IOCTL, fd, LINUX_TCGETS, (long)&saved) < 0) {
        return -1;
    }

    memset(state_out, 0, sizeof(*state_out));
    memcpy(state_out->bytes, &saved, sizeof(saved));

    raw = saved;
    raw.c_lflag &= ~(LINUX_ICANON | LINUX_ECHO);
    raw.c_cc[LINUX_VMIN] = 1;
    raw.c_cc[LINUX_VTIME] = 0;

    return linux_syscall3(LINUX_SYS_IOCTL, fd, LINUX_TCSETS, (long)&raw) < 0 ? -1 : 0;
}

int platform_terminal_restore_mode(int fd, const PlatformTerminalState *state) {
    struct linux_termios saved;

    if (state == 0) {
        return -1;
    }

    memcpy(&saved, state->bytes, sizeof(saved));
    return linux_syscall3(LINUX_SYS_IOCTL, fd, LINUX_TCSETS, (long)&saved) < 0 ? -1 : 0;
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

int platform_ignore_signal(int signal_number) {
    (void)signal_number;
    return 0;
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

    *exit_status_out = linux_decode_wait_status(status);
    return 0;
}

int platform_wait_process_timeout(
    int pid,
    unsigned long long timeout_milliseconds,
    unsigned long long kill_after_milliseconds,
    int signal_number,
    int preserve_status,
    int *exit_status_out
) {
    unsigned long long elapsed = 0;
    unsigned long long after_signal = 0;
    int timed_out = 0;
    const unsigned long long poll_milliseconds = 50ULL;

    if (exit_status_out == 0) {
        return -1;
    }

    for (;;) {
        int status = 0;
        long waited = linux_syscall4(LINUX_SYS_WAIT4, pid, (long)&status, LINUX_WNOHANG, 0);

        if (waited == pid) {
            *exit_status_out = (timed_out && !preserve_status) ? 124 : linux_decode_wait_status(status);
            return 0;
        }

        if (waited < 0) {
            return -1;
        }

        if (!timed_out && elapsed >= timeout_milliseconds) {
            (void)platform_send_signal(pid, signal_number);
            timed_out = 1;
            after_signal = 0;
        } else if (timed_out && kill_after_milliseconds > 0 && after_signal >= kill_after_milliseconds) {
            (void)platform_send_signal(pid, LINUX_SIGKILL);
            kill_after_milliseconds = 0;
        }

        {
            unsigned long long sleep_for = poll_milliseconds;

            if (!timed_out && elapsed < timeout_milliseconds && timeout_milliseconds - elapsed < sleep_for) {
                sleep_for = timeout_milliseconds - elapsed;
            } else if (timed_out && kill_after_milliseconds > 0 &&
                       after_signal < kill_after_milliseconds &&
                       kill_after_milliseconds - after_signal < sleep_for) {
                sleep_for = kill_after_milliseconds - after_signal;
            }

            if (sleep_for == 0ULL) {
                sleep_for = 1ULL;
            }
            if (platform_sleep_milliseconds(sleep_for) != 0) {
                return -1;
            }

            if (!timed_out) {
                elapsed += sleep_for;
            } else {
                after_signal += sleep_for;
            }
        }
    }
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
                entries_out[count].ppid = 0;
                entries_out[count].uid = 0;
                entries_out[count].rss_kb = 0;
                linux_copy_string(entries_out[count].state, sizeof(entries_out[count].state), "?");
                linux_copy_string(entries_out[count].user, sizeof(entries_out[count].user), "?");
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
