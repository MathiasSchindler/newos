#include "platform.h"
#include "runtime.h"

#define READLINK_CAPACITY 1024

int main(int argc, char **argv) {
    char buffer[READLINK_CAPACITY];

    if (argc != 2) {
        rt_write_line(2, "Usage: readlink PATH");
        return 1;
    }

    if (platform_read_symlink(argv[1], buffer, sizeof(buffer)) != 0) {
        rt_write_cstr(2, "readlink: cannot read ");
        rt_write_line(2, argv[1]);
        return 1;
    }

    return rt_write_line(1, buffer) == 0 ? 0 : 1;
}
