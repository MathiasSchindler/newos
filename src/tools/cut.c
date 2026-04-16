#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    unsigned long long start;
    unsigned long long end;
    int open_start;
    int open_end;
} CutRange;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " -c LIST [file ...]");
}

static int parse_range(const char *text, CutRange *range) {
    const char *dash = 0;
    size_t i = 0;
    char left[32];
    char right[32];
    size_t left_len = 0;
    size_t right_len = 0;

    rt_memset(range, 0, sizeof(*range));

    while (text[i] != '\0') {
        if (text[i] == '-') {
            dash = text + i;
            break;
        }
        i += 1;
    }

    if (dash == 0) {
        if (rt_parse_uint(text, &range->start) != 0 || range->start == 0) {
            return -1;
        }
        range->end = range->start;
        return 0;
    }

    i = 0;
    while (text[i] != '\0' && text[i] != '-' && i + 1 < sizeof(left)) {
        left[left_len++] = text[i++];
    }
    left[left_len] = '\0';

    if (text[i] == '-') {
        i += 1;
    }

    while (text[i] != '\0' && right_len + 1 < sizeof(right)) {
        right[right_len++] = text[i++];
    }
    right[right_len] = '\0';

    if (left_len == 0) {
        range->open_start = 1;
        range->start = 1;
    } else if (rt_parse_uint(left, &range->start) != 0 || range->start == 0) {
        return -1;
    }

    if (right_len == 0) {
        range->open_end = 1;
    } else if (rt_parse_uint(right, &range->end) != 0) {
        return -1;
    }

    if (!range->open_end && range->end < range->start) {
        return -1;
    }

    return 0;
}

static int in_range(unsigned long long position, const CutRange *range) {
    if (!range->open_start && position < range->start) {
        return 0;
    }

    if (!range->open_end && position > range->end) {
        return 0;
    }

    return position >= range->start;
}

static int cut_stream(int fd, const CutRange *range) {
    char buffer[4096];
    long bytes_read;
    unsigned long long position = 1;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];

            if (ch == '\n') {
                if (rt_write_char(1, '\n') != 0) {
                    return -1;
                }
                position = 1;
                continue;
            }

            if (in_range(position, range)) {
                if (rt_write_char(1, ch) != 0) {
                    return -1;
                }
            }

            position += 1;
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    CutRange range;
    int i;
    int exit_code = 0;

    if (argc < 3 || rt_strcmp(argv[1], "-c") != 0 || parse_range(argv[2], &range) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc == 3) {
        return cut_stream(0, &range) == 0 ? 0 : 1;
    }

    for (i = 3; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "cut: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (cut_stream(fd, &range) != 0) {
            rt_write_cstr(2, "cut: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
