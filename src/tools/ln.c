#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define LN_PATH_CAPACITY 1024

typedef struct {
    int symbolic;
    int force;
    int no_dereference;
    int no_target_directory;
    int verbose;
} LnOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-s] [-f] [-n] [-T] [-v] target... linkname|directory");
}

static int path_exists(const char *path) {
    PlatformDirEntry entry;
    return platform_get_path_info(path, &entry) == 0;
}

static int copy_base_name(const char *path, char *buffer, size_t buffer_size) {
    size_t len;
    size_t start;
    size_t out_len;

    if (path == 0 || buffer == 0 || buffer_size == 0) {
        return -1;
    }

    len = rt_strlen(path);
    while (len > 1U && path[len - 1U] == '/') {
        len -= 1U;
    }

    start = len;
    while (start > 0U && path[start - 1U] != '/') {
        start -= 1U;
    }

    out_len = len - start;
    if (out_len == 0U || out_len + 1U > buffer_size) {
        return -1;
    }

    memcpy(buffer, path + start, out_len);
    buffer[out_len] = '\0';
    return 0;
}

static int destination_is_directory(const char *dest_path, const LnOptions *options) {
    int is_directory = 0;

    if (options->no_target_directory) {
        return 0;
    }

    if (options->no_dereference) {
        char target[2];
        if (platform_read_symlink(dest_path, target, sizeof(target)) == 0) {
            return 0;
        }
    }

    return platform_path_is_directory(dest_path, &is_directory) == 0 && is_directory;
}

static int resolve_link_path(const char *target_path, const char *dest_path, const LnOptions *options, char *buffer, size_t buffer_size) {
    char base_name[LN_PATH_CAPACITY];

    if (destination_is_directory(dest_path, options)) {
        if (copy_base_name(target_path, base_name, sizeof(base_name)) != 0) {
            return -1;
        }
        return tool_join_path(dest_path, base_name, buffer, buffer_size);
    }

    if (rt_strlen(dest_path) + 1U > buffer_size) {
        return -1;
    }

    rt_copy_string(buffer, buffer_size, dest_path);
    return 0;
}

static int prepare_link_path(const char *link_path, const LnOptions *options) {
    int is_directory = 0;

    if (!options->force || !path_exists(link_path)) {
        return 0;
    }

    if (platform_path_is_directory(link_path, &is_directory) == 0 && is_directory) {
        rt_write_line(2, "ln: refusing to replace directory");
        return 1;
    }

    if (platform_remove_file(link_path) != 0) {
        rt_write_cstr(2, "ln: failed to replace ");
        rt_write_line(2, link_path);
        return 1;
    }

    return 0;
}

static void print_verbose_link(const char *target_path, const char *link_path, const LnOptions *options) {
    rt_write_char(1, '\'');
    rt_write_cstr(1, link_path);
    rt_write_cstr(1, options->symbolic ? "' -> '" : "' => '");
    rt_write_cstr(1, target_path);
    rt_write_line(1, "'");
}

static int create_one_link(const char *target_path, const char *dest_path, const LnOptions *options) {
    char link_path[LN_PATH_CAPACITY];

    if (resolve_link_path(target_path, dest_path, options, link_path, sizeof(link_path)) != 0) {
        rt_write_line(2, "ln: destination path too long");
        return 1;
    }

    if (prepare_link_path(link_path, options) != 0) {
        return 1;
    }

    if (options->symbolic) {
        if (platform_create_symbolic_link(target_path, link_path) != 0) {
            rt_write_cstr(2, "ln: failed to create symbolic link ");
            rt_write_line(2, link_path);
            return 1;
        }
    } else {
        if (platform_create_hard_link(target_path, link_path) != 0) {
            rt_write_cstr(2, "ln: failed to create hard link ");
            rt_write_line(2, link_path);
            return 1;
        }
    }

    if (options->verbose) {
        print_verbose_link(target_path, link_path, options);
    }

    return 0;
}

int main(int argc, char **argv) {
    LnOptions options;
    int argi = 1;
    int source_count;
    int i;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 's') {
                options.symbolic = 1;
            } else if (*flag == 'f') {
                options.force = 1;
            } else if (*flag == 'n') {
                options.no_dereference = 1;
            } else if (*flag == 'T') {
                options.no_target_directory = 1;
            } else if (*flag == 'v') {
                options.verbose = 1;
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

    if (source_count > 1) {
        if (!destination_is_directory(argv[argc - 1], &options)) {
            rt_write_line(2, "ln: target for multiple links must be an existing directory");
            return 1;
        }
    }

    for (i = argi; i < argc - 1; ++i) {
        if (create_one_link(argv[i], argv[argc - 1], &options) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
