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

typedef enum {
    LS_TIME_STYLE_HUMAN,
    LS_TIME_STYLE_FULL,
    LS_TIME_STYLE_UNIX
} LsTimeStyle;

typedef struct {
    int show_all;
    int almost_all;
    int directory_as_file;
    int human_readable;
    int long_format;
    int recursive;
    int reverse_order;
    int classify;
    int mark_directories;
    int single_column;
    int show_inode;
    int show_blocks;
    int color_mode;
    int numeric_ids;
    int omit_group;
    int quote_nonprintable;
    LsSortMode sort_mode;
    LsTimeStyle time_style;
} LsOptions;

typedef struct {
    size_t inode_width;
    size_t block_width;
    size_t link_width;
    size_t owner_width;
    size_t group_width;
    size_t size_width;
} LsLayout;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-aA] [-d] [-h] [-i] [-l] [-R] [-s] [-t] [-S] [-r] [-1] [-F] [-p] [-n] [-G] [-q] [--full-time] [--time-style=STYLE] [--color[=WHEN]] [path ...]");
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

static int is_dot_or_dotdot(const char *name) {
    return rt_strcmp(name, ".") == 0 || rt_strcmp(name, "..") == 0;
}

static void format_identity(unsigned int value, const char *text, int numeric, char *buffer, size_t buffer_size) {
    if (numeric) {
        rt_unsigned_to_string((unsigned long long)value, buffer, buffer_size);
        return;
    }
    rt_copy_string(buffer, buffer_size, (text != 0 && text[0] != '\0') ? text : "?");
}

static void format_entry_time(long long epoch_seconds, const LsOptions *options, char *buffer, size_t buffer_size) {
    const char *format = "%Y-%m-%d %H:%M";

    if (options->time_style == LS_TIME_STYLE_UNIX) {
        if (epoch_seconds < 0) {
            rt_copy_string(buffer, buffer_size, "0");
        } else {
            rt_unsigned_to_string((unsigned long long)epoch_seconds, buffer, buffer_size);
        }
        return;
    }

    if (options->time_style == LS_TIME_STYLE_FULL) {
        format = "%Y-%m-%d %H:%M:%S";
    }

    if (platform_format_time(epoch_seconds, 1, format, buffer, buffer_size) != 0) {
        if (epoch_seconds < 0) {
            rt_copy_string(buffer, buffer_size, "0");
        } else {
            rt_unsigned_to_string((unsigned long long)epoch_seconds, buffer, buffer_size);
        }
    }
}

static void write_name_text(const char *text, int replace_nonprintable) {
    size_t i = 0;

    while (text[i] != '\0') {
        unsigned char ch = (unsigned char)text[i];
        if (replace_nonprintable && (ch < 32U || ch == 127U)) {
            rt_write_char(1, '?');
        } else {
            rt_write_char(1, (char)ch);
        }
        i += 1U;
    }
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
    layout->link_width = 0U;
    layout->owner_width = 0U;
    layout->group_width = 0U;
    layout->size_width = 0U;

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
        if (options->long_format) {
            char owner_buffer[32];
            char group_buffer[32];
            char size_buffer[32];
            size_t link_width = count_digits_unsigned((unsigned long long)entries[i].nlink);

            format_identity(entries[i].uid, entries[i].owner, options->numeric_ids, owner_buffer, sizeof(owner_buffer));
            format_identity(entries[i].gid, entries[i].group, options->numeric_ids, group_buffer, sizeof(group_buffer));
            tool_format_size(entries[i].size, options->human_readable, size_buffer, sizeof(size_buffer));

            if (link_width > layout->link_width) {
                layout->link_width = link_width;
            }
            if (rt_strlen(owner_buffer) > layout->owner_width) {
                layout->owner_width = rt_strlen(owner_buffer);
            }
            if (rt_strlen(group_buffer) > layout->group_width) {
                layout->group_width = rt_strlen(group_buffer);
            }
            if (rt_strlen(size_buffer) > layout->size_width) {
                layout->size_width = rt_strlen(size_buffer);
            }
        }
    }
}

static void filter_entries(PlatformDirEntry *entries, size_t *count_io, const LsOptions *options) {
    size_t input = *count_io;
    size_t output = 0;
    size_t i;

    for (i = 0; i < input; ++i) {
        if (options->almost_all && !options->show_all && is_dot_or_dotdot(entries[i].name)) {
            continue;
        }
        if (output != i) {
            entries[output] = entries[i];
        }
        output += 1U;
    }

    *count_io = output;
}

static int ls_use_color(const LsOptions *options) {
    return tool_should_use_color_fd(1, options->color_mode);
}

static char classify_suffix(const PlatformDirEntry *entry, const LsOptions *options) {
    unsigned int file_type = entry->mode & LS_MODE_TYPE_MASK;

    if (!options->classify && !options->mark_directories) {
        return '\0';
    }
    if (entry->is_dir || file_type == LS_MODE_DIRECTORY) {
        return '/';
    }
    if (!options->classify) {
        return '\0';
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

static int entry_color_style(const PlatformDirEntry *entry, const LsOptions *options) {
    unsigned int file_type = entry->mode & LS_MODE_TYPE_MASK;

    if (!ls_use_color(options)) {
        return TOOL_STYLE_PLAIN;
    }
    if (entry->is_dir || file_type == LS_MODE_DIRECTORY) {
        return TOOL_STYLE_BOLD_BLUE;
    }
    if (file_type == LS_MODE_SYMLINK) {
        return TOOL_STYLE_BOLD_CYAN;
    }
    if (file_type == LS_MODE_SOCKET) {
        return TOOL_STYLE_BOLD_MAGENTA;
    }
    if (file_type == LS_MODE_FIFO) {
        return TOOL_STYLE_YELLOW;
    }
    if ((entry->mode & 0111U) != 0U) {
        return TOOL_STYLE_BOLD_GREEN;
    }
    return TOOL_STYLE_PLAIN;
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

static void print_entry_display_name(const PlatformDirEntry *entry, const char *full_path, const LsOptions *options, int show_link_target) {
    char target[LS_PATH_CAPACITY];
    char suffix = classify_suffix(entry, options);
    int style = entry_color_style(entry, options);
    int use_color = style != TOOL_STYLE_PLAIN;
    unsigned int file_type = entry->mode & LS_MODE_TYPE_MASK;

    if (use_color) {
        tool_style_begin(1, options->color_mode, style);
    }
    write_name_text(entry->name, options->quote_nonprintable);
    if (use_color) {
        tool_style_end(1, options->color_mode);
    }
    if (suffix != '\0') {
        rt_write_char(1, suffix);
    }
    if (show_link_target && full_path != 0 && file_type == LS_MODE_SYMLINK && platform_read_symlink(full_path, target, sizeof(target)) == 0) {
        rt_write_cstr(1, " -> ");
        write_name_text(target, options->quote_nonprintable);
    }
}

static void print_short_entry(const PlatformDirEntry *entry, const char *full_path, const LsOptions *options, const LsLayout *layout) {
    print_entry_prefix(entry, options, layout);
    print_entry_display_name(entry, full_path, options, 0);
    rt_write_char(1, '\n');
}

static void print_long_entry(const PlatformDirEntry *entry, const char *full_path, const LsOptions *options, const LsLayout *layout) {
    char mode_buffer[11];
    char size_buffer[32];
    char time_buffer[32];
    char owner_buffer[32];
    char group_buffer[32];

    platform_format_mode(entry->mode, mode_buffer);
    tool_format_size(entry->size, options->human_readable, size_buffer, sizeof(size_buffer));
    format_identity(entry->uid, entry->owner, options->numeric_ids, owner_buffer, sizeof(owner_buffer));
    format_identity(entry->gid, entry->group, options->numeric_ids, group_buffer, sizeof(group_buffer));
    format_entry_time(entry->mtime, options, time_buffer, sizeof(time_buffer));

    print_entry_prefix(entry, options, layout);
    rt_write_cstr(1, mode_buffer);
    rt_write_char(1, ' ');
    write_padding(count_digits_unsigned((unsigned long long)entry->nlink), layout->link_width);
    rt_write_uint(1, (unsigned long long)entry->nlink);
    rt_write_char(1, ' ');
    rt_write_cstr(1, owner_buffer);
    write_padding(rt_strlen(owner_buffer), layout->owner_width);
    rt_write_char(1, ' ');
    if (!options->omit_group) {
        rt_write_cstr(1, group_buffer);
        write_padding(rt_strlen(group_buffer), layout->group_width);
        rt_write_char(1, ' ');
    }
    write_padding(rt_strlen(size_buffer), layout->size_width);
    rt_write_cstr(1, size_buffer);
    rt_write_char(1, ' ');
    rt_write_cstr(1, time_buffer);
    rt_write_char(1, ' ');
    print_entry_display_name(entry, full_path, options, 1);
    rt_write_char(1, '\n');
}

static void print_entries(PlatformDirEntry *entries, size_t count, const LsOptions *options, const char *base_path, int path_is_directory) {
    size_t i;
    LsLayout layout;

    sort_entries(entries, count, options);
    build_layout(entries, count, options, &layout);
    for (i = 0; i < count; ++i) {
        char full_path[LS_PATH_CAPACITY];
        const char *entry_path = 0;

        if (path_is_directory) {
            if (tool_join_path(base_path, entries[i].name, full_path, sizeof(full_path)) == 0) {
                entry_path = full_path;
            }
        } else {
            entry_path = base_path;
        }

        if (options->long_format) {
            print_long_entry(&entries[i], entry_path, options, &layout);
        } else {
            print_short_entry(&entries[i], entry_path, options, &layout);
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
        build_layout(&entry, 1U, options, &layout);
        print_long_entry(&entry, path, options, &layout);
    } else {
        print_short_entry(&entry, path, options, &layout);
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

    if (platform_collect_entries(path, options->show_all || options->almost_all, entries, LS_ENTRY_CAPACITY, &count, &is_directory) != 0) {
        rt_write_cstr(2, "ls: cannot access ");
        rt_write_line(2, path);
        return 1;
    }

    if (!is_directory) {
        print_entries(entries, count, options, path, 0);
        platform_free_entries(entries, count);
        return 0;
    }

    filter_entries(entries, &count, options);

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

    print_entries(entries, count, options, path, 1);

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
    options.color_mode = TOOL_COLOR_AUTO;
    options.time_style = LS_TIME_STYLE_HUMAN;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        size_t j;

        if (rt_strcmp(arg, "--") == 0) {
            first_path_index = i + 1;
            break;
        }

        if (arg[0] == '-' && arg[1] == '-') {
            if (rt_strcmp(arg, "--help") == 0) {
                print_usage(argv[0]);
                return 0;
            }
            if (rt_strcmp(arg, "--all") == 0) {
                options.show_all = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--almost-all") == 0) {
                options.almost_all = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--directory") == 0) {
                options.directory_as_file = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--human-readable") == 0) {
                options.human_readable = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--inode") == 0) {
                options.show_inode = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--long") == 0) {
                options.long_format = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--recursive") == 0) {
                options.recursive = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--reverse") == 0) {
                options.reverse_order = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--classify") == 0) {
                options.classify = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--indicator-style=slash") == 0) {
                options.mark_directories = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--numeric-uid-gid") == 0) {
                options.numeric_ids = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--no-group") == 0) {
                options.omit_group = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--quote-name") == 0) {
                options.quote_nonprintable = 1;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--full-time") == 0) {
                options.long_format = 1;
                options.time_style = LS_TIME_STYLE_FULL;
                first_path_index = i + 1;
                continue;
            }
            if (rt_strncmp(arg, "--time-style=", 13U) == 0) {
                const char *style = arg + 13;
                if (rt_strcmp(style, "unix") == 0) {
                    options.time_style = LS_TIME_STYLE_UNIX;
                } else if (rt_strcmp(style, "full-iso") == 0) {
                    options.time_style = LS_TIME_STYLE_FULL;
                } else if (rt_strcmp(style, "long-iso") == 0 || rt_strcmp(style, "iso") == 0) {
                    options.time_style = LS_TIME_STYLE_HUMAN;
                } else {
                    print_usage(argv[0]);
                    return 1;
                }
                first_path_index = i + 1;
                continue;
            }
            if (rt_strcmp(arg, "--color") == 0) {
                options.color_mode = TOOL_COLOR_AUTO;
                tool_set_global_color_mode(options.color_mode);
                first_path_index = i + 1;
                continue;
            }
            if (rt_strncmp(arg, "--color=", 8U) == 0) {
                if (tool_parse_color_mode(arg + 8, &options.color_mode) != 0) {
                    print_usage(argv[0]);
                    return 1;
                }
                tool_set_global_color_mode(options.color_mode);
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
                case 'A':
                    options.almost_all = 1;
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
                case 'p':
                    options.mark_directories = 1;
                    break;
                case 'n':
                    options.numeric_ids = 1;
                    break;
                case 'G':
                    options.omit_group = 1;
                    break;
                case 'q':
                    options.quote_nonprintable = 1;
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
