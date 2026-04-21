#include "service_impl.h"

#include "platform.h"
#include "runtime.h"

#include <errno.h>

int service_read_pidfile(const char *path, int *pid_out) {
    char buffer[64];
    int fd;
    long bytes;
    int pid;

    if (path == NULL || pid_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }

    bytes = platform_read(fd, buffer, sizeof(buffer) - 1U);
    (void)platform_close(fd);
    if (bytes <= 0) {
        errno = EINVAL;
        return -1;
    }

    buffer[bytes] = '\0';
    rt_trim_newline(buffer);
    pid = rt_parse_pid_value(buffer);
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    *pid_out = pid;
    return 0;
}

int service_write_pidfile(const char *path, int pid) {
    char buffer[64];
    int fd;

    if (path == NULL || pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    rt_unsigned_to_string((unsigned long long)pid, buffer, sizeof(buffer));
    fd = platform_open_write(path, 0644U);
    if (fd < 0) {
        return -1;
    }
    if (rt_write_all(fd, buffer, rt_strlen(buffer)) != 0 || rt_write_all(fd, "\n", 1U) != 0) {
        (void)platform_close(fd);
        return -1;
    }
    return platform_close(fd);
}

int service_remove_pidfile(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (platform_remove_file(path) != 0 && errno != ENOENT) {
        return -1;
    }
    return 0;
}

int service_pid_is_running(int pid) {
    if (pid <= 0) {
        return 0;
    }
    if (platform_send_signal(pid, 0) == 0) {
        return 1;
    }
    return errno == EPERM ? 1 : 0;
}
