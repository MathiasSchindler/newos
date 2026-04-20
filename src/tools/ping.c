#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static int streq(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static int contains_char(const char *text, char ch) {
    size_t i = 0U;

    while (text[i] != '\0') {
        if (text[i] == ch) {
            return 1;
        }
        i += 1U;
    }
    return 0;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-4] [-nq] [-c COUNT] [-i SECONDS] [-W SECONDS] [-w DEADLINE] [-s BYTES] [-t TTL] HOST");
}

static void print_usage_stdout(const char *program_name) {
    rt_write_cstr(1, "Usage: ");
    rt_write_cstr(1, program_name);
    rt_write_cstr(1, " [-4] [-nq] [-c COUNT] [-i SECONDS] [-W SECONDS] [-w DEADLINE] [-s BYTES] [-t TTL] HOST\n");
}

static void print_help(const char *program_name) {
    print_usage_stdout(program_name);
    rt_write_line(1, "Send ICMP echo requests to an IPv4 host and report reachability.");
    rt_write_line(1, "");
    rt_write_line(1, "Options:");
    rt_write_line(1, "  -4           use IPv4 (default)");
    rt_write_line(1, "  -6           request IPv6 mode and fail with a clear message");
    rt_write_line(1, "  -n           numeric output only; accepted for compatibility");
    rt_write_line(1, "  -q           quiet output; show only the banner and summary");
    rt_write_line(1, "  -c COUNT     send COUNT probes");
    rt_write_line(1, "  -i SECONDS   wait SECONDS between probes");
    rt_write_line(1, "  -W SECONDS   wait up to SECONDS for each reply");
    rt_write_line(1, "  -w SECONDS   stop after SECONDS overall");
    rt_write_line(1, "  -s BYTES     set payload size");
    rt_write_line(1, "  -t TTL       set IPv4 time-to-live");
}

int main(int argc, char **argv) {
    unsigned long long count = PLATFORM_PING_DEFAULT_COUNT;
    unsigned long long interval_seconds = PLATFORM_PING_DEFAULT_INTERVAL_SECONDS;
    unsigned long long timeout_seconds = PLATFORM_PING_DEFAULT_TIMEOUT_SECONDS;
    unsigned long long payload_size = PLATFORM_PING_DEFAULT_PAYLOAD_SIZE;
    unsigned long long ttl = 0;
    unsigned long long deadline_seconds = 0;
    const char *host = 0;
    int argi = 1;
    int family_filter = PLATFORM_NETWORK_FAMILY_ANY;
    int quiet = 0;
    int numeric_only = 0;

    while (argi < argc) {
        if (streq(argv[argi], "-4")) {
            family_filter = PLATFORM_NETWORK_FAMILY_IPV4;
            argi += 1;
            continue;
        }
        if (streq(argv[argi], "-6")) {
            family_filter = PLATFORM_NETWORK_FAMILY_IPV6;
            argi += 1;
            continue;
        }
        if (streq(argv[argi], "-n")) {
            numeric_only = 1;
            argi += 1;
            continue;
        }
        if (streq(argv[argi], "-q")) {
            quiet = 1;
            argi += 1;
            continue;
        }
        if (streq(argv[argi], "-h") || streq(argv[argi], "--help")) {
            print_help(argv[0]);
            return 0;
        }
        if (streq(argv[argi], "-c")) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &count, "ping", "count") != 0 || count == 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        if (streq(argv[argi], "-i")) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &interval_seconds, "ping", "interval") != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        if (streq(argv[argi], "-W")) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &timeout_seconds, "ping", "timeout") != 0 || timeout_seconds == 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        if (streq(argv[argi], "-w")) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &deadline_seconds, "ping", "deadline") != 0 || deadline_seconds == 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        if (streq(argv[argi], "-s")) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &payload_size, "ping", "size") != 0 || payload_size > PLATFORM_PING_MAX_PAYLOAD_SIZE) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        if (streq(argv[argi], "-t")) {
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

    if (family_filter == PLATFORM_NETWORK_FAMILY_IPV6 || contains_char(host, ':')) {
        tool_write_error("ping", "IPv6 echo requests are not yet implemented", 0);
        return 1;
    }

    {
        PlatformPingOptions options;

        (void)numeric_only;
        options.count = (unsigned int)count;
        options.interval_seconds = (unsigned int)interval_seconds;
        options.timeout_seconds = (unsigned int)timeout_seconds;
        options.payload_size = (unsigned int)payload_size;
        options.ttl = (unsigned int)ttl;
        options.deadline_seconds = (unsigned int)deadline_seconds;
        options.quiet_output = quiet;
        return platform_ping_host(host, &options);
    }
}
