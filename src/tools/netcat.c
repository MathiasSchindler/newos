#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int main(int argc, char **argv) {
    unsigned long long port = 0;

    if (argc == 3 && rt_strcmp(argv[1], "-l") == 0) {
        if (tool_parse_uint_arg(argv[2], &port, "netcat", "port") != 0 || port == 0 || port > 65535ULL) {
            tool_write_usage("netcat", "HOST PORT | -l PORT");
            return 1;
        }
        if (platform_netcat_tcp(0, (unsigned int)port, 1) != 0) {
            tool_write_error("netcat", "listen failed on port ", argv[2]);
            return 1;
        }
        return 0;
    }

    if (argc == 3) {
        if (tool_parse_uint_arg(argv[2], &port, "netcat", "port") != 0 || port == 0 || port > 65535ULL) {
            tool_write_usage("netcat", "HOST PORT | -l PORT");
            return 1;
        }
        if (platform_netcat_tcp(argv[1], (unsigned int)port, 0) != 0) {
            tool_write_error("netcat", "connect failed to ", argv[1]);
            return 1;
        }
        return 0;
    }

    tool_write_usage("netcat", "HOST PORT | -l PORT");
    return 1;
}
