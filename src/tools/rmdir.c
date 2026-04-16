#include "platform.h"
#include "runtime.h"

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " directory ...");
}

int main(int argc, char **argv) {
    int exit_code = 0;
    int i;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    for (i = 1; i < argc; ++i) {
        if (platform_remove_directory(argv[i]) != 0) {
            rt_write_cstr(2, "rmdir: cannot remove ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
