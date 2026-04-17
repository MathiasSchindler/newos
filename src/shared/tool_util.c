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

void tool_write_usage(const char *program_name, const char *usage_suffix) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    if (usage_suffix != 0 && usage_suffix[0] != '\0') {
        rt_write_char(2, ' ');
        rt_write_cstr(2, usage_suffix);
    }
    rt_write_char(2, '\n');
}

void tool_write_error(const char *tool_name, const char *message, const char *detail) {
    rt_write_cstr(2, tool_name);
    rt_write_cstr(2, ": ");
    if (message != 0) {
        rt_write_cstr(2, message);
    }
    if (detail != 0) {
        rt_write_cstr(2, detail);
    }
    rt_write_char(2, '\n');
}

int tool_parse_uint_arg(const char *text, unsigned long long *value_out, const char *tool_name, const char *what) {
    if (text == 0 || rt_parse_uint(text, value_out) != 0) {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }
    return 0;
}

const char *tool_base_name(const char *path) {
    const char *last = path;
    size_t i = 0;

    if (path == 0) {
        return "";
    }

    while (path[i] != '\0') {
        if (path[i] == '/') {
            last = path + i + 1;
        }
        i += 1;
    }

    return last;
}

int tool_join_path(const char *dir_path, const char *name, char *buffer, size_t buffer_size) {
    return rt_join_path(dir_path, name, buffer, buffer_size);
}

int tool_wildcard_match(const char *pattern, const char *text) {
    if (pattern[0] == '\0') {
        return text[0] == '\0';
    }

    if (pattern[0] == '*') {
        return tool_wildcard_match(pattern + 1, text) || (text[0] != '\0' && tool_wildcard_match(pattern, text + 1));
    }

    if (pattern[0] == '?') {
        return text[0] != '\0' && tool_wildcard_match(pattern + 1, text + 1);
    }

    return pattern[0] == text[0] && tool_wildcard_match(pattern + 1, text + 1);
}

int tool_resolve_destination(const char *source_path, const char *dest_path, char *buffer, size_t buffer_size) {
    int is_directory = 0;
    size_t path_len;

    if (platform_path_is_directory(dest_path, &is_directory) == 0 && is_directory) {
        return tool_join_path(dest_path, tool_base_name(source_path), buffer, buffer_size);
    }

    path_len = rt_strlen(dest_path);
    if (path_len + 1 > buffer_size) {
        return -1;
    }

    memcpy(buffer, dest_path, path_len + 1);
    return 0;
}

int tool_copy_file(const char *source_path, const char *dest_path) {
    int src_fd = platform_open_read(source_path);
    int dst_fd;
    char buffer[4096];

    if (src_fd < 0) {
        return -1;
    }

    dst_fd = platform_open_write(dest_path, 0644U);
    if (dst_fd < 0) {
        platform_close(src_fd);
        return -1;
    }

    for (;;) {
        long bytes_read = platform_read(src_fd, buffer, sizeof(buffer));
        long offset = 0;

        if (bytes_read == 0) {
            break;
        }

        if (bytes_read < 0) {
            platform_close(src_fd);
            platform_close(dst_fd);
            return -1;
        }

        while (offset < bytes_read) {
            long bytes_written = platform_write(dst_fd, buffer + offset, (size_t)(bytes_read - offset));
            if (bytes_written <= 0) {
                platform_close(src_fd);
                platform_close(dst_fd);
                return -1;
            }
            offset += bytes_written;
        }
    }

    platform_close(src_fd);
    platform_close(dst_fd);
    return 0;
}
