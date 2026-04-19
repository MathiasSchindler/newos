#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int main(int argc, char **argv) {
    unsigned long long port = 0;
    PlatformNetcatOptions options;
    int listen_mode = 0;
    int use_udp = 0;
    int verbose = 0;
    int scan_mode = 0;
    int argi = 1;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-l") == 0) {
            listen_mode = 1;
        } else if (rt_strcmp(argv[argi], "-u") == 0) {
            use_udp = 1;
        } else if (rt_strcmp(argv[argi], "-z") == 0) {
            scan_mode = 1;
        } else if (rt_strcmp(argv[argi], "-w") == 0) {
            unsigned long long timeout_value = 0;
            if (argi + 1 >= argc || tool_parse_duration_ms(argv[argi + 1], &timeout_value) != 0 || timeout_value > 0xffffffffULL) {
                tool_write_usage("netcat", "[-u] [-z] [-w TIMEOUT] [-v] HOST PORT | -l [-u] [-w TIMEOUT] [-v] PORT");
                return 1;
            }
            options.timeout_milliseconds = (unsigned int)timeout_value;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-v") == 0) {
            verbose = 1;
        } else {
            tool_write_usage("netcat", "[-u] [-z] [-w TIMEOUT] [-v] HOST PORT | -l [-u] [-w TIMEOUT] [-v] PORT");
            return 1;
        }
        argi += 1;
    }

    options.listen_mode = listen_mode;
    options.use_udp = use_udp;
    options.scan_mode = scan_mode;

    if (listen_mode) {
        if (argc - argi != 1 || tool_parse_uint_arg(argv[argi], &port, "netcat", "port") != 0 || port == 0 || port > 65535ULL) {
            tool_write_usage("netcat", "[-u] [-z] [-w TIMEOUT] [-v] HOST PORT | -l [-u] [-w TIMEOUT] [-v] PORT");
            return 1;
        }
        if (verbose) {
            rt_write_cstr(2, "netcat: listening on port ");
            rt_write_uint(2, port);
            rt_write_cstr(2, use_udp ? " (udp)" : " (tcp)");
            rt_write_char(2, '\n');
        }
        if (platform_netcat(0, (unsigned int)port, &options) != 0) {
            tool_write_error("netcat", "listen failed on port ", argv[argi]);
            return 1;
        }
        return 0;
    }

    if (argc - argi == 2) {
        if (tool_parse_uint_arg(argv[argi + 1], &port, "netcat", "port") != 0 || port == 0 || port > 65535ULL) {
            tool_write_usage("netcat", "[-u] [-z] [-w TIMEOUT] [-v] HOST PORT | -l [-u] [-w TIMEOUT] [-v] PORT");
            return 1;
        }
        if (verbose) {
            rt_write_cstr(2, scan_mode ? "netcat: scanning " : "netcat: connecting to ");
            rt_write_cstr(2, argv[argi]);
            rt_write_cstr(2, ":");
            rt_write_uint(2, port);
            rt_write_cstr(2, use_udp ? " (udp)" : " (tcp)");
            rt_write_char(2, '\n');
        }
        if (platform_netcat(argv[argi], (unsigned int)port, &options) != 0) {
            tool_write_error("netcat", scan_mode ? "scan failed for " : "connect failed to ", argv[argi]);
            return 1;
        }
        return 0;
    }

    tool_write_usage("netcat", "[-u] [-z] [-w TIMEOUT] [-v] HOST PORT | -l [-u] [-w TIMEOUT] [-v] PORT");
    return 1;
}
