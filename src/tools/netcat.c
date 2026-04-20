#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static int streq(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static void print_usage(const char *program_name) {
    tool_write_usage(
        program_name,
        "[-46nuvz] [-w TIMEOUT] [-s ADDR] [-p PORT] [-v] HOST PORT | "
        "-l [-46kuv] [-w TIMEOUT] [-s ADDR] [-p PORT] [ADDR] PORT"
    );
}

static void print_help(const char *program_name) {
    print_usage(program_name);
    rt_write_line(1, "Create TCP or UDP connections and simple listeners.");
    rt_write_line(1, "");
    rt_write_line(1, "Options:");
    rt_write_line(1, "  -4           force IPv4");
    rt_write_line(1, "  -6           force IPv6 where the platform supports it");
    rt_write_line(1, "  -l           listen for an incoming connection");
    rt_write_line(1, "  -k           keep listening for multiple connections or datagrams");
    rt_write_line(1, "  -u           use UDP instead of TCP");
    rt_write_line(1, "  -z           scan mode; connect and close without relaying data");
    rt_write_line(1, "  -n           numeric addresses only; skip name resolution");
    rt_write_line(1, "  -s ADDR      bind the local socket or listener to ADDR");
    rt_write_line(1, "  -p PORT      bind the local socket to PORT");
    rt_write_line(1, "  -w TIMEOUT   connection or idle timeout (ms, s, or m suffixes)");
    rt_write_line(1, "  -v           verbose status output");
}

int main(int argc, char **argv) {
    unsigned long long port = 0;
    unsigned long long bind_port = 0;
    PlatformNetcatOptions options;
    int listen_mode = 0;
    int use_udp = 0;
    int verbose = 0;
    int scan_mode = 0;
    int keep_listening = 0;
    int family = PLATFORM_NETWORK_FAMILY_ANY;
    int numeric_only = 0;
    const char *bind_host = 0;
    int argi = 1;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-') {
        if (streq(argv[argi], "-4")) {
            family = PLATFORM_NETWORK_FAMILY_IPV4;
        } else if (streq(argv[argi], "-6")) {
            family = PLATFORM_NETWORK_FAMILY_IPV6;
        } else if (streq(argv[argi], "-l")) {
            listen_mode = 1;
        } else if (streq(argv[argi], "-k")) {
            keep_listening = 1;
        } else if (streq(argv[argi], "-u")) {
            use_udp = 1;
        } else if (streq(argv[argi], "-z")) {
            scan_mode = 1;
        } else if (streq(argv[argi], "-n")) {
            numeric_only = 1;
        } else if (streq(argv[argi], "-s")) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            bind_host = argv[argi + 1];
            argi += 1;
        } else if (streq(argv[argi], "-p")) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &bind_port, "netcat", "port") != 0 ||
                bind_port == 0 || bind_port > 65535ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 1;
        } else if (streq(argv[argi], "-w")) {
            unsigned long long timeout_value = 0;
            if (argi + 1 >= argc || tool_parse_duration_ms(argv[argi + 1], &timeout_value) != 0 || timeout_value > 0xffffffffULL) {
                print_usage(argv[0]);
                return 1;
            }
            options.timeout_milliseconds = (unsigned int)timeout_value;
            argi += 1;
        } else if (streq(argv[argi], "-v")) {
            verbose = 1;
        } else if (streq(argv[argi], "-h") || streq(argv[argi], "--help")) {
            print_help(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 1;
        }
        argi += 1;
    }

    if (keep_listening && !listen_mode) {
        tool_write_error("netcat", "-k requires listen mode", 0);
        return 1;
    }

    options.listen_mode = listen_mode;
    options.use_udp = use_udp;
    options.scan_mode = scan_mode;
    options.family = family;
    options.numeric_only = numeric_only;
    options.bind_port = (unsigned int)bind_port;
    if (bind_host != 0) {
        rt_copy_string(options.bind_host, sizeof(options.bind_host), bind_host);
    }

    if (listen_mode) {
        int positional_count = argc - argi;
        if (positional_count == 2) {
            if (bind_host == 0) {
                bind_host = argv[argi];
            } else if (!streq(bind_host, argv[argi])) {
                tool_write_error("netcat", "conflicting bind addresses: ", argv[argi]);
                return 1;
            }
            argi += 1;
            positional_count -= 1;
        }
        if (positional_count == 1) {
            if (tool_parse_uint_arg(argv[argi], &port, "netcat", "port") != 0 || port == 0 || port > 65535ULL) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (positional_count == 0 && bind_port != 0ULL) {
            port = bind_port;
        } else {
            print_usage(argv[0]);
            return 1;
        }
        if (bind_port != 0ULL && port != 0ULL && bind_port != port) {
            tool_write_error("netcat", "listen port conflicts with -p value", 0);
            return 1;
        }
        options.bind_port = (unsigned int)port;
        if (bind_host != 0) {
            rt_copy_string(options.bind_host, sizeof(options.bind_host), bind_host);
        }
        if (verbose) {
            rt_write_cstr(2, "netcat: listening on ");
            if (bind_host != 0) {
                rt_write_cstr(2, bind_host);
            } else {
                rt_write_cstr(2, "*");
            }
            rt_write_cstr(2, ":");
            rt_write_uint(2, (unsigned long long)port);
            rt_write_cstr(2, use_udp ? " (udp)" : " (tcp)");
            if (family == PLATFORM_NETWORK_FAMILY_IPV4) {
                rt_write_cstr(2, " ipv4");
            } else if (family == PLATFORM_NETWORK_FAMILY_IPV6) {
                rt_write_cstr(2, " ipv6");
            }
            rt_write_char(2, '\n');
        }

        if (keep_listening) {
            int handled_connections = 0;

            for (;;) {
                if (platform_netcat(bind_host, (unsigned int)port, &options) != 0) {
                    if (handled_connections > 0) {
                        return 0;
                    }
                    tool_write_error("netcat", "listen failed", 0);
                    return 1;
                }
                handled_connections += 1;
            }
        }

        if (platform_netcat(bind_host, (unsigned int)port, &options) != 0) {
            tool_write_error("netcat", "listen failed", 0);
            return 1;
        }
        return 0;
    }

    if (argc - argi == 2) {
        if (tool_parse_uint_arg(argv[argi + 1], &port, "netcat", "port") != 0 || port == 0 || port > 65535ULL) {
            print_usage(argv[0]);
            return 1;
        }
        if (verbose) {
            rt_write_cstr(2, scan_mode ? "netcat: scanning " : "netcat: connecting to ");
            rt_write_cstr(2, argv[argi]);
            rt_write_cstr(2, ":");
            rt_write_uint(2, port);
            rt_write_cstr(2, use_udp ? " (udp)" : " (tcp)");
            if (bind_host != 0 || bind_port != 0ULL) {
                rt_write_cstr(2, " from ");
                if (bind_host != 0) {
                    rt_write_cstr(2, bind_host);
                } else {
                    rt_write_cstr(2, "*");
                }
                if (bind_port != 0ULL) {
                    rt_write_cstr(2, ":");
                    rt_write_uint(2, bind_port);
                }
            }
            if (family == PLATFORM_NETWORK_FAMILY_IPV4) {
                rt_write_cstr(2, " ipv4");
            } else if (family == PLATFORM_NETWORK_FAMILY_IPV6) {
                rt_write_cstr(2, " ipv6");
            }
            rt_write_char(2, '\n');
        }
        if (platform_netcat(argv[argi], (unsigned int)port, &options) != 0) {
            tool_write_error("netcat", scan_mode ? "scan failed for " : "connect failed to ", argv[argi]);
            return 1;
        }
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
