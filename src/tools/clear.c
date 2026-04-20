#include "runtime.h"

static int string_equals(const char *left, const char *right) {
    size_t i = 0;

    if (left == 0 || right == 0) {
        return 0;
    }

    while (left[i] != '\0' && right[i] != '\0') {
        if (left[i] != right[i]) {
            return 0;
        }
        i += 1;
    }

    return left[i] == right[i];
}

static int write_usage(const char *program_name, int fd) {
    if (rt_write_cstr(fd, "Usage: ") != 0 ||
        rt_write_cstr(fd, program_name != 0 ? program_name : "clear") != 0 ||
        rt_write_cstr(fd, "\n") != 0) {
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    static const char sequence[] = "\x1b[H\x1b[2J\x1b[3J";

    if (argc > 1) {
        if (string_equals(argv[1], "-h") || string_equals(argv[1], "--help")) {
            return write_usage(argv[0], 1);
        }
        (void)write_usage(argv[0], 2);
        return 1;
    }

    return rt_write_all(1, sequence, sizeof(sequence) - 1U) == 0 ? 0 : 1;
}
