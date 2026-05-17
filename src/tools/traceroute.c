#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_help(void) {
    rt_write_line(1, "traceroute - trace the route to a host with increasing ICMP TTL values");
    rt_write_line(1, "Usage: traceroute [-4|-6] [-n] [-m MAXTTL] [-q QUERIES] [-w SECONDS] HOST");
    rt_write_line(1, "Prints each responding hop and stops when a probe reaches the destination.");
}

static void write_probe_time(unsigned int milliseconds) {
    rt_write_uint(1, (unsigned long long)milliseconds);
    rt_write_cstr(1, " ms");
}

static void print_hop(const PlatformTracerouteHop *hop) {
    unsigned int probe;

    rt_write_uint(1, (unsigned long long)hop->ttl);
    rt_write_cstr(1, "  ");
    if (hop->reply_count == 0U || hop->address[0] == '\0') {
        for (probe = 0U; probe < hop->probe_count; ++probe) {
            if (probe > 0U) rt_write_cstr(1, "  ");
            rt_write_char(1, '*');
        }
        rt_write_char(1, '\n');
        return;
    }

    if (hop->hostname[0] != '\0' && rt_strcmp(hop->hostname, hop->address) != 0) {
        rt_write_cstr(1, hop->hostname);
        rt_write_cstr(1, " (");
        rt_write_cstr(1, hop->address);
        rt_write_char(1, ')');
    } else {
        rt_write_cstr(1, hop->address);
    }
    for (probe = 0U; probe < hop->probe_count; ++probe) {
        rt_write_cstr(1, "  ");
        if (hop->probe_replied[probe]) {
            write_probe_time(hop->rtt_milliseconds[probe]);
        } else {
            rt_write_char(1, '*');
        }
    }
    rt_write_char(1, '\n');
}

int main(int argc, char **argv) {
    static PlatformTracerouteHop hops[PLATFORM_PING_MAX_TTL];
    unsigned long long max_ttl = 30ULL;
    unsigned long long queries = 1ULL;
    unsigned long long timeout = PLATFORM_PING_DEFAULT_TIMEOUT_SECONDS;
    int family = PLATFORM_NETWORK_FAMILY_ANY;
    int numeric_only = 0;
    const char *host;
    PlatformTracerouteOptions trace_options;
    size_t hop_count = 0U;
    size_t index;
    ToolOptState opt;
    int r;

    tool_opt_init(&opt, argc, argv, "traceroute", "[-4|-6] [-n] [-m MAXTTL] [-q QUERIES] [-w SECONDS] HOST");
    while ((r = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-4") == 0) {
            family = PLATFORM_NETWORK_FAMILY_IPV4;
        } else if (rt_strcmp(opt.flag, "-6") == 0) {
            family = PLATFORM_NETWORK_FAMILY_IPV6;
        } else if (rt_strcmp(opt.flag, "-n") == 0) {
            numeric_only = 1;
        } else if (rt_strcmp(opt.flag, "-m") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            if (tool_parse_uint_arg(opt.value, &max_ttl, "traceroute", "max ttl") != 0 || max_ttl == 0ULL || max_ttl > PLATFORM_PING_MAX_TTL) {
                tool_write_usage("traceroute", "[-4|-6] [-n] [-m MAXTTL] [-q QUERIES] [-w SECONDS] HOST");
                return 1;
            }
        } else if (rt_strcmp(opt.flag, "-q") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            if (tool_parse_uint_arg(opt.value, &queries, "traceroute", "queries") != 0 || queries == 0ULL || queries > 10ULL) {
                tool_write_usage("traceroute", "[-4|-6] [-n] [-m MAXTTL] [-q QUERIES] [-w SECONDS] HOST");
                return 1;
            }
        } else if (rt_strcmp(opt.flag, "-w") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            if (tool_parse_uint_arg(opt.value, &timeout, "traceroute", "timeout") != 0 || timeout == 0ULL) {
                tool_write_usage("traceroute", "[-4|-6] [-n] [-m MAXTTL] [-q QUERIES] [-w SECONDS] HOST");
                return 1;
            }
        } else {
            tool_write_error("traceroute", "unknown option: ", opt.flag);
            tool_write_usage("traceroute", "[-4|-6] [-n] [-m MAXTTL] [-q QUERIES] [-w SECONDS] HOST");
            return 1;
        }
    }
    if (r == TOOL_OPT_HELP) {
        print_help();
        return 0;
    }
    if (r == TOOL_OPT_ERROR) return 1;
    if (opt.argi + 1 != argc) {
        tool_write_usage("traceroute", "[-4|-6] [-n] [-m MAXTTL] [-q QUERIES] [-w SECONDS] HOST");
        return 1;
    }

    host = argv[opt.argi];
    rt_write_cstr(1, "traceroute to ");
    rt_write_cstr(1, host);
    rt_write_cstr(1, ", ");
    rt_write_uint(1, max_ttl);
    rt_write_line(1, " hops max");

    trace_options.max_ttl = (unsigned int)max_ttl;
    trace_options.queries = (unsigned int)queries;
    trace_options.timeout_seconds = (unsigned int)timeout;
    trace_options.payload_size = PLATFORM_PING_DEFAULT_PAYLOAD_SIZE;
    trace_options.family = family;
    trace_options.numeric_only = numeric_only;

    if (platform_trace_route(host, &trace_options, hops, sizeof(hops) / sizeof(hops[0]), &hop_count) != 0) {
        tool_write_error("traceroute", "unable to trace route to ", host);
        return 1;
    }

    for (index = 0U; index < hop_count; ++index) {
        print_hop(hops + index);
        if (hops[index].reached_destination) {
            return 0;
        }
    }
    return 1;
}