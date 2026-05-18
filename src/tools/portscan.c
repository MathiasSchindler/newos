#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PORTSCAN_HOST_SIZE 256U

typedef struct {
    int family;
    int numeric_only;
    int show_all;
    int show_services;
    int show_summary;
    int csv_output;
    int use_common_ports;
    int fail_open;
    int fail_closed;
    unsigned int timeout_milliseconds;
    unsigned int delay_milliseconds;
    unsigned int scanned_count;
    unsigned int open_count;
    unsigned int closed_count;
} PortscanOptions;

typedef struct {
    unsigned int port;
    const char *name;
} PortscanService;

static const char *common_port_spec = "20-23,25,53,67-69,80,110,123,135,137-139,143,161,162,389,443,445,465,587,636,853,993,995,1433,1521,2049,2375,2376,3000,3306,3389,5000,5432,5900,6379,8000,8080,8443,9200,9300";

static const PortscanService known_services[] = {
    {20U, "ftp-data"}, {21U, "ftp"},       {22U, "ssh"},       {23U, "telnet"},
    {25U, "smtp"},     {53U, "domain"},    {67U, "dhcp"},      {68U, "dhcp"},
    {69U, "tftp"},     {80U, "http"},      {110U, "pop3"},     {123U, "ntp"},
    {135U, "msrpc"},   {137U, "netbios"},  {138U, "netbios"},  {139U, "netbios"},
    {143U, "imap"},    {161U, "snmp"},     {162U, "snmptrap"}, {389U, "ldap"},
    {443U, "https"},   {445U, "microsoft-ds"}, {465U, "smtps"}, {587U, "submission"},
    {636U, "ldaps"},   {853U, "dot"},      {993U, "imaps"},    {995U, "pop3s"},
    {1433U, "mssql"},  {1521U, "oracle"},  {2049U, "nfs"},     {2375U, "docker"},
    {2376U, "docker-tls"}, {3000U, "dev"}, {3306U, "mysql"},   {3389U, "rdp"},
    {5000U, "upnp"},   {5432U, "postgresql"}, {5900U, "vnc"},  {6379U, "redis"},
    {8000U, "http-alt"}, {8080U, "http-alt"}, {8443U, "https-alt"},
    {9200U, "elasticsearch"}, {9300U, "elasticsearch"}
};

static int streq(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-46an] [-w TIMEOUT] [--common] [--services] [--summary] HOSTS [PORTS...]");
}

static void print_help(const char *program_name) {
    print_usage(program_name);
    rt_write_line(1, "Check TCP ports on authorized hosts using normal connect attempts.");
    rt_write_line(1, "");
    rt_write_line(1, "Options:");
    rt_write_line(1, "  -4           force IPv4");
    rt_write_line(1, "  -6           force IPv6 where the platform supports it");
    rt_write_line(1, "  -a           show closed ports as well as open ports");
    rt_write_line(1, "  -n           numeric host names only; skip name resolution");
    rt_write_line(1, "  -w TIMEOUT   connection timeout (ms, s, or m suffixes; default 1s)");
    rt_write_line(1, "  --common     use common-port scanning for admin/service ports");
    rt_write_line(1, "  --delay TIME wait between connect attempts");
    rt_write_line(1, "  --services   show well-known service-name hints for common ports");
    rt_write_line(1, "  --summary    print scanned/open/closed totals after the scan");
    rt_write_line(1, "  --csv        write CSV rows: host,port,state,service");
    rt_write_line(1, "  --fail-open  exit non-zero when any open port is found");
    rt_write_line(1, "  --fail-closed exit non-zero when any closed port is found");
    rt_write_line(1, "");
    rt_write_line(1, "Hosts may be comma-separated or an IPv4 last-octet range such as 192.0.2.1-5.");
    rt_write_line(1, "Ports may be listed individually, comma-separated, or as ranges such as 22,80,8000-8010.");
}

static const char *service_name_for_port(unsigned int port) {
    size_t index = 0U;

    while (index < sizeof(known_services) / sizeof(known_services[0])) {
        if (known_services[index].port == port) {
            return known_services[index].name;
        }
        ++index;
    }
    return "";
}

static void write_csv_field(const char *text) {
    int needs_quotes = 0;
    size_t index = 0U;

    while (text[index] != '\0') {
        if (text[index] == ',' || text[index] == '"' || text[index] == '\n' || text[index] == '\r') {
            needs_quotes = 1;
            break;
        }
        ++index;
    }
    if (!needs_quotes) {
        rt_write_cstr(1, text);
        return;
    }
    rt_write_char(1, '"');
    index = 0U;
    while (text[index] != '\0') {
        if (text[index] == '"') {
            rt_write_char(1, '"');
        }
        rt_write_char(1, text[index]);
        ++index;
    }
    rt_write_char(1, '"');
}

static int append_char(char *buffer, size_t buffer_size, size_t *length_io, char ch) {
    if (*length_io + 1U >= buffer_size) {
        return -1;
    }
    buffer[*length_io] = ch;
    *length_io += 1U;
    buffer[*length_io] = '\0';
    return 0;
}

static int append_uint(char *buffer, size_t buffer_size, size_t *length_io, unsigned int value) {
    char text[16];
    size_t index = 0U;

    rt_unsigned_to_string((unsigned long long)value, text, sizeof(text));
    while (text[index] != '\0') {
        if (append_char(buffer, buffer_size, length_io, text[index]) != 0) {
            return -1;
        }
        ++index;
    }
    return 0;
}

static int copy_slice(char *buffer, size_t buffer_size, const char *start, size_t length) {
    size_t index = 0U;

    if (length + 1U > buffer_size) {
        return -1;
    }
    while (index < length) {
        buffer[index] = start[index];
        ++index;
    }
    buffer[index] = '\0';
    return 0;
}

static int parse_uint_slice(const char *start, size_t length, unsigned int *value_out) {
    size_t index = 0U;
    unsigned int value = 0U;

    if (length == 0U) {
        return -1;
    }
    while (index < length) {
        if (start[index] < '0' || start[index] > '9') {
            return -1;
        }
        if (value > 6553U || (value == 6553U && start[index] > '5')) {
            return -1;
        }
        value = value * 10U + (unsigned int)(start[index] - '0');
        ++index;
    }
    *value_out = value;
    return 0;
}

static int find_last_char(const char *text, size_t length, char needle, size_t *index_out) {
    size_t index = length;

    while (index > 0U) {
        --index;
        if (text[index] == needle) {
            *index_out = index;
            return 0;
        }
    }
    return -1;
}

static int host_prefix_looks_ipv4(const char *text, size_t length) {
    size_t index = 0U;
    unsigned int dots = 0U;

    while (index < length) {
        if (text[index] == '.') {
            dots += 1U;
        } else if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
        ++index;
    }
    return dots == 3U;
}

static void print_result(const char *host, unsigned int port, int is_open, const PortscanOptions *options) {
    const char *service = service_name_for_port(port);

    if (options->csv_output) {
        write_csv_field(host);
        rt_write_char(1, ',');
        rt_write_uint(1, (unsigned long long)port);
        rt_write_char(1, ',');
        rt_write_cstr(1, is_open ? "open" : "closed");
        rt_write_char(1, ',');
        write_csv_field(service);
        rt_write_char(1, '\n');
        return;
    }

    rt_write_cstr(1, host);
    rt_write_char(1, ' ');
    rt_write_uint(1, (unsigned long long)port);
    rt_write_char(1, ' ');
    rt_write_cstr(1, is_open ? "open" : "closed");
    if (options->show_services && service[0] != '\0') {
        rt_write_char(1, ' ');
        rt_write_cstr(1, service);
    }
    rt_write_char(1, '\n');
}

static void scan_one(const char *host, unsigned int port, PortscanOptions *options) {
    PlatformNetcatOptions netcat_options;
    int is_open;

    rt_memset(&netcat_options, 0, sizeof(netcat_options));
    netcat_options.scan_mode = 1;
    netcat_options.family = options->family;
    netcat_options.numeric_only = options->numeric_only;
    netcat_options.timeout_milliseconds = options->timeout_milliseconds;

    is_open = platform_netcat(host, port, &netcat_options) == 0;
    options->scanned_count += 1U;
    if (is_open) {
        options->open_count += 1U;
    } else {
        options->closed_count += 1U;
    }
    if (is_open || options->show_all) {
        print_result(host, port, is_open, options);
    }
    if (options->delay_milliseconds > 0U) {
        (void)platform_sleep_milliseconds((unsigned long long)options->delay_milliseconds);
    }
}

static int scan_host_ports(const char *host, const char *port_spec, PortscanOptions *options) {
    size_t spec_length = rt_strlen(port_spec);
    size_t start = 0U;

    while (start <= spec_length) {
        size_t end = start;
        size_t dash = 0U;
        unsigned int first = 0U;
        unsigned int last = 0U;

        while (end < spec_length && port_spec[end] != ',') {
            ++end;
        }
        if (find_last_char(port_spec + start, end - start, '-', &dash) == 0) {
            if (parse_uint_slice(port_spec + start, dash, &first) != 0 ||
                parse_uint_slice(port_spec + start + dash + 1U, end - start - dash - 1U, &last) != 0) {
                return -1;
            }
        } else if (parse_uint_slice(port_spec + start, end - start, &first) == 0) {
            last = first;
        } else {
            return -1;
        }
        if (first == 0U || last == 0U || first > 65535U || last > 65535U || first > last) {
            return -1;
        }
        while (first <= last) {
            scan_one(host, first, options);
            if (first == 65535U) {
                break;
            }
            ++first;
        }
        if (end == spec_length) {
            break;
        }
        start = end + 1U;
    }
    return 0;
}

static int scan_host_with_all_ports(const char *host, int port_arg_start, int argc, char **argv, PortscanOptions *options) {
    int index = port_arg_start;

    while (index < argc) {
        if (scan_host_ports(host, argv[index], options) != 0) {
            tool_write_error("portscan", "invalid port list: ", argv[index]);
            return -1;
        }
        ++index;
    }
    if (options->use_common_ports && scan_host_ports(host, common_port_spec, options) != 0) {
        tool_write_error("portscan", "invalid common port list", 0);
        return -1;
    }
    return 0;
}

static void print_summary(const PortscanOptions *options) {
    rt_write_cstr(1, "summary scanned=");
    rt_write_uint(1, (unsigned long long)options->scanned_count);
    rt_write_cstr(1, " open=");
    rt_write_uint(1, (unsigned long long)options->open_count);
    rt_write_cstr(1, " closed=");
    rt_write_uint(1, (unsigned long long)options->closed_count);
    rt_write_char(1, '\n');
}

static int scan_host_token(const char *token, size_t token_length, int port_arg_start, int argc, char **argv, PortscanOptions *options) {
    char host[PORTSCAN_HOST_SIZE];
    size_t last_dot = 0U;
    size_t dash = 0U;
    unsigned int first = 0U;
    unsigned int last = 0U;

    if (token_length == 0U) {
        return -1;
    }

    if (find_last_char(token, token_length, '.', &last_dot) == 0 && find_last_char(token, token_length, '-', &dash) == 0 &&
        dash > last_dot && host_prefix_looks_ipv4(token, last_dot + 1U) &&
        parse_uint_slice(token + last_dot + 1U, dash - last_dot - 1U, &first) == 0 &&
        parse_uint_slice(token + dash + 1U, token_length - dash - 1U, &last) == 0 && first <= last && last <= 255U) {
        while (first <= last) {
            size_t length = 0U;
            if (copy_slice(host, sizeof(host), token, last_dot + 1U) != 0) {
                return -1;
            }
            length = rt_strlen(host);
            if (append_uint(host, sizeof(host), &length, first) != 0) {
                return -1;
            }
            if (scan_host_with_all_ports(host, port_arg_start, argc, argv, options) != 0) {
                return -1;
            }
            if (first == 255U) {
                break;
            }
            ++first;
        }
        return 0;
    }

    if (copy_slice(host, sizeof(host), token, token_length) != 0) {
        return -1;
    }
    return scan_host_with_all_ports(host, port_arg_start, argc, argv, options);
}

static int scan_hosts(const char *host_spec, int port_arg_start, int argc, char **argv, PortscanOptions *options) {
    size_t spec_length = rt_strlen(host_spec);
    size_t start = 0U;

    while (start <= spec_length) {
        size_t end = start;
        while (end < spec_length && host_spec[end] != ',') {
            ++end;
        }
        if (scan_host_token(host_spec + start, end - start, port_arg_start, argc, argv, options) != 0) {
            tool_write_error("portscan", "invalid host list: ", host_spec);
            return -1;
        }
        if (end == spec_length) {
            break;
        }
        start = end + 1U;
    }
    return 0;
}

int main(int argc, char **argv) {
    PortscanOptions options;
    int argi = 1;

    rt_memset(&options, 0, sizeof(options));
    options.family = PLATFORM_NETWORK_FAMILY_ANY;
    options.timeout_milliseconds = 1000U;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (streq(argv[argi], "-4")) {
            options.family = PLATFORM_NETWORK_FAMILY_IPV4;
        } else if (streq(argv[argi], "-6")) {
            options.family = PLATFORM_NETWORK_FAMILY_IPV6;
        } else if (streq(argv[argi], "-a")) {
            options.show_all = 1;
        } else if (streq(argv[argi], "-n")) {
            options.numeric_only = 1;
        } else if (streq(argv[argi], "-w")) {
            unsigned long long timeout_value = 0ULL;
            if (argi + 1 >= argc || tool_parse_duration_ms(argv[argi + 1], &timeout_value) != 0 || timeout_value > 0xffffffffULL) {
                print_usage(argv[0]);
                return 1;
            }
            options.timeout_milliseconds = (unsigned int)timeout_value;
            argi += 1;
        } else if (streq(argv[argi], "--common")) {
            options.use_common_ports = 1;
        } else if (streq(argv[argi], "--delay")) {
            unsigned long long delay_value = 0ULL;
            if (argi + 1 >= argc || tool_parse_duration_ms(argv[argi + 1], &delay_value) != 0 || delay_value > 0xffffffffULL) {
                print_usage(argv[0]);
                return 1;
            }
            options.delay_milliseconds = (unsigned int)delay_value;
            argi += 1;
        } else if (streq(argv[argi], "--services")) {
            options.show_services = 1;
        } else if (streq(argv[argi], "--summary")) {
            options.show_summary = 1;
        } else if (streq(argv[argi], "--csv")) {
            options.csv_output = 1;
        } else if (streq(argv[argi], "--fail-open")) {
            options.fail_open = 1;
        } else if (streq(argv[argi], "--fail-closed")) {
            options.fail_closed = 1;
        } else if (streq(argv[argi], "-h") || streq(argv[argi], "--help")) {
            print_help(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 1;
        }
        argi += 1;
    }

    if (argc - argi < 1 || (argc - argi < 2 && !options.use_common_ports)) {
        print_usage(argv[0]);
        return 1;
    }
    if (options.csv_output) {
        rt_write_line(1, "host,port,state,service");
    }
    if (scan_hosts(argv[argi], argi + 1, argc, argv, &options) != 0) {
        return 1;
    }
    if (options.show_summary) {
        print_summary(&options);
    }
    if (options.fail_open && options.open_count > 0U) {
        return 2;
    }
    if (options.fail_closed && options.closed_count > 0U) {
        return 3;
    }
    return 0;
}