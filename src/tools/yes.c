#include "runtime.h"

int main(int argc, char **argv) {
    if (argc <= 1) {
        for (;;) {
            if (rt_write_cstr(1, "y\n") != 0) {
                return 0;
            }
        }
    }

    for (;;) {
        int i;
        for (i = 1; i < argc; ++i) {
            if (i > 1 && rt_write_char(1, ' ') != 0) {
                return 0;
            }
            if (rt_write_cstr(1, argv[i]) != 0) {
                return 0;
            }
        }
        if (rt_write_char(1, '\n') != 0) {
            return 0;
        }
    }
}
