#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define DU_ENTRY_CAPACITY 1024
#define DU_PATH_CAPACITY 1024

typedef struct {
    int summarize;
    int human_readable;
} DuOptions;

static void print_total(unsigned long long total, const char *path, const DuOptions *options) {
    char size_text[32];

    tool_format_size(total, options->human_readable, size_text, sizeof(size_text));
    rt_write_cstr(1, size_text);
    rt_write_char(1, '\t');
    rt_write_line(1, path);
}

static unsigned long long du_path(const char *path, const DuOptions *options, int print_each, int *ok_out) {
    PlatformDirEntry entries[DU_ENTRY_CAPACITY];
    PlatformDirEntry current;
    size_t count = 0;
    int is_directory = 0;
    unsigned long long total = 0ULL;
    size_t i;

    *ok_out = 0;
    if (platform_get_path_info(path, &current) != 0) {
        return 0ULL;
    }

    if (!current.is_dir) {
        *ok_out = 1;
        if (print_each) {
            print_total(current.size, path, options);
        }
        return current.size;
    }

    if (platform_collect_entries(path, 1, entries, DU_ENTRY_CAPACITY, &count, &is_directory) != 0 || !is_directory) {
        return 0ULL;
    }

    total = current.size;

    for (i = 0; i < count; ++i) {
        char child_path[DU_PATH_CAPACITY];
        int child_ok = 0;

        if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
            continue;
        }

        if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0) {
            platform_free_entries(entries, count);
            return 0ULL;
        }

        total += du_path(child_path, options, print_each && !options->summarize, &child_ok);
        if (!child_ok) {
            platform_free_entries(entries, count);
            return 0ULL;
        }
    }

    if (print_each) {
        print_total(total, path, options);
    }

    platform_free_entries(entries, count);
    *ok_out = 1;
    return total;
}

int main(int argc, char **argv) {
    DuOptions options;
    int exit_code = 0;
    int argi = 1;
    int i;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 's') {
                options.summarize = 1;
            } else if (*flag == 'h') {
                options.human_readable = 1;
            } else {
                rt_write_line(2, "Usage: du [-s] [-h] [path ...]");
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (argi >= argc) {
        int ok = 0;
        unsigned long long total = du_path(".", &options, options.summarize ? 0 : 1, &ok);
        if (!ok) {
            rt_write_line(2, "du: failed to inspect .");
            return 1;
        }
        if (options.summarize) {
            print_total(total, ".", &options);
        }
        return 0;
    }

    for (i = argi; i < argc; ++i) {
        int ok = 0;
        unsigned long long total = du_path(argv[i], &options, options.summarize ? 0 : 1, &ok);
        if (!ok) {
            rt_write_cstr(2, "du: failed to inspect ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }
        if (options.summarize) {
            print_total(total, argv[i], &options);
        }
    }

    return exit_code;
}
