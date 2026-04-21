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
    char target[HTTPD_PATH_CAPACITY];

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
    while (line[index] == ' ' || line[index] == '\t') {
        index += 1U;
    }
    while (line[index] != '\0' && line[index] != ' ' && line[index] != '\t' && target_length + 1U < sizeof(target)) {
        target[target_length++] = line[index++];
    }
    target[target_length] = '\0';

    if (request->method[0] == '\0' || target[0] == '\0') {
        rt_copy_string(detail, detail_size, "missing method or path");
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

    return 0;
}
