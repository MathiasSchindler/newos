#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-L|-P]");
}

int main(int argc, char **argv) {
    char buffer[4096];
    int physical = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        if (rt_strcmp(argv[i], "-L") == 0) {
            physical = 0;
        } else if (rt_strcmp(argv[i], "-P") == 0) {
            physical = 1;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (platform_get_current_directory(buffer, sizeof(buffer)) != 0) {
        rt_write_line(2, "pwd: unable to read current directory");
        return 1;
    }

    if (!physical) {
        const char *logical_pwd = platform_getenv("PWD");
        if (logical_pwd != 0 && logical_pwd[0] == '/') {
            char logical_resolved[4096];
            if (tool_canonicalize_path(logical_pwd, 1, 0, logical_resolved, sizeof(logical_resolved)) == 0 &&
                rt_strcmp(logical_resolved, buffer) == 0) {
                return rt_write_line(1, logical_pwd) == 0 ? 0 : 1;
            }
        }
    }

    return rt_write_line(1, buffer) == 0 ? 0 : 1;
}
