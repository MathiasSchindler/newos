#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define CHMOD_ENTRY_CAPACITY 1024
#define CHMOD_PATH_CAPACITY 1024

typedef struct {
    int recursive;
    int mode_is_numeric;
    unsigned int numeric_mode;
    const char *mode_spec;
} ChmodOptions;

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

    *mode_out = value;
    return 0;
}

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-R] MODE path ...");
}

static unsigned int permission_mask_for_who(unsigned int who, char permission, unsigned int current_mode, int is_directory) {
    unsigned int mask = 0U;
    int allow_exec = is_directory || (current_mode & 0111U) != 0U;

    if (permission == 'r') {
        if ((who & 1U) != 0U) mask |= 0400U;
        if ((who & 2U) != 0U) mask |= 0040U;
        if ((who & 4U) != 0U) mask |= 0004U;
    } else if (permission == 'w') {
        if ((who & 1U) != 0U) mask |= 0200U;
        if ((who & 2U) != 0U) mask |= 0020U;
        if ((who & 4U) != 0U) mask |= 0002U;
    } else if (permission == 'x' || (permission == 'X' && allow_exec)) {
        if ((who & 1U) != 0U) mask |= 0100U;
        if ((who & 2U) != 0U) mask |= 0010U;
        if ((who & 4U) != 0U) mask |= 0001U;
    } else if (permission == 's') {
        if ((who & 1U) != 0U) mask |= 04000U;
        if ((who & 2U) != 0U) mask |= 02000U;
    } else if (permission == 't') {
        mask |= 01000U;
    }

    return mask;
}

static int apply_symbolic_mode(const char *text, unsigned int current_mode, int is_directory, unsigned int *mode_out) {
    unsigned int result = current_mode & 07777U;
    size_t i = 0U;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[i] != '\0') {
        unsigned int who = 0U;
        unsigned int set_mask = 0U;
        unsigned int clear_mask = 0U;
        char op;
        int saw_who = 0;

        while (text[i] == 'u' || text[i] == 'g' || text[i] == 'o' || text[i] == 'a') {
            saw_who = 1;
            if (text[i] == 'u') {
                who |= 1U;
            } else if (text[i] == 'g') {
                who |= 2U;
            } else if (text[i] == 'o') {
                who |= 4U;
            } else {
                who |= 7U;
            }
            i += 1U;
        }

        if (!saw_who) {
            who = 7U;
        }

        op = text[i];
        if (op != '+' && op != '-' && op != '=') {
            return -1;
        }
        i += 1U;

        while (text[i] != '\0' && text[i] != ',') {
            unsigned int mask = permission_mask_for_who(who, text[i], result, is_directory);

            if (mask == 0U && text[i] != 'X') {
                return -1;
            }

            set_mask |= mask;
            i += 1U;
        }

        if ((who & 1U) != 0U) {
            clear_mask |= 0700U | 04000U;
        }
        if ((who & 2U) != 0U) {
            clear_mask |= 0070U | 02000U;
        }
        if ((who & 4U) != 0U) {
            clear_mask |= 0007U;
        }
        if (who == 7U) {
            clear_mask |= 01000U;
        }

        if (op == '+') {
            result |= set_mask;
        } else if (op == '-') {
            result &= ~set_mask;
        } else {
            result = (result & ~clear_mask) | set_mask;
        }

        if (text[i] == ',') {
            i += 1U;
        }
    }

    *mode_out = result & 07777U;
    return 0;
}

static int compute_target_mode(const ChmodOptions *options, const PlatformDirEntry *entry, unsigned int *mode_out) {
    if (options->mode_is_numeric) {
        *mode_out = options->numeric_mode;
        return 0;
    }
    return apply_symbolic_mode(options->mode_spec, entry->mode, entry->is_dir, mode_out);
}

static int chmod_one_path(const char *path, const ChmodOptions *options) {
    PlatformDirEntry entry;
    unsigned int new_mode = 0U;
    PlatformDirEntry entries[CHMOD_ENTRY_CAPACITY];
    size_t count = 0U;
    int is_directory = 0;

    if (platform_get_path_info(path, &entry) != 0) {
        return -1;
    }

    if (options->recursive && entry.is_dir) {
        if (platform_collect_entries(path, 1, entries, CHMOD_ENTRY_CAPACITY, &count, &is_directory) != 0 || !is_directory) {
            return -1;
        }
    }

    if (compute_target_mode(options, &entry, &new_mode) != 0) {
        if (options->recursive && entry.is_dir) {
            platform_free_entries(entries, count);
        }
        return -2;
    }

    if (platform_change_mode(path, new_mode) != 0) {
        if (options->recursive && entry.is_dir) {
            platform_free_entries(entries, count);
        }
        return -1;
    }

    if (options->recursive && entry.is_dir) {
        size_t i;

        for (i = 0U; i < count; ++i) {
            char child_path[CHMOD_PATH_CAPACITY];

            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
                continue;
            }

            if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0 ||
                chmod_one_path(child_path, options) != 0) {
                platform_free_entries(entries, count);
                return -1;
            }
        }

        platform_free_entries(entries, count);
    }

    return 0;
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
        if (rt_strcmp(argv[argi], "-R") == 0) {
            options.recursive = 1;
        } else {
            print_usage(argv[0]);
            return 1;
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
        int result = chmod_one_path(argv[i], &options);

        if (result == -2) {
            rt_write_line(2, "chmod: invalid mode");
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
