#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define SORT_MAX_LINES 2048
#define SORT_MAX_LINE_LENGTH 512

static int store_line(char lines[SORT_MAX_LINES][SORT_MAX_LINE_LENGTH], size_t *count, const char *line, size_t len) {
    size_t copy_len = len;

    if (*count >= SORT_MAX_LINES) {
        return -1;
    }

    if (copy_len >= SORT_MAX_LINE_LENGTH) {
        copy_len = SORT_MAX_LINE_LENGTH - 1;
    }

    memcpy(lines[*count], line, copy_len);
    lines[*count][copy_len] = '\0';
    *count += 1;
    return 0;
}

static int collect_lines_from_fd(int fd, char lines[SORT_MAX_LINES][SORT_MAX_LINE_LENGTH], size_t *count) {
    char buffer[2048];
    char current[SORT_MAX_LINE_LENGTH];
    size_t current_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];

            if (ch == '\n') {
                if (store_line(lines, count, current, current_len) != 0) {
                    return -1;
                }
                current_len = 0;
            } else if (current_len + 1 < sizeof(current)) {
                current[current_len] = ch;
                current_len += 1;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (current_len > 0) {
        return store_line(lines, count, current, current_len);
    }

    return 0;
}

static void sort_lines(char lines[SORT_MAX_LINES][SORT_MAX_LINE_LENGTH], size_t count) {
    size_t i;
    size_t j;
    char tmp[SORT_MAX_LINE_LENGTH];

    for (i = 0; i < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            if (rt_strcmp(lines[j], lines[i]) < 0) {
                memcpy(tmp, lines[i], sizeof(tmp));
                memcpy(lines[i], lines[j], sizeof(tmp));
                memcpy(lines[j], tmp, sizeof(tmp));
            }
        }
    }
}

int main(int argc, char **argv) {
    char lines[SORT_MAX_LINES][SORT_MAX_LINE_LENGTH];
    size_t count = 0;
    int exit_code = 0;
    int i;

    if (argc == 1) {
        if (collect_lines_from_fd(0, lines, &count) != 0) {
            rt_write_line(2, "sort: read error");
            return 1;
        }
    } else {
        for (i = 1; i < argc; ++i) {
            int fd;
            int should_close;

            if (tool_open_input(argv[i], &fd, &should_close) != 0) {
                rt_write_cstr(2, "sort: cannot open ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
                continue;
            }

            if (collect_lines_from_fd(fd, lines, &count) != 0) {
                rt_write_cstr(2, "sort: read error on ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
            }

            tool_close_input(fd, should_close);
        }
    }

    sort_lines(lines, count);
    for (i = 0; i < (int)count; ++i) {
        rt_write_line(1, lines[i]);
    }

    return exit_code;
}
