#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define LOGGER_LINE_CAPACITY 2048
#define LOGGER_TAG_CAPACITY 64

static void logger_usage(const char *program_name) {
    tool_write_usage(program_name, "[-s] [-t TAG] [-p PRIORITY] [-f FILE] [MESSAGE...]");
}

static size_t append_cstr(char *buffer, size_t buffer_size, size_t used, const char *text) {
    size_t i = 0U;

    if (buffer_size == 0U) {
        return used;
    }
    while (text != 0 && text[i] != '\0' && used + 1U < buffer_size) {
        buffer[used] = text[i];
        used += 1U;
        i += 1U;
    }
    buffer[used < buffer_size ? used : buffer_size - 1U] = '\0';
    return used;
}

static size_t append_char(char *buffer, size_t buffer_size, size_t used, char ch) {
    if (buffer_size == 0U) {
        return used;
    }
    if (used + 1U < buffer_size) {
        buffer[used] = ch;
        used += 1U;
    }
    buffer[used < buffer_size ? used : buffer_size - 1U] = '\0';
    return used;
}

static int name_equals(const char *text, size_t length, const char *name) {
    return rt_strlen(name) == length && rt_strncmp(text, name, length) == 0;
}

static int parse_level_name(const char *text, size_t length, int *level_out) {
    if (name_equals(text, length, "emerg") || name_equals(text, length, "panic")) {
        *level_out = 0;
    } else if (name_equals(text, length, "alert")) {
        *level_out = 1;
    } else if (name_equals(text, length, "crit") || name_equals(text, length, "critical")) {
        *level_out = 2;
    } else if (name_equals(text, length, "err") || name_equals(text, length, "error")) {
        *level_out = 3;
    } else if (name_equals(text, length, "warning") || name_equals(text, length, "warn")) {
        *level_out = 4;
    } else if (name_equals(text, length, "notice")) {
        *level_out = 5;
    } else if (name_equals(text, length, "info")) {
        *level_out = 6;
    } else if (name_equals(text, length, "debug")) {
        *level_out = 7;
    } else {
        return -1;
    }
    return 0;
}

static int parse_priority(const char *text, int *level_out) {
    unsigned long long numeric = 0ULL;
    size_t start = 0U;
    size_t end;
    size_t i;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }
    for (i = 0U; text[i] != '\0'; ++i) {
        if (text[i] == '.') {
            start = i + 1U;
        }
    }
    end = rt_strlen(text);
    if (start >= end) {
        return -1;
    }

    if (rt_parse_uint(text + start, &numeric) == 0) {
        if (numeric > 7ULL) {
            return -1;
        }
        *level_out = (int)numeric;
        return 0;
    }
    return parse_level_name(text + start, end - start, level_out);
}

static int write_log_line(int fd, int level, const char *tag, const char *message) {
    char line[LOGGER_LINE_CAPACITY];
    char number[32];
    size_t used = 0U;

    if (fd < 0) {
        return 0;
    }

    if (fd != 2) {
        used = append_char(line, sizeof(line), used, '<');
        rt_unsigned_to_string((unsigned long long)level, number, sizeof(number));
        used = append_cstr(line, sizeof(line), used, number);
        used = append_char(line, sizeof(line), used, '>');
    }
    used = append_cstr(line, sizeof(line), used, tag);
    used = append_char(line, sizeof(line), used, '[');
    rt_unsigned_to_string((unsigned long long)platform_get_process_id(), number, sizeof(number));
    used = append_cstr(line, sizeof(line), used, number);
    used = append_cstr(line, sizeof(line), used, "]: ");
    used = append_cstr(line, sizeof(line), used, message);
    used = append_char(line, sizeof(line), used, '\n');
    return rt_write_all(fd, line, used);
}

static int log_message(int log_fd, int mirror_stderr, int level, const char *tag, const char *message) {
    int status = 0;

    if (write_log_line(log_fd, level, tag, message) != 0) {
        status = -1;
    }
    if ((mirror_stderr || log_fd < 0) && write_log_line(2, level, tag, message) != 0) {
        status = -1;
    }
    return status;
}

static int read_stdin_and_log(int log_fd, int mirror_stderr, int level, const char *tag) {
    char line[LOGGER_LINE_CAPACITY];
    size_t used = 0U;
    char ch;
    long bytes;
    int status = 0;

    while ((bytes = platform_read(0, &ch, 1U)) > 0) {
        if (ch == '\n') {
            line[used] = '\0';
            if (log_message(log_fd, mirror_stderr, level, tag, line) != 0) {
                status = -1;
            }
            used = 0U;
        } else if (used + 1U < sizeof(line)) {
            line[used] = ch;
            used += 1U;
        }
    }
    if (bytes < 0) {
        return -1;
    }
    if (used > 0U) {
        line[used] = '\0';
        if (log_message(log_fd, mirror_stderr, level, tag, line) != 0) {
            status = -1;
        }
    }
    return status;
}

int main(int argc, char **argv) {
    const char *program_name = tool_base_name(argv[0]);
    const char *tag = "logger";
    const char *file_path = 0;
    int mirror_stderr = 0;
    int level = 6;
    int log_fd = -1;
    int argi = 1;
    char tag_buffer[LOGGER_TAG_CAPACITY];

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            logger_usage(program_name);
            return 0;
        }
        if (rt_strcmp(argv[argi], "-s") == 0) {
            mirror_stderr = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-t") == 0) {
            argi += 1;
            if (argi >= argc || argv[argi][0] == '\0') {
                logger_usage(program_name);
                return 1;
            }
            rt_copy_string(tag_buffer, sizeof(tag_buffer), argv[argi]);
            tag = tag_buffer;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-p") == 0) {
            argi += 1;
            if (argi >= argc || parse_priority(argv[argi], &level) != 0) {
                tool_write_error(program_name, "invalid priority", 0);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-f") == 0) {
            argi += 1;
            if (argi >= argc) {
                logger_usage(program_name);
                return 1;
            }
            file_path = argv[argi];
            argi += 1;
        } else {
            tool_write_error(program_name, "unknown option: ", argv[argi]);
            logger_usage(program_name);
            return 1;
        }
    }

    if (file_path != 0) {
        log_fd = platform_open_append(file_path, 0644U);
        if (log_fd < 0) {
            tool_write_error(program_name, "cannot open ", file_path);
            return 1;
        }
    } else {
        log_fd = platform_open_append_existing("/dev/kmsg");
    }

    if (argi < argc) {
        char message[LOGGER_LINE_CAPACITY];
        size_t used = 0U;

        while (argi < argc) {
            if (used > 0U) {
                used = append_char(message, sizeof(message), used, ' ');
            }
            used = append_cstr(message, sizeof(message), used, argv[argi]);
            argi += 1;
        }
        if (log_message(log_fd, mirror_stderr, level, tag, message) != 0) {
            if (log_fd > 2) {
                platform_close(log_fd);
            }
            return 1;
        }
    } else if (read_stdin_and_log(log_fd, mirror_stderr, level, tag) != 0) {
        if (log_fd > 2) {
            platform_close(log_fd);
        }
        return 1;
    }

    if (log_fd > 2) {
        platform_close(log_fd);
    }
    return 0;
}
