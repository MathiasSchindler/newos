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

static size_t tool_buffer_append_char(char *buffer, size_t buffer_size, size_t length, char ch) {
    if (buffer_size == 0) {
        return 0;
    }

    if (length + 1 < buffer_size) {
        buffer[length] = ch;
        length += 1U;
        buffer[length] = '\0';
    } else {
        buffer[buffer_size - 1U] = '\0';
    }

    return length;
}

static size_t tool_buffer_append_cstr(char *buffer, size_t buffer_size, size_t length, const char *text) {
    size_t i = 0;

    while (text != 0 && text[i] != '\0') {
        length = tool_buffer_append_char(buffer, buffer_size, length, text[i]);
        i += 1U;
    }

    return length;
}

void tool_format_size(unsigned long long value, int human_readable, char *buffer, size_t buffer_size) {
    static const char units[] = { 'B', 'K', 'M', 'G', 'T', 'P' };
    size_t unit_index = 0;
    unsigned long long scaled = value;
    unsigned long long remainder = 0;
    char digits[32];
    size_t length = 0;

    if (buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (!human_readable) {
        rt_unsigned_to_string(value, buffer, buffer_size);
        return;
    }

    while (scaled >= 1024ULL && unit_index + 1U < sizeof(units)) {
        remainder = scaled % 1024ULL;
        scaled /= 1024ULL;
        unit_index += 1U;
    }

    rt_unsigned_to_string(scaled, digits, sizeof(digits));
    length = tool_buffer_append_cstr(buffer, buffer_size, length, digits);

    if (unit_index > 0U && scaled < 10ULL && remainder != 0ULL) {
        unsigned long long tenths = (remainder * 10ULL) / 1024ULL;
        length = tool_buffer_append_char(buffer, buffer_size, length, '.');
        length = tool_buffer_append_char(buffer, buffer_size, length, (char)('0' + (tenths > 9ULL ? 9ULL : tenths)));
    }

    (void)tool_buffer_append_char(buffer, buffer_size, length, units[unit_index]);
}

int tool_parse_uint_arg(const char *text, unsigned long long *value_out, const char *tool_name, const char *what) {
    if (text == 0 || rt_parse_uint(text, value_out) != 0) {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }
    return 0;
}

int tool_parse_int_arg(const char *text, long long *value_out, const char *tool_name, const char *what) {
    unsigned long long magnitude = 0;
    int negative = 0;

    if (text == 0 || value_out == 0 || text[0] == '\0') {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }

    if (text[0] == '-') {
        negative = 1;
        text += 1;
    } else if (text[0] == '+') {
        text += 1;
    }

    if (text[0] == '\0' || rt_parse_uint(text, &magnitude) != 0) {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }

    *value_out = negative ? -(long long)magnitude : (long long)magnitude;
    return 0;
}

int tool_parse_signal_name(const char *text, int *signal_out) {
    return platform_parse_signal_name(text, signal_out);
}

const char *tool_signal_name(int signal_number) {
    return platform_signal_name(signal_number);
}

void tool_write_signal_list(int fd) {
    platform_write_signal_list(fd);
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

static void tool_pop_path_component(char *path) {
    size_t len;

    if (path == 0) {
        return;
    }

    len = rt_strlen(path);
    while (len > 1U && path[len - 1U] == '/') {
        path[len - 1U] = '\0';
        len -= 1U;
    }

    while (len > 1U && path[len - 1U] != '/') {
        path[len - 1U] = '\0';
        len -= 1U;
    }

    while (len > 1U && path[len - 1U] == '/') {
        path[len - 1U] = '\0';
        len -= 1U;
    }

    if (len == 0U) {
        rt_copy_string(path, 2U, "/");
    }
}

static int tool_append_path_component(char *path, size_t buffer_size, const char *component) {
    size_t len;
    size_t component_len;

    if (path == 0 || component == 0) {
        return -1;
    }

    len = rt_strlen(path);
    component_len = rt_strlen(component);
    if (len == 0U) {
        if (buffer_size < 2U) {
            return -1;
        }
        path[0] = '/';
        path[1] = '\0';
        len = 1U;
    }

    if (!(len == 1U && path[0] == '/')) {
        if (len + 1U >= buffer_size) {
            return -1;
        }
        path[len++] = '/';
        path[len] = '\0';
    }

    if (len + component_len + 1U > buffer_size) {
        return -1;
    }

    memcpy(path + len, component, component_len + 1U);
    return 0;
}

static int tool_build_absolute_path(const char *path, char *buffer, size_t buffer_size) {
    char cwd[2048];

    if (path == 0 || buffer == 0 || buffer_size == 0U || path[0] == '\0') {
        return -1;
    }

    if (path[0] == '/') {
        if (rt_strlen(path) + 1U > buffer_size) {
            return -1;
        }
        rt_copy_string(buffer, buffer_size, path);
        return 0;
    }

    if (platform_get_current_directory(cwd, sizeof(cwd)) != 0) {
        return -1;
    }

    return tool_join_path(cwd, path, buffer, buffer_size);
}

static int tool_concat_path_suffix(const char *prefix, const char *suffix, char *buffer, size_t buffer_size) {
    size_t prefix_len;
    size_t suffix_index = 0;

    if (prefix == 0 || buffer == 0 || buffer_size == 0U) {
        return -1;
    }

    prefix_len = rt_strlen(prefix);
    if (prefix_len + 1U > buffer_size) {
        return -1;
    }

    memcpy(buffer, prefix, prefix_len + 1U);
    while (prefix_len > 1U && buffer[prefix_len - 1U] == '/' && suffix != 0 && suffix[suffix_index] == '/') {
        buffer[prefix_len - 1U] = '\0';
        prefix_len -= 1U;
    }

    while (suffix != 0 && suffix[suffix_index] != '\0') {
        if (prefix_len + 1U >= buffer_size) {
            return -1;
        }
        buffer[prefix_len++] = suffix[suffix_index++];
    }
    buffer[prefix_len] = '\0';
    return 0;
}

int tool_canonicalize_path(const char *path, int resolve_symlinks, int allow_missing, char *buffer, size_t buffer_size) {
    char pending[2048];
    char resolved[2048];
    size_t index = 0;
    unsigned int symlink_count = 0U;

    if (tool_build_absolute_path(path, pending, sizeof(pending)) != 0) {
        return -1;
    }

    rt_copy_string(resolved, sizeof(resolved), "/");

    while (pending[index] != '\0') {
        char component[256];
        size_t component_len = 0U;
        char candidate[2048];

        while (pending[index] == '/') {
            index += 1U;
        }
        if (pending[index] == '\0') {
            break;
        }

        while (pending[index] != '\0' && pending[index] != '/' && component_len + 1U < sizeof(component)) {
            component[component_len++] = pending[index++];
        }
        component[component_len] = '\0';

        while (pending[index] != '\0' && pending[index] != '/') {
            index += 1U;
        }

        if (rt_strcmp(component, ".") == 0) {
            continue;
        }

        if (rt_strcmp(component, "..") == 0) {
            tool_pop_path_component(resolved);
            continue;
        }

        rt_copy_string(candidate, sizeof(candidate), resolved);
        if (tool_append_path_component(candidate, sizeof(candidate), component) != 0) {
            return -1;
        }

        if (resolve_symlinks) {
            char target[2048];

            if (platform_read_symlink(candidate, target, sizeof(target)) == 0) {
                char replacement[2048];
                char remainder[2048];

                if (symlink_count >= 64U) {
                    return -1;
                }
                symlink_count += 1U;

                if (target[0] == '/') {
                    rt_copy_string(replacement, sizeof(replacement), target);
                } else {
                    if (tool_join_path(resolved, target, replacement, sizeof(replacement)) != 0) {
                        return -1;
                    }
                }

                rt_copy_string(remainder, sizeof(remainder), pending + index);
                if (tool_concat_path_suffix(replacement, remainder, pending, sizeof(pending)) != 0) {
                    return -1;
                }

                rt_copy_string(resolved, sizeof(resolved), "/");
                index = 0U;
                continue;
            }
        }

        if (!allow_missing) {
            PlatformDirEntry entry;
            if (platform_get_path_info(candidate, &entry) != 0) {
                return -1;
            }
        }

        rt_copy_string(resolved, sizeof(resolved), candidate);
    }

    if (resolved[0] == '\0') {
        rt_copy_string(resolved, sizeof(resolved), "/");
    }

    rt_copy_string(buffer, buffer_size, resolved);
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
