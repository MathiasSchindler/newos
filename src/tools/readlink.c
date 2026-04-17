#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define READLINK_CAPACITY 1024

int main(int argc, char **argv) {
    char buffer[READLINK_CAPACITY];
    int argi = 1;
    int canonicalize = 0;
    int allow_missing = 0;
    int no_newline = 0;
    int zero_terminated = 0;
    int quiet = 0;
    int exit_code = 0;
    int i;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 'n') {
                no_newline = 1;
            } else if (*flag == 'f') {
                canonicalize = 1;
                allow_missing = 1;
            } else if (*flag == 'e') {
                canonicalize = 1;
                allow_missing = 0;
            } else if (*flag == 'm') {
                canonicalize = 1;
                allow_missing = 1;
            } else if (*flag == 'z') {
                zero_terminated = 1;
                no_newline = 0;
            } else if (*flag == 'q') {
                quiet = 1;
            } else {
                rt_write_line(2, "Usage: readlink [-n] [-f|-e|-m] [-q] [-z] PATH...");
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (argi >= argc) {
        rt_write_line(2, "Usage: readlink [-n] [-f|-e|-m] [-q] [-z] PATH...");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        int status;

        if (canonicalize) {
            status = tool_canonicalize_path(argv[i], 1, allow_missing, buffer, sizeof(buffer));
        } else {
            status = platform_read_symlink(argv[i], buffer, sizeof(buffer));
        }

        if (status != 0) {
            if (!quiet) {
                rt_write_cstr(2, "readlink: cannot read ");
                rt_write_line(2, argv[i]);
            }
            exit_code = 1;
            continue;
        }

        if (zero_terminated) {
            if (rt_write_all(1, buffer, rt_strlen(buffer)) != 0 || rt_write_char(1, '\0') != 0) {
                return 1;
            }
        } else if (no_newline) {
            if (rt_write_cstr(1, buffer) != 0) {
                return 1;
            }
        } else {
            if (rt_write_line(1, buffer) != 0) {
                return 1;
            }
        }
    }

    return exit_code;
}
