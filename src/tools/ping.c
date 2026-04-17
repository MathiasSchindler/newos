#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-c COUNT] [-i SECONDS] [-W SECONDS] [-s BYTES] [-t TTL] HOST");
}

int main(int argc, char **argv) {
    unsigned long long count = PLATFORM_PING_DEFAULT_COUNT;
    unsigned long long interval_seconds = PLATFORM_PING_DEFAULT_INTERVAL_SECONDS;
    unsigned long long timeout_seconds = PLATFORM_PING_DEFAULT_TIMEOUT_SECONDS;
    unsigned long long payload_size = PLATFORM_PING_DEFAULT_PAYLOAD_SIZE;
    unsigned long long ttl = 0;
    const char *host = 0;
    int argi = 1;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-c") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &count, "ping", "count") != 0 || count == 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        if (rt_strcmp(argv[argi], "-i") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &interval_seconds, "ping", "interval") != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        if (rt_strcmp(argv[argi], "-W") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &timeout_seconds, "ping", "timeout") != 0 || timeout_seconds == 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        if (rt_strcmp(argv[argi], "-s") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &payload_size, "ping", "size") != 0 || payload_size > PLATFORM_PING_MAX_PAYLOAD_SIZE) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        if (rt_strcmp(argv[argi], "-t") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &ttl, "ping", "ttl") != 0 || ttl > PLATFORM_PING_MAX_TTL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        break;
    }

    if (argc != argi + 1) {
        print_usage(argv[0]);
        return 1;
    }
    host = argv[argi];

    {
        PlatformPingOptions options;
        options.count = (unsigned int)count;
        options.interval_seconds = (unsigned int)interval_seconds;
        options.timeout_seconds = (unsigned int)timeout_seconds;
        options.payload_size = (unsigned int)payload_size;
        options.ttl = (unsigned int)ttl;
        return platform_ping_host(host, &options);
    }
}
