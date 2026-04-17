#include "platform.h"
#include "runtime.h"

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-s] [-f] target linkname");
}

int main(int argc, char **argv) {
    int symbolic = 0;
    int force = 0;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 's') {
                symbolic = 1;
            } else if (*flag == 'f') {
                force = 1;
            } else {
                print_usage(argv[0]);
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (argc != argi + 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (force) {
        int is_directory = 0;
        if (platform_path_is_directory(argv[argi + 1], &is_directory) == 0) {
            if (is_directory) {
                rt_write_line(2, "ln: refusing to replace directory");
                return 1;
            }
            (void)platform_remove_file(argv[argi + 1]);
        }
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
