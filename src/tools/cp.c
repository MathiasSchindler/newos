#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define CP_ENTRY_CAPACITY 1024
#define CP_PATH_CAPACITY 1024

typedef struct {
    int recursive;
} CpOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-r] source destination");
}

static int ensure_directory(const char *path) {
    int is_directory = 0;

    if (platform_make_directory(path, 0755) == 0) {
        return 0;
    }

    if (platform_path_is_directory(path, &is_directory) == 0 && is_directory) {
        return 0;
    }

    return -1;
}

static int path_is_same_or_child(const char *path, const char *prefix) {
    size_t i = 0;

    if (prefix[0] == '/' && prefix[1] == '\0') {
        return 1;
    }

    while (prefix[i] != '\0' && path[i] == prefix[i]) {
        i += 1U;
    }

    if (prefix[i] != '\0') {
        return 0;
    }

    return path[i] == '\0' || path[i] == '/';
}

static int resolve_copy_target(const char *source_path, const char *dest_path, int source_is_directory, char *buffer, size_t buffer_size) {
    int dest_is_directory = 0;

    if (platform_path_is_directory(dest_path, &dest_is_directory) == 0 && dest_is_directory) {
        return tool_join_path(dest_path, tool_base_name(source_path), buffer, buffer_size);
    }

    if (source_is_directory) {
        if (rt_strlen(dest_path) + 1 > buffer_size) {
            return -1;
        }
        rt_copy_string(buffer, buffer_size, dest_path);
        return 0;
    }

    return tool_resolve_destination(source_path, dest_path, buffer, buffer_size);
}

static int copy_path_recursive(const char *source_path, const char *dest_path) {
    int is_directory = 0;

    if (platform_path_is_directory(source_path, &is_directory) != 0) {
        return -1;
    }

    if (!is_directory) {
        return tool_copy_file(source_path, dest_path);
    }

    if (ensure_directory(dest_path) != 0) {
        return -1;
    }

    {
        PlatformDirEntry entries[CP_ENTRY_CAPACITY];
        size_t count = 0;
        size_t i;
        int path_is_directory = 0;
        int result = 0;

        if (platform_collect_entries(source_path, 1, entries, CP_ENTRY_CAPACITY, &count, &path_is_directory) != 0 || !path_is_directory) {
            return -1;
        }

        for (i = 0; i < count; ++i) {
            char child_source[CP_PATH_CAPACITY];
            char child_dest[CP_PATH_CAPACITY];

            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
                continue;
            }

            if (tool_join_path(source_path, entries[i].name, child_source, sizeof(child_source)) != 0 ||
                tool_join_path(dest_path, entries[i].name, child_dest, sizeof(child_dest)) != 0 ||
                copy_path_recursive(child_source, child_dest) != 0) {
                result = -1;
                break;
            }
        }

        platform_free_entries(entries, count);
        return result;
    }
}

static int copy_one_path(const char *source_path, const char *dest_path, const CpOptions *options) {
    char target_path[CP_PATH_CAPACITY];
    int source_is_directory = 0;

    if (platform_path_is_directory(source_path, &source_is_directory) != 0) {
        rt_write_cstr(2, "cp: cannot access ");
        rt_write_line(2, source_path);
        return 1;
    }

    if (source_is_directory && !options->recursive) {
        rt_write_cstr(2, "cp: omitting directory ");
        rt_write_line(2, source_path);
        return 1;
    }

    if (resolve_copy_target(source_path, dest_path, source_is_directory, target_path, sizeof(target_path)) != 0) {
        rt_write_line(2, "cp: destination path too long");
        return 1;
    }

    if (rt_strcmp(source_path, target_path) == 0) {
        rt_write_line(2, "cp: source and destination are the same");
        return 1;
    }

    if (source_is_directory && path_is_same_or_child(target_path, source_path)) {
        rt_write_cstr(2, "cp: cannot copy directory into itself: ");
        rt_write_line(2, source_path);
        return 1;
    }

    if (copy_path_recursive(source_path, target_path) != 0) {
        rt_write_cstr(2, "cp: failed to copy ");
        rt_write_cstr(2, source_path);
        rt_write_cstr(2, " to ");
        rt_write_line(2, target_path);
        return 1;
    }

    return 0;
}

int main(int argc, char **argv) {
    CpOptions options;
    int argi = 1;
    int source_count;
    const char *dest_path;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        const char *flag = argv[argi] + 1;

        while (*flag != '\0') {
            if (*flag == 'r' || *flag == 'R') {
                options.recursive = 1;
            } else {
                print_usage(argv[0]);
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    source_count = argc - argi - 1;
    if (source_count < 1) {
        print_usage(argv[0]);
        return 1;
    }

    dest_path = argv[argc - 1];
    if (source_count > 1) {
        int dest_is_directory = 0;
        int i;

        if (platform_path_is_directory(dest_path, &dest_is_directory) != 0 || !dest_is_directory) {
            rt_write_line(2, "cp: target for multiple sources must be an existing directory");
            return 1;
        }

        for (i = argi; i < argc - 1; ++i) {
            if (copy_one_path(argv[i], dest_path, &options) != 0) {
                exit_code = 1;
            }
        }
        return exit_code;
    }

    return copy_one_path(argv[argi], dest_path, &options);
}
