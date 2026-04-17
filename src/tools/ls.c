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
    int directory_as_file;
    int human_readable;
    int long_format;
    int recursive;
    int reverse_order;
    int classify;
    int single_column;
    LsSortMode sort_mode;
} LsOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-a] [-d] [-h] [-l] [-R] [-t] [-S] [-r] [-1] [-F] [path ...]");
}

static int compare_entries(const PlatformDirEntry *left, const PlatformDirEntry *right, const LsOptions *options) {
    if (options->sort_mode == LS_SORT_TIME) {
        if (left->mtime > right->mtime) {
            return -1;
        }
        if (left->mtime < right->mtime) {
            return 1;
        }
    } else if (options->sort_mode == LS_SORT_SIZE) {
        if (left->size > right->size) {
            return -1;
        }
        if (left->size < right->size) {
            return 1;
        }
    }

    return rt_strcmp(left->name, right->name);
}

static int should_swap_entries(const PlatformDirEntry *left, const PlatformDirEntry *right, const LsOptions *options) {
    int cmp = compare_entries(left, right, options);
    return options->reverse_order ? (cmp < 0) : (cmp > 0);
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
    while (value >= 10ULL) {
        value /= 10ULL;
        digits += 1U;
    }
    return digits;
}

static void write_padding(size_t current_width, size_t desired_width) {
    while (current_width < desired_width) {
        rt_write_char(1, ' ');
        current_width += 1U;
    }
}

static void print_entry_name(const PlatformDirEntry *entry, const LsOptions *options) {
    rt_write_cstr(1, entry->name);
    if (options->classify) {
        if (entry->is_dir) {
            rt_write_char(1, '/');
        } else if ((entry->mode & 0111U) != 0U) {
            rt_write_char(1, '*');
        }
    }
    rt_write_char(1, '\n');
}

static void print_long_entry(const PlatformDirEntry *entry, const LsOptions *options) {
    char mode_buffer[11];
    char size_buffer[32];

    platform_format_mode(entry->mode, mode_buffer);
    tool_format_size(entry->size, options->human_readable, size_buffer, sizeof(size_buffer));

    rt_write_cstr(1, mode_buffer);
    rt_write_char(1, ' ');
    write_padding(count_digits_unsigned((unsigned long long)entry->nlink), 3U);
    rt_write_uint(1, (unsigned long long)entry->nlink);
    rt_write_char(1, ' ');
    rt_write_cstr(1, entry->owner[0] ? entry->owner : "?");
    rt_write_char(1, ' ');
    rt_write_cstr(1, entry->group[0] ? entry->group : "?");
    rt_write_char(1, ' ');
    write_padding(rt_strlen(size_buffer), 8U);
    rt_write_cstr(1, size_buffer);
    rt_write_char(1, ' ');
    rt_write_int(1, entry->mtime);
    rt_write_char(1, ' ');
    print_entry_name(entry, options);
}

static void print_entries(PlatformDirEntry *entries, size_t count, const LsOptions *options) {
    size_t i;

    sort_entries(entries, count, options);
    for (i = 0; i < count; ++i) {
        if (options->long_format) {
            print_long_entry(&entries[i], options);
        } else {
            print_entry_name(&entries[i], options);
        }
    }
}

static int print_single_path(const char *path, const LsOptions *options) {
    PlatformDirEntry entry;

    if (platform_get_path_info(path, &entry) != 0) {
        rt_write_cstr(2, "ls: cannot access ");
        rt_write_line(2, path);
        return 1;
    }

    if (options->long_format) {
        print_long_entry(&entry, options);
    } else {
        print_entry_name(&entry, options);
    }

    return 0;
}

static int list_path(const char *path, const LsOptions *options, int print_header) {
    PlatformDirEntry entries[LS_ENTRY_CAPACITY];
    size_t count = 0;
    int is_directory = 0;
    int exit_code = 0;

    if (options->directory_as_file) {
        return print_single_path(path, options);
    }

    if (platform_collect_entries(path, options->show_all, entries, LS_ENTRY_CAPACITY, &count, &is_directory) != 0) {
        rt_write_cstr(2, "ls: cannot access ");
        rt_write_line(2, path);
        return 1;
    }

    if (!is_directory) {
        print_entries(entries, count, options);
        platform_free_entries(entries, count);
        return 0;
    }

    if (print_header) {
        rt_write_cstr(1, path);
        rt_write_line(1, ":");
    }

    print_entries(entries, count, options);

    if (options->recursive) {
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
                case 'd':
                    options.directory_as_file = 1;
                    break;
                case 'h':
                    options.human_readable = 1;
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
                case 'r':
                    options.reverse_order = 1;
                    break;
                case '1':
                    options.single_column = 1;
                    break;
                case 'F':
                    options.classify = 1;
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
        return list_path(".", &options, 0);
    }

    for (i = first_path_index; i < argc; ++i) {
        int print_header = (path_count > 1 || options.recursive) && !options.directory_as_file;
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
