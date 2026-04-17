#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int human_readable;
    int inode_mode;
    int show_type;
} DfOptions;

static int print_one(const char *path, const DfOptions *options) {
    PlatformFilesystemInfo info;

    if (platform_get_filesystem_info(path, &info) != 0) {
        rt_write_cstr(2, "df: failed to inspect ");
        rt_write_line(2, path);
        return 1;
    }

    rt_write_cstr(1, path);
    rt_write_char(1, '\t');

    if (options->show_type) {
        rt_write_cstr(1, info.type_name[0] != '\0' ? info.type_name : "-");
        rt_write_char(1, '\t');
    }

    if (options->inode_mode) {
        unsigned long long used_inodes = (info.total_inodes >= info.free_inodes) ? (info.total_inodes - info.free_inodes) : 0ULL;
        unsigned long long use_percent = (info.total_inodes == 0ULL) ? 0ULL : (used_inodes * 100ULL) / info.total_inodes;

        rt_write_uint(1, info.total_inodes);
        rt_write_char(1, '\t');
        rt_write_uint(1, used_inodes);
        rt_write_char(1, '\t');
        rt_write_uint(1, info.available_inodes);
        rt_write_char(1, '\t');
        rt_write_uint(1, use_percent);
        rt_write_char(1, '%');
        rt_write_char(1, '\t');
    } else {
        unsigned long long used = (info.total_bytes >= info.free_bytes) ? (info.total_bytes - info.free_bytes) : 0ULL;
        unsigned long long use_percent = (info.total_bytes == 0ULL) ? 0ULL : (used * 100ULL) / info.total_bytes;
        char total_text[32];
        char used_text[32];
        char available_text[32];

        tool_format_size(info.total_bytes, options->human_readable, total_text, sizeof(total_text));
        tool_format_size(used, options->human_readable, used_text, sizeof(used_text));
        tool_format_size(info.available_bytes, options->human_readable, available_text, sizeof(available_text));

        rt_write_cstr(1, total_text);
        rt_write_char(1, '\t');
        rt_write_cstr(1, used_text);
        rt_write_char(1, '\t');
        rt_write_cstr(1, available_text);
        rt_write_char(1, '\t');
        rt_write_uint(1, use_percent);
        rt_write_char(1, '%');
        rt_write_char(1, '\t');
    }

    rt_write_line(1, path);
    return 0;
}

int main(int argc, char **argv) {
    DfOptions options;
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
            if (*flag == 'h') {
                options.human_readable = 1;
            } else if (*flag == 'i') {
                options.inode_mode = 1;
            } else if (*flag == 'T') {
                options.show_type = 1;
            } else {
                rt_write_line(2, "Usage: df [-h] [-i] [-T] [path ...]");
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (options.inode_mode) {
        if (options.show_type) {
            rt_write_line(1, "Filesystem\tType\tInodes\tIUsed\tIFree\tIUse%\tMounted on");
        } else {
            rt_write_line(1, "Filesystem\tInodes\tIUsed\tIFree\tIUse%\tMounted on");
        }
    } else if (options.show_type) {
        rt_write_line(1, "Filesystem\tType\tSize\tUsed\tAvailable\tUse%\tMounted on");
    } else {
        rt_write_line(1, "Filesystem\tSize\tUsed\tAvailable\tUse%\tMounted on");
    }

    if (argi >= argc) {
        return print_one("/", &options);
    }

    for (i = argi; i < argc; ++i) {
        if (print_one(argv[i], &options) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
