#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "SECONDS");
}

int main(int argc, char **argv) {
    unsigned long long seconds = 0;

    if (argc != 2 || tool_parse_uint_arg(argv[1], &seconds, "sleep", "seconds") != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (platform_sleep_seconds((unsigned int)seconds) != 0) {
        tool_write_error("sleep", "failed", 0);
        return 1;
    }

    return 0;
}
