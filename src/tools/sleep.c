#include "platform.h"
#include "runtime.h"

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " SECONDS");
}

int main(int argc, char **argv) {
    unsigned long long seconds = 0;

    if (argc != 2 || rt_parse_uint(argv[1], &seconds) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (platform_sleep_seconds((unsigned int)seconds) != 0) {
        rt_write_line(2, "sleep: failed");
        return 1;
    }

    return 0;
}
