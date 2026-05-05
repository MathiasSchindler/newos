#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PKILL_MAX_PROCESSES 4096

typedef struct {
    int exact;
    int ignore_case;
    int has_uid;
    unsigned int uid;
    int has_parent;
    int parent_pid;
    int signal_number;
    const char *pattern;
} PkillOptions;

static void print_usage(void) {
    tool_write_usage("pkill", "[-SIGNAL] [-s SIGNAL|--signal SIGNAL] [-ix] [-u USER] [-P PPID] PATTERN");
}

static char ascii_fold(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int strings_equal_ignore_case(const char *left, const char *right) {
    size_t i = 0U;

    while (left[i] != '\0' && right[i] != '\0') {
        if (ascii_fold(left[i]) != ascii_fold(right[i])) {
            return 0;
        }
        i += 1U;
    }
    return left[i] == '\0' && right[i] == '\0';
}

static int resolve_user(const char *text, unsigned int *uid_out) {
    unsigned long long value = 0ULL;
    PlatformIdentity identity;

    if (rt_parse_uint(text, &value) == 0) {
        *uid_out = (unsigned int)value;
        return 0;
    }
    if (platform_lookup_identity(text, &identity) != 0) {
        return -1;
    }
    *uid_out = identity.uid;
    return 0;
}

static int parse_pid(const char *text, int *pid_out) {
    unsigned long long value = 0ULL;

    if (rt_parse_uint(text, &value) != 0 || value > 2147483647ULL) {
        return -1;
    }
    *pid_out = (int)value;
    return 0;
}

static int process_name_matches(const PlatformProcessEntry *entry, const PkillOptions *options) {
    const char *name = entry->name;
    const char *base = tool_base_name(entry->name);
    size_t start = 0U;
    size_t end = 0U;

    if (options->has_uid && entry->uid != options->uid) {
        return 0;
    }
    if (options->has_parent && entry->ppid != options->parent_pid) {
        return 0;
    }

    if (options->exact) {
        if (options->ignore_case) {
            return strings_equal_ignore_case(options->pattern, name) || strings_equal_ignore_case(options->pattern, base);
        }
        return rt_strcmp(options->pattern, name) == 0 || rt_strcmp(options->pattern, base) == 0;
    }

    return tool_regex_search(options->pattern, name, options->ignore_case, 0U, &start, &end) == 0 ||
           tool_regex_search(options->pattern, base, options->ignore_case, 0U, &start, &end) == 0;
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
            options.ignore_case = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-x") == 0) {
            options.exact = 1;
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
            if (require_value(argi, argc, arg) != 0 || resolve_user(argv[argi + 1], &options.uid) != 0) {
                print_usage();
                return 2;
            }
            options.has_uid = 1;
            argi += 2;
            continue;
        }
        if (tool_starts_with(arg, "-u") && arg[2] != '\0') {
            if (resolve_user(arg + 2, &options.uid) != 0) {
                tool_write_error("pkill", "invalid user: ", arg + 2);
                return 2;
            }
            options.has_uid = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-P") == 0) {
            if (require_value(argi, argc, arg) != 0 || parse_pid(argv[argi + 1], &options.parent_pid) != 0) {
                print_usage();
                return 2;
            }
            options.has_parent = 1;
            argi += 2;
            continue;
        }
        if (tool_starts_with(arg, "-P") && arg[2] != '\0') {
            if (parse_pid(arg + 2, &options.parent_pid) != 0) {
                tool_write_error("pkill", "invalid parent pid: ", arg + 2);
                return 2;
            }
            options.has_parent = 1;
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
    options.pattern = argv[argi];

    if (platform_list_processes(entries, PKILL_MAX_PROCESSES, &count) != 0) {
        tool_write_error("pkill", "cannot list processes", 0);
        return 2;
    }

    for (i = 0U; i < count; ++i) {
        if (entries[i].pid == self_pid) {
            continue;
        }
        if (process_name_matches(&entries[i], &options)) {
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
