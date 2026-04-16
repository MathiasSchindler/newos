#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define FIND_MAX_ENTRIES 1024
#define FIND_PATH_CAPACITY 1024

static int find_walk(const char *path, const char *pattern) {
    PlatformDirEntry entries[FIND_MAX_ENTRIES];
    size_t count = 0;
    int is_directory = 0;
    size_t i;

    if (pattern == 0 || tool_wildcard_match(pattern, tool_base_name(path))) {
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
            return -1;
        }

        if (find_walk(child_path, pattern) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    const char *start_path = ".";
    const char *pattern = 0;
    int i;

    if (argc > 1 && argv[1][0] != '-') {
        start_path = argv[1];
    }

    for (i = 1; i < argc; ++i) {
        if (rt_strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            pattern = argv[i + 1];
            i += 1;
        }
    }

    return find_walk(start_path, pattern) == 0 ? 0 : 1;
}
