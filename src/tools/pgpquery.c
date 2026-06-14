#include "crypto/sha1.h"
#include "pgp.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PGPQUERY_USAGE "[--server NAME|URL] [--json] [--get] [-o OUT|--import-keyring KEYRING] [--print-url] [--timeout DURATION] SELECTOR..."
#define PGPQUERY_URL_CAPACITY 2048U
#define PGPQUERY_HOST_CAPACITY 256U
#define PGPQUERY_PATH_CAPACITY 1536U
#define PGPQUERY_BUFFER_CAPACITY 4096U
#define PGPQUERY_HEADER_CAPACITY 8192U
#define PGPQUERY_MAX_BODY_SIZE (4U * 1024U * 1024U)
#define PGPQUERY_DEFAULT_TIMEOUT_MS 30000ULL

#define PGPQUERY_SCHEME_HTTP 1
#define PGPQUERY_SCHEME_HTTPS 2

#define PGPQUERY_SERVER_ALL 0
#define PGPQUERY_SERVER_OPENPGP 1
#define PGPQUERY_SERVER_UBUNTU 2
#define PGPQUERY_SERVER_MAILVELOPE 3
#define PGPQUERY_SERVER_MIT 4
#define PGPQUERY_SERVER_WKD 5
#define PGPQUERY_SERVER_CUSTOM 6

typedef struct {
    int scheme;
    unsigned int port;
    char host[PGPQUERY_HOST_CAPACITY];
    char path[PGPQUERY_PATH_CAPACITY];
} PgpQueryUrl;

typedef struct {
    int use_tls;
    int socket_fd;
    PlatformTlsClient tls;
} PgpQueryConnection;

typedef struct {
    int server;
    int json;
    int get;
    int print_url;
    unsigned long long timeout_ms;
    const char *output_path;
    const char *import_keyring_path;
    char custom_base[PGPQUERY_URL_CAPACITY];
} PgpQueryOptions;

typedef struct {
    const char *server_name;
    const char *selector;
    int json;
    int count;
} PgpQueryCertificateContext;

typedef struct {
    PgpPublicKeyInfo primary;
    int found;
    int is_secret;
} PgpQueryFirstCertificateContext;

typedef struct {
    const PgpPublicKeyInfo *key;
    int found;
} PgpQueryFindCertificateContext;

static void print_usage(void) {
    tool_write_usage("pgpquery", PGPQUERY_USAGE);
}

static unsigned int default_port_for_scheme(int scheme) {
    return scheme == PGPQUERY_SCHEME_HTTPS ? 443U : 80U;
}

static int parse_http_url_tail(const char *text, unsigned int default_port, int scheme, PgpQueryUrl *url_out) {
    size_t index = 0U;
    size_t host_start = 0U;
    size_t host_length = 0U;
    unsigned long long parsed_port = default_port;
    int saw_port_digit = 0;

    rt_memset(url_out, 0, sizeof(*url_out));
    url_out->scheme = scheme;
    url_out->port = default_port;

    if (text[0] == '[') {
        host_start = 1U;
        index = 1U;
        while (text[index] != '\0' && text[index] != ']') index += 1U;
        if (text[index] != ']') return -1;
        host_length = index - host_start;
        index += 1U;
    } else {
        while (text[index] != '\0' && text[index] != '/' && text[index] != ':' && text[index] != '?' && text[index] != '#') index += 1U;
        host_length = index;
    }
    if (host_length == 0U || host_length + 1U > sizeof(url_out->host)) return -1;
    memcpy(url_out->host, text + host_start, host_length);
    url_out->host[host_length] = '\0';

    if (text[index] == ':') {
        parsed_port = 0ULL;
        index += 1U;
        while (text[index] >= '0' && text[index] <= '9') {
            saw_port_digit = 1;
            parsed_port = (parsed_port * 10ULL) + (unsigned long long)(text[index] - '0');
            index += 1U;
        }
        if (!saw_port_digit || parsed_port == 0ULL || parsed_port > 65535ULL) return -1;
    }
    url_out->port = (unsigned int)parsed_port;
    if (text[index] == '\0' || text[index] == '?' || text[index] == '#') {
        rt_copy_string(url_out->path, sizeof(url_out->path), "/");
    } else {
        rt_copy_string(url_out->path, sizeof(url_out->path), text + index);
    }
    return 0;
}

static int parse_url(const char *text, PgpQueryUrl *url_out) {
    if (tool_starts_with(text, "https://")) return parse_http_url_tail(text + 8, 443U, PGPQUERY_SCHEME_HTTPS, url_out);
    if (tool_starts_with(text, "http://")) return parse_http_url_tail(text + 7, 80U, PGPQUERY_SCHEME_HTTP, url_out);
    return -1;
}

static int query_connect(const PgpQueryUrl *url, PgpQueryConnection *connection) {
    rt_memset(connection, 0, sizeof(*connection));
    connection->socket_fd = -1;
    connection->use_tls = url->scheme == PGPQUERY_SCHEME_HTTPS;
    if (connection->use_tls) {
        if (platform_tls_connect(&connection->tls, url->host, url->port) != 0) return -1;
        connection->socket_fd = connection->tls.socket_fd;
        return 0;
    }
    return platform_connect_tcp(url->host, url->port, &connection->socket_fd);
}

static long query_read(PgpQueryConnection *connection, void *buffer, size_t count) {
    return connection->use_tls ? platform_tls_read(&connection->tls, buffer, count) : platform_read(connection->socket_fd, buffer, count);
}

static int query_write_all(PgpQueryConnection *connection, const void *buffer, size_t count) {
    const unsigned char *bytes = (const unsigned char *)buffer;
    size_t written = 0U;

    while (written < count) {
        long result = connection->use_tls ? platform_tls_write(&connection->tls, bytes + written, count - written) : platform_write(connection->socket_fd, bytes + written, count - written);
        if (result <= 0) return -1;
        written += (size_t)result;
    }
    return 0;
}

static void query_close(PgpQueryConnection *connection) {
    if (connection->use_tls) platform_tls_close(&connection->tls);
    else if (connection->socket_fd >= 0) (void)platform_close(connection->socket_fd);
    connection->socket_fd = -1;
}

static int wait_for_plain_socket(PgpQueryConnection *connection, unsigned long long timeout_ms) {
    int fds[1];
    size_t ready_index = 0U;

    if (connection->use_tls || timeout_ms == 0ULL) return 0;
    fds[0] = connection->socket_fd;
    return platform_poll_fds(fds, 1U, &ready_index, (int)timeout_ms) > 0 ? 0 : -1;
}

static int parse_status(const char *headers) {
    size_t index = 0U;
    int code = 0;
    int saw_digit = 0;

    while (headers[index] != '\0' && headers[index] != ' ') index += 1U;
    while (headers[index] == ' ') index += 1U;
    while (headers[index] >= '0' && headers[index] <= '9') {
        saw_digit = 1;
        code = code * 10 + (int)(headers[index] - '0');
        index += 1U;
    }
    return saw_digit ? code : -1;
}

static int ascii_equal_insensitive_n(const char *left, const char *right, size_t size) {
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (tool_ascii_tolower(left[index]) != tool_ascii_tolower(right[index])) return 0;
    }
    return 1;
}

static int http_headers_have_chunked_transfer(const char *headers) {
    size_t offset = 0U;

    while (headers[offset] != '\0') {
        size_t line_start = offset;
        size_t line_end;
        size_t name_end;
        size_t value_start;

        while (headers[offset] != '\0' && headers[offset] != '\n') offset += 1U;
        line_end = offset;
        if (headers[offset] == '\n') offset += 1U;
        if (line_end > line_start && headers[line_end - 1U] == '\r') line_end -= 1U;
        name_end = line_start;
        while (name_end < line_end && headers[name_end] != ':') name_end += 1U;
        if (name_end == line_end) continue;
        if (name_end - line_start != 17U || !ascii_equal_insensitive_n(headers + line_start, "transfer-encoding", 17U)) continue;
        value_start = name_end + 1U;
        while (value_start < line_end && (headers[value_start] == ' ' || headers[value_start] == '\t')) value_start += 1U;
        while (value_start + 7U <= line_end) {
            if (ascii_equal_insensitive_n(headers + value_start, "chunked", 7U)) return 1;
            value_start += 1U;
        }
    }
    return 0;
}

static int hex_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return (int)(ch - 'a') + 10;
    if (ch >= 'A' && ch <= 'F') return (int)(ch - 'A') + 10;
    return -1;
}

static int dechunk_http_body(unsigned char *body, size_t body_size, size_t *body_size_io) {
    size_t in = 0U;
    size_t out = 0U;

    while (1) {
        size_t chunk_size = 0U;
        int saw_digit = 0;

        while (in < body_size) {
            int value = hex_value(body[in]);
            if (value < 0) break;
            saw_digit = 1;
            if (chunk_size > (((size_t)-1) >> 4U)) return -1;
            chunk_size = (chunk_size << 4U) | (size_t)value;
            in += 1U;
        }
        if (!saw_digit) return -1;
        while (in < body_size && body[in] != '\n') in += 1U;
        if (in >= body_size) return -1;
        in += 1U;
        if (chunk_size == 0U) {
            *body_size_io = out;
            body[out] = '\0';
            return 0;
        }
        if (chunk_size > body_size - in) return -1;
        memmove(body + out, body + in, chunk_size);
        out += chunk_size;
        in += chunk_size;
        if (in < body_size && body[in] == '\r') in += 1U;
        if (in >= body_size || body[in] != '\n') return -1;
        in += 1U;
    }
}

static int write_body_output(const PgpQueryOptions *options, const unsigned char *body, size_t body_size) {
    int fd = options->output_path != 0 ? platform_open_write(options->output_path, 0644U) : 1;
    int result;

    if (fd < 0) return -1;
    result = rt_write_all(fd, body, body_size) == 0 ? 0 : -1;
    if (fd > 2 && platform_close(fd) != 0) result = -1;
    return result;
}

static int write_all_fd(int fd, const unsigned char *data, size_t size) {
    size_t written = 0U;

    while (written < size) {
        long chunk = platform_write(fd, data + written, size - written);
        if (chunk <= 0) return -1;
        written += (size_t)chunk;
    }
    return 0;
}

static int http_fetch_to_memory(const char *url_text, const PgpQueryOptions *options, unsigned char **body_out, size_t *body_size_out, int *status_out) {
    PgpQueryUrl url;
    PgpQueryConnection connection;
    char request[PGPQUERY_URL_CAPACITY + 512U];
    size_t request_length = 0U;
    char header_buffer[PGPQUERY_HEADER_CAPACITY];
    size_t header_length = 0U;
    int header_complete = 0;
    unsigned char *body = 0;
    size_t body_size = 0U;
    size_t body_capacity = 0U;
    int chunked_response = 0;
    char buffer[PGPQUERY_BUFFER_CAPACITY];
    long bytes_read;

    *body_out = 0;
    *body_size_out = 0U;
    *status_out = 0;
    if (parse_url(url_text, &url) != 0) return -1;
    if (query_connect(&url, &connection) != 0) return -1;

    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "GET ");
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, url.path[0] != '\0' ? url.path : "/");
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, " HTTP/1.1\r\nHost: ");
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, url.host);
    if (url.port != default_port_for_scheme(url.scheme)) {
        char port_text[16];
        rt_unsigned_to_string(url.port, port_text, sizeof(port_text));
        request_length = tool_buffer_append_char(request, sizeof(request), request_length, ':');
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, port_text);
    }
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\nUser-Agent: newos-pgpquery/0.1\r\nAccept: */*\r\nConnection: close\r\n\r\n");
    if (rt_strlen(request) != request_length || query_write_all(&connection, request, request_length) != 0) {
        query_close(&connection);
        return -1;
    }

    while (1) {
        if (wait_for_plain_socket(&connection, options->timeout_ms) != 0) {
            query_close(&connection);
            if (body != 0) rt_free(body);
            return -1;
        }
        bytes_read = query_read(&connection, buffer, sizeof(buffer));
        if (bytes_read < 0 && connection.use_tls && header_complete) bytes_read = 0;
        if (bytes_read <= 0) break;
        if (!header_complete) {
            size_t body_offset = 0U;
            if (header_length + (size_t)bytes_read >= sizeof(header_buffer)) {
                query_close(&connection);
                return -1;
            }
            memcpy(header_buffer + header_length, buffer, (size_t)bytes_read);
            header_length += (size_t)bytes_read;
            header_buffer[header_length] = '\0';
            if (tool_find_http_header_end(header_buffer, header_length, &body_offset) != 0) continue;
            header_complete = 1;
            {
                char saved = header_buffer[body_offset];
                header_buffer[body_offset] = '\0';
                *status_out = parse_status(header_buffer);
                chunked_response = http_headers_have_chunked_transfer(header_buffer);
                header_buffer[body_offset] = saved;
            }
            if (header_length > body_offset) {
                size_t initial = header_length - body_offset;
                body_capacity = initial + 1U;
                body = (unsigned char *)rt_malloc(body_capacity);
                if (body == 0) {
                    query_close(&connection);
                    return -1;
                }
                memcpy(body, header_buffer + body_offset, initial);
                body_size = initial;
            }
        } else {
            size_t chunk_size = (size_t)bytes_read;
            if (body_size + chunk_size > PGPQUERY_MAX_BODY_SIZE) {
                query_close(&connection);
                if (body != 0) rt_free(body);
                return -1;
            }
            if (body_size + chunk_size + 1U > body_capacity) {
                size_t next_capacity = body_capacity == 0U ? 8192U : body_capacity * 2U;
                unsigned char *resized;
                while (next_capacity < body_size + chunk_size + 1U) next_capacity *= 2U;
                resized = (unsigned char *)rt_realloc(body, next_capacity);
                if (resized == 0) {
                    query_close(&connection);
                    if (body != 0) rt_free(body);
                    return -1;
                }
                body = resized;
                body_capacity = next_capacity;
            }
            memcpy(body + body_size, buffer, chunk_size);
            body_size += chunk_size;
        }
    }
    query_close(&connection);
    if (!header_complete || bytes_read < 0) {
        if (body != 0) rt_free(body);
        return -1;
    }
    if (body == 0) {
        body = (unsigned char *)rt_malloc(1U);
        if (body == 0) return -1;
    }
    if (chunked_response && dechunk_http_body(body, body_size, &body_size) != 0) {
        rt_free(body);
        return -1;
    }
    body[body_size] = '\0';
    *body_out = body;
    *body_size_out = body_size;
    return 0;
}

static int append_url_encoded(char *buffer, size_t buffer_size, size_t *used_io, const char *text) {
    static const char hex[] = "0123456789ABCDEF";
    size_t index;
    size_t used = *used_io;

    for (index = 0U; text[index] != '\0'; ++index) {
        unsigned char ch = (unsigned char)text[index];
        int safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == 'x';
        if (safe) {
            used = tool_buffer_append_char(buffer, buffer_size, used, (char)ch);
        } else {
            used = tool_buffer_append_char(buffer, buffer_size, used, '%');
            used = tool_buffer_append_char(buffer, buffer_size, used, hex[(ch >> 4U) & 0x0fU]);
            used = tool_buffer_append_char(buffer, buffer_size, used, hex[ch & 0x0fU]);
        }
    }
    if (rt_strlen(buffer) != used) return -1;
    *used_io = used;
    return 0;
}

static const char *selector_hex_start(const char *selector) {
    if (selector[0] == '0' && (selector[1] == 'x' || selector[1] == 'X')) return selector + 2;
    return selector;
}

static int append_normalized_hex_selector(char *buffer, size_t buffer_size, size_t *used_io, const char *selector) {
    static const char hex[] = "0123456789abcdef";
    unsigned char parsed[PGP_FINGERPRINT_MAX_SIZE];
    size_t parsed_size = 0U;
    size_t index;
    size_t used = *used_io;

    if (pgp_parse_fingerprint_text(selector_hex_start(selector), parsed, &parsed_size) != 0) return -1;
    for (index = 0U; index < parsed_size; ++index) {
        used = tool_buffer_append_char(buffer, buffer_size, used, hex[(parsed[index] >> 4U) & 0x0fU]);
        used = tool_buffer_append_char(buffer, buffer_size, used, hex[parsed[index] & 0x0fU]);
    }
    if (rt_strlen(buffer) != used) return -1;
    *used_io = used;
    return 0;
}

static int selector_is_hex_key(const char *selector, size_t *bytes_out) {
    unsigned char parsed[PGP_FINGERPRINT_MAX_SIZE];
    size_t parsed_size = 0U;

    if (pgp_parse_fingerprint_text(selector_hex_start(selector), parsed, &parsed_size) != 0) return 0;
    *bytes_out = parsed_size;
    return parsed_size == PGP_KEY_ID_SIZE || parsed_size == 20U || parsed_size == 32U;
}

static int selector_is_email(const char *selector) {
    size_t index;
    int saw_at = 0;

    for (index = 0U; selector[index] != '\0'; ++index) {
        unsigned char ch = (unsigned char)selector[index];
        if (ch == '@') saw_at = 1;
        if (ch <= ' ' || ch == 0x7fU) return 0;
    }
    return saw_at;
}

static int split_email_selector(const char *selector, char *local_out, size_t local_size, char *domain_out, size_t domain_size) {
    size_t index = 0U;
    size_t at_index = 0U;
    size_t local_length;
    size_t domain_length;

    if (!selector_is_email(selector)) return -1;
    while (selector[index] != '\0' && selector[index] != '@') index += 1U;
    if (selector[index] != '@') return -1;
    at_index = index;
    local_length = at_index;
    domain_length = rt_strlen(selector + at_index + 1U);
    if (local_length == 0U || domain_length == 0U || local_length + 1U > local_size || domain_length + 1U > domain_size) return -1;
    memcpy(local_out, selector, local_length);
    local_out[local_length] = '\0';
    memcpy(domain_out, selector + at_index + 1U, domain_length);
    domain_out[domain_length] = '\0';
    return 0;
}

static void lowercase_ascii_in_place(char *text) {
    size_t index;

    for (index = 0U; text[index] != '\0'; ++index) text[index] = tool_ascii_tolower(text[index]);
}

static int append_zbase32_sha1(char *buffer, size_t buffer_size, size_t *used_io, const char *text) {
    static const char alphabet[] = "ybndrfg8ejkmcpqxot1uwisza345h769";
    unsigned char digest[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned int bits = 0U;
    unsigned int bit_count = 0U;
    size_t index;
    size_t used = *used_io;

    crypto_sha1_hash((const unsigned char *)text, rt_strlen(text), digest);
    for (index = 0U; index < sizeof(digest); ++index) {
        bits = (bits << 8U) | (unsigned int)digest[index];
        bit_count += 8U;
        while (bit_count >= 5U) {
            unsigned int value = (bits >> (bit_count - 5U)) & 0x1fU;
            used = tool_buffer_append_char(buffer, buffer_size, used, alphabet[value]);
            bit_count -= 5U;
        }
    }
    if (bit_count != 0U) {
        unsigned int value = (bits << (5U - bit_count)) & 0x1fU;
        used = tool_buffer_append_char(buffer, buffer_size, used, alphabet[value]);
    }
    if (rt_strlen(buffer) != used) return -1;
    *used_io = used;
    return 0;
}

static int build_openpgp_url(const char *selector, char *url, size_t url_size) {
    size_t parsed_size = 0U;
    size_t used = 0U;

    if (selector_is_hex_key(selector, &parsed_size)) {
        used = tool_buffer_append_cstr(url, url_size, used, parsed_size == PGP_KEY_ID_SIZE ? "https://keys.openpgp.org/vks/v1/by-keyid/" : "https://keys.openpgp.org/vks/v1/by-fingerprint/");
        if (append_normalized_hex_selector(url, url_size, &used, selector) != 0) return -1;
        return 0;
    }
    if (selector_is_email(selector)) {
        used = tool_buffer_append_cstr(url, url_size, used, "https://keys.openpgp.org/vks/v1/by-email/");
        if (append_url_encoded(url, url_size, &used, selector) != 0) return -1;
        return 0;
    }
    return -1;
}

static int build_hkp_url(const char *host, const char *selector, int get, char *url, size_t url_size) {
    size_t parsed_size = 0U;
    size_t used = 0U;

    used = tool_buffer_append_cstr(url, url_size, used, "https://");
    used = tool_buffer_append_cstr(url, url_size, used, host);
    used = tool_buffer_append_cstr(url, url_size, used, "/pks/lookup?op=");
    used = tool_buffer_append_cstr(url, url_size, used, get ? "get" : "index&options=mr");
    used = tool_buffer_append_cstr(url, url_size, used, "&search=");
    if (selector_is_hex_key(selector, &parsed_size)) {
        used = tool_buffer_append_cstr(url, url_size, used, "0x");
        if (append_normalized_hex_selector(url, url_size, &used, selector) != 0) return -1;
    } else if (!selector_is_email(selector)) {
        return -1;
    } else {
        if (append_url_encoded(url, url_size, &used, selector) != 0) return -1;
    }
    return 0;
}

static int build_wkd_url(const char *selector, int advanced, char *url, size_t url_size) {
    char local[256];
    char domain[256];
    size_t used = 0U;

    if (split_email_selector(selector, local, sizeof(local), domain, sizeof(domain)) != 0) return -1;
    lowercase_ascii_in_place(local);
    lowercase_ascii_in_place(domain);
    used = tool_buffer_append_cstr(url, url_size, used, "https://");
    if (advanced) {
        used = tool_buffer_append_cstr(url, url_size, used, "openpgpkey.");
        used = tool_buffer_append_cstr(url, url_size, used, domain);
        used = tool_buffer_append_cstr(url, url_size, used, "/.well-known/openpgpkey/");
        used = tool_buffer_append_cstr(url, url_size, used, domain);
    } else {
        used = tool_buffer_append_cstr(url, url_size, used, domain);
        used = tool_buffer_append_cstr(url, url_size, used, "/.well-known/openpgpkey");
    }
    used = tool_buffer_append_cstr(url, url_size, used, "/hu/");
    if (append_zbase32_sha1(url, url_size, &used, local) != 0) return -1;
    used = tool_buffer_append_cstr(url, url_size, used, "?l=");
    if (append_url_encoded(url, url_size, &used, local) != 0) return -1;
    return 0;
}

static int build_custom_url(const PgpQueryOptions *options, const char *selector, char *url, size_t url_size) {
    size_t used = 0U;

    used = tool_buffer_append_cstr(url, url_size, used, options->custom_base);
    if (used != 0U && url[used - 1U] != '/' && url[used - 1U] != '=') used = tool_buffer_append_char(url, url_size, used, '/');
    if (append_url_encoded(url, url_size, &used, selector) != 0) return -1;
    return 0;
}

static int write_hex_bytes(int fd, const unsigned char *data, size_t size) {
    static const char hex[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (rt_write_char(fd, hex[(data[index] >> 4U) & 0x0fU]) != 0) return -1;
        if (rt_write_char(fd, hex[data[index] & 0x0fU]) != 0) return -1;
    }
    return 0;
}

static int print_certificate_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpQueryCertificateContext *ctx = (PgpQueryCertificateContext *)ctx_ptr;
    size_t uid_index;

    ctx->count += 1;
    if (ctx->json) {
        if (tool_json_begin_event(1, "pgpquery", "stdout", "certificate") != 0) return -1;
        if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
        if (rt_write_cstr(1, "\"server\":") != 0 || tool_json_write_string(1, ctx->server_name) != 0) return -1;
        if (rt_write_cstr(1, ",\"selector\":") != 0 || tool_json_write_string(1, ctx->selector) != 0) return -1;
        if (rt_write_cstr(1, ",\"fingerprint\":\"") != 0 || write_hex_bytes(1, certificate->primary.fingerprint, certificate->primary.fingerprint_size) != 0) return -1;
        if (rt_write_cstr(1, "\",\"key_id\":\"") != 0 || write_hex_bytes(1, certificate->primary.key_id, PGP_KEY_ID_SIZE) != 0) return -1;
        if (rt_write_cstr(1, "\",\"user_ids\":[") != 0) return -1;
        for (uid_index = 0U; uid_index < certificate->user_id_count; ++uid_index) {
            if (uid_index != 0U && rt_write_char(1, ',') != 0) return -1;
            if (tool_json_write_string(1, certificate->user_ids[uid_index]) != 0) return -1;
        }
        if (rt_write_cstr(1, "]}}") != 0) return -1;
        return tool_json_end_event(1);
    }
    if (rt_write_cstr(1, "server: ") != 0 || rt_write_line(1, ctx->server_name) != 0) return -1;
    if (rt_write_cstr(1, "selector: ") != 0 || rt_write_line(1, ctx->selector) != 0) return -1;
    if (rt_write_cstr(1, "fingerprint: ") != 0 || write_hex_bytes(1, certificate->primary.fingerprint, certificate->primary.fingerprint_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "key-id: ") != 0 || write_hex_bytes(1, certificate->primary.key_id, PGP_KEY_ID_SIZE) != 0 || rt_write_char(1, '\n') != 0) return -1;
    for (uid_index = 0U; uid_index < certificate->user_id_count; ++uid_index) {
        if (rt_write_cstr(1, "uid: ") != 0 || tool_write_visible_line(1, certificate->user_ids[uid_index]) != 0) return -1;
    }
    return rt_write_char(1, '\n');
}

static int count_certificate_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    int *count = (int *)ctx_ptr;

    (void)certificate;
    *count += 1;
    return 0;
}

static int first_certificate_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpQueryFirstCertificateContext *ctx = (PgpQueryFirstCertificateContext *)ctx_ptr;

    if (!ctx->found) {
        ctx->primary = certificate->primary;
        ctx->is_secret = certificate->primary.tag == 5U;
        ctx->found = 1;
    }
    return 1;
}

static int find_certificate_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpQueryFindCertificateContext *ctx = (PgpQueryFindCertificateContext *)ctx_ptr;

    if (ctx->key->fingerprint_size == certificate->primary.fingerprint_size && memcmp(ctx->key->fingerprint, certificate->primary.fingerprint, ctx->key->fingerprint_size) == 0) {
        ctx->found = 1;
        return 1;
    }
    return 0;
}

static int print_certificate_body(const char *server_name, const char *selector, const unsigned char *body, size_t body_size, int json) {
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;
    char error[160];
    PgpQueryCertificateContext ctx;

    if (pgp_decode_input(body, body_size, &decoded, &decoded_size, error, sizeof(error)) != 0) return -1;
    ctx.server_name = server_name;
    ctx.selector = selector;
    ctx.json = json;
    ctx.count = 0;
    if (pgp_for_each_certificate(decoded, decoded_size, print_certificate_callback, &ctx, error, sizeof(error)) != 0) {
        rt_free(decoded);
        return -1;
    }
    rt_free(decoded);
    return ctx.count == 0 ? -1 : 0;
}

static int public_certificate_body_is_valid(const unsigned char *body, size_t body_size) {
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;
    char error[160];
    int count = 0;

    if (pgp_decode_input(body, body_size, &decoded, &decoded_size, error, sizeof(error)) != 0) return 0;
    if (pgp_for_each_certificate(decoded, decoded_size, count_certificate_callback, &count, error, sizeof(error)) != 0) {
        rt_free(decoded);
        return 0;
    }
    rt_free(decoded);
    return count != 0;
}

static int write_fingerprint_text(int fd, const PgpPublicKeyInfo *key) {
    return write_hex_bytes(fd, key->fingerprint, key->fingerprint_size);
}

static int keyring_contains_key(const char *keyring_path, const PgpPublicKeyInfo *key) {
    unsigned char *data = 0;
    size_t data_size = 0U;
    char error[160];
    PgpQueryFindCertificateContext ctx;

    if (tool_read_all_input(keyring_path, &data, &data_size) != 0) return 0;
    ctx.key = key;
    ctx.found = 0;
    (void)pgp_for_each_certificate(data, data_size, find_certificate_callback, &ctx, error, sizeof(error));
    rt_free(data);
    return ctx.found;
}

static int import_certificate_body(const PgpQueryOptions *options, const unsigned char *body, size_t body_size) {
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;
    char error[160];
    PgpQueryFirstCertificateContext first;
    int fd;

    if (pgp_decode_input(body, body_size, &decoded, &decoded_size, error, sizeof(error)) != 0) {
        tool_write_error("pgpquery", "cannot decode fetched key: ", error);
        return -1;
    }
    rt_memset(&first, 0, sizeof(first));
    if (pgp_for_each_certificate(decoded, decoded_size, first_certificate_callback, &first, error, sizeof(error)) != 0 && !first.found) {
        rt_free(decoded);
        tool_write_error("pgpquery", "cannot parse fetched key: ", error);
        return -1;
    }
    if (!first.found || first.is_secret) {
        rt_free(decoded);
        tool_write_error("pgpquery", "fetched data is not an importable public key", 0);
        return -1;
    }
    if (keyring_contains_key(options->import_keyring_path, &first.primary)) {
        if (rt_write_cstr(1, "unchanged: ") != 0 || write_fingerprint_text(1, &first.primary) != 0 || rt_write_char(1, '\n') != 0) {
            rt_free(decoded);
            return -1;
        }
        rt_free(decoded);
        return 0;
    }
    fd = platform_open_append(options->import_keyring_path, 0600U);
    if (fd < 0 || write_all_fd(fd, decoded, decoded_size) != 0 || platform_close(fd) != 0) {
        if (fd >= 0) (void)platform_close(fd);
        rt_free(decoded);
        tool_write_error("pgpquery", "cannot write keyring: ", options->import_keyring_path);
        return -1;
    }
    rt_free(decoded);
    return rt_write_cstr(1, "imported: ") != 0 || write_fingerprint_text(1, &first.primary) != 0 || rt_write_char(1, '\n') != 0 ? -1 : 0;
}

static size_t field_end(const char *text, size_t start) {
    while (text[start] != '\0' && text[start] != ':' && text[start] != '\n' && text[start] != '\r') start += 1U;
    return start;
}

static int field_equals(const char *text, size_t start, size_t end, const char *value) {
    size_t index = 0U;
    while (start + index < end && value[index] != '\0') {
        if (text[start + index] != value[index]) return 0;
        index += 1U;
    }
    return start + index == end && value[index] == '\0';
}

static int write_field_json_string(const char *text, size_t start, size_t end) {
    return tool_json_write_string_n(1, text + start, end - start);
}

static int parse_hkp_index(const char *server_name, const char *selector, const char *body, int json, int *found_out) {
    size_t offset = 0U;
    int have_pub = 0;

    *found_out = 0;
    while (body[offset] != '\0') {
        size_t line_start = offset;
        size_t line_end;
        size_t tag_end;

        while (body[offset] != '\0' && body[offset] != '\n') offset += 1U;
        line_end = offset;
        if (body[offset] == '\n') offset += 1U;
        if (line_end > line_start && body[line_end - 1U] == '\r') line_end -= 1U;
        tag_end = field_end(body, line_start);
        if (field_equals(body, line_start, tag_end, "pub")) {
            size_t fpr_start = tag_end + 1U;
            size_t fpr_end = field_end(body, fpr_start);
            size_t alg_start = fpr_end + 1U;
            size_t alg_end = field_end(body, alg_start);
            size_t bits_start = alg_end + 1U;
            size_t bits_end = field_end(body, bits_start);
            size_t created_start = bits_end + 1U;
            size_t created_end = field_end(body, created_start);

            have_pub = 1;
            *found_out = 1;
            if (json) {
                if (tool_json_begin_event(1, "pgpquery", "stdout", "hkp_index") != 0) return -1;
                if (rt_write_cstr(1, ",\"data\":{\"server\":") != 0 || tool_json_write_string(1, server_name) != 0) return -1;
                if (rt_write_cstr(1, ",\"selector\":") != 0 || tool_json_write_string(1, selector) != 0) return -1;
                if (rt_write_cstr(1, ",\"fingerprint\":") != 0 || write_field_json_string(body, fpr_start, fpr_end) != 0) return -1;
                if (rt_write_cstr(1, ",\"algorithm\":") != 0 || write_field_json_string(body, alg_start, alg_end) != 0) return -1;
                if (rt_write_cstr(1, ",\"bits\":") != 0 || write_field_json_string(body, bits_start, bits_end) != 0) return -1;
                if (rt_write_cstr(1, ",\"created\":") != 0 || write_field_json_string(body, created_start, created_end) != 0 || rt_write_char(1, '}') != 0 || tool_json_end_event(1) != 0) return -1;
            } else {
                if (rt_write_cstr(1, "server: ") != 0 || rt_write_line(1, server_name) != 0) return -1;
                if (rt_write_cstr(1, "selector: ") != 0 || rt_write_line(1, selector) != 0) return -1;
                if (rt_write_cstr(1, "fingerprint: ") != 0 || rt_write_all(1, body + fpr_start, fpr_end - fpr_start) != 0 || rt_write_char(1, '\n') != 0) return -1;
                if (rt_write_cstr(1, "algorithm: ") != 0 || rt_write_all(1, body + alg_start, alg_end - alg_start) != 0 || rt_write_cstr(1, ", bits: ") != 0 || rt_write_all(1, body + bits_start, bits_end - bits_start) != 0 || rt_write_char(1, '\n') != 0) return -1;
                if (rt_write_cstr(1, "created: ") != 0 || rt_write_all(1, body + created_start, created_end - created_start) != 0 || rt_write_char(1, '\n') != 0) return -1;
            }
        } else if (have_pub && field_equals(body, line_start, tag_end, "uid")) {
            size_t uid_start = tag_end + 1U;
            size_t uid_end = field_end(body, uid_start);
            if (json) {
                if (tool_json_begin_event(1, "pgpquery", "stdout", "hkp_uid") != 0) return -1;
                if (rt_write_cstr(1, ",\"data\":{\"server\":") != 0 || tool_json_write_string(1, server_name) != 0) return -1;
                if (rt_write_cstr(1, ",\"selector\":") != 0 || tool_json_write_string(1, selector) != 0) return -1;
                if (rt_write_cstr(1, ",\"uid\":") != 0 || write_field_json_string(body, uid_start, uid_end) != 0 || rt_write_char(1, '}') != 0 || tool_json_end_event(1) != 0) return -1;
            } else {
                if (rt_write_cstr(1, "uid: ") != 0 || rt_write_all(1, body + uid_start, uid_end - uid_start) != 0 || rt_write_char(1, '\n') != 0) return -1;
            }
        }
    }
    if (have_pub && !json) return rt_write_char(1, '\n');
    return 0;
}

static int fetch_and_print(const char *server_name, const char *url, const char *selector, const PgpQueryOptions *options, int armored_response, int quiet_failure) {
    unsigned char *body = 0;
    size_t body_size = 0U;
    int status = 0;
    int result = 1;

    if (http_fetch_to_memory(url, options, &body, &body_size, &status) != 0) {
        if (!quiet_failure) tool_write_error("pgpquery", "request failed: ", url);
        return 1;
    }
    if (status == 404) {
        if (!quiet_failure && options->get) {
            tool_write_error("pgpquery", "not found: ", selector);
        }
        if (!quiet_failure && !options->json && !options->get) {
            rt_write_cstr(1, "server: ");
            rt_write_line(1, server_name);
            rt_write_cstr(1, "selector: ");
            rt_write_line(1, selector);
            rt_write_line(1, "status: not found\n");
        }
        rt_free(body);
        return 1;
    }
    if (status < 200 || status >= 300) {
        if (!quiet_failure) tool_write_error("pgpquery", "server returned non-success for: ", url);
        rt_free(body);
        return 1;
    }
    if (options->get) {
        if (armored_response && !public_certificate_body_is_valid(body, body_size)) {
            if (!quiet_failure) tool_write_error("pgpquery", "server response is not an importable public key: ", selector);
        } else if (options->import_keyring_path != 0) {
            result = import_certificate_body(options, body, body_size) == 0 ? 0 : 1;
        } else if (write_body_output(options, body, body_size) == 0) {
            result = 0;
        } else if (!quiet_failure) {
            tool_write_error("pgpquery", "cannot write output: ", options->output_path != 0 ? options->output_path : "stdout");
        }
    } else if (armored_response) {
        result = print_certificate_body(server_name, selector, body, body_size, options->json) == 0 ? 0 : 1;
    } else {
        int found = 0;
        result = parse_hkp_index(server_name, selector, (const char *)body, options->json, &found) == 0 && found ? 0 : 1;
    }
    rt_free(body);
    return result;
}

static int print_query_url(const char *server_name, const char *selector, const char *url, int json) {
    if (json) {
        if (tool_json_begin_event(1, "pgpquery", "stdout", "query_url") != 0) return -1;
        if (rt_write_cstr(1, ",\"data\":{\"server\":") != 0 || tool_json_write_string(1, server_name) != 0) return -1;
        if (rt_write_cstr(1, ",\"selector\":") != 0 || tool_json_write_string(1, selector) != 0) return -1;
        if (rt_write_cstr(1, ",\"url\":") != 0 || tool_json_write_string(1, url) != 0) return -1;
        if (rt_write_cstr(1, "}") != 0 || tool_json_end_event(1) != 0) return -1;
        return 0;
    }
    if (rt_write_cstr(1, server_name) != 0 || rt_write_cstr(1, ": ") != 0 || rt_write_line(1, url) != 0) return -1;
    return 0;
}

static int query_selector(const PgpQueryOptions *options, const char *selector) {
    char url[PGPQUERY_URL_CAPACITY];
    int status = 1;
    int attempted = 0;

    if (options->server == PGPQUERY_SERVER_OPENPGP || options->server == PGPQUERY_SERVER_ALL) {
        if (!options->get && build_openpgp_url(selector, url, sizeof(url)) == 0) {
            attempted = 1;
            if (options->print_url) {
                if (print_query_url("keys.openpgp.org", selector, url, options->json) == 0) status = 0;
            } else if (fetch_and_print("keys.openpgp.org", url, selector, options, 1, 0) == 0) status = 0;
        }
    }
    if (options->server == PGPQUERY_SERVER_UBUNTU || options->server == PGPQUERY_SERVER_ALL) {
        if (build_hkp_url("keyserver.ubuntu.com", selector, options->get, url, sizeof(url)) == 0) {
            attempted = 1;
            if (options->print_url) {
                if (print_query_url("keyserver.ubuntu.com", selector, url, options->json) == 0) status = 0;
            } else if (fetch_and_print("keyserver.ubuntu.com", url, selector, options, options->get, 0) == 0) status = 0;
        }
    }
    if (options->server == PGPQUERY_SERVER_MAILVELOPE) {
        if (build_hkp_url("keys.mailvelope.com", selector, options->get, url, sizeof(url)) == 0) {
            attempted = 1;
            if (options->print_url) {
                if (print_query_url("keys.mailvelope.com", selector, url, options->json) == 0) status = 0;
            } else if (fetch_and_print("keys.mailvelope.com", url, selector, options, options->get, 0) == 0) status = 0;
        }
    }
    if (options->server == PGPQUERY_SERVER_MIT) {
        if (build_hkp_url("pgp.mit.edu", selector, options->get, url, sizeof(url)) == 0) {
            attempted = 1;
            if (options->print_url) {
                if (print_query_url("pgp.mit.edu", selector, url, options->json) == 0) status = 0;
            } else if (fetch_and_print("pgp.mit.edu", url, selector, options, options->get, 0) == 0) status = 0;
        }
    }
    if (options->server == PGPQUERY_SERVER_WKD) {
        if (build_wkd_url(selector, 1, url, sizeof(url)) == 0) {
            attempted = 1;
            if (options->print_url) {
                if (print_query_url("wkd-advanced", selector, url, options->json) == 0) status = 0;
            } else if (fetch_and_print("wkd-advanced", url, selector, options, 1, 1) == 0) status = 0;
        }
        if (build_wkd_url(selector, 0, url, sizeof(url)) == 0) {
            attempted = 1;
            if (options->print_url) {
                if (print_query_url("wkd-direct", selector, url, options->json) == 0) status = 0;
            } else if (status != 0 && fetch_and_print("wkd-direct", url, selector, options, 1, 0) == 0) status = 0;
        }
    }
    if (options->server == PGPQUERY_SERVER_CUSTOM) {
        if (build_custom_url(options, selector, url, sizeof(url)) == 0) {
            attempted = 1;
            if (options->print_url) {
                if (print_query_url(options->custom_base, selector, url, options->json) == 0) status = 0;
            } else if (fetch_and_print(options->custom_base, url, selector, options, options->get, 0) == 0) status = 0;
        }
    }
    if (!attempted) tool_write_error("pgpquery", "unsupported selector for selected server: ", selector);
    return status;
}

static int parse_server_option(PgpQueryOptions *options, const char *value) {
    if (rt_strcmp(value, "all") == 0) options->server = PGPQUERY_SERVER_ALL;
    else if (rt_strcmp(value, "openpgp") == 0 || rt_strcmp(value, "keys.openpgp.org") == 0) options->server = PGPQUERY_SERVER_OPENPGP;
    else if (rt_strcmp(value, "ubuntu") == 0 || rt_strcmp(value, "keyserver.ubuntu.com") == 0) options->server = PGPQUERY_SERVER_UBUNTU;
    else if (rt_strcmp(value, "mailvelope") == 0 || rt_strcmp(value, "keys.mailvelope.com") == 0) options->server = PGPQUERY_SERVER_MAILVELOPE;
    else if (rt_strcmp(value, "mit") == 0 || rt_strcmp(value, "pgp.mit.edu") == 0) options->server = PGPQUERY_SERVER_MIT;
    else if (rt_strcmp(value, "wkd") == 0) options->server = PGPQUERY_SERVER_WKD;
    else if (tool_starts_with(value, "http://") || tool_starts_with(value, "https://")) {
        options->server = PGPQUERY_SERVER_CUSTOM;
        rt_copy_string(options->custom_base, sizeof(options->custom_base), value);
    } else {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    PgpQueryOptions options;
    ToolOptState opt;
    int option_result;
    int status = 0;
    int index;

    rt_memset(&options, 0, sizeof(options));
    options.server = PGPQUERY_SERVER_ALL;
    options.timeout_ms = PGPQUERY_DEFAULT_TIMEOUT_MS;
    tool_opt_init(&opt, argc, argv, "pgpquery", PGPQUERY_USAGE);
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--server") == 0 || rt_strcmp(opt.flag, "-s") == 0) {
            if (tool_opt_require_value(&opt) != 0 || parse_server_option(&options, opt.value) != 0) {
                print_usage();
                return 1;
            }
        } else if (tool_starts_with(opt.flag, "--server=")) {
            if (parse_server_option(&options, opt.flag + 9) != 0) {
                print_usage();
                return 1;
            }
        } else if (rt_strcmp(opt.flag, "--json") == 0) {
            options.json = 1;
        } else if (rt_strcmp(opt.flag, "--get") == 0) {
            options.get = 1;
        } else if (rt_strcmp(opt.flag, "--output") == 0 || rt_strcmp(opt.flag, "-o") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.output_path = opt.value;
        } else if (tool_starts_with(opt.flag, "--output=")) {
            options.output_path = opt.flag + 9;
        } else if (rt_strcmp(opt.flag, "--import-keyring") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.import_keyring_path = opt.value;
        } else if (tool_starts_with(opt.flag, "--import-keyring=")) {
            options.import_keyring_path = opt.flag + 17;
        } else if (rt_strcmp(opt.flag, "--print-url") == 0) {
            options.print_url = 1;
        } else if (rt_strcmp(opt.flag, "--timeout") == 0 || rt_strcmp(opt.flag, "-T") == 0) {
            if (tool_opt_require_value(&opt) != 0 || tool_parse_duration_ms(opt.value, &options.timeout_ms) != 0) {
                print_usage();
                return 1;
            }
        } else if (tool_starts_with(opt.flag, "--timeout=")) {
            if (tool_parse_duration_ms(opt.flag + 10, &options.timeout_ms) != 0) {
                print_usage();
                return 1;
            }
        } else {
            tool_write_error("pgpquery", "unknown option: ", opt.flag);
            print_usage();
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        print_usage();
        return 0;
    }
    if (opt.argi >= argc) {
        print_usage();
        return 1;
    }
    options.json = tool_json_is_enabled();
    if (options.output_path != 0 && !options.get) {
        tool_write_error("pgpquery", "--output requires --get", 0);
        return 1;
    }
    if (options.import_keyring_path != 0 && !options.get) {
        tool_write_error("pgpquery", "--import-keyring requires --get", 0);
        return 1;
    }
    if (options.output_path != 0 && options.import_keyring_path != 0) {
        tool_write_error("pgpquery", "--output cannot be used with --import-keyring", 0);
        return 1;
    }
    if (options.output_path != 0 && opt.argi + 1 < argc) {
        tool_write_error("pgpquery", "--output accepts one selector", 0);
        return 1;
    }
    if ((options.output_path != 0 || options.import_keyring_path != 0) && options.print_url) {
        tool_write_error("pgpquery", "output/import options cannot be used with --print-url", 0);
        return 1;
    }
    if (options.get && (options.server == PGPQUERY_SERVER_ALL || options.server == PGPQUERY_SERVER_OPENPGP)) {
        options.server = PGPQUERY_SERVER_UBUNTU;
    }
    for (index = opt.argi; index < argc; ++index) {
        if (query_selector(&options, argv[index]) != 0) status = 1;
    }
    return status;
}
