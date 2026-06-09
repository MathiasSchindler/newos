#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define NSLOOKUP_MAX_RESULTS 32U

#define streq tool_str_equal

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-4|-6] [-s SERVER] [-p PORT] NAME");
}

static void print_help(const char *program_name) {
    print_usage(program_name);
    rt_write_line(1, "Look up IPv4 and IPv6 addresses using the platform DNS backend.");
    rt_write_line(1, "");
    rt_write_line(1, "Options:");
    rt_write_line(1, "  -4           query IPv4 A records only");
    rt_write_line(1, "  -6           query IPv6 AAAA records only");
    rt_write_line(1, "  -s SERVER    use the specified DNS server instead of the default resolver");
    rt_write_line(1, "  -p PORT      use a custom DNS server port (default: 53)");
    rt_write_line(1, "  --json       write newline-delimited JSON events");
}

static void write_json_nslookup_result(const char *name, const PlatformDnsEntry *entries, size_t count) {
    size_t i;
    if (tool_json_begin_event(1, "nslookup", "stdout", "dns_result") != 0) return;
    rt_write_cstr(1, ",\"data\":{\"query\":");
    tool_json_write_string(1, name);
    rt_write_cstr(1, ",\"answers\":[");
    for (i = 0U; i < count; ++i) {
        if (i > 0U) rt_write_char(1, ',');
        rt_write_char(1, '{');
        rt_write_cstr(1, "\"address\":");
        tool_json_write_string(1, entries[i].address);
        rt_write_char(1, '}');
    }
    rt_write_cstr(1, "]}");
    tool_json_end_event(1);
}

int main(int argc, char **argv) {
    int argi = 1;
    int family = PLATFORM_NETWORK_FAMILY_ANY;
    const char *server = 0;
    const char *name = 0;
    unsigned long long port_value = 53ULL;
    PlatformDnsEntry entries[NSLOOKUP_MAX_RESULTS];
    size_t count = 0U;
    size_t i;

    while (argi < argc) {
        if (streq(argv[argi], "-4")) {
            family = PLATFORM_NETWORK_FAMILY_IPV4;
            argi += 1;
            continue;
        }
        if (streq(argv[argi], "-6")) {
            family = PLATFORM_NETWORK_FAMILY_IPV6;
            argi += 1;
            continue;
        }
        if (streq(argv[argi], "--json")) {
            tool_json_set_enabled(1);
            argi += 1;
            continue;
        }
        if (streq(argv[argi], "-s")) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            server = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (streq(argv[argi], "-p")) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &port_value, "nslookup", "port") != 0 ||
                port_value == 0ULL || port_value > 65535ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        if (streq(argv[argi], "-h") || streq(argv[argi], "--help")) {
            print_help(argv[0]);
            return 0;
        }
        break;
    }

    if (argi + 1 != argc) {
        print_usage(argv[0]);
        return 1;
    }
    name = argv[argi];

    if (platform_dns_lookup(server, (unsigned int)port_value, name, family, entries, NSLOOKUP_MAX_RESULTS, &count) != 0 || count == 0U) {
        tool_write_error("nslookup", "lookup failed for ", name);
        return 1;
    }

    if (tool_json_is_enabled()) {
        write_json_nslookup_result(name, entries, count);
        return 0;
    }

    rt_write_cstr(1, "Name:    ");
    rt_write_line(1, name);
    for (i = 0U; i < count; ++i) {
        rt_write_cstr(1, i == 0U ? "Address: " : "Address: ");
        rt_write_line(1, entries[i].address);
    }

    return 0;
}
