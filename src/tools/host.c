#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define HOST_MAX_RESULTS 48U

static int streq(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static char ascii_tolower(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static int streq_ignore_case(const char *left, const char *right) {
    size_t index = 0U;

    while (left[index] != '\0' && right[index] != '\0') {
        if (ascii_tolower(left[index]) != ascii_tolower(right[index])) {
            return 0;
        }
        index += 1U;
    }
    return left[index] == '\0' && right[index] == '\0';
}

static int parse_type(const char *text, unsigned short *type_out) {
    if (streq_ignore_case(text, "A")) {
        *type_out = PLATFORM_DNS_RECORD_A;
    } else if (streq_ignore_case(text, "AAAA")) {
        *type_out = PLATFORM_DNS_RECORD_AAAA;
    } else if (streq_ignore_case(text, "MX")) {
        *type_out = PLATFORM_DNS_RECORD_MX;
    } else if (streq_ignore_case(text, "NS")) {
        *type_out = PLATFORM_DNS_RECORD_NS;
    } else if (streq_ignore_case(text, "TXT")) {
        *type_out = PLATFORM_DNS_RECORD_TXT;
    } else {
        return -1;
    }
    return 0;
}

static const char *type_name(unsigned short type) {
    if (type == PLATFORM_DNS_RECORD_A) return "A";
    if (type == PLATFORM_DNS_RECORD_AAAA) return "AAAA";
    if (type == PLATFORM_DNS_RECORD_MX) return "MX";
    if (type == PLATFORM_DNS_RECORD_NS) return "NS";
    if (type == PLATFORM_DNS_RECORD_TXT) return "TXT";
    if (type == PLATFORM_DNS_RECORD_CNAME) return "CNAME";
    return "record";
}

static void print_usage(void) {
    tool_write_usage("host", "[-4|-6] [-t TYPE] [-s SERVER] NAME [TYPE]");
}

static void print_result(const char *query, const PlatformDnsEntry *entry) {
    const char *data = entry->data[0] != '\0' ? entry->data : entry->address;

    rt_write_cstr(1, query);
    if (entry->record_type == PLATFORM_DNS_RECORD_MX) {
        rt_write_cstr(1, " mail is handled by ");
        rt_write_uint(1, entry->preference);
        rt_write_char(1, ' ');
        rt_write_line(1, data);
    } else if (entry->record_type == PLATFORM_DNS_RECORD_NS) {
        rt_write_cstr(1, " name server ");
        rt_write_line(1, data);
    } else if (entry->record_type == PLATFORM_DNS_RECORD_TXT) {
        rt_write_cstr(1, " descriptive text \"");
        rt_write_cstr(1, data);
        rt_write_line(1, "\"");
    } else {
        rt_write_cstr(1, " has ");
        rt_write_cstr(1, type_name(entry->record_type));
        rt_write_cstr(1, " address ");
        rt_write_line(1, data);
    }
}

int main(int argc, char **argv) {
    int argi = 1;
    const char *server = 0;
    const char *name = 0;
    unsigned short query_type = PLATFORM_DNS_RECORD_A;
    int family = PLATFORM_NETWORK_FAMILY_ANY;
    PlatformDnsEntry entries[HOST_MAX_RESULTS];
    size_t count = 0U;
    size_t i;

    while (argi < argc) {
        if (streq(argv[argi], "-4")) {
            query_type = PLATFORM_DNS_RECORD_A;
            family = PLATFORM_NETWORK_FAMILY_IPV4;
            argi += 1;
        } else if (streq(argv[argi], "-6")) {
            query_type = PLATFORM_DNS_RECORD_AAAA;
            family = PLATFORM_NETWORK_FAMILY_IPV6;
            argi += 1;
        } else if (streq(argv[argi], "-t")) {
            if (argi + 1 >= argc || parse_type(argv[argi + 1], &query_type) != 0) {
                print_usage();
                return 1;
            }
            argi += 2;
        } else if (streq(argv[argi], "-s")) {
            if (argi + 1 >= argc) {
                print_usage();
                return 1;
            }
            server = argv[argi + 1];
            argi += 2;
        } else if (streq(argv[argi], "-h") || streq(argv[argi], "--help")) {
            print_usage();
            return 0;
        } else {
            break;
        }
    }
    if (argi >= argc) {
        print_usage();
        return 1;
    }
    name = argv[argi++];
    if (argi < argc && parse_type(argv[argi], &query_type) != 0) {
        print_usage();
        return 1;
    }
    if (query_type == PLATFORM_DNS_RECORD_A) family = PLATFORM_NETWORK_FAMILY_IPV4;
    if (query_type == PLATFORM_DNS_RECORD_AAAA) family = PLATFORM_NETWORK_FAMILY_IPV6;

    if (query_type == PLATFORM_DNS_RECORD_A || query_type == PLATFORM_DNS_RECORD_AAAA) {
        if (platform_dns_lookup(server, 53U, name, family, entries, HOST_MAX_RESULTS, &count) != 0 || count == 0U) {
            tool_write_error("host", "lookup failed for ", name);
            return 1;
        }
    } else if (platform_dns_query(server, 53U, name, query_type, entries, HOST_MAX_RESULTS, &count) != 0 || count == 0U) {
        tool_write_error("host", "lookup failed for ", name);
        return 1;
    }
    for (i = 0U; i < count; ++i) {
        print_result(name, &entries[i]);
    }
    return 0;
}
