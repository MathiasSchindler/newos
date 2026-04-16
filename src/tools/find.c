#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define FIND_MAX_ENTRIES 1024
#define FIND_PATH_CAPACITY 1024

typedef struct {
    const char *name_pattern;
    char type_filter;
    int has_mtime;
    int mtime_relation;
    unsigned long long mtime_days;
    int has_size;
    int size_relation;
    unsigned long long size_bytes;
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

static int matches_filters(const char *path, const PlatformDirEntry *entry, const FindOptions *options, long long now) {
    const char *base_name = tool_base_name(path);

    if (options->name_pattern != 0 && !tool_wildcard_match(options->name_pattern, base_name)) {
        return 0;
    }

    if (options->type_filter == 'f' && entry->is_dir) {
        return 0;
    }
    if (options->type_filter == 'd' && !entry->is_dir) {
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

static int find_walk(const char *path, const FindOptions *options, long long now) {
    PlatformDirEntry current;
    PlatformDirEntry entries[FIND_MAX_ENTRIES];
    size_t count = 0;
    int is_directory = 0;
    size_t i;

    if (platform_get_path_info(path, &current) != 0) {
        rt_write_cstr(2, "find: cannot access ");
        rt_write_line(2, path);
        return -1;
    }

    if (matches_filters(path, &current, options, now)) {
        if (rt_write_line(1, path) != 0) {
            return -1;
        }
    }

    if (platform_collect_entries(path, 1, entries, FIND_MAX_ENTRIES, &count, &is_directory) != 0) {
        rt_write_cstr(2, "find: cannot access ");
        rt_write_line(2, path);
        return -1;
    }

    if (!is_directory) {
        return 0;
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

        if (find_walk(child_path, options, now) != 0) {
            platform_free_entries(entries, count);
            return -1;
        }
    }

    platform_free_entries(entries, count);
    return 0;
}

int main(int argc, char **argv) {
    const char *start_path = ".";
    FindOptions options;
    long long now;
    int i;

    rt_memset(&options, 0, sizeof(options));

    for (i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' && rt_strcmp(start_path, ".") == 0) {
            start_path = argv[i];
            continue;
        }

        if (rt_strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            options.name_pattern = argv[i + 1];
            i += 1;
        } else if (rt_strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            options.type_filter = argv[i + 1][0];
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
        } else {
            rt_write_line(2, "Usage: find [path] [-name pattern] [-type f|d] [-mtime n] [-size n[c|k|M]]");
            return 1;
        }
    }

    now = platform_get_epoch_time();
    return find_walk(start_path, &options, now) == 0 ? 0 : 1;
}
