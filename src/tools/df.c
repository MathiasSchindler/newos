#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int human_readable;
    int inode_mode;
    int show_type;
    unsigned long long block_size;
} DfOptions;

typedef struct {
    char path[1024];
    PlatformFilesystemInfo info;
} DfRow;

typedef struct {
    size_t filesystem_width;
    size_t type_width;
    size_t size_width;
    size_t used_width;
    size_t avail_width;
    size_t use_width;
    size_t mount_width;
} DfLayout;

#define DF_ROW_CAPACITY 64
#define DF_SCAN_CAPACITY 128

static void write_padding(size_t current_width, size_t desired_width) {
    while (current_width < desired_width) {
        rt_write_char(1, ' ');
        current_width += 1U;
    }
}

static void write_text_cell(const char *text, size_t width) {
    size_t length = rt_strlen(text);
    rt_write_cstr(1, text);
    write_padding(length, width);
    rt_write_char(1, ' ');
}

static void format_size_text(unsigned long long value, const DfOptions *options, char *buffer, size_t buffer_size) {
    if (options->human_readable) {
        tool_format_size(value, 1, buffer, buffer_size);
        return;
    }
    if (options->block_size > 1ULL) {
        unsigned long long scaled = (value == 0ULL) ? 0ULL : ((value + options->block_size - 1ULL) / options->block_size);
        rt_unsigned_to_string(scaled, buffer, buffer_size);
        return;
    }
    rt_unsigned_to_string(value, buffer, buffer_size);
}

static void format_percent_text(unsigned long long used, unsigned long long total, char *buffer, size_t buffer_size) {
    unsigned long long use_percent = (total == 0ULL) ? 0ULL : (used * 100ULL) / total;
    char digits[32];
    size_t length;

    rt_unsigned_to_string(use_percent, digits, sizeof(digits));
    length = rt_strlen(digits);
    if (length + 2U > buffer_size) {
        if (buffer_size > 0U) {
            buffer[0] = '\0';
        }
        return;
    }
    rt_copy_string(buffer, buffer_size, digits);
    buffer[length] = '%';
    buffer[length + 1U] = '\0';
}

static int same_row_path(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static int same_filesystem_info(const PlatformFilesystemInfo *left, const PlatformFilesystemInfo *right) {
    return left->total_bytes == right->total_bytes &&
           left->free_bytes == right->free_bytes &&
           left->available_bytes == right->available_bytes &&
           left->total_inodes == right->total_inodes &&
           left->free_inodes == right->free_inodes &&
           left->available_inodes == right->available_inodes &&
           rt_strcmp(left->type_name, right->type_name) == 0;
}

static int add_row(DfRow *rows, size_t *count, const char *path, int dedupe_by_filesystem) {
    char resolved[1024];
    const char *lookup_path = path;
    PlatformFilesystemInfo info;
    size_t i;

    if (path == 0 || count == 0 || *count >= DF_ROW_CAPACITY) {
        return -1;
    }

    if (tool_canonicalize_path(path, 1, 0, resolved, sizeof(resolved)) == 0) {
        lookup_path = resolved;
    }

    if (platform_get_filesystem_info(lookup_path, &info) != 0) {
        return -1;
    }

    for (i = 0; i < *count; ++i) {
        if (same_row_path(rows[i].path, lookup_path)) {
            return 0;
        }
        if (dedupe_by_filesystem && same_filesystem_info(&rows[i].info, &info)) {
            return 0;
        }
    }

    rt_copy_string(rows[*count].path, sizeof(rows[*count].path), lookup_path);
    rows[*count].info = info;
    *count += 1U;
    return 0;
}

static void collect_child_mounts(const char *base_path, DfRow *rows, size_t *count) {
    PlatformDirEntry entries[DF_SCAN_CAPACITY];
    size_t entry_count = 0;
    size_t i;
    int is_directory = 0;

    if (platform_collect_entries(base_path, 0, entries, DF_SCAN_CAPACITY, &entry_count, &is_directory) != 0 || !is_directory) {
        return;
    }

    for (i = 0; i < entry_count && *count < DF_ROW_CAPACITY; ++i) {
        char child_path[1024];

        if (!entries[i].is_dir) {
            continue;
        }
        if (tool_join_path(base_path, entries[i].name, child_path, sizeof(child_path)) != 0) {
            continue;
        }
        (void)add_row(rows, count, child_path, 1);
    }

    platform_free_entries(entries, entry_count);
}

static void collect_default_rows(DfRow *rows, size_t *count) {
    char current_directory[1024];

    (void)add_row(rows, count, "/", 1);
    if (platform_get_current_directory(current_directory, sizeof(current_directory)) == 0) {
        (void)add_row(rows, count, current_directory, 1);
    }
    collect_child_mounts("/", rows, count);
    collect_child_mounts("/System/Volumes", rows, count);
    collect_child_mounts("/Volumes", rows, count);
}

static void build_layout(const DfRow *rows, size_t count, const DfOptions *options, DfLayout *layout) {
    size_t i;

    layout->filesystem_width = rt_strlen("Filesystem");
    layout->type_width = rt_strlen("Type");
    layout->size_width = rt_strlen(options->block_size == 1024ULL ? "1K-blocks" : "Size");
    layout->used_width = rt_strlen(options->inode_mode ? "IUsed" : "Used");
    layout->avail_width = rt_strlen(options->inode_mode ? "IFree" : "Available");
    layout->use_width = rt_strlen(options->inode_mode ? "IUse%" : "Use%");
    layout->mount_width = rt_strlen("Mounted on");

    for (i = 0; i < count; ++i) {
        char total_text[32];
        char used_text[32];
        char avail_text[32];
        char use_text[32];
        unsigned long long used_inodes;
        unsigned long long used_bytes;

        if (rt_strlen(rows[i].path) > layout->filesystem_width) {
            layout->filesystem_width = rt_strlen(rows[i].path);
        }
        if (rt_strlen(rows[i].path) > layout->mount_width) {
            layout->mount_width = rt_strlen(rows[i].path);
        }
        if (rt_strlen(rows[i].info.type_name) > layout->type_width) {
            layout->type_width = rt_strlen(rows[i].info.type_name);
        }

        if (options->inode_mode) {
            used_inodes = (rows[i].info.total_inodes >= rows[i].info.free_inodes) ? (rows[i].info.total_inodes - rows[i].info.free_inodes) : 0ULL;
            rt_unsigned_to_string(rows[i].info.total_inodes, total_text, sizeof(total_text));
            rt_unsigned_to_string(used_inodes, used_text, sizeof(used_text));
            rt_unsigned_to_string(rows[i].info.available_inodes, avail_text, sizeof(avail_text));
            format_percent_text(used_inodes, rows[i].info.total_inodes, use_text, sizeof(use_text));
        } else {
            used_bytes = (rows[i].info.total_bytes >= rows[i].info.free_bytes) ? (rows[i].info.total_bytes - rows[i].info.free_bytes) : 0ULL;
            format_size_text(rows[i].info.total_bytes, options, total_text, sizeof(total_text));
            format_size_text(used_bytes, options, used_text, sizeof(used_text));
            format_size_text(rows[i].info.available_bytes, options, avail_text, sizeof(avail_text));
            format_percent_text(used_bytes, rows[i].info.total_bytes, use_text, sizeof(use_text));
        }

        if (rt_strlen(total_text) > layout->size_width) {
            layout->size_width = rt_strlen(total_text);
        }
        if (rt_strlen(used_text) > layout->used_width) {
            layout->used_width = rt_strlen(used_text);
        }
        if (rt_strlen(avail_text) > layout->avail_width) {
            layout->avail_width = rt_strlen(avail_text);
        }
        if (rt_strlen(use_text) > layout->use_width) {
            layout->use_width = rt_strlen(use_text);
        }
    }
}

static void print_header(const DfOptions *options, const DfLayout *layout) {
    write_text_cell("Filesystem", layout->filesystem_width);
    if (options->show_type) {
        write_text_cell("Type", layout->type_width);
    }
    if (options->inode_mode) {
        write_text_cell("Inodes", layout->size_width);
        write_text_cell("IUsed", layout->used_width);
        write_text_cell("IFree", layout->avail_width);
        write_text_cell("IUse%", layout->use_width);
    } else {
        write_text_cell(options->block_size == 1024ULL ? "1K-blocks" : "Size", layout->size_width);
        write_text_cell("Used", layout->used_width);
        write_text_cell("Available", layout->avail_width);
        write_text_cell("Use%", layout->use_width);
    }
    rt_write_cstr(1, "Mounted on");
    rt_write_char(1, '\n');
}

static void print_row(const DfRow *row, const DfOptions *options, const DfLayout *layout) {
    char total_text[32];
    char used_text[32];
    char avail_text[32];
    char use_text[32];
    unsigned long long used_inodes;
    unsigned long long used_bytes;

    rt_write_cstr(1, row->path);
    rt_write_char(1, ' ');
    if (options->show_type) {
        write_text_cell(row->info.type_name[0] != '\0' ? row->info.type_name : "-", layout->type_width);
    }

    if (options->inode_mode) {
        used_inodes = (row->info.total_inodes >= row->info.free_inodes) ? (row->info.total_inodes - row->info.free_inodes) : 0ULL;
        rt_unsigned_to_string(row->info.total_inodes, total_text, sizeof(total_text));
        rt_unsigned_to_string(used_inodes, used_text, sizeof(used_text));
        rt_unsigned_to_string(row->info.available_inodes, avail_text, sizeof(avail_text));
        format_percent_text(used_inodes, row->info.total_inodes, use_text, sizeof(use_text));
    } else {
        used_bytes = (row->info.total_bytes >= row->info.free_bytes) ? (row->info.total_bytes - row->info.free_bytes) : 0ULL;
        format_size_text(row->info.total_bytes, options, total_text, sizeof(total_text));
        format_size_text(used_bytes, options, used_text, sizeof(used_text));
        format_size_text(row->info.available_bytes, options, avail_text, sizeof(avail_text));
        format_percent_text(used_bytes, row->info.total_bytes, use_text, sizeof(use_text));
    }

    write_text_cell(total_text, layout->size_width);
    write_text_cell(used_text, layout->used_width);
    write_text_cell(avail_text, layout->avail_width);
    write_text_cell(use_text, layout->use_width);
    rt_write_line(1, row->path);
}

int main(int argc, char **argv) {
    DfOptions options;
    DfRow rows[DF_ROW_CAPACITY];
    size_t row_count = 0;
    DfLayout layout;
    int argi = 1;
    int i;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));
    options.block_size = 1ULL;
    rt_memset(rows, 0, sizeof(rows));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 'h') {
                options.human_readable = 1;
            } else if (*flag == 'i') {
                options.inode_mode = 1;
            } else if (*flag == 'k') {
                options.block_size = 1024ULL;
            } else if (*flag == 'T') {
                options.show_type = 1;
            } else {
                rt_write_line(2, "Usage: df [-h] [-i] [-k] [-T] [path ...]");
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (argi >= argc) {
        collect_default_rows(rows, &row_count);
    } else {
        for (i = argi; i < argc; ++i) {
            if (add_row(rows, &row_count, argv[i], 0) != 0) {
                rt_write_cstr(2, "df: failed to inspect ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
            }
        }
    }

    if (row_count == 0U) {
        return exit_code != 0 ? exit_code : 1;
    }

    build_layout(rows, row_count, &options, &layout);
    print_header(&options, &layout);
    for (i = 0; i < (int)row_count; ++i) {
        print_row(&rows[i], &options, &layout);
    }

    return exit_code;
}
