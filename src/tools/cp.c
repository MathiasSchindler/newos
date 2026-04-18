#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define CP_PATH_CAPACITY 2048

typedef struct {
    int recursive;
    int interactive;
    int no_clobber;
    int update_only;
    int verbose;
    int preserve_mode;
    int preserve_times;
    int preserve_symlinks;
} CpOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-a] [-f] [-i] [-n] [-p] [-r|-R] [-u] [-v] source... destination");
}

static int path_is_same_or_child(const char *path, const char *prefix) {
    char normalized_path[CP_PATH_CAPACITY];
    char normalized_prefix[CP_PATH_CAPACITY];
    size_t i = 0;

    if (tool_canonicalize_path(path, 0, 1, normalized_path, sizeof(normalized_path)) != 0) {
        rt_copy_string(normalized_path, sizeof(normalized_path), path);
    }
    if (tool_canonicalize_path(prefix, 0, 1, normalized_prefix, sizeof(normalized_prefix)) != 0) {
        rt_copy_string(normalized_prefix, sizeof(normalized_prefix), prefix);
    }

    if (normalized_prefix[0] == '/' && normalized_prefix[1] == '\0') {
        return 1;
    }

    while (normalized_prefix[i] != '\0' && normalized_path[i] == normalized_prefix[i]) {
        i += 1U;
    }

    if (normalized_prefix[i] != '\0') {
        return 0;
    }

    return normalized_path[i] == '\0' || normalized_path[i] == '/';
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

static int should_copy_to_target(const char *source_path, const char *target_path, int source_is_directory, const CpOptions *options) {
    PlatformDirEntry source_info;
    PlatformDirEntry target_info;

    if (!tool_path_exists(target_path)) {
        return 1;
    }

    if (options->no_clobber) {
        return 0;
    }

    if (options->update_only &&
        !source_is_directory &&
        platform_get_path_info(source_path, &source_info) == 0 &&
        platform_get_path_info(target_path, &target_info) == 0 &&
        source_info.mtime <= target_info.mtime) {
        return 0;
    }

    if (options->interactive) {
        return tool_prompt_yes_no("cp: overwrite ", target_path);
    }

    return 1;
}

static void preserve_copy_metadata(const char *source_path, const char *target_path, const CpOptions *options) {
    PlatformDirEntry source_info;

    if ((!options->preserve_mode && !options->preserve_times) ||
        platform_get_path_info(source_path, &source_info) != 0) {
        return;
    }

    if (options->preserve_mode) {
        (void)platform_change_mode(target_path, source_info.mode & 07777U);
    }
    if (options->preserve_times) {
        (void)platform_set_path_times(target_path, source_info.atime, source_info.mtime, 0, 1, 1);
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

    if (tool_paths_equal(source_path, target_path)) {
        rt_write_line(2, "cp: source and destination are the same");
        return 1;
    }

    if (source_is_directory && path_is_same_or_child(target_path, source_path)) {
        rt_write_cstr(2, "cp: cannot copy directory into itself: ");
        rt_write_line(2, source_path);
        return 1;
    }

    if (!should_copy_to_target(source_path, target_path, source_is_directory, options)) {
        return 0;
    }

    if (tool_copy_path(source_path, target_path, options->recursive, options->preserve_mode, options->preserve_symlinks) != 0) {
        rt_write_cstr(2, "cp: failed to copy ");
        rt_write_cstr(2, source_path);
        rt_write_cstr(2, " to ");
        rt_write_line(2, target_path);
        return 1;
    }

    preserve_copy_metadata(source_path, target_path, options);

    if (options->verbose) {
        rt_write_cstr(1, source_path);
        rt_write_cstr(1, " -> ");
        rt_write_line(1, target_path);
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
            if (*flag == 'a') {
                options.recursive = 1;
                options.preserve_mode = 1;
                options.preserve_times = 1;
                options.preserve_symlinks = 1;
            } else if (*flag == 'r' || *flag == 'R') {
                options.recursive = 1;
            } else if (*flag == 'i') {
                options.interactive = 1;
                options.no_clobber = 0;
            } else if (*flag == 'f') {
                options.interactive = 0;
                options.no_clobber = 0;
            } else if (*flag == 'n') {
                options.no_clobber = 1;
                options.interactive = 0;
            } else if (*flag == 'u') {
                options.update_only = 1;
            } else if (*flag == 'v') {
                options.verbose = 1;
            } else if (*flag == 'p') {
                options.preserve_mode = 1;
                options.preserve_times = 1;
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
