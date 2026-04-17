#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int human_readable;
} DfOptions;

static int print_one(const char *path, const DfOptions *options) {
    unsigned long long total = 0;
    unsigned long long free_space = 0;
    unsigned long long available = 0;
    unsigned long long used;
    unsigned long long use_percent;
    char total_text[32];
    char used_text[32];
    char available_text[32];

    if (platform_get_filesystem_usage(path, &total, &free_space, &available) != 0) {
        rt_write_cstr(2, "df: failed to inspect ");
        rt_write_line(2, path);
        return 1;
    }

    used = (total >= free_space) ? (total - free_space) : 0ULL;
    use_percent = (total == 0ULL) ? 0ULL : (used * 100ULL) / total;

    tool_format_size(total, options->human_readable, total_text, sizeof(total_text));
    tool_format_size(used, options->human_readable, used_text, sizeof(used_text));
    tool_format_size(available, options->human_readable, available_text, sizeof(available_text));

    rt_write_cstr(1, path);
    rt_write_char(1, '\t');
    rt_write_cstr(1, total_text);
    rt_write_char(1, '\t');
    rt_write_cstr(1, used_text);
    rt_write_char(1, '\t');
    rt_write_cstr(1, available_text);
    rt_write_char(1, '\t');
    rt_write_uint(1, use_percent);
    rt_write_char(1, '%');
    rt_write_char(1, '\t');
    rt_write_line(1, path);
    return 0;
}

int main(int argc, char **argv) {
    DfOptions options;
    int argi = 1;
    int i;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 'h') {
                options.human_readable = 1;
            } else {
                rt_write_line(2, "Usage: df [-h] [path ...]");
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    rt_write_line(1, "Filesystem\tSize\tUsed\tAvailable\tUse%\tMounted on");

    if (argi >= argc) {
        return print_one("/", &options);
    }

    for (i = argi; i < argc; ++i) {
        if (print_one(argv[i], &options) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
