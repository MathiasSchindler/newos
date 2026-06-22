#define main git_tool_main
#include "../../../src/tools/git.c"
#undef main

#include "io_loop.h"

#define GITD_REQUEST_HEADER_CAPACITY 16384U
#define GITD_MAX_BODY_SIZE (64U * 1024U * 1024U)
#define GITD_IO_CHUNK 8192U
#define GITD_SIDEBAND_CHUNK 60000U

typedef struct {
    char bind_host[PLATFORM_NETWORK_TEXT_CAPACITY];
    char repo_root[GIT_PATH_CAPACITY];
    unsigned int port;
    int quiet;
    int once;
} GitdOptions;

typedef struct {
    char method[8];
    char target[GIT_PATH_CAPACITY];
    char path[GIT_PATH_CAPACITY];
    char query[512];
    char content_type[128];
    char git_protocol[128];
    size_t content_length;
    int has_content_length;
} GitdRequest;

typedef struct {
    char name[GIT_REF_CAPACITY];
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
} GitdRef;

typedef struct {
    GitdRef *refs;
    size_t count;
    size_t capacity;
} GitdRefList;

typedef struct {
    unsigned char old_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char new_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char ref_name[GIT_REF_CAPACITY];
} GitdReceiveCommand;

typedef struct {
    GitdReceiveCommand *commands;
    size_t count;
    size_t capacity;
    const unsigned char *pack_data;
    size_t pack_size;
    int report_status;
    int sideband;
} GitdReceiveRequest;

typedef struct {
    RtIoLoop loop;
    GitdOptions options;
    int listener_fd;
    size_t handled_connections;
} GitdServer;

typedef struct {
    GitdServer *server;
    int fd;
    GitBuffer raw;
    GitBuffer body;
    GitdRequest request;
    size_t header_end;
    int saw_header;
} GitdConnection;

static int gitd_path_has_parent_reference(const char *path);

static void gitd_usage(const char *program_name) {
    tool_write_usage(program_name, "[-b HOST] [-p PORT] [-r REPO_ROOT] [--once] [-q]");
}

static const char *gitd_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 500: return "Internal Server Error";
        default: return "Error";
    }
}

static int gitd_write_all(int fd, const void *data, size_t size) {
    return rt_write_all(fd, data, size);
}

static int gitd_oid_is_zero(const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    size_t i;

    for (i = 0U; i < CRYPTO_SHA1_DIGEST_SIZE; ++i) {
        if (oid[i] != 0U) return 0;
    }
    return 1;
}

static int gitd_ref_is_branch(const char *ref_name) {
    return rt_strncmp(ref_name, "refs/heads/", 11U) == 0 && ref_name[11] != '\0' && !gitd_path_has_parent_reference(ref_name);
}

static int gitd_ref_is_safe(const char *ref_name) {
    size_t i;
    size_t component_start = 0U;
    size_t component_length = 0U;
    int saw_slash = 0;

    if (rt_strncmp(ref_name, "refs/", 5U) != 0 || ref_name[5] == '\0' || tool_path_is_unsafe_relative(ref_name) || gitd_path_has_parent_reference(ref_name)) return 0;
    for (i = 0U; ref_name[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)ref_name[i];

        if (ch <= 32U || ch == 127U || ch == '\\' || ch == '~' || ch == '^' || ch == ':' || ch == '?' || ch == '*' || ch == '[') return 0;
        if (ch == '/') {
            if (component_length == 0U) return 0;
            if (component_length == 1U && ref_name[component_start] == '.') return 0;
            if (component_length >= 5U && memcmp(ref_name + i - 5U, ".lock", 5U) == 0) return 0;
            component_start = i + 1U;
            component_length = 0U;
            saw_slash = 1;
            continue;
        }
        if (ch == '.' && i > 0U && ref_name[i - 1U] == '.') return 0;
        if (ch == '@' && ref_name[i + 1U] == '{') return 0;
        component_length += 1U;
    }
    if (!saw_slash || component_length == 0U) return 0;
    if (component_length == 1U && ref_name[component_start] == '.') return 0;
    if (component_length >= 5U && memcmp(ref_name + i - 5U, ".lock", 5U) == 0) return 0;
    if (i > 0U && (ref_name[i - 1U] == '.' || ref_name[i - 1U] == '/')) return 0;
    return 1;
}

static void gitd_receive_request_destroy(GitdReceiveRequest *request) {
    if (request == 0) return;
    rt_free(request->commands);
    rt_memset(request, 0, sizeof(*request));
}

static int gitd_receive_request_push(GitdReceiveRequest *request, const GitdReceiveCommand *command) {
    GitdReceiveCommand *new_commands;
    size_t new_capacity;

    if (request->count == request->capacity) {
        new_capacity = request->capacity == 0U ? 4U : request->capacity * 2U;
        new_commands = (GitdReceiveCommand *)rt_realloc_array(request->commands, new_capacity, sizeof(request->commands[0]));
        if (new_commands == 0) return -1;
        request->commands = new_commands;
        request->capacity = new_capacity;
    }
    request->commands[request->count++] = *command;
    return 0;
}

static int gitd_header_append_common(GitBuffer *header, int status, const char *content_type, size_t content_length) {
    char status_digits[16];

    rt_unsigned_to_string((unsigned long long)status, status_digits, sizeof(status_digits));
    if (tool_byte_buffer_append_cstr(header, "HTTP/1.1 ") != 0 ||
        tool_byte_buffer_append_cstr(header, status_digits) != 0 ||
        tool_byte_buffer_append_char(header, ' ') != 0 ||
        tool_byte_buffer_append_cstr(header, gitd_status_text(status)) != 0 ||
        tool_byte_buffer_append_cstr(header, "\r\nContent-Length: ") != 0) {
        return -1;
    }
    {
        char digits[32];
        rt_unsigned_to_string((unsigned long long)content_length, digits, sizeof(digits));
        if (tool_byte_buffer_append_cstr(header, digits) != 0) {
            return -1;
        }
    }
    if (tool_byte_buffer_append_cstr(header, "\r\nContent-Type: ") != 0 ||
        tool_byte_buffer_append_cstr(header, content_type != 0 ? content_type : "text/plain; charset=utf-8") != 0 ||
        tool_byte_buffer_append_cstr(header, "\r\nAccess-Control-Allow-Origin: *") != 0 ||
        tool_byte_buffer_append_cstr(header, "\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS") != 0 ||
        tool_byte_buffer_append_cstr(header, "\r\nAccess-Control-Allow-Headers: content-type, authorization") != 0 ||
        tool_byte_buffer_append_cstr(header, "\r\nAccess-Control-Max-Age: 600") != 0 ||
        tool_byte_buffer_append_cstr(header, "\r\nX-Content-Type-Options: nosniff") != 0 ||
        tool_byte_buffer_append_cstr(header, "\r\nConnection: close\r\n\r\n") != 0) {
        return -1;
    }
    return 0;
}

static int gitd_send_body(int fd, int status, const char *content_type, const unsigned char *body, size_t body_size) {
    GitBuffer header;
    int result = -1;

    rt_memset(&header, 0, sizeof(header));
    if (gitd_header_append_common(&header, status, content_type, body_size) != 0) {
        goto done;
    }
    if (gitd_write_all(fd, header.data, header.size) != 0) {
        goto done;
    }
    if (body_size > 0U && gitd_write_all(fd, body, body_size) != 0) {
        goto done;
    }
    result = 0;
done:
    git_buffer_destroy(&header);
    return result;
}

static int gitd_send_text(int fd, int status, const char *message) {
    return gitd_send_body(fd, status, "text/plain; charset=utf-8", (const unsigned char *)message, message != 0 ? rt_strlen(message) : 0U);
}

static int gitd_send_options(int fd) {
    return gitd_send_body(fd, 204, "text/plain; charset=utf-8", 0, 0U);
}

static int gitd_hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return (int)(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'F') return (int)(ch - 'A' + 10);
    return -1;
}

static int gitd_decode_component(const char *source, size_t length, char *out, size_t out_size) {
    size_t src = 0U;
    size_t dst = 0U;

    if (out_size == 0U) {
        return -1;
    }
    while (src < length) {
        unsigned char ch = (unsigned char)source[src++];
        if (ch == '%') {
            int hi;
            int lo;
            if (src + 1U >= length) {
                return -1;
            }
            hi = gitd_hex_value(source[src]);
            lo = gitd_hex_value(source[src + 1U]);
            if (hi < 0 || lo < 0) {
                return -1;
            }
            ch = (unsigned char)((hi << 4) | lo);
            src += 2U;
        }
        if (ch < 32U || ch == '\\' || ch == '\0') {
            return -1;
        }
        if (dst + 1U >= out_size) {
            return -1;
        }
        out[dst++] = (char)ch;
    }
    out[dst] = '\0';
    return 0;
}

static int gitd_path_has_parent_reference(const char *path) {
    size_t i = 0U;

    while (path[i] != '\0') {
        if ((i == 0U || path[i - 1U] == '/') && path[i] == '.' && path[i + 1U] == '.' && (path[i + 2U] == '\0' || path[i + 2U] == '/')) {
            return 1;
        }
        i += 1U;
    }
    return 0;
}

static int gitd_find_header_end(const unsigned char *data, size_t size, size_t *end_out) {
    size_t i;

    for (i = 0U; i + 3U < size; ++i) {
        if (data[i] == '\r' && data[i + 1U] == '\n' && data[i + 2U] == '\r' && data[i + 3U] == '\n') {
            *end_out = i + 4U;
            return 0;
        }
    }
    for (i = 0U; i + 1U < size; ++i) {
        if (data[i] == '\n' && data[i + 1U] == '\n') {
            *end_out = i + 2U;
            return 0;
        }
    }
    return -1;
}

static int gitd_parse_uint_header(const char *text, size_t length, size_t *value_out) {
    size_t i;
    size_t value = 0U;
    int saw_digit = 0;

    for (i = 0U; i < length; ++i) {
        if (text[i] >= '0' && text[i] <= '9') {
            saw_digit = 1;
            value = value * 10U + (size_t)(text[i] - '0');
        } else if (text[i] != ' ' && text[i] != '\t') {
            return -1;
        }
    }
    if (!saw_digit) {
        return -1;
    }
    *value_out = value;
    return 0;
}

static int gitd_parse_request_line(const unsigned char *headers, size_t header_size, GitdRequest *request) {
    size_t pos = 0U;
    size_t start;
    size_t method_length;
    size_t target_length;
    size_t version_length;
    char version[16];
    char raw_target[GIT_PATH_CAPACITY];
    size_t query_start;

    start = pos;
    while (pos < header_size && headers[pos] != ' ' && headers[pos] != '\t' && headers[pos] != '\r' && headers[pos] != '\n') pos += 1U;
    method_length = pos - start;
    if (method_length == 0U || method_length >= sizeof(request->method)) return -1;
    memcpy(request->method, headers + start, method_length);
    request->method[method_length] = '\0';
    while (pos < header_size && (headers[pos] == ' ' || headers[pos] == '\t')) pos += 1U;
    start = pos;
    while (pos < header_size && headers[pos] != ' ' && headers[pos] != '\t' && headers[pos] != '\r' && headers[pos] != '\n') pos += 1U;
    target_length = pos - start;
    if (target_length == 0U || target_length >= sizeof(raw_target)) return -1;
    memcpy(raw_target, headers + start, target_length);
    raw_target[target_length] = '\0';
    while (pos < header_size && (headers[pos] == ' ' || headers[pos] == '\t')) pos += 1U;
    start = pos;
    while (pos < header_size && headers[pos] != ' ' && headers[pos] != '\t' && headers[pos] != '\r' && headers[pos] != '\n') pos += 1U;
    version_length = pos - start;
    if (version_length == 0U || version_length >= sizeof(version)) return -1;
    memcpy(version, headers + start, version_length);
    version[version_length] = '\0';
    if (rt_strcmp(version, "HTTP/1.0") != 0 && rt_strcmp(version, "HTTP/1.1") != 0) return -1;
    if (raw_target[0] != '/') return -1;
    query_start = 0U;
    while (raw_target[query_start] != '\0' && raw_target[query_start] != '?') query_start += 1U;
    if (gitd_decode_component(raw_target, query_start, request->path, sizeof(request->path)) != 0) return -1;
    if (raw_target[query_start] == '?') {
        if (gitd_decode_component(raw_target + query_start + 1U, rt_strlen(raw_target + query_start + 1U), request->query, sizeof(request->query)) != 0) return -1;
    } else {
        request->query[0] = '\0';
    }
    if (gitd_path_has_parent_reference(request->path)) return -1;
    rt_copy_string(request->target, sizeof(request->target), raw_target);
    return 0;
}

static void gitd_trim_header_value(const unsigned char *data, size_t size, size_t *start_io, size_t *end_io) {
    while (*start_io < *end_io && (data[*start_io] == ' ' || data[*start_io] == '\t')) *start_io += 1U;
    while (*end_io > *start_io && (data[*end_io - 1U] == ' ' || data[*end_io - 1U] == '\t' || data[*end_io - 1U] == '\r')) *end_io -= 1U;
    (void)size;
}

static int gitd_parse_headers(const unsigned char *headers, size_t header_size, GitdRequest *request) {
    size_t pos = 0U;

    while (pos < header_size && headers[pos] != '\n') pos += 1U;
    if (pos < header_size) pos += 1U;
    while (pos < header_size) {
        size_t line_start = pos;
        size_t line_end;
        size_t colon;
        size_t value_start;
        size_t value_end;
        size_t name_end;

        while (pos < header_size && headers[pos] != '\n') pos += 1U;
        line_end = pos;
        if (pos < header_size) pos += 1U;
        if (line_end > line_start && headers[line_end - 1U] == '\r') line_end -= 1U;
        if (line_end == line_start) break;
        colon = line_start;
        while (colon < line_end && headers[colon] != ':') colon += 1U;
        if (colon >= line_end) return -1;
        name_end = colon;
        while (name_end > line_start && (headers[name_end - 1U] == ' ' || headers[name_end - 1U] == '\t')) name_end -= 1U;
        value_start = colon + 1U;
        value_end = line_end;
        gitd_trim_header_value(headers, header_size, &value_start, &value_end);
        if (git_header_name_equals(headers + line_start, name_end - line_start, "Content-Length")) {
            if (gitd_parse_uint_header((const char *)headers + value_start, value_end - value_start, &request->content_length) != 0) return -1;
            request->has_content_length = 1;
        } else if (git_header_name_equals(headers + line_start, name_end - line_start, "Content-Type")) {
            size_t copy = value_end - value_start;
            if (copy >= sizeof(request->content_type)) copy = sizeof(request->content_type) - 1U;
            memcpy(request->content_type, headers + value_start, copy);
            request->content_type[copy] = '\0';
        } else if (git_header_name_equals(headers + line_start, name_end - line_start, "Git-Protocol")) {
            size_t copy = value_end - value_start;
            if (copy >= sizeof(request->git_protocol)) copy = sizeof(request->git_protocol) - 1U;
            memcpy(request->git_protocol, headers + value_start, copy);
            request->git_protocol[copy] = '\0';
        }
    }
    return 0;
}

static int gitd_read_request_step(GitdConnection *connection, int *complete_out) {
    unsigned char buffer[GITD_IO_CHUNK];
    long bytes;

    *complete_out = 0;
    bytes = platform_read(connection->fd, buffer, sizeof(buffer));
    if (bytes <= 0) return -1;
    if (!connection->saw_header) {
        size_t already;
        if (git_buffer_append(&connection->raw, buffer, (size_t)bytes) != 0) return -1;
        if (connection->raw.size > GITD_REQUEST_HEADER_CAPACITY + GITD_MAX_BODY_SIZE) return -1;
        if (gitd_find_header_end(connection->raw.data, connection->raw.size, &connection->header_end) != 0) {
            if (connection->raw.size > GITD_REQUEST_HEADER_CAPACITY) return -1;
            return 0;
        }
        connection->saw_header = 1;
        if (gitd_parse_request_line(connection->raw.data, connection->header_end, &connection->request) != 0 || gitd_parse_headers(connection->raw.data, connection->header_end, &connection->request) != 0) return -1;
        if (rt_strcmp(connection->request.method, "POST") == 0 && (!connection->request.has_content_length || connection->request.content_length > GITD_MAX_BODY_SIZE)) return -1;
        already = connection->raw.size - connection->header_end;
        if (rt_strcmp(connection->request.method, "POST") == 0) {
            if (already > connection->request.content_length) already = connection->request.content_length;
            if (git_buffer_append(&connection->body, connection->raw.data + connection->header_end, already) != 0) return -1;
        }
        git_buffer_destroy(&connection->raw);
        if (rt_strcmp(connection->request.method, "POST") != 0 || connection->body.size >= connection->request.content_length) {
            *complete_out = 1;
        }
        return 0;
    }
    if (rt_strcmp(connection->request.method, "POST") != 0) {
        *complete_out = 1;
        return 0;
    }
    if (connection->body.size + (size_t)bytes > connection->request.content_length) {
        bytes = (long)(connection->request.content_length - connection->body.size);
    }
    if (bytes > 0 && git_buffer_append(&connection->body, buffer, (size_t)bytes) != 0) return -1;
    if (connection->body.size >= connection->request.content_length) {
        *complete_out = 1;
    }
    return 0;
}

static void gitd_ref_list_destroy(GitdRefList *list) {
    rt_free(list->refs);
    rt_memset(list, 0, sizeof(*list));
}

static int gitd_ref_list_push(GitdRefList *list, const char *name, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitdRef *new_refs;
    size_t new_capacity;

    if (list->count == list->capacity) {
        new_capacity = list->capacity == 0U ? 16U : list->capacity * 2U;
        new_refs = (GitdRef *)rt_realloc_array(list->refs, new_capacity, sizeof(list->refs[0]));
        if (new_refs == 0) return -1;
        list->refs = new_refs;
        list->capacity = new_capacity;
    }
    if (git_copy(list->refs[list->count].name, sizeof(list->refs[list->count].name), name) != 0) return -1;
    memcpy(list->refs[list->count].oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
    list->count += 1U;
    return 0;
}

static int gitd_collect_loose_refs_dir(GitRepo *repo, const char *prefix, GitdRefList *list) {
    char dir[GIT_PATH_CAPACITY];
    PlatformDirEntry entries[128];
    size_t count = 0U;
    int is_directory = 0;
    size_t i;

    if (git_join(dir, sizeof(dir), repo->git_dir, prefix) != 0) return -1;
    if (platform_collect_entries(dir, 0, entries, sizeof(entries) / sizeof(entries[0]), &count, &is_directory) != 0 || !is_directory) return 0;
    for (i = 0U; i < count; ++i) {
        char ref_name[GIT_REF_CAPACITY];
        char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];

        if (entries[i].name[0] == '.') continue;
        if (git_join(ref_name, sizeof(ref_name), prefix, entries[i].name) != 0) return -1;
        if (entries[i].is_dir) {
            if (gitd_collect_loose_refs_dir(repo, ref_name, list) != 0) return -1;
            continue;
        }
        if (gitd_ref_is_safe(ref_name) && git_resolve_ref(repo, ref_name, oid_hex, sizeof(oid_hex)) == 0 && git_parse_oid_hex_n(oid_hex, GIT_OBJECT_HEX_SIZE, oid) == 0) {
            if (gitd_ref_list_push(list, ref_name, oid) != 0) return -1;
        }
    }
    return 0;
}

static int gitd_ref_exists(const GitdRefList *list, const char *name) {
    size_t i;

    for (i = 0U; i < list->count; ++i) {
        if (rt_strcmp(list->refs[i].name, name) == 0) return 1;
    }
    return 0;
}

static int gitd_collect_packed_refs(GitRepo *repo, GitdRefList *list) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;

    if (git_join(path, sizeof(path), repo->git_dir, "packed-refs") != 0 || git_read_file(path, &data, &size) != 0) return 0;
    while (pos < size) {
        size_t start = pos;
        size_t end;
        while (pos < size && data[pos] != '\n') pos += 1U;
        end = pos;
        if (pos < size) pos += 1U;
        while (end > start && data[end - 1U] == '\r') end -= 1U;
        if (end > start + GIT_OBJECT_HEX_SIZE + 1U && data[start] != '#' && data[start] != '^' && data[start + GIT_OBJECT_HEX_SIZE] == ' ') {
            char ref_name[GIT_REF_CAPACITY];
            unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
            size_t ref_length = end - start - GIT_OBJECT_HEX_SIZE - 1U;
            if (ref_length < sizeof(ref_name) && git_parse_oid_hex_n((const char *)data + start, GIT_OBJECT_HEX_SIZE, oid) == 0) {
                memcpy(ref_name, data + start + GIT_OBJECT_HEX_SIZE + 1U, ref_length);
                ref_name[ref_length] = '\0';
                if (!gitd_ref_exists(list, ref_name) && gitd_ref_is_safe(ref_name)) {
                    if (gitd_ref_list_push(list, ref_name, oid) != 0) {
                        rt_free(data);
                        return -1;
                    }
                }
            }
        }
    }
    rt_free(data);
    return 0;
}

static int gitd_collect_refs(GitRepo *repo, GitdRefList *list) {
    rt_memset(list, 0, sizeof(*list));
    if (gitd_collect_loose_refs_dir(repo, "refs", list) != 0) return -1;
    return gitd_collect_packed_refs(repo, list);
}

static int gitd_append_service_advertisement(GitBuffer *out, const char *service) {
    GitBuffer line;
    int result;

    rt_memset(&line, 0, sizeof(line));
    if (tool_byte_buffer_append_cstr(&line, "# service=") != 0 || tool_byte_buffer_append_cstr(&line, service) != 0 || tool_byte_buffer_append_char(&line, '\n') != 0) {
        git_buffer_destroy(&line);
        return -1;
    }
    result = git_append_pkt_data(out, line.data, line.size);
    git_buffer_destroy(&line);
    if (result != 0) return -1;
    return tool_byte_buffer_append_cstr(out, "0000");
}

static int gitd_append_ref_advertisement(GitBuffer *out, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const char *name, const char *caps) {
    GitBuffer line;
    char hex[GIT_OBJECT_HEX_SIZE + 1U];
    int result;

    rt_memset(&line, 0, sizeof(line));
    git_format_oid_hex(oid, hex);
    if (tool_byte_buffer_append_cstr(&line, hex) != 0 || tool_byte_buffer_append_char(&line, ' ') != 0 || tool_byte_buffer_append_cstr(&line, name) != 0) {
        git_buffer_destroy(&line);
        return -1;
    }
    if (caps != 0 && caps[0] != '\0') {
        if (tool_byte_buffer_append_char(&line, '\0') != 0 || tool_byte_buffer_append_cstr(&line, caps) != 0) {
            git_buffer_destroy(&line);
            return -1;
        }
    }
    if (tool_byte_buffer_append_char(&line, '\n') != 0) {
        git_buffer_destroy(&line);
        return -1;
    }
    result = git_append_pkt_data(out, line.data, line.size);
    git_buffer_destroy(&line);
    return result;
}

static int gitd_append_zero_ref_advertisement(GitBuffer *out, const char *caps) {
    unsigned char zero_oid[CRYPTO_SHA1_DIGEST_SIZE];

    rt_memset(zero_oid, 0, sizeof(zero_oid));
    return gitd_append_ref_advertisement(out, zero_oid, "capabilities^{}", caps);
}

static int gitd_repo_from_path(const GitdOptions *options, const char *repo_url_path, GitRepo *repo) {
    char relative[GIT_PATH_CAPACITY];
    char head_path[GIT_PATH_CAPACITY];
    PlatformDirEntry head_entry;
    const char *path = repo_url_path;

    if (path[0] == '/') path += 1;
    if (path[0] == '\0' || gitd_path_has_parent_reference(path)) return -1;
    if (git_copy(relative, sizeof(relative), path) != 0) return -1;
    while (relative[0] != '\0' && relative[rt_strlen(relative) - 1U] == '/') relative[rt_strlen(relative) - 1U] = '\0';
    rt_memset(repo, 0, sizeof(*repo));
    if (git_join(repo->git_dir, sizeof(repo->git_dir), options->repo_root, relative) != 0 || git_copy(repo->work_tree, sizeof(repo->work_tree), repo->git_dir) != 0) return -1;
    if (git_join(head_path, sizeof(head_path), repo->git_dir, "HEAD") != 0) return -1;
    if (platform_get_path_info(head_path, &head_entry) != 0 || head_entry.is_dir) return -1;
    (void)git_load_head(repo);
    return 0;
}

static int gitd_strip_suffix(const char *path, const char *suffix, char *repo_path, size_t repo_path_size) {
    size_t path_length = rt_strlen(path);
    size_t suffix_length = rt_strlen(suffix);

    if (path_length <= suffix_length || rt_strcmp(path + path_length - suffix_length, suffix) != 0) return -1;
    if (path_length - suffix_length >= repo_path_size) return -1;
    memcpy(repo_path, path, path_length - suffix_length);
    repo_path[path_length - suffix_length] = '\0';
    return 0;
}

static int gitd_handle_info_refs(int fd, const GitdOptions *options, const GitdRequest *request) {
    char repo_path[GIT_PATH_CAPACITY];
    GitRepo repo;
    GitdRefList refs;
    GitBuffer body;
    char caps[512];
    const char *service;
    const char *content_type;
    unsigned char head_oid[CRYPTO_SHA1_DIGEST_SIZE];
    size_t i;
    int have_head = 0;
    int receive_pack = 0;
    int result = -1;

    if (rt_strcmp(request->method, "GET") != 0) return gitd_send_text(fd, 405, "method not allowed\n");
    if (rt_strcmp(request->query, "service=git-upload-pack") == 0) {
        service = "git-upload-pack";
        content_type = "application/x-git-upload-pack-advertisement";
    } else if (rt_strcmp(request->query, "service=git-receive-pack") == 0) {
        service = "git-receive-pack";
        content_type = "application/x-git-receive-pack-advertisement";
        receive_pack = 1;
    } else {
        return gitd_send_text(fd, 400, "expected git service query\n");
    }
    if (gitd_strip_suffix(request->path, "/info/refs", repo_path, sizeof(repo_path)) != 0 || gitd_repo_from_path(options, repo_path, &repo) != 0) return gitd_send_text(fd, 404, "repository not found\n");
    rt_memset(&refs, 0, sizeof(refs));
    rt_memset(&body, 0, sizeof(body));
    if (gitd_collect_refs(&repo, &refs) != 0 || gitd_append_service_advertisement(&body, service) != 0) goto done;
    if (repo.head_oid[0] != '\0' && git_parse_oid_hex_n(repo.head_oid, GIT_OBJECT_HEX_SIZE, head_oid) == 0) {
        have_head = 1;
    } else if (refs.count > 0U) {
        memcpy(head_oid, refs.refs[0].oid, sizeof(head_oid));
        have_head = 1;
    }
    rt_copy_string(caps, sizeof(caps), receive_pack ? "report-status side-band-64k delete-refs agent=newos-gitd" : "multi_ack multi_ack_detailed side-band-64k agent=newos-gitd");
    if (!receive_pack && repo.head_ref[0] != '\0') {
        size_t used = rt_strlen(caps);
        if (used + 13U + rt_strlen(repo.head_ref) < sizeof(caps)) {
            rt_copy_string(caps + used, sizeof(caps) - used, " symref=HEAD:");
            rt_copy_string(caps + rt_strlen(caps), sizeof(caps) - rt_strlen(caps), repo.head_ref);
        }
    }
    if (have_head) {
        if (gitd_append_ref_advertisement(&body, head_oid, "HEAD", caps) != 0) goto done;
        for (i = 0U; i < refs.count; ++i) {
            if (gitd_append_ref_advertisement(&body, refs.refs[i].oid, refs.refs[i].name, 0) != 0) goto done;
        }
    } else if (receive_pack) {
        if (gitd_append_zero_ref_advertisement(&body, caps) != 0) goto done;
    }
    if (tool_byte_buffer_append_cstr(&body, "0000") != 0) goto done;
    result = gitd_send_body(fd, 200, content_type, body.data, body.size);
done:
    git_buffer_destroy(&body);
    gitd_ref_list_destroy(&refs);
    if (result != 0) return gitd_send_text(fd, 500, "cannot advertise refs\n");
    return 0;
}

static int gitd_upload_pack_line_has_unsupported_feature(const unsigned char *payload, size_t payload_length) {
    while (payload_length > 0U && (payload[payload_length - 1U] == '\n' || payload[payload_length - 1U] == '\r')) payload_length -= 1U;
    if (payload_length >= 8U && memcmp(payload, "deepen ", 7U) == 0) return 1;
    if (payload_length >= 8U && memcmp(payload, "shallow ", 8U) == 0) return 1;
    if (payload_length >= 13U && memcmp(payload, "deepen-since ", 13U) == 0) return 1;
    if (payload_length >= 11U && memcmp(payload, "deepen-not ", 11U) == 0) return 1;
    if (payload_length >= 7U && memcmp(payload, "filter ", 7U) == 0) return 1;
    return 0;
}

static int gitd_parse_upload_pack_request(const GitBuffer *body, unsigned char want_oid[CRYPTO_SHA1_DIGEST_SIZE], GitOidList *haves, int *sideband_out, int *unsupported_out) {
    size_t pos = 0U;
    int saw_want = 0;
    int saw_done = 0;

    *sideband_out = 0;
    *unsupported_out = 0;
    rt_memset(haves, 0, sizeof(*haves));
    while (pos < body->size) {
        size_t packet_length;
        const unsigned char *payload;
        size_t payload_length;

        if (git_pkt_length(body->data + pos, body->size - pos, &packet_length) != 0) return -1;
        pos += 4U;
        if (packet_length == GIT_PACKET_FLUSH) continue;
        if (packet_length < 4U || pos + packet_length - 4U > body->size) return -1;
        payload = body->data + pos;
        payload_length = packet_length - 4U;
        pos += payload_length;
        if (gitd_upload_pack_line_has_unsupported_feature(payload, payload_length)) {
            *unsupported_out = 1;
        }
        if (payload_length >= 45U && memcmp(payload, "want ", 5U) == 0) {
            size_t i;
            if (!saw_want && git_parse_oid_hex_n((const char *)payload + 5U, GIT_OBJECT_HEX_SIZE, want_oid) != 0) return -1;
            saw_want = 1;
            for (i = 45U; i + 13U <= payload_length; ++i) {
                if (memcmp(payload + i, "side-band-64k", 13U) == 0) *sideband_out = 1;
            }
        } else if (payload_length >= 45U && memcmp(payload, "have ", 5U) == 0) {
            unsigned char have_oid[CRYPTO_SHA1_DIGEST_SIZE];
            if (git_parse_oid_hex_n((const char *)payload + 5U, GIT_OBJECT_HEX_SIZE, have_oid) != 0 || git_oid_list_push_unique(haves, have_oid) != 0) return -1;
        } else if (payload_length >= 5U && memcmp(payload, "done\n", 5U) == 0) {
            saw_done = 1;
            break;
        }
    }
    if (saw_want && !saw_done && *unsupported_out == 0) *unsupported_out = 2;
    return saw_want ? 0 : -1;
}

static int gitd_collect_excluded_haves(GitRepo *repo, const GitPack *pack_cache, const GitOidList *haves, GitOidList *excluded) {
    size_t i;

    rt_memset(excluded, 0, sizeof(*excluded));
    for (i = 0U; i < haves->count; ++i) {
        GitOidList reachable;
        size_t j;

        rt_memset(&reachable, 0, sizeof(reachable));
        if (git_collect_reachable_commits(repo, haves->oids[i], pack_cache, &reachable) != 0) {
            git_oid_list_destroy(&reachable);
            continue;
        }
        for (j = 0U; j < reachable.count; ++j) {
            if (git_oid_list_push_unique(excluded, reachable.oids[j]) != 0) {
                git_oid_list_destroy(&reachable);
                return -1;
            }
        }
        git_oid_list_destroy(&reachable);
    }
    return 0;
}

static int gitd_append_delta_varint(GitBuffer *buffer, size_t value) {
    unsigned char byte;

    byte = (unsigned char)(value & 0x7fU);
    value >>= 7U;
    while (value != 0U) {
        if (tool_byte_buffer_append_byte(buffer, byte | 0x80U) != 0) return -1;
        byte = (unsigned char)(value & 0x7fU);
        value >>= 7U;
    }
    return tool_byte_buffer_append_byte(buffer, byte);
}

static int gitd_build_insert_delta(size_t base_size, const unsigned char *target, size_t target_size, GitBuffer *delta_out) {
    size_t pos = 0U;

    rt_memset(delta_out, 0, sizeof(*delta_out));
    if (gitd_append_delta_varint(delta_out, base_size) != 0 || gitd_append_delta_varint(delta_out, target_size) != 0) goto fail;
    while (pos < target_size) {
        size_t chunk = target_size - pos;

        if (chunk > 127U) chunk = 127U;
        if (tool_byte_buffer_append_byte(delta_out, (unsigned char)chunk) != 0 || git_buffer_append(delta_out, target + pos, chunk) != 0) goto fail;
        pos += chunk;
    }
    return 0;
fail:
    git_buffer_destroy(delta_out);
    return -1;
}

static int gitd_append_compressed_pack_payload(GitBuffer *pack, const unsigned char *payload, size_t payload_size, const char *oid_hex) {
    unsigned char *compressed = 0;
    size_t compressed_capacity = compression_zlib_deflate_bound(payload_size);
    size_t compressed_size = 0U;
    int result = -1;

    compressed = (unsigned char *)rt_malloc(compressed_capacity == 0U ? 1U : compressed_capacity);
    if (compressed == 0) {
        tool_write_error("gitd", "out of memory while compressing pack object: ", oid_hex);
        return -1;
    }
    if (compression_zlib_deflate_level(payload, payload_size, compressed, compressed_capacity, &compressed_size, 6) != 0) {
        rt_free(compressed);
        compressed_capacity = compression_zlib_store_bound(payload_size);
        compressed = (unsigned char *)rt_malloc(compressed_capacity == 0U ? 1U : compressed_capacity);
        if (compressed == 0) {
            tool_write_error("gitd", "out of memory while storing pack object: ", oid_hex);
            return -1;
        }
        if (compression_zlib_store(payload, payload_size, compressed, compressed_capacity, &compressed_size) != 0) {
            tool_write_error("gitd", "cannot store pack object: ", oid_hex);
            goto done;
        }
    }
    if (git_buffer_append(pack, compressed, compressed_size) != 0) {
        tool_write_error("gitd", "cannot append compressed pack object: ", oid_hex);
        goto done;
    }
    result = 0;
done:
    rt_free(compressed);
    return result;
}

static int gitd_build_pack(GitRepo *repo, const GitPack *pack_cache, const GitOidList *objects, GitBuffer *pack_out) {
    GitBuffer pack;
    CryptoSha1Context sha1;
    unsigned char digest[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char base_blob_oid[CRYPTO_SHA1_DIGEST_SIZE];
    size_t base_blob_size = 0U;
    size_t i;
    int have_base_blob = 0;
    int result = -1;

    rt_memset(&pack, 0, sizeof(pack));
    rt_memset(base_blob_oid, 0, sizeof(base_blob_oid));
    if (tool_byte_buffer_append_cstr(&pack, "PACK") != 0 || tool_byte_buffer_append_u32_be(&pack, 2U) != 0 || tool_byte_buffer_append_u32_be(&pack, (unsigned long long)objects->count) != 0) goto done;
    for (i = 0U; i < objects->count; ++i) {
        int type = 0;
        unsigned char *data = 0;
        size_t size = 0U;
        char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];

        git_format_oid_hex(objects->oids[i], oid_hex);
        if (git_read_object(repo, objects->oids[i], pack_cache, &type, &data, &size) != 0) {
            tool_write_error("gitd", "cannot read object for upload pack: ", oid_hex);
            goto done;
        }
        if (type < GIT_OBJECT_COMMIT || type > GIT_OBJECT_TAG) {
            tool_write_error("gitd", "unsupported object type for upload pack: ", oid_hex);
            rt_free(data);
            goto done;
        }
        if (type == GIT_OBJECT_BLOB && have_base_blob) {
            GitBuffer delta;

            if (gitd_build_insert_delta(base_blob_size, data, size, &delta) != 0) {
                rt_free(data);
                goto done;
            }
            if (git_pack_append_object_header(&pack, GIT_OBJECT_REF_DELTA, delta.size) != 0 || git_buffer_append(&pack, base_blob_oid, sizeof(base_blob_oid)) != 0 || gitd_append_compressed_pack_payload(&pack, delta.data, delta.size, oid_hex) != 0) {
                git_buffer_destroy(&delta);
                rt_free(data);
                goto done;
            }
            git_buffer_destroy(&delta);
        } else {
            if (git_pack_append_object_header(&pack, type, size) != 0 || gitd_append_compressed_pack_payload(&pack, data, size, oid_hex) != 0) {
                rt_free(data);
                goto done;
            }
            if (type == GIT_OBJECT_BLOB && !have_base_blob) {
                memcpy(base_blob_oid, objects->oids[i], sizeof(base_blob_oid));
                base_blob_size = size;
                have_base_blob = 1;
            }
        }
        rt_free(data);
    }
    crypto_sha1_init(&sha1);
    crypto_sha1_update(&sha1, pack.data, pack.size);
    crypto_sha1_final(&sha1, digest);
    if (git_buffer_append(&pack, digest, sizeof(digest)) != 0) goto done;
    *pack_out = pack;
    rt_memset(&pack, 0, sizeof(pack));
    result = 0;
done:
    git_buffer_destroy(&pack);
    return result;
}

static int gitd_append_sideband_pack(GitBuffer *out, const GitBuffer *pack, int sideband) {
    size_t pos = 0U;

    if (git_append_pkt_line(out, "NAK\n") != 0) return -1;
    while (pos < pack->size) {
        size_t chunk = pack->size - pos;
        GitBuffer payload;
        int result;
        if (chunk > GITD_SIDEBAND_CHUNK) chunk = GITD_SIDEBAND_CHUNK;
        rt_memset(&payload, 0, sizeof(payload));
        if (sideband) {
            if (tool_byte_buffer_append_byte(&payload, 1U) != 0 || git_buffer_append(&payload, pack->data + pos, chunk) != 0) {
                git_buffer_destroy(&payload);
                return -1;
            }
            result = git_append_pkt_data(out, payload.data, payload.size);
        } else {
            result = git_append_pkt_data(out, pack->data + pos, chunk);
        }
        git_buffer_destroy(&payload);
        if (result != 0) return -1;
        pos += chunk;
    }
    return tool_byte_buffer_append_cstr(out, "0000");
}

static int gitd_handle_upload_pack(int fd, const GitdOptions *options, const GitdRequest *request, const GitBuffer *body) {
    char repo_path[GIT_PATH_CAPACITY];
    GitRepo repo;
    GitPack pack_cache;
    GitOidList objects;
    GitOidList visited;
    GitOidList haves;
    GitOidList excluded;
    GitBuffer pack;
    GitBuffer response;
    unsigned char want_oid[CRYPTO_SHA1_DIGEST_SIZE];
    int have_pack = 0;
    int sideband = 0;
    int unsupported = 0;
    int result = -1;

    if (rt_strcmp(request->method, "POST") != 0) return gitd_send_text(fd, 405, "method not allowed\n");
        if (git_header_value_contains((const unsigned char *)request->git_protocol, rt_strlen(request->git_protocol), "version=2")) return gitd_send_text(fd, 501, "protocol v2 is not supported\n");
    if (!git_header_value_contains((const unsigned char *)request->content_type, rt_strlen(request->content_type), "application/x-git-upload-pack-request")) return gitd_send_text(fd, 415, "expected git-upload-pack request\n");
    if (gitd_strip_suffix(request->path, "/git-upload-pack", repo_path, sizeof(repo_path)) != 0 || gitd_repo_from_path(options, repo_path, &repo) != 0) return gitd_send_text(fd, 404, "repository not found\n");
    rt_memset(&pack_cache, 0, sizeof(pack_cache));
    rt_memset(&objects, 0, sizeof(objects));
    rt_memset(&visited, 0, sizeof(visited));
    rt_memset(&haves, 0, sizeof(haves));
    rt_memset(&excluded, 0, sizeof(excluded));
    rt_memset(&pack, 0, sizeof(pack));
    rt_memset(&response, 0, sizeof(response));
    if (gitd_parse_upload_pack_request(body, want_oid, &haves, &sideband, &unsupported) != 0) return gitd_send_text(fd, 400, "malformed upload-pack request\n");
    if (unsupported == 1) return gitd_send_text(fd, 501, "shallow and filter upload-pack requests are not supported\n");
    if (unsupported == 2) return gitd_send_text(fd, 501, "multi-round upload-pack negotiation is not supported\n");
    have_pack = git_load_pack_cache(&repo, &pack_cache) == 0;
    if (gitd_collect_excluded_haves(&repo, have_pack ? &pack_cache : 0, &haves, &excluded) != 0) goto done;
    if (git_push_collect_commit_objects(&repo, want_oid, have_pack ? &pack_cache : 0, excluded.count > 0U ? &excluded : 0, &objects, &visited) != 0) goto done;
    if (gitd_build_pack(&repo, have_pack ? &pack_cache : 0, &objects, &pack) != 0) goto done;
    if (gitd_append_sideband_pack(&response, &pack, sideband) != 0) goto done;
    result = gitd_send_body(fd, 200, "application/x-git-upload-pack-result", response.data, response.size);
done:
    if (have_pack) git_pack_destroy(&pack_cache);
    git_oid_list_destroy(&objects);
    git_oid_list_destroy(&visited);
    git_oid_list_destroy(&haves);
    git_oid_list_destroy(&excluded);
    git_buffer_destroy(&pack);
    git_buffer_destroy(&response);
    if (result != 0) return gitd_send_text(fd, 500, "cannot build upload pack\n");
    return 0;
}

static int gitd_parse_receive_pack_request(const GitBuffer *body, GitdReceiveRequest *receive) {
    size_t pos = 0U;
    int first = 1;

    rt_memset(receive, 0, sizeof(*receive));
    while (pos < body->size) {
        size_t packet_length;
        const unsigned char *payload;
        size_t payload_length;
        size_t line_length;
        size_t old_start = 0U;
        size_t old_end;
        size_t new_start;
        size_t new_end;
        size_t ref_start;
        size_t ref_end;
        GitdReceiveCommand command;

        if (git_pkt_length(body->data + pos, body->size - pos, &packet_length) != 0) return -1;
        pos += 4U;
        if (packet_length == GIT_PACKET_FLUSH) {
            receive->pack_data = body->data + pos;
            receive->pack_size = body->size - pos;
            return receive->count > 0U ? 0 : -1;
        }
        if (packet_length < 4U || pos + packet_length - 4U > body->size) return -1;
        payload = body->data + pos;
        payload_length = packet_length - 4U;
        pos += payload_length;
        line_length = payload_length;
        while (line_length > 0U && (payload[line_length - 1U] == '\n' || payload[line_length - 1U] == '\r')) line_length -= 1U;
        if (first) {
            size_t cap_start;
            for (cap_start = 0U; cap_start < line_length; ++cap_start) {
                if (payload[cap_start] == '\0') break;
            }
            if (cap_start < line_length) {
                size_t cap_pos = cap_start + 1U;
                line_length = cap_start;
                while (cap_pos < payload_length) {
                    size_t start = cap_pos;
                    while (cap_pos < payload_length && payload[cap_pos] != ' ' && payload[cap_pos] != '\n' && payload[cap_pos] != '\r') cap_pos += 1U;
                    if (cap_pos > start) {
                        if (cap_pos - start == 13U && memcmp(payload + start, "report-status", 13U) == 0) receive->report_status = 1;
                        if (cap_pos - start == 13U && memcmp(payload + start, "side-band-64k", 13U) == 0) receive->sideband = 1;
                    }
                    while (cap_pos < payload_length && (payload[cap_pos] == ' ' || payload[cap_pos] == '\n' || payload[cap_pos] == '\r')) cap_pos += 1U;
                }
            }
            first = 0;
        }
        old_end = old_start;
        while (old_end < line_length && payload[old_end] != ' ') old_end += 1U;
        new_start = old_end + 1U;
        new_end = new_start;
        while (new_end < line_length && payload[new_end] != ' ') new_end += 1U;
        ref_start = new_end + 1U;
        ref_end = line_length;
        if (old_end != GIT_OBJECT_HEX_SIZE || new_end - new_start != GIT_OBJECT_HEX_SIZE || ref_start >= ref_end || ref_end - ref_start >= GIT_REF_CAPACITY) return -1;
        rt_memset(&command, 0, sizeof(command));
        if (git_parse_oid_hex_n((const char *)payload + old_start, GIT_OBJECT_HEX_SIZE, command.old_oid) != 0 ||
            git_parse_oid_hex_n((const char *)payload + new_start, GIT_OBJECT_HEX_SIZE, command.new_oid) != 0) return -1;
        memcpy(command.ref_name, payload + ref_start, ref_end - ref_start);
        command.ref_name[ref_end - ref_start] = '\0';
        if (gitd_receive_request_push(receive, &command) != 0) return -1;
    }
    return -1;
}

static int gitd_store_received_pack(GitRepo *repo, const GitdReceiveRequest *receive, GitPack *pack_out) {
    GitPack pack;
    int result = -1;

    rt_memset(pack_out, 0, sizeof(*pack_out));
    rt_memset(&pack, 0, sizeof(pack));
    if (receive->pack_size < 12U || memcmp(receive->pack_data, "PACK", 4U) != 0) {
        return -1;
    }
    if (git_parse_pack(receive->pack_data, receive->pack_size, &pack) != 0 || git_resolve_pack_deltas(&pack) != 0) {
        goto done;
    }
    if (git_write_pack_file(repo, receive->pack_data, receive->pack_size, &pack) != 0) {
        goto done;
    }
    *pack_out = pack;
    rt_memset(&pack, 0, sizeof(pack));
    result = 0;
done:
    git_pack_destroy(&pack);
    return result;
}

static int gitd_receive_request_is_delete_only(const GitdReceiveRequest *receive) {
    size_t i;

    if (receive->count == 0U) return 0;
    for (i = 0U; i < receive->count; ++i) {
        if (!gitd_oid_is_zero(receive->commands[i].new_oid)) return 0;
    }
    return 1;
}

static int gitd_current_ref_oid(GitRepo *repo, const char *ref_name, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], int *exists_out) {
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];

    *exists_out = 0;
    if (git_resolve_ref(repo, ref_name, oid_hex, sizeof(oid_hex)) != 0) {
        rt_memset(oid, 0, CRYPTO_SHA1_DIGEST_SIZE);
        return 0;
    }
    if (git_parse_oid_hex_n(oid_hex, GIT_OBJECT_HEX_SIZE, oid) != 0) return -1;
    *exists_out = 1;
    return 0;
}

static int gitd_delete_ref(GitRepo *repo, const char *ref_name) {
    char path[GIT_PATH_CAPACITY];
    PlatformDirEntry entry;
    int deleted = 0;

    if (git_join(path, sizeof(path), repo->git_dir, ref_name) != 0) return -1;
    if (platform_remove_file(path) == 0) deleted = 1;
    if (platform_get_path_info(path, &entry) == 0 && !entry.is_dir) return -1;
    if (git_delete_packed_ref(repo, ref_name, &deleted) != 0) return -1;
    if (platform_get_path_info(path, &entry) == 0 && !entry.is_dir) return -1;
    return 0;
}

static const char *gitd_validate_receive_command(GitRepo *repo, const GitPack *pack_cache, const GitdReceiveCommand *command) {
    unsigned char current_oid[CRYPTO_SHA1_DIGEST_SIZE];
    int exists = 0;
    int type = 0;
    unsigned char *data = 0;
    size_t size = 0U;

    if (!gitd_ref_is_safe(command->ref_name)) return "unsafe ref name";
    if (gitd_current_ref_oid(repo, command->ref_name, current_oid, &exists) != 0) return "cannot read current ref";
    if (exists) {
        if (gitd_oid_is_zero(command->old_oid) || !git_oid_equal(current_oid, command->old_oid)) return "stale ref";
        if (!gitd_oid_is_zero(command->new_oid) && gitd_ref_is_branch(command->ref_name) && !git_commit_is_ancestor_of(repo, current_oid, command->new_oid, pack_cache)) return "non-fast-forward";
    } else if (!gitd_oid_is_zero(command->old_oid)) {
        return "cannot create ref with nonzero old oid";
    }
    if (gitd_oid_is_zero(command->new_oid)) return exists ? 0 : "cannot delete missing ref";
    if (git_read_object(repo, command->new_oid, pack_cache, &type, &data, &size) != 0 || type < GIT_OBJECT_COMMIT || type > GIT_OBJECT_TAG) {
        rt_free(data);
        return "new oid is not an object";
    }
    if (gitd_ref_is_branch(command->ref_name) && type != GIT_OBJECT_COMMIT) {
        rt_free(data);
        return "branch oid is not a commit";
    }
    rt_free(data);
    return 0;
}

static int gitd_apply_receive_commands(GitRepo *repo, const GitPack *pack_cache, const GitdReceiveRequest *receive, const char **error_out, const char **error_ref_out) {
    size_t i;

    for (i = 0U; i < receive->count; ++i) {
        const char *error = gitd_validate_receive_command(repo, pack_cache, &receive->commands[i]);
        if (error != 0) {
            *error_out = error;
            *error_ref_out = receive->commands[i].ref_name;
            return -1;
        }
    }
    for (i = 0U; i < receive->count; ++i) {
        if (gitd_oid_is_zero(receive->commands[i].new_oid)) {
            if (gitd_delete_ref(repo, receive->commands[i].ref_name) != 0) {
                *error_out = "cannot delete ref";
                *error_ref_out = receive->commands[i].ref_name;
                return -1;
            }
            continue;
        }
        if (git_write_ref_oid(repo, receive->commands[i].ref_name, receive->commands[i].new_oid) != 0) {
            *error_out = "cannot update ref";
            *error_ref_out = receive->commands[i].ref_name;
            return -1;
        }
    }
    return 0;
}

static int gitd_append_receive_status_payload(GitBuffer *payload, const GitdReceiveRequest *receive, const char *error, const char *error_ref) {
    size_t i;

    if (tool_byte_buffer_append_cstr(payload, "unpack ok\n") != 0) return -1;
    for (i = 0U; i < receive->count; ++i) {
        if (error != 0 && rt_strcmp(receive->commands[i].ref_name, error_ref) == 0) {
            if (tool_byte_buffer_append_cstr(payload, "ng ") != 0 || tool_byte_buffer_append_cstr(payload, receive->commands[i].ref_name) != 0 || tool_byte_buffer_append_char(payload, ' ') != 0 || tool_byte_buffer_append_cstr(payload, error) != 0 || tool_byte_buffer_append_char(payload, '\n') != 0) return -1;
        } else if (error != 0) {
            if (tool_byte_buffer_append_cstr(payload, "ng ") != 0 || tool_byte_buffer_append_cstr(payload, receive->commands[i].ref_name) != 0 || tool_byte_buffer_append_cstr(payload, " transaction rejected\n") != 0) return -1;
        } else if (tool_byte_buffer_append_cstr(payload, "ok ") != 0 || tool_byte_buffer_append_cstr(payload, receive->commands[i].ref_name) != 0 || tool_byte_buffer_append_char(payload, '\n') != 0) {
            return -1;
        }
    }
    return 0;
}

static int gitd_send_receive_status(int fd, const GitdReceiveRequest *receive, const char *error, const char *error_ref) {
    GitBuffer payload;
    GitBuffer status;
    GitBuffer response;
    int result = -1;
    size_t pos = 0U;

    rt_memset(&payload, 0, sizeof(payload));
    rt_memset(&status, 0, sizeof(status));
    rt_memset(&response, 0, sizeof(response));
    if (gitd_append_receive_status_payload(&payload, receive, error, error_ref) != 0) goto done;
    while (pos < payload.size) {
        size_t start = pos;

        while (pos < payload.size && payload.data[pos] != '\n') pos += 1U;
        if (pos < payload.size) pos += 1U;
        if (git_append_pkt_data(&status, payload.data + start, pos - start) != 0) goto done;
    }
    if (tool_byte_buffer_append_cstr(&status, "0000") != 0) goto done;
    if (receive->sideband) {
        pos = 0U;
        while (pos < status.size) {
            GitBuffer band;
            size_t chunk = status.size - pos;
            int append_result;

            if (chunk > GITD_SIDEBAND_CHUNK) chunk = GITD_SIDEBAND_CHUNK;
            rt_memset(&band, 0, sizeof(band));
            if (tool_byte_buffer_append_byte(&band, 1U) != 0 || git_buffer_append(&band, status.data + pos, chunk) != 0) {
                git_buffer_destroy(&band);
                goto done;
            }
            append_result = git_append_pkt_data(&response, band.data, band.size);
            git_buffer_destroy(&band);
            if (append_result != 0) goto done;
            pos += chunk;
        }
        if (tool_byte_buffer_append_cstr(&response, "0000") != 0) goto done;
    } else if (git_buffer_append(&response, status.data, status.size) != 0) {
        goto done;
    }
    result = gitd_send_body(fd, 200, "application/x-git-receive-pack-result", response.data, response.size);
done:
    git_buffer_destroy(&payload);
    git_buffer_destroy(&status);
    git_buffer_destroy(&response);
    return result;
}

static int gitd_handle_receive_pack(int fd, const GitdOptions *options, const GitdRequest *request, const GitBuffer *body) {
    char repo_path[GIT_PATH_CAPACITY];
    GitRepo repo;
    GitdReceiveRequest receive;
    GitPack received_pack;
    const char *error = 0;
    const char *error_ref = 0;
    int result;

    if (rt_strcmp(request->method, "POST") != 0) return gitd_send_text(fd, 405, "method not allowed\n");
    if (!git_header_value_contains((const unsigned char *)request->content_type, rt_strlen(request->content_type), "application/x-git-receive-pack-request")) return gitd_send_text(fd, 415, "expected git-receive-pack request\n");
    if (gitd_strip_suffix(request->path, "/git-receive-pack", repo_path, sizeof(repo_path)) != 0 || gitd_repo_from_path(options, repo_path, &repo) != 0) return gitd_send_text(fd, 404, "repository not found\n");
    rt_memset(&receive, 0, sizeof(receive));
    rt_memset(&received_pack, 0, sizeof(received_pack));
    if (gitd_parse_receive_pack_request(body, &receive) != 0) {
        gitd_receive_request_destroy(&receive);
        return gitd_send_text(fd, 400, "malformed receive-pack request\n");
    }
    if (gitd_receive_request_is_delete_only(&receive)) {
        if (gitd_apply_receive_commands(&repo, 0, &receive, &error, &error_ref) != 0) {
            /* error fields are set by validation. */
        }
    } else if (gitd_store_received_pack(&repo, &receive, &received_pack) != 0) {
        error = "unpack failed";
        error_ref = receive.count > 0U ? receive.commands[0].ref_name : "refs/heads/unknown";
    } else if (gitd_apply_receive_commands(&repo, &received_pack, &receive, &error, &error_ref) != 0) {
        /* error fields are set by validation. */
    }
    result = gitd_send_receive_status(fd, &receive, error, error_ref);
    git_pack_destroy(&received_pack);
    gitd_receive_request_destroy(&receive);
    return result;
}

static int gitd_dispatch_request(int fd, const GitdOptions *options, const GitdRequest *request, const GitBuffer *body) {
    int result;

    if (rt_strcmp(request->method, "OPTIONS") == 0) {
        result = gitd_send_options(fd);
    } else if (rt_strcmp(request->path, "/health") == 0 || rt_strcmp(request->path, "/_status") == 0) {
        result = gitd_send_text(fd, 200, "ok\n");
    } else if (rt_strlen(request->path) >= 10U && rt_strcmp(request->path + rt_strlen(request->path) - 10U, "/info/refs") == 0) {
        result = gitd_handle_info_refs(fd, options, request);
    } else if (rt_strlen(request->path) >= 16U && rt_strcmp(request->path + rt_strlen(request->path) - 16U, "/git-upload-pack") == 0) {
        result = gitd_handle_upload_pack(fd, options, request, body);
    } else if (rt_strlen(request->path) >= 17U && rt_strcmp(request->path + rt_strlen(request->path) - 17U, "/git-receive-pack") == 0) {
        result = gitd_handle_receive_pack(fd, options, request, body);
    } else {
        result = gitd_send_text(fd, 404, "not found\n");
    }
    return result;
}

static void gitd_connection_destroy(GitdConnection *connection) {
    if (connection == 0) return;
    if (connection->fd >= 0) {
        (void)rt_io_loop_remove(&connection->server->loop, connection->fd);
        (void)platform_close(connection->fd);
    }
    git_buffer_destroy(&connection->raw);
    git_buffer_destroy(&connection->body);
    rt_free(connection);
}

static void gitd_connection_ready(int fd, unsigned int events, void *arg) {
    GitdConnection *connection = (GitdConnection *)arg;
    int complete = 0;

    (void)events;
    if (gitd_read_request_step(connection, &complete) != 0) {
        (void)gitd_send_text(fd, 400, "bad request\n");
        gitd_connection_destroy(connection);
        return;
    }
    if (!complete) {
        return;
    }
    (void)rt_io_loop_remove(&connection->server->loop, fd);
    (void)gitd_dispatch_request(fd, &connection->server->options, &connection->request, &connection->body);
    connection->server->handled_connections += 1U;
    if (connection->server->options.once) {
        rt_io_loop_stop(&connection->server->loop);
    }
    connection->fd = -1;
    (void)platform_close(fd);
    gitd_connection_destroy(connection);
}

static int gitd_connection_add(GitdServer *server, int client_fd) {
    GitdConnection *connection;

    connection = (GitdConnection *)rt_malloc(sizeof(*connection));
    if (connection == 0) return -1;
    rt_memset(connection, 0, sizeof(*connection));
    connection->server = server;
    connection->fd = client_fd;
    if (rt_io_loop_add(&server->loop, client_fd, RT_IO_READ, gitd_connection_ready, connection) != 0) {
        connection->fd = -1;
        rt_free(connection);
        return -1;
    }
    return 0;
}

static int gitd_parse_options(int argc, char **argv, GitdOptions *options) {
    ToolOptState opt;
    int parse_result;

    rt_memset(options, 0, sizeof(*options));
    rt_copy_string(options->bind_host, sizeof(options->bind_host), "0.0.0.0");
    rt_copy_string(options->repo_root, sizeof(options->repo_root), ".");
    options->port = 8090U;
    tool_opt_init(&opt, argc, argv, tool_base_name(argv[0]), "[-b HOST] [-p PORT] [-r REPO_ROOT] [--once] [-q]");
    while ((parse_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        unsigned long long number;
        if (rt_strcmp(opt.flag, "-b") == 0 || rt_strcmp(opt.flag, "--bind") == 0) {
            if (tool_opt_require_value(&opt) != 0) return -1;
            rt_copy_string(options->bind_host, sizeof(options->bind_host), opt.value);
        } else if (rt_strcmp(opt.flag, "-p") == 0 || rt_strcmp(opt.flag, "--port") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_parse_uint(opt.value, &number) != 0 || number == 0ULL || number > 65535ULL) return -1;
            options->port = (unsigned int)number;
        } else if (rt_strcmp(opt.flag, "-r") == 0 || rt_strcmp(opt.flag, "--repo-root") == 0) {
            if (tool_opt_require_value(&opt) != 0) return -1;
            rt_copy_string(options->repo_root, sizeof(options->repo_root), opt.value);
        } else if (rt_strcmp(opt.flag, "--once") == 0) {
            options->once = 1;
        } else if (rt_strcmp(opt.flag, "-q") == 0 || rt_strcmp(opt.flag, "--quiet") == 0) {
            options->quiet = 1;
        } else {
            return -1;
        }
    }
    if (parse_result == TOOL_OPT_HELP) {
        gitd_usage(argv[0]);
        rt_write_line(1, "Serve bare Git repositories over smart HTTP with permissive CORS.");
        return 1;
    }
    if (parse_result != TOOL_OPT_END) return -1;
    return 0;
}

static void gitd_listener_ready(int fd, unsigned int events, void *arg) {
    GitdServer *server = (GitdServer *)arg;
    int client_fd = -1;

    (void)events;
    if (platform_accept_tcp(fd, &client_fd) != 0) {
        return;
    }
    (void)rt_io_loop_remove(&server->loop, fd);
    if (gitd_connection_add(server, client_fd) != 0) {
        (void)platform_close(client_fd);
    }
    if (!server->options.once) {
        (void)rt_io_loop_add(&server->loop, fd, RT_IO_READ, gitd_listener_ready, server);
    }
}

static int gitd_run_server(GitdServer *server) {
    if (rt_io_loop_init(&server->loop) != 0) return -1;
    if (rt_io_loop_add(&server->loop, server->listener_fd, RT_IO_READ, gitd_listener_ready, server) != 0) {
        rt_io_loop_destroy(&server->loop);
        return -1;
    }
    if (rt_io_loop_run(&server->loop) != 0) {
        rt_io_loop_destroy(&server->loop);
        return -1;
    }
    rt_io_loop_destroy(&server->loop);
    return 0;
}

int main(int argc, char **argv) {
    GitdServer server;
    int parse_status;

    rt_memset(&server, 0, sizeof(server));
    server.listener_fd = -1;
    parse_status = gitd_parse_options(argc, argv, &server.options);
    if (parse_status > 0) return 0;
    if (parse_status != 0) {
        gitd_usage(argv[0]);
        return 1;
    }
    if (platform_open_tcp_listener(server.options.bind_host, server.options.port, &server.listener_fd) != 0) {
        tool_write_error("gitd", "cannot listen on port", 0);
        return 1;
    }
    if (!server.options.quiet) {
        char port_text[32];
        rt_unsigned_to_string(server.options.port, port_text, sizeof(port_text));
        rt_write_cstr(2, "gitd listening on http://");
        rt_write_cstr(2, server.options.bind_host);
        rt_write_cstr(2, ":");
        rt_write_cstr(2, port_text);
        rt_write_cstr(2, "/ from ");
        rt_write_line(2, server.options.repo_root);
    }
    if (gitd_run_server(&server) != 0) {
        (void)platform_close(server.listener_fd);
        return 1;
    }
    (void)platform_close(server.listener_fd);
    return 0;
}