#include "platform.h"
#include "runtime.h"

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-c COUNT] IPV4_ADDRESS");
}

int main(int argc, char **argv) {
    unsigned long long count = 4;
    int arg_index = 1;

    if (argc > 1 && rt_strcmp(argv[1], "-c") == 0) {
        if (argc < 4 || rt_parse_uint(argv[2], &count) != 0 || count == 0) {
            print_usage(argv[0]);
            return 1;
        }
        arg_index = 3;
    }

    if (argc != arg_index + 1) {
        print_usage(argv[0]);
        return 1;
    }

    if (platform_ping_host(argv[arg_index], (unsigned int)count) != 0) {
        rt_write_cstr(2, "ping: unable to reach ");
        rt_write_line(2, argv[arg_index]);
        return 1;
    }

    rt_write_cstr(1, "PING ok: ");
    rt_write_cstr(1, argv[arg_index]);
    rt_write_cstr(1, " replies=");
    rt_write_uint(1, count);
    rt_write_char(1, '\n');
    return 0;
}
