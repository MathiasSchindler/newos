#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define LN_PATH_CAPACITY 1024

typedef struct {
    int symbolic;
    int force;
    int no_dereference;
    int no_target_directory;
    int relative;
    int verbose;
} LnOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-s] [-f] [-n] [-T] [-r] [-v] TARGET [LINK_NAME]");
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

static int copy_parent_directory(const char *path, char *buffer, size_t buffer_size) {
    size_t len;

    if (path == 0 || buffer == 0 || buffer_size == 0) {
        return -1;
    }

    len = rt_strlen(path);
    while (len > 1U && path[len - 1U] == '/') {
        len -= 1U;
    }
    while (len > 0U && path[len - 1U] != '/') {
        len -= 1U;
    }

    if (len == 0U) {
        if (buffer_size < 2U) {
            return -1;
        }
        buffer[0] = '.';
        buffer[1] = '\0';
        return 0;
    }

    if (len == 1U) {
        if (buffer_size < 2U) {
            return -1;
        }
        buffer[0] = '/';
        buffer[1] = '\0';
        return 0;
    }

    if (len + 1U > buffer_size) {
        return -1;
    }

    memcpy(buffer, path, len - 1U);
    buffer[len - 1U] = '\0';
    return 0;
}

static const char *skip_path_separators(const char *path) {
    while (path != 0 && *path == '/') {
        path += 1;
    }
    return path;
}

static size_t path_component_length(const char *path) {
    size_t length = 0U;

    while (path[length] != '\0' && path[length] != '/') {
        length += 1U;
    }

    return length;
}

static int path_components_match(const char *left, size_t left_length, const char *right, size_t right_length) {
    size_t i;

    if (left_length != right_length) {
        return 0;
    }

    for (i = 0U; i < left_length; ++i) {
        if (left[i] != right[i]) {
            return 0;
        }
    }

    return 1;
}

static int append_fragment(char *buffer, size_t buffer_size, size_t *length_io, const char *text) {
    size_t length = *length_io;
    size_t i = 0U;

    while (text[i] != '\0') {
        if (length + 1U >= buffer_size) {
            return -1;
        }
        buffer[length++] = text[i++];
    }

    buffer[length] = '\0';
    *length_io = length;
    return 0;
}

static int append_path_component(char *buffer, size_t buffer_size, size_t *length_io, const char *component, size_t component_length) {
    size_t length = *length_io;
    size_t i;

    if (component_length == 0U) {
        return 0;
    }

    if (length != 0U && buffer[length - 1U] != '/') {
        if (length + 1U >= buffer_size) {
            return -1;
        }
        buffer[length++] = '/';
    }

    for (i = 0U; i < component_length; ++i) {
        if (length + 1U >= buffer_size) {
            return -1;
        }
        buffer[length++] = component[i];
    }

    buffer[length] = '\0';
    *length_io = length;
    return 0;
}

static int build_relative_target_path(const char *target_path, const char *link_path, char *buffer, size_t buffer_size) {
    char target_absolute[LN_PATH_CAPACITY];
    char link_parent[LN_PATH_CAPACITY];
    char link_parent_absolute[LN_PATH_CAPACITY];
    const char *target_cursor;
    const char *link_cursor;
    size_t length = 0U;

    if (tool_canonicalize_path(target_path, 0, 1, target_absolute, sizeof(target_absolute)) != 0 ||
        copy_parent_directory(link_path, link_parent, sizeof(link_parent)) != 0 ||
        tool_canonicalize_path(link_parent, 0, 1, link_parent_absolute, sizeof(link_parent_absolute)) != 0) {
        return -1;
    }

    buffer[0] = '\0';
    target_cursor = skip_path_separators(target_absolute);
    link_cursor = skip_path_separators(link_parent_absolute);

    while (*target_cursor != '\0' && *link_cursor != '\0') {
        size_t target_length = path_component_length(target_cursor);
        size_t link_length = path_component_length(link_cursor);

        if (!path_components_match(target_cursor, target_length, link_cursor, link_length)) {
            break;
        }

        target_cursor += target_length;
        link_cursor += link_length;
        target_cursor = skip_path_separators(target_cursor);
        link_cursor = skip_path_separators(link_cursor);
    }

    while (*link_cursor != '\0') {
        size_t link_length = path_component_length(link_cursor);

        if (link_length != 0U && append_fragment(buffer, buffer_size, &length, "../") != 0) {
            return -1;
        }

        link_cursor += link_length;
        link_cursor = skip_path_separators(link_cursor);
    }

    while (*target_cursor != '\0') {
        size_t target_length = path_component_length(target_cursor);

        if (append_path_component(buffer, buffer_size, &length, target_cursor, target_length) != 0) {
            return -1;
        }

        target_cursor += target_length;
        target_cursor = skip_path_separators(target_cursor);
    }

    if (length > 1U && buffer[length - 1U] == '/') {
        buffer[length - 1U] = '\0';
        length -= 1U;
    }

    if (length == 0U) {
        if (buffer_size < 2U) {
            return -1;
        }
        buffer[0] = '.';
        buffer[1] = '\0';
    }

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
    char relative_target[LN_PATH_CAPACITY];
    const char *link_target = target_path;

    if (resolve_link_path(target_path, dest_path, options, link_path, sizeof(link_path)) != 0) {
        rt_write_line(2, "ln: destination path too long");
        return 1;
    }

    if (options->relative && options->symbolic) {
        if (build_relative_target_path(target_path, link_path, relative_target, sizeof(relative_target)) != 0) {
            rt_write_line(2, "ln: failed to compute relative target path");
            return 1;
        }
        link_target = relative_target;
    }

    if (prepare_link_path(link_path, options) != 0) {
        return 1;
    }

    if (options->symbolic) {
        if (platform_create_symbolic_link(link_target, link_path) != 0) {
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
        print_verbose_link(link_target, link_path, options);
    }

    return 0;
}

int main(int argc, char **argv) {
    LnOptions options;
    int argi = 1;
    int operand_count;
    int source_count;
    int i;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        if (rt_strcmp(argv[argi], "--symbolic") == 0) {
            options.symbolic = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--force") == 0) {
            options.force = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--no-dereference") == 0) {
            options.no_dereference = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--no-target-directory") == 0) {
            options.no_target_directory = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--relative") == 0) {
            options.relative = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--verbose") == 0) {
            options.verbose = 1;
            argi += 1;
            continue;
        }

        {
            const char *flag = argv[argi] + 1;

            while (*flag != '\0') {
                if (*flag == 's') {
                    options.symbolic = 1;
                } else if (*flag == 'f') {
                    options.force = 1;
                } else if (*flag == 'n') {
                    options.no_dereference = 1;
                } else if (*flag == 'T') {
                    options.no_target_directory = 1;
                } else if (*flag == 'r') {
                    options.relative = 1;
                } else if (*flag == 'v') {
                    options.verbose = 1;
                } else {
                    print_usage(argv[0]);
                    return 1;
                }
                flag += 1;
            }
        }

        argi += 1;
    }

    if (options.relative && !options.symbolic) {
        rt_write_line(2, "ln: --relative requires symbolic links");
        return 1;
    }

    operand_count = argc - argi;
    if (operand_count < 1) {
        print_usage(argv[0]);
        return 1;
    }

    if (operand_count == 1) {
        return create_one_link(argv[argi], ".", &options);
    }

    source_count = operand_count - 1;
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
