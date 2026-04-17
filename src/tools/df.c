#include "platform.h"
#include "runtime.h"

static int print_one(const char *path) {
    unsigned long long total = 0;
    unsigned long long free_space = 0;
    unsigned long long available = 0;
    unsigned long long used;

    if (platform_get_filesystem_usage(path, &total, &free_space, &available) != 0) {
        rt_write_cstr(2, "df: failed to inspect ");
        rt_write_line(2, path);
        return 1;
    }

    used = (total >= free_space) ? (total - free_space) : 0ULL;

    rt_write_cstr(1, path);
    rt_write_char(1, '\t');
    rt_write_uint(1, total);
    rt_write_char(1, '\t');
    rt_write_uint(1, used);
    rt_write_char(1, '\t');
    rt_write_uint(1, available);
    rt_write_char(1, '\t');
    rt_write_line(1, path);
    return 0;
}

int main(int argc, char **argv) {
    int i;
    int exit_code = 0;

    rt_write_line(1, "Filesystem\tTotal\tUsed\tAvailable\tMounted on");

    if (argc == 1) {
        return print_one("/");
    }

    for (i = 1; i < argc; ++i) {
        if (print_one(argv[i]) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
