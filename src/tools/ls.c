#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define LS_ENTRY_CAPACITY 1024
#define LS_PATH_CAPACITY 1024

typedef enum {
    LS_SORT_NAME,
    LS_SORT_TIME,
    LS_SORT_SIZE
} LsSortMode;

typedef struct {
    int show_all;
    int long_format;
    int recursive;
    LsSortMode sort_mode;
} LsOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-a] [-l] [-R] [-t] [-S] [path ...]");
}

static int should_swap_entries(const PlatformDirEntry *left, const PlatformDirEntry *right, const LsOptions *options) {
    if (options->sort_mode == LS_SORT_TIME) {
        if (right->mtime != left->mtime) {
            return right->mtime > left->mtime;
        }
    } else if (options->sort_mode == LS_SORT_SIZE) {
        if (right->size != left->size) {
            return right->size > left->size;
        }
    }

    return rt_strcmp(right->name, left->name) < 0;
}

static void sort_entries(PlatformDirEntry *entries, size_t count, const LsOptions *options) {
    size_t i;
    size_t j;

    for (i = 0; i < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            if (should_swap_entries(&entries[i], &entries[j], options)) {
                PlatformDirEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
}

static size_t count_digits_unsigned(unsigned long long value) {
    size_t digits = 1;

    while (value >= 10) {
        value /= 10;
        digits += 1;
    }

    return digits;
}

static void write_padding(size_t current_width, size_t target_width) {
    while (current_width < target_width) {
        rt_write_char(1, ' ');
        current_width += 1;
    }
}

static void print_long_entry(const PlatformDirEntry *entry) {
    char mode_buffer[11];

    platform_format_mode(entry->mode, mode_buffer);

    rt_write_cstr(1, mode_buffer);
    rt_write_char(1, ' ');
    write_padding(count_digits_unsigned(entry->nlink), 3);
    rt_write_uint(1, entry->nlink);
    rt_write_char(1, ' ');
    rt_write_cstr(1, entry->owner[0] ? entry->owner : "?");
    rt_write_char(1, ' ');
    rt_write_cstr(1, entry->group[0] ? entry->group : "?");
    rt_write_char(1, ' ');
    write_padding(count_digits_unsigned(entry->size), 8);
    rt_write_uint(1, entry->size);
    rt_write_char(1, ' ');
    rt_write_int(1, entry->mtime);
    rt_write_char(1, ' ');
    rt_write_line(1, entry->name);
}

static void print_entries(PlatformDirEntry *entries, size_t count, const LsOptions *options) {
    size_t i;

    sort_entries(entries, count, options);

    for (i = 0; i < count; ++i) {
        if (options->long_format) {
            print_long_entry(&entries[i]);
        } else {
            rt_write_line(1, entries[i].name);
        }
    }
}

static int list_path(const char *path, const LsOptions *options, int print_header) {
    PlatformDirEntry entries[LS_ENTRY_CAPACITY];
    size_t count = 0;
    int is_directory = 0;
    int exit_code = 0;

    if (platform_collect_entries(path, options->show_all, entries, LS_ENTRY_CAPACITY, &count, &is_directory) != 0) {
        rt_write_cstr(2, "ls: cannot access ");
        rt_write_line(2, path);
        return 1;
    }

    if (print_header) {
        rt_write_cstr(1, path);
        rt_write_line(1, ":");
    }

    print_entries(entries, count, options);

    if (is_directory && options->recursive) {
        size_t i;

        for (i = 0; i < count; ++i) {
            char child_path[LS_PATH_CAPACITY];

            if (!entries[i].is_dir || rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
                continue;
            }

            if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0) {
                exit_code = 1;
                continue;
            }

            rt_write_char(1, '\n');
            if (list_path(child_path, options, 1) != 0) {
                exit_code = 1;
            }
        }
    }

    platform_free_entries(entries, count);
    return exit_code;
}

int main(int argc, char **argv) {
    LsOptions options;
    int first_path_index = 1;
    int exit_code = 0;
    int i;
    int path_count;

    rt_memset(&options, 0, sizeof(options));

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        size_t j;

        if (rt_strcmp(arg, "--") == 0) {
            first_path_index = i + 1;
            break;
        }

        if (arg[0] != '-' || arg[1] == '\0') {
            first_path_index = i;
            break;
        }

        for (j = 1; arg[j] != '\0'; ++j) {
            switch (arg[j]) {
                case 'a':
                    options.show_all = 1;
                    break;
                case 'l':
                    options.long_format = 1;
                    break;
                case 'R':
                    options.recursive = 1;
                    break;
                case 't':
                    options.sort_mode = LS_SORT_TIME;
                    break;
                case 'S':
                    options.sort_mode = LS_SORT_SIZE;
                    break;
                default:
                    print_usage(argv[0]);
                    return 1;
            }
        }

        first_path_index = i + 1;
    }

    path_count = argc - first_path_index;
    if (path_count <= 0) {
        return list_path(".", &options, options.recursive);
    }

    for (i = first_path_index; i < argc; ++i) {
        int print_header = (path_count > 1 || options.recursive) ? 1 : 0;
        int result = list_path(argv[i], &options, print_header);

        if (result != 0) {
            exit_code = result;
        }

        if (i + 1 < argc) {
            rt_write_char(1, '\n');
        }
    }

    return exit_code;
}
