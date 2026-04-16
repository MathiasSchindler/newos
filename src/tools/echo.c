#include "runtime.h"

int main(int argc, char **argv) {
    int i = 1;
    int trailing_newline = 1;

    if (argc > 1 && rt_strcmp(argv[1], "-n") == 0) {
        trailing_newline = 0;
        i = 2;
    }

    for (; i < argc; ++i) {
        if (i > ((trailing_newline == 0) ? 2 : 1)) {
            if (rt_write_char(1, ' ') != 0) {
                return 1;
            }
        }

        if (rt_write_cstr(1, argv[i]) != 0) {
            return 1;
        }
    }

    if (trailing_newline) {
        return rt_write_char(1, '\n') == 0 ? 0 : 1;
    }

    return 0;
}
