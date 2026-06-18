#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int tool_open_input(const char *path, int *fd_out, int *should_close_out) {
    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        *fd_out = 0;
        *should_close_out = 0;
        return 0;
    }

    *fd_out = platform_open_read(path);
    if (*fd_out < 0) {
        return -1;
    }

    *should_close_out = 1;
    return 0;
}

void tool_close_input(int fd, int should_close) {
    if (should_close) {
        (void)platform_close(fd);
    }
}

static int tool_read_all_input_common(const char *path, unsigned char **data_out, size_t *size_out, const char *tool_name) {
    int fd;
    int should_close;
    unsigned char *buffer;
    size_t capacity = 65536U;
    size_t used = 0U;
    const char *display_path = path != 0 ? path : "stdin";

    *data_out = 0;
    *size_out = 0U;
    if (tool_open_input(path, &fd, &should_close) != 0) {
        if (tool_name != 0) tool_write_error(tool_name, "cannot open: ", display_path);
        return -1;
    }
    buffer = (unsigned char *)rt_malloc(capacity);
    if (buffer == 0) {
        tool_close_input(fd, should_close);
        if (tool_name != 0) tool_write_error(tool_name, "out of memory: ", display_path);
        return -1;
    }
    while (1) {
        long bytes_read;

        if (used == capacity) {
            size_t next_capacity = capacity * 2U;
            unsigned char *next;

            if (next_capacity <= capacity) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                if (tool_name != 0) tool_write_error(tool_name, "input too large: ", display_path);
                return -1;
            }
            next = (unsigned char *)rt_realloc(buffer, next_capacity);
            if (next == 0) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                if (tool_name != 0) tool_write_error(tool_name, "out of memory: ", display_path);
                return -1;
            }
            buffer = next;
            capacity = next_capacity;
        }
        bytes_read = platform_read(fd, buffer + used, capacity - used);
        if (bytes_read < 0) {
            rt_free(buffer);
            tool_close_input(fd, should_close);
            if (tool_name != 0) tool_write_error(tool_name, "read failed: ", display_path);
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }
        used += (size_t)bytes_read;
    }
    tool_close_input(fd, should_close);
    *data_out = buffer;
    *size_out = used;
    return 0;
}

int tool_read_all_input(const char *path, unsigned char **data_out, size_t *size_out) {
    return tool_read_all_input_common(path, data_out, size_out, 0);
}

int tool_read_all_input_report(const char *path, unsigned char **data_out, size_t *size_out, const char *tool_name) {
    return tool_read_all_input_common(path, data_out, size_out, tool_name);
}