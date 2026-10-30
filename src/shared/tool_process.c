#include "concurrency.h"
#include "runtime.h"
#include "tool_util.h"

static char tool_ascii_fold(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int tool_strings_equal_ignore_case(const char *left, const char *right) {
    size_t i = 0U;

    while (left[i] != '\0' && right[i] != '\0') {
        if (tool_ascii_fold(left[i]) != tool_ascii_fold(right[i])) {
            return 0;
        }
        i += 1U;
    }
    return left[i] == '\0' && right[i] == '\0';
}

int tool_is_shell_builtin_name(const char *name) {
    return rt_strcmp(name, "cd") == 0 ||
           rt_strcmp(name, "exit") == 0 ||
           rt_strcmp(name, "jobs") == 0 ||
           rt_strcmp(name, "history") == 0 ||
           rt_strcmp(name, "fg") == 0 ||
           rt_strcmp(name, "bg") == 0 ||
           rt_strcmp(name, "export") == 0 ||
           rt_strcmp(name, "unset") == 0 ||
           rt_strcmp(name, "command") == 0 ||
           rt_strcmp(name, "alias") == 0 ||
           rt_strcmp(name, "set") == 0 ||
           rt_strcmp(name, "shift") == 0;
}

int tool_resolve_user_id(const char *text, unsigned int *uid_out) {
    unsigned long long value = 0ULL;
    PlatformIdentity identity;

    if (text == 0 || uid_out == 0) {
        return -1;
    }
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

int tool_resolve_group_id(const char *text, unsigned int *gid_out) {
    unsigned long long value = 0ULL;

    if (text == 0 || text[0] == '\0' || gid_out == 0) {
        return -1;
    }
    if (rt_parse_uint(text, &value) == 0) {
        *gid_out = (unsigned int)value;
        return 0;
    }
    return platform_lookup_group(text, gid_out);
}

int tool_parse_pid(const char *text, int *pid_out) {
    unsigned long long value = 0ULL;

    if (text == 0 || pid_out == 0 || rt_parse_uint(text, &value) != 0 || value > 2147483647ULL) {
        return -1;
    }
    *pid_out = (int)value;
    return 0;
}

int tool_process_matches(const PlatformProcessEntry *entry, const ToolProcessMatchOptions *options) {
    const char *name;
    const char *base;
    size_t start = 0U;
    size_t end = 0U;

    if (entry == 0 || options == 0 || options->pattern == 0) {
        return 0;
    }
    if (options->skip_pid > 0 && entry->pid == options->skip_pid) {
        return 0;
    }
    if (options->has_uid && entry->uid != options->uid) {
        return 0;
    }
    if (options->has_parent && entry->ppid != options->parent_pid) {
        return 0;
    }

    name = entry->name;
    base = tool_base_name(entry->name);

    if (options->exact) {
        if (options->ignore_case) {
            return tool_strings_equal_ignore_case(options->pattern, name) ||
                   tool_strings_equal_ignore_case(options->pattern, base);
        }
        return rt_strcmp(options->pattern, name) == 0 || rt_strcmp(options->pattern, base) == 0;
    }

        return tool_regex_search(options->pattern, name, options->ignore_case, 0U, &start, &end) != 0 ||
            tool_regex_search(options->pattern, base, options->ignore_case, 0U, &start, &end) != 0;
}

int tool_validate_absolute_program_path(const char *tool_name, const char *path) {
    if (path == 0 || path[0] != '/') {
        tool_write_error(tool_name, "refusing non-absolute program path: ", path != 0 ? path : "(null)");
        return -1;
    }
    return 0;
}

unsigned int tool_worker_count_from_env(const char *env_name, unsigned int default_max_workers) {
    const char *value_text = platform_getenv(env_name);
    unsigned long long value;
    unsigned int platform_width;

    if (value_text == 0 || value_text[0] == '\0') {
        platform_width = platform_worker_thread_count();
        if (platform_width == 0U) return 0U;
        return platform_width > default_max_workers ? default_max_workers : platform_width;
    }
    if (rt_parse_uint(value_text, &value) != 0) {
        return default_max_workers;
    }
    if (value > RT_TASK_POOL_MAX_WORKERS) {
        return RT_TASK_POOL_MAX_WORKERS;
    }
    return (unsigned int)value;
}

int tool_parse_signal_name(const char *text, int *signal_out) {
    return platform_parse_signal_name(text, signal_out);
}

const char *tool_signal_name(int signal_number) {
    return platform_signal_name(signal_number);
}

void tool_write_signal_list(int fd) {
    platform_write_signal_list(fd);
}

int tool_parse_pid_filter_list(const char *spec, int *pids_out, size_t max_count, size_t *count_out, const char *tool_name, int require_nonempty) {
    size_t count = 0U;
    size_t i = 0U;

    while (spec[i] != '\0') {
        char token[32];
        size_t token_len = 0U;
        long long pid_value = 0;

        while (spec[i] == ',' || rt_is_space(spec[i])) i += 1U;
        while (spec[i] != '\0' && spec[i] != ',' && token_len + 1U < sizeof(token)) token[token_len++] = spec[i++];
        token[token_len] = '\0';
        tool_trim_whitespace(token);
        if (token[0] == '\0' || count >= max_count || tool_parse_int_arg(token, &pid_value, tool_name, "pid") != 0 || pid_value <= 0) return -1;
        pids_out[count++] = (int)pid_value;
        while (spec[i] != '\0' && spec[i] != ',') i += 1U;
    }
    if (require_nonempty && count == 0U) return -1;
    *count_out = count;
    return 0;
}
