#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define TREE_MAX_DEPTH 64
#define TREE_MAX_ENTRIES 2048

typedef struct {
    int show_all;
    int dirs_only;
    int max_depth;
    unsigned long long files;
    unsigned long long dirs;
    int branch_more[TREE_MAX_DEPTH + 1];
} TreeOptions;

typedef struct {
    char path[1024];
    char name[PLATFORM_NAME_CAPACITY];
    int is_dir;
} TreeEntry;

static int compare_entries(const void *left_ptr, const void *right_ptr) {
    const TreeEntry *left = (const TreeEntry *)left_ptr;
    const TreeEntry *right = (const TreeEntry *)right_ptr;

    if (left->is_dir != right->is_dir) {
        return left->is_dir ? -1 : 1;
    }
    return rt_strcmp(left->name, right->name);
}

static void print_usage(void) {
    tool_write_usage("tree", "[-a] [-d] [-L LEVEL] [PATH ...]");
}

static void write_prefix(const TreeOptions *options, int depth) {
    int i;

    for (i = 0; i < depth; ++i) {
        rt_write_cstr(1, options->branch_more[i] ? "|   " : "    ");
    }
}

static int collect_entries(const char *path, const TreeOptions *options, TreeEntry *entries, size_t *count_out) {
    PlatformDirEntry dir_entries[TREE_MAX_ENTRIES];
    size_t dir_count = 0U;
    size_t i;
    size_t count = 0U;
    int is_directory = 0;

    if (platform_collect_entries(path, options->show_all, dir_entries, TREE_MAX_ENTRIES, &dir_count, &is_directory) != 0 || !is_directory) {
        return -1;
    }
    for (i = 0U; i < dir_count && count < TREE_MAX_ENTRIES; ++i) {
        if (!options->show_all && dir_entries[i].name[0] == '.') {
            continue;
        }
        if (options->dirs_only && !dir_entries[i].is_dir) {
            continue;
        }
        rt_copy_string(entries[count].name, sizeof(entries[count].name), dir_entries[i].name);
        if (tool_join_path(path, dir_entries[i].name, entries[count].path, sizeof(entries[count].path)) != 0) {
            continue;
        }
        entries[count].is_dir = dir_entries[i].is_dir;
        count += 1U;
    }
    platform_free_entries(dir_entries, dir_count);
    rt_sort(entries, count, sizeof(entries[0]), compare_entries);
    *count_out = count;
    return 0;
}

static int walk_tree(const char *path, TreeOptions *options, int depth) {
    TreeEntry entries[TREE_MAX_ENTRIES];
    size_t count = 0U;
    size_t i;

    if (options->max_depth >= 0 && depth >= options->max_depth) {
        return 0;
    }
    if (collect_entries(path, options, entries, &count) != 0) {
        write_prefix(options, depth);
        rt_write_line(1, "[error opening dir]");
        return 1;
    }
    for (i = 0U; i < count; ++i) {
        int is_last = i + 1U == count;

        write_prefix(options, depth);
        rt_write_cstr(1, is_last ? "`-- " : "|-- ");
        rt_write_line(1, entries[i].name);
        if (entries[i].is_dir) {
            options->dirs += 1ULL;
            if (depth + 1 < TREE_MAX_DEPTH) {
                options->branch_more[depth] = !is_last;
                if (walk_tree(entries[i].path, options, depth + 1) != 0) {
                    return 1;
                }
            }
        } else {
            options->files += 1ULL;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    TreeOptions options;
    int argi = 1;
    int saw_path = 0;
    int status = 0;

    rt_memset(&options, 0, sizeof(options));
    options.max_depth = -1;
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-a") == 0) {
            options.show_all = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-d") == 0) {
            options.dirs_only = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-L") == 0) {
            unsigned long long level;
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &level, "tree", "level") != 0 || level > TREE_MAX_DEPTH) {
                return 1;
            }
            options.max_depth = (int)level;
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--help") == 0 || rt_strcmp(argv[argi], "-h") == 0) {
            print_usage();
            return 0;
        } else {
            tool_write_error("tree", "unknown option: ", argv[argi]);
            return 1;
        }
    }
    if (argi >= argc) {
        rt_write_line(1, ".");
        if (walk_tree(".", &options, 0) != 0) status = 1;
    }
    for (; argi < argc; ++argi) {
        saw_path = 1;
        rt_write_line(1, argv[argi]);
        if (walk_tree(argv[argi], &options, 0) != 0) status = 1;
    }
    rt_write_char(1, '\n');
    rt_write_uint(1, options.dirs);
    rt_write_cstr(1, options.dirs == 1ULL ? " directory" : " directories");
    if (!options.dirs_only) {
        rt_write_cstr(1, ", ");
        rt_write_uint(1, options.files);
        rt_write_cstr(1, options.files == 1ULL ? " file" : " files");
    }
    rt_write_char(1, '\n');
    (void)saw_path;
    return status;
}
