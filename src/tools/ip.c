#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define IP_USAGE "[-4|-6] {address|addr|a|link|l|route|r} ..."
#define IP_MAX_LINKS 128U
#define IP_MAX_ADDRESSES 512U
#define IP_MAX_ROUTES 256U

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

static int is_object_name(const char *text, const char *long_name, const char *medium_name, const char *short_name) {
    return streq(text, long_name) || streq(text, medium_name) || streq(text, short_name);
}

static void print_help(const char *program_name) {
    rt_write_cstr(1, "Usage: ");
    rt_write_cstr(1, program_name);
    rt_write_cstr(1, " [-4|-6] [-br|-brief|-o|--oneline] {address|addr|a|link|l|route|r} ...\n");
    rt_write_line(1, "Show and change network links, IPv4/IPv6 addresses, and IPv4 routes.");
    rt_write_line(1, "Common flags:");
    rt_write_line(1, "  -4, -6         restrict output to IPv4 or IPv6");
    rt_write_line(1, "  -br, -brief    concise one-line summaries");
    rt_write_line(1, "  -o, --oneline  compatibility alias for one-line output");
    rt_write_line(1, "");
    rt_write_line(1, "Objects:");
    rt_write_line(1, "  address|addr|a  [show [IFACE|dev IFACE]] | flush dev IFACE");
    rt_write_line(1, "                  | add CIDR dev IFACE | del CIDR dev IFACE");
    rt_write_line(1, "  link|l          [show [IFACE|dev IFACE]] | set dev IFACE [up|down] [mtu N]");
    rt_write_line(1, "  route|r         [show [default|PREFIX|dev IFACE]]");
    rt_write_line(1, "                  | add DEST|default [via GW] [dev IFACE]");
    rt_write_line(1, "                  | del DEST|default [via GW] [dev IFACE]");
}

static int append_flag_name(char *buffer, size_t buffer_size, const char *token) {
    size_t used = rt_strlen(buffer);
    size_t token_length = rt_strlen(token);

    if (used != 0U) {
        if (used + 1U >= buffer_size) {
            return -1;
        }
        buffer[used] = ',';
        used += 1U;
        buffer[used] = '\0';
    }
    if (used + token_length + 1U > buffer_size) {
        return -1;
    }
    memcpy(buffer + used, token, token_length + 1U);
    return 0;
}

static void format_flags(unsigned int flags, char *buffer, size_t buffer_size) {
    buffer[0] = '\0';

    if ((flags & PLATFORM_NETWORK_FLAG_UP) != 0U) {
        (void)append_flag_name(buffer, buffer_size, "UP");
    }
    if ((flags & PLATFORM_NETWORK_FLAG_BROADCAST) != 0U) {
        (void)append_flag_name(buffer, buffer_size, "BROADCAST");
    }
    if ((flags & PLATFORM_NETWORK_FLAG_LOOPBACK) != 0U) {
        (void)append_flag_name(buffer, buffer_size, "LOOPBACK");
    }
    if ((flags & PLATFORM_NETWORK_FLAG_RUNNING) != 0U) {
        (void)append_flag_name(buffer, buffer_size, "RUNNING");
    }
    if ((flags & PLATFORM_NETWORK_FLAG_MULTICAST) != 0U) {
        (void)append_flag_name(buffer, buffer_size, "MULTICAST");
    }

    if (buffer[0] == '\0') {
        rt_copy_string(buffer, buffer_size, "NONE");
    }
}

static const char *link_state_name(unsigned int flags) {
    if ((flags & PLATFORM_NETWORK_FLAG_UP) == 0U) {
        return "DOWN";
    }
    if ((flags & PLATFORM_NETWORK_FLAG_RUNNING) != 0U) {
        return "UP";
    }
    return "UNKNOWN";
}

static void write_link_header(size_t ordinal, const PlatformNetworkLink *link) {
    char flags[128];

    format_flags(link->flags, flags, sizeof(flags));
    rt_write_uint(1, link->index != 0U ? (unsigned long long)link->index : (unsigned long long)(ordinal + 1U));
    rt_write_cstr(1, ": ");
    rt_write_cstr(1, link->name);
    rt_write_cstr(1, ": <");
    rt_write_cstr(1, flags);
    rt_write_cstr(1, "> mtu ");
    rt_write_uint(1, (unsigned long long)link->mtu);
    rt_write_cstr(1, " state ");
    rt_write_line(1, link_state_name(link->flags));

    rt_write_cstr(1, "    link/");
    if ((link->flags & PLATFORM_NETWORK_FLAG_LOOPBACK) != 0U) {
        rt_write_cstr(1, "loopback");
    } else if (link->has_mac) {
        rt_write_cstr(1, "ether ");
        rt_write_cstr(1, link->mac);
    } else {
        rt_write_cstr(1, "unknown");
    }
    rt_write_char(1, '\n');
}

static void write_padding(size_t width, size_t used) {
    while (used < width) {
        rt_write_char(1, ' ');
        used += 1U;
    }
}

static int append_text(char *buffer, size_t buffer_size, const char *text) {
    size_t used = rt_strlen(buffer);
    size_t length = rt_strlen(text);

    if (used + length + 1U > buffer_size) {
        return -1;
    }
    memcpy(buffer + used, text, length + 1U);
    return 0;
}

static int append_uint_text(char *buffer, size_t buffer_size, unsigned long long value) {
    char digits[32];
    size_t count = 0U;
    size_t i;

    if (value == 0ULL) {
        return append_text(buffer, buffer_size, "0");
    }
    while (value > 0ULL && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    for (i = 0U; i < count; ++i) {
        char ch[2];
        ch[0] = digits[count - 1U - i];
        ch[1] = '\0';
        if (append_text(buffer, buffer_size, ch) != 0) {
            return -1;
        }
    }
    return 0;
}

static void write_brief_link_line(const PlatformNetworkLink *link) {
    size_t name_length = rt_strlen(link->name);
    const char *state = link_state_name(link->flags);
    size_t state_length = rt_strlen(state);

    rt_write_cstr(1, link->name);
    write_padding(16U, name_length);
    rt_write_cstr(1, state);
    write_padding(10U, state_length);
    rt_write_cstr(1, "mtu ");
    rt_write_uint(1, (unsigned long long)link->mtu);
    if ((link->flags & PLATFORM_NETWORK_FLAG_LOOPBACK) != 0U) {
        rt_write_cstr(1, " loopback");
    } else if (link->has_mac) {
        rt_write_cstr(1, " ");
        rt_write_cstr(1, link->mac);
    }
    rt_write_char(1, '\n');
}

static int show_links_brief(const char *dev_name) {
    PlatformNetworkLink links[IP_MAX_LINKS];
    size_t count = 0U;
    size_t i;
    int matched = 0;

    if (platform_list_network_links(links, IP_MAX_LINKS, &count) != 0) {
        tool_write_error("ip", "cannot inspect network links", 0);
        return 1;
    }

    for (i = 0U; i < count; ++i) {
        if (dev_name != 0 && rt_strcmp(dev_name, links[i].name) != 0) {
            continue;
        }
        write_brief_link_line(&links[i]);
        matched = 1;
    }

    if (!matched && dev_name != 0) {
        tool_write_error("ip", "unknown device: ", dev_name);
        return 1;
    }
    return 0;
}

static int show_links(const char *dev_name) {
    PlatformNetworkLink links[IP_MAX_LINKS];
    size_t count = 0U;
    size_t i;
    int matched = 0;

    if (platform_list_network_links(links, IP_MAX_LINKS, &count) != 0) {
        tool_write_error("ip", "cannot inspect network links", 0);
        return 1;
    }

    for (i = 0U; i < count; ++i) {
        if (dev_name != 0 && rt_strcmp(dev_name, links[i].name) != 0) {
            continue;
        }
        write_link_header(i, &links[i]);
        matched = 1;
    }

    if (!matched && dev_name != 0) {
        tool_write_error("ip", "unknown device: ", dev_name);
        return 1;
    }
    return 0;
}

static void write_brief_addresses_for_link(
    const PlatformNetworkLink *link,
    const PlatformNetworkAddress *addresses,
    size_t address_count
) {
    size_t name_length = rt_strlen(link->name);
    const char *state = link_state_name(link->flags);
    size_t state_length = rt_strlen(state);
    size_t j;
    int wrote_any = 0;

    rt_write_cstr(1, link->name);
    write_padding(16U, name_length);
    rt_write_cstr(1, state);
    write_padding(10U, state_length);

    for (j = 0U; j < address_count; ++j) {
        if (rt_strcmp(addresses[j].ifname, link->name) != 0) {
            continue;
        }
        if (wrote_any) {
            rt_write_char(1, ' ');
        }
        rt_write_cstr(1, addresses[j].address);
        rt_write_char(1, '/');
        rt_write_uint(1, (unsigned long long)addresses[j].prefix_length);
        wrote_any = 1;
    }

    if (!wrote_any) {
        rt_write_cstr(1, "-");
    }
    rt_write_char(1, '\n');
}

static int show_addresses(const char *dev_name, int family_filter, int brief_mode) {
    PlatformNetworkLink links[IP_MAX_LINKS];
    PlatformNetworkAddress addresses[IP_MAX_ADDRESSES];
    size_t link_count = 0U;
    size_t address_count = 0U;
    size_t i;
    size_t j;
    int matched = 0;

    if (platform_list_network_links(links, IP_MAX_LINKS, &link_count) != 0 ||
        platform_list_network_addresses(addresses, IP_MAX_ADDRESSES, &address_count, family_filter, dev_name) != 0) {
        tool_write_error("ip", "cannot inspect interface addresses", 0);
        return 1;
    }

    for (i = 0U; i < link_count; ++i) {
        if (dev_name != 0 && rt_strcmp(dev_name, links[i].name) != 0) {
            continue;
        }

        if (brief_mode) {
            write_brief_addresses_for_link(&links[i], addresses, address_count);
        } else {
            write_link_header(i, &links[i]);
            for (j = 0U; j < address_count; ++j) {
                if (rt_strcmp(addresses[j].ifname, links[i].name) != 0) {
                    continue;
                }
                rt_write_cstr(1, "    ");
                rt_write_cstr(1, addresses[j].family == PLATFORM_NETWORK_FAMILY_IPV6 ? "inet6 " : "inet ");
                rt_write_cstr(1, addresses[j].address);
                rt_write_char(1, '/');
                rt_write_uint(1, (unsigned long long)addresses[j].prefix_length);
                if (addresses[j].has_broadcast) {
                    rt_write_cstr(1, " brd ");
                    rt_write_cstr(1, addresses[j].broadcast);
                }
                rt_write_cstr(1, " scope ");
                rt_write_cstr(1, addresses[j].scope);
                rt_write_cstr(1, " ");
                rt_write_line(1, addresses[j].ifname);
            }
        }
        matched = 1;
    }

    if (!matched && dev_name != 0) {
        tool_write_error("ip", "unknown device: ", dev_name);
        return 1;
    }
    return 0;
}

static int route_matches_filter(const PlatformRouteEntry *route, const char *filter) {
    char destination[PLATFORM_NETWORK_TEXT_CAPACITY + 16];

    if (filter == 0 || filter[0] == '\0') {
        return 1;
    }
    if (route->is_default) {
        return streq(filter, "default");
    }
    if (streq(filter, route->destination)) {
        return 1;
    }

    destination[0] = '\0';
    rt_copy_string(destination, sizeof(destination), route->destination);
    if (!contains_char(route->destination, '/') && route->prefix_length != 0U) {
        if (append_text(destination, sizeof(destination), "/") != 0 ||
            append_uint_text(destination, sizeof(destination), (unsigned long long)route->prefix_length) != 0) {
            return 0;
        }
    }
    return streq(filter, destination);
}

static int show_routes(const char *dev_name, int family_filter, const char *route_filter) {
    PlatformRouteEntry routes[IP_MAX_ROUTES];
    size_t count = 0U;
    size_t i;
    int matched = 0;

    if (platform_list_network_routes(routes, IP_MAX_ROUTES, &count, family_filter, dev_name) != 0) {
        tool_write_error("ip", "route listing is not available on this platform", 0);
        return 1;
    }

    for (i = 0U; i < count; ++i) {
        if (!route_matches_filter(&routes[i], route_filter)) {
            continue;
        }
        if (routes[i].is_default) {
            rt_write_cstr(1, "default");
        } else {
            rt_write_cstr(1, routes[i].destination);
            if (!contains_char(routes[i].destination, '/') && routes[i].prefix_length != 0U) {
                rt_write_char(1, '/');
                rt_write_uint(1, (unsigned long long)routes[i].prefix_length);
            }
        }

        if (routes[i].has_gateway) {
            rt_write_cstr(1, " via ");
            rt_write_cstr(1, routes[i].gateway);
        }

        rt_write_cstr(1, " dev ");
        rt_write_cstr(1, routes[i].ifname);
        if (routes[i].metric != 0U) {
            rt_write_cstr(1, " metric ");
            rt_write_uint(1, (unsigned long long)routes[i].metric);
        }
        rt_write_char(1, '\n');
        matched = 1;
    }

    if (!matched && dev_name != 0) {
        tool_write_error("ip", "unknown device: ", dev_name);
        return 1;
    }

    return 0;
}

static int flush_addresses(const char *dev_name, int family_filter) {
    PlatformNetworkAddress addresses[IP_MAX_ADDRESSES];
    size_t address_count = 0U;
    size_t i;
    int removed_any = 0;

    if (dev_name == 0) {
        tool_write_error("ip", "flush requires a device name", 0);
        return 1;
    }
    if (family_filter == PLATFORM_NETWORK_FAMILY_IPV6) {
        tool_write_error("ip", "IPv6 address changes are not yet implemented", 0);
        return 1;
    }
    if (platform_list_network_addresses(addresses, IP_MAX_ADDRESSES, &address_count, family_filter, dev_name) != 0) {
        tool_write_error("ip", "cannot inspect interface addresses", 0);
        return 1;
    }

    for (i = 0U; i < address_count; ++i) {
        char cidr[PLATFORM_NETWORK_TEXT_CAPACITY + 16];

        if (rt_strcmp(addresses[i].ifname, dev_name) != 0 || contains_char(addresses[i].address, ':')) {
            continue;
        }
        rt_copy_string(cidr, sizeof(cidr), addresses[i].address);
        if (append_text(cidr, sizeof(cidr), "/") != 0 ||
            append_uint_text(cidr, sizeof(cidr), (unsigned long long)addresses[i].prefix_length) != 0) {
            tool_write_error("ip", "address text is too long for ", dev_name);
            return 1;
        }
        if (platform_network_address_change(dev_name, cidr, 0) != 0) {
            tool_write_error("ip", "cannot delete address on ", dev_name);
            return 1;
        }
        removed_any = 1;
    }

    if (!removed_any) {
        return 0;
    }
    return 0;
}

static int parse_optional_dev_name(int argc, char **argv, int *argi_in_out, const char **dev_name_out, const char *context) {
    int argi = *argi_in_out;

    if (argi >= argc) {
        return 0;
    }
    if (streq(argv[argi], "dev")) {
        if (argi + 1 >= argc) {
            tool_write_error("ip", "missing device name after dev in ", context);
            return 1;
        }
        *dev_name_out = argv[argi + 1];
        *argi_in_out = argi + 2;
        return 0;
    }
    *dev_name_out = argv[argi];
    *argi_in_out = argi + 1;
    return 0;
}

static int handle_address_command(int argc, char **argv, int argi, int family_filter, int brief_mode) {
    const char *command = 0;
    const char *dev_name = 0;
    const char *cidr = 0;
    int add = 1;

    if (argi >= argc) {
        return show_addresses(0, family_filter, brief_mode);
    }

    command = argv[argi];
    if (streq(command, "show") || streq(command, "list")) {
        argi += 1;
        if (argi < argc) {
            if (parse_optional_dev_name(argc, argv, &argi, &dev_name, "address show") != 0) {
                return 1;
            }
        }
        if (argi != argc) {
            tool_write_error("ip", "unexpected address argument: ", argv[argi]);
            return 1;
        }
        return show_addresses(dev_name, family_filter, brief_mode);
    }

    if (streq(command, "flush")) {
        argi += 1;
        if (argi < argc && parse_optional_dev_name(argc, argv, &argi, &dev_name, "address flush") != 0) {
            return 1;
        }
        if (argi != argc) {
            tool_write_error("ip", "unexpected address argument: ", argv[argi]);
            return 1;
        }
        return flush_addresses(dev_name, family_filter);
    }

    if (!(streq(command, "add") || streq(command, "replace") || streq(command, "del") || streq(command, "delete"))) {
        tool_write_error("ip", "unknown address action: ", command);
        return 1;
    }

    add = !(streq(command, "del") || streq(command, "delete"));
    argi += 1;
    if (argi >= argc) {
        print_help(argv[0]);
        return 1;
    }
    cidr = argv[argi++];

    while (argi < argc) {
        if (streq(argv[argi], "dev") && argi + 1 < argc) {
            dev_name = argv[argi + 1];
            argi += 2;
        } else {
            tool_write_error("ip", "unexpected address argument: ", argv[argi]);
            return 1;
        }
    }

    if (dev_name == 0) {
        tool_write_error("ip", "missing required device name", 0);
        return 1;
    }
    if (contains_char(cidr, ':') || family_filter == PLATFORM_NETWORK_FAMILY_IPV6) {
        tool_write_error("ip", "IPv6 address changes are not yet implemented", 0);
        return 1;
    }
    if (platform_network_address_change(dev_name, cidr, add) != 0) {
        tool_write_error("ip", add ? "cannot change address on " : "cannot delete address on ", dev_name);
        return 1;
    }
    return 0;
}

static int handle_link_command(int argc, char **argv, int argi, int brief_mode) {
    const char *command = 0;
    const char *dev_name = 0;
    unsigned long long mtu_value = 0ULL;
    int want_up = -1;
    int set_mtu = 0;

    if (argi >= argc) {
        return brief_mode ? show_links_brief(0) : show_links(0);
    }

    command = argv[argi];
    if (streq(command, "show") || streq(command, "list")) {
        argi += 1;
        if (argi < argc) {
            if (parse_optional_dev_name(argc, argv, &argi, &dev_name, "link show") != 0) {
                return 1;
            }
        }
        if (argi != argc) {
            tool_write_error("ip", "unexpected link argument: ", argv[argi]);
            return 1;
        }
        return brief_mode ? show_links_brief(dev_name) : show_links(dev_name);
    }

    if (!streq(command, "set")) {
        tool_write_error("ip", "unknown link action: ", command);
        return 1;
    }
    argi += 1;

    while (argi < argc) {
        if (streq(argv[argi], "dev") && argi + 1 < argc) {
            dev_name = argv[argi + 1];
            argi += 2;
        } else if (streq(argv[argi], "up")) {
            want_up = 1;
            argi += 1;
        } else if (streq(argv[argi], "down")) {
            want_up = 0;
            argi += 1;
        } else if (streq(argv[argi], "mtu") && argi + 1 < argc) {
            if (tool_parse_uint_arg(argv[argi + 1], &mtu_value, "ip", "mtu") != 0 ||
                mtu_value == 0ULL || mtu_value > 65535ULL) {
                return 1;
            }
            set_mtu = 1;
            argi += 2;
        } else {
            tool_write_error("ip", "unexpected link argument: ", argv[argi]);
            return 1;
        }
    }

    if (dev_name == 0) {
        tool_write_error("ip", "missing required device name", 0);
        return 1;
    }
    if (want_up < 0 && !set_mtu) {
        tool_write_error("ip", "nothing to change; specify up, down, or mtu", 0);
        return 1;
    }
    if (platform_network_link_set(dev_name, want_up, (unsigned int)mtu_value, set_mtu) != 0) {
        tool_write_error("ip", "cannot change link settings on ", dev_name);
        return 1;
    }
    return 0;
}

static int handle_route_command(int argc, char **argv, int argi, int family_filter) {
    const char *command = 0;
    const char *dev_name = 0;
    const char *route_filter = 0;
    const char *destination = 0;
    const char *gateway = 0;
    int add = 1;

    if (argi >= argc) {
        return show_routes(0, family_filter, 0);
    }

    command = argv[argi];
    if (streq(command, "show") || streq(command, "list")) {
        argi += 1;
        while (argi < argc) {
            if (streq(argv[argi], "dev") && argi + 1 < argc) {
                dev_name = argv[argi + 1];
                argi += 2;
            } else if (route_filter == 0 &&
                       (streq(argv[argi], "default") ||
                        contains_char(argv[argi], '/') ||
                        contains_char(argv[argi], '.') ||
                        contains_char(argv[argi], ':'))) {
                route_filter = argv[argi];
                argi += 1;
            } else if (dev_name == 0) {
                dev_name = argv[argi];
                argi += 1;
            } else {
                tool_write_error("ip", "unexpected route argument: ", argv[argi]);
                return 1;
            }
        }
        return show_routes(dev_name, family_filter, route_filter);
    }

    if (!(streq(command, "add") || streq(command, "del") || streq(command, "delete"))) {
        tool_write_error("ip", "unknown route action: ", command);
        return 1;
    }

    add = !(streq(command, "del") || streq(command, "delete"));
    argi += 1;
    if (argi >= argc) {
        print_help(argv[0]);
        return 1;
    }
    destination = argv[argi++];

    while (argi < argc) {
        if (streq(argv[argi], "via") && argi + 1 < argc) {
            gateway = argv[argi + 1];
            argi += 2;
        } else if (streq(argv[argi], "dev") && argi + 1 < argc) {
            dev_name = argv[argi + 1];
            argi += 2;
        } else {
            tool_write_error("ip", "unexpected route argument: ", argv[argi]);
            return 1;
        }
    }

    if (family_filter == PLATFORM_NETWORK_FAMILY_IPV6 ||
        contains_char(destination, ':') ||
        (gateway != 0 && contains_char(gateway, ':'))) {
        tool_write_error("ip", "IPv6 route changes are not yet implemented", 0);
        return 1;
    }
    if (platform_network_route_change(destination, gateway, dev_name, add) != 0) {
        tool_write_error("ip", add ? "cannot add route " : "cannot delete route ", destination);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int argi = 1;
    int family_filter = PLATFORM_NETWORK_FAMILY_ANY;
    int brief_mode = 0;

    while (argi < argc && argv[argi][0] == '-') {
        if (streq(argv[argi], "-4")) {
            family_filter = PLATFORM_NETWORK_FAMILY_IPV4;
        } else if (streq(argv[argi], "-6")) {
            family_filter = PLATFORM_NETWORK_FAMILY_IPV6;
        } else if (streq(argv[argi], "-br") || streq(argv[argi], "-brief") ||
                   streq(argv[argi], "-o") || streq(argv[argi], "--oneline")) {
            brief_mode = 1;
        } else if (streq(argv[argi], "-h") || streq(argv[argi], "--help")) {
            print_help(argv[0]);
            return 0;
        } else {
            tool_write_error("ip", "unknown option: ", argv[argi]);
            print_help(argv[0]);
            return 1;
        }
        argi += 1;
    }

    if (argi >= argc) {
        return show_addresses(0, family_filter, brief_mode);
    }

    if (is_object_name(argv[argi], "address", "addr", "a")) {
        return handle_address_command(argc, argv, argi + 1, family_filter, brief_mode);
    }
    if (is_object_name(argv[argi], "link", "link", "l")) {
        return handle_link_command(argc, argv, argi + 1, brief_mode);
    }
    if (is_object_name(argv[argi], "route", "route", "r")) {
        return handle_route_command(argc, argv, argi + 1, family_filter);
    }

    tool_write_error("ip", "unknown object: ", argv[argi]);
    print_help(argv[0]);
    return 1;
}
