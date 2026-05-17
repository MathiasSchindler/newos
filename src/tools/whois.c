#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define WHOIS_BUFFER_SIZE 4096U
#define WHOIS_CAPTURE_SIZE 65536U
#define WHOIS_REFERRAL_DEPTH 3U

static int write_all(int fd, const char *text, size_t length) {
    size_t offset = 0U;

    while (offset < length) {
        long written = platform_write(fd, text + offset, length - offset);
        if (written <= 0) {
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static void print_help(void) {
    rt_write_line(1, "whois - query a WHOIS server");
    rt_write_line(1, "Usage: whois [-R] [-h SERVER] [-p PORT] QUERY");
    rt_write_line(1, "Options:");
    rt_write_line(1, "  -R          do not follow referral servers");
    rt_write_line(1, "  -h SERVER   query SERVER instead of whois.iana.org");
    rt_write_line(1, "  -p PORT     connect to PORT instead of 43");
}

static int ascii_lower(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch + ('a' - 'A');
    }
    return ch;
}

static int field_name_matches(const char *line, size_t line_length, const char *field) {
    size_t field_length = rt_strlen(field);
    size_t index = 0U;

    if (line_length < field_length + 1U) {
        return 0;
    }
    while (index < field_length) {
        if (ascii_lower((unsigned char)line[index]) != field[index]) {
            return 0;
        }
        ++index;
    }
    return line[index] == ':';
}

static int copy_field_value(const char *line, size_t line_length, const char *field, char *out, size_t out_size) {
    size_t index = rt_strlen(field) + 1U;
    size_t out_index = 0U;
    size_t end = line_length;

    if (out_size == 0U || !field_name_matches(line, line_length, field)) {
        return -1;
    }
    while (index < end && (line[index] == ' ' || line[index] == '\t')) {
        ++index;
    }
    while (end > index && (line[end - 1U] == ' ' || line[end - 1U] == '\t' || line[end - 1U] == '\r')) {
        --end;
    }
    if (index == end || end - index >= out_size) {
        return -1;
    }
    while (index < end) {
        out[out_index++] = line[index++];
    }
    out[out_index] = '\0';
    return 0;
}

static void normalize_referral_server(char *server) {
    const char *prefix = "whois://";
    size_t prefix_length = rt_strlen(prefix);
    size_t index = 0U;
    size_t out_index = 0U;

    if (rt_strncmp(server, prefix, prefix_length) == 0) {
        index = prefix_length;
        while (server[index] != '\0' && server[index] != '/') {
            server[out_index++] = server[index++];
        }
        server[out_index] = '\0';
    }
}

static int find_referral_server(const char *response, size_t response_length, char *server_out, size_t server_size) {
    size_t line_start = 0U;

    while (line_start < response_length) {
        size_t line_end = line_start;
        while (line_end < response_length && response[line_end] != '\n') {
            ++line_end;
        }
        if (copy_field_value(response + line_start, line_end - line_start, "refer", server_out, server_size) == 0) {
            normalize_referral_server(server_out);
            return 0;
        }
        line_start = line_end < response_length ? line_end + 1U : response_length;
    }

    line_start = 0U;
    while (line_start < response_length) {
        size_t line_end = line_start;
        while (line_end < response_length && response[line_end] != '\n') {
            ++line_end;
        }
        if (copy_field_value(response + line_start, line_end - line_start, "referralserver", server_out, server_size) == 0) {
            normalize_referral_server(server_out);
            return 0;
        }
        line_start = line_end < response_length ? line_end + 1U : response_length;
    }

    line_start = 0U;
    while (line_start < response_length) {
        size_t line_end = line_start;
        while (line_end < response_length && response[line_end] != '\n') {
            ++line_end;
        }
        if (copy_field_value(response + line_start, line_end - line_start, "whois", server_out, server_size) == 0) {
            normalize_referral_server(server_out);
            return 0;
        }
        line_start = line_end < response_length ? line_end + 1U : response_length;
    }
    return -1;
}

static int query_whois_server(const char *server, unsigned int port, const char *query, char *capture, size_t capture_size, size_t *capture_length_out) {
    int fd = -1;
    char buffer[WHOIS_BUFFER_SIZE];
    size_t capture_length = 0U;

    if (capture_length_out != 0) {
        *capture_length_out = 0U;
    }
    if (capture != 0 && capture_size > 0U) {
        capture[0] = '\0';
    }
    if (platform_connect_tcp(server, port, &fd) != 0) {
        tool_write_error("whois", "connect failed to ", server);
        return 1;
    }
    if (write_all(fd, query, rt_strlen(query)) != 0 || write_all(fd, "\r\n", 2U) != 0) {
        (void)platform_close(fd);
        tool_write_error("whois", "write failed", 0);
        return 1;
    }

    for (;;) {
        long count = platform_read(fd, buffer, sizeof(buffer));
        if (count < 0) {
            (void)platform_close(fd);
            tool_write_error("whois", "read failed", 0);
            return 1;
        }
        if (count == 0) {
            break;
        }
        if (capture != 0 && capture_size > 0U && capture_length + 1U < capture_size) {
            size_t room = capture_size - capture_length - 1U;
            size_t copy_count = (size_t)count < room ? (size_t)count : room;
            memcpy(capture + capture_length, buffer, copy_count);
            capture_length += copy_count;
            capture[capture_length] = '\0';
        }
        if (write_all(1, buffer, (size_t)count) != 0) {
            (void)platform_close(fd);
            return 1;
        }
    }

    (void)platform_close(fd);
    if (capture_length_out != 0) {
        *capture_length_out = capture_length;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *server = "whois.iana.org";
    const char *query = 0;
    unsigned long long port = 43ULL;
    int argi = 1;
    int explicit_server = 0;
    int follow_referrals = 1;
    char current_server[256];
    char response[WHOIS_CAPTURE_SIZE];
    size_t response_length = 0U;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--help") == 0) {
            print_help();
            return 0;
        } else if (rt_strcmp(argv[argi], "-R") == 0) {
            follow_referrals = 0;
            ++argi;
            continue;
        } else if (rt_strcmp(argv[argi], "-h") == 0) {
            if (argi + 1 >= argc) {
                tool_write_usage("whois", "[-R] [-h SERVER] [-p PORT] QUERY");
                return 1;
            }
            server = argv[argi + 1];
            explicit_server = 1;
            argi += 2;
            continue;
        } else if (rt_strcmp(argv[argi], "-p") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &port, "whois", "port") != 0 ||
                port == 0ULL || port > 65535ULL) {
                tool_write_usage("whois", "[-R] [-h SERVER] [-p PORT] QUERY");
                return 1;
            }
            argi += 2;
            continue;
        }
        tool_write_error("whois", "unknown option: ", argv[argi]);
        tool_write_usage("whois", "[-R] [-h SERVER] [-p PORT] QUERY");
        return 1;
    }

    if (argi + 1 != argc) {
        tool_write_usage("whois", "[-R] [-h SERVER] [-p PORT] QUERY");
        return 1;
    }
    query = argv[argi];

    rt_copy_string(current_server, sizeof(current_server), server);

    if (query_whois_server(current_server, (unsigned int)port, query, response, sizeof(response), &response_length) != 0) {
        return 1;
    }

    if (follow_referrals && !explicit_server && port == 43ULL) {
        char referral[256];
        unsigned int depth = 0U;
        while (depth < WHOIS_REFERRAL_DEPTH && find_referral_server(response, response_length, referral, sizeof(referral)) == 0 &&
               rt_strcmp(referral, current_server) != 0) {
            rt_copy_string(current_server, sizeof(current_server), referral);
            if (query_whois_server(current_server, 43U, query, response, sizeof(response), &response_length) != 0) {
                return 1;
            }
            ++depth;
        }
    }
    return 0;
}