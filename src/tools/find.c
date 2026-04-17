#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define FIND_MAX_ENTRIES 1024
#define FIND_PATH_CAPACITY 1024
#define FIND_MAX_START_PATHS 64
#define FIND_MAX_EXEC_ARGS 64

#define FIND_ACTION_PRINT 1
#define FIND_ACTION_PRINT0 2
#define FIND_ACTION_EXEC 4

typedef struct {
    const char *name_pattern;
    const char *path_pattern;
    char type_filter;
    int has_mtime;
    int mtime_relation;
    unsigned long long mtime_days;
    int has_size;
    int size_relation;
    unsigned long long size_bytes;
    int has_mindepth;
    int mindepth;
    int has_maxdepth;
    int maxdepth;
    int prune_matched_directories;
    int actions;
    int exec_argc;
    const char *exec_argv[FIND_MAX_EXEC_ARGS];
} FindOptions;

static int parse_relation_prefix(const char **text, int *relation_out) {
    *relation_out = 0;

    if (**text == '+') {
        *relation_out = 1;
        *text += 1;
    } else if (**text == '-') {
        *relation_out = -1;
        *text += 1;
    }

    return 0;
}

static int parse_size_filter(const char *text, int *relation_out, unsigned long long *size_out) {
    char digits[32];
    size_t length = 0;
    unsigned long long value = 0;
    unsigned long long scale = 1;

    parse_relation_prefix(&text, relation_out);

    while (text[length] != '\0' && text[length] >= '0' && text[length] <= '9') {
        if (length + 1 >= sizeof(digits)) {
            return -1;
        }
        digits[length] = text[length];
        length += 1;
    }

    if (length == 0) {
        return -1;
    }

    digits[length] = '\0';
    if (rt_parse_uint(digits, &value) != 0) {
        return -1;
    }

    if (text[length] == 'k' || text[length] == 'K') {
        scale = 1024ULL;
        length += 1;
    } else if (text[length] == 'm' || text[length] == 'M') {
        scale = 1024ULL * 1024ULL;
        length += 1;
    } else if (text[length] == 'c') {
        scale = 1ULL;
        length += 1;
    }

    if (text[length] != '\0') {
        return -1;
    }

    *size_out = value * scale;
    return 0;
}

static int parse_mtime_filter(const char *text, int *relation_out, unsigned long long *days_out) {
    parse_relation_prefix(&text, relation_out);
    return rt_parse_uint(text, days_out);
}

static int matches_numeric_filter(unsigned long long actual, int relation, unsigned long long expected) {
    if (relation < 0) {
        return actual < expected;
    }
    if (relation > 0) {
        return actual > expected;
    }
    return actual == expected;
}

static int matches_type_filter(const PlatformDirEntry *entry, char type_filter) {
    if (type_filter == '\0') {
        return 1;
    }
    if (type_filter == 'f') {
        return !entry->is_dir;
    }
    if (type_filter == 'd') {
        return entry->is_dir;
    }
    if (type_filter == 'l') {
        return (entry->mode & 0170000U) == 0120000U;
    }
    return 0;
}

static int matches_filters(const char *path, const PlatformDirEntry *entry, const FindOptions *options, long long now) {
    const char *base_name = tool_base_name(path);

    if (options->name_pattern != 0 && !tool_wildcard_match(options->name_pattern, base_name)) {
        return 0;
    }
    if (options->path_pattern != 0 && !tool_wildcard_match(options->path_pattern, path)) {
        return 0;
    }

    if (!matches_type_filter(entry, options->type_filter)) {
        return 0;
    }

    if (options->has_mtime) {
        unsigned long long age_days = 0;

        if (now > entry->mtime) {
            age_days = (unsigned long long)((now - entry->mtime) / 86400);
        }

        if (!matches_numeric_filter(age_days, options->mtime_relation, options->mtime_days)) {
            return 0;
        }
    }

    if (options->has_size && !matches_numeric_filter(entry->size, options->size_relation, options->size_bytes)) {
        return 0;
    }

    return 1;
}

static int emit_path(const char *path, int nul_terminated) {
    if (rt_write_cstr(1, path) != 0) {
        return -1;
    }
    return rt_write_char(1, nul_terminated ? '\0' : '\n');
}

static int run_exec_action(const char *path, const FindOptions *options) {
    char *spawn_argv[FIND_MAX_EXEC_ARGS + 1];
    int pid;
    int status;
    int i;

    if (options->exec_argc <= 0) {
        return 0;
    }

    for (i = 0; i < options->exec_argc; ++i) {
        spawn_argv[i] = (char *)(rt_strcmp(options->exec_argv[i], "{}") == 0 ? path : options->exec_argv[i]);
    }
    spawn_argv[options->exec_argc] = 0;

    if (platform_spawn_process(spawn_argv, -1, -1, 0, 0, 0, &pid) != 0) {
        tool_write_error("find", "failed to execute ", spawn_argv[0]);
        return -1;
    }
    if (platform_wait_process(pid, &status) != 0) {
        tool_write_error("find", "wait failed for ", spawn_argv[0]);
        return -1;
    }
    if (status != 0) {
        tool_write_error("find", "command failed: ", spawn_argv[0]);
        return -1;
    }
    return 0;
}

static int perform_actions(const char *path, const FindOptions *options) {
    int actions = options->actions;

    if (actions == 0) {
        actions = FIND_ACTION_PRINT;
    }

    if ((actions & FIND_ACTION_PRINT) != 0 && emit_path(path, 0) != 0) {
        return -1;
    }
    if ((actions & FIND_ACTION_PRINT0) != 0 && emit_path(path, 1) != 0) {
        return -1;
    }
    if ((actions & FIND_ACTION_EXEC) != 0 && run_exec_action(path, options) != 0) {
        return -1;
    }

    return 0;
}

static int find_walk(const char *path, const FindOptions *options, long long now, int depth) {
    PlatformDirEntry current;
    PlatformDirEntry entries[FIND_MAX_ENTRIES];
    size_t count = 0;
    int is_directory = 0;
    int matched;
    size_t i;

    if (platform_get_path_info(path, &current) != 0) {
        rt_write_cstr(2, "find: cannot access ");
        rt_write_line(2, path);
        return -1;
    }

    matched = matches_filters(path, &current, options, now);
    if ((!options->has_mindepth || depth >= options->mindepth) &&
        (!options->has_maxdepth || depth <= options->maxdepth) &&
        matched) {
        if (perform_actions(path, options) != 0) {
            return -1;
        }
    }

    if (!current.is_dir) {
        return 0;
    }
    if ((options->has_maxdepth && depth >= options->maxdepth) ||
        (options->prune_matched_directories && matched)) {
        return 0;
    }
    if (platform_collect_entries(path, 1, entries, FIND_MAX_ENTRIES, &count, &is_directory) != 0) {
        rt_write_cstr(2, "find: cannot access ");
        rt_write_line(2, path);
        return -1;
    }

    for (i = 0; i < count; ++i) {
        char child_path[FIND_PATH_CAPACITY];

        if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
            continue;
        }

        if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0) {
            rt_write_line(2, "find: path too long");
            platform_free_entries(entries, count);
            return -1;
        }

        if (find_walk(child_path, options, now, depth + 1) != 0) {
            platform_free_entries(entries, count);
            return -1;
        }
    }

    platform_free_entries(entries, count);
    return 0;
}

int main(int argc, char **argv) {
    const char *start_paths[FIND_MAX_START_PATHS];
    int start_count = 0;
    FindOptions options;
    long long now;
    int i;

    rt_memset(&options, 0, sizeof(options));

    for (i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' && start_count == 0) {
            start_paths[start_count++] = argv[i];
            continue;
        }
        if (argv[i][0] != '-' && start_count > 0 && options.exec_argc == 0 && options.actions == 0 &&
            options.name_pattern == 0 && options.path_pattern == 0 && options.type_filter == '\0' &&
            !options.has_mtime && !options.has_size && !options.has_mindepth && !options.has_maxdepth &&
            !options.prune_matched_directories) {
            if (start_count >= FIND_MAX_START_PATHS) {
                rt_write_line(2, "find: too many start paths");
                return 1;
            }
            start_paths[start_count++] = argv[i];
            continue;
        }

        if (rt_strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            options.name_pattern = argv[i + 1];
            i += 1;
        } else if (rt_strcmp(argv[i], "-path") == 0 && i + 1 < argc) {
            options.path_pattern = argv[i + 1];
            i += 1;
        } else if (rt_strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            options.type_filter = argv[i + 1][0];
            if (options.type_filter != 'f' && options.type_filter != 'd' && options.type_filter != 'l') {
                rt_write_line(2, "find: unsupported -type value");
                return 1;
            }
            i += 1;
        } else if (rt_strcmp(argv[i], "-mtime") == 0 && i + 1 < argc) {
            if (parse_mtime_filter(argv[i + 1], &options.mtime_relation, &options.mtime_days) != 0) {
                rt_write_line(2, "find: invalid -mtime value");
                return 1;
            }
            options.has_mtime = 1;
            i += 1;
        } else if (rt_strcmp(argv[i], "-size") == 0 && i + 1 < argc) {
            if (parse_size_filter(argv[i + 1], &options.size_relation, &options.size_bytes) != 0) {
                rt_write_line(2, "find: invalid -size value");
                return 1;
            }
            options.has_size = 1;
            i += 1;
        } else if (rt_strcmp(argv[i], "-mindepth") == 0 && i + 1 < argc) {
            long long depth = 0;
            if (tool_parse_int_arg(argv[i + 1], &depth, "find", "mindepth") != 0 || depth < 0) {
                return 1;
            }
            options.has_mindepth = 1;
            options.mindepth = (int)depth;
            i += 1;
        } else if (rt_strcmp(argv[i], "-maxdepth") == 0 && i + 1 < argc) {
            long long depth = 0;
            if (tool_parse_int_arg(argv[i + 1], &depth, "find", "maxdepth") != 0 || depth < 0) {
                return 1;
            }
            options.has_maxdepth = 1;
            options.maxdepth = (int)depth;
            i += 1;
        } else if (rt_strcmp(argv[i], "-prune") == 0) {
            options.prune_matched_directories = 1;
        } else if (rt_strcmp(argv[i], "-print") == 0) {
            options.actions |= FIND_ACTION_PRINT;
        } else if (rt_strcmp(argv[i], "-print0") == 0) {
            options.actions |= FIND_ACTION_PRINT0;
        } else if (rt_strcmp(argv[i], "-exec") == 0) {
            int exec_count = 0;

            while (i + 1 < argc && exec_count < FIND_MAX_EXEC_ARGS) {
                i += 1;
                if (rt_strcmp(argv[i], ";") == 0 || rt_strcmp(argv[i], "+") == 0) {
                    break;
                }
                options.exec_argv[exec_count++] = argv[i];
            }
            if (exec_count == 0 || i >= argc || (rt_strcmp(argv[i], ";") != 0 && rt_strcmp(argv[i], "+") != 0)) {
                rt_write_line(2, "find: -exec requires a command terminated by ';' or '+'");
                return 1;
            }
            options.exec_argc = exec_count;
            options.actions |= FIND_ACTION_EXEC;
        } else {
            rt_write_line(
                2,
                "Usage: find [path ...] [-name pattern] [-path pattern] [-type f|d|l] "
                "[-mtime n] [-size n[c|k|M]] [-mindepth n] [-maxdepth n] [-prune] "
                "[-print|-print0] [-exec cmd {} ;]"
            );
            return 1;
        }
    }

    if (start_count == 0) {
        start_paths[start_count++] = ".";
    }

    now = platform_get_epoch_time();
    for (i = 0; i < start_count; ++i) {
        if (find_walk(start_paths[i], &options, now, 0) != 0) {
            return 1;
        }
    }
    return 0;
}
