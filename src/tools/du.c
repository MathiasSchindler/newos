#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define DU_ENTRY_CAPACITY 1024
#define DU_PATH_CAPACITY 1024

typedef struct {
    int summarize;
    int human_readable;
    int all_entries;
    int grand_total;
    int follow_symlinks;
    int max_depth_set;
    unsigned long long max_depth;
    unsigned long long block_size;
} DuOptions;

static int parse_block_size_text(const char *text, unsigned long long *size_out) {
    char digits[32];
    size_t index = 0;
    unsigned long long value = 0ULL;
    unsigned long long multiplier = 1ULL;
    char suffix;

    if (text == 0 || text[0] == '\0' || size_out == 0) {
        return -1;
    }

    while (text[index] >= '0' && text[index] <= '9' && index + 1U < sizeof(digits)) {
        digits[index] = text[index];
        index += 1U;
    }
    digits[index] = '\0';
    if (index == 0U || rt_parse_uint(digits, &value) != 0) {
        return -1;
    }

    suffix = text[index];
    if (suffix != '\0') {
        if (text[index + 1U] != '\0' &&
            !((text[index + 1U] == 'B' || text[index + 1U] == 'b') && text[index + 2U] == '\0')) {
            return -1;
        }
        if (suffix == 'K' || suffix == 'k') {
            multiplier = 1024ULL;
        } else if (suffix == 'M' || suffix == 'm') {
            multiplier = 1024ULL * 1024ULL;
        } else if (suffix == 'G' || suffix == 'g') {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        } else if (suffix == 'T' || suffix == 't') {
            multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        } else if (suffix == 'P' || suffix == 'p') {
            multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        } else if (suffix != 'B' && suffix != 'b') {
            return -1;
        }
    }

    *size_out = value * multiplier;
    return *size_out == 0ULL ? -1 : 0;
}

static void format_total_text(unsigned long long total, const DuOptions *options, char *size_text, size_t size_text_size) {
    if (options->human_readable) {
        tool_format_size(total, 1, size_text, size_text_size);
        return;
    }

    if (options->block_size > 1ULL) {
        unsigned long long scaled = (total == 0ULL) ? 0ULL : ((total + options->block_size - 1ULL) / options->block_size);
        rt_unsigned_to_string(scaled, size_text, size_text_size);
        return;
    }

    rt_unsigned_to_string(total, size_text, size_text_size);
}

static void print_total(unsigned long long total, const char *path, const DuOptions *options) {
    char size_text[32];

    format_total_text(total, options, size_text, sizeof(size_text));
    rt_write_cstr(1, size_text);
    rt_write_char(1, '\t');
    rt_write_line(1, path);
}

static int within_depth(const DuOptions *options, unsigned long long depth) {
    if (options->summarize) {
        return depth == 0ULL;
    }

    if (!options->max_depth_set) {
        return 1;
    }

    return depth <= options->max_depth;
}

static unsigned long long du_path(const char *path, const DuOptions *options, unsigned long long depth, int *ok_out) {
    PlatformDirEntry entries[DU_ENTRY_CAPACITY];
    PlatformDirEntry current;
    char lookup_path[DU_PATH_CAPACITY];
    const char *scan_path = path;
    size_t count = 0;
    int is_directory = 0;
    unsigned long long total = 0ULL;
    size_t i;

    *ok_out = 0;
    if (options->follow_symlinks && tool_canonicalize_path(path, 1, 0, lookup_path, sizeof(lookup_path)) == 0) {
        scan_path = lookup_path;
    }

    if ((options->follow_symlinks ? platform_get_path_info_follow(scan_path, &current)
                                  : platform_get_path_info(scan_path, &current)) != 0) {
        return 0ULL;
    }

    if (!current.is_dir) {
        *ok_out = 1;
        if ((depth == 0ULL || options->all_entries) && within_depth(options, depth)) {
            print_total(current.size, path, options);
        }
        return current.size;
    }

    if (platform_collect_entries(scan_path, 1, entries, DU_ENTRY_CAPACITY, &count, &is_directory) != 0 || !is_directory) {
        return 0ULL;
    }

    total = current.size;

    for (i = 0; i < count; ++i) {
        char child_path[DU_PATH_CAPACITY];
        int child_ok = 0;

        if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
            continue;
        }

        if (tool_join_path(scan_path, entries[i].name, child_path, sizeof(child_path)) != 0) {
            platform_free_entries(entries, count);
            return 0ULL;
        }

        total += du_path(child_path, options, depth + 1ULL, &child_ok);
        if (!child_ok) {
            platform_free_entries(entries, count);
            return 0ULL;
        }
    }

    if (within_depth(options, depth)) {
        print_total(total, path, options);
    }

    platform_free_entries(entries, count);
    *ok_out = 1;
    return total;
}

static int parse_depth_text(const char *text, DuOptions *options) {
    unsigned long long depth;

    if (tool_parse_uint_arg(text, &depth, "du", "max depth") != 0) {
        return -1;
    }

    options->max_depth = depth;
    options->max_depth_set = 1;
    return 0;
}

int main(int argc, char **argv) {
    DuOptions options;
    int exit_code = 0;
    int argi = 1;
    int i;
    unsigned long long grand_total = 0ULL;

    rt_memset(&options, 0, sizeof(options));
    options.block_size = 1ULL;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *arg = argv[argi];
        const char *flag;

        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }

        if (arg[1] == '-' && arg[2] != '\0') {
            if (rt_strcmp(arg, "--summarize") == 0) {
                options.summarize = 1;
                argi += 1;
                continue;
            }
            if (rt_strcmp(arg, "--all") == 0) {
                options.all_entries = 1;
                argi += 1;
                continue;
            }
            if (rt_strcmp(arg, "--total") == 0) {
                options.grand_total = 1;
                argi += 1;
                continue;
            }
            if (rt_strcmp(arg, "--human-readable") == 0) {
                options.human_readable = 1;
                argi += 1;
                continue;
            }
            if (rt_strcmp(arg, "--bytes") == 0) {
                options.block_size = 1ULL;
                argi += 1;
                continue;
            }
            if (rt_strcmp(arg, "--dereference") == 0) {
                options.follow_symlinks = 1;
                argi += 1;
                continue;
            }
            if (rt_strcmp(arg, "--no-dereference") == 0) {
                options.follow_symlinks = 0;
                argi += 1;
                continue;
            }
            if (rt_strcmp(arg, "--block-size") == 0 && argi + 1 < argc) {
                if (parse_block_size_text(argv[argi + 1], &options.block_size) != 0) {
                    rt_write_line(2, "Usage: du [-a] [-b] [-c] [-h] [-k] [-L|-P] [-B SIZE] [-d N] [path ...]");
                    return 1;
                }
                argi += 2;
                continue;
            }
            if (arg[2] == 'b' &&
                arg[3] == 'l' &&
                arg[4] == 'o' &&
                arg[5] == 'c' &&
                arg[6] == 'k' &&
                arg[7] == '-' &&
                arg[8] == 's' &&
                arg[9] == 'i' &&
                arg[10] == 'z' &&
                arg[11] == 'e' &&
                arg[12] == '=') {
                if (parse_block_size_text(arg + 13, &options.block_size) != 0) {
                    rt_write_line(2, "Usage: du [-a] [-b] [-c] [-h] [-k] [-L|-P] [-B SIZE] [-d N] [path ...]");
                    return 1;
                }
                argi += 1;
                continue;
            }
            if (arg[2] == 'm' &&
                arg[3] == 'a' &&
                arg[4] == 'x' &&
                arg[5] == '-' &&
                arg[6] == 'd' &&
                arg[7] == 'e' &&
                arg[8] == 'p' &&
                arg[9] == 't' &&
                arg[10] == 'h') {
                if (arg[11] == '=') {
                    if (parse_depth_text(arg + 12, &options) != 0) {
                        return 1;
                    }
                    argi += 1;
                    continue;
                }
                if (arg[11] == '\0' && argi + 1 < argc) {
                    if (parse_depth_text(argv[argi + 1], &options) != 0) {
                        return 1;
                    }
                    argi += 2;
                    continue;
                }
            }

            rt_write_line(2, "Usage: du [-a] [-b] [-c] [-h] [-k] [-L|-P] [-B SIZE] [-d N] [path ...]");
            return 1;
        }

        flag = arg + 1;
        while (*flag != '\0') {
            if (*flag == 's') {
                options.summarize = 1;
                flag += 1;
            } else if (*flag == 'h') {
                options.human_readable = 1;
                flag += 1;
            } else if (*flag == 'a') {
                options.all_entries = 1;
                flag += 1;
            } else if (*flag == 'c') {
                options.grand_total = 1;
                flag += 1;
            } else if (*flag == 'b') {
                options.block_size = 1ULL;
                flag += 1;
            } else if (*flag == 'k') {
                options.block_size = 1024ULL;
                flag += 1;
            } else if (*flag == 'm') {
                options.block_size = 1024ULL * 1024ULL;
                flag += 1;
            } else if (*flag == 'L') {
                options.follow_symlinks = 1;
                flag += 1;
            } else if (*flag == 'P') {
                options.follow_symlinks = 0;
                flag += 1;
            } else if (*flag == 'B') {
                if (flag[1] != '\0') {
                    if (parse_block_size_text(flag + 1, &options.block_size) != 0) {
                        return 1;
                    }
                    flag += rt_strlen(flag);
                } else {
                    if (argi + 1 >= argc || parse_block_size_text(argv[argi + 1], &options.block_size) != 0) {
                        rt_write_line(2, "Usage: du [-a] [-b] [-c] [-h] [-k] [-L|-P] [-B SIZE] [-d N] [path ...]");
                        return 1;
                    }
                    argi += 1;
                    flag += 1;
                }
            } else if (*flag == 'd') {
                if (flag[1] != '\0') {
                    if (parse_depth_text(flag + 1, &options) != 0) {
                        return 1;
                    }
                    flag += rt_strlen(flag);
                } else {
                    if (argi + 1 >= argc || parse_depth_text(argv[argi + 1], &options) != 0) {
                        rt_write_line(2, "Usage: du [-a] [-b] [-c] [-h] [-k] [-L|-P] [-B SIZE] [-d N] [path ...]");
                        return 1;
                    }
                    argi += 1;
                    flag += 1;
                }
            } else {
                rt_write_line(2, "Usage: du [-a] [-b] [-c] [-h] [-k] [-L|-P] [-B SIZE] [-d N] [path ...]");
                return 1;
            }
        }

        argi += 1;
    }

    if (argi >= argc) {
        int ok = 0;
        unsigned long long total = du_path(".", &options, 0ULL, &ok);
        if (!ok) {
            rt_write_line(2, "du: failed to inspect .");
            return 1;
        }
        if (options.grand_total) {
            print_total(total, "total", &options);
        }
        return 0;
    }

    for (i = argi; i < argc; ++i) {
        int ok = 0;
        unsigned long long total = du_path(argv[i], &options, 0ULL, &ok);
        if (!ok) {
            rt_write_cstr(2, "du: failed to inspect ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }
        grand_total += total;
    }

    if (options.grand_total && argc - argi > 0) {
        print_total(grand_total, "total", &options);
    }

    return exit_code;
}
