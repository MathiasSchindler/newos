#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define CHMOD_ENTRY_CAPACITY 1024
#define CHMOD_PATH_CAPACITY 1024

typedef struct {
    int recursive;
    int no_dereference;
    int symlink_traversal;
    int mode_is_numeric;
    unsigned int numeric_mode;
    const char *mode_spec;
} ChmodOptions;

#define CHMOD_SYMLINKS_NONE 0
#define CHMOD_SYMLINKS_COMMAND_LINE 1
#define CHMOD_SYMLINKS_ALL 2

static int parse_octal_mode(const char *text, unsigned int *mode_out) {
    unsigned int value = 0;
    int i = 0;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[i] != '\0') {
        if (text[i] < '0' || text[i] > '7') {
            return -1;
        }
        value = (value * 8U) + (unsigned int)(text[i] - '0');
        i += 1;
    }

    if (value > 07777U) {
        return -1;
    }

    *mode_out = value;
    return 0;
}

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-R] [-h] [-H|-L|-P] MODE path ...");
}

static int compute_target_mode(const ChmodOptions *options, const PlatformDirEntry *entry, unsigned int *mode_out) {
    if (options->mode_is_numeric) {
        *mode_out = options->numeric_mode;
        return 0;
    }
    return tool_apply_symbolic_mode(options->mode_spec,
                                    entry->mode,
                                    TOOL_SYMBOLIC_MODE_ALLOW_COPY |
                                        TOOL_SYMBOLIC_MODE_REQUIRE_PERMISSION |
                                        (entry->is_dir ? TOOL_SYMBOLIC_MODE_DIRECTORY : 0U),
                                    mode_out);
}

static int path_is_symlink(const char *path) {
    char target[2];
    return platform_read_symlink(path, target, sizeof(target)) == 0;
}

static int should_recurse_path(const ChmodOptions *options, int command_line_path, int is_symlink, const PlatformDirEntry *entry, const PlatformDirEntry *effective_entry) {
    if (entry->is_dir) {
        return 1;
    }
    if (!is_symlink || effective_entry->is_dir == 0 || options->no_dereference) {
        return 0;
    }
    if (options->symlink_traversal == CHMOD_SYMLINKS_ALL) {
        return 1;
    }
    if (options->symlink_traversal == CHMOD_SYMLINKS_COMMAND_LINE && command_line_path) {
        return 1;
    }
    return 0;
}

static int chmod_one_path(const char *path, const ChmodOptions *options, int command_line_path) {
    PlatformDirEntry entry;
    PlatformDirEntry effective_entry;
    unsigned int new_mode = 0U;
    PlatformDirEntry entries[CHMOD_ENTRY_CAPACITY];
    size_t count = 0U;
    int is_directory = 0;
    int is_symlink = 0;
    int skip_mode_change = 0;

    if (platform_get_path_info(path, &entry) != 0) {
        return -1;
    }

    is_symlink = path_is_symlink(path);
    effective_entry = entry;

    if (is_symlink) {
        if (options->no_dereference) {
            skip_mode_change = 1;
        } else if (platform_get_path_info_follow(path, &effective_entry) != 0) {
            return -1;
        }
    }

    if (options->recursive && should_recurse_path(options, command_line_path, is_symlink, &entry, &effective_entry)) {
        if (platform_collect_entries(path, 1, entries, CHMOD_ENTRY_CAPACITY, &count, &is_directory) != 0 || !is_directory) {
            return -1;
        }
    }

    if (compute_target_mode(options, &effective_entry, &new_mode) != 0) {
        if (options->recursive && count != 0U) {
            platform_free_entries(entries, count);
        }
        return -2;
    }

    if (!skip_mode_change && platform_change_mode(path, new_mode) != 0) {
        if (options->recursive && count != 0U) {
            platform_free_entries(entries, count);
        }
        return -1;
    }

    if (options->recursive && count != 0U) {
        size_t i;

        for (i = 0U; i < count; ++i) {
            char child_path[CHMOD_PATH_CAPACITY];

            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
                continue;
            }

            if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0 ||
                chmod_one_path(child_path, options, 0) != 0) {
                platform_free_entries(entries, count);
                return -1;
            }
        }

        platform_free_entries(entries, count);
    }

    return 0;
}

static void print_invalid_mode(const char *mode_spec) {
    rt_write_cstr(2, "chmod: invalid mode '");
    rt_write_cstr(2, mode_spec);
    rt_write_line(2, "'");
}

int main(int argc, char **argv) {
    ChmodOptions options;
    int argi = 1;
    int i;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        if (rt_strcmp(argv[argi], "--recursive") == 0) {
            options.recursive = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--no-dereference") == 0) {
            options.no_dereference = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--dereference") == 0) {
            options.no_dereference = 0;
            argi += 1;
            continue;
        }

        {
            const char *flag = argv[argi] + 1;

            while (*flag != '\0') {
                if (*flag == 'R') {
                    options.recursive = 1;
                } else if (*flag == 'h') {
                    options.no_dereference = 1;
                } else if (*flag == 'H') {
                    options.symlink_traversal = CHMOD_SYMLINKS_COMMAND_LINE;
                } else if (*flag == 'L') {
                    options.symlink_traversal = CHMOD_SYMLINKS_ALL;
                } else if (*flag == 'P') {
                    options.symlink_traversal = CHMOD_SYMLINKS_NONE;
                } else {
                    print_usage(argv[0]);
                    return 1;
                }
                flag += 1;
            }
        }

        argi += 1;
    }

    if (argc - argi < 2) {
        print_usage(argv[0]);
        return 1;
    }

    options.mode_spec = argv[argi];
    if (parse_octal_mode(argv[argi], &options.numeric_mode) == 0) {
        options.mode_is_numeric = 1;
    }

    for (i = argi + 1; i < argc; ++i) {
        int result = chmod_one_path(argv[i], &options, 1);

        if (result == -2) {
            print_invalid_mode(options.mode_spec);
            return 1;
        }
        if (result != 0) {
            rt_write_cstr(2, "chmod: cannot change mode for ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
