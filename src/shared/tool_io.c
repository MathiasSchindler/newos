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

int tool_parse_escaped_string(const char *text, char *buffer, size_t buffer_size, size_t *length_out) {
    size_t in_index = 0;
    size_t out_index = 0;

    if (text == 0 || buffer == 0 || buffer_size == 0) {
        return -1;
    }

    while (text[in_index] != '\0') {
        char ch = text[in_index];

        if (ch == '\\' && text[in_index + 1U] != '\0') {
            in_index += 1U;
            ch = text[in_index];
            if (ch == 'n') {
                ch = '\n';
            } else if (ch == 'r') {
                ch = '\r';
            } else if (ch == 't') {
                ch = '\t';
            } else if (ch == 'f') {
                ch = '\f';
            } else if (ch == 'v') {
                ch = '\v';
            } else if (ch == '0') {
                ch = '\0';
            }
        }

        if (out_index + 1U >= buffer_size) {
            buffer[buffer_size - 1U] = '\0';
            return -1;
        }

        buffer[out_index++] = ch;
        in_index += 1U;
    }

    buffer[out_index] = '\0';
    if (length_out != 0) {
        *length_out = out_index;
    }
    return 0;
}

int tool_prompt_yes_no(const char *message, const char *path) {
    char ch = '\0';
    char answer = '\0';
    long bytes_read;

    if (message != 0) {
        rt_write_cstr(2, message);
    }
    if (path != 0) {
        rt_write_cstr(2, path);
    }
    rt_write_cstr(2, "? ");

    for (;;) {
        bytes_read = platform_read(0, &ch, 1U);
        if (bytes_read <= 0) {
            return 0;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            break;
        }
        if (answer == '\0' && ch != ' ' && ch != '\t') {
            answer = ch;
        }
    }

    return answer == 'y' || answer == 'Y';
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
