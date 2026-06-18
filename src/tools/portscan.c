#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PORTSCAN_HOST_SIZE 256U
#define PORTSCAN_BANNER_MAX 1024U
#define PORTSCAN_BANNER_DEFAULT 256U
#define PORTSCAN_BANNER_TIMEOUT_DEFAULT 500U
#define PORTSCAN_MAX_BASELINE 4096U
#define PORTSCAN_MAX_JOBS 64U

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
    int show_details;
    int tls_cert;
    int tls_insecure;
    int diff_only;
    int worker_mode;
    unsigned int timeout_milliseconds;
    unsigned int delay_milliseconds;
    unsigned int jobs;
    unsigned int per_host_jobs;
    unsigned int rate_per_second;
    unsigned int banner_byte_limit;
    unsigned int banner_timeout_milliseconds;
    unsigned int scanned_count;
    unsigned int open_count;
    unsigned int closed_count;
    unsigned int filtered_count;
    unsigned int unreachable_count;
    unsigned int error_count;
    unsigned long long started_at;
    unsigned long long finished_at;
    const char *baseline_path;
    const char *program_name;
    const char *profile_specs[8];
    unsigned int profile_count;
} PortscanOptions;

typedef struct {
    char host[PORTSCAN_HOST_SIZE];
    unsigned int port;
    int status;
    unsigned int latency_milliseconds;
    char reason[32];
    char banner[PORTSCAN_BANNER_MAX * 4U + 1U];
    char change[32];
    int has_tls;
    PlatformTlsPeerInfo tls;
} PortscanResult;

typedef struct {
    char host[PORTSCAN_HOST_SIZE];
    unsigned int port;
    char state[16];
    int seen;
} PortscanBaselineEntry;

typedef struct {
    PortscanBaselineEntry entries[PORTSCAN_MAX_BASELINE];
    size_t count;
} PortscanBaseline;

typedef struct {
    int pid;
    int fd;
} PortscanChild;

static PortscanChild active_children[PORTSCAN_MAX_JOBS];
static unsigned int active_child_count = 0U;

static const char common_port_spec[] = "20-23,25,53,67-69,80,110,123,135,137-139,143,161,162,389,443,445,465,587,636,853,993,995,1433,1521,2049,2375,2376,3000,3306,3389,5000,5432,5900,6379,8000,8080,8443,9200,9300";
static const char admin_port_spec[] = "22,23,53,80,443,445,3389,5900,8000,8080,8443";
static const char database_port_spec[] = "1433,1521,3306,5432,6379,9200,9300";
static const char windows_port_spec[] = "135,137-139,445,3389,5985,5986";
static const char web_port_spec[] = "80,443,8000,8080,8443,9000,9443";
static const char risky_port_spec[] = "21-23,135,139,445,1433,1521,2375,2376,3306,3389,5432,5900,6379,9200,9300";


static const char *profile_port_spec(const char *name) {
    if (tool_str_equal(name, "admin")) return admin_port_spec;
    if (tool_str_equal(name, "databases") || tool_str_equal(name, "database")) return database_port_spec;
    if (tool_str_equal(name, "windows")) return windows_port_spec;
    if (tool_str_equal(name, "web")) return web_port_spec;
    if (tool_str_equal(name, "risky")) return risky_port_spec;
    if (tool_str_equal(name, "common")) return common_port_spec;
    return 0;
}

static int parse_rate_arg(const char *text, unsigned int *rate_out) {
    char number[32];
    size_t length;
    size_t index;
    unsigned long long value = 0ULL;

    if (text == 0 || rate_out == 0) {
        return -1;
    }
    length = rt_strlen(text);
    if (length > 2U && text[length - 2U] == '/' && text[length - 1U] == 's') {
        length -= 2U;
    }
    if (length == 0U || length >= sizeof(number)) {
        return -1;
    }
    for (index = 0U; index < length; ++index) {
        number[index] = text[index];
    }
    number[length] = '\0';
    if (rt_parse_uint(number, &value) != 0 || value == 0ULL || value > 1000000ULL) {
        return -1;
    }
    *rate_out = (unsigned int)value;
    return 0;
}

static const char *reason_for_status(int status) {
    switch (status) {
    case PLATFORM_CONNECT_STATUS_OPEN: return "connected";
    case PLATFORM_CONNECT_STATUS_CLOSED: return "connection_refused";
    case PLATFORM_CONNECT_STATUS_FILTERED: return "timeout";
    case PLATFORM_CONNECT_STATUS_UNREACHABLE: return "unreachable";
    default: return "error";
    }
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-46an] [-w TIMEOUT] [--common|--profile NAME] [--jobs N] [--per-host N] [--rate N/s] [--services] [--summary] [--progress] [--banner] [--tls-cert] [--baseline FILE] [--diff] HOSTS [PORTS...]");
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
    rt_write_line(1, "  --profile NAME use a named port profile: admin, databases, windows, web, risky");
    rt_write_line(1, "  --jobs N     run up to N scan workers at once (max 64)");
    rt_write_line(1, "  --per-host N cap concurrent workers for each host");
    rt_write_line(1, "  --rate N/s   limit worker starts to about N per second");
    rt_write_line(1, "  --delay TIME wait between connect attempts");
    rt_write_line(1, "  --services   show well-known service-name hints for common ports");
    rt_write_line(1, "  --summary    print scanned/open/closed totals after the scan");
    rt_write_line(1, "  --progress   print each result as soon as it completes");
    rt_write_line(1, "  --csv        write detailed CSV rows");
    rt_write_line(1, "  --details    show latency, reason, baseline change, and TLS metadata when present");
    rt_write_line(1, "  --baseline FILE compare results with a previous text, CSV, or JSON-lines scan");
    rt_write_line(1, "  --diff       with --baseline, print only changed, new, or missing results");
    rt_write_line(1, "  --fail-open  exit non-zero when any open port is found");
    rt_write_line(1, "  --fail-closed exit non-zero when any closed port is found");
    rt_write_line(1, "  --banner     passively read any banner the service sends on connect");
    rt_write_line(1, "  --banner-bytes N   maximum banner bytes to read (default 256, max 1024)");
    rt_write_line(1, "  --banner-timeout TIME  wait for banner data (default 500ms)");
    rt_write_line(1, "  --tls-cert   perform a TLS handshake on open ports and report peer certificate metadata");
    rt_write_line(1, "  --tls-insecure allow --tls-cert to report untrusted/self-signed certificates");
    rt_write_line(1, "");
    rt_write_line(1, "Hosts may be comma-separated or an IPv4 last-octet range such as 192.0.2.1-5.");
    rt_write_line(1, "Ports may be listed individually, comma-separated, or as ranges such as 22,80,8000-8010.");
}

static const char *service_name_for_port(unsigned int port) {
    switch (port) {
    case 20U: return "ftp-data";
    case 21U: return "ftp";
    case 22U: return "ssh";
    case 23U: return "telnet";
    case 25U: return "smtp";
    case 53U: return "domain";
    case 67U: return "dhcp";
    case 68U: return "dhcp";
    case 69U: return "tftp";
    case 80U: return "http";
    case 110U: return "pop3";
    case 123U: return "ntp";
    case 135U: return "msrpc";
    case 137U: return "netbios";
    case 138U: return "netbios";
    case 139U: return "netbios";
    case 143U: return "imap";
    case 161U: return "snmp";
    case 162U: return "snmptrap";
    case 389U: return "ldap";
    case 443U: return "https";
    case 445U: return "microsoft-ds";
    case 465U: return "smtps";
    case 587U: return "submission";
    case 636U: return "ldaps";
    case 853U: return "dot";
    case 993U: return "imaps";
    case 995U: return "pop3s";
    case 1433U: return "mssql";
    case 1521U: return "oracle";
    case 2049U: return "nfs";
    case 2375U: return "docker";
    case 2376U: return "docker-tls";
    case 3000U: return "dev";
    case 3306U: return "mysql";
    case 3389U: return "rdp";
    case 5000U: return "upnp";
    case 5432U: return "postgresql";
    case 5900U: return "vnc";
    case 5985U: return "winrm";
    case 5986U: return "winrm-tls";
    case 6379U: return "redis";
    case 8000U: return "http-alt";
    case 8080U: return "http-alt";
    case 8443U: return "https-alt";
    case 9200U: return "elasticsearch";
    case 9300U: return "elasticsearch";
    default: return "";
    }
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
    buffer[*length_io + 2U] = tool_hex_digit((unsigned int)(byte >> 4U));
    buffer[*length_io + 3U] = tool_hex_digit((unsigned int)byte);
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

static void print_json_nullable_string(const char *text) {
    if (text != 0 && text[0] != '\0') tool_json_write_string(1, text);
    else rt_write_cstr(1, "null");
}

static void print_result(const PortscanResult *result, const PortscanOptions *options) {
    const char *service = service_name_for_port(result->port);
    const char *status_text = connect_status_name(result->status);

    if (tool_json_is_enabled()) {
        if (tool_json_begin_event(1, "portscan", "stdout", "port_result") != 0) return;
        rt_write_cstr(1, ",\"data\":{\"host\":");
        tool_json_write_string(1, result->host);
        rt_write_cstr(1, ",\"port\":");
        rt_write_uint(1, (unsigned long long)result->port);
        rt_write_cstr(1, ",\"state\":");
        tool_json_write_string(1, status_text);
        rt_write_cstr(1, ",\"service\":");
        print_json_nullable_string(service);
        rt_write_cstr(1, ",\"latency_ms\":");
        rt_write_uint(1, (unsigned long long)result->latency_milliseconds);
        rt_write_cstr(1, ",\"reason\":");
        tool_json_write_string(1, result->reason);
        rt_write_cstr(1, ",\"change\":");
        print_json_nullable_string(result->change);
        rt_write_cstr(1, ",\"banner\":");
        print_json_nullable_string(result->banner);
        rt_write_cstr(1, ",\"tls_protocol\":");
        print_json_nullable_string(result->has_tls ? result->tls.protocol : 0);
        rt_write_cstr(1, ",\"tls_cipher\":");
        print_json_nullable_string(result->has_tls ? result->tls.cipher : 0);
        rt_write_cstr(1, ",\"tls_verification\":");
        print_json_nullable_string(result->has_tls ? result->tls.verification : 0);
        rt_write_cstr(1, ",\"tls_subject\":");
        print_json_nullable_string(result->has_tls ? result->tls.subject : 0);
        rt_write_cstr(1, ",\"tls_issuer\":");
        print_json_nullable_string(result->has_tls ? result->tls.issuer : 0);
        rt_write_cstr(1, ",\"tls_dns_names\":");
        print_json_nullable_string(result->has_tls ? result->tls.dns_names : 0);
        rt_write_cstr(1, ",\"tls_not_before\":");
        if (result->has_tls) rt_write_int(1, result->tls.not_before);
        else rt_write_cstr(1, "null");
        rt_write_cstr(1, ",\"tls_not_after\":");
        if (result->has_tls) rt_write_int(1, result->tls.not_after);
        else rt_write_cstr(1, "null");
        rt_write_char(1, '}');
        tool_json_end_event(1);
        return;
    }

    if (options->csv_output) {
        write_csv_field(result->host);
        rt_write_char(1, ',');
        rt_write_uint(1, (unsigned long long)result->port);
        rt_write_char(1, ',');
        rt_write_cstr(1, status_text);
        rt_write_char(1, ',');
        write_csv_field(service);
        rt_write_char(1, ',');
        rt_write_uint(1, (unsigned long long)result->latency_milliseconds);
        rt_write_char(1, ',');
        write_csv_field(result->reason);
        rt_write_char(1, ',');
        write_csv_field(result->change);
        rt_write_char(1, ',');
        write_csv_field(result->banner);
        rt_write_char(1, ',');
        write_csv_field(result->has_tls ? result->tls.protocol : "");
        rt_write_char(1, ',');
        write_csv_field(result->has_tls ? result->tls.cipher : "");
        rt_write_char(1, ',');
        write_csv_field(result->has_tls ? result->tls.verification : "");
        rt_write_char(1, ',');
        write_csv_field(result->has_tls ? result->tls.subject : "");
        rt_write_char(1, ',');
        write_csv_field(result->has_tls ? result->tls.issuer : "");
        rt_write_char(1, ',');
        write_csv_field(result->has_tls ? result->tls.dns_names : "");
        rt_write_char(1, ',');
        if (result->has_tls) rt_write_int(1, result->tls.not_after);
        rt_write_char(1, '\n');
        return;
    }

    rt_write_cstr(1, result->host);
    rt_write_char(1, ' ');
    rt_write_uint(1, (unsigned long long)result->port);
    rt_write_char(1, ' ');
    rt_write_cstr(1, status_text);
    if (options->show_services && service[0] != '\0') {
        rt_write_char(1, ' ');
        rt_write_cstr(1, service);
    }
    if (options->read_banner && result->banner[0] != '\0') {
        rt_write_char(1, ' ');
        rt_write_cstr(1, result->banner);
    }
    if (options->show_details) {
        rt_write_cstr(1, " latency_ms=");
        rt_write_uint(1, (unsigned long long)result->latency_milliseconds);
        rt_write_cstr(1, " reason=");
        rt_write_cstr(1, result->reason);
        if (result->change[0] != '\0') {
            rt_write_cstr(1, " change=");
            rt_write_cstr(1, result->change);
        }
        if (result->has_tls) {
            rt_write_cstr(1, " tls=");
            rt_write_cstr(1, result->tls.protocol);
            rt_write_cstr(1, " cipher=");
            rt_write_cstr(1, result->tls.cipher);
            rt_write_cstr(1, " cert_subject=");
            rt_write_cstr(1, result->tls.subject);
            rt_write_cstr(1, " cert_not_after=");
            rt_write_int(1, result->tls.not_after);
        }
    }
    rt_write_char(1, '\n');
}

static void update_counts(const PortscanResult *result, PortscanOptions *options) {
    options->scanned_count += 1U;
    if (result->status == PLATFORM_CONNECT_STATUS_OPEN) {
        options->open_count += 1U;
    } else if (result->status == PLATFORM_CONNECT_STATUS_CLOSED) {
        options->closed_count += 1U;
    } else if (result->status == PLATFORM_CONNECT_STATUS_FILTERED) {
        options->filtered_count += 1U;
    } else if (result->status == PLATFORM_CONNECT_STATUS_UNREACHABLE) {
        options->unreachable_count += 1U;
    } else {
        options->error_count += 1U;
    }
}

static void apply_baseline_change(PortscanResult *result, PortscanBaseline *baseline) {
    size_t index;
    const char *state = connect_status_name(result->status);

    result->change[0] = '\0';
    if (baseline == 0) {
        return;
    }
    for (index = 0U; index < baseline->count; ++index) {
        if (baseline->entries[index].port == result->port && rt_strcmp(baseline->entries[index].host, result->host) == 0) {
            baseline->entries[index].seen = 1;
            if (rt_strcmp(baseline->entries[index].state, state) == 0) {
                rt_copy_string(result->change, sizeof(result->change), "unchanged");
            } else if (rt_strcmp(baseline->entries[index].state, "open") == 0 && rt_strcmp(state, "open") != 0) {
                rt_copy_string(result->change, sizeof(result->change), "now-closed");
            } else if (rt_strcmp(baseline->entries[index].state, "open") != 0 && rt_strcmp(state, "open") == 0) {
                rt_copy_string(result->change, sizeof(result->change), "new-open");
            } else {
                rt_copy_string(result->change, sizeof(result->change), "changed");
            }
            return;
        }
    }
    rt_copy_string(result->change, sizeof(result->change), rt_strcmp(state, "open") == 0 ? "new-open" : "new-result");
}

static int should_print_result(const PortscanResult *result, const PortscanOptions *options) {
    if (options->diff_only) {
        return result->change[0] != '\0' && rt_strcmp(result->change, "unchanged") != 0;
    }
    return result->status == PLATFORM_CONNECT_STATUS_OPEN || options->show_all || options->show_progress ||
           (result->change[0] != '\0' && rt_strcmp(result->change, "unchanged") != 0);
}

static int scan_one_result(const char *host, unsigned int port, const PortscanOptions *options, PortscanResult *result) {
    PlatformNetcatOptions netcat_options;
    unsigned char banner_buffer[PORTSCAN_BANNER_MAX];
    unsigned int banner_length = 0U;
    int connect_status = PLATFORM_CONNECT_STATUS_ERROR;
    int is_open;
    long long start_seconds;
    long long end_seconds;

    if (result == 0) {
        return -1;
    }
    rt_memset(result, 0, sizeof(*result));
    rt_copy_string(result->host, sizeof(result->host), host);
    result->port = port;
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

    start_seconds = platform_get_epoch_time();
    is_open = platform_netcat(host, port, &netcat_options) == 0;
    end_seconds = platform_get_epoch_time();
    result->latency_milliseconds = end_seconds > start_seconds ? (unsigned int)((end_seconds - start_seconds) * 1000LL) : 0U;
    if (is_open || connect_status == PLATFORM_CONNECT_STATUS_OPEN) {
        connect_status = PLATFORM_CONNECT_STATUS_OPEN;
    }
    result->status = connect_status;
    rt_copy_string(result->reason, sizeof(result->reason), reason_for_status(connect_status));
    if (options->read_banner && is_open && banner_length > 0U) {
        escape_banner(banner_buffer, banner_length, result->banner, sizeof(result->banner));
    }
    if (options->tls_cert && is_open) {
#ifdef PORTSCAN_NO_TLS
        rt_copy_string(result->reason, sizeof(result->reason), "tls_unavailable");
#else
        PlatformTlsClient client;
        if (options->tls_insecure) {
            (void)platform_setenv("NEWOS_NATIVE_TLS_INSECURE", "1", 1);
        }
        if (platform_tls_connect(&client, host, port) == 0) {
            if (platform_tls_peer_info(&client, &result->tls) == 0) {
                result->has_tls = 1;
            }
            platform_tls_close(&client);
        } else {
            rt_copy_string(result->reason, sizeof(result->reason), platform_tls_last_error());
        }
#endif
    }
    return 0;
}

static void write_worker_field(const char *text) {
    rt_write_cstr(1, text != 0 ? text : "");
}

static void print_worker_result(const PortscanResult *result) {
    write_worker_field(result->host);
    rt_write_char(1, '\t');
    rt_write_uint(1, (unsigned long long)result->port);
    rt_write_char(1, '\t');
    rt_write_uint(1, (unsigned long long)result->status);
    rt_write_char(1, '\t');
    rt_write_uint(1, (unsigned long long)result->latency_milliseconds);
    rt_write_char(1, '\t');
    write_worker_field(result->reason);
    rt_write_char(1, '\t');
    write_worker_field(result->banner);
    rt_write_char(1, '\t');
    rt_write_uint(1, (unsigned long long)result->has_tls);
    rt_write_char(1, '\t');
    write_worker_field(result->tls.protocol);
    rt_write_char(1, '\t');
    write_worker_field(result->tls.cipher);
    rt_write_char(1, '\t');
    write_worker_field(result->tls.verification);
    rt_write_char(1, '\t');
    write_worker_field(result->tls.subject);
    rt_write_char(1, '\t');
    write_worker_field(result->tls.issuer);
    rt_write_char(1, '\t');
    write_worker_field(result->tls.dns_names);
    rt_write_char(1, '\t');
    rt_write_int(1, result->tls.not_before);
    rt_write_char(1, '\t');
    rt_write_int(1, result->tls.not_after);
    rt_write_char(1, '\n');
}

static int parse_worker_result(char *line, PortscanResult *result) {
    char *fields[15];
    unsigned int field_count = 0U;
    char *cursor = line;
    unsigned long long value = 0ULL;

    rt_memset(result, 0, sizeof(*result));
    while (field_count < 15U) {
        fields[field_count++] = cursor;
        while (*cursor != '\0' && *cursor != '\t' && *cursor != '\n' && *cursor != '\r') {
            ++cursor;
        }
        if (*cursor == '\0' || *cursor == '\n' || *cursor == '\r') {
            *cursor = '\0';
            break;
        }
        *cursor = '\0';
        ++cursor;
    }
    if (field_count < 7U) {
        return -1;
    }
    rt_copy_string(result->host, sizeof(result->host), fields[0]);
    if (rt_parse_uint(fields[1], &value) != 0) return -1;
    result->port = (unsigned int)value;
    if (rt_parse_uint(fields[2], &value) != 0) return -1;
    result->status = (int)value;
    if (rt_parse_uint(fields[3], &value) != 0) return -1;
    result->latency_milliseconds = (unsigned int)value;
    rt_copy_string(result->reason, sizeof(result->reason), fields[4]);
    rt_copy_string(result->banner, sizeof(result->banner), fields[5]);
    if (rt_parse_uint(fields[6], &value) == 0 && value != 0ULL) {
        result->has_tls = 1;
    }
    if (field_count >= 15U) {
        rt_copy_string(result->tls.protocol, sizeof(result->tls.protocol), fields[7]);
        rt_copy_string(result->tls.cipher, sizeof(result->tls.cipher), fields[8]);
        rt_copy_string(result->tls.verification, sizeof(result->tls.verification), fields[9]);
        rt_copy_string(result->tls.subject, sizeof(result->tls.subject), fields[10]);
        rt_copy_string(result->tls.issuer, sizeof(result->tls.issuer), fields[11]);
        rt_copy_string(result->tls.dns_names, sizeof(result->tls.dns_names), fields[12]);
        result->tls.not_before = 0;
        result->tls.not_after = 0;
        if (rt_parse_uint(fields[13], &value) == 0) result->tls.not_before = (long long)value;
        if (rt_parse_uint(fields[14], &value) == 0) result->tls.not_after = (long long)value;
    }
    return 0;
}

static int reap_child_index(unsigned int index, PortscanOptions *options, PortscanBaseline *baseline) {
    char buffer[8192];
    size_t used = 0U;
    int exit_status = 0;
    PortscanResult result;

    if (index >= active_child_count) {
        return -1;
    }
    while (used + 1U < sizeof(buffer)) {
        long got = platform_read(active_children[index].fd, buffer + used, sizeof(buffer) - used - 1U);
        if (got <= 0) {
            break;
        }
        used += (size_t)got;
    }
    buffer[used] = '\0';
    platform_close(active_children[index].fd);
    (void)platform_wait_process(active_children[index].pid, &exit_status);
    active_children[index] = active_children[active_child_count - 1U];
    active_child_count -= 1U;
    if (exit_status != 0 || parse_worker_result(buffer, &result) != 0) {
        return -1;
    }
    apply_baseline_change(&result, baseline);
    update_counts(&result, options);
    if (should_print_result(&result, options)) {
        print_result(&result, options);
    }
    return 0;
}

static int flush_scan_children(PortscanOptions *options, PortscanBaseline *baseline) {
    while (active_child_count > 0U) {
        if (reap_child_index(0U, options, baseline) != 0) {
            return -1;
        }
    }
    return 0;
}

static int spawn_scan_child(const char *host, unsigned int port, PortscanOptions *options, PortscanBaseline *baseline) {
    char port_text[16];
    char timeout_text[32];
    char banner_bytes_text[16];
    char banner_timeout_text[32];
    char *argv_exec[24];
    int argc = 0;
    int pipe_fds[2];
    int pid = 0;
    unsigned int limit = options->jobs;

    if (limit == 0U) limit = 1U;
    if (options->per_host_jobs != 0U && options->per_host_jobs < limit) limit = options->per_host_jobs;
    while (active_child_count >= limit) {
        if (reap_child_index(0U, options, baseline) != 0) return -1;
    }
    if (options->rate_per_second > 0U) {
        unsigned int interval = (1000U + options->rate_per_second - 1U) / options->rate_per_second;
        if (interval > 0U) (void)platform_sleep_milliseconds((unsigned long long)interval);
    }
    rt_unsigned_to_string((unsigned long long)port, port_text, sizeof(port_text));
    rt_unsigned_to_string((unsigned long long)options->timeout_milliseconds, timeout_text, sizeof(timeout_text));
    rt_unsigned_to_string((unsigned long long)(options->banner_byte_limit != 0U ? options->banner_byte_limit : PORTSCAN_BANNER_DEFAULT), banner_bytes_text, sizeof(banner_bytes_text));
    rt_unsigned_to_string((unsigned long long)(options->banner_timeout_milliseconds != 0U ? options->banner_timeout_milliseconds : PORTSCAN_BANNER_TIMEOUT_DEFAULT), banner_timeout_text, sizeof(banner_timeout_text));
    if (platform_create_pipe(pipe_fds) != 0) {
        return -1;
    }
    argv_exec[argc++] = (char *)(options->program_name != 0 ? options->program_name : "portscan");
    argv_exec[argc++] = "--worker";
    if (options->family == PLATFORM_NETWORK_FAMILY_IPV4) argv_exec[argc++] = "-4";
    if (options->family == PLATFORM_NETWORK_FAMILY_IPV6) argv_exec[argc++] = "-6";
    if (options->numeric_only) argv_exec[argc++] = "-n";
    argv_exec[argc++] = "-w";
    argv_exec[argc++] = timeout_text;
    if (options->read_banner) {
        argv_exec[argc++] = "--banner";
        argv_exec[argc++] = "--banner-bytes";
        argv_exec[argc++] = banner_bytes_text;
        argv_exec[argc++] = "--banner-timeout";
        argv_exec[argc++] = banner_timeout_text;
    }
    if (options->tls_cert) argv_exec[argc++] = "--tls-cert";
    if (options->tls_insecure) argv_exec[argc++] = "--tls-insecure";
    argv_exec[argc++] = (char *)host;
    argv_exec[argc++] = port_text;
    argv_exec[argc] = 0;
    if (platform_spawn_process(argv_exec, -1, pipe_fds[1], 0, 0, 0, &pid) != 0) {
        platform_close(pipe_fds[0]);
        platform_close(pipe_fds[1]);
        return -1;
    }
    platform_close(pipe_fds[1]);
    active_children[active_child_count].pid = pid;
    active_children[active_child_count].fd = pipe_fds[0];
    active_child_count += 1U;
    return 0;
}

static void scan_one(const char *host, unsigned int port, PortscanOptions *options, PortscanBaseline *baseline) {
    PortscanResult result;

    if (options->jobs > 1U) {
        if (spawn_scan_child(host, port, options, baseline) != 0) {
            tool_write_error("portscan", "failed to start scan worker", 0);
        }
        return;
    }
    if (scan_one_result(host, port, options, &result) != 0) {
        return;
    }
    apply_baseline_change(&result, baseline);
    update_counts(&result, options);
    if (should_print_result(&result, options)) {
        print_result(&result, options);
    }
    if (options->delay_milliseconds > 0U) {
        (void)platform_sleep_milliseconds((unsigned long long)options->delay_milliseconds);
    }
    if (options->rate_per_second > 0U) {
        unsigned int interval = (1000U + options->rate_per_second - 1U) / options->rate_per_second;
        if (interval > 0U) {
            (void)platform_sleep_milliseconds((unsigned long long)interval);
        }
    }
}

static int scan_host_ports(const char *host, const char *port_spec, PortscanOptions *options, PortscanBaseline *baseline) {
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
            scan_one(host, first, options, baseline);
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

static int scan_host_with_all_ports(const char *host, int port_count, char **ports, PortscanOptions *options, PortscanBaseline *baseline) {
    int index = 0;

    while (index < port_count) {
        if (scan_host_ports(host, ports[index], options, baseline) != 0) {
            tool_write_error("portscan", "invalid port list: ", ports[index]);
            return -1;
        }
        ++index;
    }
    if (options->use_common_ports && scan_host_ports(host, common_port_spec, options, baseline) != 0) {
        tool_write_error("portscan", "invalid common port list", 0);
        return -1;
    }
    for (index = 0; (unsigned int)index < options->profile_count; ++index) {
        if (scan_host_ports(host, options->profile_specs[index], options, baseline) != 0) {
            tool_write_error("portscan", "invalid profile port list", 0);
            return -1;
        }
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
        rt_write_cstr(1, ",\"elapsed_ms\":");
        rt_write_uint(1, options->finished_at > options->started_at ? (options->finished_at - options->started_at) * 1000ULL : 0ULL);
        rt_write_cstr(1, ",\"jobs\":");
        rt_write_uint(1, (unsigned long long)options->jobs);
        rt_write_cstr(1, ",\"timeout_ms\":");
        rt_write_uint(1, (unsigned long long)options->timeout_milliseconds);
        rt_write_cstr(1, ",\"rate_per_second\":");
        rt_write_uint(1, (unsigned long long)options->rate_per_second);
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
    rt_write_cstr(1, " elapsed_ms=");
    rt_write_uint(1, options->finished_at > options->started_at ? (options->finished_at - options->started_at) * 1000ULL : 0ULL);
    rt_write_cstr(1, " jobs=");
    rt_write_uint(1, (unsigned long long)options->jobs);
    rt_write_cstr(1, " timeout_ms=");
    rt_write_uint(1, (unsigned long long)options->timeout_milliseconds);
    if (options->rate_per_second != 0U) {
        rt_write_cstr(1, " rate_per_second=");
        rt_write_uint(1, (unsigned long long)options->rate_per_second);
    }
    rt_write_char(1, '\n');
}

static int scan_host_token(const char *token, size_t token_length, int port_count, char **ports, PortscanOptions *options, PortscanBaseline *baseline) {
    char host[PORTSCAN_HOST_SIZE];
    size_t last_dot = 0U;
    size_t dash = 0U;
    unsigned int first = 0U;
    unsigned int last = 0U;

    if (token_length == 0U) {
        return -1;
    }

    if (token[0] == '[' && token[token_length - 1U] == ']') {
        if (copy_slice(host, sizeof(host), token + 1U, token_length - 2U) != 0) {
            return -1;
        }
        return scan_host_with_all_ports(host, port_count, ports, options, baseline);
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
            if (tool_buffer_append_uint_checked(host, sizeof(host), &length, first) != 0) {
                return -1;
            }
            if (scan_host_with_all_ports(host, port_count, ports, options, baseline) != 0) {
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
    return scan_host_with_all_ports(host, port_count, ports, options, baseline);
}

static int scan_hosts(const char *host_spec, int port_count, char **ports, PortscanOptions *options, PortscanBaseline *baseline) {
    size_t spec_length = rt_strlen(host_spec);
    size_t start = 0U;

    while (start <= spec_length) {
        size_t end = start;
        while (end < spec_length && host_spec[end] != ',') {
            ++end;
        }
        if (scan_host_token(host_spec + start, end - start, port_count, ports, options, baseline) != 0) {
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

static int state_name_known(const char *state) {
    return tool_str_equal(state, "open") || tool_str_equal(state, "closed") || tool_str_equal(state, "filtered") ||
           tool_str_equal(state, "unreachable") || tool_str_equal(state, "error");
}

static int baseline_add(PortscanBaseline *baseline, const char *host, unsigned int port, const char *state) {
    if (baseline == 0 || host == 0 || state == 0 || !state_name_known(state) || baseline->count >= PORTSCAN_MAX_BASELINE) {
        return -1;
    }
    rt_copy_string(baseline->entries[baseline->count].host, sizeof(baseline->entries[baseline->count].host), host);
    baseline->entries[baseline->count].port = port;
    rt_copy_string(baseline->entries[baseline->count].state, sizeof(baseline->entries[baseline->count].state), state);
    baseline->entries[baseline->count].seen = 0;
    baseline->count += 1U;
    return 0;
}

static int split_words_line(char *line, char **first, char **second, char **third) {
    char *parts[3];
    int part_count = 0;
    char *cursor = line;

    while (*cursor != '\0' && part_count < 3) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            ++cursor;
        }
        if (*cursor == '\0' || *cursor == '\n' || *cursor == '\r') {
            break;
        }
        parts[part_count++] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != ',' && *cursor != '\n' && *cursor != '\r') {
            ++cursor;
        }
        if (*cursor != '\0') {
            *cursor = '\0';
            ++cursor;
        }
    }
    if (part_count < 3) {
        return -1;
    }
    *first = parts[0];
    *second = parts[1];
    *third = parts[2];
    return 0;
}

static int json_extract_string_field(const char *line, const char *name, char *out, size_t out_size) {
    size_t name_len = rt_strlen(name);
    size_t index = 0U;

    if (out == 0 || out_size == 0U) {
        return -1;
    }
    while (line[index] != '\0') {
        if (line[index] == '"' && rt_strncmp(line + index + 1U, name, name_len) == 0 && line[index + 1U + name_len] == '"') {
            size_t pos = index + 1U + name_len + 1U;
            size_t used = 0U;
            while (line[pos] == ' ' || line[pos] == ':') ++pos;
            if (line[pos] != '"') return -1;
            ++pos;
            while (line[pos] != '\0' && line[pos] != '"' && used + 1U < out_size) {
                out[used++] = line[pos++];
            }
            out[used] = '\0';
            return line[pos] == '"' ? 0 : -1;
        }
        ++index;
    }
    return -1;
}

static int json_extract_uint_field(const char *line, const char *name, unsigned int *out) {
    size_t name_len = rt_strlen(name);
    size_t index = 0U;

    while (line[index] != '\0') {
        if (line[index] == '"' && rt_strncmp(line + index + 1U, name, name_len) == 0 && line[index + 1U + name_len] == '"') {
            unsigned int value = 0U;
            size_t pos = index + 1U + name_len + 1U;
            while (line[pos] == ' ' || line[pos] == ':') ++pos;
            if (line[pos] < '0' || line[pos] > '9') return -1;
            while (line[pos] >= '0' && line[pos] <= '9') {
                unsigned int digit = (unsigned int)(line[pos] - '0');
                if (value > 6553U || (value == 6553U && digit > 5U)) return -1;
                value = value * 10U + digit;
                ++pos;
            }
            *out = value;
            return 0;
        }
        ++index;
    }
    return -1;
}

static int load_baseline_line(PortscanBaseline *baseline, char *line) {
    char *host;
    char *port_text;
    char *state;
    unsigned long long port_value = 0ULL;

    if (line[0] == '\0' || line[0] == '#' || (line[0] == 'h' && line[1] == 'o' && line[2] == 's' && line[3] == 't')) {
        return 0;
    }
    if (line[0] == '{') {
        char json_host[PORTSCAN_HOST_SIZE];
        char json_state[16];
        unsigned int json_port = 0U;
        if (json_extract_string_field(line, "host", json_host, sizeof(json_host)) == 0 &&
            json_extract_uint_field(line, "port", &json_port) == 0 &&
            json_extract_string_field(line, "state", json_state, sizeof(json_state)) == 0) {
            return baseline_add(baseline, json_host, json_port, json_state);
        }
        return 0;
    }
    if (split_words_line(line, &host, &port_text, &state) != 0) {
        return 0;
    }
    if (rt_parse_uint(port_text, &port_value) != 0 || port_value == 0ULL || port_value > 65535ULL) {
        return 0;
    }
    return baseline_add(baseline, host, (unsigned int)port_value, state);
}

static int load_baseline_file(const char *path, PortscanBaseline *baseline) {
    int fd;
    char line[1024];
    size_t used = 0U;

    if (path == 0 || baseline == 0) {
        return 0;
    }
    baseline->count = 0U;
    fd = platform_open_read(path);
    if (fd < 0) {
        tool_write_error("portscan", "cannot read baseline: ", path);
        return -1;
    }
    for (;;) {
        char ch;
        long got = platform_read(fd, &ch, 1U);
        if (got < 0) {
            platform_close(fd);
            return -1;
        }
        if (got == 0) {
            if (used > 0U) {
                line[used] = '\0';
                if (load_baseline_line(baseline, line) != 0) {
                    platform_close(fd);
                    return -1;
                }
            }
            break;
        }
        if (ch == '\n') {
            line[used] = '\0';
            if (load_baseline_line(baseline, line) != 0) {
                platform_close(fd);
                return -1;
            }
            used = 0U;
        } else if (used + 1U < sizeof(line)) {
            line[used++] = ch;
        }
    }
    platform_close(fd);
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
    options.jobs = 1U;
    options.per_host_jobs = 1U;
    options.started_at = (unsigned long long)platform_get_epoch_time();
    options.program_name = argv[0];

    positionals = (char **)rt_malloc(sizeof(char *) * (size_t)(argc > 1 ? argc - 1 : 1));
    if (positionals == 0) {
        tool_write_error("portscan", "out of memory", 0);
        return 1;
    }

    while (argi < argc) {
        if (tool_str_equal(argv[argi], "--worker")) {
            options.worker_mode = 1;
        } else if (tool_str_equal(argv[argi], "-4")) {
            options.family = PLATFORM_NETWORK_FAMILY_IPV4;
        } else if (tool_str_equal(argv[argi], "-6")) {
            options.family = PLATFORM_NETWORK_FAMILY_IPV6;
        } else if (tool_str_equal(argv[argi], "-a")) {
            options.show_all = 1;
        } else if (tool_str_equal(argv[argi], "-n")) {
            options.numeric_only = 1;
        } else if (tool_str_equal(argv[argi], "-w")) {
            unsigned long long timeout_value = 0ULL;
            if (argi + 1 >= argc || tool_parse_duration_ms(argv[argi + 1], &timeout_value) != 0 || timeout_value > 0xffffffffULL) {
                print_usage(argv[0]);
                rt_free(positionals);
                return 1;
            }
            options.timeout_milliseconds = (unsigned int)timeout_value;
            argi += 1;
        } else if (tool_str_equal(argv[argi], "--common")) {
            options.use_common_ports = 1;
        } else if (tool_str_equal(argv[argi], "--profile")) {
            const char *spec;
            if (argi + 1 >= argc || options.profile_count >= sizeof(options.profile_specs) / sizeof(options.profile_specs[0])) {
                print_usage(argv[0]);
                rt_free(positionals);
                return 1;
            }
            spec = profile_port_spec(argv[argi + 1]);
            if (spec == 0) {
                tool_write_error("portscan", "unknown profile: ", argv[argi + 1]);
                rt_free(positionals);
                return 1;
            }
            options.profile_specs[options.profile_count++] = spec;
            argi += 1;
        } else if (tool_str_equal(argv[argi], "--jobs")) {
            unsigned long long value = 0ULL;
            if (argi + 1 >= argc || rt_parse_uint(argv[argi + 1], &value) != 0 || value == 0ULL || value > PORTSCAN_MAX_JOBS) {
                print_usage(argv[0]);
                rt_free(positionals);
                return 1;
            }
            options.jobs = (unsigned int)value;
            argi += 1;
        } else if (tool_str_equal(argv[argi], "--per-host")) {
            unsigned long long value = 0ULL;
            if (argi + 1 >= argc || rt_parse_uint(argv[argi + 1], &value) != 0 || value == 0ULL || value > PORTSCAN_MAX_JOBS) {
                print_usage(argv[0]);
                rt_free(positionals);
                return 1;
            }
            options.per_host_jobs = (unsigned int)value;
            argi += 1;
        } else if (tool_str_equal(argv[argi], "--rate")) {
            if (argi + 1 >= argc || parse_rate_arg(argv[argi + 1], &options.rate_per_second) != 0) {
                print_usage(argv[0]);
                rt_free(positionals);
                return 1;
            }
            argi += 1;
        } else if (tool_str_equal(argv[argi], "--delay")) {
            unsigned long long delay_value = 0ULL;
            if (argi + 1 >= argc || tool_parse_duration_ms(argv[argi + 1], &delay_value) != 0 || delay_value > 0xffffffffULL) {
                print_usage(argv[0]);
                rt_free(positionals);
                return 1;
            }
            options.delay_milliseconds = (unsigned int)delay_value;
            argi += 1;
        } else if (tool_str_equal(argv[argi], "--services")) {
            options.show_services = 1;
        } else if (tool_str_equal(argv[argi], "--summary")) {
            options.show_summary = 1;
        } else if (tool_str_equal(argv[argi], "--progress")) {
            options.show_progress = 1;
        } else if (tool_str_equal(argv[argi], "--csv")) {
            options.csv_output = 1;
        } else if (tool_str_equal(argv[argi], "--json")) {
            tool_json_set_enabled(1);
        } else if (tool_str_equal(argv[argi], "--details")) {
            options.show_details = 1;
        } else if (tool_str_equal(argv[argi], "--baseline")) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                rt_free(positionals);
                return 1;
            }
            options.baseline_path = argv[argi + 1];
            argi += 1;
        } else if (tool_str_equal(argv[argi], "--diff")) {
            options.diff_only = 1;
        } else if (tool_str_equal(argv[argi], "--fail-open")) {
            options.fail_open = 1;
        } else if (tool_str_equal(argv[argi], "--fail-closed")) {
            options.fail_closed = 1;
        } else if (tool_str_equal(argv[argi], "--banner")) {
            options.read_banner = 1;
        } else if (tool_str_equal(argv[argi], "--tls-cert")) {
            options.tls_cert = 1;
            options.show_details = 1;
        } else if (tool_str_equal(argv[argi], "--tls-insecure")) {
            options.tls_insecure = 1;
        } else if (tool_str_equal(argv[argi], "--banner-bytes")) {
            unsigned long long value = 0ULL;
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &value, "portscan", "banner byte limit") != 0 ||
                value == 0ULL || value > (unsigned long long)PORTSCAN_BANNER_MAX) {
                print_usage(argv[0]);
                rt_free(positionals);
                return 1;
            }
            options.banner_byte_limit = (unsigned int)value;
            argi += 1;
        } else if (tool_str_equal(argv[argi], "--banner-timeout")) {
            unsigned long long banner_timeout = 0ULL;
            if (argi + 1 >= argc || tool_parse_duration_ms(argv[argi + 1], &banner_timeout) != 0 || banner_timeout > 0xffffffffULL) {
                print_usage(argv[0]);
                rt_free(positionals);
                return 1;
            }
            options.banner_timeout_milliseconds = (unsigned int)banner_timeout;
            argi += 1;
        } else if (tool_str_equal(argv[argi], "-h") || tool_str_equal(argv[argi], "--help")) {
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

    if (options.worker_mode) {
        unsigned long long port_value = 0ULL;
        PortscanResult result;
        if (positional_count != 2 || rt_parse_uint(positionals[1], &port_value) != 0 || port_value == 0ULL || port_value > 65535ULL) {
            print_usage(argv[0]);
            rt_free(positionals);
            return 1;
        }
        if (scan_one_result(positionals[0], (unsigned int)port_value, &options, &result) != 0) {
            rt_free(positionals);
            return 1;
        }
        print_worker_result(&result);
        rt_free(positionals);
        return 0;
    }

    if (positional_count < 1 || (positional_count < 2 && !options.use_common_ports && options.profile_count == 0U)) {
        print_usage(argv[0]);
        rt_free(positionals);
        return 1;
    }
    if (options.csv_output && tool_json_is_enabled()) {
        tool_write_error("portscan", "--csv and --json are mutually exclusive", 0);
        rt_free(positionals);
        return 1;
    }
    if (options.diff_only && options.baseline_path == 0) {
        tool_write_error("portscan", "--diff requires --baseline", 0);
        rt_free(positionals);
        return 1;
    }
    if (options.csv_output) {
        rt_write_line(1, "host,port,state,service,latency_ms,reason,change,banner,tls_protocol,tls_cipher,tls_verification,tls_subject,tls_issuer,tls_dns_names,tls_not_after");
    }
    {
        PortscanBaseline baseline;
        PortscanBaseline *baseline_ptr = 0;

        rt_memset(&baseline, 0, sizeof(baseline));
        if (options.baseline_path != 0) {
            if (load_baseline_file(options.baseline_path, &baseline) != 0) {
                rt_free(positionals);
                return 1;
            }
            baseline_ptr = &baseline;
        }
        if (scan_hosts(positionals[0], positional_count - 1, positionals + 1, &options, baseline_ptr) != 0) {
            rt_free(positionals);
            return 1;
        }
        if (flush_scan_children(&options, baseline_ptr) != 0) {
            rt_free(positionals);
            return 1;
        }
    }
    options.finished_at = (unsigned long long)platform_get_epoch_time();
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
