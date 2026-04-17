#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "tool_util.h"

#if __STDC_HOSTED__
#include <stdlib.h>
extern char **environ;
#endif

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
#if __STDC_HOSTED__
        char **current = environ;
        while (current != 0 && *current != 0) {
            if (write_with_separator(*current, zero_terminated) != 0) {
                return 1;
            }
            current += 1;
        }
#endif
        return 0;
    }

    for (; argi < argc; ++argi) {
#if __STDC_HOSTED__
        const char *value = getenv(argv[argi]);
        if (value != 0) {
            if (write_with_separator(value, zero_terminated) != 0) {
                return 1;
            }
        } else {
            exit_code = 1;
        }
#else
        (void)zero_terminated;
        exit_code = 1;
#endif
    }

    return exit_code;
}
