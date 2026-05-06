#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PGREP_MAX_PROCESSES 4096

typedef struct {
    ToolProcessMatchOptions match;
    int list_name;
    int count_only;
} PgrepOptions;

static void print_usage(void) {
    tool_write_usage("pgrep", "[-ilcx] [-u USER] [-P PPID] PATTERN");
}

static int option_needs_value(int argi, int argc, const char *tool_name, const char *flag) {
    if (argi + 1 < argc) {
        return 0;
    }
    tool_write_error(tool_name, "option requires an argument: ", flag);
    return -1;
}

int main(int argc, char **argv) {
    PgrepOptions options;
    PlatformProcessEntry entries[PGREP_MAX_PROCESSES];
    size_t count = 0U;
    size_t i;
    int argi = 1;
    int matched = 0;
    int self_pid = platform_get_process_id();

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *arg = argv[argi];

        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        }
        if (rt_strcmp(arg, "-i") == 0) {
            options.match.ignore_case = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-l") == 0) {
            options.list_name = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-c") == 0) {
            options.count_only = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-x") == 0) {
            options.match.exact = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-u") == 0) {
            if (option_needs_value(argi, argc, "pgrep", arg) != 0 || tool_resolve_user_id(argv[argi + 1], &options.match.uid) != 0) {
                print_usage();
                return 2;
            }
            options.match.has_uid = 1;
            argi += 2;
            continue;
        }
        if (tool_starts_with(arg, "-u") && arg[2] != '\0') {
            if (tool_resolve_user_id(arg + 2, &options.match.uid) != 0) {
                tool_write_error("pgrep", "invalid user: ", arg + 2);
                return 2;
            }
            options.match.has_uid = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-P") == 0) {
            if (option_needs_value(argi, argc, "pgrep", arg) != 0 || tool_parse_pid(argv[argi + 1], &options.match.parent_pid) != 0) {
                print_usage();
                return 2;
            }
            options.match.has_parent = 1;
            argi += 2;
            continue;
        }
        if (tool_starts_with(arg, "-P") && arg[2] != '\0') {
            if (tool_parse_pid(arg + 2, &options.match.parent_pid) != 0) {
                tool_write_error("pgrep", "invalid parent pid: ", arg + 2);
                return 2;
            }
            options.match.has_parent = 1;
            argi += 1;
            continue;
        }

        print_usage();
        return 2;
    }

    if (argc - argi != 1) {
        print_usage();
        return 2;
    }
    options.match.pattern = argv[argi];
    options.match.skip_pid = self_pid;

    if (platform_list_processes(entries, PGREP_MAX_PROCESSES, &count) != 0) {
        tool_write_error("pgrep", "cannot list processes", 0);
        return 2;
    }

    for (i = 0U; i < count; ++i) {
        if (tool_process_matches(&entries[i], &options.match)) {
            matched += 1;
            if (!options.count_only) {
                rt_write_uint(1, (unsigned long long)entries[i].pid);
                if (options.list_name) {
                    rt_write_char(1, ' ');
                    rt_write_cstr(1, entries[i].name);
                }
                rt_write_char(1, '\n');
            }
        }
    }

    if (options.count_only) {
        rt_write_uint(1, (unsigned long long)matched);
        rt_write_char(1, '\n');
    }

    return matched > 0 ? 0 : 1;
}
