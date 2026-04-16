#include "platform.h"
#include "runtime.h"

static int parse_octal_mode(const char *text, unsigned int *mode_out) {
    unsigned int value = 0;
    int i = 0;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[i] != '\0') {
        if (text[i] < '0' || text[i] > '7') {
            return -1;
        }
        value = (value * 8U) + (unsigned int)(text[i] - '0');
        i += 1;
    }

    *mode_out = value;
    return 0;
}

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " MODE path ...");
}

int main(int argc, char **argv) {
    unsigned int mode = 0;
    int i;

    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (parse_octal_mode(argv[1], &mode) != 0) {
        rt_write_line(2, "chmod: octal numeric mode required");
        return 1;
    }

    for (i = 2; i < argc; ++i) {
        if (platform_change_mode(argv[i], mode) != 0) {
            rt_write_cstr(2, "chmod: cannot change mode for ");
            rt_write_line(2, argv[i]);
            return 1;
        }
    }

    return 0;
}
