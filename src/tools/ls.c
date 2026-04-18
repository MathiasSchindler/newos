#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define LS_ENTRY_CAPACITY 1024
#define LS_PATH_CAPACITY 1024
#define LS_MODE_TYPE_MASK 0170000U
#define LS_MODE_FIFO 0010000U
#define LS_MODE_DIRECTORY 0040000U
#define LS_MODE_SYMLINK 0120000U
#define LS_MODE_SOCKET 0140000U

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
    int show_inode;
    int show_blocks;
    int color_mode;
    LsSortMode sort_mode;
} LsOptions;

typedef struct {
    size_t inode_width;
    size_t block_width;
} LsLayout;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-a] [-d] [-h] [-i] [-l] [-R] [-s] [-t] [-S] [-r] [-1] [-F] [--color[=WHEN]] [path ...]");
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

static unsigned long long ls_entry_blocks(const PlatformDirEntry *entry) {
    if (entry->size == 0ULL) {
        return 0ULL;
    }
    return (entry->size + 1023ULL) / 1024ULL;
}

static void build_layout(PlatformDirEntry *entries, size_t count, const LsOptions *options, LsLayout *layout) {
    size_t i;

    layout->inode_width = 0U;
    layout->block_width = 0U;

    for (i = 0; i < count; ++i) {
        if (options->show_inode) {
            size_t width = count_digits_unsigned(entries[i].inode);
            if (width > layout->inode_width) {
                layout->inode_width = width;
            }
        }
        if (options->show_blocks || options->long_format) {
            size_t width = count_digits_unsigned(ls_entry_blocks(&entries[i]));
            if (width > layout->block_width) {
                layout->block_width = width;
            }
        }
    }
}

static int ls_use_color(const LsOptions *options) {
    if (options->color_mode == 2) {
        return 1;
    }
    if (options->color_mode == 1) {
        return platform_isatty(1);
    }
    return 0;
}

static char classify_suffix(const PlatformDirEntry *entry, const LsOptions *options) {
    unsigned int file_type = entry->mode & LS_MODE_TYPE_MASK;

    if (!options->classify) {
        return '\0';
    }
    if (entry->is_dir || file_type == LS_MODE_DIRECTORY) {
        return '/';
    }
    if (file_type == LS_MODE_SYMLINK) {
        return '@';
    }
    if (file_type == LS_MODE_FIFO) {
        return '|';
    }
    if (file_type == LS_MODE_SOCKET) {
        return '=';
    }
    if ((entry->mode & 0111U) != 0U) {
        return '*';
    }
    return '\0';
}

static const char *entry_color_code(const PlatformDirEntry *entry, const LsOptions *options) {
    unsigned int file_type = entry->mode & LS_MODE_TYPE_MASK;

    if (!ls_use_color(options)) {
        return "";
    }
    if (entry->is_dir || file_type == LS_MODE_DIRECTORY) {
        return "\033[1;34m";
    }
    if (file_type == LS_MODE_SYMLINK) {
        return "\033[1;36m";
    }
    if (file_type == LS_MODE_SOCKET) {
        return "\033[1;35m";
    }
    if (file_type == LS_MODE_FIFO) {
        return "\033[33m";
    }
    if ((entry->mode & 0111U) != 0U) {
        return "\033[1;32m";
    }
    return "";
}

static void print_numeric_prefix(unsigned long long value, size_t width) {
    write_padding(count_digits_unsigned(value), width);
    rt_write_uint(1, value);
    rt_write_char(1, ' ');
}

static void print_entry_prefix(const PlatformDirEntry *entry, const LsOptions *options, const LsLayout *layout) {
    if (options->show_inode) {
        print_numeric_prefix(entry->inode, layout->inode_width);
    }
    if (options->show_blocks) {
        print_numeric_prefix(ls_entry_blocks(entry), layout->block_width);
    }
}

static void print_entry_name(const PlatformDirEntry *entry, const LsOptions *options, const LsLayout *layout) {
    char suffix = classify_suffix(entry, options);
    const char *color = entry_color_code(entry, options);
    int use_color = color[0] != '\0';

    print_entry_prefix(entry, options, layout);
    if (use_color) {
        rt_write_cstr(1, color);
    }
    rt_write_cstr(1, entry->name);
    if (use_color) {
        rt_write_cstr(1, "\033[0m");
    }
    if (suffix != '\0') {
        rt_write_char(1, suffix);
    }
    rt_write_char(1, '\n');
}

static void print_long_entry(const PlatformDirEntry *entry, const LsOptions *options, const LsLayout *layout) {
    char mode_buffer[11];
    char size_buffer[32];

    platform_format_mode(entry->mode, mode_buffer);
    tool_format_size(entry->size, options->human_readable, size_buffer, sizeof(size_buffer));

    print_entry_prefix(entry, options, layout);
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
    print_entry_name(entry, options, layout);
}

static void print_entries(PlatformDirEntry *entries, size_t count, const LsOptions *options) {
    size_t i;
    LsLayout layout;

    sort_entries(entries, count, options);
    build_layout(entries, count, options, &layout);
    for (i = 0; i < count; ++i) {
        if (options->long_format) {
            print_long_entry(&entries[i], options, &layout);
        } else {
            print_entry_name(&entries[i], options, &layout);
        }
    }
}

static int print_single_path(const char *path, const LsOptions *options) {
    PlatformDirEntry entry;
    LsLayout layout;

    if (platform_get_path_info(path, &entry) != 0) {
        rt_write_cstr(2, "ls: cannot access ");
        rt_write_line(2, path);
        return 1;
    }

    rt_memset(&layout, 0, sizeof(layout));
    if (options->show_inode) {
        layout.inode_width = count_digits_unsigned(entry.inode);
    }
    if (options->show_blocks) {
        layout.block_width = count_digits_unsigned(ls_entry_blocks(&entry));
    }

    if (options->long_format) {
        print_long_entry(&entry, options, &layout);
    } else {
        print_entry_name(&entry, options, &layout);
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

    if (options->long_format || options->show_blocks) {
        unsigned long long total_blocks = 0ULL;
        size_t i;
        for (i = 0; i < count; ++i) {
            total_blocks += ls_entry_blocks(&entries[i]);
        }
        rt_write_cstr(1, "total ");
        rt_write_uint(1, total_blocks);
        rt_write_char(1, '\n');
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

        if (arg[0] == '-' && arg[1] == '-') {
            if (rt_strcmp(arg, "--color") == 0 || rt_strcmp(arg, "--color=auto") == 0) {
                options.color_mode = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--color=always") == 0) {
                options.color_mode = 2;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--color=never") == 0) {
                options.color_mode = 0;
                first_path_index = i + 1;
                continue;
            }
            print_usage(argv[0]);
            return 1;
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
                case 'i':
                    options.show_inode = 1;
                    break;
                case 'l':
                    options.long_format = 1;
                    break;
                case 'R':
                    options.recursive = 1;
                    break;
                case 's':
                    options.show_blocks = 1;
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
