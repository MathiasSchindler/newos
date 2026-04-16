#include "platform.h"
#include "runtime.h"

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-s] target linkname");
}

int main(int argc, char **argv) {
    int symbolic = 0;
    int argi = 1;

    if (argc > 1 && rt_strcmp(argv[1], "-s") == 0) {
        symbolic = 1;
        argi = 2;
    }

    if (argc != argi + 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (symbolic) {
        if (platform_create_symbolic_link(argv[argi], argv[argi + 1]) != 0) {
            rt_write_line(2, "ln: failed to create symbolic link");
            return 1;
        }
    } else {
        if (platform_create_hard_link(argv[argi], argv[argi + 1]) != 0) {
            rt_write_line(2, "ln: failed to create hard link");
            return 1;
        }
    }

    return 0;
}
