#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int main(int argc, char **argv) {
    unsigned long long port = 0;
    int listen_mode = 0;
    int verbose = 0;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-l") == 0) {
            listen_mode = 1;
        } else if (rt_strcmp(argv[argi], "-v") == 0) {
            verbose = 1;
        } else {
            tool_write_usage("netcat", "[-v] HOST PORT | -l [-v] PORT");
            return 1;
        }
        argi += 1;
    }

    if (listen_mode) {
        if (argc - argi != 1 || tool_parse_uint_arg(argv[argi], &port, "netcat", "port") != 0 || port == 0 || port > 65535ULL) {
            tool_write_usage("netcat", "[-v] HOST PORT | -l [-v] PORT");
            return 1;
        }
        if (verbose) {
            rt_write_cstr(2, "netcat: listening on port ");
            rt_write_uint(2, port);
            rt_write_char(2, '\n');
        }
        if (platform_netcat_tcp(0, (unsigned int)port, 1) != 0) {
            tool_write_error("netcat", "listen failed on port ", argv[argi]);
            return 1;
        }
        return 0;
    }

    if (argc - argi == 2) {
        if (tool_parse_uint_arg(argv[argi + 1], &port, "netcat", "port") != 0 || port == 0 || port > 65535ULL) {
            tool_write_usage("netcat", "[-v] HOST PORT | -l [-v] PORT");
            return 1;
        }
        if (verbose) {
            rt_write_cstr(2, "netcat: connecting to ");
            rt_write_cstr(2, argv[argi]);
            rt_write_cstr(2, ":");
            rt_write_uint(2, port);
            rt_write_char(2, '\n');
        }
        if (platform_netcat_tcp(argv[argi], (unsigned int)port, 0) != 0) {
            tool_write_error("netcat", "connect failed to ", argv[argi]);
            return 1;
        }
        return 0;
    }

    tool_write_usage("netcat", "[-v] HOST PORT | -l [-v] PORT");
    return 1;
}
