#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "crypto/sha256.h"

#define WP_SCHEME_HTTP 1
#define WP_SCHEME_HTTPS 2
#define WP_BUFFER_SIZE 16384U
#define WP_HEADER_SIZE 32768U
#define WP_PAGE_MAX_SIZE 262144U
#define WP_URL_SIZE 2048U
#define WP_PATH_SIZE 1024U
#define WP_NAME_SIZE 256U
#define WP_HASH_HEX_SIZE 65U
#define WP_MAX_FILES 64U
#define WP_DEFAULT_TIMEOUT_MS 30000ULL
#define WP_PROGRESS_INTERVAL_NS 1000000000ULL

typedef struct {
    int scheme;
    unsigned int port;
    char host[256];
    char path[1024];
} WpUrl;

typedef struct {
    const char *out_dir;
    const char *date;
    unsigned long long timeout_ms;
    int quiet;
} WpOptions;

typedef struct {
    char name[WP_NAME_SIZE];
    char expected_hex[WP_HASH_HEX_SIZE];
    unsigned long long content_length;
    int has_content_length;
} WpDumpFile;

typedef struct {
    WpDumpFile files[WP_MAX_FILES];
    size_t count;
} WpDumpList;

typedef struct {
    int status_code;
    char location[WP_URL_SIZE];
    unsigned long long content_length;
    int has_content_length;
} WpHttpHeaders;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-q] [-o DIR] [--date YYYY-MM-DD] LANG");
}

static void write_info(const char *message, const char *detail) {
    rt_write_cstr(2, "wp-download: ");
    rt_write_cstr(2, message);
    if (detail != 0) {
        rt_write_cstr(2, detail);
    }
    rt_write_char(2, '\n');
}

static int append_cstr_checked(char *buffer, size_t buffer_size, size_t *length_io, const char *text) {
    size_t next = tool_buffer_append_cstr(buffer, buffer_size, *length_io, text);
    if (next == *length_io && text[0] != '\0') {
        return -1;
    }
    *length_io = next;
    return 0;
}

static int append_char_checked(char *buffer, size_t buffer_size, size_t *length_io, char ch) {
    size_t next = tool_buffer_append_char(buffer, buffer_size, *length_io, ch);
    if (next == *length_io) {
        return -1;
    }
    *length_io = next;
    return 0;
}

static int ends_with(const char *text, const char *suffix) {
    size_t text_length = rt_strlen(text);
    size_t suffix_length = rt_strlen(suffix);

    if (suffix_length > text_length) {
        return 0;
    }
    return rt_strcmp(text + text_length - suffix_length, suffix) == 0;
}

static int is_dump_filename_for_language(const char *name, const char *wiki_name) {
    size_t wiki_length = rt_strlen(wiki_name);

    if (rt_strncmp(name, wiki_name, wiki_length) != 0) {
        return 0;
    }
    return name[wiki_length] == '-' && ends_with(name, ".xml.bz2");
}

static int is_valid_lang_code(const char *text) {
    size_t index;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        char ch = text[index];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) {
            return 0;
        }
    }
    return 1;
}

static int make_wiki_name(const char *lang, char *buffer, size_t buffer_size) {
    size_t length = 0U;

    length = tool_buffer_append_cstr(buffer, buffer_size, length, lang);
    length = tool_buffer_append_cstr(buffer, buffer_size, length, "wiki");
    return rt_strlen(buffer) == length ? 0 : -1;
}

static int parse_http_url_tail(const char *text, unsigned int default_port, int scheme, WpUrl *url_out) {
    size_t index = 0U;
    size_t host_start;
    size_t host_length;
    unsigned long long parsed_port = default_port;
    int saw_port_digit = 0;

    rt_memset(url_out, 0, sizeof(*url_out));
    url_out->scheme = scheme;
    url_out->port = default_port;

    if (text[0] == '[') {
        host_start = 1U;
        while (text[index] != '\0' && text[index] != ']') {
            index += 1U;
        }
        if (text[index] != ']') {
            return -1;
        }
        host_length = index - host_start;
        index += 1U;
    } else {
        host_start = 0U;
        while (text[index] != '\0' && text[index] != '/' && text[index] != ':' && text[index] != '?' && text[index] != '#') {
            index += 1U;
        }
        host_length = index;
    }

    if (host_length == 0U || host_length + 1U > sizeof(url_out->host)) {
        return -1;
    }
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
        if (!saw_port_digit || parsed_port == 0ULL || parsed_port > 65535ULL) {
            return -1;
        }
    }
    url_out->port = (unsigned int)parsed_port;

    if (text[index] == '\0' || text[index] == '?' || text[index] == '#') {
        rt_copy_string(url_out->path, sizeof(url_out->path), "/");
    } else {
        rt_copy_string(url_out->path, sizeof(url_out->path), text + index);
    }
    return 0;
}

static int parse_url(const char *text, WpUrl *url_out) {
    if (tool_starts_with(text, "https://")) {
        return parse_http_url_tail(text + 8, 443U, WP_SCHEME_HTTPS, url_out);
    }
    if (tool_starts_with(text, "http://")) {
        return parse_http_url_tail(text + 7, 80U, WP_SCHEME_HTTP, url_out);
    }
    return -1;
}

static int build_url(const char *wiki_name, const char *date, const char *leaf, char *buffer, size_t buffer_size) {
    size_t length = 0U;

    buffer[0] = '\0';
    if (append_cstr_checked(buffer, buffer_size, &length, "https://dumps.wikimedia.org/other/mediawiki_content_current/") != 0 ||
        append_cstr_checked(buffer, buffer_size, &length, wiki_name) != 0 ||
        append_char_checked(buffer, buffer_size, &length, '/') != 0) {
        return -1;
    }
    if (date != 0) {
        if (append_cstr_checked(buffer, buffer_size, &length, date) != 0 ||
            append_cstr_checked(buffer, buffer_size, &length, "/xml/bzip2/") != 0) {
            return -1;
        }
        if (leaf != 0 && append_cstr_checked(buffer, buffer_size, &length, leaf) != 0) {
            return -1;
        }
    }
    return 0;
}

static int find_line_end(const char *text, size_t start) {
    size_t index = start;

    while (text[index] != '\0' && text[index] != '\n') {
        index += 1U;
    }
    return (int)index;
}

static int header_name_equals(const char *line, size_t name_length, const char *name) {
    size_t index = 0U;

    while (index < name_length && name[index] != '\0') {
        if (tool_ascii_tolower(line[index]) != tool_ascii_tolower(name[index])) {
            return 0;
        }
        index += 1U;
    }
    return index == name_length && name[index] == '\0';
}

static int parse_header_u64(const char *text, size_t length, unsigned long long *value_out) {
    size_t index = 0U;
    unsigned long long value = 0ULL;
    int saw_digit = 0;

    while (index < length && (text[index] == ' ' || text[index] == '\t')) {
        index += 1U;
    }
    while (index < length && text[index] >= '0' && text[index] <= '9') {
        unsigned long long digit = (unsigned long long)(text[index] - '0');
        unsigned long long next = value * 10ULL + digit;
        if (next < value) {
            return -1;
        }
        value = next;
        saw_digit = 1;
        index += 1U;
    }
    while (index < length && (text[index] == ' ' || text[index] == '\t')) {
        index += 1U;
    }
    if (!saw_digit || index != length) {
        return -1;
    }
    *value_out = value;
    return 0;
}

static int copy_header_value(const char *value, size_t value_length, char *out, size_t out_size) {
    size_t start = 0U;
    size_t end = value_length;
    size_t index;

    while (start < end && (value[start] == ' ' || value[start] == '\t')) {
        start += 1U;
    }
    while (end > start && (value[end - 1U] == ' ' || value[end - 1U] == '\t')) {
        end -= 1U;
    }
    if (end - start >= out_size) {
        return -1;
    }
    for (index = start; index < end; ++index) {
        unsigned char ch = (unsigned char)value[index];
        if (ch <= ' ' || ch == 0x7fU) {
            return -1;
        }
        out[index - start] = value[index];
    }
    out[end - start] = '\0';
    return 0;
}

static int parse_http_headers(const char *headers, WpHttpHeaders *parsed) {
    size_t line_start = 0U;
    int line_index = 0;

    rt_memset(parsed, 0, sizeof(*parsed));
    parsed->status_code = tool_parse_http_status(headers);
    if (parsed->status_code < 0) {
        return -1;
    }

    while (headers[line_start] != '\0') {
        size_t line_end = (size_t)find_line_end(headers, line_start);
        size_t length = line_end > line_start ? line_end - line_start : 0U;

        if (length > 0U && headers[line_end - 1U] == '\r') {
            length -= 1U;
        }
        if (line_index > 0 && length > 0U) {
            size_t colon_index = 0U;

            while (colon_index < length && headers[line_start + colon_index] != ':') {
                colon_index += 1U;
            }
            if (colon_index < length) {
                size_t name_end = colon_index;
                const char *value = headers + line_start + colon_index + 1U;
                size_t value_length = length - colon_index - 1U;

                while (name_end > 0U && (headers[line_start + name_end - 1U] == ' ' || headers[line_start + name_end - 1U] == '\t')) {
                    name_end -= 1U;
                }
                if (header_name_equals(headers + line_start, name_end, "Location")) {
                    if (copy_header_value(value, value_length, parsed->location, sizeof(parsed->location)) != 0) {
                        return -1;
                    }
                } else if (header_name_equals(headers + line_start, name_end, "Content-Length")) {
                    if (parse_header_u64(value, value_length, &parsed->content_length) != 0) {
                        return -1;
                    }
                    parsed->has_content_length = 1;
                }
            }
        }
        if (headers[line_end] == '\0') {
            break;
        }
        line_start = line_end + 1U;
        line_index += 1;
    }
    return 0;
}

static int maybe_wait_for_socket(int socket_fd, unsigned long long timeout_ms) {
    int fds[1];
    size_t ready_index = 0U;

    if (timeout_ms == 0ULL) {
        return 0;
    }
    fds[0] = socket_fd;
    return platform_poll_fds(fds, 1U, &ready_index, (int)timeout_ms) > 0 ? 0 : -1;
}

static int http_request_start(ToolHttpConnection *connection, const WpUrl *url) {
    char request[2048];
    char port_text[16];
    size_t length = 0U;

    length = tool_buffer_append_cstr(request, sizeof(request), length, "GET ");
    length = tool_buffer_append_cstr(request, sizeof(request), length, url->path[0] != '\0' ? url->path : "/");
    length = tool_buffer_append_cstr(request, sizeof(request), length, url->scheme == WP_SCHEME_HTTPS ? " HTTP/1.1\r\nHost: " : " HTTP/1.0\r\nHost: ");
    length = tool_buffer_append_cstr(request, sizeof(request), length, url->host);
    if (url->port != tool_http_default_port(url->scheme == WP_SCHEME_HTTPS)) {
        rt_unsigned_to_string(url->port, port_text, sizeof(port_text));
        length = tool_buffer_append_char(request, sizeof(request), length, ':');
        length = tool_buffer_append_cstr(request, sizeof(request), length, port_text);
    }
    length = tool_buffer_append_cstr(request, sizeof(request), length, "\r\nUser-Agent: newos-wp-download/0.1\r\nAccept: */*\r\nConnection: close\r\n\r\n");
    if (rt_strlen(request) != length) {
        return -1;
    }
    return tool_http_connection_write_all(connection, request, length);
}

static int http_connect_and_request(const char *url_text, ToolHttpConnection *connection, WpUrl *url_out) {
    if (parse_url(url_text, url_out) != 0) {
        return -1;
    }
    if (tool_http_connection_connect(connection, url_out->host, url_out->port, url_out->scheme == WP_SCHEME_HTTPS) != 0) {
        return -1;
    }
    if (http_request_start(connection, url_out) != 0) {
        tool_http_connection_close(connection);
        return -1;
    }
    return 0;
}

static int read_http_to_memory(const char *url_text, unsigned char **data_out, size_t *size_out, unsigned long long timeout_ms) {
    ToolHttpConnection connection;
    WpUrl url;
    WpHttpHeaders headers;
    unsigned char *data = 0;
    size_t size = 0U;
    size_t capacity = 0U;
    char header_buffer[WP_HEADER_SIZE];
    size_t header_length = 0U;
    int header_complete = 0;
    char buffer[WP_BUFFER_SIZE];
    long bytes_read;

    *data_out = 0;
    *size_out = 0U;
    if (http_connect_and_request(url_text, &connection, &url) != 0) {
        return -1;
    }

    for (;;) {
        if (!connection.use_tls && maybe_wait_for_socket(tool_http_connection_fd(&connection), timeout_ms) != 0) {
            bytes_read = -1;
            break;
        }
        bytes_read = tool_http_connection_read(&connection, buffer, sizeof(buffer));
        if (bytes_read < 0 && connection.use_tls && header_complete) {
            bytes_read = 0;
            break;
        }
        if (bytes_read <= 0) {
            break;
        }

        if (!header_complete) {
            size_t body_offset = 0U;
            if (header_length + (size_t)bytes_read >= sizeof(header_buffer)) {
                tool_http_connection_close(&connection);
                return -1;
            }
            memcpy(header_buffer + header_length, buffer, (size_t)bytes_read);
            header_length += (size_t)bytes_read;
            header_buffer[header_length] = '\0';
            if (tool_find_http_header_end(header_buffer, header_length, &body_offset) != 0) {
                continue;
            }
            header_complete = 1;
            {
                char saved = header_buffer[body_offset];
                header_buffer[body_offset] = '\0';
                if (parse_http_headers(header_buffer, &headers) != 0 || headers.status_code < 200 || headers.status_code >= 300) {
                    tool_http_connection_close(&connection);
                    return -1;
                }
                header_buffer[body_offset] = saved;
            }
            if (headers.has_content_length && headers.content_length > WP_PAGE_MAX_SIZE) {
                tool_http_connection_close(&connection);
                return -1;
            }
            if (header_length > body_offset) {
                size_t body_size = header_length - body_offset;
                if (size + body_size > WP_PAGE_MAX_SIZE) {
                    tool_http_connection_close(&connection);
                    return -1;
                }
                data = (unsigned char *)rt_realloc(data, size + body_size + 1U);
                if (data == 0) {
                    tool_http_connection_close(&connection);
                    return -1;
                }
                memcpy(data + size, header_buffer + body_offset, body_size);
                size += body_size;
                capacity = size + 1U;
            }
        } else {
            if (size + (size_t)bytes_read > WP_PAGE_MAX_SIZE) {
                rt_free(data);
                tool_http_connection_close(&connection);
                return -1;
            }
            if (size + (size_t)bytes_read + 1U > capacity) {
                size_t next_capacity = capacity == 0U ? 4096U : capacity * 2U;
                while (next_capacity < size + (size_t)bytes_read + 1U) {
                    next_capacity *= 2U;
                }
                if (next_capacity > WP_PAGE_MAX_SIZE + 1U) {
                    next_capacity = WP_PAGE_MAX_SIZE + 1U;
                }
                data = (unsigned char *)rt_realloc(data, next_capacity);
                if (data == 0) {
                    tool_http_connection_close(&connection);
                    return -1;
                }
                capacity = next_capacity;
            }
            memcpy(data + size, buffer, (size_t)bytes_read);
            size += (size_t)bytes_read;
        }
    }

    tool_http_connection_close(&connection);
    if (!header_complete || bytes_read < 0) {
        rt_free(data);
        return -1;
    }
    if (data == 0) {
        data = (unsigned char *)rt_malloc(1U);
        if (data == 0) {
            return -1;
        }
    }
    data[size] = '\0';
    *data_out = data;
    *size_out = size;
    return 0;
}

static int is_date_link(const char *name) {
    size_t index;

    if (rt_strlen(name) != 11U || name[10] != '/') {
        return 0;
    }
    for (index = 0U; index < 10U; ++index) {
        if (index == 4U || index == 7U) {
            if (name[index] != '-') {
                return 0;
            }
        } else if (!tool_ascii_is_digit(name[index])) {
            return 0;
        }
    }
    return 1;
}

static int html_name_is_safe(const char *name, size_t length) {
    size_t index;

    if (length == 0U || length >= WP_NAME_SIZE) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        unsigned char ch = (unsigned char)name[index];
        if (ch <= ' ' || ch == '"' || ch == '\'' || ch == '<' || ch == '>' || ch == '&') {
            return 0;
        }
    }
    return 1;
}

static int copy_html_link_name(const char *page, size_t start, size_t end, char *out, size_t out_size) {
    size_t index;
    size_t length = end - start;

    if (!html_name_is_safe(page + start, length) || length + 1U > out_size) {
        return -1;
    }
    for (index = 0U; index < length; ++index) {
        out[index] = page[start + index];
    }
    out[length] = '\0';
    return 0;
}

static int find_latest_date(const char *page, char *date_out, size_t date_size) {
    size_t index = 0U;
    char best[16];

    best[0] = '\0';
    while (page[index] != '\0') {
        if (page[index] == 'h' && rt_strncmp(page + index, "href=\"", 6U) == 0) {
            size_t name_start = index + 6U;
            size_t name_end = name_start;
            char name[WP_NAME_SIZE];

            while (page[name_end] != '\0' && page[name_end] != '"') {
                name_end += 1U;
            }
            if (page[name_end] == '"' && copy_html_link_name(page, name_start, name_end, name, sizeof(name)) == 0 && is_date_link(name)) {
                name[10] = '\0';
                if (best[0] == '\0' || rt_strcmp(name, best) > 0) {
                    rt_copy_string(best, sizeof(best), name);
                }
            }
            index = name_end;
        }
        if (page[index] != '\0') {
            index += 1U;
        }
    }
    if (best[0] == '\0') {
        return -1;
    }
    rt_copy_string(date_out, date_size, best);
    return 0;
}

static int list_has_file(const WpDumpList *list, const char *name) {
    size_t index;

    for (index = 0U; index < list->count; ++index) {
        if (rt_strcmp(list->files[index].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int add_list_file(WpDumpList *list, const char *name) {
    if (list->count >= WP_MAX_FILES || list_has_file(list, name)) {
        return list_has_file(list, name) ? 0 : -1;
    }
    rt_copy_string(list->files[list->count].name, sizeof(list->files[list->count].name), name);
    list->files[list->count].expected_hex[0] = '\0';
    list->files[list->count].content_length = 0ULL;
    list->files[list->count].has_content_length = 0;
    list->count += 1U;
    return 0;
}

static int parse_bzip2_listing(const char *page, const char *wiki_name, WpDumpList *list) {
    size_t index = 0U;

    rt_memset(list, 0, sizeof(*list));
    while (page[index] != '\0') {
        if (page[index] == 'h' && rt_strncmp(page + index, "href=\"", 6U) == 0) {
            size_t name_start = index + 6U;
            size_t name_end = name_start;
            char name[WP_NAME_SIZE];

            while (page[name_end] != '\0' && page[name_end] != '"') {
                name_end += 1U;
            }
            if (page[name_end] == '"' && copy_html_link_name(page, name_start, name_end, name, sizeof(name)) == 0 && is_dump_filename_for_language(name, wiki_name)) {
                if (add_list_file(list, name) != 0) {
                    return -1;
                }
            }
            index = name_end;
        }
        if (page[index] != '\0') {
            index += 1U;
        }
    }
    return list->count == 0U ? -1 : 0;
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return (int)(ch - 'a') + 10;
    if (ch >= 'A' && ch <= 'F') return (int)(ch - 'A') + 10;
    return -1;
}

static int is_sha256_hex(const char *text) {
    size_t index;

    for (index = 0U; index < 64U; ++index) {
        if (hex_value(text[index]) < 0) {
            return 0;
        }
    }
    return text[64] == '\0';
}

static int set_expected_hash(WpDumpList *list, const char *name, const char *hash_hex) {
    size_t index;

    for (index = 0U; index < list->count; ++index) {
        if (rt_strcmp(list->files[index].name, name) == 0) {
            rt_copy_string(list->files[index].expected_hex, sizeof(list->files[index].expected_hex), hash_hex);
            return 0;
        }
    }
    return 0;
}

static int parse_sha256sums(const char *manifest, WpDumpList *list) {
    size_t line_start = 0U;

    while (manifest[line_start] != '\0') {
        size_t line_end = (size_t)find_line_end(manifest, line_start);
        size_t line_length = line_end - line_start;
        char hash_hex[WP_HASH_HEX_SIZE];
        char name[WP_NAME_SIZE];
        size_t index;
        size_t name_start;
        size_t name_length;

        if (line_length > 0U && manifest[line_start + line_length - 1U] == '\r') {
            line_length -= 1U;
        }
        if (line_length >= 66U) {
            for (index = 0U; index < 64U; ++index) {
                hash_hex[index] = manifest[line_start + index];
            }
            hash_hex[64] = '\0';
            name_start = line_start + 64U;
            while (name_start < line_start + line_length && (manifest[name_start] == ' ' || manifest[name_start] == '\t' || manifest[name_start] == '*')) {
                name_start += 1U;
            }
            name_length = line_start + line_length - name_start;
            if (is_sha256_hex(hash_hex) && name_length > 0U && name_length < sizeof(name)) {
                for (index = 0U; index < name_length; ++index) {
                    name[index] = manifest[name_start + index];
                }
                name[name_length] = '\0';
                (void)set_expected_hash(list, name, hash_hex);
            }
        }
        if (manifest[line_end] == '\0') {
            break;
        }
        line_start = line_end + 1U;
    }

    for (line_start = 0U; line_start < list->count; ++line_start) {
        if (list->files[line_start].expected_hex[0] == '\0') {
            return -1;
        }
    }
    return 0;
}

static int join_output_path(const char *dir, const char *name, char *buffer, size_t buffer_size) {
    if (dir == 0 || dir[0] == '\0' || (dir[0] == '.' && dir[1] == '\0')) {
        rt_copy_string(buffer, buffer_size, name);
        return rt_strlen(buffer) == rt_strlen(name) ? 0 : -1;
    }
    return rt_join_path(dir, name, buffer, buffer_size);
}

static void write_duration_value(int fd, unsigned long long seconds) {
    unsigned long long hours = seconds / 3600ULL;
    unsigned long long minutes = (seconds / 60ULL) % 60ULL;
    unsigned long long remaining_seconds = seconds % 60ULL;

    if (hours > 0ULL) {
        rt_write_uint(fd, hours);
        rt_write_char(fd, 'h');
        rt_write_uint(fd, minutes);
        rt_write_char(fd, 'm');
        rt_write_uint(fd, remaining_seconds);
        rt_write_char(fd, 's');
    } else if (minutes > 0ULL) {
        rt_write_uint(fd, minutes);
        rt_write_char(fd, 'm');
        rt_write_uint(fd, remaining_seconds);
        rt_write_char(fd, 's');
    } else {
        rt_write_uint(fd, remaining_seconds);
        rt_write_char(fd, 's');
    }
}

static void write_file_count_suffix(size_t file_index, size_t file_count) {
    if (file_count > 1U) {
        rt_write_cstr(2, " (file ");
        rt_write_uint(2, (unsigned long long)file_index);
        rt_write_char(2, '/');
        rt_write_uint(2, (unsigned long long)file_count);
        rt_write_cstr(2, ", ");
        rt_write_uint(2, file_index < file_count ? (unsigned long long)(file_count - file_index) : 0ULL);
        rt_write_cstr(2, " remaining)");
    }
}

static void write_download_start_line(const char *name, size_t file_index, size_t file_count) {
    rt_write_cstr(2, "wp-download: downloading ");
    rt_write_cstr(2, name);
    write_file_count_suffix(file_index, file_count);
    rt_write_char(2, '\n');
}

static void write_progress_line(
    const char *name,
    unsigned long long written,
    int has_total,
    unsigned long long total,
    unsigned long long start_ns,
    size_t file_index,
    size_t file_count
) {
    char written_text[32];
    char total_text[32];
    char speed_text[32];
    unsigned long long now_ns = platform_get_monotonic_time_ns();
    unsigned long long elapsed_ms = now_ns > start_ns ? (now_ns - start_ns) / 1000000ULL : 0ULL;
    unsigned long long bytes_per_second = elapsed_ms > 0ULL ? (written * 1000ULL) / elapsed_ms : 0ULL;

    rt_write_cstr(2, "wp-download: ");
    rt_write_cstr(2, name);
    rt_write_cstr(2, ": ");
    tool_format_size(written, 1, written_text, sizeof(written_text));
    rt_write_cstr(2, written_text);
    if (has_total) {
        rt_write_cstr(2, " / ");
        tool_format_size(total, 1, total_text, sizeof(total_text));
        rt_write_cstr(2, total_text);
        if (total > 0ULL) {
            unsigned long long percent_tenths = (written * 1000ULL) / total;
            rt_write_cstr(2, " (");
            rt_write_uint(2, percent_tenths / 10ULL);
            rt_write_char(2, '.');
            rt_write_char(2, (char)('0' + (percent_tenths % 10ULL)));
            rt_write_cstr(2, "%)");
        }
    }
    if (bytes_per_second > 0ULL) {
        tool_format_size(bytes_per_second, 1, speed_text, sizeof(speed_text));
        rt_write_cstr(2, ", ");
        rt_write_cstr(2, speed_text);
        rt_write_cstr(2, "/s");
        if (has_total && total > written) {
            unsigned long long remaining_seconds = (total - written + bytes_per_second - 1ULL) / bytes_per_second;
            rt_write_cstr(2, ", eta ");
            write_duration_value(2, remaining_seconds);
        }
    }
    if (file_count > 1U) {
        rt_write_cstr(2, ", files remaining ");
        rt_write_uint(2, file_index < file_count ? (unsigned long long)(file_count - file_index) : 0ULL);
    }
    rt_write_char(2, '\n');
}

static int hex_digest_matches(const unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE], const char *expected_hex) {
    size_t index;

    for (index = 0U; index < CRYPTO_SHA256_DIGEST_SIZE; ++index) {
        int hi = hex_value(expected_hex[index * 2U]);
        int lo = hex_value(expected_hex[index * 2U + 1U]);
        if (hi < 0 || lo < 0 || digest[index] != (unsigned char)((hi << 4) | lo)) {
            return 0;
        }
    }
    return 1;
}

static int download_file(const char *url_text, const char *output_path, const char *name, const WpOptions *options, const char *expected_hex, size_t file_index, size_t file_count) {
    ToolHttpConnection connection;
    WpUrl url;
    WpHttpHeaders headers;
    CryptoSha256Context sha;
    unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE];
    char header_buffer[WP_HEADER_SIZE];
    size_t header_length = 0U;
    int header_complete = 0;
    char buffer[WP_BUFFER_SIZE];
    int output_fd = -1;
    long bytes_read;
    unsigned long long written = 0ULL;
    unsigned long long next_progress_ns = 0ULL;
    unsigned long long start_ns = 0ULL;
    int failed = 0;

    if (http_connect_and_request(url_text, &connection, &url) != 0) {
        return -1;
    }
    crypto_sha256_init(&sha);
    output_fd = platform_open_write(output_path, 0644U);
    if (output_fd < 0) {
        tool_http_connection_close(&connection);
        return -1;
    }

    if (!options->quiet) {
        write_download_start_line(name, file_index, file_count);
    }
    start_ns = platform_get_monotonic_time_ns();
    next_progress_ns = start_ns;

    for (;;) {
        if (!connection.use_tls && maybe_wait_for_socket(tool_http_connection_fd(&connection), options->timeout_ms) != 0) {
            bytes_read = -1;
            break;
        }
        bytes_read = tool_http_connection_read(&connection, buffer, sizeof(buffer));
        if (bytes_read < 0 && connection.use_tls && header_complete) {
            bytes_read = 0;
            break;
        }
        if (bytes_read <= 0) {
            break;
        }

        if (!header_complete) {
            size_t body_offset = 0U;
            if (header_length + (size_t)bytes_read >= sizeof(header_buffer)) {
                bytes_read = -1;
                break;
            }
            memcpy(header_buffer + header_length, buffer, (size_t)bytes_read);
            header_length += (size_t)bytes_read;
            header_buffer[header_length] = '\0';
            if (tool_find_http_header_end(header_buffer, header_length, &body_offset) != 0) {
                continue;
            }
            header_complete = 1;
            {
                char saved = header_buffer[body_offset];
                header_buffer[body_offset] = '\0';
                if (parse_http_headers(header_buffer, &headers) != 0 || headers.status_code < 200 || headers.status_code >= 300) {
                    bytes_read = -1;
                    break;
                }
                header_buffer[body_offset] = saved;
            }
            if (header_length > body_offset) {
                size_t body_size = header_length - body_offset;
                if (headers.has_content_length && (unsigned long long)body_size > headers.content_length - written) {
                    body_size = (size_t)(headers.content_length - written);
                }
                if (body_size > 0U && rt_write_all(output_fd, header_buffer + body_offset, body_size) != 0) {
                    bytes_read = -1;
                    break;
                }
                crypto_sha256_update(&sha, (const unsigned char *)header_buffer + body_offset, body_size);
                written += (unsigned long long)body_size;
            }
            if (headers.has_content_length && written >= headers.content_length) {
                break;
            }
        } else {
            size_t body_size = (size_t)bytes_read;
            if (headers.has_content_length && (unsigned long long)body_size > headers.content_length - written) {
                body_size = (size_t)(headers.content_length - written);
            }
            if (body_size > 0U && rt_write_all(output_fd, buffer, body_size) != 0) {
                bytes_read = -1;
                break;
            }
            crypto_sha256_update(&sha, (const unsigned char *)buffer, body_size);
            written += (unsigned long long)body_size;
            if (headers.has_content_length && written >= headers.content_length) {
                break;
            }
        }

        if (!options->quiet) {
            unsigned long long now_ns = platform_get_monotonic_time_ns();
            if (now_ns >= next_progress_ns) {
                write_progress_line(name, written, headers.has_content_length, headers.content_length, start_ns, file_index, file_count);
                next_progress_ns = now_ns + WP_PROGRESS_INTERVAL_NS;
            }
        }
    }

    tool_http_connection_close(&connection);
    if (platform_close(output_fd) != 0) {
        failed = 1;
    }
    if (!header_complete || bytes_read < 0 || (headers.has_content_length && written < headers.content_length)) {
        return -1;
    }
    if (failed) {
        return -1;
    }
    crypto_sha256_final(&sha, digest);
    if (!hex_digest_matches(digest, expected_hex)) {
        return -2;
    }
    if (!options->quiet) {
        write_progress_line(name, written, headers.has_content_length, headers.content_length, start_ns, file_index, file_count);
        write_info("verified ", name);
    }
    return 0;
}

static int run_download(const char *lang, const WpOptions *options) {
    char wiki_name[64];
    char root_url[WP_URL_SIZE];
    char listing_url[WP_URL_SIZE];
    char manifest_url[WP_URL_SIZE];
    char date[16];
    unsigned char *page = 0;
    size_t page_size = 0U;
    WpDumpList list;
    size_t index;

    if (!is_valid_lang_code(lang) || make_wiki_name(lang, wiki_name, sizeof(wiki_name)) != 0) {
        tool_write_error("wp-download", "invalid language edition: ", lang);
        return 1;
    }

    if (options->date != 0) {
        rt_copy_string(date, sizeof(date), options->date);
    } else {
        if (build_url(wiki_name, 0, 0, root_url, sizeof(root_url)) != 0 || read_http_to_memory(root_url, &page, &page_size, options->timeout_ms) != 0) {
            tool_write_error("wp-download", "cannot read dump index for ", wiki_name);
            return 1;
        }
        (void)page_size;
        if (find_latest_date((const char *)page, date, sizeof(date)) != 0) {
            rt_free(page);
            tool_write_error("wp-download", "cannot find latest snapshot for ", wiki_name);
            return 1;
        }
        rt_free(page);
        page = 0;
    }

    if (!options->quiet) {
        write_info("snapshot ", date);
    }

    if (build_url(wiki_name, date, 0, listing_url, sizeof(listing_url)) != 0 || read_http_to_memory(listing_url, &page, &page_size, options->timeout_ms) != 0) {
        tool_write_error("wp-download", "cannot read bzip2 listing for ", wiki_name);
        return 1;
    }
    (void)page_size;
    if (parse_bzip2_listing((const char *)page, wiki_name, &list) != 0) {
        rt_free(page);
        tool_write_error("wp-download", "cannot find bzip2 XML dumps for ", wiki_name);
        return 1;
    }
    rt_free(page);
    page = 0;

    if (build_url(wiki_name, date, "SHA256SUMS", manifest_url, sizeof(manifest_url)) != 0 || read_http_to_memory(manifest_url, &page, &page_size, options->timeout_ms) != 0) {
        tool_write_error("wp-download", "cannot read SHA256SUMS for ", wiki_name);
        return 1;
    }
    (void)page_size;
    if (parse_sha256sums((const char *)page, &list) != 0) {
        rt_free(page);
        tool_write_error("wp-download", "SHA256SUMS does not cover all selected dumps", 0);
        return 1;
    }
    rt_free(page);

    if (!options->quiet) {
        rt_write_cstr(2, "wp-download: found ");
        rt_write_uint(2, (unsigned long long)list.count);
        rt_write_cstr(2, list.count == 1U ? " dump file\n" : " dump files\n");
    }

    for (index = 0U; index < list.count; ++index) {
        char file_url[WP_URL_SIZE];
        char output_path[WP_PATH_SIZE];
        int result;

        if (build_url(wiki_name, date, list.files[index].name, file_url, sizeof(file_url)) != 0 ||
            join_output_path(options->out_dir, list.files[index].name, output_path, sizeof(output_path)) != 0) {
            tool_write_error("wp-download", "path too long for ", list.files[index].name);
            return 1;
        }
        result = download_file(file_url, output_path, list.files[index].name, options, list.files[index].expected_hex, index + 1U, list.count);
        if (result == -2) {
            tool_write_error("wp-download", "sha256 mismatch for ", list.files[index].name);
            return 1;
        }
        if (result != 0) {
            tool_write_error("wp-download", "download failed for ", list.files[index].name);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState state;
    WpOptions options;
    int parse_result;

    rt_memset(&options, 0, sizeof(options));
    options.out_dir = ".";
    options.timeout_ms = WP_DEFAULT_TIMEOUT_MS;

    tool_opt_init(&state, argc, argv, argv[0], "[-q] [-o DIR] [--date YYYY-MM-DD] LANG");
    while ((parse_result = tool_opt_next(&state)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(state.flag, "-q") == 0 || rt_strcmp(state.flag, "--quiet") == 0) {
            options.quiet = 1;
        } else if (rt_strcmp(state.flag, "-o") == 0 || rt_strcmp(state.flag, "--output-dir") == 0) {
            if (tool_opt_require_value(&state) != 0) {
                return 1;
            }
            options.out_dir = state.value;
        } else if (tool_starts_with(state.flag, "--output-dir=")) {
            options.out_dir = state.flag + 13;
        } else if (rt_strcmp(state.flag, "--date") == 0) {
            if (tool_opt_require_value(&state) != 0) {
                return 1;
            }
            options.date = state.value;
        } else if (tool_starts_with(state.flag, "--date=")) {
            options.date = state.flag + 7;
        } else if (rt_strcmp(state.flag, "-T") == 0 || rt_strcmp(state.flag, "--timeout") == 0) {
            if (tool_opt_require_value(&state) != 0 || tool_parse_duration_ms(state.value, &options.timeout_ms) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (tool_starts_with(state.flag, "--timeout=")) {
            if (tool_parse_duration_ms(state.flag + 10, &options.timeout_ms) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else {
            tool_write_error("wp-download", "unknown option: ", state.flag);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        print_usage(argv[0]);
        return 0;
    }
    if (parse_result == TOOL_OPT_ERROR) {
        return 1;
    }
    if (state.argi + 1 != argc) {
        print_usage(argv[0]);
        return 1;
    }
    return run_download(argv[state.argi], &options);
}
