#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define WTF_BUFFER_CHUNK 4096U
#define WTF_HEADER_LIMIT 16384U
#define WTF_URL_LIMIT 2048U
#define WTF_DEFAULT_WRAP_COLUMNS 80U

typedef struct {
    int https;
    unsigned int port;
    char host[256];
    char path[1536];
} WtfUrl;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} WtfBuffer;

typedef struct {
    const char *base_url;
    char default_base_url[128];
    char language[3];
    unsigned long long timeout_ms;
    int color_mode;
    int show_title;
    int show_description;
    int show_extract;
    int show_url;
} WtfOptions;

typedef struct {
    char title[256];
    char description[512];
    char extract[4096];
    char page_url[1024];
    int missing;
} WtfSummary;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-l LANG] [-T TIMEOUT] [--base-url URL] [--url] [--color[=WHEN]] TERM...");
}

static void print_help(const char *program_name) {
    print_usage(program_name);
    rt_write_line(1, "Look up a short Wikipedia-style summary for a term.");
    rt_write_line(1, "");
    rt_write_line(1, "Options:");
    rt_write_line(1, "  -l LANG           use a two-letter Wikipedia language, such as de or fr");
    rt_write_line(1, "  -T TIMEOUT        set network timeout, such as 2s or 500ms");
    rt_write_line(1, "  --base-url URL    use an alternate REST summary endpoint base");
    rt_write_line(1, "  --url             print the page URL when present");
    rt_write_line(1, "  --no-title        do not print the article title");
    rt_write_line(1, "  --no-description  do not print the one-line description");
    rt_write_line(1, "  --no-extract      do not print the introduction extract");
    rt_write_line(1, "  --only-title      print only the article title");
    rt_write_line(1, "  --only-description  print only the one-line description");
    rt_write_line(1, "  --only-extract    print only the introduction extract");
    rt_write_line(1, "  --color[=WHEN]    colorize/bolden output: auto, always, or never");
}

static char lower_ascii(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static int equals_ignore_case(const char *left, const char *right) {
    size_t index = 0U;
    while (left[index] != '\0' && right[index] != '\0') {
        if (lower_ascii(left[index]) != lower_ascii(right[index])) return 0;
        index += 1U;
    }
    return left[index] == '\0' && right[index] == '\0';
}

static void trim_ascii(char *text) {
    size_t start = 0U;
    size_t end = rt_strlen(text);
    size_t out = 0U;
    while (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n') start += 1U;
    while (end > start && (text[end - 1U] == ' ' || text[end - 1U] == '\t' || text[end - 1U] == '\r' || text[end - 1U] == '\n')) end -= 1U;
    while (start + out < end) {
        text[out] = text[start + out];
        out += 1U;
    }
    text[out] = '\0';
}

static void buffer_init(WtfBuffer *buffer) {
    buffer->data = 0;
    buffer->size = 0U;
    buffer->capacity = 0U;
}

static void buffer_free(WtfBuffer *buffer) {
    rt_free(buffer->data);
    buffer_init(buffer);
}

static int buffer_reserve(WtfBuffer *buffer, size_t extra) {
    size_t needed = buffer->size + extra;
    if (needed < buffer->size) return -1;
    if (needed + 1U > buffer->capacity) {
        size_t next = buffer->capacity == 0U ? WTF_BUFFER_CHUNK : buffer->capacity;
        char *resized;
        while (next < needed + 1U) {
            size_t doubled = next * 2U;
            if (doubled <= next) return -1;
            next = doubled;
        }
        resized = (char *)rt_realloc(buffer->data, next);
        if (resized == 0) return -1;
        buffer->data = resized;
        buffer->capacity = next;
    }
    return 0;
}

static int buffer_append(WtfBuffer *buffer, const char *data, size_t size) {
    if (buffer_reserve(buffer, size) != 0) return -1;
    if (size > 0U) memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 0;
}

static size_t append_char(char *buffer, size_t buffer_size, size_t length, char ch) {
    if (length + 1U < buffer_size) {
        buffer[length++] = ch;
        buffer[length] = '\0';
    }
    return length;
}

static size_t append_cstr(char *buffer, size_t buffer_size, size_t length, const char *text) {
    size_t index = 0U;
    while (text != 0 && text[index] != '\0') {
        length = append_char(buffer, buffer_size, length, text[index]);
        index += 1U;
    }
    return length;
}

static int parse_http_url(const char *text, WtfUrl *url) {
    size_t index = 0U;
    size_t host_start;
    size_t host_length;
    unsigned long long port;
    int saw_port = 0;

    rt_memset(url, 0, sizeof(*url));
    if (tool_starts_with(text, "http://")) {
        url->https = 0;
        url->port = 80U;
        text += 7;
    } else if (tool_starts_with(text, "https://")) {
        url->https = 1;
        url->port = 443U;
        text += 8;
    } else {
        return -1;
    }

    if (text[0] == '[') {
        host_start = 1U;
        while (text[index] != '\0' && text[index] != ']') index += 1U;
        if (text[index] != ']') return -1;
        host_length = index - host_start;
        index += 1U;
    } else {
        host_start = 0U;
        while (text[index] != '\0' && text[index] != '/' && text[index] != ':' && text[index] != '?' && text[index] != '#') index += 1U;
        host_length = index;
    }
    if (host_length == 0U || host_length + 1U > sizeof(url->host)) return -1;
    memcpy(url->host, text + host_start, host_length);
    url->host[host_length] = '\0';

    if (text[index] == ':') {
        port = 0ULL;
        index += 1U;
        while (text[index] >= '0' && text[index] <= '9') {
            saw_port = 1;
            port = port * 10ULL + (unsigned long long)(text[index] - '0');
            index += 1U;
        }
        if (!saw_port || port == 0ULL || port > 65535ULL) return -1;
        url->port = (unsigned int)port;
    }

    if (text[index] == '\0') rt_copy_string(url->path, sizeof(url->path), "/");
    else if (text[index] == '?' || text[index] == '#') rt_copy_string(url->path, sizeof(url->path), "/");
    else rt_copy_string(url->path, sizeof(url->path), text + index);
    return 0;
}

static int url_encode_term(const char *text, char *buffer, size_t buffer_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t in = 0U;
    size_t out = 0U;
    while (text[in] != '\0') {
        unsigned char ch = (unsigned char)text[in++];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            if (out + 1U >= buffer_size) return -1;
            buffer[out++] = (char)ch;
        } else if (ch == ' ') {
            if (out + 1U >= buffer_size) return -1;
            buffer[out++] = '_';
        } else {
            if (out + 3U >= buffer_size) return -1;
            buffer[out++] = '%';
            buffer[out++] = hex[(ch >> 4U) & 0x0fU];
            buffer[out++] = hex[ch & 0x0fU];
        }
    }
    buffer[out] = '\0';
    return 0;
}

static int compose_url(const char *base_url, const char *term, char *buffer, size_t buffer_size) {
    char encoded[1024];
    size_t length = 0U;
    if (url_encode_term(term, encoded, sizeof(encoded)) != 0) return -1;
    length = append_cstr(buffer, buffer_size, length, base_url);
    if (length == 0U || buffer[length - 1U] != '/') length = append_char(buffer, buffer_size, length, '/');
    length = append_cstr(buffer, buffer_size, length, encoded);
    return rt_strlen(buffer) == length ? 0 : -1;
}

static int parse_language_code(const char *text, char out[3]) {
    if (text == 0 || out == 0) return -1;
    if (!((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z'))) return -1;
    if (!((text[1] >= 'A' && text[1] <= 'Z') || (text[1] >= 'a' && text[1] <= 'z'))) return -1;
    if (text[2] != '\0') return -1;
    out[0] = lower_ascii(text[0]);
    out[1] = lower_ascii(text[1]);
    out[2] = '\0';
    return 0;
}

static int compose_default_base_url(const char language[3], char *buffer, size_t buffer_size) {
    size_t length = 0U;
    if (language == 0 || language[0] == '\0') return -1;
    length = append_cstr(buffer, buffer_size, length, "https://");
    length = append_cstr(buffer, buffer_size, length, language);
    length = append_cstr(buffer, buffer_size, length, ".wikipedia.org/api/rest_v1/page/summary");
    return rt_strlen(buffer) == length ? 0 : -1;
}

static int find_header_end(const char *buffer, size_t length, size_t *offset_out) {
    size_t index;
    for (index = 0U; index + 3U < length; ++index) {
        if (buffer[index] == '\r' && buffer[index + 1U] == '\n' && buffer[index + 2U] == '\r' && buffer[index + 3U] == '\n') {
            *offset_out = index + 4U;
            return 0;
        }
    }
    for (index = 0U; index + 1U < length; ++index) {
        if (buffer[index] == '\n' && buffer[index + 1U] == '\n') {
            *offset_out = index + 2U;
            return 0;
        }
    }
    return -1;
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

static size_t line_end(const char *text, size_t start) {
    size_t index = start;
    while (text[index] != '\0' && text[index] != '\n') index += 1U;
    return index;
}

static void parse_headers(const char *headers, int *status_out, char *location, size_t location_size) {
    size_t line_start = 0U;
    int line_index = 0;
    location[0] = '\0';
    *status_out = parse_status(headers);
    while (headers[line_start] != '\0') {
        size_t end = line_end(headers, line_start);
        size_t length = end > line_start ? end - line_start : 0U;
        if (length > 0U && headers[end - 1U] == '\r') length -= 1U;
        if (line_index > 0 && length + 1U < 1024U) {
            char line[1024];
            size_t colon = 0U;
            memcpy(line, headers + line_start, length);
            line[length] = '\0';
            while (line[colon] != '\0' && line[colon] != ':') colon += 1U;
            if (line[colon] == ':') {
                line[colon] = '\0';
                trim_ascii(line);
                trim_ascii(line + colon + 1U);
                if (equals_ignore_case(line, "Location")) rt_copy_string(location, location_size, line + colon + 1U);
            }
        }
        if (headers[end] == '\0') break;
        line_start = end + 1U;
        line_index += 1;
    }
}

static int wait_socket(int fd, unsigned long long timeout_ms) {
    int fds[1];
    size_t ready = 0U;
    if (timeout_ms == 0ULL) return 0;
    fds[0] = fd;
    return platform_poll_fds(fds, 1U, &ready, (int)timeout_ms) > 0 ? 0 : -1;
}

static void write_tls_error(const char *phase, const char *url) {
    rt_write_cstr(2, "wtf: ");
    rt_write_cstr(2, phase);
    rt_write_cstr(2, " failed for ");
    rt_write_cstr(2, url);
    rt_write_cstr(2, ": ");
    rt_write_cstr(2, platform_tls_last_error());
    rt_write_cstr(2, " (");
    rt_write_cstr(2, platform_tls_peer_verification_status());
    rt_write_cstr(2, ")\n");
}

static int write_all_transport(int fd, PlatformTlsClient *tls, int use_tls, const char *data, size_t size) {
    size_t done = 0U;
    while (done < size) {
        long written = use_tls ? platform_tls_write(tls, data + done, size - done) : platform_write(fd, data + done, size - done);
        if (written <= 0) return -1;
        done += (size_t)written;
    }
    return 0;
}

static long read_transport(int fd, PlatformTlsClient *tls, int use_tls, char *buffer, size_t size) {
    return use_tls ? platform_tls_read(tls, buffer, size) : platform_read(fd, buffer, size);
}

static int fetch_http(const WtfUrl *url, const char *request_url, unsigned long long timeout_ms, WtfBuffer *body, char *redirect, size_t redirect_size) {
    int fd = -1;
    PlatformTlsClient tls;
    int use_tls = url->https;
    char request[2048];
    char chunk[WTF_BUFFER_CHUNK];
    WtfBuffer response;
    size_t request_length = 0U;
    size_t body_offset = 0U;
    int status = 0;
    long bytes_read;
    int result = -1;

    redirect[0] = '\0';
    rt_memset(&tls, 0, sizeof(tls));
    tls.socket_fd = -1;
    buffer_init(&response);
    if (use_tls) {
        if (platform_tls_connect(&tls, url->host, url->port) != 0) {
            write_tls_error("tls connect", request_url);
            goto done;
        }
        fd = tls.socket_fd;
    } else if (platform_connect_tcp(url->host, url->port, &fd) != 0) goto done;

    request_length = append_cstr(request, sizeof(request), request_length, "GET ");
    request_length = append_cstr(request, sizeof(request), request_length, url->path[0] != '\0' ? url->path : "/");
    request_length = append_cstr(request, sizeof(request), request_length, use_tls ? " HTTP/1.1\r\nHost: " : " HTTP/1.0\r\nHost: ");
    request_length = append_cstr(request, sizeof(request), request_length, url->host);
    if ((!use_tls && url->port != 80U) || (use_tls && url->port != 443U)) {
        char port_text[16];
        rt_unsigned_to_string(url->port, port_text, sizeof(port_text));
        request_length = append_char(request, sizeof(request), request_length, ':');
        request_length = append_cstr(request, sizeof(request), request_length, port_text);
    }
    request_length = append_cstr(request, sizeof(request), request_length,
        "\r\nUser-Agent: Wikipedia Terminal Facts (https://github.com/MathiasSchindler/newos)\r\nAccept: application/json\r\nConnection: close\r\n\r\n");
    if (write_all_transport(fd, &tls, use_tls, request, request_length) != 0) {
        if (use_tls) write_tls_error("tls write", request_url);
        goto done;
    }

    for (;;) {
        if (!use_tls && wait_socket(fd, timeout_ms) != 0) goto done;
        bytes_read = read_transport(fd, &tls, use_tls, chunk, sizeof(chunk));
        if (bytes_read < 0) {
            if (use_tls) write_tls_error("tls read", request_url);
            goto done;
        }
        if (bytes_read == 0) break;
        if (buffer_append(&response, chunk, (size_t)bytes_read) != 0 || response.size > 4U * 1024U * 1024U) goto done;
    }
    if (find_header_end(response.data, response.size, &body_offset) != 0 || body_offset > WTF_HEADER_LIMIT) goto done;
    response.data[body_offset - 1U] = '\0';
    parse_headers(response.data, &status, redirect, redirect_size);
    response.data[body_offset - 1U] = '\n';
    if (status >= 300 && status < 400 && redirect[0] != '\0') {
        result = 1;
        goto done;
    }
    if (status < 200 || status >= 300) goto done;
    result = buffer_append(body, response.data + body_offset, response.size - body_offset);
done:
    if (use_tls) platform_tls_close(&tls);
    else if (fd >= 0) (void)platform_close(fd);
    buffer_free(&response);
    return result;
}

static int json_match_key(const char *json, size_t pos, const char *key) {
    size_t index = 0U;
    if (json[pos++] != '"') return 0;
    while (key[index] != '\0') {
        if (json[pos + index] != key[index]) return 0;
        index += 1U;
    }
    return json[pos + index] == '"';
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return 10 + (int)(ch - 'a');
    if (ch >= 'A' && ch <= 'F') return 10 + (int)(ch - 'A');
    return -1;
}

static int append_utf8(char *out, size_t out_size, size_t *out_pos, unsigned int codepoint) {
    char encoded[4];
    size_t encoded_size = 0U;
    if (rt_utf8_encode(codepoint, encoded, sizeof(encoded), &encoded_size) != 0 || *out_pos + encoded_size >= out_size) return -1;
    memcpy(out + *out_pos, encoded, encoded_size);
    *out_pos += encoded_size;
    out[*out_pos] = '\0';
    return 0;
}

static int json_copy_string(const char *json, size_t start, char *out, size_t out_size) {
    size_t pos = start;
    size_t out_pos = 0U;
    if (out_size == 0U || json[pos] != '"') return -1;
    pos += 1U;
    out[0] = '\0';
    while (json[pos] != '\0' && json[pos] != '"') {
        unsigned char ch = (unsigned char)json[pos++];
        if (ch == '\\') {
            char escape = json[pos++];
            if (escape == '"' || escape == '\\' || escape == '/') ch = (unsigned char)escape;
            else if (escape == 'b') ch = '\b';
            else if (escape == 'f') ch = '\f';
            else if (escape == 'n') ch = '\n';
            else if (escape == 'r') ch = '\r';
            else if (escape == 't') ch = '\t';
            else if (escape == 'u') {
                int a = hex_value(json[pos]);
                int b = hex_value(json[pos + 1U]);
                int c = hex_value(json[pos + 2U]);
                int d = hex_value(json[pos + 3U]);
                unsigned int codepoint;
                if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
                codepoint = ((unsigned int)a << 12U) | ((unsigned int)b << 8U) | ((unsigned int)c << 4U) | (unsigned int)d;
                pos += 4U;
                if (append_utf8(out, out_size, &out_pos, codepoint) != 0) return -1;
                continue;
            } else return -1;
        }
        if (out_pos + 1U >= out_size) return -1;
        out[out_pos++] = (char)ch;
        out[out_pos] = '\0';
    }
    return json[pos] == '"' ? 0 : -1;
}

static int json_find_string_value(const char *json, const char *key, char *out, size_t out_size) {
    size_t pos = 0U;
    while (json[pos] != '\0') {
        if (json[pos] == '"' && json_match_key(json, pos, key)) {
            pos += rt_strlen(key) + 2U;
            while (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n') pos += 1U;
            if (json[pos] != ':') continue;
            pos += 1U;
            while (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n') pos += 1U;
            if (json[pos] == '"') return json_copy_string(json, pos, out, out_size);
        }
        pos += 1U;
    }
    return -1;
}

static int json_has_missing_type(const char *json) {
    char type[64];
    return json_find_string_value(json, "type", type, sizeof(type)) == 0 && rt_strcmp(type, "https://mediawiki.org/wiki/HyperSwitch/errors/not_found") == 0;
}

static int parse_summary(const char *json, WtfSummary *summary) {
    rt_memset(summary, 0, sizeof(*summary));
    summary->missing = json_has_missing_type(json);
    (void)json_find_string_value(json, "title", summary->title, sizeof(summary->title));
    (void)json_find_string_value(json, "description", summary->description, sizeof(summary->description));
    (void)json_find_string_value(json, "extract", summary->extract, sizeof(summary->extract));
    (void)json_find_string_value(json, "page", summary->page_url, sizeof(summary->page_url));
    return summary->extract[0] != '\0' || summary->missing ? 0 : -1;
}

static void write_styled_line(int fd, int color_mode, int style, const char *text) {
    tool_style_begin(fd, color_mode, style);
    rt_write_line(fd, text);
    tool_style_end(fd, color_mode);
}

static unsigned int output_columns(void) {
    unsigned int rows = 0U;
    unsigned int columns = 0U;
    const char *columns_env = platform_getenv("COLUMNS");
    unsigned long long parsed_columns = 0ULL;

    if (platform_get_terminal_size(1, &rows, &columns) == 0 && columns > 0U) return columns;
    if (columns_env != 0 && rt_parse_uint(columns_env, &parsed_columns) == 0 && parsed_columns > 0ULL && parsed_columns <= 10000ULL) {
        return (unsigned int)parsed_columns;
    }
    return WTF_DEFAULT_WRAP_COLUMNS;
}

static size_t skip_leading_wrap_spaces(const char *text, size_t length, size_t start) {
    size_t index = start;
    RtTextSegment segment;

    while (index < length && rt_text_next_segment(text, length, index, &segment) == 0) {
        if (!rt_text_segment_is_space(text, length, &segment)) break;
        index = segment.end;
    }
    return index;
}

static size_t find_wrap_split(const char *text, size_t length, size_t start, unsigned int columns, size_t *next_start_out) {
    size_t index = start;
    size_t split = start;
    size_t last_space_start = 0U;
    size_t last_space_end = 0U;
    unsigned long long width = 0ULL;
    RtTextSegment segment;

    while (index < length && rt_text_next_segment(text, length, index, &segment) == 0) {
        unsigned long long next_width = rt_text_apply_segment_width(width, &segment);

        if (next_width > (unsigned long long)columns) {
            if (last_space_start > start) {
                *next_start_out = skip_leading_wrap_spaces(text, length, last_space_end);
                return last_space_start;
            }
            if (split > start) {
                *next_start_out = split;
                return split;
            }
            *next_start_out = segment.end;
            return segment.end;
        }
        if (rt_text_segment_is_space(text, length, &segment)) {
            last_space_start = segment.start;
            last_space_end = segment.end;
        }
        split = segment.end;
        width = next_width;
        index = segment.end;
    }

    *next_start_out = length;
    return length;
}

static int write_wrapped_span(int fd, const char *text, size_t length, unsigned int columns) {
    size_t start = skip_leading_wrap_spaces(text, length, 0U);

    while (start < length) {
        size_t next_start = length;
        size_t split = find_wrap_split(text, length, start, columns, &next_start);

        if (split > start && rt_write_all(fd, text + start, split - start) != 0) return -1;
        if (next_start < length && rt_write_char(fd, '\n') != 0) return -1;
        start = skip_leading_wrap_spaces(text, length, next_start);
    }
    return 0;
}

static int write_wrapped_text(int fd, const char *text, unsigned int columns) {
    size_t start = 0U;

    while (text[start] != '\0') {
        size_t end = line_end(text, start);

        if (write_wrapped_span(fd, text + start, end - start, columns) != 0) return -1;
        if (rt_write_char(fd, '\n') != 0) return -1;
        if (text[end] == '\0') break;
        start = end + 1U;
    }
    return 0;
}

static void print_summary(const WtfSummary *summary, const WtfOptions *options) {
    int printed_heading = 0;

    if (options->show_title && summary->title[0] != '\0') {
        write_styled_line(1, options->color_mode, TOOL_STYLE_BOLD, summary->title);
        printed_heading = 1;
    }
    if (options->show_description && summary->description[0] != '\0') {
        if (options->show_title && summary->title[0] != '\0') rt_write_cstr(1, "  ");
        rt_write_line(1, summary->description);
        printed_heading = 1;
    }
    if (printed_heading && options->show_extract && summary->extract[0] != '\0') {
        rt_write_char(1, '\n');
    }
    if (options->show_extract && summary->extract[0] != '\0') (void)write_wrapped_text(1, summary->extract, output_columns());
    if (options->show_url && summary->page_url[0] != '\0') {
        if ((options->show_title && summary->title[0] != '\0') ||
            (options->show_description && summary->description[0] != '\0') ||
            (options->show_extract && summary->extract[0] != '\0')) {
            rt_write_char(1, '\n');
        }
        rt_write_line(1, summary->page_url);
    }
}

static void select_only(WtfOptions *options, int title, int description, int extract) {
    options->show_title = title;
    options->show_description = description;
    options->show_extract = extract;
}

static int join_terms(int argc, char **argv, int argi, char *buffer, size_t buffer_size) {
    size_t length = 0U;
    while (argi < argc) {
        if (length > 0U) length = append_char(buffer, buffer_size, length, ' ');
        length = append_cstr(buffer, buffer_size, length, argv[argi]);
        argi += 1;
    }
    return rt_strlen(buffer) == length && length > 0U ? 0 : -1;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    WtfOptions options;
    int parsed;
    char term[1024];
    char request_url[WTF_URL_LIMIT];
    char redirect[WTF_URL_LIMIT];
    WtfUrl url;
    WtfBuffer body;
    WtfSummary summary;
    int result;

    rt_copy_string(options.language, sizeof(options.language), "en");
    if (compose_default_base_url(options.language, options.default_base_url, sizeof(options.default_base_url)) != 0) return 1;
    options.base_url = options.default_base_url;
    options.timeout_ms = 5000ULL;
    options.color_mode = TOOL_COLOR_AUTO;
    options.show_title = 1;
    options.show_description = 1;
    options.show_extract = 1;
    options.show_url = 0;
    tool_opt_init(&opt, argc, argv, argv[0], "[-l LANG] [-T TIMEOUT] [--base-url URL] [--url] [--color[=WHEN]] TERM...");
    while ((parsed = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-l") == 0 || rt_strcmp(opt.flag, "--language") == 0 || rt_strcmp(opt.flag, "--lang") == 0) {
            if (tool_opt_require_value(&opt) != 0 || parse_language_code(opt.value, options.language) != 0 ||
                compose_default_base_url(options.language, options.default_base_url, sizeof(options.default_base_url)) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            if (options.base_url == options.default_base_url) options.base_url = options.default_base_url;
        } else if (tool_starts_with(opt.flag, "--language=") || tool_starts_with(opt.flag, "--lang=")) {
            const char *value = tool_starts_with(opt.flag, "--language=") ? opt.flag + 11 : opt.flag + 7;
            if (parse_language_code(value, options.language) != 0 ||
                compose_default_base_url(options.language, options.default_base_url, sizeof(options.default_base_url)) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            if (options.base_url == options.default_base_url) options.base_url = options.default_base_url;
        } else if (rt_strcmp(opt.flag, "-T") == 0 || rt_strcmp(opt.flag, "--timeout") == 0) {
            if (tool_opt_require_value(&opt) != 0 || tool_parse_duration_ms(opt.value, &options.timeout_ms) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (tool_starts_with(opt.flag, "--timeout=")) {
            if (tool_parse_duration_ms(opt.flag + 10, &options.timeout_ms) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (rt_strcmp(opt.flag, "--base-url") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.base_url = opt.value;
        } else if (tool_starts_with(opt.flag, "--base-url=")) {
            options.base_url = opt.flag + 11;
        } else if (rt_strcmp(opt.flag, "--url") == 0) {
            options.show_url = 1;
        } else if (rt_strcmp(opt.flag, "--no-title") == 0) {
            options.show_title = 0;
        } else if (rt_strcmp(opt.flag, "--no-description") == 0) {
            options.show_description = 0;
        } else if (rt_strcmp(opt.flag, "--no-extract") == 0) {
            options.show_extract = 0;
        } else if (rt_strcmp(opt.flag, "--only-title") == 0) {
            select_only(&options, 1, 0, 0);
        } else if (rt_strcmp(opt.flag, "--only-description") == 0) {
            select_only(&options, 0, 1, 0);
        } else if (rt_strcmp(opt.flag, "--only-extract") == 0) {
            select_only(&options, 0, 0, 1);
        } else if (rt_strcmp(opt.flag, "--color") == 0 || rt_strcmp(opt.flag, "--colour") == 0) {
            options.color_mode = TOOL_COLOR_ALWAYS;
        } else if (tool_starts_with(opt.flag, "--color=") || tool_starts_with(opt.flag, "--colour=")) {
            const char *value = tool_starts_with(opt.flag, "--color=") ? opt.flag + 8 : opt.flag + 9;
            if (tool_parse_color_mode(value, &options.color_mode) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else {
            tool_write_error("wtf", "unknown option: ", opt.flag);
            print_usage(argv[0]);
            return 1;
        }
    }
    if (parsed == TOOL_OPT_HELP) {
        print_help(argv[0]);
        return 0;
    }
    if (parsed == TOOL_OPT_ERROR) return 1;
    if (opt.argi >= argc || join_terms(argc, argv, opt.argi, term, sizeof(term)) != 0 || compose_url(options.base_url, term, request_url, sizeof(request_url)) != 0) {
        print_usage(argv[0]);
        return 1;
    }
    if (parse_http_url(request_url, &url) != 0) {
        tool_write_error("wtf", "unsupported URL ", request_url);
        return 1;
    }
    if (!options.show_title && !options.show_description && !options.show_extract && !options.show_url) {
        tool_write_error("wtf", "no output fields selected", 0);
        return 1;
    }
    buffer_init(&body);
    result = fetch_http(&url, request_url, options.timeout_ms, &body, redirect, sizeof(redirect));
    if (result == 1 && tool_starts_with(redirect, "https://")) {
        tool_write_error("wtf", "redirects are not yet followed for ", redirect);
        buffer_free(&body);
        return 1;
    }
    if (result != 0) {
        tool_write_error("wtf", "request failed for ", request_url);
        buffer_free(&body);
        return 1;
    }
    if (parse_summary(body.data, &summary) != 0) {
        tool_write_error("wtf", "could not parse summary for ", term);
        buffer_free(&body);
        return 1;
    }
    if (summary.missing) {
        tool_write_error("wtf", "no Wikipedia summary for ", term);
        buffer_free(&body);
        return 1;
    }
    print_summary(&summary, &options);
    buffer_free(&body);
    return 0;
}