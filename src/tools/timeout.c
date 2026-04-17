#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if __STDC_HOSTED__
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int parse_duration_seconds(const char *text, unsigned int *seconds_out) {
    unsigned long long value = 0;
    unsigned long long multiplier = 1;
    size_t i = 0;

    if (text == 0 || text[0] == '\0' || seconds_out == 0) {
        return -1;
    }

    while (text[i] >= '0' && text[i] <= '9') {
        value = (value * 10ULL) + (unsigned long long)(text[i] - '0');
        i += 1;
    }

    if (i == 0) {
        return -1;
    }

    if (text[i] == 'm' && text[i + 1] == '\0') {
        multiplier = 60ULL;
        i += 1;
    } else if (text[i] == 'h' && text[i + 1] == '\0') {
        multiplier = 3600ULL;
        i += 1;
    } else if (text[i] == 'd' && text[i + 1] == '\0') {
        multiplier = 86400ULL;
        i += 1;
    } else if (text[i] == 's' && text[i + 1] == '\0') {
        i += 1;
    }

    if (text[i] != '\0') {
        return -1;
    }

    *seconds_out = (unsigned int)(value * multiplier);
    return 0;
}

#if __STDC_HOSTED__
static int wait_with_timeout(int pid, unsigned int timeout_seconds, unsigned int kill_after, int signal_number, int preserve_status, int *status_out) {
    unsigned int elapsed = 0;
    unsigned int after_signal = 0;
    int timed_out = 0;

    for (;;) {
        int status = 0;
        pid_t waited = waitpid((pid_t)pid, &status, WNOHANG);

        if (waited == (pid_t)pid) {
            if (WIFEXITED(status)) {
                *status_out = (timed_out && !preserve_status) ? 124 : WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                *status_out = (timed_out && !preserve_status) ? 124 : (128 + WTERMSIG(status));
            } else {
                *status_out = (timed_out && !preserve_status) ? 124 : 1;
            }
            return 0;
        }

        if (waited < 0) {
            return -1;
        }

        if (!timed_out && elapsed >= timeout_seconds) {
            (void)platform_send_signal(pid, signal_number);
            timed_out = 1;
            after_signal = 0;
        } else if (timed_out && kill_after > 0 && after_signal >= kill_after) {
            (void)platform_send_signal(pid, SIGKILL);
            kill_after = 0;
        }

        if (sleep(1U) != 0) {
            return -1;
        }

        if (!timed_out) {
            elapsed += 1;
        } else {
            after_signal += 1;
        }
    }
}
#endif

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[--preserve-status] [-s SIGNAL] [-k SECONDS] SECONDS COMMAND [ARG ...]");
}

int main(int argc, char **argv) {
    unsigned int timeout_seconds = 0;
    unsigned int kill_after = 0;
    int signal_number = 15;
    int preserve_status = 0;
    int argi = 1;
    int pid;
    int status = 0;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "--preserve-status") == 0) {
            preserve_status = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-s") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 125;
            }
#if __STDC_HOSTED__
            if (tool_parse_signal_name(argv[argi + 1], &signal_number) != 0) {
                tool_write_error("timeout", "invalid signal: ", argv[argi + 1]);
                return 125;
            }
#endif
            argi += 2;
            continue;
        }
        if (rt_strcmp(argv[argi], "-k") == 0) {
            if (argi + 1 >= argc || parse_duration_seconds(argv[argi + 1], &kill_after) != 0) {
                print_usage(argv[0]);
                return 125;
            }
            argi += 2;
            continue;
        }
        break;
    }

    if (argc - argi < 2 || parse_duration_seconds(argv[argi], &timeout_seconds) != 0) {
        print_usage(argv[0]);
        return 125;
    }
    argi += 1;

    if (platform_spawn_process(&argv[argi], -1, -1, 0, 0, 0, &pid) != 0) {
        tool_write_error("timeout", "failed to execute ", argv[argi]);
        return 127;
    }

#if __STDC_HOSTED__
    if (wait_with_timeout(pid, timeout_seconds, kill_after, signal_number, preserve_status, &status) != 0) {
        tool_write_error("timeout", "wait failed", 0);
        return 125;
    }
    return status;
#else
    if (timeout_seconds > 0) {
        (void)platform_sleep_seconds(timeout_seconds);
        (void)platform_send_signal(pid, signal_number);
        if (kill_after > 0) {
            (void)platform_sleep_seconds(kill_after);
            (void)platform_send_signal(pid, 9);
        }
        if (platform_wait_process(pid, &status) != 0) {
            return 125;
        }
        return preserve_status ? status : 124;
    }

    if (platform_wait_process(pid, &status) != 0) {
        tool_write_error("timeout", "wait failed", 0);
        return 125;
    }

    return status;
#endif
}
