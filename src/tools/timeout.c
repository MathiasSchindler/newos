#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

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
            if (tool_parse_signal_name(argv[argi + 1], &signal_number) != 0) {
                tool_write_error("timeout", "invalid signal: ", argv[argi + 1]);
                return 125;
            }
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

    if (platform_wait_process_timeout(pid, timeout_seconds, kill_after, signal_number, preserve_status, &status) != 0) {
        tool_write_error("timeout", "wait failed", 0);
        return 125;
    }
    return status;
}
