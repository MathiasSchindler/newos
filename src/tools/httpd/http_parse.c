#include "httpd_impl.h"

#include "runtime.h"

static int httpd_hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return (int)(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return (int)(ch - 'a' + 10);
    }
    if (ch >= 'A' && ch <= 'F') {
        return (int)(ch - 'A' + 10);
    }
    return -1;
}

static int httpd_decode_path(const char *source, char *dest, size_t dest_size) {
    size_t src = 0U;
    size_t dst = 0U;

    if (source == NULL || dest == NULL || dest_size == 0U) {
        return -1;
    }

    while (source[src] != '\0' && source[src] != '?' && source[src] != '#') {
        unsigned char ch = (unsigned char)source[src];
        if (ch == '%') {
            int hi = httpd_hex_value(source[src + 1U]);
            int lo = httpd_hex_value(source[src + 2U]);
            if (hi < 0 || lo < 0) {
                return -1;
            }
            ch = (unsigned char)((hi << 4) | lo);
            src += 3U;
        } else {
            src += 1U;
        }
        if (ch < 32U || ch == '\\') {
            return -1;
        }
        if (dst + 1U >= dest_size) {
            return -1;
        }
        dest[dst++] = (char)ch;
    }

    if (dst == 0U) {
        if (dest_size < 2U) {
            return -1;
        }
        dest[0] = '/';
        dest[1] = '\0';
        return 0;
    }

    dest[dst] = '\0';
    return 0;
}

static int httpd_ascii_tolower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (int)(ch - 'A' + 'a');
    }
    return (int)ch;
}

static int httpd_header_name_equals(const char *header, const char *expected) {
    size_t index = 0U;

    while (header[index] != '\0' && expected[index] != '\0') {
        if (httpd_ascii_tolower(header[index]) != httpd_ascii_tolower(expected[index])) {
            return 0;
        }
        index += 1U;
    }
    return header[index] == '\0' && expected[index] == '\0';
}

static void httpd_trim_spaces(char *text) {
    size_t start = 0U;
    size_t end = rt_strlen(text);
    size_t out = 0U;

    while (text[start] == ' ' || text[start] == '\t') {
        start += 1U;
    }
    while (end > start && (text[end - 1U] == ' ' || text[end - 1U] == '\t')) {
        end -= 1U;
    }
    while (start + out < end) {
        text[out] = text[start + out];
        out += 1U;
    }
    text[out] = '\0';
}

static int httpd_path_has_parent_reference(const char *path) {
    size_t index = 0U;

    while (path != NULL && path[index] != '\0') {
        if ((index == 0U || path[index - 1U] == '/') &&
            path[index] == '.' && path[index + 1U] == '.' &&
            (path[index + 2U] == '\0' || path[index + 2U] == '/')) {
            return 1;
        }
        index += 1U;
    }
    return 0;
}

int httpd_parse_request(const char *buffer, HttpRequest *request, char *detail, size_t detail_size) {
    char line[1024];
    size_t line_length = 0U;
    size_t index = 0U;
    size_t method_length = 0U;
    size_t target_length = 0U;
    size_t version_length = 0U;
    char target[HTTPD_PATH_CAPACITY];
    char version[16];

    if (detail != NULL && detail_size > 0U) {
        detail[0] = '\0';
    }
    if (buffer == NULL || request == NULL) {
        return 400;
    }

    while (buffer[line_length] != '\0' && buffer[line_length] != '\n' && line_length + 1U < sizeof(line)) {
        line[line_length] = buffer[line_length];
        line_length += 1U;
    }
    if (line_length == 0U || line_length + 1U >= sizeof(line)) {
        rt_copy_string(detail, detail_size, "invalid request line");
        return 400;
    }
    if (line_length > 0U && line[line_length - 1U] == '\r') {
        line_length -= 1U;
    }
    line[line_length] = '\0';

    while (line[index] != '\0' && line[index] != ' ' && line[index] != '\t' && method_length + 1U < sizeof(request->method)) {
        request->method[method_length++] = line[index++];
    }
    request->method[method_length] = '\0';
    if (line[index] != '\0' && line[index] != ' ' && line[index] != '\t') {
        rt_copy_string(detail, detail_size, "method token too long");
        return 400;
    }
    while (line[index] == ' ' || line[index] == '\t') {
        index += 1U;
    }
    while (line[index] != '\0' && line[index] != ' ' && line[index] != '\t' && target_length + 1U < sizeof(target)) {
        target[target_length++] = line[index++];
    }
    target[target_length] = '\0';
    if (line[index] != '\0' && line[index] != ' ' && line[index] != '\t') {
        rt_copy_string(detail, detail_size, "request target too long");
        return 400;
    }
    while (line[index] == ' ' || line[index] == '\t') {
        index += 1U;
    }
    while (line[index] != '\0' && line[index] != ' ' && line[index] != '\t' && version_length + 1U < sizeof(version)) {
        version[version_length++] = line[index++];
    }
    version[version_length] = '\0';
    while (line[index] == ' ' || line[index] == '\t') {
        index += 1U;
    }

    if (request->method[0] == '\0' || target[0] == '\0' || version[0] == '\0') {
        rt_copy_string(detail, detail_size, "missing method, path, or version");
        return 400;
    }
    if ((rt_strcmp(version, "HTTP/1.1") != 0 && rt_strcmp(version, "HTTP/1.0") != 0) || line[index] != '\0') {
        rt_copy_string(detail, detail_size, "unsupported or malformed HTTP version");
        return 400;
    }

    if (rt_strcmp(request->method, "GET") == 0) {
        request->head_only = 0;
    } else if (rt_strcmp(request->method, "HEAD") == 0) {
        request->head_only = 1;
    } else {
        rt_copy_string(detail, detail_size, "method not allowed");
        return 405;
    }

    if (target[0] != '/' || httpd_decode_path(target, request->path, sizeof(request->path)) != 0) {
        rt_copy_string(detail, detail_size, "invalid request target");
        return 400;
    }
    if (httpd_path_has_parent_reference(request->path)) {
        rt_copy_string(detail, detail_size, "path traversal rejected");
        return 403;
    }

    {
        const char *cursor = buffer;

        while (*cursor != '\0' && *cursor != '\n') {
            cursor += 1U;
        }
        if (*cursor == '\n') {
            cursor += 1U;
        }

        while (*cursor != '\0') {
            char header_line[512];
            size_t header_length = 0U;
            size_t raw_length = 0U;
            char *colon;

            while (cursor[raw_length] != '\0' && cursor[raw_length] != '\n' && raw_length + 1U < sizeof(header_line)) {
                header_line[raw_length] = cursor[raw_length];
                raw_length += 1U;
            }
            if (cursor[raw_length] != '\0' && cursor[raw_length] != '\n') {
                rt_copy_string(detail, detail_size, "header line too long");
                return 400;
            }
            header_length = raw_length;
            if (header_length > 0U && header_line[header_length - 1U] == '\r') {
                header_length -= 1U;
            }
            header_line[header_length] = '\0';
            cursor += raw_length;
            if (*cursor == '\n') {
                cursor += 1U;
            }

            if (header_line[0] == '\0') {
                break;
            }

            colon = header_line;
            while (*colon != '\0' && *colon != ':') {
                colon += 1U;
            }
            if (*colon != ':') {
                rt_copy_string(detail, detail_size, "malformed header line");
                return 400;
            }
            *colon = '\0';
            httpd_trim_spaces(header_line);
            httpd_trim_spaces(colon + 1U);

            if (httpd_header_name_equals(header_line, "Content-Length") ||
                httpd_header_name_equals(header_line, "Transfer-Encoding") ||
                httpd_header_name_equals(header_line, "Expect")) {
                rt_copy_string(detail, detail_size, "request body framing is not supported");
                return 400;
            }
        }
    }

    return 0;
}
