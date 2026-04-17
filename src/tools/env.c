#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if __STDC_HOSTED__
#include <stdlib.h>
extern char **environ;
#endif

#define ENV_MAX_UNSETS 64

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

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-i] [-0] [-u NAME] [NAME=VALUE ...] [COMMAND [ARG ...]]");
}

#if __STDC_HOSTED__
static void reset_environment(void) {
    static char *empty_environment[] = { 0 };
    environ = empty_environment;
}
#endif

static int print_environment(int nul_delimited) {
#if __STDC_HOSTED__
    char **current = environ;
    while (current != 0 && *current != 0) {
        if (nul_delimited) {
            if (rt_write_cstr(1, *current) != 0 || rt_write_char(1, '\0') != 0) {
                return 1;
            }
        } else if (rt_write_line(1, *current) != 0) {
            return 1;
        }
        current += 1;
    }
#else
    (void)nul_delimited;
#endif
    return 0;
}

int main(int argc, char **argv) {
    int argi = 1;
    int nul_delimited = 0;
    int ignore_environment = 0;
    const char *unset_names[ENV_MAX_UNSETS];
    size_t unset_count = 0;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-0") == 0) {
            nul_delimited = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-i") == 0) {
            ignore_environment = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-u") == 0) {
            if (argi + 1 >= argc || unset_count >= ENV_MAX_UNSETS) {
                print_usage(argv[0]);
                return 1;
            }
            unset_names[unset_count++] = argv[argi + 1];
            argi += 2;
            continue;
        }
        break;
    }

#if __STDC_HOSTED__
    if (ignore_environment) {
        reset_environment();
    }
    {
        size_t i;
        for (i = 0; i < unset_count; ++i) {
            (void)unsetenv(unset_names[i]);
        }
    }
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
        return print_environment(nul_delimited
    int ignore_environment = 0;
    const char *unset_names[ENV_MAX_UNSETS];
    size_t unset_count = 0;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-0") == 0) {
            nul_delimited = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-i") == 0) {
            ignore_environment = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-u") == 0) {
            if (argi + 1 >= argc || unset_count >= ENV_MAX_UNSETS) {
                print_usage(argv[0]);
                return 1;
            }
            unset_names[unset_count++] = argv[argi + 1];
            argi += 2;
            continue;
        }
        break;
    }

#if __STDC_HOSTED__
    if (ignore_environment) {
        reset_environment();
    }
    {
        size_t i;
        for (i = 0; i < unset_count; ++i) {
            (void)unsetenv(unset_names[i]);
        }
    }
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
        return print_environment(nul_delimited);
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
