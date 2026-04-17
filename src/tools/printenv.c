#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static int write_with_separator(const char *text, int zero_terminated) {
    if (rt_write_cstr(1, text) != 0) {
        return -1;
    }

    return rt_write_char(1, zero_terminated ? '\0' : '\n');
}

int main(int argc, char **argv) {
    int zero_terminated = 0;
    int argi = 1;
    int exit_code = 0;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        if (rt_strcmp(argv[argi], "-0") == 0) {
            zero_terminated = 1;
            argi += 1;
            continue;
        }

        if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_usage(argv[0], "[-0] [NAME ...]");
            return 1;
        }

        break;
    }

    if (argi >= argc) {
        size_t index = 0;
        for (;;) {
            const char *current = platform_getenv_entry(index);
            if (current == 0) {
                break;
            }
            if (write_with_separator(current, zero_terminated) != 0) {
                return 1;
            }
            index += 1;
        }
        return 0;
    }

    for (; argi < argc; ++argi) {
        const char *value = platform_getenv(argv[argi]);
        if (value != 0) {
            if (write_with_separator(value, zero_terminated) != 0) {
                return 1;
            }
        } else {
            exit_code = 1;
        }
    }

    return exit_code;
}
