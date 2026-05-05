#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#include <limits.h>

typedef struct {
    PlatformSessionEntry session;
} UserEntry;

typedef struct {
    int sort_output;
    int unique_output;
    int count_only;
    int long_output;
    const char *host_filter;
    const char *terminal_filter;
    int have_since;
    long long since_time;
    const char *user_filters[64];
    size_t user_filter_count;
} UsersOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-s] [-u] [-c] [-l] [--host HOST] [--terminal TTY] [--since EPOCH] [USER ...]");
}

static int compare_users(const void *lhs, const void *rhs) {
    const UserEntry *left = (const UserEntry *)lhs;
    const UserEntry *right = (const UserEntry *)rhs;
    int cmp = rt_strcmp(left->session.username, right->session.username);

    if (cmp != 0) {
        return cmp;
    }
    cmp = rt_strcmp(left->session.terminal, right->session.terminal);
    if (cmp != 0) {
        return cmp;
    }
    return rt_strcmp(left->session.host, right->session.host);
}

static void sort_users(UserEntry *entries, size_t count) {
    size_t i;
    size_t j;

    for (i = 0; i < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            if (compare_users(&entries[i], &entries[j]) > 0) {
                UserEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
}

static int parse_signed_value(const char *text, long long *value_out) {
    unsigned long long magnitude = 0ULL;
    int negative = 0;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }
    if (*text == '-') {
        negative = 1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }
    if (*text == '@') {
        text += 1;
    }
    if (*text == '\0' || rt_parse_uint(text, &magnitude) != 0) {
        return -1;
    }
    if (!negative) {
        if (magnitude > (unsigned long long)LLONG_MAX) {
            return -1;
        }
        *value_out = (long long)magnitude;
    } else if (magnitude == (unsigned long long)LLONG_MAX + 1ULL) {
        *value_out = LLONG_MIN;
    } else {
        if (magnitude > (unsigned long long)LLONG_MAX) {
            return -1;
        }
        *value_out = -(long long)magnitude;
    }
    return 0;
}

static int user_filter_matches(const UsersOptions *options, const char *username) {
    size_t i;

    if (options->user_filter_count == 0U) {
        return 1;
    }
    for (i = 0; i < options->user_filter_count; ++i) {
        if (rt_strcmp(options->user_filters[i], username) == 0) {
            return 1;
        }
    }
    return 0;
}

static int session_matches(const UsersOptions *options, const PlatformSessionEntry *session) {
    const char *username = session->username[0] != '\0' ? session->username : "unknown";

    if (!user_filter_matches(options, username)) {
        return 0;
    }
    if (options->host_filter != 0 && rt_strcmp(options->host_filter, session->host) != 0) {
        return 0;
    }
    if (options->terminal_filter != 0 && rt_strcmp(options->terminal_filter, session->terminal) != 0) {
        return 0;
    }
    if (options->have_since && session->login_time < options->since_time) {
        return 0;
    }
    return 1;
}

static int same_user(const UserEntry *left, const UserEntry *right) {
    return rt_strcmp(left->session.username, right->session.username) == 0;
}

static int parse_options(int argc, char **argv, UsersOptions *options) {
    int i;

    rt_memset(options, 0, sizeof(*options));
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        size_t j;

        if (rt_strcmp(arg, "--help") == 0) {
            return 1;
        }
        if (rt_strcmp(arg, "--host") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            options->host_filter = argv[++i];
            continue;
        }
        if (tool_starts_with(arg, "--host=")) {
            options->host_filter = arg + 7;
            continue;
        }
        if (rt_strcmp(arg, "--terminal") == 0 || rt_strcmp(arg, "--tty") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            options->terminal_filter = argv[++i];
            continue;
        }
        if (tool_starts_with(arg, "--terminal=")) {
            options->terminal_filter = arg + 11;
            continue;
        }
        if (tool_starts_with(arg, "--tty=")) {
            options->terminal_filter = arg + 6;
            continue;
        }
        if (rt_strcmp(arg, "--since") == 0) {
            if (i + 1 >= argc || parse_signed_value(argv[i + 1], &options->since_time) != 0) {
                return -1;
            }
            options->have_since = 1;
            i += 1;
            continue;
        }
        if (tool_starts_with(arg, "--since=")) {
            if (parse_signed_value(arg + 8, &options->since_time) != 0) {
                return -1;
            }
            options->have_since = 1;
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            for (j = 1U; arg[j] != '\0'; ++j) {
                if (arg[j] == 's') {
                    options->sort_output = 1;
                } else if (arg[j] == 'u') {
                    options->unique_output = 1;
                } else if (arg[j] == 'c') {
                    options->count_only = 1;
                } else if (arg[j] == 'l') {
                    options->long_output = 1;
                } else {
                    return -1;
                }
            }
            continue;
        }
        if (options->user_filter_count >= sizeof(options->user_filters) / sizeof(options->user_filters[0])) {
            return -1;
        }
        options->user_filters[options->user_filter_count++] = arg;
    }
    if (options->unique_output) {
        options->sort_output = 1;
    }
    return 0;
}

static int write_long_session(const PlatformSessionEntry *session) {
    char time_text[64];

    if (rt_write_cstr(1, session->username[0] != '\0' ? session->username : "unknown") != 0 ||
        rt_write_char(1, ' ') != 0 ||
        rt_write_cstr(1, session->terminal[0] != '\0' ? session->terminal : "-") != 0 ||
        rt_write_char(1, ' ') != 0) {
        return -1;
    }
    if (session->login_time > 0 && platform_format_time(session->login_time, 1, "%Y-%m-%d %H:%M", time_text, sizeof(time_text)) == 0) {
        if (rt_write_cstr(1, time_text) != 0) {
            return -1;
        }
    } else if (rt_write_cstr(1, "-") != 0) {
        return -1;
    }
    if (rt_write_char(1, ' ') != 0 || rt_write_cstr(1, session->host[0] != '\0' ? session->host : "-") != 0) {
        return -1;
    }
    return rt_write_char(1, '\n');
}

int main(int argc, char **argv) {
    PlatformSessionEntry sessions[128];
    UserEntry entries[256];
    UsersOptions options;
    size_t count = 0;
    size_t session_count = 0;
    size_t display_count;
    size_t output_count = 0;
    size_t i;
    int first = 1;
    int parse_status;

    parse_status = parse_options(argc, argv, &options);
    if (parse_status > 0) {
        print_usage(argv[0]);
        return 0;
    }
    if (parse_status < 0) {
        print_usage(argv[0]);
        return 1;
    }
    if (platform_list_sessions(sessions, sizeof(sessions) / sizeof(sessions[0]), &session_count) != 0) {
        tool_write_error("users", "user information unavailable", 0);
        return 1;
    }
    display_count = session_count < sizeof(sessions) / sizeof(sessions[0]) ? session_count : sizeof(sessions) / sizeof(sessions[0]);
    for (i = 0; i < display_count && count < sizeof(entries) / sizeof(entries[0]); ++i) {
        if (session_matches(&options, &sessions[i])) {
            entries[count].session = sessions[i];
            if (entries[count].session.username[0] == '\0') {
                rt_copy_string(entries[count].session.username, sizeof(entries[count].session.username), "unknown");
            }
            count += 1U;
        }
    }
    if (options.sort_output && count > 1U) {
        sort_users(entries, count);
    }
    for (i = 0; i < count; ++i) {
        if (options.unique_output && i > 0 && same_user(&entries[i - 1U], &entries[i])) {
            continue;
        }
        output_count += 1U;
        if (options.count_only) {
            continue;
        }
        if (options.long_output) {
            if (write_long_session(&entries[i].session) != 0) {
                return 1;
            }
            continue;
        }
        if (!first && rt_write_char(1, ' ') != 0) {
            return 1;
        }
        if (rt_write_cstr(1, entries[i].session.username) != 0) {
            return 1;
        }
        first = 0;
    }
    if (options.count_only) {
        if (rt_write_uint(1, (unsigned long long)output_count) != 0 || rt_write_char(1, '\n') != 0) {
            return 1;
        }
    } else if (!options.long_output && rt_write_char(1, '\n') != 0) {
        return 1;
    }
    return 0;
}