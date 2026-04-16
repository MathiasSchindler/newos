#include "platform.h"
#include "runtime.h"

#define UNAME_FIELD_CAPACITY 128

int main(int argc, char **argv) {
    char sysname[UNAME_FIELD_CAPACITY];
    char nodename[UNAME_FIELD_CAPACITY];
    char release[UNAME_FIELD_CAPACITY];
    char machine[UNAME_FIELD_CAPACITY];
    int show_all = 0;

    if (argc > 1 && rt_strcmp(argv[1], "-a") == 0) {
        show_all = 1;
    }

    if (platform_get_uname(sysname, sizeof(sysname), nodename, sizeof(nodename), release, sizeof(release), machine, sizeof(machine)) != 0) {
        rt_write_line(2, "uname: failed");
        return 1;
    }

    if (!show_all) {
        return rt_write_line(1, sysname) == 0 ? 0 : 1;
    }

    if (rt_write_cstr(1, sysname) != 0 ||
        rt_write_char(1, ' ') != 0 ||
        rt_write_cstr(1, nodename) != 0 ||
        rt_write_char(1, ' ') != 0 ||
        rt_write_cstr(1, release) != 0 ||
        rt_write_char(1, ' ') != 0 ||
        rt_write_line(1, machine) != 0) {
        return 1;
    }

    return 0;
}
