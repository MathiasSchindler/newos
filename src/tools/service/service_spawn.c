#include "service_impl.h"

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static int service_is_quote(char ch) {
    return ch == '\'' || ch == '"';
}

static int service_has_explicit_program_path(const char *text) {
    size_t index = 0U;

    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return 0;
    }
    while (text[index] != '\0') {
        if (text[index] == '/') {
            return 1;
        }
        index += 1U;
    }
    return 0;
}

static void service_parent_directory(const char *path, char *buffer, size_t buffer_size) {
    size_t length;

    if (buffer == NULL || buffer_size == 0U) {
        return;
    }
    if (path == NULL || path[0] == '\0') {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    rt_copy_string(buffer, buffer_size, path);
    length = rt_strlen(buffer);
    while (length > 0U && buffer[length - 1U] != '/') {
        buffer[length - 1U] = '\0';
        length -= 1U;
    }
    while (length > 1U && buffer[length - 1U] == '/') {
        buffer[length - 1U] = '\0';
        length -= 1U;
    }
    if (buffer[0] == '\0') {
        rt_copy_string(buffer, buffer_size, ".");
    }
}

static int service_validate_path_target(const char *path) {
    char symlink_target[SERVICE_PATH_CAPACITY];
    char parent[SERVICE_PATH_CAPACITY];
    PlatformDirEntry entry;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (platform_read_symlink(path, symlink_target, sizeof(symlink_target)) == 0) {
        return -1;
    }
    service_parent_directory(path, parent, sizeof(parent));
    if (tool_canonicalize_path(parent, 1, 0, parent, sizeof(parent)) != 0) {
        return -1;
    }
    if (platform_get_path_info(parent, &entry) != 0 || !entry.is_dir) {
        return -1;
    }
    if (platform_get_path_info(path, &entry) == 0 && entry.is_dir) {
        return -1;
    }
    return 0;
}

int service_split_command(const char *command, char *storage, size_t storage_size, char *argv_out[], size_t argv_capacity) {
    size_t used = 0U;
    size_t argc = 0U;
    size_t index = 0U;

    if (command == NULL || storage == NULL || argv_out == NULL || argv_capacity < 2U) {
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

            if (ch == '\\' && command[index + 1U] != '\0' && quote != '\'') {
                index += 1U;
                ch = command[index];
            }
            if (used + 1U >= storage_size) {
                return -1;
            }
            storage[used++] = ch;
            index += 1U;
        }

        if (quote != '\0') {
            return -1;
        }
        if (used + 1U >= storage_size) {
            return -1;
        }
        storage[used++] = '\0';
    }

    argv_out[argc] = NULL;
    return argc > 0U ? 0 : -1;
}

int service_start_process(const ServiceConfig *config, int *pid_out) {
    char storage[SERVICE_COMMAND_CAPACITY];
    char path_candidate[SERVICE_PATH_CAPACITY];
    char resolved_program[SERVICE_PATH_CAPACITY];
    char resolved_workdir[SERVICE_PATH_CAPACITY];
    char *argv[SERVICE_MAX_ARGS];
    const char *output_path = NULL;
    const char *workdir = NULL;
    const char *drop_user = NULL;
    const char *drop_group = NULL;
    int pid = -1;

    if (config == NULL || pid_out == NULL) {
        return -1;
    }

    if (service_split_command(config->command, storage, sizeof(storage), argv, SERVICE_MAX_ARGS) != 0) {
        return -1;
    }
    if (!service_has_explicit_program_path(argv[0])) {
        return -1;
    }

    if (config->workdir[0] != '\0') {
        if (tool_canonicalize_path(config->workdir, 1, 0, resolved_workdir, sizeof(resolved_workdir)) != 0) {
            return -1;
        }
        workdir = resolved_workdir;
    }
    if (config->drop_user[0] != '\0') {
        drop_user = config->drop_user;
    }
    if (config->drop_group[0] != '\0') {
        drop_group = config->drop_group;
    }

    if (argv[0][0] != '/' && workdir != NULL) {
        if (tool_join_path(workdir, argv[0], path_candidate, sizeof(path_candidate)) != 0) {
            return -1;
        }
        if (tool_canonicalize_path(path_candidate, 1, 0, resolved_program, sizeof(resolved_program)) != 0) {
            return -1;
        }
    } else {
        if (tool_canonicalize_path(argv[0], 1, 0, resolved_program, sizeof(resolved_program)) != 0) {
            return -1;
        }
    }
    argv[0] = resolved_program;

    if (config->stdout_path[0] != '\0') {
        output_path = config->stdout_path;
    } else if (config->stderr_path[0] != '\0') {
        output_path = config->stderr_path;
    }
    if (service_validate_path_target(config->pidfile) != 0 || service_validate_path_target(output_path) != 0) {
        return -1;
    }

    if (platform_spawn_process_ex(argv, -1, -1, NULL, output_path, 1, workdir, drop_user, drop_group, &pid) != 0) {
        return -1;
    }
    if (service_write_pidfile(config->pidfile, pid, argv[0]) != 0) {
        (void)platform_send_signal(pid, 15);
        (void)platform_wait_process_timeout(pid, 500ULL, 500ULL, 15, 0, &pid);
        return -1;
    }

    (void)platform_sleep_milliseconds(150ULL);
    {
        int finished = 0;
        int exit_status = 0;

        if (platform_poll_process_exit(pid, &finished, &exit_status) == 0 && finished) {
            (void)service_remove_pidfile(config->pidfile);
            return -1;
        }
    }

    *pid_out = pid;
    return 0;
}
