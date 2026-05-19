#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PORTSCAN_HOST_SIZE 256U
#define PORTSCAN_BANNER_MAX 1024U
#define PORTSCAN_BANNER_DEFAULT 256U
#define PORTSCAN_BANNER_TIMEOUT_DEFAULT 500U

typedef struct {
    int family;
    int numeric_only;
    int show_all;
    int show_services;
    int show_summary;
    int show_progress;
    int csv_output;
    int use_common_ports;
    int fail_open;
    int fail_closed;
    int read_banner;
    unsigned int timeout_milliseconds;
    unsigned int delay_milliseconds;
    unsigned int banner_byte_limit;
    unsigned int banner_timeout_milliseconds;
    unsigned int scanned_count;
    unsigned int open_count;
    unsigned int closed_count;
    unsigned int filtered_count;
    unsigned int unreachable_count;
    unsigned int error_count;
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
    tool_write_usage(program_name, "[-46an] [-w TIMEOUT] [--common] [--services] [--summary] [--progress] [--banner] HOSTS [PORTS...]");
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
    rt_write_line(1, "  --progress   print each result as soon as it completes");
    rt_write_line(1, "  --csv        write CSV rows: host,port,state,service");
    rt_write_line(1, "  --fail-open  exit non-zero when any open port is found");
    rt_write_line(1, "  --fail-closed exit non-zero when any closed port is found");
    rt_write_line(1, "  --banner     passively read any banner the service sends on connect");
    rt_write_line(1, "  --banner-bytes N   maximum banner bytes to read (default 256, max 1024)");
    rt_write_line(1, "  --banner-timeout TIME  wait for banner data (default 500ms)");
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

static char hex_digit(unsigned int value) {
    return (char)(value < 10U ? '0' + value : 'a' + (value - 10U));
}

static int append_escaped_byte(char *buffer, size_t buffer_size, size_t *length_io, unsigned char byte) {
    char escape;
    int use_named = 0;

    switch (byte) {
    case 0x00U: escape = '0'; use_named = 1; break;
    case 0x09U: escape = 't'; use_named = 1; break;
    case 0x0AU: escape = 'n'; use_named = 1; break;
    case 0x0DU: escape = 'r'; use_named = 1; break;
    case 0x5CU: escape = '\\'; use_named = 1; break;
    default: escape = '\0'; break;
    }
    if (use_named) {
        if (*length_io + 2U >= buffer_size) {
            return -1;
        }
        buffer[*length_io] = '\\';
        buffer[*length_io + 1U] = escape;
        *length_io += 2U;
        buffer[*length_io] = '\0';
        return 0;
    }
    if (byte >= 0x20U && byte <= 0x7EU) {
        if (*length_io + 1U >= buffer_size) {
            return -1;
        }
        buffer[*length_io] = (char)byte;
        *length_io += 1U;
        buffer[*length_io] = '\0';
        return 0;
    }
    if (*length_io + 4U >= buffer_size) {
        return -1;
    }
    buffer[*length_io] = '\\';
    buffer[*length_io + 1U] = 'x';
    buffer[*length_io + 2U] = hex_digit((unsigned int)(byte >> 4U) & 0x0FU);
    buffer[*length_io + 3U] = hex_digit((unsigned int)byte & 0x0FU);
    *length_io += 4U;
    buffer[*length_io] = '\0';
    return 0;
}

static void escape_banner(const unsigned char *raw, unsigned int raw_length, char *out, size_t out_size) {
    size_t length = 0U;
    unsigned int index = 0U;

    if (out_size == 0U) {
        return;
    }
    out[0] = '\0';
    while (index < raw_length) {
        if (append_escaped_byte(out, out_size, &length, raw[index]) != 0) {
            break;
        }
        ++index;
    }
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

static const char *connect_status_name(int status) {
    switch (status) {
    case PLATFORM_CONNECT_STATUS_OPEN:
        return "open";
    case PLATFORM_CONNECT_STATUS_CLOSED:
        return "closed";
    case PLATFORM_CONNECT_STATUS_FILTERED:
        return "filtered";
    case PLATFORM_CONNECT_STATUS_UNREACHABLE:
        return "unreachable";
    default:
        return "error";
    }
}

static void print_result(const char *host, unsigned int port, int connect_status, const char *banner_text, const PortscanOptions *options) {
    const char *service = service_name_for_port(port);
    const char *status_text = connect_status_name(connect_status);

    if (tool_json_is_enabled()) {
        if (tool_json_begin_event(1, "portscan", "stdout", "port_result") != 0) return;
        rt_write_cstr(1, ",\"data\":{\"host\":");
        tool_json_write_string(1, host);
        rt_write_cstr(1, ",\"port\":");
        rt_write_uint(1, (unsigned long long)port);
        rt_write_cstr(1, ",\"state\":");
        tool_json_write_string(1, status_text);
        rt_write_cstr(1, ",\"service\":");
        if (service[0] != '\0') tool_json_write_string(1, service);
        else rt_write_cstr(1, "null");
        rt_write_cstr(1, ",\"banner\":");
        if (options->read_banner && banner_text != 0 && banner_text[0] != '\0') tool_json_write_string(1, banner_text);
        else rt_write_cstr(1, "null");
        rt_write_char(1, '}');
        tool_json_end_event(1);
        return;
    }

    if (options->csv_output) {
        write_csv_field(host);
        rt_write_char(1, ',');
        rt_write_uint(1, (unsigned long long)port);
        rt_write_char(1, ',');
        rt_write_cstr(1, status_text);
        rt_write_char(1, ',');
        write_csv_field(service);
        if (options->read_banner) {
            rt_write_char(1, ',');
            write_csv_field(banner_text != 0 ? banner_text : "");
        }
        rt_write_char(1, '\n');
        return;
    }

    rt_write_cstr(1, host);
    rt_write_char(1, ' ');
    rt_write_uint(1, (unsigned long long)port);
    rt_write_char(1, ' ');
    rt_write_cstr(1, status_text);
    if (options->show_services && service[0] != '\0') {
        rt_write_char(1, ' ');
        rt_write_cstr(1, service);
    }
    if (options->read_banner && banner_text != 0 && banner_text[0] != '\0') {
        rt_write_char(1, ' ');
        rt_write_cstr(1, banner_text);
    }
    rt_write_char(1, '\n');
}

static void scan_one(const char *host, unsigned int port, PortscanOptions *options) {
    PlatformNetcatOptions netcat_options;
    unsigned char banner_buffer[PORTSCAN_BANNER_MAX];
    char banner_text[PORTSCAN_BANNER_MAX * 4U + 1U];
    unsigned int banner_length = 0U;
    int connect_status = PLATFORM_CONNECT_STATUS_ERROR;
    int is_open;

    rt_memset(&netcat_options, 0, sizeof(netcat_options));
    netcat_options.scan_mode = 1;
    netcat_options.family = options->family;
    netcat_options.numeric_only = options->numeric_only;
    netcat_options.timeout_milliseconds = options->timeout_milliseconds;
    netcat_options.connect_status_out = &connect_status;
    if (options->read_banner) {
        unsigned int capacity = options->banner_byte_limit;
        if (capacity == 0U || capacity > PORTSCAN_BANNER_MAX) {
            capacity = PORTSCAN_BANNER_DEFAULT;
        }
        netcat_options.banner_buffer = banner_buffer;
        netcat_options.banner_capacity = capacity;
        netcat_options.banner_read_timeout_milliseconds = options->banner_timeout_milliseconds;
        netcat_options.banner_received_length = &banner_length;
    }

    is_open = platform_netcat(host, port, &netcat_options) == 0;
    options->scanned_count += 1U;
    if (is_open || connect_status == PLATFORM_CONNECT_STATUS_OPEN) {
        connect_status = PLATFORM_CONNECT_STATUS_OPEN;
        options->open_count += 1U;
    } else if (connect_status == PLATFORM_CONNECT_STATUS_CLOSED) {
        options->closed_count += 1U;
    } else if (connect_status == PLATFORM_CONNECT_STATUS_FILTERED) {
        options->filtered_count += 1U;
    } else if (connect_status == PLATFORM_CONNECT_STATUS_UNREACHABLE) {
        options->unreachable_count += 1U;
    } else {
        options->error_count += 1U;
    }
    banner_text[0] = '\0';
    if (options->read_banner && is_open && banner_length > 0U) {
        escape_banner(banner_buffer, banner_length, banner_text, sizeof(banner_text));
    }
    if (is_open || options->show_all || options->show_progress) {
        print_result(host, port, connect_status, banner_text, options);
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

static int scan_host_with_all_ports(const char *host, int port_count, char **ports, PortscanOptions *options) {
    int index = 0;

    while (index < port_count) {
        if (scan_host_ports(host, ports[index], options) != 0) {
            tool_write_error("portscan", "invalid port list: ", ports[index]);
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
    if (tool_json_is_enabled()) {
        if (tool_json_begin_event(1, "portscan", "stdout", "scan_summary") != 0) return;
        rt_write_cstr(1, ",\"data\":{\"scanned\":");
        rt_write_uint(1, (unsigned long long)options->scanned_count);
        rt_write_cstr(1, ",\"open\":");
        rt_write_uint(1, (unsigned long long)options->open_count);
        rt_write_cstr(1, ",\"closed\":");
        rt_write_uint(1, (unsigned long long)options->closed_count);
        rt_write_cstr(1, ",\"filtered\":");
        rt_write_uint(1, (unsigned long long)options->filtered_count);
        rt_write_cstr(1, ",\"unreachable\":");
        rt_write_uint(1, (unsigned long long)options->unreachable_count);
        rt_write_cstr(1, ",\"error\":");
        rt_write_uint(1, (unsigned long long)options->error_count);
        rt_write_char(1, '}');
        tool_json_end_event(1);
        return;
    }

    rt_write_cstr(1, "summary scanned=");
    rt_write_uint(1, (unsigned long long)options->scanned_count);
    rt_write_cstr(1, " open=");
    rt_write_uint(1, (unsigned long long)options->open_count);
    rt_write_cstr(1, " closed=");
    rt_write_uint(1, (unsigned long long)options->closed_count);
    rt_write_cstr(1, " filtered=");
    rt_write_uint(1, (unsigned long long)options->filtered_count);
    rt_write_cstr(1, " unreachable=");
    rt_write_uint(1, (unsigned long long)options->unreachable_count);
    rt_write_cstr(1, " error=");
    rt_write_uint(1, (unsigned long long)options->error_count);
    rt_write_char(1, '\n');
}

static int scan_host_token(const char *token, size_t token_length, int port_count, char **ports, PortscanOptions *options) {
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
            if (scan_host_with_all_ports(host, port_count, ports, options) != 0) {
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
    return scan_host_with_all_ports(host, port_count, ports, options);
}

static int scan_hosts(const char *host_spec, int port_count, char **ports, PortscanOptions *options) {
    size_t spec_length = rt_strlen(host_spec);
    size_t start = 0U;

    while (start <= spec_length) {
        size_t end = start;
        while (end < spec_length && host_spec[end] != ',') {
            ++end;
        }
        if (scan_host_token(host_spec + start, end - start, port_count, ports, options) != 0) {
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
    char **positionals;
    int positional_count = 0;
    int argi = 1;

    rt_memset(&options, 0, sizeof(options));
    options.family = PLATFORM_NETWORK_FAMILY_ANY;
    options.timeout_milliseconds = 1000U;

    positionals = (char **)rt_malloc(sizeof(char *) * (size_t)(argc > 1 ? argc - 1 : 1));
    if (positionals == 0) {
        tool_write_error("portscan", "out of memory", 0);
        return 1;
    }

    while (argi < argc) {
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
                rt_free(positionals);
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
                rt_free(positionals);
                return 1;
            }
            options.delay_milliseconds = (unsigned int)delay_value;
            argi += 1;
        } else if (streq(argv[argi], "--services")) {
            options.show_services = 1;
        } else if (streq(argv[argi], "--summary")) {
            options.show_summary = 1;
        } else if (streq(argv[argi], "--progress")) {
            options.show_progress = 1;
        } else if (streq(argv[argi], "--csv")) {
            options.csv_output = 1;
        } else if (streq(argv[argi], "--json")) {
            tool_json_set_enabled(1);
        } else if (streq(argv[argi], "--fail-open")) {
            options.fail_open = 1;
        } else if (streq(argv[argi], "--fail-closed")) {
            options.fail_closed = 1;
        } else if (streq(argv[argi], "--banner")) {
            options.read_banner = 1;
        } else if (streq(argv[argi], "--banner-bytes")) {
            unsigned long long value = 0ULL;
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &value, "portscan", "banner byte limit") != 0 ||
                value == 0ULL || value > (unsigned long long)PORTSCAN_BANNER_MAX) {
                print_usage(argv[0]);
                rt_free(positionals);
                return 1;
            }
            options.banner_byte_limit = (unsigned int)value;
            argi += 1;
        } else if (streq(argv[argi], "--banner-timeout")) {
            unsigned long long banner_timeout = 0ULL;
            if (argi + 1 >= argc || tool_parse_duration_ms(argv[argi + 1], &banner_timeout) != 0 || banner_timeout > 0xffffffffULL) {
                print_usage(argv[0]);
                rt_free(positionals);
                return 1;
            }
            options.banner_timeout_milliseconds = (unsigned int)banner_timeout;
            argi += 1;
        } else if (streq(argv[argi], "-h") || streq(argv[argi], "--help")) {
            print_help(argv[0]);
            rt_free(positionals);
            return 0;
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            print_usage(argv[0]);
            rt_free(positionals);
            return 1;
        } else {
            positionals[positional_count] = argv[argi];
            positional_count += 1;
        }
        argi += 1;
    }

    if (positional_count < 1 || (positional_count < 2 && !options.use_common_ports)) {
        print_usage(argv[0]);
        rt_free(positionals);
        return 1;
    }
    if (options.csv_output && tool_json_is_enabled()) {
        tool_write_error("portscan", "--csv and --json are mutually exclusive", 0);
        rt_free(positionals);
        return 1;
    }
    if (options.csv_output) {
        if (options.read_banner) {
            rt_write_line(1, "host,port,state,service,banner");
        } else {
            rt_write_line(1, "host,port,state,service");
        }
    }
    if (scan_hosts(positionals[0], positional_count - 1, positionals + 1, &options) != 0) {
        rt_free(positionals);
        return 1;
    }
    if (options.show_summary) {
        print_summary(&options);
    }
    if (options.fail_open && options.open_count > 0U) {
        rt_free(positionals);
        return 2;
    }
    if (options.fail_closed && options.closed_count > 0U) {
        rt_free(positionals);
        return 3;
    }
    rt_free(positionals);
    return 0;
}