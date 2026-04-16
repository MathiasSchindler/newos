#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define DIFF_MAX_LINES 2048
#define DIFF_MAX_LINE_LENGTH 512

static int store_line(char lines[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH], size_t *count, const char *line, size_t len) {
    size_t copy_len = len;

    if (*count >= DIFF_MAX_LINES) {
        return -1;
    }
    if (copy_len >= DIFF_MAX_LINE_LENGTH) {
        copy_len = DIFF_MAX_LINE_LENGTH - 1U;
    }

    memcpy(lines[*count], line, copy_len);
    lines[*count][copy_len] = '\0';
    *count += 1U;
    return 0;
}

static int collect_lines_from_fd(int fd, char lines[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH], size_t *count) {
    char buffer[2048];
    char current[DIFF_MAX_LINE_LENGTH];
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
            } else if (current_len + 1U < sizeof(current)) {
                current[current_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (current_len > 0U) {
        return store_line(lines, count, current, current_len);
    }

    return 0;
}

int main(int argc, char **argv) {
    char left[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH];
    char right[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH];
    size_t left_count = 0;
    size_t right_count = 0;
    int fd;
    int should_close;
    size_t i;
    int differences = 0;

    if (argc != 3) {
        rt_write_line(2, "Usage: diff file1 file2");
        return 1;
    }

    if (tool_open_input(argv[1], &fd, &should_close) != 0 || collect_lines_from_fd(fd, left, &left_count) != 0) {
        rt_write_line(2, "diff: cannot read first file");
        return 1;
    }
    tool_close_input(fd, should_close);

    if (tool_open_input(argv[2], &fd, &should_close) != 0 || collect_lines_from_fd(fd, right, &right_count) != 0) {
        rt_write_line(2, "diff: cannot read second file");
        return 1;
    }
    tool_close_input(fd, should_close);

    for (i = 0; i < left_count || i < right_count; ++i) {
        const char *lhs = (i < left_count) ? left[i] : "";
        const char *rhs = (i < right_count) ? right[i] : "";

        if ((i < left_count) != (i < right_count) || rt_strcmp(lhs, rhs) != 0) {
            differences = 1;
            rt_write_cstr(1, "line ");
            rt_write_uint(1, (unsigned long long)(i + 1U));
            rt_write_line(1, ":");
            if (i < left_count) {
                rt_write_cstr(1, "< ");
                rt_write_line(1, lhs);
            }
            if (i < right_count) {
                rt_write_cstr(1, "> ");
                rt_write_line(1, rhs);
            }
        }
    }

    return differences;
}