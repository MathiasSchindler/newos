#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#include <limits.h>

#define DMESG_USAGE "[-crwC] [-l LEVELS] [-n LEVEL]"
#define DMESG_BUFFER_CAPACITY 65536U
#define DMESG_LINE_CAPACITY 8192U

typedef struct {
    int any_enabled;
    int enabled[8];
} DmesgLevelFilter;

static int starts_with(const char *text, const char *prefix) {
    size_t i = 0U;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }

    return 1;
}

static void print_help(const char *program_name) {
    rt_write_cstr(1, "Usage: ");
    rt_write_cstr(1, program_name);
    rt_write_cstr(1, " " DMESG_USAGE "\n");
    rt_write_line(1, "Print the kernel message buffer, optionally follow new messages, or adjust the console log level.");
}

static int token_equals(const char *text, size_t length, const char *token) {
    size_t i = 0U;

    while (i < length && token[i] != '\0') {
        if (text[i] != token[i]) {
            return 0;
        }
        i += 1U;
    }

    return i == length && token[i] == '\0';
}

static int parse_level_token(const char *text, size_t length, unsigned int *level_out) {
    unsigned long long numeric = 0ULL;
    char token[16];
    size_t i;

    if (level_out == 0 || length == 0U || length >= sizeof(token)) {
        return -1;
    }

    for (i = 0U; i < length; ++i) {
        char ch = text[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        token[i] = ch;
    }
    token[length] = '\0';

    if (rt_parse_uint(token, &numeric) == 0 && numeric < 8ULL) {
        *level_out = (unsigned int)numeric;
        return 0;
    }

    if (token_equals(token, length, "emerg") || token_equals(token, length, "panic")) {
        *level_out = 0U;
    } else if (token_equals(token, length, "alert")) {
        *level_out = 1U;
    } else if (token_equals(token, length, "crit") || token_equals(token, length, "critical")) {
        *level_out = 2U;
    } else if (token_equals(token, length, "err") || token_equals(token, length, "error")) {
        *level_out = 3U;
    } else if (token_equals(token, length, "warn") || token_equals(token, length, "warning")) {
        *level_out = 4U;
    } else if (token_equals(token, length, "notice")) {
        *level_out = 5U;
    } else if (token_equals(token, length, "info")) {
        *level_out = 6U;
    } else if (token_equals(token, length, "debug")) {
        *level_out = 7U;
    } else {
        return -1;
    }

    return 0;
}

static int parse_level_list(const char *text, DmesgLevelFilter *filter) {
    size_t index = 0U;

    if (text == 0 || filter == 0) {
        return -1;
    }

    filter->any_enabled = 0;
    rt_memset(filter->enabled, 0, sizeof(filter->enabled));

    while (text[index] != '\0') {
        size_t start;
        size_t length;
        unsigned int level = 0U;

        while (text[index] == ',' || text[index] == ' ' || text[index] == '\t') {
            index += 1U;
        }
        start = index;
        while (text[index] != '\0' && text[index] != ',') {
            index += 1U;
        }
        length = index - start;
        while (length > 0U &&
               (text[start + length - 1U] == ' ' || text[start + length - 1U] == '\t')) {
            length -= 1U;
        }

        if (length == 0U) {
            continue;
        }
        if (parse_level_token(text + start, length, &level) != 0) {
            return -1;
        }

        filter->enabled[level] = 1;
        filter->any_enabled = 1;
    }

    return 0;
}

static int filter_allows_level(const DmesgLevelFilter *filter, unsigned int level) {
    if (filter == 0 || !filter->any_enabled) {
        return 1;
    }
    if (level >= sizeof(filter->enabled) / sizeof(filter->enabled[0])) {
        return 0;
    }
    return filter->enabled[level];
}

static const char *level_name(unsigned int level) {
    static const char *names[] = {
        "emerg", "alert", "crit", "err", "warn", "notice", "info", "debug"
    };

    if (level < sizeof(names) / sizeof(names[0])) {
        return names[level];
    }
    return "unknown";
}

static int parse_decimal_value(const char *text, size_t start, size_t end, unsigned long long *value_out) {
    unsigned long long value = 0ULL;
    size_t i = start;

    if (value_out == 0 || start >= end) {
        return -1;
    }

    while (i < end) {
        unsigned long long digit;

        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }
        digit = (unsigned long long)(text[i] - '0');
        if (value > (ULLONG_MAX - digit) / 10ULL) {
            return -1;
        }
        value = (value * 10ULL) + digit;
        i += 1U;
    }

    *value_out = value;
    return 0;
}

static int parse_angle_level(const char *line, size_t length, unsigned int *level_out, const char **message_out) {
    size_t index = 1U;
    unsigned long long value = 0ULL;

    if (length < 3U || line[0] != '<') {
        return -1;
    }
    while (index < length && line[index] != '>') {
        unsigned long long digit;

        if (line[index] < '0' || line[index] > '9') {
            return -1;
        }
        digit = (unsigned long long)(line[index] - '0');
        if (value > (ULLONG_MAX - digit) / 10ULL) {
            return -1;
        }
        value = (value * 10ULL) + digit;
        index += 1U;
    }
    if (index >= length || line[index] != '>') {
        return -1;
    }

    *level_out = (unsigned int)(value & 7ULL);
    *message_out = line + index + 1U;
    return 0;
}

static int parse_kmsg_record(const char *line,
                             size_t length,
                             unsigned int *level_out,
                             unsigned long long *timestamp_us_out,
                             const char **message_out) {
    size_t pos = 0U;
    size_t start = 0U;
    unsigned long long priority = 0ULL;
    unsigned long long timestamp = 0ULL;

    if (line == 0 || level_out == 0 || timestamp_us_out == 0 || message_out == 0) {
        return -1;
    }

    while (pos < length && line[pos] != ',') {
        pos += 1U;
    }
    if (pos >= length || parse_decimal_value(line, start, pos, &priority) != 0) {
        return -1;
    }

    pos += 1U;
    while (pos < length && line[pos] != ',') {
        pos += 1U;
    }
    if (pos >= length) {
        return -1;
    }

    start = pos + 1U;
    pos = start;
    while (pos < length && line[pos] != ',' && line[pos] != ';') {
        pos += 1U;
    }
    if (pos >= length || parse_decimal_value(line, start, pos, &timestamp) != 0) {
        return -1;
    }

    while (pos < length && line[pos] != ';') {
        pos += 1U;
    }
    if (pos >= length || line[pos] != ';') {
        return -1;
    }

    *level_out = (unsigned int)(priority & 7ULL);
    *timestamp_us_out = timestamp;
    *message_out = line + pos + 1U;
    return 0;
}

static void write_fraction_6(unsigned long long value) {
    char digits[6];
    int i;

    for (i = 5; i >= 0; --i) {
        digits[i] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }

    (void)platform_write(1, digits, sizeof(digits));
}

static void write_formatted_record(unsigned int level,
                                   unsigned long long timestamp_us,
                                   const char *message,
                                   size_t message_length) {
    rt_write_char(1, '[');
    rt_write_uint(1, timestamp_us / 1000000ULL);
    rt_write_char(1, '.');
    write_fraction_6(timestamp_us % 1000000ULL);
    rt_write_cstr(1, "] ");
    rt_write_cstr(1, level_name(level));
    rt_write_cstr(1, ": ");
    (void)tool_write_visible(1, message, message_length);
    rt_write_char(1, '\n');
}

static void write_plain_line(const char *text, size_t length) {
    (void)tool_write_visible(1, text, length);
    rt_write_char(1, '\n');
}

static void process_line(const char *line, size_t length, int raw_output, const DmesgLevelFilter *filter) {
    unsigned int level = 6U;
    unsigned long long timestamp_us = 0ULL;
    const char *message = line;
    size_t message_length = length;

    if (length == 0U) {
        return;
    }

    if (parse_kmsg_record(line, length, &level, &timestamp_us, &message) == 0) {
        if (!filter_allows_level(filter, level)) {
            return;
        }
        if (raw_output) {
            write_plain_line(line, length);
        } else {
            message_length = length - (size_t)(message - line);
            write_formatted_record(level, timestamp_us, message, message_length);
        }
        return;
    }

    if (parse_angle_level(line, length, &level, &message) == 0) {
        if (!filter_allows_level(filter, level)) {
            return;
        }
        if (raw_output) {
            write_plain_line(line, length);
        } else {
            message_length = length - (size_t)(message - line);
            write_plain_line(message, message_length);
        }
        return;
    }

    write_plain_line(line, length);
}

static int read_stream_lines(int fd, int raw_output, const DmesgLevelFilter *filter) {
    char buffer[4096];
    char line[DMESG_LINE_CAPACITY];
    size_t used = 0U;

    for (;;) {
        long bytes = platform_read(fd, buffer, sizeof(buffer));
        size_t index = 0U;

        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            break;
        }

        while (index < (size_t)bytes) {
            char ch = buffer[index++];
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                process_line(line, used, raw_output, filter);
                used = 0U;
                continue;
            }
            if (used + 1U < sizeof(line)) {
                line[used++] = ch;
                line[used] = '\0';
            }
        }
    }

    if (used > 0U) {
        process_line(line, used, raw_output, filter);
    }

    return 0;
}

static int open_follow_source(void) {
    int fd = platform_open_read("/dev/kmsg");
    if (fd >= 0) {
        return fd;
    }

    return platform_open_read("/proc/kmsg");
}

int main(int argc, char **argv) {
    ToolOptState options;
    DmesgLevelFilter filter;
    int raw_output = 0;
    int follow = 0;
    int clear_after_read = 0;
    int clear_only = 0;
    int set_level = -1;
    int parse_result;

    filter.any_enabled = 0;
    rt_memset(filter.enabled, 0, sizeof(filter.enabled));
    tool_opt_init(&options, argc, argv, tool_base_name(argv[0]), DMESG_USAGE);

    while ((parse_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-r") == 0 || rt_strcmp(options.flag, "--raw") == 0) {
            raw_output = 1;
        } else if (rt_strcmp(options.flag, "-w") == 0 || rt_strcmp(options.flag, "--follow") == 0) {
            follow = 1;
        } else if (rt_strcmp(options.flag, "-c") == 0 || rt_strcmp(options.flag, "--read-clear") == 0) {
            clear_after_read = 1;
        } else if (rt_strcmp(options.flag, "-C") == 0 || rt_strcmp(options.flag, "--clear") == 0) {
            clear_only = 1;
        } else if (rt_strcmp(options.flag, "-l") == 0 || rt_strcmp(options.flag, "--level") == 0) {
            if (tool_opt_require_value(&options) != 0 || parse_level_list(options.value, &filter) != 0) {
                tool_write_error("dmesg", "invalid level list", 0);
                return 1;
            }
        } else if (starts_with(options.flag, "--level=")) {
            if (parse_level_list(options.flag + 8, &filter) != 0) {
                tool_write_error("dmesg", "invalid level list", 0);
                return 1;
            }
        } else if (rt_strcmp(options.flag, "-n") == 0 || rt_strcmp(options.flag, "--console-level") == 0) {
            unsigned int level = 0U;

            if (tool_opt_require_value(&options) != 0 || parse_level_token(options.value, rt_strlen(options.value), &level) != 0) {
                tool_write_error("dmesg", "invalid console log level", 0);
                return 1;
            }
            set_level = (int)level;
        } else if (starts_with(options.flag, "--console-level=")) {
            unsigned int level = 0U;
            const char *value = options.flag + 16;

            if (parse_level_token(value, rt_strlen(value), &level) != 0) {
                tool_write_error("dmesg", "invalid console log level", 0);
                return 1;
            }
            set_level = (int)level;
        } else {
            tool_write_error("dmesg", "unknown option: ", options.flag);
            print_help(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        print_help(argv[0]);
        return 0;
    }
    if (options.argi < argc) {
        tool_write_error("dmesg", "unexpected argument: ", argv[options.argi]);
        print_help(argv[0]);
        return 1;
    }

    if (set_level >= 0 && platform_set_console_log_level(set_level) != 0) {
        tool_write_error("dmesg", "cannot set the console log level on this platform", 0);
        return 1;
    }
    if (clear_only) {
        if (platform_clear_kernel_log() != 0) {
            tool_write_error("dmesg", "cannot clear the kernel log buffer on this platform", 0);
            return 1;
        }
        if (!follow) {
            return 0;
        }
    }

    if (follow) {
        int fd = open_follow_source();
        if (fd < 0) {
            tool_write_error("dmesg", "live kernel log follow mode is not available", 0);
            return 1;
        }
        if (read_stream_lines(fd, raw_output, &filter) != 0) {
            tool_write_error("dmesg", "failed to read the kernel message stream", 0);
            (void)platform_close(fd);
            return 1;
        }
        (void)platform_close(fd);
        return 0;
    }

    {
        static char buffer[DMESG_BUFFER_CAPACITY];
        long bytes = platform_read_kernel_log(buffer, sizeof(buffer), clear_after_read);
        if (bytes < 0) {
            tool_write_error("dmesg", "kernel log is not accessible on this platform", 0);
            return 1;
        }

        if (raw_output) {
            (void)platform_write(1, buffer, (size_t)bytes);
            if (bytes > 0 && buffer[bytes - 1] != '\n') {
                rt_write_char(1, '\n');
            }
        } else {
            size_t index = 0U;
            size_t start = 0U;

            while (index < (size_t)bytes) {
                if (buffer[index] == '\n') {
                    process_line(buffer + start, index - start, 0, &filter);
                    start = index + 1U;
                }
                index += 1U;
            }
            if (start < (size_t)bytes) {
                process_line(buffer + start, (size_t)bytes - start, 0, &filter);
            }
        }
    }

    return 0;
}
