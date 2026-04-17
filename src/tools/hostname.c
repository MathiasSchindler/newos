#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int main(int argc, char **argv) {
    char name[256];

    if (argc == 1) {
        if (platform_get_hostname(name, sizeof(name)) != 0) {
            tool_write_error("hostname", "failed", 0);
            return 1;
        }
        rt_write_line(1, name);
        return 0;
    }

    if (argc != 2) {
        tool_write_usage("hostname", "[NAME]");
        return 1;
    }

    if (platform_set_hostname(argv[1]) != 0) {
        tool_write_error("hostname", "cannot set ", argv[1]);
        return 1;
    }

    return 0;
}
