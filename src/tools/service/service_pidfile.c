#include "service_impl.h"

#include "platform.h"
#include "runtime.h"

static const char *service_base_name(const char *path) {
    const char *last = path;
    size_t index = 0U;

    if (path == NULL) {
        return "";
    }
    while (path[index] != '\0') {
        if (path[index] == '/') {
            last = path + index + 1U;
        }
        index += 1U;
    }
    return last;
}

int service_read_pidfile_info(const char *path, int *pid_out, char *name_out, size_t name_capacity) {
    char buffer[256];
    int fd;
    long bytes;
    int pid = -1;

    if (path == NULL || pid_out == NULL) {
        return -1;
    }
    if (name_out != NULL && name_capacity > 0U) {
        name_out[0] = '\0';
    }

    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }

    bytes = platform_read(fd, buffer, sizeof(buffer) - 1U);
    (void)platform_close(fd);
    if (bytes <= 0) {
        return -1;
    }

    buffer[bytes] = '\0';
    if (rt_strncmp(buffer, "pid=", 4U) == 0) {
        char *cursor = buffer;

        while (*cursor != '\0') {
            char *line = cursor;
            char *line_end = cursor;

            while (*line_end != '\0' && *line_end != '\n') {
                line_end += 1U;
            }
            if (*line_end == '\n') {
                *line_end = '\0';
                cursor = line_end + 1U;
            } else {
                cursor = line_end;
            }

            if (rt_strncmp(line, "pid=", 4U) == 0) {
                pid = rt_parse_pid_value(line + 4U);
            } else if (name_out != NULL && name_capacity > 0U && rt_strncmp(line, "name=", 5U) == 0) {
                rt_copy_string(name_out, name_capacity, line + 5U);
            }
        }
    } else {
        rt_trim_newline(buffer);
        pid = rt_parse_pid_value(buffer);
    }

    if (pid <= 0) {
        return -1;
    }
    *pid_out = pid;
    return 0;
}

int service_read_pidfile(const char *path, int *pid_out) {
    return service_read_pidfile_info(path, pid_out, NULL, 0U);
}

int service_write_pidfile(const char *path, int pid, const char *process_name) {
    char buffer[256];
    char pid_text[64];
    size_t used = 0U;
    int fd;

    if (path == NULL || pid <= 0) {
        return -1;
    }

    rt_unsigned_to_string((unsigned long long)pid, pid_text, sizeof(pid_text));
    rt_copy_string(buffer, sizeof(buffer), "pid=");
    used = rt_strlen(buffer);
    rt_copy_string(buffer + used, sizeof(buffer) - used, pid_text);
    used = rt_strlen(buffer);
    rt_copy_string(buffer + used, sizeof(buffer) - used, "\n");
    used = rt_strlen(buffer);
    if (process_name != NULL && process_name[0] != '\0') {
        rt_copy_string(buffer + used, sizeof(buffer) - used, "name=");
        used = rt_strlen(buffer);
        rt_copy_string(buffer + used, sizeof(buffer) - used, service_base_name(process_name));
        used = rt_strlen(buffer);
        rt_copy_string(buffer + used, sizeof(buffer) - used, "\n");
    }

    fd = platform_open_create_exclusive(path, 0600U);
    if (fd < 0) {
        return -1;
    }
    if (rt_write_all(fd, buffer, rt_strlen(buffer)) != 0) {
        (void)platform_close(fd);
        return -1;
    }
    return platform_close(fd);
}

int service_remove_pidfile(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (platform_path_access(path, PLATFORM_ACCESS_EXISTS) != 0) {
        return 0;
    }
    if (platform_remove_file(path) != 0) {
        return -1;
    }
    return 0;
}

int service_pid_is_running(int pid, const char *expected_name) {
    if (pid <= 0) {
        return 0;
    }
    if (platform_send_signal(pid, 0) != 0) {
        return 0;
    }
    if (expected_name != NULL && expected_name[0] != '\0') {
        PlatformProcessEntry entries[1024];
        size_t count = 0U;
        size_t index;
        const char *expected_base = service_base_name(expected_name);

        if (platform_list_processes(entries, sizeof(entries) / sizeof(entries[0]), &count) != 0) {
            return 0;
        }
        for (index = 0U; index < count; ++index) {
            if (entries[index].pid == pid) {
                return rt_strcmp(service_base_name(entries[index].name), expected_base) == 0;
            }
        }
        return 0;
    }
    return 1;
}
