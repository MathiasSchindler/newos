#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define DU_ENTRY_CAPACITY 1024
#define DU_PATH_CAPACITY 1024

static unsigned long long du_path(const char *path, int *ok_out) {
    PlatformDirEntry entries[DU_ENTRY_CAPACITY];
    size_t count = 0;
    int is_directory = 0;
    unsigned long long total = 0;
    size_t i;

    *ok_out = 0;
    if (platform_collect_entries(path, 1, entries, DU_ENTRY_CAPACITY, &count, &is_directory) != 0 || count == 0) {
        return 0ULL;
    }

    if (!is_directory) {
        *ok_out = 1;
        return entries[0].size;
    }

    for (i = 0; i < count; ++i) {
        char child_path[DU_PATH_CAPACITY];
        int child_ok = 0;

        if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
            continue;
        }

        if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0) {
            return 0ULL;
        }

        total += du_path(child_path, &child_ok);
        if (!child_ok) {
            return 0ULL;
        }
    }

    *ok_out = 1;
    return total;
}

int main(int argc, char **argv) {
    int exit_code = 0;
    int i;

    if (argc == 1) {
        int ok = 0;
        unsigned long long total = du_path(".", &ok);
        if (!ok) {
            rt_write_line(2, "du: failed to inspect .");
            return 1;
        }
        rt_write_uint(1, total);
        rt_write_cstr(1, "\t.\n");
        return 0;
    }

    for (i = 1; i < argc; ++i) {
        int ok = 0;
        unsigned long long total = du_path(argv[i], &ok);
        if (!ok) {
            rt_write_cstr(2, "du: failed to inspect ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }
        rt_write_uint(1, total);
        rt_write_char(1, '\t');
        rt_write_line(1, argv[i]);
    }

    return exit_code;
}
