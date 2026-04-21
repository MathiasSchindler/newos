#include "platform.h"
#include "common.h"
#include "syscall.h"
#include "signal_util.h"

_Static_assert(sizeof(struct linux_termios) <= PLATFORM_TERMINAL_STATE_CAPACITY, "PlatformTerminalState is too small");

#define LINUX_ENV_MAX_ENTRIES 256
#define LINUX_ENV_ENTRY_CAPACITY 1024

static char linux_env_storage[LINUX_ENV_MAX_ENTRIES][LINUX_ENV_ENTRY_CAPACITY];
static char linux_env_raw[LINUX_ENV_MAX_ENTRIES * LINUX_ENV_ENTRY_CAPACITY];
static char *linux_env_entries[LINUX_ENV_MAX_ENTRIES + 1];
static size_t linux_env_count = 0U;
static int linux_env_initialized = 0;

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

static int linux_is_valid_env_name(const char *name) {
    size_t i = 0U;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    if (!((name[0] >= 'A' && name[0] <= 'Z') ||
          (name[0] >= 'a' && name[0] <= 'z') ||
          name[0] == '_')) {
        return 0;
    }
    while (name[i] != '\0') {
        if (!((name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= '0' && name[i] <= '9') ||
              name[i] == '_')) {
            return 0;
        }
        i += 1U;
    }
    return 1;
}

static int linux_env_name_matches(const char *entry, const char *name) {
    size_t i = 0U;

    if (entry == 0 || name == 0) {
        return 0;
    }
    while (name[i] != '\0' && entry[i] == name[i]) {
        i += 1U;
    }
    return name[i] == '\0' && entry[i] == '=';
}

static int linux_write_env_entry(size_t index, const char *name, const char *value) {
    size_t name_len;
    size_t value_len;

    if (index >= LINUX_ENV_MAX_ENTRIES || name == 0) {
        return -1;
    }
    name_len = rt_strlen(name);
    value_len = value != 0 ? rt_strlen(value) : 0U;
    if (name_len + 1U + value_len + 1U > sizeof(linux_env_storage[index])) {
        return -1;
    }
    memcpy(linux_env_storage[index], name, name_len);
    linux_env_storage[index][name_len] = '=';
    if (value_len > 0U) {
        memcpy(linux_env_storage[index] + name_len + 1U, value, value_len);
    }
    linux_env_storage[index][name_len + 1U + value_len] = '\0';
    linux_env_entries[index] = linux_env_storage[index];
    return 0;
}

static void linux_env_ensure_loaded(void) {
    long fd;
    long bytes;
    size_t start = 0U;
    size_t i;

    if (linux_env_initialized) {
        return;
    }
    linux_env_initialized = 1;
    linux_env_count = 0U;
    fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)"/proc/self/environ", LINUX_O_RDONLY, 0);
    if (fd < 0) {
        linux_env_entries[0] = 0;
        return;
    }
    bytes = linux_syscall3(LINUX_SYS_READ, fd, (long)linux_env_raw, (long)(sizeof(linux_env_raw) - 1U));
    linux_syscall1(LINUX_SYS_CLOSE, fd);
    if (bytes <= 0) {
        linux_env_entries[0] = 0;
        return;
    }
    linux_env_raw[bytes] = '\0';
    for (i = 0U; i < (size_t)bytes && linux_env_count < LINUX_ENV_MAX_ENTRIES; ++i) {
        if (linux_env_raw[i] == '\0') {
            if (i > start) {
                rt_copy_string(linux_env_storage[linux_env_count],
                               sizeof(linux_env_storage[linux_env_count]),
                               linux_env_raw + start);
                linux_env_entries[linux_env_count] = linux_env_storage[linux_env_count];
                linux_env_count += 1U;
            }
            start = i + 1U;
        }
    }
    if (start < (size_t)bytes && linux_env_count < LINUX_ENV_MAX_ENTRIES) {
        rt_copy_string(linux_env_storage[linux_env_count],
                       sizeof(linux_env_storage[linux_env_count]),
                       linux_env_raw + start);
        linux_env_entries[linux_env_count] = linux_env_storage[linux_env_count];
        linux_env_count += 1U;
    }
    linux_env_entries[linux_env_count] = 0;
}

static int linux_find_env_index(const char *name) {
    size_t i;

    linux_env_ensure_loaded();
    for (i = 0U; i < linux_env_count; ++i) {
        if (linux_env_name_matches(linux_env_entries[i], name)) {
            return (int)i;
        }
    }
    return -1;
}

static void linux_child_exit(int status) {
    linux_syscall1(LINUX_SYS_EXIT, status);
    for (;;) {
    }
}

static void linux_try_exec(const char *path, char *const argv[]) {
    linux_env_ensure_loaded();
    linux_syscall3(LINUX_SYS_EXECVE, (long)path, (long)argv, (long)linux_env_entries);
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
    int index;
    char *entry;

    if (!linux_is_valid_env_name(name)) {
        return 0;
    }
    index = linux_find_env_index(name);
    if (index < 0) {
        return 0;
    }
    entry = linux_env_entries[index];
    while (*entry != '\0' && *entry != '=') {
        entry += 1;
    }
    return *entry == '=' ? entry + 1 : 0;
}

const char *platform_getenv_entry(size_t index) {
    linux_env_ensure_loaded();
    return index < linux_env_count ? linux_env_entries[index] : 0;
}

int platform_setenv(const char *name, const char *value, int overwrite) {
    int index;

    if (!linux_is_valid_env_name(name)) {
        return -1;
    }
    linux_env_ensure_loaded();
    index = linux_find_env_index(name);
    if (index >= 0) {
        if (!overwrite) {
            return 0;
        }
        return linux_write_env_entry((size_t)index, name, value != 0 ? value : "");
    }
    if (linux_env_count >= LINUX_ENV_MAX_ENTRIES) {
        return -1;
    }
    if (linux_write_env_entry(linux_env_count, name, value != 0 ? value : "") != 0) {
        return -1;
    }
    linux_env_count += 1U;
    linux_env_entries[linux_env_count] = 0;
    return 0;
}

int platform_unsetenv(const char *name) {
    int index;
    size_t i;

    if (!linux_is_valid_env_name(name)) {
        return -1;
    }
    index = linux_find_env_index(name);
    if (index < 0) {
        return 0;
    }
    for (i = (size_t)index; i + 1U < linux_env_count; ++i) {
        rt_copy_string(linux_env_storage[i], sizeof(linux_env_storage[i]), linux_env_entries[i + 1U]);
        linux_env_entries[i] = linux_env_storage[i];
    }
    if (linux_env_count > 0U) {
        linux_env_count -= 1U;
        linux_env_storage[linux_env_count][0] = '\0';
        linux_env_entries[linux_env_count] = 0;
    }
    return 0;
}

int platform_clearenv(void) {
    linux_env_ensure_loaded();
    linux_env_count = 0U;
    linux_env_entries[0] = 0;
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

long platform_read_kernel_log(char *buffer, size_t buffer_size, int clear_after_read) {
    long bytes;
    long action;

    if (buffer == 0 || buffer_size == 0U) {
        return -1;
    }

    action = clear_after_read ? 4 : 3;
    bytes = linux_syscall3(LINUX_SYS_SYSLOG, action, (long)buffer, (long)(buffer_size - 1U));
    if (bytes < 0) {
        return -1;
    }

    buffer[bytes] = '\0';
    return bytes;
}

int platform_clear_kernel_log(void) {
    return linux_syscall3(LINUX_SYS_SYSLOG, 5, 0, 0) < 0 ? -1 : 0;
}

int platform_set_console_log_level(int level) {
    return linux_syscall3(LINUX_SYS_SYSLOG, 8, 0, level) < 0 ? -1 : 0;
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
    raw.c_iflag &= ~(LINUX_BRKINT | LINUX_ICRNL | LINUX_INPCK | LINUX_ISTRIP | LINUX_IXON);
    raw.c_oflag &= ~LINUX_OPOST;
    raw.c_cflag |= LINUX_CS8;
    raw.c_lflag &= ~(LINUX_ECHO | LINUX_ICANON | LINUX_IEXTEN | LINUX_ISIG);
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

        if (output_path != 0 || stdout_fd >= 0) {
            if (linux_syscall3(LINUX_SYS_DUP3, 1, 2, 0) < 0) {
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

int platform_shutdown_system(int action) {
    unsigned long command = LINUX_REBOOT_CMD_POWER_OFF;

    if (action == PLATFORM_SHUTDOWN_REBOOT) {
        command = LINUX_REBOOT_CMD_RESTART;
    } else if (action == PLATFORM_SHUTDOWN_HALT) {
        command = LINUX_REBOOT_CMD_HALT;
    }

    (void)platform_sync_all();
    return linux_syscall4(LINUX_SYS_REBOOT,
                          LINUX_REBOOT_MAGIC1,
                          LINUX_REBOOT_MAGIC2,
                          command,
                          0) < 0 ? -1 : 0;
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
