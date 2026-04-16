#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define STRINGS_BUFFER_CAPACITY 1024

static int is_printable_byte(unsigned char ch) {
    return ch >= 32U && ch <= 126U;
}

static int flush_sequence(char *buffer, size_t *length, size_t min_length) {
    if (*length >= min_length) {
        buffer[*length] = '\0';
        if (rt_write_line(1, buffer) != 0) {
            return -1;
        }
    }
    *length = 0;
    return 0;
}

static int strings_stream(int fd, size_t min_length) {
    unsigned char input[4096];
    char current[STRINGS_BUFFER_CAPACITY];
    size_t current_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, input, sizeof(input))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (is_printable_byte(input[i])) {
                if (current_len + 1U < sizeof(current)) {
                    current[current_len++] = (char)input[i];
                }
            } else if (flush_sequence(current, &current_len, min_length) != 0) {
                return -1;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    return flush_sequence(current, &current_len, min_length);
}

int main(int argc, char **argv) {
    size_t min_length = 4U;
    int argi = 1;
    int exit_code = 0;
    int i;

    if (argc > 2 && rt_strcmp(argv[1], "-n") == 0) {
        unsigned long long parsed = 0;
        if (rt_parse_uint(argv[2], &parsed) != 0 || parsed == 0ULL) {
            rt_write_line(2, "strings: invalid minimum length");
            return 1;
        }
        min_length = (size_t)parsed;
        argi = 3;
    }

    if (argi == argc) {
        return strings_stream(0, min_length) == 0 ? 0 : 1;
    }

    for (i = argi; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "strings: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (strings_stream(fd, min_length) != 0) {
            rt_write_cstr(2, "strings: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}