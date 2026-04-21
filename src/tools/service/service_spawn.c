#include "service_impl.h"

#include "platform.h"
#include "runtime.h"

#include <errno.h>

static int service_is_quote(char ch) {
    return ch == '\'' || ch == '"';
}

int service_split_command(const char *command, char *storage, size_t storage_size, char *argv_out[], size_t argv_capacity) {
    size_t used = 0U;
    size_t argc = 0U;
    size_t index = 0U;

    if (command == NULL || storage == NULL || argv_out == NULL || argv_capacity < 2U) {
        errno = EINVAL;
        return -1;
    }

    while (command[index] != '\0') {
        char quote = '\0';

        while (rt_is_space(command[index])) {
            index += 1U;
        }
        if (command[index] == '\0') {
            break;
        }
        if (argc + 1U >= argv_capacity) {
            errno = E2BIG;
            return -1;
        }

        argv_out[argc++] = storage + used;
        while (command[index] != '\0') {
            char ch = command[index];

            if (quote != '\0') {
                if (ch == quote) {
                    quote = '\0';
                    index += 1U;
                    continue;
                }
            } else {
                if (service_is_quote(ch)) {
                    quote = ch;
                    index += 1U;
                    continue;
                }
                if (rt_is_space(ch)) {
                    break;
                }
            }

            if (ch == '\\' && command[index + 1U] != '\0') {
                index += 1U;
                ch = command[index];
            }
            if (used + 1U >= storage_size) {
                errno = E2BIG;
                return -1;
            }
            storage[used++] = ch;
            index += 1U;
        }

        if (quote != '\0') {
            errno = EINVAL;
            return -1;
        }
        if (used + 1U >= storage_size) {
            errno = E2BIG;
            return -1;
        }
        storage[used++] = '\0';
    }

    argv_out[argc] = NULL;
    return argc > 0U ? 0 : -1;
}

int service_start_process(const ServiceConfig *config, int *pid_out) {
    char storage[SERVICE_COMMAND_CAPACITY];
    char *argv[SERVICE_MAX_ARGS];
    const char *output_path = NULL;
    int pid = -1;

    if (config == NULL || pid_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (service_split_command(config->command, storage, sizeof(storage), argv, SERVICE_MAX_ARGS) != 0) {
        return -1;
    }

    if (config->stdout_path[0] != '\0') {
        output_path = config->stdout_path;
    } else if (config->stderr_path[0] != '\0') {
        output_path = config->stderr_path;
    }

    if (platform_spawn_process(argv, -1, -1, NULL, output_path, 1, &pid) != 0) {
        return -1;
    }
    if (service_write_pidfile(config->pidfile, pid) != 0) {
        return -1;
    }

    *pid_out = pid;
    return 0;
}
