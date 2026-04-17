#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

const char *platform_getenv(const char *name) {
    if (name == NULL || name[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }

    return getenv(name);
}

const char *platform_getenv_entry(size_t index) {
    size_t current_index = 0;
    char **current = environ;

    while (current != NULL && *current != NULL) {
        if (current_index == index) {
            return *current;
        }
        current += 1;
        current_index += 1;
    }

    return NULL;
}

int platform_setenv(const char *name, const char *value, int overwrite) {
    if (name == NULL || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    return setenv(name, value != NULL ? value : "", overwrite);
}

int platform_unsetenv(const char *name) {
    if (name == NULL || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    return unsetenv(name);
}

int platform_clearenv(void) {
    static char *empty_environment[] = { NULL };

    environ = empty_environment;
    return 0;
}

int platform_isatty(int fd) {
    return isatty(fd) ? 1 : 0;
}

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

int platform_send_signal(int pid, int signal_number) {
    return kill((pid_t)pid, signal_number);
}

static int decode_wait_status(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

static int sleep_uninterrupted(unsigned int seconds) {
    struct timespec req;
    struct timespec rem;

    req.tv_sec = (time_t)seconds;
    req.tv_nsec = 0;

    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) {
            return -1;
        }
        req = rem;
    }

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

    *exit_status_out = decode_wait_status(status);

    return 0;
}

int platform_wait_process_timeout(
    int pid,
    unsigned int timeout_seconds,
    unsigned int kill_after_seconds,
    int signal_number,
    int preserve_status,
    int *exit_status_out
) {
    unsigned int elapsed = 0;
    unsigned int after_signal = 0;
    int timed_out = 0;

    if (exit_status_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    for (;;) {
        int status = 0;
        pid_t waited = waitpid((pid_t)pid, &status, WNOHANG);

        if (waited == (pid_t)pid) {
            *exit_status_out = (timed_out && !preserve_status) ? 124 : decode_wait_status(status);
            return 0;
        }

        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (!timed_out && elapsed >= timeout_seconds) {
            (void)platform_send_signal(pid, signal_number);
            timed_out = 1;
            after_signal = 0;
        } else if (timed_out && kill_after_seconds > 0 && after_signal >= kill_after_seconds) {
            (void)platform_send_signal(pid, SIGKILL);
            kill_after_seconds = 0;
        }

        if (sleep_uninterrupted(1U) != 0) {
            return -1;
        }

        if (!timed_out) {
            elapsed += 1;
        } else {
            after_signal += 1;
        }
    }
}

static void init_process_entry(PlatformProcessEntry *entry, int pid, const char *fallback_name) {
    if (entry == NULL) {
        return;
    }

    entry->pid = pid;
    entry->ppid = 0;
    entry->uid = 0;
    entry->rss_kb = 0;
    posix_copy_string(entry->state, sizeof(entry->state), "?");
    posix_copy_string(entry->user, sizeof(entry->user), "?");
    posix_copy_string(entry->name, sizeof(entry->name), fallback_name != NULL ? fallback_name : "?");
}

#ifndef __APPLE__
static void fill_username(char *buffer, size_t buffer_size, unsigned int uid) {
    struct passwd *pw = getpwuid((uid_t)uid);

    if (pw != NULL && pw->pw_name != NULL && pw->pw_name[0] != '\0') {
        posix_copy_string(buffer, buffer_size, pw->pw_name);
    } else {
        (void)snprintf(buffer, buffer_size, "%u", uid);
    }
}

static void load_status_file(const char *status_path, PlatformProcessEntry *entry) {
    FILE *fp = fopen(status_path, "r");
    char line[512];

    if (fp == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, "Name:", 5) == 0) {
            char name[PLATFORM_NAME_CAPACITY];
            if (sscanf(line + 5, " %255s", name) == 1) {
                posix_copy_string(entry->name, sizeof(entry->name), name);
            }
        } else if (strncmp(line, "State:", 6) == 0) {
            char state[sizeof(entry->state)];
            if (sscanf(line + 6, " %15s", state) == 1) {
                posix_copy_string(entry->state, sizeof(entry->state), state);
            }
        } else if (strncmp(line, "PPid:", 5) == 0) {
            int ppid = 0;
            if (sscanf(line + 5, " %d", &ppid) == 1) {
                entry->ppid = ppid;
            }
        } else if (strncmp(line, "Uid:", 4) == 0) {
            unsigned int uid = 0;
            if (sscanf(line + 4, " %u", &uid) == 1) {
                entry->uid = uid;
                fill_username(entry->user, sizeof(entry->user), uid);
            }
        } else if (strncmp(line, "VmRSS:", 6) == 0) {
            unsigned long long rss = 0;
            if (sscanf(line + 6, " %llu", &rss) == 1) {
                entry->rss_kb = rss;
            }
        }
    }

    fclose(fp);
}
#endif

int platform_list_processes(PlatformProcessEntry *entries_out, size_t entry_capacity, size_t *count_out) {
    size_t count = 0;

    if (entries_out == NULL || count_out == NULL) {
        errno = EINVAL;
        return -1;
    }

#ifdef __APPLE__
    {
        FILE *pipe = popen("/bin/ps -axo pid=,ppid=,uid=,user=,state=,rss=,comm=", "r");
        char line[1024];

        if (pipe == NULL) {
            pipe = popen("ps -axo pid=,ppid=,uid=,user=,state=,rss=,comm=", "r");
        }
        if (pipe == NULL) {
            return -1;
        }

        while (fgets(line, sizeof(line), pipe) != NULL && count < entry_capacity) {
            int pid = 0;
            int ppid = 0;
            unsigned int uid = 0;
            unsigned long long rss = 0;
            char user[PLATFORM_NAME_CAPACITY];
            char state[16];
            char name[PLATFORM_NAME_CAPACITY];

            user[0] = '\0';
            state[0] = '\0';
            name[0] = '\0';

            if (sscanf(line, " %d %d %u %255s %15s %llu %255[^\n]", &pid, &ppid, &uid, user, state, &rss, name) == 7 && pid > 0) {
                init_process_entry(&entries_out[count], pid, name);
                entries_out[count].ppid = ppid;
                entries_out[count].uid = uid;
                entries_out[count].rss_kb = rss;
                posix_copy_string(entries_out[count].user, sizeof(entries_out[count].user), user);
                posix_copy_string(entries_out[count].state, sizeof(entries_out[count].state), state);
                count += 1;
            }
        }

        (void)pclose(pipe);
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
            char status_path[1024];

            if (!posix_is_digit_string(de->d_name)) {
                continue;
            }

            init_process_entry(&entries_out[count], posix_parse_pid_value(de->d_name), de->d_name);
            if (posix_join_path("/proc", de->d_name, proc_dir, sizeof(proc_dir)) == 0 &&
                posix_join_path(proc_dir, "status", status_path, sizeof(status_path)) == 0) {
                load_status_file(status_path, &entries_out[count]);
            }
            count += 1;
        }

        closedir(dir);
    }
#endif

    *count_out = count;
    return 0;
}
