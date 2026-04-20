#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define NSLOOKUP_MAX_RESULTS 32U

static int streq(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

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

    rt_write_cstr(1, "Name:    ");
    rt_write_line(1, name);
    for (i = 0U; i < count; ++i) {
        rt_write_cstr(1, i == 0U ? "Address: " : "Address: ");
        rt_write_line(1, entries[i].address);
    }

    return 0;
}
