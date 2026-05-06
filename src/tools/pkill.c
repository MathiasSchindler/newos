#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PKILL_MAX_PROCESSES 4096

typedef struct {
    ToolProcessMatchOptions match;
    int signal_number;
} PkillOptions;

static void print_usage(void) {
    tool_write_usage("pkill", "[-SIGNAL] [-s SIGNAL|--signal SIGNAL] [-ix] [-u USER] [-P PPID] PATTERN");
}

static int require_value(int argi, int argc, const char *flag) {
    if (argi + 1 < argc) {
        return 0;
    }
    tool_write_error("pkill", "option requires an argument: ", flag);
    return -1;
}

int main(int argc, char **argv) {
    PkillOptions options;
    PlatformProcessEntry entries[PKILL_MAX_PROCESSES];
    size_t count = 0U;
    size_t i;
    int argi = 1;
    int matched = 0;
    int failed = 0;
    int self_pid = platform_get_process_id();

    rt_memset(&options, 0, sizeof(options));
    options.signal_number = 15;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *arg = argv[argi];
        int parsed_signal = 0;

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
        if (rt_strcmp(arg, "-x") == 0) {
            options.match.exact = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-s") == 0 || rt_strcmp(arg, "--signal") == 0) {
            if (require_value(argi, argc, arg) != 0 || tool_parse_signal_name(argv[argi + 1], &options.signal_number) != 0) {
                tool_write_error("pkill", "invalid signal: ", argi + 1 < argc ? argv[argi + 1] : arg);
                return 2;
            }
            argi += 2;
            continue;
        }
        if (tool_starts_with(arg, "--signal=")) {
            if (tool_parse_signal_name(arg + 9, &options.signal_number) != 0) {
                tool_write_error("pkill", "invalid signal: ", arg + 9);
                return 2;
            }
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-u") == 0) {
            if (require_value(argi, argc, arg) != 0 || tool_resolve_user_id(argv[argi + 1], &options.match.uid) != 0) {
                print_usage();
                return 2;
            }
            options.match.has_uid = 1;
            argi += 2;
            continue;
        }
        if (tool_starts_with(arg, "-u") && arg[2] != '\0') {
            if (tool_resolve_user_id(arg + 2, &options.match.uid) != 0) {
                tool_write_error("pkill", "invalid user: ", arg + 2);
                return 2;
            }
            options.match.has_uid = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-P") == 0) {
            if (require_value(argi, argc, arg) != 0 || tool_parse_pid(argv[argi + 1], &options.match.parent_pid) != 0) {
                print_usage();
                return 2;
            }
            options.match.has_parent = 1;
            argi += 2;
            continue;
        }
        if (tool_starts_with(arg, "-P") && arg[2] != '\0') {
            if (tool_parse_pid(arg + 2, &options.match.parent_pid) != 0) {
                tool_write_error("pkill", "invalid parent pid: ", arg + 2);
                return 2;
            }
            options.match.has_parent = 1;
            argi += 1;
            continue;
        }
        if (tool_parse_signal_name(arg + 1, &parsed_signal) == 0) {
            options.signal_number = parsed_signal;
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

    if (platform_list_processes(entries, PKILL_MAX_PROCESSES, &count) != 0) {
        tool_write_error("pkill", "cannot list processes", 0);
        return 2;
    }

    for (i = 0U; i < count; ++i) {
        if (tool_process_matches(&entries[i], &options.match)) {
            matched += 1;
            if (platform_send_signal(entries[i].pid, options.signal_number) != 0) {
                tool_write_error("pkill", "cannot signal ", entries[i].name);
                failed = 1;
            }
        }
    }

    if (matched == 0) {
        return 1;
    }
    return failed ? 1 : 0;
}
