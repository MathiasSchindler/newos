#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define UNAME_FIELD_CAPACITY 128

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-a] [-s] [-n] [-r] [-m] [-p] [-i] [-o]");
}

static int write_selected_field(const char *text, int *printed_any) {
    if (*printed_any) {
        if (rt_write_char(1, ' ') != 0) {
            return 1;
        }
    }
    if (rt_write_cstr(1, text) != 0) {
        return 1;
    }
    *printed_any = 1;
    return 0;
}

int main(int argc, char **argv) {
    char sysname[UNAME_FIELD_CAPACITY];
    char nodename[UNAME_FIELD_CAPACITY];
    char release[UNAME_FIELD_CAPACITY];
    char machine[UNAME_FIELD_CAPACITY];
    int show_sysname = 0;
    int show_nodename = 0;
    int show_release = 0;
    int show_machine = 0;
    int printed_any = 0;
    int argi;

    if (argc == 1) {
        show_sysname = 1;
    } else {
        for (argi = 1; argi < argc; ++argi) {
            const char *arg = argv[argi];
            size_t j = 0;

            if (arg[0] != '-' || arg[1] == '\0') {
                print_usage(argv[0]);
                return 1;
            }

            if (rt_strcmp(arg, "--help") == 0) {
                print_usage(argv[0]);
                return 0;
            }

            for (j = 1; arg[j] != '\0'; ++j) {
                switch (arg[j]) {
                    case 'a':
                        show_sysname = 1;
                        show_nodename = 1;
                        show_release = 1;
                        show_machine = 1;
                        break;
                    case 's':
                    case 'o':
                        show_sysname = 1;
                        break;
                    case 'n':
                        show_nodename = 1;
                        break;
                    case 'r':
                        show_release = 1;
                        break;
                    case 'm':
                    case 'p':
                    case 'i':
                        show_machine = 1;
                        break;
                    default:
                        print_usage(argv[0]);
                        return 1;
                }
            }
        }
    }

    if (platform_get_uname(sysname, sizeof(sysname), nodename, sizeof(nodename), release, sizeof(release), machine, sizeof(machine)) != 0) {
        rt_write_line(2, "uname: failed");
        return 1;
    }

    if (show_sysname && write_selected_field(sysname, &printed_any) != 0) {
        return 1;
    }
    if (show_nodename && write_selected_field(nodename, &printed_any) != 0) {
        return 1;
    }
    if (show_release && write_selected_field(release, &printed_any) != 0) {
        return 1;
    }
    if (show_machine && write_selected_field(machine, &printed_any) != 0) {
        return 1;
    }
    if (!printed_any || rt_write_char(1, '\n') != 0) {
        return 1;
    }

    return 0;
}
