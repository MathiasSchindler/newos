#include "runtime.h"
#include "tool_util.h"

#define REALPATH_PATH_CAPACITY 2048

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-e|-m] path ...");
}

int main(int argc, char **argv) {
    char resolved[REALPATH_PATH_CAPACITY];
    int allow_missing = 0;
    int argi = 1;
    int exit_code = 0;
    int i;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-e") == 0) {
            allow_missing = 0;
        } else if (rt_strcmp(argv[argi], "-m") == 0) {
            allow_missing = 1;
        } else {
            print_usage(argv[0]);
            return 1;
        }
        argi += 1;
    }

    if (argi >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        if (tool_canonicalize_path(argv[i], 1, allow_missing, resolved, sizeof(resolved)) != 0) {
            rt_write_cstr(2, "realpath: cannot resolve ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }
        rt_write_line(1, resolved);
    }

    return exit_code;
}
