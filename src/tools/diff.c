#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define DIFF_MAX_LINES 4096
#define DIFF_MAX_LINE_LENGTH 1024
#define DIFF_ENTRY_CAPACITY 1024
#define DIFF_PATH_CAPACITY 1024

typedef struct {
    int unified;
    int context;
    int recursive;
    int brief;
} DiffOptions;

static size_t diff_max_size(size_t left, size_t right) {
    return (left > right) ? left : right;
}

static size_t diff_min_size(size_t left, size_t right) {
    return (left < right) ? left : right;
}

static int store_line(
    char lines[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH],
    size_t *count,
    const char *line,
    size_t len,
    int *truncated_out
) {
    size_t copy_len = len;

    if (*count >= DIFF_MAX_LINES) {
        *truncated_out = 1;
        return 0;
    }
    if (copy_len >= DIFF_MAX_LINE_LENGTH) {
        copy_len = DIFF_MAX_LINE_LENGTH - 1U;
        *truncated_out = 1;
    }

    memcpy(lines[*count], line, copy_len);
    lines[*count][copy_len] = '\0';
    *count += 1U;
    return 0;
}

static int collect_lines_from_fd(
    int fd,
    char lines[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH],
    size_t *count,
    int *truncated_out
) {
    char buffer[2048];
    char current[DIFF_MAX_LINE_LENGTH];
    size_t current_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];
            if (ch == '\n') {
                if (store_line(lines, count, current, current_len, truncated_out) != 0) {
                    return -1;
                }
                current_len = 0;
            } else if (current_len + 1U < sizeof(current)) {
                current[current_len++] = ch;
            } else {
                *truncated_out = 1;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (current_len > 0U) {
        return store_line(lines, count, current, current_len, truncated_out);
    }

    return 0;
}

static void print_brief_difference(const char *left_path, const char *right_path) {
    rt_write_cstr(1, "Files ");
    rt_write_cstr(1, left_path);
    rt_write_cstr(1, " and ");
    rt_write_cstr(1, right_path);
    rt_write_line(1, " differ");
}

static void print_default_diff(
    char left[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH],
    size_t left_count,
    char right[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH],
    size_t right_count
) {
    size_t i;

    for (i = 0; i < left_count || i < right_count; ++i) {
        const char *lhs = (i < left_count) ? left[i] : "";
        const char *rhs = (i < right_count) ? right[i] : "";

        if ((i < left_count) != (i < right_count) || rt_strcmp(lhs, rhs) != 0) {
            rt_write_cstr(1, "line ");
            rt_write_uint(1, (unsigned long long)(i + 1U));
            rt_write_line(1, ":");
            if (i < left_count) {
                rt_write_cstr(1, "< ");
                rt_write_line(1, lhs);
            }
            if (i < right_count) {
                rt_write_cstr(1, "> ");
                rt_write_line(1, rhs);
            }
        }
    }
}

static void write_diff_range(size_t start, size_t count) {
    if (count == 0U) {
        rt_write_uint(1, (unsigned long long)start);
        rt_write_cstr(1, ",0");
        return;
    }

    rt_write_uint(1, (unsigned long long)(start + 1U));
    if (count > 1U) {
        rt_write_char(1, ',');
        rt_write_uint(1, (unsigned long long)count);
    }
}

static size_t find_common_prefix(
    char left[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH],
    size_t left_count,
    char right[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH],
    size_t right_count
) {
    size_t prefix = 0U;
    size_t limit = diff_min_size(left_count, right_count);

    while (prefix < limit && rt_strcmp(left[prefix], right[prefix]) == 0) {
        prefix += 1U;
    }

    return prefix;
}

static void find_common_suffix(
    char left[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH],
    size_t left_count,
    char right[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH],
    size_t right_count,
    size_t prefix,
    size_t *left_suffix_out,
    size_t *right_suffix_out
) {
    size_t left_suffix = left_count;
    size_t right_suffix = right_count;

    while (left_suffix > prefix &&
           right_suffix > prefix &&
           rt_strcmp(left[left_suffix - 1U], right[right_suffix - 1U]) == 0) {
        left_suffix -= 1U;
        right_suffix -= 1U;
    }

    *left_suffix_out = left_suffix;
    *right_suffix_out = right_suffix;
}

static void print_unified_diff(
    const char *left_name,
    const char *right_name,
    char left[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH],
    size_t left_count,
    char right[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH],
    size_t right_count
) {
    size_t i;
    size_t prefix = find_common_prefix(left, left_count, right, right_count);
    size_t left_suffix = left_count;
    size_t right_suffix = right_count;
    size_t context = 3U;
    size_t left_from;
    size_t right_from;
    size_t left_to;
    size_t right_to;
    size_t line_count;

    find_common_suffix(left, left_count, right, right_count, prefix, &left_suffix, &right_suffix);

    left_from = (prefix > context) ? (prefix - context) : 0U;
    right_from = left_from;
    left_to = diff_min_size(left_count, left_suffix + context);
    right_to = diff_min_size(right_count, right_suffix + context);
    line_count = diff_max_size(left_to, right_to);

    rt_write_cstr(1, "--- ");
    rt_write_line(1, left_name);
    rt_write_cstr(1, "+++ ");
    rt_write_line(1, right_name);
    rt_write_cstr(1, "@@ -");
    write_diff_range(left_from, left_to - left_from);
    rt_write_cstr(1, " +");
    write_diff_range(right_from, right_to - right_from);
    rt_write_line(1, " @@");

    for (i = left_from; i < line_count; ++i) {
        const char *lhs = (i < left_to) ? left[i] : 0;
        const char *rhs = (i < right_to) ? right[i] : 0;

        if ((i < prefix || (i >= left_suffix && i >= right_suffix)) &&
            lhs != 0 && rhs != 0 && rt_strcmp(lhs, rhs) == 0) {
            rt_write_cstr(1, " ");
            rt_write_line(1, lhs);
        } else {
            if (lhs != 0) {
                rt_write_cstr(1, "-");
                rt_write_line(1, lhs);
            }
            if (rhs != 0) {
                rt_write_cstr(1, "+");
                rt_write_line(1, rhs);
            }
        }
    }
}

static void print_context_diff(
    const char *left_name,
    const char *right_name,
    char left[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH],
    size_t left_count,
    char right[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH],
    size_t right_count
) {
    size_t i;
    size_t prefix = find_common_prefix(left, left_count, right, right_count);
    size_t left_suffix = left_count;
    size_t right_suffix = right_count;
    size_t context = 3U;
    size_t left_from;
    size_t right_from;
    size_t left_to;
    size_t right_to;

    find_common_suffix(left, left_count, right, right_count, prefix, &left_suffix, &right_suffix);

    left_from = (prefix > context) ? (prefix - context) : 0U;
    right_from = left_from;
    left_to = diff_min_size(left_count, left_suffix + context);
    right_to = diff_min_size(right_count, right_suffix + context);

    rt_write_cstr(1, "*** ");
    rt_write_line(1, left_name);
    rt_write_cstr(1, "--- ");
    rt_write_line(1, right_name);
    rt_write_line(1, "***************");
    rt_write_cstr(1, "*** ");
    write_diff_range(left_from, left_to - left_from);
    rt_write_line(1, " ****");
    for (i = left_from; i < left_to; ++i) {
        if (i < prefix || i >= left_suffix) {
            rt_write_cstr(1, "  ");
        } else {
            rt_write_cstr(1, "! ");
        }
        rt_write_line(1, left[i]);
    }
    rt_write_cstr(1, "--- ");
    write_diff_range(right_from, right_to - right_from);
    rt_write_line(1, " ----");
    for (i = right_from; i < right_to; ++i) {
        if (i < prefix || i >= right_suffix) {
            rt_write_cstr(1, "  ");
        } else {
            rt_write_cstr(1, "! ");
        }
        rt_write_line(1, right[i]);
    }
}

static int files_differ_bytewise(const char *left_path, const char *right_path, int *differences_out) {
    int left_fd = platform_open_read(left_path);
    int right_fd = platform_open_read(right_path);
    char left_buffer[4096];
    char right_buffer[4096];

    *differences_out = 0;

    if (left_fd < 0 || right_fd < 0) {
        if (left_fd >= 0) {
            (void)platform_close(left_fd);
        }
        if (right_fd >= 0) {
            (void)platform_close(right_fd);
        }
        return -1;
    }

    for (;;) {
        long left_read = platform_read(left_fd, left_buffer, sizeof(left_buffer));
        long right_read = platform_read(right_fd, right_buffer, sizeof(right_buffer));
        long i;

        if (left_read < 0 || right_read < 0) {
            (void)platform_close(left_fd);
            (void)platform_close(right_fd);
            return -1;
        }
        if (left_read != right_read) {
            *differences_out = 1;
            break;
        }
        if (left_read == 0) {
            break;
        }
        for (i = 0; i < left_read; ++i) {
            if (left_buffer[i] != right_buffer[i]) {
                *differences_out = 1;
                break;
            }
        }
        if (*differences_out != 0) {
            break;
        }
    }

    (void)platform_close(left_fd);
    (void)platform_close(right_fd);
    return 0;
}

static int compare_regular_files(const char *left_path, const char *right_path, const DiffOptions *options, int *differences_out) {
    static char left[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH];
    static char right[DIFF_MAX_LINES][DIFF_MAX_LINE_LENGTH];
    size_t left_count = 0U;
    size_t right_count = 0U;
    int fd;
    int should_close;
    int left_truncated = 0;
    int right_truncated = 0;
    int differences = 0;
    int used_fast_compare = 0;

    if (!(left_path[0] == '-' && left_path[1] == '\0') &&
        !(right_path[0] == '-' && right_path[1] == '\0')) {
        if (files_differ_bytewise(left_path, right_path, &differences) != 0) {
            rt_write_line(2, "diff: cannot read input file");
            return -1;
        }
        used_fast_compare = 1;

        if (!differences) {
            *differences_out = 0;
            return 0;
        }

        if (options->brief) {
            print_brief_difference(left_path, right_path);
            *differences_out = 1;
            return 0;
        }
    }

    if (tool_open_input(left_path, &fd, &should_close) != 0) {
        rt_write_line(2, "diff: cannot read first file");
        return -1;
    }
    if (collect_lines_from_fd(fd, left, &left_count, &left_truncated) != 0) {
        tool_close_input(fd, should_close);
        rt_write_line(2, "diff: cannot read first file");
        return -1;
    }
    tool_close_input(fd, should_close);

    if (tool_open_input(right_path, &fd, &should_close) != 0) {
        rt_write_line(2, "diff: cannot read second file");
        return -1;
    }
    if (collect_lines_from_fd(fd, right, &right_count, &right_truncated) != 0) {
        tool_close_input(fd, should_close);
        rt_write_line(2, "diff: cannot read second file");
        return -1;
    }
    tool_close_input(fd, should_close);

    if (!used_fast_compare) {
        size_t i;

        for (i = 0U; i < left_count || i < right_count; ++i) {
            const char *lhs = (i < left_count) ? left[i] : "";
            const char *rhs = (i < right_count) ? right[i] : "";

            if ((i < left_count) != (i < right_count) || rt_strcmp(lhs, rhs) != 0) {
                differences = 1;
                break;
            }
        }

        if (!differences) {
            *differences_out = 0;
            return 0;
        }

        if (options->brief) {
            print_brief_difference(left_path, right_path);
            *differences_out = 1;
            return 0;
        }
    }

    if (left_truncated || right_truncated) {
        print_brief_difference(left_path, right_path);
    } else if (options->context) {
        print_context_diff(left_path, right_path, left, left_count, right, right_count);
    } else if (options->unified) {
            print_unified_diff(left_path, right_path, left, left_count, right, right_count);
    } else {
        print_default_diff(left, left_count, right, right_count);
    }

    *differences_out = 1;
    return 0;
}

static void sort_entries(PlatformDirEntry *entries, size_t count) {
    size_t i;
    size_t j;

    for (i = 0U; i < count; ++i) {
        for (j = i + 1U; j < count; ++j) {
            if (rt_strcmp(entries[i].name, entries[j].name) > 0) {
                PlatformDirEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
}

static int is_dot_entry(const char *name) {
    return rt_strcmp(name, ".") == 0 || rt_strcmp(name, "..") == 0;
}

static int compare_paths(const char *left_path, const char *right_path, const DiffOptions *options, int *differences_out);

static int compare_directories(const char *left_path, const char *right_path, const DiffOptions *options, int *differences_out) {
    PlatformDirEntry left_entries[DIFF_ENTRY_CAPACITY];
    PlatformDirEntry right_entries[DIFF_ENTRY_CAPACITY];
    size_t left_count = 0U;
    size_t right_count = 0U;
    int left_is_directory = 0;
    int right_is_directory = 0;
    size_t left_index = 0U;
    size_t right_index = 0U;
    int differences = 0;

    if (platform_collect_entries(left_path, 1, left_entries, DIFF_ENTRY_CAPACITY, &left_count, &left_is_directory) != 0 ||
        platform_collect_entries(right_path, 1, right_entries, DIFF_ENTRY_CAPACITY, &right_count, &right_is_directory) != 0 ||
        !left_is_directory || !right_is_directory) {
        rt_write_line(2, "diff: cannot compare directories");
        return -1;
    }

    sort_entries(left_entries, left_count);
    sort_entries(right_entries, right_count);

    while (left_index < left_count || right_index < right_count) {
        const char *left_name = 0;
        const char *right_name = 0;

        while (left_index < left_count && is_dot_entry(left_entries[left_index].name)) {
            left_index += 1U;
        }
        while (right_index < right_count && is_dot_entry(right_entries[right_index].name)) {
            right_index += 1U;
        }
        if (left_index >= left_count && right_index >= right_count) {
            break;
        }

        if (left_index < left_count) {
            left_name = left_entries[left_index].name;
        }
        if (right_index < right_count) {
            right_name = right_entries[right_index].name;
        }

        if (right_name == 0 || (left_name != 0 && rt_strcmp(left_name, right_name) < 0)) {
            rt_write_cstr(1, "Only in ");
            rt_write_cstr(1, left_path);
            rt_write_cstr(1, ": ");
            rt_write_line(1, left_name);
            differences = 1;
            left_index += 1U;
            continue;
        }

        if (left_name == 0 || rt_strcmp(left_name, right_name) > 0) {
            rt_write_cstr(1, "Only in ");
            rt_write_cstr(1, right_path);
            rt_write_cstr(1, ": ");
            rt_write_line(1, right_name);
            differences = 1;
            right_index += 1U;
            continue;
        }

        {
            char left_child[DIFF_PATH_CAPACITY];
            char right_child[DIFF_PATH_CAPACITY];
            int child_differences = 0;

            if (tool_join_path(left_path, left_name, left_child, sizeof(left_child)) != 0 ||
                tool_join_path(right_path, right_name, right_child, sizeof(right_child)) != 0 ||
                compare_paths(left_child, right_child, options, &child_differences) != 0) {
                platform_free_entries(left_entries, left_count);
                platform_free_entries(right_entries, right_count);
                return -1;
            }
            if (child_differences) {
                differences = 1;
            }
        }

        left_index += 1U;
        right_index += 1U;
    }

    platform_free_entries(left_entries, left_count);
    platform_free_entries(right_entries, right_count);
    *differences_out = differences;
    return 0;
}

static int compare_paths(const char *left_path, const char *right_path, const DiffOptions *options, int *differences_out) {
    PlatformDirEntry left_entry;
    PlatformDirEntry right_entry;

    *differences_out = 0;

    if (platform_get_path_info(left_path, &left_entry) != 0 || platform_get_path_info(right_path, &right_entry) != 0) {
        rt_write_line(2, "diff: cannot access input path");
        return -1;
    }

    if (left_entry.is_dir || right_entry.is_dir) {
        if (!left_entry.is_dir || !right_entry.is_dir) {
            rt_write_cstr(1, "File type differs: ");
            rt_write_cstr(1, left_path);
            rt_write_cstr(1, " and ");
            rt_write_line(1, right_path);
            *differences_out = 1;
            return 0;
        }

        if (!options->recursive) {
            rt_write_line(2, "diff: use -r to compare directories");
            return -1;
        }

        return compare_directories(left_path, right_path, options, differences_out);
    }

    return compare_regular_files(left_path, right_path, options, differences_out);
}

int main(int argc, char **argv) {
    DiffOptions options;
    int argi = 1;
    int differences = 0;

    rt_memset(&options, 0, sizeof(options));
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 'u') {
                options.unified = 1;
                options.context = 0;
            } else if (*flag == 'c') {
                options.context = 1;
                options.unified = 0;
            } else if (*flag == 'q') {
                options.brief = 1;
            } else if (*flag == 'r') {
                options.recursive = 1;
            } else {
                rt_write_line(2, "Usage: diff [-u|-c] [-q] [-r] file1 file2");
                return 1;
            }
            flag += 1;
        }
        argi += 1;
    }

    if (argc - argi != 2) {
        rt_write_line(2, "Usage: diff [-u|-c] [-q] [-r] file1 file2");
        return 1;
    }

    if (compare_paths(argv[argi], argv[argi + 1], &options, &differences) != 0) {
        return 1;
    }

    return differences;
}
