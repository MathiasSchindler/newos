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

static void print_json_start(const char *host, const PlatformTracerouteOptions *options) {
    if (tool_json_begin_event(1, "traceroute", "stdout", "trace_start") != 0) return;
    rt_write_cstr(1, ",\"data\":{\"host\":");
    tool_json_write_string(1, host);
    rt_write_cstr(1, ",\"max_ttl\":");
    rt_write_uint(1, (unsigned long long)options->max_ttl);
    rt_write_cstr(1, ",\"queries\":");
    rt_write_uint(1, (unsigned long long)options->queries);
    rt_write_cstr(1, ",\"timeout_seconds\":");
    rt_write_uint(1, (unsigned long long)options->timeout_seconds);
    rt_write_cstr(1, ",\"family\":");
    if (options->family == PLATFORM_NETWORK_FAMILY_IPV4) tool_json_write_string(1, "ipv4");
    else if (options->family == PLATFORM_NETWORK_FAMILY_IPV6) tool_json_write_string(1, "ipv6");
    else tool_json_write_string(1, "any");
    rt_write_cstr(1, ",\"numeric_only\":");
    rt_write_cstr(1, options->numeric_only ? "true" : "false");
    rt_write_char(1, '}');
    tool_json_end_event(1);
}

static void print_json_hop(const PlatformTracerouteHop *hop) {
    unsigned int probe;

    if (tool_json_begin_event(1, "traceroute", "stdout", "trace_hop") != 0) return;
    rt_write_cstr(1, ",\"data\":{\"ttl\":");
    rt_write_uint(1, (unsigned long long)hop->ttl);
    rt_write_cstr(1, ",\"address\":");
    if (hop->address[0] != '\0') tool_json_write_string(1, hop->address);
    else rt_write_cstr(1, "null");
    rt_write_cstr(1, ",\"hostname\":");
    if (hop->hostname[0] != '\0') tool_json_write_string(1, hop->hostname);
    else rt_write_cstr(1, "null");
    rt_write_cstr(1, ",\"reply_count\":");
    rt_write_uint(1, (unsigned long long)hop->reply_count);
    rt_write_cstr(1, ",\"reached_destination\":");
    rt_write_cstr(1, hop->reached_destination ? "true" : "false");
    rt_write_cstr(1, ",\"probes\":[");
    for (probe = 0U; probe < hop->probe_count; ++probe) {
        if (probe > 0U) rt_write_char(1, ',');
        rt_write_cstr(1, "{\"replied\":");
        rt_write_cstr(1, hop->probe_replied[probe] ? "true" : "false");
        rt_write_cstr(1, ",\"rtt_ms\":");
        if (hop->probe_replied[probe]) rt_write_uint(1, (unsigned long long)hop->rtt_milliseconds[probe]);
        else rt_write_cstr(1, "null");
        rt_write_char(1, '}');
    }
    rt_write_cstr(1, "]}");
    tool_json_end_event(1);
}

static void print_hop_callback(const PlatformTracerouteHop *hop, void *user_data) {
    (void)user_data;
    print_hop(hop);
}

static void print_json_hop_callback(const PlatformTracerouteHop *hop, void *user_data) {
    (void)user_data;
    print_json_hop(hop);
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

    trace_options.max_ttl = (unsigned int)max_ttl;
    trace_options.queries = (unsigned int)queries;
    trace_options.timeout_seconds = (unsigned int)timeout;
    trace_options.payload_size = PLATFORM_PING_DEFAULT_PAYLOAD_SIZE;
    trace_options.family = family;
    trace_options.numeric_only = numeric_only;
    trace_options.hop_callback = tool_json_is_enabled() ? print_json_hop_callback : print_hop_callback;
    trace_options.hop_callback_user_data = 0;

    if (tool_json_is_enabled()) {
        print_json_start(host, &trace_options);
    } else {
        rt_write_cstr(1, "traceroute to ");
        rt_write_cstr(1, host);
        rt_write_cstr(1, ", ");
        rt_write_uint(1, max_ttl);
        rt_write_line(1, " hops max");
    }

    if (platform_trace_route(host, &trace_options, hops, sizeof(hops) / sizeof(hops[0]), &hop_count) != 0) {
        tool_write_error("traceroute", "unable to trace route to ", host);
        return 1;
    }

    if (hop_count > 0U && hops[hop_count - 1U].reached_destination) {
        return 0;
    }
    return 1;
}