#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define TAC_BUFFER_CAPACITY 1048576
#define TAC_MAX_RECORDS 4096

typedef struct {
    char separator[32];
    size_t separator_len;
} TacOptions;

static int store_record(
    size_t starts[TAC_MAX_RECORDS],
    size_t lengths[TAC_MAX_RECORDS],
    size_t *count,
    size_t start,
    size_t length
) {
    if (*count >= TAC_MAX_RECORDS) {
        return -1;
    }

    starts[*count] = start;
    lengths[*count] = length;
    *count += 1U;
    return 0;
}

static int print_usage_and_fail(const char *program_name) {
    tool_write_usage(program_name, "[-s SEP|-0] [file ...]");
    return 1;
}

static int match_separator(const char *buffer, size_t length, size_t start, const TacOptions *options) {
    size_t i;

    if (start + options->separator_len > length) {
        return 0;
    }

    for (i = 0; i < options->separator_len; ++i) {
        if (buffer[start + i] != options->separator[i]) {
            return 0;
        }
    }

    return 1;
}

static int tac_stream(int fd, const TacOptions *options) {
    static char buffer[TAC_BUFFER_CAPACITY];
    static size_t starts[TAC_MAX_RECORDS];
    static size_t lengths[TAC_MAX_RECORDS];
    char chunk[4096];
    size_t buffer_len = 0;
    size_t record_start = 0;
    size_t count = 0;
    size_t pos = 0;
    int trailing_separator = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        if (buffer_len + (size_t)bytes_read > sizeof(buffer)) {
            return -1;
        }
        memcpy(buffer + buffer_len, chunk, (size_t)bytes_read);
        buffer_len += (size_t)bytes_read;
    }

    if (bytes_read < 0 || options->separator_len == 0U) {
        return -1;
    }

    while (pos + options->separator_len <= buffer_len) {
        if (match_separator(buffer, buffer_len, pos, options)) {
            if (store_record(starts, lengths, &count, record_start, pos - record_start) != 0) {
                return -1;
            }
            pos += options->separator_len;
            record_start = pos;
        } else {
            pos += 1U;
        }
    }

    if (record_start < buffer_len) {
        if (store_record(starts, lengths, &count, record_start, buffer_len - record_start) != 0) {
            return -1;
        }
    }

    if (buffer_len >= options->separator_len) {
        trailing_separator = match_separator(buffer, buffer_len, buffer_len - options->separator_len, options);
    }

    while (count > 0U) {
        count -= 1U;

        if (lengths[count] > 0U && rt_write_all(1, buffer + starts[count], lengths[count]) != 0) {
            return -1;
        }

        if (count > 0U || trailing_separator) {
            if (rt_write_all(1, options->separator, options->separator_len) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    TacOptions options;
    int i = 1;
    int exit_code = 0;

    options.separator[0] = '\n';
    options.separator[1] = '\0';
    options.separator_len = 1U;

    while (i < argc && argv[i][0] == '-') {
        if (rt_strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc ||
                tool_parse_escaped_string(argv[i + 1], options.separator, sizeof(options.separator), &options.separator_len) != 0 ||
                options.separator_len == 0U) {
                return print_usage_and_fail(argv[0]);
            }
            i += 2;
        } else if (rt_strcmp(argv[i], "-0") == 0) {
            options.separator[0] = '\0';
            options.separator[1] = '\0';
            options.separator_len = 1U;
            i += 1;
        } else if (rt_strcmp(argv[i], "--") == 0) {
            i += 1;
            break;
        } else if (rt_strcmp(argv[i], "-") == 0) {
            break;
        } else {
            return print_usage_and_fail(argv[0]);
        }
    }

    if (i == argc) {
        return tac_stream(0, &options) == 0 ? 0 : 1;
    }

    for (; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            tool_write_error("tac", "cannot open ", argv[i]);
            exit_code = 1;
            continue;
        }

        if (tac_stream(fd, &options) != 0) {
            tool_write_error("tac", "read error on ", argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
