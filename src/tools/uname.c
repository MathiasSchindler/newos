#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define UNAME_FIELD_CAPACITY 128

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-asnrvmipo] [--all] [--kernel-name] [--nodename] [--kernel-release] [--kernel-version] [--machine] [--processor] [--hardware-platform] [--operating-system]");
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

static void enable_all_fields(
    int *show_sysname,
    int *show_nodename,
    int *show_release,
    int *show_version,
    int *show_machine,
    int *show_processor,
    int *show_platform,
    int *show_os
) {
    *show_sysname = 1;
    *show_nodename = 1;
    *show_release = 1;
    *show_version = 1;
    *show_machine = 1;
    *show_processor = 1;
    *show_platform = 1;
    *show_os = 1;
}

int main(int argc, char **argv) {
    char sysname[UNAME_FIELD_CAPACITY];
    char nodename[UNAME_FIELD_CAPACITY];
    char release[UNAME_FIELD_CAPACITY];
    char version[UNAME_FIELD_CAPACITY];
    char machine[UNAME_FIELD_CAPACITY];
    int show_sysname = 0;
    int show_nodename = 0;
    int show_release = 0;
    int show_version = 0;
    int show_machine = 0;
    int show_processor = 0;
    int show_platform = 0;
    int show_os = 0;
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
            if (rt_strcmp(arg, "--all") == 0) {
                enable_all_fields(
                    &show_sysname,
                    &show_nodename,
                    &show_release,
                    &show_version,
                    &show_machine,
                    &show_processor,
                    &show_platform,
                    &show_os
                );
                continue;
            }
            if (rt_strcmp(arg, "--kernel-name") == 0) {
                show_sysname = 1;
                continue;
            }
            if (rt_strcmp(arg, "--nodename") == 0) {
                show_nodename = 1;
                continue;
            }
            if (rt_strcmp(arg, "--kernel-release") == 0) {
                show_release = 1;
                continue;
            }
            if (rt_strcmp(arg, "--kernel-version") == 0) {
                show_version = 1;
                continue;
            }
            if (rt_strcmp(arg, "--machine") == 0) {
                show_machine = 1;
                continue;
            }
            if (rt_strcmp(arg, "--processor") == 0) {
                show_processor = 1;
                continue;
            }
            if (rt_strcmp(arg, "--hardware-platform") == 0) {
                show_platform = 1;
                continue;
            }
            if (rt_strcmp(arg, "--operating-system") == 0) {
                show_os = 1;
                continue;
            }
            if (arg[1] == '-') {
                print_usage(argv[0]);
                return 1;
            }

            for (j = 1; arg[j] != '\0'; ++j) {
                switch (arg[j]) {
                    case 'a':
                        enable_all_fields(
                            &show_sysname,
                            &show_nodename,
                            &show_release,
                            &show_version,
                            &show_machine,
                            &show_processor,
                            &show_platform,
                            &show_os
                        );
                        break;
                    case 's':
                        show_sysname = 1;
                        break;
                    case 'n':
                        show_nodename = 1;
                        break;
                    case 'r':
                        show_release = 1;
                        break;
                    case 'v':
                        show_version = 1;
                        break;
                    case 'm':
                        show_machine = 1;
                        break;
                    case 'p':
                        show_processor = 1;
                        break;
                    case 'i':
                        show_platform = 1;
                        break;
                    case 'o':
                        show_os = 1;
                        break;
                    default:
                        print_usage(argv[0]);
                        return 1;
                }
            }
        }
    }

    if (platform_get_uname(
            sysname,
            sizeof(sysname),
            nodename,
            sizeof(nodename),
            release,
            sizeof(release),
            version,
            sizeof(version),
            machine,
            sizeof(machine)
        ) != 0) {
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
    if (show_version && write_selected_field(version, &printed_any) != 0) {
        return 1;
    }
    if (show_machine && write_selected_field(machine, &printed_any) != 0) {
        return 1;
    }
    if (show_processor && write_selected_field(machine, &printed_any) != 0) {
        return 1;
    }
    if (show_platform && write_selected_field(machine, &printed_any) != 0) {
        return 1;
    }
    if (show_os && write_selected_field(sysname, &printed_any) != 0) {
        return 1;
    }
    if (!printed_any || rt_write_char(1, '\n') != 0) {
        return 1;
    }

    return 0;
}
