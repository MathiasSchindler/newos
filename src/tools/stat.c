#include "platform.h"
#include "runtime.h"

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

static int print_one(const char *path) {
    PlatformDirEntry entry;
    char mode_text[11];
    char target[1024];

    if (platform_get_path_info(path, &entry) != 0) {
        rt_write_cstr(2, "stat: cannot read ");
        rt_write_line(2, path);
        return 1;
    }

    platform_format_mode(entry.mode, mode_text);
    rt_write_cstr(1, "Path: ");
    rt_write_line(1, path);
    rt_write_cstr(1, "Type: ");
    rt_write_line(1, detect_type(entry.mode));
    rt_write_cstr(1, "Mode: ");
    rt_write_cstr(1, mode_text);
    rt_write_cstr(1, " (");
    rt_write_uint(1, (unsigned long long)(entry.mode & 0777U));
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
    rt_write_cstr(1, "Modified: ");
    rt_write_int(1, entry.mtime);
    rt_write_char(1, '\n');

    if (platform_read_symlink(path, target, sizeof(target)) == 0) {
        rt_write_cstr(1, "Target: ");
        rt_write_line(1, target);
    }

    return 0;
}

int main(int argc, char **argv) {
    int i;
    int exit_code = 0;

    if (argc < 2) {
        rt_write_line(2, "Usage: stat PATH...");
        return 1;
    }

    for (i = 1; i < argc; ++i) {
        if (print_one(argv[i]) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
