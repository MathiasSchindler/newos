#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MV_PATH_CAPACITY 1024

typedef struct {
    int interactive;
    int no_clobber;
} MvOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-i] [-f] [-n] source... destination");
}

static int source_is_directory(const char *path) {
    int is_directory = 0;
    return platform_path_is_directory(path, &is_directory) == 0 && is_directory;
}

static int path_exists(const char *path) {
    PlatformDirEntry entry;
    return platform_get_path_info(path, &entry) == 0;
}

static int prompt_yes_no(const char *message, const char *path) {
    char reply[8];
    long bytes_read;

    rt_write_cstr(2, message);
    rt_write_cstr(2, path);
    rt_write_cstr(2, "? ");

    bytes_read = platform_read(0, reply, sizeof(reply));
    return bytes_read > 0 && (reply[0] == 'y' || reply[0] == 'Y');
}

static int should_replace(const char *target_path, const MvOptions *options) {
    if (!path_exists(target_path)) {
        return 1;
    }
    if (options->no_clobber) {
        return 0;
    }
    if (options->interactive) {
        return prompt_yes_no("mv: overwrite ", target_path);
    }
    return 1;
}

static int move_one_path(const char *source_path, const char *dest_path, const MvOptions *options) {
    char target_path[MV_PATH_CAPACITY];

    if (tool_resolve_destination(source_path, dest_path, target_path, sizeof(target_path)) != 0) {
        rt_write_line(2, "mv: destination path too long");
        return 1;
    }

    if (rt_strcmp(source_path, target_path) == 0) {
        return 0;
    }

    if (!should_replace(target_path, options)) {
        return 0;
    }

    if (platform_rename_path(source_path, target_path) == 0) {
        return 0;
    }

    if (!source_is_directory(source_path) && tool_copy_file(source_path, target_path) == 0) {
        if (platform_remove_file(source_path) == 0) {
            return 0;
        }
        (void)platform_remove_file(target_path);
    }

    rt_write_cstr(2, "mv: failed to move ");
    rt_write_cstr(2, source_path);
    rt_write_cstr(2, " to ");
    rt_write_line(2, target_path);
    return 1;
}

int main(int argc, char **argv) {
    MvOptions options;
    int argi = 1;
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
            if (*flag == 'i') {
                options.interactive = 1;
                options.no_clobber = 0;
            } else if (*flag == 'f') {
                options.interactive = 0;
                options.no_clobber = 0;
            } else if (*flag == 'n') {
                options.no_clobber = 1;
                options.interactive = 0;
            } else {
                print_usage(argv[0]);
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (argc - argi < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc - argi > 2) {
        int dest_is_directory = 0;
        if (platform_path_is_directory(argv[argc - 1], &dest_is_directory) != 0 || !dest_is_directory) {
            rt_write_line(2, "mv: target for multiple sources must be an existing directory");
            return 1;
        }
    }

    for (i = argi; i < argc - 1; ++i) {
        if (move_one_path(argv[i], argv[argc - 1], &options) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
