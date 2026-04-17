#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if __STDC_HOSTED__
#include <stdlib.h>
extern char **environ;
#endif

static int is_assignment(const char *text) {
    size_t i = 0;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }

    while (text[i] != '\0') {
        if (text[i] == '=') {
            return i > 0;
        }
        i += 1;
    }

    return 0;
}

static int print_environment(void) {
#if __STDC_HOSTED__
    char **current = environ;
    while (current != 0 && *current != 0) {
        rt_write_line(1, *current);
        current += 1;
    }
#endif
    return 0;
}

int main(int argc, char **argv) {
    int argi = 1;

#if __STDC_HOSTED__
    while (argi < argc && is_assignment(argv[argi])) {
        char *eq = argv[argi];
        while (*eq != '\0' && *eq != '=') {
            eq += 1;
        }
        if (*eq == '=') {
            *eq = '\0';
            setenv(argv[argi], eq + 1, 1);
            *eq = '=';
        }
        argi += 1;
    }
#else
    while (argi < argc && is_assignment(argv[argi])) {
        argi += 1;
    }
#endif

    if (argi >= argc) {
        return print_environment();
    }

    {
        int pid;
        int status = 1;
        if (platform_spawn_process(&argv[argi], -1, -1, 0, 0, 0, &pid) != 0) {
            tool_write_error("env", "failed to execute ", argv[argi]);
            return 127;
        }
        if (platform_wait_process(pid, &status) != 0) {
            tool_write_error("env", "wait failed", 0);
            return 1;
        }
        return status;
    }
}
