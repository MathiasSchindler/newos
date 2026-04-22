#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define DIG_MAX_RESULTS 48U

static int streq(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static char ascii_tolower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int streq_ignore_case(const char *left, const char *right) {
    size_t index = 0U;

    if (left == 0 || right == 0) {
        return 0;
    }
    while (left[index] != '\0' && right[index] != '\0') {
        if (ascii_tolower(left[index]) != ascii_tolower(right[index])) {
            return 0;
        }
        index += 1U;
    }
    return left[index] == '\0' && right[index] == '\0';
}

static const char *dns_type_name(unsigned short record_type) {
    switch (record_type) {
        case PLATFORM_DNS_RECORD_A:
            return "A";
        case PLATFORM_DNS_RECORD_AAAA:
            return "AAAA";
        case PLATFORM_DNS_RECORD_MX:
            return "MX";
        case PLATFORM_DNS_RECORD_NS:
            return "NS";
        case PLATFORM_DNS_RECORD_TXT:
            return "TXT";
        case PLATFORM_DNS_RECORD_CNAME:
            return "CNAME";
        default:
            return "UNKNOWN";
    }
}

static int parse_type(const char *text, unsigned short *record_type_out) {
    if (text == 0 || record_type_out == 0) {
        return -1;
    }
    if (streq_ignore_case(text, "A")) {
        *record_type_out = PLATFORM_DNS_RECORD_A;
        return 0;
    }
    if (streq_ignore_case(text, "AAAA")) {
        *record_type_out = PLATFORM_DNS_RECORD_AAAA;
        return 0;
    }
    if (streq_ignore_case(text, "MX")) {
        *record_type_out = PLATFORM_DNS_RECORD_MX;
        return 0;
    }
    if (streq_ignore_case(text, "NS")) {
        *record_type_out = PLATFORM_DNS_RECORD_NS;
        return 0;
    }
    if (streq_ignore_case(text, "TXT")) {
        *record_type_out = PLATFORM_DNS_RECORD_TXT;
        return 0;
    }
    return -1;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-4|-6] [-t TYPE] [-s SERVER|@SERVER] [-p PORT] NAME [TYPE]");
}

static void print_help(const char *program_name) {
    print_usage(program_name);
    rt_write_line(1, "Query a small subset of DNS records in a dig-style format.");
    rt_write_line(1, "");
    rt_write_line(1, "Supported record types: A, AAAA, MX, NS, TXT");
    rt_write_line(1, "");
    rt_write_line(1, "Options:");
    rt_write_line(1, "  -4           force an IPv4 A query");
    rt_write_line(1, "  -6           force an IPv6 AAAA query");
    rt_write_line(1, "  -t TYPE      select the DNS record type");
    rt_write_line(1, "  -s SERVER    query the specified DNS server");
    rt_write_line(1, "  -p PORT      use a custom DNS server port (default: 53)");
    rt_write_line(1, "  -h, --help   show this help text");
}

static void write_name_with_dot(const char *name) {
    size_t length;

    if (name == 0 || name[0] == '\0') {
        return;
    }
    rt_write_cstr(1, name);
    length = rt_strlen(name);
    if (length > 0U && name[length - 1U] != '.') {
        rt_write_char(1, '.');
    }
}

static void print_answer(const PlatformDnsEntry *entry) {
    const char *type_name;
    const char *text;

    if (entry == 0) {
        return;
    }
    type_name = dns_type_name(entry->record_type);
    text = entry->data[0] != '\0' ? entry->data : entry->address;

    write_name_with_dot(entry->name);
    rt_write_char(1, '\t');
    rt_write_uint(1, entry->ttl);
    rt_write_cstr(1, "\tIN\t");
    rt_write_cstr(1, type_name);
    rt_write_char(1, '\t');
    if (entry->record_type == PLATFORM_DNS_RECORD_MX) {
        rt_write_uint(1, entry->preference);
        rt_write_char(1, ' ');
    }
    if (entry->record_type == PLATFORM_DNS_RECORD_TXT) {
        rt_write_char(1, '"');
        rt_write_cstr(1, text);
        rt_write_char(1, '"');
        rt_write_char(1, '\n');
        return;
    }
    rt_write_line(1, text);
}

int main(int argc, char **argv) {
    int argi = 1;
    const char *server = 0;
    const char *name = 0;
    unsigned short query_type = PLATFORM_DNS_RECORD_A;
    unsigned long long port_value = 53ULL;
    PlatformDnsEntry entries[DIG_MAX_RESULTS];
    size_t count = 0U;
    size_t i;
    int family_hint = PLATFORM_NETWORK_FAMILY_ANY;

    while (argi < argc) {
        if (streq(argv[argi], "-4")) {
            query_type = PLATFORM_DNS_RECORD_A;
            family_hint = PLATFORM_NETWORK_FAMILY_IPV4;
            argi += 1;
            continue;
        }
        if (streq(argv[argi], "-6")) {
            query_type = PLATFORM_DNS_RECORD_AAAA;
            family_hint = PLATFORM_NETWORK_FAMILY_IPV6;
            argi += 1;
            continue;
        }
        if (streq(argv[argi], "-t")) {
            if (argi + 1 >= argc || parse_type(argv[argi + 1], &query_type) != 0) {
                tool_write_error("dig", "unsupported type: ", argi + 1 < argc ? argv[argi + 1] : "(missing)");
                print_usage(argv[0]);
                return 1;
            }
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
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &port_value, "dig", "port") != 0 ||
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
        if (argv[argi][0] == '@' && argv[argi][1] != '\0') {
            server = argv[argi] + 1;
            argi += 1;
            continue;
        }
        if (argv[argi][0] == '-') {
            tool_write_error("dig", "unknown option: ", argv[argi]);
            print_usage(argv[0]);
            return 1;
        }
        break;
    }

    if (argi >= argc || argi + 2 < argc) {
        print_usage(argv[0]);
        return 1;
    }
    name = argv[argi++];
    if (argi < argc && parse_type(argv[argi], &query_type) != 0) {
        tool_write_error("dig", "unsupported type: ", argv[argi]);
        return 1;
    }
    if (argi < argc) {
        argi += 1;
    }

    if ((family_hint == PLATFORM_NETWORK_FAMILY_IPV4 && query_type != PLATFORM_DNS_RECORD_A) ||
        (family_hint == PLATFORM_NETWORK_FAMILY_IPV6 && query_type != PLATFORM_DNS_RECORD_AAAA)) {
        tool_write_error("dig", "type does not match address family flag: ", dns_type_name(query_type));
        return 1;
    }

    if (platform_dns_query(server, (unsigned int)port_value, name, query_type, entries, DIG_MAX_RESULTS, &count) != 0 || count == 0U) {
        tool_write_error("dig", "lookup failed for ", name);
        return 1;
    }

    rt_write_cstr(1, "; <<>> dig <<>> ");
    rt_write_cstr(1, name);
    rt_write_cstr(1, " ");
    rt_write_line(1, dns_type_name(query_type));
    if (server != 0 && server[0] != '\0') {
        rt_write_cstr(1, ";; SERVER: ");
        rt_write_cstr(1, server);
        rt_write_cstr(1, "#");
        rt_write_uint(1, port_value);
        rt_write_char(1, '\n');
    }
    rt_write_line(1, ";; QUESTION SECTION:");
    rt_write_char(1, ';');
    write_name_with_dot(name);
    rt_write_cstr(1, "\tIN\t");
    rt_write_line(1, dns_type_name(query_type));
    rt_write_line(1, "");
    rt_write_line(1, ";; ANSWER SECTION:");

    for (i = 0U; i < count; ++i) {
        print_answer(&entries[i]);
    }

    return 0;
}
