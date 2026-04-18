#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define STAT_PATH_CAPACITY 1024

typedef struct {
    int follow_symlinks;
    int filesystem;
    const char *format;
    int suppress_newline;
    int interpret_escapes;
} StatOptions;

static void print_usage(void) {
    rt_write_line(2, "Usage: stat [-L] [-f] [-c FORMAT] PATH...");
}

static const char *detect_type(unsigned int mode) {
    char formatted[11];
    platform_format_mode(mode, formatted);

    if (formatted[0] == 'd') return "directory";
    if (formatted[0] == 'l') return "symlink";
    if (formatted[0] == '-') return "file";
    if (formatted[0] == 'c') return "char-device";
    if (formatted[0] == 'b') return "block-device";
    if (formatted[0] == 'p') return "fifo";
    if (formatted[0] == 's') return "socket";
    return "other";
}

static void format_octal(unsigned long long value, char *buffer, size_t buffer_size) {
    char digits[32];
    size_t count = 0;
    size_t i;

    if (buffer == 0 || buffer_size == 0) {
        return;
    }

    if (value == 0ULL) {
        if (buffer_size > 1U) {
            buffer[0] = '0';
            buffer[1] = '\0';
        } else {
            buffer[0] = '\0';
        }
        return;
    }

    while (value > 0ULL && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value & 7ULL));
        value >>= 3U;
        count += 1U;
    }

    if (count + 1U > buffer_size) {
        count = buffer_size - 1U;
    }

    for (i = 0; i < count; ++i) {
        buffer[i] = digits[count - i - 1U];
    }
    buffer[count] = '\0';
}

static void write_quoted(const char *text) {
    rt_write_char(1, '\'');
    rt_write_cstr(1, text);
    rt_write_char(1, '\'');
}

static int write_with_optional_newline(const StatOptions *options) {
    if (options != 0 && options->suppress_newline) {
        return 0;
    }
    return rt_write_char(1, '\n');
}

static void format_hex(unsigned long long value, char *buffer, size_t buffer_size) {
    static const char digits[] = "0123456789abcdef";
    char scratch[32];
    size_t count = 0;
    size_t i;

    if (buffer == 0 || buffer_size == 0U) {
        return;
    }

    if (value == 0ULL) {
        if (buffer_size > 1U) {
            buffer[0] = '0';
            buffer[1] = '\0';
        } else {
            buffer[0] = '\0';
        }
        return;
    }

    while (value > 0ULL && count < sizeof(scratch)) {
        scratch[count++] = digits[value & 0xFULL];
        value >>= 4U;
    }

    if (count + 1U > buffer_size) {
        count = buffer_size - 1U;
    }

    for (i = 0; i < count; ++i) {
        buffer[i] = scratch[count - i - 1U];
    }
    buffer[count] = '\0';
}

static int write_numeric_identity(const char *name_or_id, int is_group) {
    unsigned int id_value = 0U;

    if (name_or_id == 0 || name_or_id[0] == '\0') {
        return rt_write_char(1, '0');
    }

    if (rt_is_digit_string(name_or_id)) {
        return rt_write_cstr(1, name_or_id);
    }

    if (is_group) {
        if (platform_lookup_group(name_or_id, &id_value) == 0) {
            return rt_write_uint(1, (unsigned long long)id_value);
        }
    } else {
        PlatformIdentity identity;
        if (platform_lookup_identity(name_or_id, &identity) == 0) {
            return rt_write_uint(1, (unsigned long long)identity.uid);
        }
    }

    return rt_write_cstr(1, name_or_id);
}

static int write_time_value(long long epoch_seconds) {
    char buffer[64];
    if (platform_format_time(epoch_seconds, 1, "%Y-%m-%d %H:%M:%S", buffer, sizeof(buffer)) != 0) {
        return rt_write_int(1, epoch_seconds);
    }
    return rt_write_cstr(1, buffer);
}

static int write_labeled_time(const char *label, long long epoch_seconds) {
    if (rt_write_cstr(1, label) != 0) {
        return 1;
    }
    if (write_time_value(epoch_seconds) != 0) {
        return 1;
    }
    if (rt_write_cstr(1, " (") != 0) {
        return 1;
    }
    if (rt_write_int(1, epoch_seconds) != 0) {
        return 1;
    }
    return rt_write_line(1, ")");
}

static int print_file_format(const StatOptions *options, const char *path, const PlatformDirEntry *entry, const char *target) {
    size_t i = 0;
    char mode_text[11];
    char octal_text[16];
    char mode_hex_text[16];
    char device_hex_text[32];
    unsigned long long block_count = (entry->size + 511ULL) / 512ULL;

    platform_format_mode(entry->mode, mode_text);
    format_octal((unsigned long long)(entry->mode & 07777U), octal_text, sizeof(octal_text));
    format_hex((unsigned long long)entry->mode, mode_hex_text, sizeof(mode_hex_text));
    format_hex(entry->device, device_hex_text, sizeof(device_hex_text));

    while (options->format[i] != '\0') {
        if (options->interpret_escapes && options->format[i] == '\\' && options->format[i + 1U] != '\0') {
            char ch = options->format[i + 1U];
            if (ch == 'n') {
                ch = '\n';
            } else if (ch == 't') {
                ch = '\t';
            } else if (ch == '0') {
                ch = '\0';
            }
            if (rt_write_char(1, ch) != 0) {
                return 1;
            }
            i += 2U;
            continue;
        }

        if (options->format[i] != '%') {
            if (rt_write_char(1, options->format[i]) != 0) {
                return 1;
            }
            i += 1U;
            continue;
        }

        i += 1U;
        if (options->format[i] == '\0') {
            break;
        }

        switch (options->format[i]) {
            case '%':
                if (rt_write_char(1, '%') != 0) {
                    return 1;
                }
                break;
            case 'n':
                if (rt_write_cstr(1, path) != 0) {
                    return 1;
                }
                break;
            case 'N':
                write_quoted(path);
                if (target != 0 && target[0] != '\0') {
                    if (rt_write_cstr(1, " -> ") != 0) {
                        return 1;
                    }
                    write_quoted(target);
                }
                break;
            case 's':
                if (rt_write_uint(1, entry->size) != 0) {
                    return 1;
                }
                break;
            case 'b':
                if (rt_write_uint(1, block_count) != 0) {
                    return 1;
                }
                break;
            case 'B':
                if (rt_write_uint(1, 512ULL) != 0) {
                    return 1;
                }
                break;
            case 'F':
                if (rt_write_cstr(1, detect_type(entry->mode)) != 0) {
                    return 1;
                }
                break;
            case 'a':
                if (rt_write_cstr(1, octal_text) != 0) {
                    return 1;
                }
                break;
            case 'A':
                if (rt_write_cstr(1, mode_text) != 0) {
                    return 1;
                }
                break;
            case 'd':
                if (rt_write_uint(1, entry->device) != 0) {
                    return 1;
                }
                break;
            case 'D':
                if (rt_write_cstr(1, device_hex_text) != 0) {
                    return 1;
                }
                break;
            case 'f':
                if (rt_write_cstr(1, mode_hex_text) != 0) {
                    return 1;
                }
                break;
            case 'h':
                if (rt_write_uint(1, (unsigned long long)entry->nlink) != 0) {
                    return 1;
                }
                break;
            case 'i':
                if (rt_write_uint(1, entry->inode) != 0) {
                    return 1;
                }
                break;
            case 'U':
                if (rt_write_cstr(1, entry->owner) != 0) {
                    return 1;
                }
                break;
            case 'u':
                if (write_numeric_identity(entry->owner, 0) != 0) {
                    return 1;
                }
                break;
            case 'G':
                if (rt_write_cstr(1, entry->group) != 0) {
                    return 1;
                }
                break;
            case 'g':
                if (write_numeric_identity(entry->group, 1) != 0) {
                    return 1;
                }
                break;
            case 'W':
                if (rt_write_char(1, '0') != 0) {
                    return 1;
                }
                break;
            case 'Y':
                if (rt_write_int(1, entry->mtime) != 0) {
                    return 1;
                }
                break;
            case 'X':
                if (rt_write_int(1, entry->atime) != 0) {
                    return 1;
                }
                break;
            case 'Z':
                if (rt_write_int(1, entry->ctime) != 0) {
                    return 1;
                }
                break;
            case 'w':
                if (rt_write_char(1, '-') != 0) {
                    return 1;
                }
                break;
            case 'x':
                if (write_time_value(entry->atime) != 0) {
                    return 1;
                }
                break;
            case 'y':
                if (write_time_value(entry->mtime) != 0) {
                    return 1;
                }
                break;
            case 'z':
                if (write_time_value(entry->ctime) != 0) {
                    return 1;
                }
                break;
            default:
                if (rt_write_char(1, '%') != 0 || rt_write_char(1, options->format[i]) != 0) {
                    return 1;
                }
                break;
        }

        i += 1U;
    }

    return write_with_optional_newline(options);
}

static int print_fs_format(const StatOptions *options, const char *path, const PlatformFilesystemInfo *info) {
    size_t i = 0;
    unsigned long long used = (info->total_bytes >= info->free_bytes) ? (info->total_bytes - info->free_bytes) : 0ULL;
    unsigned long long use_percent = (info->total_bytes == 0ULL) ? 0ULL : (used * 100ULL) / info->total_bytes;

    while (options->format[i] != '\0') {
        if (options->interpret_escapes && options->format[i] == '\\' && options->format[i + 1U] != '\0') {
            char ch = options->format[i + 1U];
            if (ch == 'n') {
                ch = '\n';
            } else if (ch == 't') {
                ch = '\t';
            } else if (ch == '0') {
                ch = '\0';
            }
            if (rt_write_char(1, ch) != 0) {
                return 1;
            }
            i += 2U;
            continue;
        }

        if (options->format[i] != '%') {
            if (rt_write_char(1, options->format[i]) != 0) {
                return 1;
            }
            i += 1U;
            continue;
        }

        i += 1U;
        if (options->format[i] == '\0') {
            break;
        }

        switch (options->format[i]) {
            case '%':
                if (rt_write_char(1, '%') != 0) {
                    return 1;
                }
                break;
            case 'n':
                if (rt_write_cstr(1, path) != 0) {
                    return 1;
                }
                break;
            case 'T':
                if (rt_write_cstr(1, info->type_name[0] != '\0' ? info->type_name : "-") != 0) {
                    return 1;
                }
                break;
            case 'b':
                if (rt_write_uint(1, info->total_bytes) != 0) {
                    return 1;
                }
                break;
            case 'f':
                if (rt_write_uint(1, info->free_bytes) != 0) {
                    return 1;
                }
                break;
            case 'a':
                if (rt_write_uint(1, info->available_bytes) != 0) {
                    return 1;
                }
                break;
            case 'u':
                if (rt_write_uint(1, used) != 0) {
                    return 1;
                }
                break;
            case 'c':
                if (rt_write_uint(1, info->total_inodes) != 0) {
                    return 1;
                }
                break;
            case 'd':
                if (rt_write_uint(1, info->free_inodes) != 0) {
                    return 1;
                }
                break;
            case 'S':
                if (rt_write_uint(1, 1ULL) != 0) {
                    return 1;
                }
                break;
            case 'p':
                if (rt_write_uint(1, use_percent) != 0 || rt_write_char(1, '%') != 0) {
                    return 1;
                }
                break;
            default:
                if (rt_write_char(1, '%') != 0 || rt_write_char(1, options->format[i]) != 0) {
                    return 1;
                }
                break;
        }

        i += 1U;
    }

    return write_with_optional_newline(options);
}

static int load_entry(const char *path, const StatOptions *options, PlatformDirEntry *entry_out) {
    char resolved[STAT_PATH_CAPACITY];
    const char *lookup_path = path;

    if (options->follow_symlinks) {
        if (tool_canonicalize_path(path, 1, 0, resolved, sizeof(resolved)) != 0) {
            return -1;
        }
        lookup_path = resolved;
    }

    return platform_get_path_info(lookup_path, entry_out);
}

static int print_file_report(const char *path, const StatOptions *options) {
    PlatformDirEntry entry;
    char mode_text[11];
    char target[STAT_PATH_CAPACITY];
    char octal_text[16];
    int has_target = 0;

    if (load_entry(path, options, &entry) != 0) {
        rt_write_cstr(2, "stat: cannot read ");
        rt_write_line(2, path);
        return 1;
    }

    if (!options->follow_symlinks && platform_read_symlink(path, target, sizeof(target)) == 0) {
        has_target = 1;
    } else {
        target[0] = '\0';
    }

    if (options->format != 0) {
        return print_file_format(options, path, &entry, has_target ? target : 0);
    }

    platform_format_mode(entry.mode, mode_text);
    format_octal((unsigned long long)(entry.mode & 07777U), octal_text, sizeof(octal_text));

    rt_write_cstr(1, "Path: ");
    rt_write_line(1, path);
    rt_write_cstr(1, "Type: ");
    rt_write_line(1, detect_type(entry.mode));
    rt_write_cstr(1, "Mode: ");
    rt_write_cstr(1, mode_text);
    rt_write_cstr(1, " (");
    rt_write_cstr(1, octal_text);
    rt_write_line(1, ")");
    rt_write_cstr(1, "Size: ");
    rt_write_uint(1, entry.size);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "Inode: ");
    rt_write_uint(1, entry.inode);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "Links: ");
    rt_write_uint(1, (unsigned long long)entry.nlink);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "Owner: ");
    rt_write_line(1, entry.owner);
    rt_write_cstr(1, "Group: ");
    rt_write_line(1, entry.group);
    if (write_labeled_time("Access: ", entry.atime) != 0 ||
        write_labeled_time("Modify: ", entry.mtime) != 0 ||
        write_labeled_time("Change: ", entry.ctime) != 0) {
        return 1;
    }

    if (has_target) {
        rt_write_cstr(1, "Target: ");
        rt_write_line(1, target);
    }

    return 0;
}

static int print_filesystem_report(const char *path, const StatOptions *options) {
    PlatformFilesystemInfo info;
    unsigned long long used;
    unsigned long long use_percent;

    if (platform_get_filesystem_info(path, &info) != 0) {
        rt_write_cstr(2, "stat: cannot read filesystem for ");
        rt_write_line(2, path);
        return 1;
    }

    if (options->format != 0) {
        return print_fs_format(options, path, &info);
    }

    used = (info.total_bytes >= info.free_bytes) ? (info.total_bytes - info.free_bytes) : 0ULL;
    use_percent = (info.total_bytes == 0ULL) ? 0ULL : (used * 100ULL) / info.total_bytes;

    rt_write_cstr(1, "Path: ");
    rt_write_line(1, path);
    rt_write_cstr(1, "Filesystem: ");
    rt_write_line(1, info.type_name[0] != '\0' ? info.type_name : "unknown");
    rt_write_cstr(1, "Total: ");
    rt_write_uint(1, info.total_bytes);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "Used: ");
    rt_write_uint(1, used);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "Available: ");
    rt_write_uint(1, info.available_bytes);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "Use%: ");
    rt_write_uint(1, use_percent);
    rt_write_line(1, "%");
    rt_write_cstr(1, "Inodes: ");
    rt_write_uint(1, info.total_inodes);
    rt_write_char(1, '\n');
    return 0;
}

int main(int argc, char **argv) {
    StatOptions options;
    int i;
    int argi = 1;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        if (rt_strcmp(argv[argi], "--dereference") == 0) {
            options.follow_symlinks = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--file-system") == 0) {
            options.filesystem = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--format") == 0 || rt_strcmp(argv[argi], "--printf") == 0) {
            if (argi + 1 >= argc) {
                print_usage();
                return 1;
            }
            options.format = argv[argi + 1];
            options.suppress_newline = (rt_strcmp(argv[argi], "--printf") == 0);
            options.interpret_escapes = 1;
            argi += 2;
            continue;
        }
        if (argv[argi][0] == '-' && argv[argi][1] == '-' &&
            argv[argi][2] == 'f' && argv[argi][3] == 'o' &&
            argv[argi][4] == 'r' && argv[argi][5] == 'm' &&
            argv[argi][6] == 'a' && argv[argi][7] == 't' &&
            argv[argi][8] == '=') {
            options.format = argv[argi] + 9;
            options.suppress_newline = 0;
            options.interpret_escapes = 1;
            argi += 1;
            continue;
        }
        if (argv[argi][0] == '-' && argv[argi][1] == '-' &&
            argv[argi][2] == 'p' && argv[argi][3] == 'r' &&
            argv[argi][4] == 'i' && argv[argi][5] == 'n' &&
            argv[argi][6] == 't' && argv[argi][7] == 'f' &&
            argv[argi][8] == '=') {
            options.format = argv[argi] + 9;
            options.suppress_newline = 1;
            options.interpret_escapes = 1;
            argi += 1;
            continue;
        }

        while (*flag != '\0') {
            if (*flag == 'L') {
                options.follow_symlinks = 1;
                flag += 1;
            } else if (*flag == 'f') {
                options.filesystem = 1;
                flag += 1;
            } else if (*flag == 'c') {
                if (flag[1] != '\0') {
                    options.format = flag + 1;
                    flag += rt_strlen(flag);
                } else {
                    if (argi + 1 >= argc) {
                        print_usage();
                        return 1;
                    }
                    options.format = argv[argi + 1];
                    argi += 1;
                    flag += 1;
                }
                options.suppress_newline = 0;
                options.interpret_escapes = 1;
            } else {
                print_usage();
                return 1;
            }
        }

        argi += 1;
    }

    if (argi >= argc) {
        print_usage();
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        if (options.filesystem) {
            if (print_filesystem_report(argv[i], &options) != 0) {
                exit_code = 1;
            }
        } else {
            if (print_file_report(argv[i], &options) != 0) {
                exit_code = 1;
            }
        }
    }

    return exit_code;
}
