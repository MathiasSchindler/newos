#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static int streq(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static int format_cidr(const PlatformDhcpLease *lease, char *buffer, size_t buffer_size) {
    char digits[16];
    size_t used;

    if (lease == 0 || buffer == 0 || buffer_size == 0U || lease->address[0] == '\0') {
        return -1;
    }

    rt_copy_string(buffer, buffer_size, lease->address);
    used = rt_strlen(buffer);
    if (used + 2U >= buffer_size) {
        return -1;
    }
    buffer[used++] = '/';
    buffer[used] = '\0';
    rt_unsigned_to_string((unsigned long long)lease->prefix_length, digits, sizeof(digits));
    if (used + rt_strlen(digits) + 1U > buffer_size) {
        return -1;
    }
    rt_copy_string(buffer + used, buffer_size - used, digits);
    return 0;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-A] [-i IFACE] [-s SERVER] [-p SERVER_PORT] [-P CLIENT_PORT] [-t TIMEOUT]");
}

static void print_help(const char *program_name) {
    print_usage(program_name);
    rt_write_line(1, "Acquire a small IPv4 DHCP lease and optionally apply it to an interface.");
    rt_write_line(1, "");
    rt_write_line(1, "Options:");
    rt_write_line(1, "  -A           apply the acquired address and default route to IFACE");
    rt_write_line(1, "  -i IFACE     interface name for apply mode or interface-specific probing");
    rt_write_line(1, "  -s SERVER    DHCP server address (default: broadcast discover)");
    rt_write_line(1, "  -p PORT      DHCP server port (default: 67)");
    rt_write_line(1, "  -P PORT      local client port (default: 68)");
    rt_write_line(1, "  -t TIMEOUT   request timeout (for example 3s or 500ms)");
}

int main(int argc, char **argv) {
    int argi = 1;
    int apply = 0;
    const char *ifname = 0;
    const char *server = 0;
    unsigned long long server_port = 67ULL;
    unsigned long long client_port = 68ULL;
    unsigned long long timeout_ms = 3000ULL;
    PlatformDhcpLease lease;

    rt_memset(&lease, 0, sizeof(lease));

    while (argi < argc) {
        if (streq(argv[argi], "-A")) {
            apply = 1;
            argi += 1;
            continue;
        }
        if (streq(argv[argi], "-i")) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            ifname = argv[argi + 1];
            argi += 2;
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
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &server_port, "dhcp", "server port") != 0 ||
                server_port == 0ULL || server_port > 65535ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        if (streq(argv[argi], "-P")) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &client_port, "dhcp", "client port") != 0 ||
                client_port == 0ULL || client_port > 65535ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
            continue;
        }
        if (streq(argv[argi], "-t")) {
            if (argi + 1 >= argc || tool_parse_duration_ms(argv[argi + 1], &timeout_ms) != 0 || timeout_ms == 0ULL) {
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
        print_usage(argv[0]);
        return 1;
    }

    if (apply && (ifname == 0 || ifname[0] == '\0')) {
        tool_write_error("dhcp", "-A requires an interface via -i ", 0);
        return 1;
    }

    if (platform_dhcp_request(ifname, server, (unsigned int)server_port, (unsigned int)client_port, (unsigned int)timeout_ms, &lease) != 0) {
        tool_write_error("dhcp", "failed to acquire a lease", 0);
        return 1;
    }

    rt_write_line(1, "DHCP lease acquired");
    rt_write_cstr(1, "  address: ");
    rt_write_cstr(1, lease.address[0] != '\0' ? lease.address : "(none)");
    if (lease.prefix_length > 0U) {
        rt_write_cstr(1, "/");
        rt_write_uint(1, (unsigned long long)lease.prefix_length);
    }
    rt_write_char(1, '\n');
    if (lease.server[0] != '\0') {
        rt_write_cstr(1, "  server:  ");
        rt_write_line(1, lease.server);
    }
    if (lease.router[0] != '\0') {
        rt_write_cstr(1, "  router:  ");
        rt_write_line(1, lease.router);
    }
    if (lease.dns1[0] != '\0') {
        rt_write_cstr(1, "  dns:     ");
        rt_write_cstr(1, lease.dns1);
        if (lease.dns2[0] != '\0') {
            rt_write_cstr(1, ", ");
            rt_write_cstr(1, lease.dns2);
        }
        rt_write_char(1, '\n');
    }
    if (lease.lease_seconds > 0U) {
        rt_write_cstr(1, "  lease:   ");
        rt_write_uint(1, (unsigned long long)lease.lease_seconds);
        rt_write_line(1, "s");
    }

    if (apply) {
        char cidr[96];

        if (platform_network_link_set(ifname, 1, 0U, 0) != 0) {
            tool_write_error("dhcp", "failed to bring up ", ifname);
            return 1;
        }
        if (format_cidr(&lease, cidr, sizeof(cidr)) != 0 || platform_network_address_change(ifname, cidr, 1) != 0) {
            tool_write_error("dhcp", "failed to apply the leased address to ", ifname);
            return 1;
        }
        if (lease.router[0] != '\0' && platform_network_route_change("default", lease.router, ifname, 1) != 0) {
            tool_write_error("dhcp", "failed to install the default route for ", ifname);
            return 1;
        }
        rt_write_cstr(1, "Applied lease to ");
        rt_write_line(1, ifname);
    }

    return 0;
}
