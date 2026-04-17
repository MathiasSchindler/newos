#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int main(int argc, char **argv) {
    int use_utc = 1;
    const char *format = 0;
    const char *actual_format = "%Y-%m-%d %H:%M:%S";
    int argi;
    char output[256];
    long long epoch_seconds;

    for (argi = 1; argi < argc; ++argi) {
        if (rt_strcmp(argv[argi], "-u") == 0) {
            use_utc = 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-l") == 0) {
            use_utc = 0;
            continue;
        }
        if (argv[argi][0] == '+' && argv[argi][1] != '\0' && format == 0) {
            format = argv[argi] + 1;
            continue;
        }
        tool_write_usage(argv[0], "[-u|-l] [+FORMAT]");
        return 1;
    }

    if (format != 0) {
        actual_format = format;
    }

    epoch_seconds = platform_get_epoch_time();
    if (platform_format_time(epoch_seconds, use_utc ? 0 : 1, actual_format, output, sizeof(output)) != 0) {
        if (platform_format_time(epoch_seconds, 0, "%Y-%m-%d %H:%M:%S", output, sizeof(output)) != 0) {
            tool_write_error("date", "failed to format time", 0);
            return 1;
        }
        use_utc = 1;
        format = 0;
    }

    if (rt_write_cstr(1, output) != 0) {
        return 1;
    }
    if (format == 0 && use_utc) {
        return rt_write_line(1, " UTC") == 0 ? 0 : 1;
    }
    return rt_write_char(1, '\n') == 0 ? 0 : 1;
}
