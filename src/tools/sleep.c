#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "DURATION...");
}

int main(int argc, char **argv) {
    unsigned long long milliseconds = 0;
    int argi;

    if (argc <= 1) {
        print_usage(argv[0]);
        return 1;
    }

    for (argi = 1; argi < argc; ++argi) {
        unsigned long long value = 0;

        if (rt_strcmp(argv[argi], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (tool_parse_duration_ms(argv[argi], &value) != 0) {
            tool_write_error("sleep", "invalid duration ", argv[argi]);
            print_usage(argv[0]);
            return 1;
        }
        milliseconds += value;
    }

    if (platform_sleep_milliseconds(milliseconds) != 0) {
        tool_write_error("sleep", "failed", 0);
        return 1;
    }

    return 0;
}
