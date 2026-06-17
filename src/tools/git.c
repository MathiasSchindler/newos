#include "compression/zlib.h"
#include "crypto/sha1.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define GIT_PATH_CAPACITY 2048U
#define GIT_REF_CAPACITY 512U
#define GIT_OBJECT_HEX_SIZE 40U
#define GIT_INDEX_SIGNATURE 0x44495243U
#define GIT_MODE_TYPE_MASK 0170000U
#define GIT_MODE_REGULAR_FILE 0100644U
#define GIT_MODE_REGULAR_EXECUTABLE 0100755U
#define GIT_MODE_REGULAR_TYPE 0100000U
#define GIT_MODE_TREE 0040000U
#define GIT_MODE_SYMLINK 0120000U
#define GIT_MODE_GITLINK 0160000U
#define GIT_MODE_EXEC_BITS 0111U
#define GIT_SCHEME_HTTP 1
#define GIT_SCHEME_HTTPS 2
#define GIT_PACKET_FLUSH 0
#define GIT_OBJECT_COMMIT 1
#define GIT_OBJECT_TREE 2
#define GIT_OBJECT_BLOB 3
#define GIT_OBJECT_TAG 4
#define GIT_OBJECT_OFS_DELTA 6
#define GIT_OBJECT_REF_DELTA 7
#define GIT_MAX_OBJECT_SIZE (128U * 1024U * 1024U)

typedef struct {
    char work_tree[GIT_PATH_CAPACITY];
    char git_dir[GIT_PATH_CAPACITY];
    char head_ref[GIT_REF_CAPACITY];
    char head_oid[GIT_OBJECT_HEX_SIZE + 1U];
    int head_is_branch;
} GitRepo;

typedef struct {
    char *path;
    unsigned int mode;
    unsigned long long size;
    unsigned long long mtime_seconds;
    unsigned int mtime_nanos;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
} GitIndexEntry;

typedef struct {
    GitIndexEntry *entries;
    size_t count;
    size_t capacity;
} GitIndex;

typedef struct {
    char *pattern;
    int directory_only;
    int negated;
    int has_slash;
    int has_wildcard;
} GitIgnorePattern;

typedef struct {
    GitIgnorePattern *patterns;
    size_t count;
    size_t capacity;
} GitIgnoreList;

static int git_path_has_slash(const char *path);
static int git_path_has_wildcard(const char *path);

typedef struct {
    const GitRepo *repo;
    const GitIndex *index;
    const GitIgnoreList *ignores;
    int porcelain;
    int saw_change;
} GitStatusWalk;

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} GitBuffer;

typedef struct {
    int scheme;
    unsigned int port;
    char host[256];
    char path[1024];
} GitUrl;

typedef struct {
    int use_tls;
    int socket_fd;
    PlatformTlsClient tls;
} GitHttpConnection;

typedef struct {
    char *name;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
} GitRemoteRef;

typedef struct {
    GitRemoteRef *refs;
    size_t count;
    size_t capacity;
    char head_ref[GIT_REF_CAPACITY];
    unsigned char head_oid[CRYPTO_SHA1_DIGEST_SIZE];
    int has_head;
} GitRemoteRefs;

typedef struct {
    unsigned long long offset;
    int type;
    int resolved;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char base_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned long long base_offset;
    unsigned char *data;
    size_t size;
} GitPackObject;

typedef struct {
    GitPackObject *objects;
    size_t count;
    size_t capacity;
} GitPack;

typedef struct {
    GitIndex entries;
    const GitRepo *repo;
} GitCheckoutIndex;

static void git_index_destroy(GitIndex *index) {
    size_t i;

    if (index == 0) {
        return;
    }
    for (i = 0U; i < index->count; ++i) {
        rt_free(index->entries[i].path);
    }
    rt_free(index->entries);
    rt_memset(index, 0, sizeof(*index));
}

static void git_ignore_destroy(GitIgnoreList *ignores) {
    size_t i;

    if (ignores == 0) {
        return;
    }
    for (i = 0U; i < ignores->count; ++i) {
        rt_free(ignores->patterns[i].pattern);
    }
    rt_free(ignores->patterns);
    rt_memset(ignores, 0, sizeof(*ignores));
}

static void git_buffer_destroy(GitBuffer *buffer) {
    if (buffer == 0) {
        return;
    }
    rt_free(buffer->data);
    rt_memset(buffer, 0, sizeof(*buffer));
}

static void git_remote_refs_destroy(GitRemoteRefs *refs) {
    size_t i;

    if (refs == 0) {
        return;
    }
    for (i = 0U; i < refs->count; ++i) {
        rt_free(refs->refs[i].name);
    }
    rt_free(refs->refs);
    rt_memset(refs, 0, sizeof(*refs));
}

static void git_pack_destroy(GitPack *pack) {
    size_t i;

    if (pack == 0) {
        return;
    }
    for (i = 0U; i < pack->count; ++i) {
        rt_free(pack->objects[i].data);
    }
    rt_free(pack->objects);
    rt_memset(pack, 0, sizeof(*pack));
}

static char *git_strdup_n(const char *text, size_t length) {
    char *copy = (char *)rt_malloc(length + 1U);

    if (copy == 0) {
        return 0;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static int git_copy(char *buffer, size_t buffer_size, const char *text) {
    if (buffer_size == 0U || rt_strlen(text) >= buffer_size) {
        return -1;
    }
    rt_copy_string(buffer, buffer_size, text);
    return 0;
}

static int git_buffer_reserve(GitBuffer *buffer, size_t needed) {
    unsigned char *new_data;
    size_t new_capacity;

    if (needed <= buffer->capacity) {
        return 0;
    }
    new_capacity = buffer->capacity == 0U ? 4096U : buffer->capacity;
    while (new_capacity < needed) {
        if (new_capacity > ((size_t)-1) / 2U) {
            return -1;
        }
        new_capacity *= 2U;
    }
    new_data = (unsigned char *)rt_realloc(buffer->data, new_capacity);
    if (new_data == 0) {
        return -1;
    }
    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return 0;
}

static int git_buffer_append(GitBuffer *buffer, const void *data, size_t size) {
    if (size == 0U) {
        return 0;
    }
    if (data == 0 || buffer->size > ((size_t)-1) - size || git_buffer_reserve(buffer, buffer->size + size) != 0) {
        return -1;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 0;
}

static int git_buffer_append_cstr(GitBuffer *buffer, const char *text) {
    return git_buffer_append(buffer, text, rt_strlen(text));
}

static int git_buffer_append_char(GitBuffer *buffer, char ch) {
    return git_buffer_append(buffer, &ch, 1U);
}

static int git_hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return (int)(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (int)(ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (int)(ch - 'A');
    }
    return -1;
}

static int git_parse_oid_hex_n(const char *text, size_t length, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    size_t i;

    if (text == 0 || length < GIT_OBJECT_HEX_SIZE) {
        return -1;
    }
    for (i = 0U; i < CRYPTO_SHA1_DIGEST_SIZE; ++i) {
        int hi = git_hex_value(text[i * 2U]);
        int lo = git_hex_value(text[i * 2U + 1U]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        oid[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

static int git_parse_oid_hex(const char *text, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    return git_parse_oid_hex_n(text, rt_strlen(text), oid);
}

static int git_oid_equal(const unsigned char left[CRYPTO_SHA1_DIGEST_SIZE], const unsigned char right[CRYPTO_SHA1_DIGEST_SIZE]) {
    return memcmp(left, right, CRYPTO_SHA1_DIGEST_SIZE) == 0;
}

static void git_write_u32_be(unsigned char *out, unsigned int value) {
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static int git_write_all_file(const char *path, const void *data, size_t size, unsigned int mode) {
    int fd = platform_open_write(path, mode);
    int result;

    if (fd < 0) {
        return -1;
    }
    result = rt_write_all(fd, data, size);
    if (platform_close(fd) != 0) {
        result = -1;
    }
    return result;
}

static int git_join(char *buffer, size_t buffer_size, const char *left, const char *right) {
    return tool_join_path(left, right, buffer, buffer_size);
}

static int git_read_file(const char *path, unsigned char **data_out, size_t *size_out) {
    return tool_read_all_input(path, data_out, size_out);
}

static void git_trim_line(char *text) {
    size_t len;

    if (text == 0) {
        return;
    }
    len = rt_strlen(text);
    while (len > 0U && (text[len - 1U] == '\n' || text[len - 1U] == '\r' || text[len - 1U] == ' ' || text[len - 1U] == '\t')) {
        text[len - 1U] = '\0';
        len -= 1U;
    }
}

static int git_read_text_file(const char *path, char *buffer, size_t buffer_size) {
    unsigned char *data = 0;
    size_t size = 0U;
    size_t copy_size;

    if (git_read_file(path, &data, &size) != 0) {
        return -1;
    }
    if (buffer_size == 0U) {
        rt_free(data);
        return -1;
    }
    copy_size = size < buffer_size - 1U ? size : buffer_size - 1U;
    memcpy(buffer, data, copy_size);
    buffer[copy_size] = '\0';
    rt_free(data);
    git_trim_line(buffer);
    return 0;
}

static unsigned int git_default_port_for_scheme(int scheme) {
    return scheme == GIT_SCHEME_HTTPS ? 443U : 80U;
}

static int git_parse_http_url(const char *text, unsigned int default_port, int scheme, GitUrl *url_out) {
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
    if (host_length == 0U || host_length >= sizeof(url_out->host)) {
        return -1;
    }
    memcpy(url_out->host, text + host_start, host_length);
    url_out->host[host_length] = '\0';

    if (text[index] == ':') {
        parsed_port = 0ULL;
        index += 1U;
        while (text[index] >= '0' && text[index] <= '9') {
            saw_port_digit = 1;
            parsed_port = parsed_port * 10ULL + (unsigned long long)(text[index] - '0');
            index += 1U;
        }
        if (!saw_port_digit || parsed_port == 0ULL || parsed_port > 65535ULL) {
            return -1;
        }
    }
    url_out->port = (unsigned int)parsed_port;
    if (text[index] == '\0') {
        rt_copy_string(url_out->path, sizeof(url_out->path), "/");
        return 0;
    }
    if (text[index] == '?' || text[index] == '#') {
        rt_copy_string(url_out->path, sizeof(url_out->path), "/");
        return 0;
    }
    if (rt_strlen(text + index) >= sizeof(url_out->path)) {
        return -1;
    }
    rt_copy_string(url_out->path, sizeof(url_out->path), text + index);
    return 0;
}

static int git_parse_url(const char *text, GitUrl *url_out) {
    if (tool_starts_with(text, "http://")) {
        return git_parse_http_url(text + 7, 80U, GIT_SCHEME_HTTP, url_out);
    }
    if (tool_starts_with(text, "https://")) {
        return git_parse_http_url(text + 8, 443U, GIT_SCHEME_HTTPS, url_out);
    }
    return -1;
}

static int git_url_is_http(const char *text) {
    return tool_starts_with(text, "http://") || tool_starts_with(text, "https://");
}

static int git_http_connect(const GitUrl *url, GitHttpConnection *connection) {
    rt_memset(connection, 0, sizeof(*connection));
    connection->socket_fd = -1;
    connection->use_tls = url->scheme == GIT_SCHEME_HTTPS;
    if (connection->use_tls) {
        if (platform_tls_connect(&connection->tls, url->host, url->port) != 0) {
            return -1;
        }
        connection->socket_fd = connection->tls.socket_fd;
        return 0;
    }
    return platform_connect_tcp(url->host, url->port, &connection->socket_fd);
}

static long git_http_read(GitHttpConnection *connection, void *buffer, size_t count) {
    return connection->use_tls ? platform_tls_read(&connection->tls, buffer, count) : platform_read(connection->socket_fd, buffer, count);
}

static int git_http_write_all(GitHttpConnection *connection, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t written = 0U;

    while (written < size) {
        long result = connection->use_tls ? platform_tls_write(&connection->tls, bytes + written, size - written) : platform_write(connection->socket_fd, bytes + written, size - written);
        if (result <= 0) {
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

static void git_http_close(GitHttpConnection *connection) {
    if (connection->use_tls) {
        platform_tls_close(&connection->tls);
    } else if (connection->socket_fd >= 0) {
        (void)platform_close(connection->socket_fd);
    }
    connection->socket_fd = -1;
}

static int git_http_status_code(const unsigned char *headers, size_t header_size) {
    size_t index = 0U;
    int code = 0;
    int saw_digit = 0;

    while (index < header_size && headers[index] != ' ') {
        index += 1U;
    }
    while (index < header_size && headers[index] == ' ') {
        index += 1U;
    }
    while (index < header_size && headers[index] >= '0' && headers[index] <= '9') {
        saw_digit = 1;
        code = code * 10 + (int)(headers[index] - '0');
        index += 1U;
    }
    return saw_digit ? code : -1;
}

static int git_header_name_equals(const unsigned char *line, size_t name_length, const char *name) {
    size_t i = 0U;

    while (i < name_length && name[i] != '\0') {
        if (tool_ascii_tolower((char)line[i]) != tool_ascii_tolower(name[i])) {
            return 0;
        }
        i += 1U;
    }
    return i == name_length && name[i] == '\0';
}

static int git_header_value_contains(const unsigned char *value, size_t value_length, const char *needle) {
    size_t needle_length = rt_strlen(needle);
    size_t i;

    if (needle_length == 0U || value_length < needle_length) {
        return 0;
    }
    for (i = 0U; i + needle_length <= value_length; ++i) {
        size_t j;
        int match = 1;
        for (j = 0U; j < needle_length; ++j) {
            if (tool_ascii_tolower((char)value[i + j]) != tool_ascii_tolower(needle[j])) {
                match = 0;
                break;
            }
        }
        if (match) {
            return 1;
        }
    }
    return 0;
}

static int git_parse_headers(const unsigned char *headers, size_t header_size, int *chunked_out, size_t *content_length_out, int *has_content_length_out) {
    size_t line_start = 0U;
    int line_index = 0;

    *chunked_out = 0;
    *content_length_out = 0U;
    *has_content_length_out = 0;
    while (line_start < header_size) {
        size_t line_end = line_start;
        size_t length;

        while (line_end < header_size && headers[line_end] != '\n') {
            line_end += 1U;
        }
        length = line_end - line_start;
        if (length > 0U && headers[line_start + length - 1U] == '\r') {
            length -= 1U;
        }
        if (line_index > 0 && length > 0U) {
            size_t colon = 0U;
            while (colon < length && headers[line_start + colon] != ':') {
                colon += 1U;
            }
            if (colon < length) {
                size_t name_end = colon;
                const unsigned char *value = headers + line_start + colon + 1U;
                size_t value_length = length - colon - 1U;

                while (name_end > 0U && (headers[line_start + name_end - 1U] == ' ' || headers[line_start + name_end - 1U] == '\t')) {
                    name_end -= 1U;
                }
                while (value_length > 0U && (*value == ' ' || *value == '\t')) {
                    value += 1U;
                    value_length -= 1U;
                }
                if (git_header_name_equals(headers + line_start, name_end, "Transfer-Encoding")) {
                    if (git_header_value_contains(value, value_length, "chunked")) {
                        *chunked_out = 1;
                    }
                } else if (git_header_name_equals(headers + line_start, name_end, "Content-Length")) {
                    size_t i;
                    size_t parsed = 0U;
                    int saw_digit = 0;
                    for (i = 0U; i < value_length; ++i) {
                        if (value[i] >= '0' && value[i] <= '9') {
                            parsed = parsed * 10U + (size_t)(value[i] - '0');
                            saw_digit = 1;
                        } else if (value[i] != ' ' && value[i] != '\t') {
                            return -1;
                        }
                    }
                    if (saw_digit) {
                        *content_length_out = parsed;
                        *has_content_length_out = 1;
                    }
                }
            }
        }
        if (line_end >= header_size) {
            break;
        }
        line_start = line_end + 1U;
        line_index += 1;
    }
    return 0;
}

static int git_parse_chunk_size(const unsigned char *data, size_t size, size_t *line_end_out, size_t *chunk_size_out) {
    size_t index = 0U;
    size_t value = 0U;
    int saw_digit = 0;

    while (index < size && data[index] != '\n') {
        unsigned char ch = data[index];
        int digit = git_hex_value((char)ch);

        if (ch == '\r' || ch == ';') {
            while (index < size && data[index] != '\n') {
                index += 1U;
            }
            break;
        }
        if (digit < 0) {
            return -1;
        }
        value = value * 16U + (size_t)digit;
        saw_digit = 1;
        index += 1U;
    }
    if (index >= size || data[index] != '\n' || !saw_digit) {
        return -1;
    }
    *line_end_out = index + 1U;
    *chunk_size_out = value;
    return 0;
}

static int git_decode_chunked_body(const unsigned char *data, size_t size, GitBuffer *out) {
    size_t pos = 0U;

    while (pos < size) {
        size_t line_end;
        size_t chunk_size;

        if (git_parse_chunk_size(data + pos, size - pos, &line_end, &chunk_size) != 0) {
            return -1;
        }
        pos += line_end;
        if (chunk_size == 0U) {
            return 0;
        }
        if (pos + chunk_size + 2U > size) {
            return -1;
        }
        if (git_buffer_append(out, data + pos, chunk_size) != 0) {
            return -1;
        }
        pos += chunk_size;
        if (data[pos] == '\r') {
            pos += 1U;
        }
        if (pos >= size || data[pos] != '\n') {
            return -1;
        }
        pos += 1U;
    }
    return -1;
}

static int git_http_request(const GitUrl *url, const char *method, const char *accept, const char *content_type, const unsigned char *body, size_t body_size, GitBuffer *response) {
    GitHttpConnection connection;
    GitBuffer raw;
    char request[4096];
    char length_text[32];
    size_t request_length = 0U;
    char read_buffer[8192];
    long bytes_read;
    size_t header_offset = 0U;
    int status_code;
    int chunked = 0;
    int has_content_length = 0;
    size_t content_length = 0U;

    rt_memset(&raw, 0, sizeof(raw));
    rt_memset(response, 0, sizeof(*response));
    if (git_http_connect(url, &connection) != 0) {
        return -1;
    }
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, method);
    request_length = tool_buffer_append_char(request, sizeof(request), request_length, ' ');
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, url->path[0] != '\0' ? url->path : "/");
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, " HTTP/1.1\r\nHost: ");
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, url->host);
    if (url->port != git_default_port_for_scheme(url->scheme)) {
        rt_unsigned_to_string(url->port, length_text, sizeof(length_text));
        request_length = tool_buffer_append_char(request, sizeof(request), request_length, ':');
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, length_text);
    }
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\nUser-Agent: newos-git/0.1\r\nAccept: ");
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, accept != 0 ? accept : "*/*");
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\nConnection: close\r\n");
    if (content_type != 0) {
        rt_unsigned_to_string(body_size, length_text, sizeof(length_text));
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "Content-Type: ");
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, content_type);
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\nContent-Length: ");
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, length_text);
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\n");
    }
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\n");
    if (request_length >= sizeof(request) || git_http_write_all(&connection, request, request_length) != 0 || (body_size > 0U && git_http_write_all(&connection, body, body_size) != 0)) {
        git_http_close(&connection);
        git_buffer_destroy(&raw);
        return -1;
    }
    while ((bytes_read = git_http_read(&connection, read_buffer, sizeof(read_buffer))) > 0) {
        if (git_buffer_append(&raw, read_buffer, (size_t)bytes_read) != 0) {
            git_http_close(&connection);
            git_buffer_destroy(&raw);
            return -1;
        }
    }
    git_http_close(&connection);
    if (bytes_read < 0 || tool_find_http_header_end((const char *)raw.data, raw.size, &header_offset) != 0) {
        git_buffer_destroy(&raw);
        return -1;
    }
    status_code = git_http_status_code(raw.data, header_offset);
    if (status_code < 200 || status_code >= 300 || git_parse_headers(raw.data, header_offset, &chunked, &content_length, &has_content_length) != 0) {
        git_buffer_destroy(&raw);
        return -1;
    }
    if (chunked) {
        int result = git_decode_chunked_body(raw.data + header_offset, raw.size - header_offset, response);
        git_buffer_destroy(&raw);
        return result;
    }
    if (has_content_length && raw.size - header_offset > content_length) {
        raw.size = header_offset + content_length;
    }
    if (git_buffer_append(response, raw.data + header_offset, raw.size - header_offset) != 0) {
        git_buffer_destroy(&raw);
        return -1;
    }
    git_buffer_destroy(&raw);
    return 0;
}

static int git_append_pkt_line(GitBuffer *buffer, const char *payload) {
    static const char digits[] = "0123456789abcdef";
    size_t payload_length = rt_strlen(payload);
    size_t packet_length = payload_length + 4U;
    char header[4];

    if (packet_length > 0xffffU) {
        return -1;
    }
    header[0] = digits[(packet_length >> 12) & 15U];
    header[1] = digits[(packet_length >> 8) & 15U];
    header[2] = digits[(packet_length >> 4) & 15U];
    header[3] = digits[packet_length & 15U];
    return git_buffer_append(buffer, header, 4U) != 0 || git_buffer_append(buffer, payload, payload_length) != 0 ? -1 : 0;
}

static int git_pkt_length(const unsigned char *data, size_t size, size_t *length_out) {
    int a;
    int b;
    int c;
    int d;

    if (size < 4U) {
        return -1;
    }
    a = git_hex_value((char)data[0]);
    b = git_hex_value((char)data[1]);
    c = git_hex_value((char)data[2]);
    d = git_hex_value((char)data[3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) {
        return -1;
    }
    *length_out = (size_t)((a << 12) | (b << 8) | (c << 4) | d);
    return 0;
}

static int git_parent_path(char *path) {
    size_t len = rt_strlen(path);

    while (len > 1U && path[len - 1U] == '/') {
        path[len - 1U] = '\0';
        len -= 1U;
    }
    while (len > 0U && path[len - 1U] != '/') {
        len -= 1U;
    }
    if (len == 0U) {
        return -1;
    }
    if (len == 1U) {
        path[1] = '\0';
        return 0;
    }
    path[len - 1U] = '\0';
    return 0;
}

static int git_is_absolute_path(const char *path) {
    return path != 0 && path[0] == '/';
}

static int git_resolve_gitfile(const char *work_tree, const char *gitfile_path, char *git_dir, size_t git_dir_size) {
    char text[GIT_PATH_CAPACITY];
    const char *target;

    if (git_read_text_file(gitfile_path, text, sizeof(text)) != 0 || rt_strncmp(text, "gitdir:", 7U) != 0) {
        return -1;
    }
    target = text + 7U;
    while (*target == ' ' || *target == '\t') {
        target += 1;
    }
    if (git_is_absolute_path(target)) {
        return git_copy(git_dir, git_dir_size, target);
    }
    return git_join(git_dir, git_dir_size, work_tree, target);
}

static int git_discover_from(const char *start_path, GitRepo *repo) {
    char current[GIT_PATH_CAPACITY];

    rt_memset(repo, 0, sizeof(*repo));
    if (git_copy(current, sizeof(current), start_path) != 0) {
        return -1;
    }

    for (;;) {
        char dotgit[GIT_PATH_CAPACITY];
        PlatformDirEntry entry;

        if (git_join(dotgit, sizeof(dotgit), current, ".git") != 0) {
            return -1;
        }
        if (platform_get_path_info(dotgit, &entry) == 0) {
            if (entry.is_dir) {
                if (git_copy(repo->work_tree, sizeof(repo->work_tree), current) != 0 ||
                    git_copy(repo->git_dir, sizeof(repo->git_dir), dotgit) != 0) {
                    return -1;
                }
                return 0;
            }
            if (git_resolve_gitfile(current, dotgit, repo->git_dir, sizeof(repo->git_dir)) == 0 &&
                git_copy(repo->work_tree, sizeof(repo->work_tree), current) == 0) {
                return 0;
            }
        }
        if (tool_path_is_root(current) || git_parent_path(current) != 0) {
            break;
        }
    }

    return -1;
}

static int git_discover(GitRepo *repo) {
    char current[GIT_PATH_CAPACITY];

    if (platform_get_current_directory(current, sizeof(current)) != 0) {
        return -1;
    }
    return git_discover_from(current, repo);
}

static void git_format_oid_hex(const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], char hex[GIT_OBJECT_HEX_SIZE + 1U]) {
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0U; i < CRYPTO_SHA1_DIGEST_SIZE; ++i) {
        hex[i * 2U] = digits[(oid[i] >> 4) & 15U];
        hex[i * 2U + 1U] = digits[oid[i] & 15U];
    }
    hex[GIT_OBJECT_HEX_SIZE] = '\0';
}

static int git_ignore_push(GitIgnoreList *ignores, GitIgnorePattern *pattern) {
    GitIgnorePattern *new_patterns;
    size_t new_capacity;

    if (ignores->count == ignores->capacity) {
        new_capacity = ignores->capacity == 0U ? 16U : ignores->capacity * 2U;
        new_patterns = (GitIgnorePattern *)rt_realloc_array(ignores->patterns, new_capacity, sizeof(ignores->patterns[0]));
        if (new_patterns == 0) {
            return -1;
        }
        ignores->patterns = new_patterns;
        ignores->capacity = new_capacity;
    }
    ignores->patterns[ignores->count++] = *pattern;
    return 0;
}

static int git_ignore_add_line(GitIgnoreList *ignores, const char *line, size_t line_length) {
    GitIgnorePattern pattern;
    char *copy = git_strdup_n(line, line_length);
    size_t length;

    if (copy == 0) {
        return -1;
    }
    tool_trim_whitespace(copy);
    if (copy[0] == '\0' || copy[0] == '#') {
        rt_free(copy);
        return 0;
    }

    rt_memset(&pattern, 0, sizeof(pattern));
    if (copy[0] == '!') {
        pattern.negated = 1;
        memmove(copy, copy + 1, rt_strlen(copy));
        tool_trim_whitespace(copy);
    }
    length = rt_strlen(copy);
    if (length == 0U) {
        rt_free(copy);
        return 0;
    }
    if (copy[length - 1U] == '/') {
        pattern.directory_only = 1;
        copy[length - 1U] = '\0';
    }
    if (copy[0] == '\0') {
        rt_free(copy);
        return 0;
    }
    pattern.has_slash = copy[0] == '/' || git_path_has_slash(copy);
    pattern.has_wildcard = git_path_has_wildcard(copy);
    pattern.pattern = copy;
    if (git_ignore_push(ignores, &pattern) != 0) {
        rt_free(copy);
        return -1;
    }
    return 0;
}

static int git_ignore_load_file(GitIgnoreList *ignores, const char *path) {
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;

    if (git_read_file(path, &data, &size) != 0) {
        return 0;
    }
    while (pos < size) {
        size_t start = pos;
        size_t end;

        while (pos < size && data[pos] != '\n') {
            pos += 1U;
        }
        end = pos;
        if (pos < size) {
            pos += 1U;
        }
        if (end > start && data[end - 1U] == '\r') {
            end -= 1U;
        }
        if (git_ignore_add_line(ignores, (const char *)data + start, end - start) != 0) {
            rt_free(data);
            return -1;
        }
    }
    rt_free(data);
    return 0;
}

static int git_ignore_load(const GitRepo *repo, GitIgnoreList *ignores) {
    char path[GIT_PATH_CAPACITY];

    rt_memset(ignores, 0, sizeof(*ignores));
    if (git_join(path, sizeof(path), repo->work_tree, ".gitignore") == 0 && git_ignore_load_file(ignores, path) != 0) {
        return -1;
    }
    if (git_join(path, sizeof(path), repo->git_dir, "info/exclude") == 0 && git_ignore_load_file(ignores, path) != 0) {
        git_ignore_destroy(ignores);
        return -1;
    }
    return 0;
}

static int git_path_has_slash(const char *path) {
    size_t i;

    for (i = 0U; path[i] != '\0'; ++i) {
        if (path[i] == '/') {
            return 1;
        }
    }
    return 0;
}

static int git_path_has_wildcard(const char *path) {
    size_t i;

    for (i = 0U; path[i] != '\0'; ++i) {
        if (path[i] == '*' || path[i] == '?' || path[i] == '[') {
            return 1;
        }
    }
    return 0;
}

static const char *git_path_basename(const char *path) {
    const char *base = path;
    size_t i;

    for (i = 0U; path[i] != '\0'; ++i) {
        if (path[i] == '/') {
            base = path + i + 1U;
        }
    }
    return base;
}

static int git_ignore_pattern_matches(const GitIgnorePattern *pattern, const char *relative, int is_directory) {
    const char *match_pattern = pattern->pattern;

    if (pattern->directory_only && !is_directory) {
        return 0;
    }
    if (match_pattern[0] == '/') {
        match_pattern += 1;
        return pattern->has_wildcard ? tool_wildcard_match(match_pattern, relative) : rt_strcmp(match_pattern, relative) == 0;
    }
    if (pattern->has_slash) {
        return pattern->has_wildcard ? tool_wildcard_match(match_pattern, relative) : rt_strcmp(match_pattern, relative) == 0;
    }
    return pattern->has_wildcard ? tool_wildcard_match(match_pattern, git_path_basename(relative)) : rt_strcmp(match_pattern, git_path_basename(relative)) == 0;
}

static int git_ignore_matches(const GitIgnoreList *ignores, const char *relative, int is_directory) {
    int ignored = 0;
    size_t i;

    if (ignores == 0) {
        return 0;
    }
    for (i = 0U; i < ignores->count; ++i) {
        if (git_ignore_pattern_matches(&ignores->patterns[i], relative, is_directory)) {
            ignored = ignores->patterns[i].negated ? 0 : 1;
        }
    }
    return ignored;
}

static int git_read_ref_file(const GitRepo *repo, const char *ref_name, char *oid_hex, size_t oid_hex_size) {
    char path[GIT_PATH_CAPACITY];
    const char *relative = ref_name;

    if (rt_strncmp(relative, "refs/", 5U) != 0) {
        return -1;
    }
    if (git_join(path, sizeof(path), repo->git_dir, relative) != 0 ||
        git_read_text_file(path, oid_hex, oid_hex_size) != 0) {
        return -1;
    }
    return rt_strlen(oid_hex) >= GIT_OBJECT_HEX_SIZE ? 0 : -1;
}

static int git_read_packed_ref(const GitRepo *repo, const char *ref_name, char *oid_hex, size_t oid_hex_size) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;
    size_t ref_len = rt_strlen(ref_name);

    if (git_join(path, sizeof(path), repo->git_dir, "packed-refs") != 0 || git_read_file(path, &data, &size) != 0) {
        return -1;
    }

    while (pos < size) {
        size_t start = pos;
        size_t end;
        size_t line_len;

        while (pos < size && data[pos] != '\n') {
            pos += 1U;
        }
        end = pos;
        if (pos < size) {
            pos += 1U;
        }
        while (end > start && data[end - 1U] == '\r') {
            end -= 1U;
        }
        line_len = end - start;
        if (line_len >= GIT_OBJECT_HEX_SIZE + 1U + ref_len && data[start] != '#' && data[start] != '^' &&
            data[start + GIT_OBJECT_HEX_SIZE] == ' ' &&
            memcmp(data + start + GIT_OBJECT_HEX_SIZE + 1U, ref_name, ref_len) == 0 &&
            GIT_OBJECT_HEX_SIZE + 1U + ref_len == line_len) {
            if (oid_hex_size <= GIT_OBJECT_HEX_SIZE) {
                rt_free(data);
                return -1;
            }
            memcpy(oid_hex, data + start, GIT_OBJECT_HEX_SIZE);
            oid_hex[GIT_OBJECT_HEX_SIZE] = '\0';
            rt_free(data);
            return 0;
        }
    }

    rt_free(data);
    return -1;
}

static int git_resolve_ref(const GitRepo *repo, const char *ref_name, char *oid_hex, size_t oid_hex_size) {
    if (git_read_ref_file(repo, ref_name, oid_hex, oid_hex_size) == 0) {
        return 0;
    }
    return git_read_packed_ref(repo, ref_name, oid_hex, oid_hex_size);
}

static int git_load_head(GitRepo *repo) {
    char path[GIT_PATH_CAPACITY];
    char text[GIT_REF_CAPACITY];

    if (git_join(path, sizeof(path), repo->git_dir, "HEAD") != 0 || git_read_text_file(path, text, sizeof(text)) != 0) {
        return -1;
    }
    if (rt_strncmp(text, "ref:", 4U) == 0) {
        const char *ref = text + 4U;

        while (*ref == ' ' || *ref == '\t') {
            ref += 1;
        }
        if (git_copy(repo->head_ref, sizeof(repo->head_ref), ref) != 0) {
            return -1;
        }
        repo->head_is_branch = rt_strncmp(repo->head_ref, "refs/heads/", 11U) == 0;
        if (git_resolve_ref(repo, repo->head_ref, repo->head_oid, sizeof(repo->head_oid)) != 0) {
            repo->head_oid[0] = '\0';
        }
        return 0;
    }
    if (rt_strlen(text) >= GIT_OBJECT_HEX_SIZE) {
        repo->head_is_branch = 0;
        repo->head_ref[0] = '\0';
        return git_copy(repo->head_oid, sizeof(repo->head_oid), text);
    }
    return -1;
}

static const char *git_branch_name(const GitRepo *repo) {
    if (!repo->head_is_branch) {
        return 0;
    }
    return repo->head_ref + 11U;
}

static unsigned short git_read_u16_be_raw(const unsigned char *bytes) {
    return (unsigned short)(((unsigned int)bytes[0] << 8) | (unsigned int)bytes[1]);
}

static unsigned int git_read_u32_be_raw(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 24) | ((unsigned int)bytes[1] << 16) | ((unsigned int)bytes[2] << 8) | (unsigned int)bytes[3];
}

static int git_index_push(GitIndex *index, GitIndexEntry *entry) {
    GitIndexEntry *new_entries;
    size_t new_capacity;

    if (index->count == index->capacity) {
        new_capacity = index->capacity == 0U ? 32U : index->capacity * 2U;
        new_entries = (GitIndexEntry *)rt_realloc_array(index->entries, new_capacity, sizeof(index->entries[0]));
        if (new_entries == 0) {
            return -1;
        }
        index->entries = new_entries;
        index->capacity = new_capacity;
    }
    index->entries[index->count++] = *entry;
    return 0;
}

static int git_index_mode_is_regular(unsigned int mode) {
    return (mode & GIT_MODE_TYPE_MASK) == GIT_MODE_REGULAR_TYPE;
}

static unsigned int git_regular_index_mode_from_worktree(unsigned int mode) {
    return (mode & GIT_MODE_EXEC_BITS) != 0U ? GIT_MODE_REGULAR_EXECUTABLE : GIT_MODE_REGULAR_FILE;
}

static unsigned int git_worktree_mode_from_regular_index(unsigned int mode) {
    return (mode & GIT_MODE_EXEC_BITS) != 0U ? 0755U : 0644U;
}

static int git_make_directory_chain(const char *path);
static int git_ensure_parent_directory(const char *path);
static int git_path_parent(char *path);
static int git_compare_entries_by_path(const void *left, const void *right);

static const char *git_object_type_name(int type) {
    if (type == GIT_OBJECT_COMMIT) {
        return "commit";
    }
    if (type == GIT_OBJECT_TREE) {
        return "tree";
    }
    if (type == GIT_OBJECT_BLOB) {
        return "blob";
    }
    if (type == GIT_OBJECT_TAG) {
        return "tag";
    }
    return "unknown";
}

static int git_object_type_from_name(const char *name, size_t length) {
    if (length == 6U && memcmp(name, "commit", 6U) == 0) {
        return GIT_OBJECT_COMMIT;
    }
    if (length == 4U && memcmp(name, "tree", 4U) == 0) {
        return GIT_OBJECT_TREE;
    }
    if (length == 4U && memcmp(name, "blob", 4U) == 0) {
        return GIT_OBJECT_BLOB;
    }
    if (length == 3U && memcmp(name, "tag", 3U) == 0) {
        return GIT_OBJECT_TAG;
    }
    return 0;
}

static int git_hash_object_data(int type, const unsigned char *data, size_t size, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], GitBuffer *full_out) {
    CryptoSha1Context sha1;
    char size_digits[32];

    if (full_out != 0) {
        rt_memset(full_out, 0, sizeof(*full_out));
    }
    rt_unsigned_to_string(size, size_digits, sizeof(size_digits));
    crypto_sha1_init(&sha1);
    crypto_sha1_update(&sha1, (const unsigned char *)git_object_type_name(type), rt_strlen(git_object_type_name(type)));
    crypto_sha1_update(&sha1, (const unsigned char *)" ", 1U);
    crypto_sha1_update(&sha1, (const unsigned char *)size_digits, rt_strlen(size_digits));
    crypto_sha1_update(&sha1, (const unsigned char *)"\0", 1U);
    crypto_sha1_update(&sha1, data, size);
    crypto_sha1_final(&sha1, oid);
    if (full_out != 0) {
        if (git_buffer_append_cstr(full_out, git_object_type_name(type)) != 0 ||
            git_buffer_append_char(full_out, ' ') != 0 ||
            git_buffer_append_cstr(full_out, size_digits) != 0 ||
            git_buffer_append_char(full_out, '\0') != 0 ||
            git_buffer_append(full_out, data, size) != 0) {
            git_buffer_destroy(full_out);
            return -1;
        }
    }
    return 0;
}

static int git_object_path(const GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], char *path, size_t path_size, int want_dir) {
    char hex[GIT_OBJECT_HEX_SIZE + 1U];
    char dir[GIT_PATH_CAPACITY];
    char tail[GIT_OBJECT_HEX_SIZE];

    git_format_oid_hex(oid, hex);
    tail[0] = hex[2];
    memcpy(tail, hex + 2U, GIT_OBJECT_HEX_SIZE - 2U);
    tail[GIT_OBJECT_HEX_SIZE - 2U] = '\0';
    if (git_join(dir, sizeof(dir), repo->git_dir, "objects") != 0) {
        return -1;
    }
    if (git_join(dir, sizeof(dir), dir, "xx") != 0) {
        return -1;
    }
    dir[rt_strlen(dir) - 2U] = hex[0];
    dir[rt_strlen(dir) - 1U] = hex[1];
    if (want_dir) {
        return git_copy(path, path_size, dir);
    }
    return git_join(path, path_size, dir, tail);
}

static int git_write_loose_object(const GitRepo *repo, int type, const unsigned char *data, size_t size, const unsigned char expected_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitBuffer full;
    GitBuffer compressed;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    char object_dir[GIT_PATH_CAPACITY];
    char object_path[GIT_PATH_CAPACITY];
    size_t compressed_size = 0U;
    size_t bound;

    rt_memset(&full, 0, sizeof(full));
    rt_memset(&compressed, 0, sizeof(compressed));
    if (git_hash_object_data(type, data, size, oid, &full) != 0) {
        return -1;
    }
    if (expected_oid != 0 && !git_oid_equal(oid, expected_oid)) {
        git_buffer_destroy(&full);
        return -1;
    }
    if (git_object_path(repo, oid, object_dir, sizeof(object_dir), 1) != 0 || git_object_path(repo, oid, object_path, sizeof(object_path), 0) != 0) {
        git_buffer_destroy(&full);
        return -1;
    }
    {
        PlatformDirEntry existing;
        if (platform_get_path_info(object_path, &existing) == 0 && !existing.is_dir) {
            git_buffer_destroy(&full);
            return 0;
        }
    }
    if (git_make_directory_chain(object_dir) != 0) {
        git_buffer_destroy(&full);
        return -1;
    }
    bound = compression_zlib_deflate_bound(full.size);
    if (bound == 0U || git_buffer_reserve(&compressed, bound) != 0) {
        git_buffer_destroy(&full);
        return -1;
    }
    if (compression_zlib_deflate_level(full.data, full.size, compressed.data, compressed.capacity, &compressed_size, 6) != 0) {
        git_buffer_destroy(&full);
        git_buffer_destroy(&compressed);
        return -1;
    }
    compressed.size = compressed_size;
    if (git_write_all_file(object_path, compressed.data, compressed.size, 0444U) != 0) {
        git_buffer_destroy(&full);
        git_buffer_destroy(&compressed);
        return -1;
    }
    git_buffer_destroy(&full);
    git_buffer_destroy(&compressed);
    return 0;
}

static int git_read_loose_object(const GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], int *type_out, unsigned char **data_out, size_t *size_out) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *compressed = 0;
    size_t compressed_size = 0U;
    unsigned char *full = 0;
    size_t full_capacity;
    size_t full_size = 0U;
    size_t pos = 0U;
    size_t type_start;
    size_t type_length;
    size_t parsed_size = 0U;

    if (git_object_path(repo, oid, path, sizeof(path), 0) != 0 || git_read_file(path, &compressed, &compressed_size) != 0) {
        return -1;
    }
    full_capacity = compressed_size * 4U + 1024U;
    if (full_capacity < 4096U) {
        full_capacity = 4096U;
    }
    while (full_capacity <= GIT_MAX_OBJECT_SIZE) {
        full = (unsigned char *)rt_malloc(full_capacity);
        if (full == 0) {
            rt_free(compressed);
            return -1;
        }
        if (compression_zlib_inflate(compressed, compressed_size, full, full_capacity, &full_size) == 0) {
            break;
        }
        rt_free(full);
        full = 0;
        full_capacity *= 2U;
    }
    rt_free(compressed);
    if (full == 0) {
        return -1;
    }
    type_start = 0U;
    while (pos < full_size && full[pos] != ' ') {
        pos += 1U;
    }
    if (pos >= full_size) {
        rt_free(full);
        return -1;
    }
    type_length = pos - type_start;
    *type_out = git_object_type_from_name((const char *)full + type_start, type_length);
    if (*type_out == 0) {
        rt_free(full);
        return -1;
    }
    pos += 1U;
    while (pos < full_size && full[pos] >= '0' && full[pos] <= '9') {
        parsed_size = parsed_size * 10U + (size_t)(full[pos] - '0');
        pos += 1U;
    }
    if (pos >= full_size || full[pos] != '\0' || parsed_size != full_size - pos - 1U) {
        rt_free(full);
        return -1;
    }
    pos += 1U;
    *data_out = (unsigned char *)rt_malloc(parsed_size == 0U ? 1U : parsed_size);
    if (*data_out == 0) {
        rt_free(full);
        return -1;
    }
    memcpy(*data_out, full + pos, parsed_size);
    *size_out = parsed_size;
    rt_free(full);
    return 0;
}

static int git_pack_push(GitPack *pack, GitPackObject *object) {
    GitPackObject *new_objects;
    size_t new_capacity;

    if (pack->count == pack->capacity) {
        new_capacity = pack->capacity == 0U ? 64U : pack->capacity * 2U;
        new_objects = (GitPackObject *)rt_realloc_array(pack->objects, new_capacity, sizeof(pack->objects[0]));
        if (new_objects == 0) {
            return -1;
        }
        pack->objects = new_objects;
        pack->capacity = new_capacity;
    }
    pack->objects[pack->count++] = *object;
    return 0;
}

static GitPackObject *git_pack_find_offset(GitPack *pack, unsigned long long offset) {
    size_t i;

    for (i = 0U; i < pack->count; ++i) {
        if (pack->objects[i].offset == offset) {
            return &pack->objects[i];
        }
    }
    return 0;
}

static GitPackObject *git_pack_find_oid(GitPack *pack, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    size_t i;

    for (i = 0U; i < pack->count; ++i) {
        if (pack->objects[i].resolved && git_oid_equal(pack->objects[i].oid, oid)) {
            return &pack->objects[i];
        }
    }
    return 0;
}

static int git_read_varint_delta(const unsigned char *data, size_t size, size_t *pos_io, size_t *value_out) {
    size_t shift = 0U;
    size_t value = 0U;

    while (*pos_io < size) {
        unsigned char ch = data[(*pos_io)++];
        value |= ((size_t)(ch & 0x7fU)) << shift;
        if ((ch & 0x80U) == 0U) {
            *value_out = value;
            return 0;
        }
        shift += 7U;
        if (shift >= sizeof(size_t) * 8U) {
            return -1;
        }
    }
    return -1;
}

static int git_apply_delta(const unsigned char *base, size_t base_size, const unsigned char *delta, size_t delta_size, unsigned char **out_data, size_t *out_size) {
    size_t pos = 0U;
    size_t declared_base = 0U;
    size_t result_size = 0U;
    size_t out_pos = 0U;
    unsigned char *out;

    if (git_read_varint_delta(delta, delta_size, &pos, &declared_base) != 0 || git_read_varint_delta(delta, delta_size, &pos, &result_size) != 0 || declared_base != base_size) {
        return -1;
    }
    out = (unsigned char *)rt_malloc(result_size == 0U ? 1U : result_size);
    if (out == 0) {
        return -1;
    }
    while (pos < delta_size) {
        unsigned char op = delta[pos++];

        if ((op & 0x80U) != 0U) {
            size_t copy_offset = 0U;
            size_t copy_size = 0U;
            if ((op & 0x01U) != 0U && pos < delta_size) copy_offset |= (size_t)delta[pos++];
            if ((op & 0x02U) != 0U && pos < delta_size) copy_offset |= (size_t)delta[pos++] << 8U;
            if ((op & 0x04U) != 0U && pos < delta_size) copy_offset |= (size_t)delta[pos++] << 16U;
            if ((op & 0x08U) != 0U && pos < delta_size) copy_offset |= (size_t)delta[pos++] << 24U;
            if ((op & 0x10U) != 0U && pos < delta_size) copy_size |= (size_t)delta[pos++];
            if ((op & 0x20U) != 0U && pos < delta_size) copy_size |= (size_t)delta[pos++] << 8U;
            if ((op & 0x40U) != 0U && pos < delta_size) copy_size |= (size_t)delta[pos++] << 16U;
            if (copy_size == 0U) copy_size = 0x10000U;
            if (copy_offset > base_size || copy_size > base_size - copy_offset || out_pos > result_size || copy_size > result_size - out_pos) {
                rt_free(out);
                return -1;
            }
            memcpy(out + out_pos, base + copy_offset, copy_size);
            out_pos += copy_size;
        } else if (op != 0U) {
            size_t insert_size = (size_t)op;
            if (pos + insert_size > delta_size || out_pos + insert_size > result_size) {
                rt_free(out);
                return -1;
            }
            memcpy(out + out_pos, delta + pos, insert_size);
            pos += insert_size;
            out_pos += insert_size;
        } else {
            rt_free(out);
            return -1;
        }
    }
    if (out_pos != result_size) {
        rt_free(out);
        return -1;
    }
    *out_data = out;
    *out_size = result_size;
    return 0;
}

static int git_parse_pack(const unsigned char *data, size_t size, GitPack *pack) {
    size_t pos = 12U;
    unsigned int version;
    unsigned int count;
    unsigned int i;

    rt_memset(pack, 0, sizeof(*pack));
    if (size < 12U || memcmp(data, "PACK", 4U) != 0) {
        return -1;
    }
    version = git_read_u32_be_raw(data + 4U);
    count = git_read_u32_be_raw(data + 8U);
    if (version != 2U && version != 3U) {
        return -1;
    }
    for (i = 0U; i < count; ++i) {
        GitPackObject object;
        unsigned char byte;
        unsigned int shift = 4U;
        size_t object_size;
        size_t consumed = 0U;

        if (pos >= size) {
            git_pack_destroy(pack);
            return -1;
        }
        rt_memset(&object, 0, sizeof(object));
        object.offset = (unsigned long long)pos;
        byte = data[pos++];
        object.type = (byte >> 4) & 7U;
        object_size = (size_t)(byte & 0x0fU);
        while ((byte & 0x80U) != 0U) {
            if (pos >= size) {
                git_pack_destroy(pack);
                return -1;
            }
            byte = data[pos++];
            object_size |= ((size_t)(byte & 0x7fU)) << shift;
            shift += 7U;
        }
        object.size = object_size;
        if (object.type == GIT_OBJECT_OFS_DELTA) {
            unsigned long long offset_value;
            if (pos >= size) {
                git_pack_destroy(pack);
                return -1;
            }
            byte = data[pos++];
            offset_value = (unsigned long long)(byte & 0x7fU);
            while ((byte & 0x80U) != 0U) {
                if (pos >= size) {
                    git_pack_destroy(pack);
                    return -1;
                }
                byte = data[pos++];
                offset_value = ((offset_value + 1ULL) << 7) | (unsigned long long)(byte & 0x7fU);
            }
            object.base_offset = object.offset - offset_value;
        } else if (object.type == GIT_OBJECT_REF_DELTA) {
            if (pos + CRYPTO_SHA1_DIGEST_SIZE > size) {
                git_pack_destroy(pack);
                return -1;
            }
            memcpy(object.base_oid, data + pos, CRYPTO_SHA1_DIGEST_SIZE);
            pos += CRYPTO_SHA1_DIGEST_SIZE;
        }
        object.data = (unsigned char *)rt_malloc(object_size == 0U ? 1U : object_size);
        if (object.data == 0 || compression_zlib_inflate_consumed(data + pos, size - pos, object.data, object_size, &object.size, &consumed) != 0 || object.size != object_size) {
            rt_free(object.data);
            git_pack_destroy(pack);
            return -1;
        }
        pos += consumed;
        if (object.type >= GIT_OBJECT_COMMIT && object.type <= GIT_OBJECT_TAG) {
            if (git_hash_object_data(object.type, object.data, object.size, object.oid, 0) != 0) {
                rt_free(object.data);
                git_pack_destroy(pack);
                return -1;
            }
            object.resolved = 1;
        }
        if (git_pack_push(pack, &object) != 0) {
            rt_free(object.data);
            git_pack_destroy(pack);
            return -1;
        }
    }
    return 0;
}

static int git_resolve_pack_deltas(GitPack *pack) {
    int progress = 1;

    while (progress) {
        size_t i;
        progress = 0;
        for (i = 0U; i < pack->count; ++i) {
            GitPackObject *object = &pack->objects[i];
            GitPackObject *base;
            unsigned char *resolved_data = 0;
            size_t resolved_size = 0U;

            if (object->resolved || (object->type != GIT_OBJECT_OFS_DELTA && object->type != GIT_OBJECT_REF_DELTA)) {
                continue;
            }
            base = object->type == GIT_OBJECT_OFS_DELTA ? git_pack_find_offset(pack, object->base_offset) : git_pack_find_oid(pack, object->base_oid);
            if (base == 0 || !base->resolved) {
                continue;
            }
            if (git_apply_delta(base->data, base->size, object->data, object->size, &resolved_data, &resolved_size) != 0) {
                return -1;
            }
            rt_free(object->data);
            object->data = resolved_data;
            object->size = resolved_size;
            object->type = base->type;
            if (git_hash_object_data(object->type, object->data, object->size, object->oid, 0) != 0) {
                return -1;
            }
            object->resolved = 1;
            progress = 1;
        }
    }
    {
        size_t i;
        for (i = 0U; i < pack->count; ++i) {
            if (!pack->objects[i].resolved) {
                return -1;
            }
        }
    }
    return 0;
}

static int git_write_pack_objects(const GitRepo *repo, GitPack *pack) {
    size_t i;

    for (i = 0U; i < pack->count; ++i) {
        if (git_write_loose_object(repo, pack->objects[i].type, pack->objects[i].data, pack->objects[i].size, pack->objects[i].oid) != 0) {
            return -1;
        }
    }
    return 0;
}

static int git_commit_tree_oid(const GitRepo *repo, const unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE], unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    int type = 0;
    unsigned char *data = 0;
    size_t size = 0U;
    int result = -1;

    if (git_read_loose_object(repo, commit_oid, &type, &data, &size) != 0 || type != GIT_OBJECT_COMMIT) {
        rt_free(data);
        return -1;
    }
    if (size >= 46U && memcmp(data, "tree ", 5U) == 0 && git_parse_oid_hex_n((const char *)data + 5U, GIT_OBJECT_HEX_SIZE, tree_oid) == 0) {
        result = 0;
    }
    rt_free(data);
    return result;
}

static int git_index_append_checkout_entry(GitCheckoutIndex *checkout, const char *relative, unsigned int mode, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], size_t size) {
    GitIndexEntry entry;

    rt_memset(&entry, 0, sizeof(entry));
    entry.path = git_strdup_n(relative, rt_strlen(relative));
    if (entry.path == 0) {
        return -1;
    }
    entry.mode = mode;
    entry.size = size;
    memcpy(entry.oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
    if (git_index_push(&checkout->entries, &entry) != 0) {
        rt_free(entry.path);
        return -1;
    }
    return 0;
}

static int git_checkout_tree_recursive(const GitRepo *repo, const unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE], const char *prefix, GitCheckoutIndex *checkout) {
    int type = 0;
    unsigned char *tree = 0;
    size_t tree_size = 0U;
    size_t pos = 0U;

    if (git_read_loose_object(repo, tree_oid, &type, &tree, &tree_size) != 0 || type != GIT_OBJECT_TREE) {
        rt_free(tree);
        return -1;
    }
    while (pos < tree_size) {
        unsigned int mode = 0U;
        size_t name_start;
        size_t name_length;
        char relative[GIT_PATH_CAPACITY];
        char full_path[GIT_PATH_CAPACITY];
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];

        while (pos < tree_size && tree[pos] >= '0' && tree[pos] <= '7') {
            mode = mode * 8U + (unsigned int)(tree[pos] - '0');
            pos += 1U;
        }
        if (pos >= tree_size || tree[pos] != ' ') {
            rt_free(tree);
            return -1;
        }
        pos += 1U;
        name_start = pos;
        while (pos < tree_size && tree[pos] != '\0') {
            pos += 1U;
        }
        if (pos >= tree_size || pos + 1U + CRYPTO_SHA1_DIGEST_SIZE > tree_size) {
            rt_free(tree);
            return -1;
        }
        name_length = pos - name_start;
        pos += 1U;
        memcpy(oid, tree + pos, CRYPTO_SHA1_DIGEST_SIZE);
        pos += CRYPTO_SHA1_DIGEST_SIZE;
        if (prefix[0] != '\0') {
            if (rt_strlen(prefix) + 1U + name_length >= sizeof(relative)) {
                rt_free(tree);
                return -1;
            }
            rt_copy_string(relative, sizeof(relative), prefix);
            relative[rt_strlen(relative) + 1U] = '\0';
            relative[rt_strlen(relative)] = '/';
            memcpy(relative + rt_strlen(prefix) + 1U, tree + name_start, name_length);
            relative[rt_strlen(prefix) + 1U + name_length] = '\0';
        } else {
            if (name_length >= sizeof(relative)) {
                rt_free(tree);
                return -1;
            }
            memcpy(relative, tree + name_start, name_length);
            relative[name_length] = '\0';
        }
        if (tool_path_is_unsafe_relative(relative) || git_join(full_path, sizeof(full_path), repo->work_tree, relative) != 0) {
            rt_free(tree);
            return -1;
        }
        if (mode == GIT_MODE_TREE || mode == 040000U) {
            if (git_make_directory_chain(full_path) != 0 || git_checkout_tree_recursive(repo, oid, relative, checkout) != 0) {
                rt_free(tree);
                return -1;
            }
        } else if ((mode & GIT_MODE_TYPE_MASK) == GIT_MODE_REGULAR_TYPE || mode == 0100644U || mode == 0100755U) {
            int blob_type = 0;
            unsigned char *blob = 0;
            size_t blob_size = 0U;
            unsigned int index_mode = (mode & GIT_MODE_EXEC_BITS) != 0U ? GIT_MODE_REGULAR_EXECUTABLE : GIT_MODE_REGULAR_FILE;

            if (git_read_loose_object(repo, oid, &blob_type, &blob, &blob_size) != 0 || blob_type != GIT_OBJECT_BLOB) {
                rt_free(blob);
                rt_free(tree);
                return -1;
            }
            if (git_ensure_parent_directory(full_path) != 0 || git_write_all_file(full_path, blob, blob_size, git_worktree_mode_from_regular_index(index_mode)) != 0) {
                rt_free(blob);
                rt_free(tree);
                return -1;
            }
            (void)platform_change_mode(full_path, git_worktree_mode_from_regular_index(index_mode));
            if (git_index_append_checkout_entry(checkout, relative, index_mode, oid, blob_size) != 0) {
                rt_free(blob);
                rt_free(tree);
                return -1;
            }
            rt_free(blob);
        } else if (mode == GIT_MODE_SYMLINK || mode == 0120000U) {
            int blob_type = 0;
            unsigned char *target = 0;
            size_t target_size = 0U;
            char link_target[GIT_PATH_CAPACITY];

            if (git_read_loose_object(repo, oid, &blob_type, &target, &target_size) != 0 || blob_type != GIT_OBJECT_BLOB || target_size >= sizeof(link_target)) {
                rt_free(target);
                rt_free(tree);
                return -1;
            }
            memcpy(link_target, target, target_size);
            link_target[target_size] = '\0';
            (void)platform_remove_file(full_path);
            if (git_ensure_parent_directory(full_path) != 0 || platform_create_symbolic_link(link_target, full_path) != 0) {
                rt_free(target);
                rt_free(tree);
                return -1;
            }
            if (git_index_append_checkout_entry(checkout, relative, GIT_MODE_SYMLINK, oid, target_size) != 0) {
                rt_free(target);
                rt_free(tree);
                return -1;
            }
            rt_free(target);
        } else if (mode == GIT_MODE_GITLINK || mode == 0160000U) {
            continue;
        } else {
            rt_free(tree);
            return -1;
        }
    }
    rt_free(tree);
    return 0;
}

static int git_index_write_entry(GitBuffer *buffer, const GitIndexEntry *entry) {
    unsigned char header[62];
    size_t path_length = rt_strlen(entry->path);
    size_t entry_length;
    unsigned short flags;

    rt_memset(header, 0, sizeof(header));
    git_write_u32_be(header + 24U, entry->mode);
    git_write_u32_be(header + 36U, (unsigned int)entry->size);
    memcpy(header + 40U, entry->oid, CRYPTO_SHA1_DIGEST_SIZE);
    flags = path_length < 0x0fffU ? (unsigned short)path_length : 0x0fffU;
    header[60] = (unsigned char)(flags >> 8);
    header[61] = (unsigned char)flags;
    if (git_buffer_append(buffer, header, sizeof(header)) != 0 || git_buffer_append(buffer, entry->path, path_length) != 0 || git_buffer_append_char(buffer, '\0') != 0) {
        return -1;
    }
    entry_length = 62U + path_length + 1U;
    while ((entry_length & 7U) != 0U) {
        if (git_buffer_append_char(buffer, '\0') != 0) {
            return -1;
        }
        entry_length += 1U;
    }
    return 0;
}

static int git_write_index_file(const GitRepo *repo, GitIndex *index) {
    GitBuffer buffer;
    unsigned char word[4];
    unsigned char digest[CRYPTO_SHA1_DIGEST_SIZE];
    CryptoSha1Context sha1;
    char path[GIT_PATH_CAPACITY];
    size_t i;

    rt_memset(&buffer, 0, sizeof(buffer));
    if (index->count > 1U) {
        rt_sort(index->entries, index->count, sizeof(index->entries[0]), git_compare_entries_by_path);
    }
    if (git_buffer_append(&buffer, "DIRC", 4U) != 0) {
        return -1;
    }
    git_write_u32_be(word, 2U);
    if (git_buffer_append(&buffer, word, 4U) != 0) {
        git_buffer_destroy(&buffer);
        return -1;
    }
    git_write_u32_be(word, (unsigned int)index->count);
    if (git_buffer_append(&buffer, word, 4U) != 0) {
        git_buffer_destroy(&buffer);
        return -1;
    }
    for (i = 0U; i < index->count; ++i) {
        if (git_index_write_entry(&buffer, &index->entries[i]) != 0) {
            git_buffer_destroy(&buffer);
            return -1;
        }
    }
    crypto_sha1_init(&sha1);
    crypto_sha1_update(&sha1, buffer.data, buffer.size);
    crypto_sha1_final(&sha1, digest);
    if (git_buffer_append(&buffer, digest, sizeof(digest)) != 0 || git_join(path, sizeof(path), repo->git_dir, "index") != 0 || git_write_all_file(path, buffer.data, buffer.size, 0644U) != 0) {
        git_buffer_destroy(&buffer);
        return -1;
    }
    git_buffer_destroy(&buffer);
    return 0;
}

static int git_checkout_commit_to_worktree(const GitRepo *repo, const unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE];
    GitCheckoutIndex checkout;
    int result;

    rt_memset(&checkout, 0, sizeof(checkout));
    checkout.repo = repo;
    if (git_commit_tree_oid(repo, commit_oid, tree_oid) != 0) {
        return -1;
    }
    result = git_checkout_tree_recursive(repo, tree_oid, "", &checkout);
    if (result == 0) {
        result = git_write_index_file(repo, &checkout.entries);
    }
    git_index_destroy(&checkout.entries);
    return result;
}

static int git_remote_refs_push(GitRemoteRefs *refs, const char *name, size_t name_length, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitRemoteRef *new_refs;
    size_t new_capacity;

    if (refs->count == refs->capacity) {
        new_capacity = refs->capacity == 0U ? 16U : refs->capacity * 2U;
        new_refs = (GitRemoteRef *)rt_realloc_array(refs->refs, new_capacity, sizeof(refs->refs[0]));
        if (new_refs == 0) {
            return -1;
        }
        refs->refs = new_refs;
        refs->capacity = new_capacity;
    }
    refs->refs[refs->count].name = git_strdup_n(name, name_length);
    if (refs->refs[refs->count].name == 0) {
        return -1;
    }
    memcpy(refs->refs[refs->count].oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
    refs->count += 1U;
    return 0;
}

static int git_url_service_path(const GitUrl *base, const char *suffix, char *path, size_t path_size) {
    size_t length = rt_strlen(base->path);
    size_t out = 0U;

    if (length + rt_strlen(suffix) + 2U > path_size) {
        return -1;
    }
    rt_copy_string(path, path_size, base->path[0] != '\0' ? base->path : "/");
    out = rt_strlen(path);
    if (out > 0U && path[out - 1U] == '/') {
        path[out - 1U] = '\0';
    }
    rt_copy_string(path + rt_strlen(path), path_size - rt_strlen(path), suffix);
    return 0;
}

static int git_remote_url_with_path(const GitUrl *base, const char *path, GitUrl *out) {
    *out = *base;
    if (rt_strlen(path) >= sizeof(out->path)) {
        return -1;
    }
    rt_copy_string(out->path, sizeof(out->path), path);
    return 0;
}

static int git_parse_advertised_refs(const GitBuffer *body, GitRemoteRefs *refs) {
    size_t pos = 0U;
    int saw_service = 0;
    int saw_first_ref = 0;

    rt_memset(refs, 0, sizeof(*refs));
    while (pos < body->size) {
        size_t packet_length;
        const unsigned char *payload;
        size_t payload_length;

        if (git_pkt_length(body->data + pos, body->size - pos, &packet_length) != 0) {
            git_remote_refs_destroy(refs);
            return -1;
        }
        pos += 4U;
        if (packet_length == GIT_PACKET_FLUSH) {
            continue;
        }
        if (packet_length < 4U || pos + packet_length - 4U > body->size) {
            git_remote_refs_destroy(refs);
            return -1;
        }
        payload = body->data + pos;
        payload_length = packet_length - 4U;
        pos += payload_length;
        if (!saw_service && payload_length >= 15U && memcmp(payload, "# service=", 10U) == 0) {
            saw_service = 1;
            continue;
        }
        if (payload_length < GIT_OBJECT_HEX_SIZE + 2U) {
            continue;
        }
        if (payload[GIT_OBJECT_HEX_SIZE] == ' ') {
            unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
            size_t name_start = GIT_OBJECT_HEX_SIZE + 1U;
            size_t name_end = name_start;
            size_t cap_start;

            if (git_parse_oid_hex_n((const char *)payload, GIT_OBJECT_HEX_SIZE, oid) != 0) {
                continue;
            }
            while (name_end < payload_length && payload[name_end] != '\0' && payload[name_end] != '\n' && payload[name_end] != '\r') {
                name_end += 1U;
            }
            cap_start = name_end < payload_length && payload[name_end] == '\0' ? name_end + 1U : payload_length;
            if (name_end > name_start) {
                if (name_end - name_start == 4U && memcmp(payload + name_start, "HEAD", 4U) == 0) {
                    memcpy(refs->head_oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
                    refs->has_head = 1;
                }
                if (git_remote_refs_push(refs, (const char *)payload + name_start, name_end - name_start, oid) != 0) {
                    git_remote_refs_destroy(refs);
                    return -1;
                }
            }
            if (!saw_first_ref && cap_start < payload_length) {
                size_t i = cap_start;
                const char symref[] = "symref=HEAD:";
                while (i + sizeof(symref) - 1U < payload_length) {
                    if (memcmp(payload + i, symref, sizeof(symref) - 1U) == 0) {
                        size_t start = i + sizeof(symref) - 1U;
                        size_t end = start;
                        while (end < payload_length && payload[end] != ' ' && payload[end] != '\n' && payload[end] != '\r') {
                            end += 1U;
                        }
                        if (end > start && end - start < sizeof(refs->head_ref)) {
                            memcpy(refs->head_ref, payload + start, end - start);
                            refs->head_ref[end - start] = '\0';
                        }
                        break;
                    }
                    i += 1U;
                }
            }
            saw_first_ref = 1;
        }
    }
    return refs->count > 0U ? 0 : -1;
}

static GitRemoteRef *git_remote_find_ref(GitRemoteRefs *refs, const char *name) {
    size_t i;

    for (i = 0U; i < refs->count; ++i) {
        if (rt_strcmp(refs->refs[i].name, name) == 0) {
            return &refs->refs[i];
        }
    }
    return 0;
}

static GitRemoteRef *git_select_remote_ref(GitRemoteRefs *refs, const char *wanted) {
    char refname[GIT_REF_CAPACITY];
    GitRemoteRef *found;
    size_t i;

    if (wanted != 0 && wanted[0] != '\0') {
        found = git_remote_find_ref(refs, wanted);
        if (found != 0) {
            return found;
        }
        if (rt_strncmp(wanted, "refs/", 5U) != 0) {
            if (git_copy(refname, sizeof(refname), "refs/heads/") == 0 && rt_strlen(refname) + rt_strlen(wanted) < sizeof(refname)) {
                rt_copy_string(refname + rt_strlen(refname), sizeof(refname) - rt_strlen(refname), wanted);
                found = git_remote_find_ref(refs, refname);
                if (found != 0) {
                    return found;
                }
            }
        }
    }
    if (refs->head_ref[0] != '\0') {
        found = git_remote_find_ref(refs, refs->head_ref);
        if (found != 0) {
            return found;
        }
    }
    found = git_remote_find_ref(refs, "refs/heads/main");
    if (found != 0) {
        return found;
    }
    found = git_remote_find_ref(refs, "refs/heads/master");
    if (found != 0) {
        return found;
    }
    for (i = 0U; i < refs->count; ++i) {
        if (rt_strncmp(refs->refs[i].name, "refs/heads/", 11U) == 0) {
            return &refs->refs[i];
        }
    }
    return refs->count > 0U ? &refs->refs[0] : 0;
}

static int git_discover_remote_refs(const char *remote_url, GitUrl *base_url, GitRemoteRefs *refs) {
    GitUrl info_url;
    GitBuffer body;
    char path[1024];
    int result;

    if (git_parse_url(remote_url, base_url) != 0 || git_url_service_path(base_url, "/info/refs?service=git-upload-pack", path, sizeof(path)) != 0 || git_remote_url_with_path(base_url, path, &info_url) != 0) {
        return -1;
    }
    if (git_http_request(&info_url, "GET", "application/x-git-upload-pack-advertisement", 0, 0, 0U, &body) != 0) {
        return -1;
    }
    result = git_parse_advertised_refs(&body, refs);
    git_buffer_destroy(&body);
    return result;
}

static int git_extract_pack_from_upload_pack(const GitBuffer *response, GitBuffer *pack_data) {
    size_t pos = 0U;

    rt_memset(pack_data, 0, sizeof(*pack_data));
    while (pos < response->size) {
        size_t packet_length;
        const unsigned char *payload;
        size_t payload_length;

        if (git_pkt_length(response->data + pos, response->size - pos, &packet_length) != 0) {
            return -1;
        }
        pos += 4U;
        if (packet_length == GIT_PACKET_FLUSH) {
            continue;
        }
        if (packet_length < 4U || pos + packet_length - 4U > response->size) {
            return -1;
        }
        payload = response->data + pos;
        payload_length = packet_length - 4U;
        pos += payload_length;
        if (payload_length == 4U && memcmp(payload, "NAK\n", 4U) == 0) {
            continue;
        }
        if (payload_length > 0U) {
            unsigned char channel = payload[0];
            if (channel == 1U) {
                if (git_buffer_append(pack_data, payload + 1U, payload_length - 1U) != 0) {
                    return -1;
                }
            } else if (channel == 2U) {
                continue;
            } else if (channel == 3U) {
                return -1;
            } else if (memcmp(payload, "PACK", payload_length < 4U ? payload_length : 4U) == 0) {
                if (git_buffer_append(pack_data, payload, payload_length) != 0) {
                    return -1;
                }
            }
        }
    }
    return pack_data->size >= 4U && memcmp(pack_data->data, "PACK", 4U) == 0 ? 0 : -1;
}

static int git_fetch_pack(const GitRepo *repo, const GitUrl *base_url, const unsigned char want_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitUrl upload_url;
    GitBuffer request;
    GitBuffer response;
    GitBuffer pack_data;
    GitPack pack;
    char path[1024];
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
    char want_line[256];
    size_t want_length = 0U;
    int result = -1;

    rt_memset(&request, 0, sizeof(request));
    rt_memset(&response, 0, sizeof(response));
    rt_memset(&pack_data, 0, sizeof(pack_data));
    rt_memset(&pack, 0, sizeof(pack));
    git_format_oid_hex(want_oid, oid_hex);
    want_length = tool_buffer_append_cstr(want_line, sizeof(want_line), want_length, "want ");
    want_length = tool_buffer_append_cstr(want_line, sizeof(want_line), want_length, oid_hex);
    want_length = tool_buffer_append_cstr(want_line, sizeof(want_line), want_length, " multi_ack_detailed side-band-64k ofs-delta agent=newos-git\n");
    if (want_length >= sizeof(want_line) || git_append_pkt_line(&request, want_line) != 0 || git_buffer_append_cstr(&request, "0000") != 0 || git_append_pkt_line(&request, "done\n") != 0) {
        goto done;
    }
    if (git_url_service_path(base_url, "/git-upload-pack", path, sizeof(path)) != 0 || git_remote_url_with_path(base_url, path, &upload_url) != 0) {
        goto done;
    }
    if (git_http_request(&upload_url, "POST", "application/x-git-upload-pack-result", "application/x-git-upload-pack-request", request.data, request.size, &response) != 0) {
        goto done;
    }
    if (git_extract_pack_from_upload_pack(&response, &pack_data) != 0 || git_parse_pack(pack_data.data, pack_data.size, &pack) != 0 || git_resolve_pack_deltas(&pack) != 0 || git_write_pack_objects(repo, &pack) != 0) {
        goto done;
    }
    result = 0;
done:
    git_buffer_destroy(&request);
    git_buffer_destroy(&response);
    git_buffer_destroy(&pack_data);
    git_pack_destroy(&pack);
    return result;
}

static int git_write_ref_oid(const GitRepo *repo, const char *ref_name, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    char path[GIT_PATH_CAPACITY];
    char text[GIT_OBJECT_HEX_SIZE + 2U];
    char parent[GIT_PATH_CAPACITY];

    if (git_join(path, sizeof(path), repo->git_dir, ref_name) != 0 || git_copy(parent, sizeof(parent), path) != 0 || git_path_parent(parent) != 0) {
        return -1;
    }
    if (git_make_directory_chain(parent) != 0) {
        return -1;
    }
    git_format_oid_hex(oid, text);
    text[GIT_OBJECT_HEX_SIZE] = '\n';
    text[GIT_OBJECT_HEX_SIZE + 1U] = '\0';
    return git_write_all_file(path, text, GIT_OBJECT_HEX_SIZE + 1U, 0644U);
}

static int git_write_head_ref(const GitRepo *repo, const char *ref_name) {
    char path[GIT_PATH_CAPACITY];
    GitBuffer text;
    int result;

    rt_memset(&text, 0, sizeof(text));
    if (git_join(path, sizeof(path), repo->git_dir, "HEAD") != 0 || git_buffer_append_cstr(&text, "ref: ") != 0 || git_buffer_append_cstr(&text, ref_name) != 0 || git_buffer_append_char(&text, '\n') != 0) {
        git_buffer_destroy(&text);
        return -1;
    }
    result = git_write_all_file(path, text.data, text.size, 0644U);
    git_buffer_destroy(&text);
    return result;
}

static int git_write_fetch_head(const GitRepo *repo, const char *remote_url, const char *ref_name, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    char path[GIT_PATH_CAPACITY];
    GitBuffer text;
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
    int result;

    rt_memset(&text, 0, sizeof(text));
    git_format_oid_hex(oid, oid_hex);
    if (git_join(path, sizeof(path), repo->git_dir, "FETCH_HEAD") != 0 || git_buffer_append_cstr(&text, oid_hex) != 0 || git_buffer_append_cstr(&text, "\t\t") != 0 || git_buffer_append_cstr(&text, ref_name) != 0 || git_buffer_append_cstr(&text, "\t") != 0 || git_buffer_append_cstr(&text, remote_url) != 0 || git_buffer_append_char(&text, '\n') != 0) {
        git_buffer_destroy(&text);
        return -1;
    }
    result = git_write_all_file(path, text.data, text.size, 0644U);
    git_buffer_destroy(&text);
    return result;
}

static int git_remote_tracking_ref_name(const char *remote_ref, char *buffer, size_t buffer_size) {
    if (rt_strncmp(remote_ref, "refs/heads/", 11U) == 0) {
        size_t branch_length = rt_strlen(remote_ref + 11U);
        const char prefix[] = "refs/remotes/origin/";
        if (sizeof(prefix) - 1U + branch_length >= buffer_size) {
            return -1;
        }
        rt_copy_string(buffer, buffer_size, prefix);
        rt_copy_string(buffer + sizeof(prefix) - 1U, buffer_size - sizeof(prefix) + 1U, remote_ref + 11U);
        return 0;
    }
    return git_copy(buffer, buffer_size, remote_ref);
}

static int git_read_origin_url(const GitRepo *repo, char *buffer, size_t buffer_size) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;
    int in_origin = 0;

    if (git_join(path, sizeof(path), repo->git_dir, "config") != 0 || git_read_file(path, &data, &size) != 0) {
        return -1;
    }
    while (pos < size) {
        size_t start = pos;
        size_t end;
        while (pos < size && data[pos] != '\n') {
            pos += 1U;
        }
        end = pos;
        if (pos < size) {
            pos += 1U;
        }
        while (end > start && (data[end - 1U] == '\r' || data[end - 1U] == ' ' || data[end - 1U] == '\t')) {
            end -= 1U;
        }
        while (start < end && (data[start] == ' ' || data[start] == '\t')) {
            start += 1U;
        }
        if (end > start && data[start] == '[') {
            in_origin = (end - start == 17U && memcmp(data + start, "[remote \"origin\"]", 17U) == 0);
        } else if (in_origin && end > start + 3U && memcmp(data + start, "url", 3U) == 0) {
            size_t eq = start + 3U;
            while (eq < end && (data[eq] == ' ' || data[eq] == '\t')) eq += 1U;
            if (eq < end && data[eq] == '=') {
                eq += 1U;
                while (eq < end && (data[eq] == ' ' || data[eq] == '\t')) eq += 1U;
                if (end > eq && end - eq < buffer_size) {
                    memcpy(buffer, data + eq, end - eq);
                    buffer[end - eq] = '\0';
                    rt_free(data);
                    return 0;
                }
            }
        }
    }
    rt_free(data);
    return -1;
}

static int git_write_clone_config(const GitRepo *repo, const char *remote_url, const char *branch_name) {
    char path[GIT_PATH_CAPACITY];
    GitBuffer text;
    int result;

    rt_memset(&text, 0, sizeof(text));
    if (git_join(path, sizeof(path), repo->git_dir, "config") != 0 ||
        git_buffer_append_cstr(&text, "[core]\n\trepositoryformatversion = 0\n\tfilemode = true\n\tbare = false\n\tlogallrefupdates = true\n") != 0 ||
        git_buffer_append_cstr(&text, "[remote \"origin\"]\n\turl = ") != 0 || git_buffer_append_cstr(&text, remote_url) != 0 ||
        git_buffer_append_cstr(&text, "\n\tfetch = +refs/heads/*:refs/remotes/origin/*\n[branch \"") != 0 || git_buffer_append_cstr(&text, branch_name) != 0 ||
        git_buffer_append_cstr(&text, "\"]\n\tremote = origin\n\tmerge = refs/heads/") != 0 || git_buffer_append_cstr(&text, branch_name) != 0 || git_buffer_append_char(&text, '\n') != 0) {
        git_buffer_destroy(&text);
        return -1;
    }
    result = git_write_all_file(path, text.data, text.size, 0644U);
    git_buffer_destroy(&text);
    return result;
}

static int git_fetch_remote_to_repo(const GitRepo *repo, const char *remote_url, const char *wanted_ref, char *selected_ref_out, size_t selected_ref_size, unsigned char selected_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitUrl base_url;
    GitRemoteRefs refs;
    GitRemoteRef *selected;
    char remote_tracking[GIT_REF_CAPACITY];
    int result = -1;

    rt_memset(&refs, 0, sizeof(refs));
    if (git_discover_remote_refs(remote_url, &base_url, &refs) != 0) {
        return -1;
    }
    selected = git_select_remote_ref(&refs, wanted_ref);
    if (selected == 0) {
        goto done;
    }
    if (git_fetch_pack(repo, &base_url, selected->oid) != 0) {
        goto done;
    }
    if (selected_ref_out != 0 && git_copy(selected_ref_out, selected_ref_size, selected->name) != 0) {
        goto done;
    }
    memcpy(selected_oid, selected->oid, CRYPTO_SHA1_DIGEST_SIZE);
    if (git_remote_tracking_ref_name(selected->name, remote_tracking, sizeof(remote_tracking)) == 0) {
        (void)git_write_ref_oid(repo, remote_tracking, selected->oid);
    }
    (void)git_write_fetch_head(repo, remote_url, selected->name, selected->oid);
    result = 0;
done:
    git_remote_refs_destroy(&refs);
    return result;
}

static int git_load_index(const GitRepo *repo, GitIndex *index) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 12U;
    unsigned int version;
    unsigned int count;
    unsigned int i;

    rt_memset(index, 0, sizeof(*index));
    if (git_join(path, sizeof(path), repo->git_dir, "index") != 0) {
        return -1;
    }
    if (git_read_file(path, &data, &size) != 0) {
        return 0;
    }
    if (size < 12U || git_read_u32_be_raw(data) != GIT_INDEX_SIGNATURE) {
        rt_free(data);
        return -1;
    }
    version = git_read_u32_be_raw(data + 4U);
    count = git_read_u32_be_raw(data + 8U);
    if (version < 2U || version > 4U) {
        rt_free(data);
        return -1;
    }

    for (i = 0U; i < count; ++i) {
        GitIndexEntry entry;
        unsigned int flags;
        size_t path_start;
        size_t path_length;
        size_t entry_start = pos;
        size_t entry_length;

        if (pos + 62U > size) {
            git_index_destroy(index);
            rt_free(data);
            return -1;
        }
        rt_memset(&entry, 0, sizeof(entry));
        entry.mtime_seconds = git_read_u32_be_raw(data + pos + 8U);
        entry.mtime_nanos = git_read_u32_be_raw(data + pos + 12U);
        entry.mode = git_read_u32_be_raw(data + pos + 24U);
        entry.size = git_read_u32_be_raw(data + pos + 36U);
        memcpy(entry.oid, data + pos + 40U, CRYPTO_SHA1_DIGEST_SIZE);
        flags = git_read_u16_be_raw(data + pos + 60U);
        path_start = pos + 62U;
        path_length = flags & 0x0FFFU;
        if (path_length == 0x0FFFU) {
            path_length = 0U;
            while (path_start + path_length < size && data[path_start + path_length] != 0) {
                path_length += 1U;
            }
        }
        if (path_start + path_length > size) {
            git_index_destroy(index);
            rt_free(data);
            return -1;
        }
        entry.path = git_strdup_n((const char *)(data + path_start), path_length);
        if (entry.path == 0) {
            git_index_destroy(index);
            rt_free(data);
            return -1;
        }
        entry_length = 62U + path_length + 1U;
        if (version < 4U) {
            entry_length = (entry_length + 7U) & ~(size_t)7U;
        }
        if (entry_start + entry_length > size || git_index_push(index, &entry) != 0) {
            rt_free(entry.path);
            git_index_destroy(index);
            rt_free(data);
            return -1;
        }
        pos = entry_start + entry_length;
    }

    rt_free(data);
    return 0;
}

static int git_compare_entries_by_path(const void *left, const void *right) {
    const GitIndexEntry *left_entry = (const GitIndexEntry *)left;
    const GitIndexEntry *right_entry = (const GitIndexEntry *)right;

    return rt_strcmp(left_entry->path, right_entry->path);
}

static int git_index_is_sorted(const GitIndex *index) {
    size_t i;

    for (i = 1U; i < index->count; ++i) {
        if (rt_strcmp(index->entries[i - 1U].path, index->entries[i].path) > 0) {
            return 0;
        }
    }
    return 1;
}

static GitIndexEntry *git_index_find(const GitIndex *index, const char *path) {
    size_t lo = 0U;
    size_t hi = index->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        int cmp = rt_strcmp(path, index->entries[mid].path);

        if (cmp == 0) {
            return &index->entries[mid];
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1U;
        }
    }
    return 0;
}

static int git_relative_path(const GitRepo *repo, const char *path, char *buffer, size_t buffer_size) {
    size_t root_len = rt_strlen(repo->work_tree);
    const char *relative;

    if (rt_strncmp(path, repo->work_tree, root_len) != 0) {
        return -1;
    }
    relative = path + root_len;
    if (*relative == '/') {
        relative += 1;
    }
    return git_copy(buffer, buffer_size, relative);
}

static int git_blob_hash_path(const char *path, unsigned long long size, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    int fd;
    CryptoSha1Context sha1;
    char header[64];
    size_t header_len = 0U;
    char size_digits[32];
    char buffer[16384];

    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }
    rt_unsigned_to_string(size, size_digits, sizeof(size_digits));
    header_len = tool_buffer_append_cstr(header, sizeof(header), header_len, "blob ");
    header_len = tool_buffer_append_cstr(header, sizeof(header), header_len, size_digits);
    header_len = tool_buffer_append_char(header, sizeof(header), header_len, '\0');
    if (header_len >= sizeof(header)) {
        platform_close(fd);
        return -1;
    }

    crypto_sha1_init(&sha1);
    crypto_sha1_update(&sha1, (const unsigned char *)header, header_len);
    for (;;) {
        long bytes_read = platform_read(fd, buffer, sizeof(buffer));

        if (bytes_read < 0) {
            platform_close(fd);
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }
        crypto_sha1_update(&sha1, (const unsigned char *)buffer, (size_t)bytes_read);
    }
    platform_close(fd);
    crypto_sha1_final(&sha1, oid);
    return 0;
}

static int git_blob_hash_data(const unsigned char *data, size_t size, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    CryptoSha1Context sha1;
    char header[64];
    size_t header_len = 0U;
    char size_digits[32];

    rt_unsigned_to_string(size, size_digits, sizeof(size_digits));
    header_len = tool_buffer_append_cstr(header, sizeof(header), header_len, "blob ");
    header_len = tool_buffer_append_cstr(header, sizeof(header), header_len, size_digits);
    header_len = tool_buffer_append_char(header, sizeof(header), header_len, '\0');
    if (header_len >= sizeof(header)) {
        return -1;
    }
    crypto_sha1_init(&sha1);
    crypto_sha1_update(&sha1, (const unsigned char *)header, header_len);
    crypto_sha1_update(&sha1, data, size);
    crypto_sha1_final(&sha1, oid);
    return 0;
}

static int git_write_status_line(const char *code, const char *path, int porcelain) {
    if (!porcelain) {
        if (rt_strcmp(code, " M") == 0) {
            rt_write_cstr(1, "modified: ");
        } else if (rt_strcmp(code, " D") == 0) {
            rt_write_cstr(1, "deleted: ");
        } else if (rt_strcmp(code, "??") == 0) {
            rt_write_cstr(1, "untracked: ");
        } else {
            rt_write_cstr(1, code);
            rt_write_char(1, ' ');
        }
        return rt_write_line(1, path);
    }
    if (rt_write_cstr(1, code) != 0 || rt_write_char(1, ' ') != 0 || rt_write_line(1, path) != 0) {
        return -1;
    }
    return 0;
}

static int git_entry_is_modified(const GitRepo *repo, const GitIndexEntry *entry) {
    char full_path[GIT_PATH_CAPACITY];
    PlatformDirEntry info;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];

    if (git_join(full_path, sizeof(full_path), repo->work_tree, entry->path) != 0) {
        return 1;
    }
    if (platform_get_path_info(full_path, &info) != 0) {
        return -1;
    }
    if (info.is_dir) {
        return 1;
    }
    if (entry->mode == GIT_MODE_SYMLINK) {
        char link_target[GIT_PATH_CAPACITY];
        size_t link_size;

        if (platform_read_symlink(full_path, link_target, sizeof(link_target)) != 0) {
            return 1;
        }
        link_size = rt_strlen(link_target);
        if (link_size != entry->size || git_blob_hash_data((const unsigned char *)link_target, link_size, oid) != 0) {
            return 1;
        }
        return memcmp(oid, entry->oid, CRYPTO_SHA1_DIGEST_SIZE) == 0 ? 0 : 1;
    }
    if (!git_index_mode_is_regular(entry->mode)) {
        return 1;
    }
    if (git_regular_index_mode_from_worktree(info.mode) != git_regular_index_mode_from_worktree(entry->mode)) {
        return 1;
    }
    if (info.size != entry->size) {
        return 1;
    }
    if (entry->mtime_seconds != 0ULL &&
        info.mtime == (long long)entry->mtime_seconds &&
        info.mtime_nanos == entry->mtime_nanos) {
        return 0;
    }
    if (git_blob_hash_path(full_path, info.size, oid) != 0) {
        return 1;
    }
    return memcmp(oid, entry->oid, CRYPTO_SHA1_DIGEST_SIZE) == 0 ? 0 : 1;
}

static int git_status_tracked(const GitRepo *repo, const GitIndex *index, int porcelain, int *saw_change) {
    size_t i;

    for (i = 0U; i < index->count; ++i) {
        int modified = git_entry_is_modified(repo, &index->entries[i]);

        if (modified < 0) {
            if (git_write_status_line(" D", index->entries[i].path, porcelain) != 0) {
                return -1;
            }
            *saw_change = 1;
        } else if (modified > 0) {
            if (git_write_status_line(" M", index->entries[i].path, porcelain) != 0) {
                return -1;
            }
            *saw_change = 1;
        }
    }
    return 0;
}

static int git_status_walk_callback(const char *path, const PlatformDirEntry *entry, int depth, ToolWalkControl *control, void *user_data) {
    GitStatusWalk *walk = (GitStatusWalk *)user_data;
    char relative[GIT_PATH_CAPACITY];
    GitIndexEntry *indexed;

    (void)depth;
    if (git_relative_path(walk->repo, path, relative, sizeof(relative)) != 0 || relative[0] == '\0') {
        return 0;
    }
    if (rt_strcmp(relative, ".git") == 0 || rt_strncmp(relative, ".git/", 5U) == 0) {
        control->prune = 1;
        return 0;
    }
    if (git_ignore_matches(walk->ignores, relative, entry->is_dir)) {
        if (entry->is_dir) {
            control->prune = 1;
        }
        return 0;
    }
    indexed = git_index_find(walk->index, relative);
    if (indexed != 0) {
        if (entry->is_dir) {
            control->prune = 0;
        }
        return 0;
    }
    if (entry->is_dir) {
        return 0;
    }
    if (git_write_status_line("??", relative, walk->porcelain) != 0) {
        return -1;
    }
    walk->saw_change = 1;
    return 0;
}

static int git_status_untracked(const GitRepo *repo, const GitIndex *index, const GitIgnoreList *ignores, int porcelain, int *saw_change) {
    GitStatusWalk walk;
    ToolWalkOptions options;

    walk.repo = repo;
    walk.index = index;
    walk.ignores = ignores;
    walk.porcelain = porcelain;
    walk.saw_change = 0;
    options.min_depth = 0;
    options.max_depth = -1;
    if (tool_walk_path(repo->work_tree, &options, git_status_walk_callback, &walk) != 0) {
        return -1;
    }
    if (walk.saw_change) {
        *saw_change = 1;
    }
    return 0;
}

static int git_path_parent(char *path) {
    size_t length = rt_strlen(path);

    while (length > 0U && path[length - 1U] != '/') {
        length -= 1U;
    }
    if (length == 0U) {
        path[0] = '\0';
        return 0;
    }
    if (length == 1U) {
        path[1] = '\0';
        return 0;
    }
    path[length - 1U] = '\0';
    return 0;
}

static int git_make_directory_chain(const char *path) {
    char buffer[GIT_PATH_CAPACITY];
    size_t i;

    if (path == 0 || path[0] == '\0') {
        return 0;
    }
    if (git_copy(buffer, sizeof(buffer), path) != 0) {
        return -1;
    }
    for (i = 1U; buffer[i] != '\0'; ++i) {
        if (buffer[i] == '/') {
            buffer[i] = '\0';
            if (buffer[0] != '\0') {
                int is_directory = 0;
                if (platform_path_is_directory(buffer, &is_directory) != 0) {
                    if (platform_make_directory(buffer, 0755U) != 0) {
                        return -1;
                    }
                } else if (!is_directory) {
                    return -1;
                }
            }
            buffer[i] = '/';
        }
    }
    {
        int is_directory = 0;
        if (platform_path_is_directory(buffer, &is_directory) != 0) {
            return platform_make_directory(buffer, 0755U);
        }
        return is_directory ? 0 : -1;
    }
}

static int git_ensure_parent_directory(const char *path) {
    char parent[GIT_PATH_CAPACITY];

    if (git_copy(parent, sizeof(parent), path) != 0 || git_path_parent(parent) != 0) {
        return -1;
    }
    return git_make_directory_chain(parent);
}

static int git_source_arg_to_path(const char *arg, char *buffer, size_t buffer_size) {
    char cwd[GIT_PATH_CAPACITY];
    const char *path = arg;

    if (rt_strncmp(arg, "file://", 7U) == 0) {
        path = arg + 7U;
    } else if (rt_strncmp(arg, "http://", 7U) == 0 || rt_strncmp(arg, "https://", 8U) == 0 ||
               rt_strncmp(arg, "git://", 6U) == 0 || rt_strncmp(arg, "ssh://", 6U) == 0) {
        tool_write_error("git", "clone transport is not implemented: ", arg);
        return -1;
    }

    if (git_is_absolute_path(path)) {
        return git_copy(buffer, buffer_size, path);
    }
    if (platform_get_current_directory(cwd, sizeof(cwd)) != 0) {
        return -1;
    }
    return git_join(buffer, buffer_size, cwd, path);
}

static int git_default_clone_destination(const char *source, char *buffer, size_t buffer_size) {
    size_t end = rt_strlen(source);
    size_t start;
    size_t length;

    while (end > 0U && source[end - 1U] == '/') {
        end -= 1U;
    }
    start = end;
    while (start > 0U && source[start - 1U] != '/') {
        start -= 1U;
    }
    length = end - start;
    if (length > 4U && source[start + length - 4U] == '.' && source[start + length - 3U] == 'g' &&
        source[start + length - 2U] == 'i' && source[start + length - 1U] == 't') {
        length -= 4U;
    }
    if (length == 0U || length >= buffer_size) {
        return -1;
    }
    memcpy(buffer, source + start, length);
    buffer[length] = '\0';
    return 0;
}

static int git_copy_tracked_files(const GitRepo *source_repo, const GitIndex *index, const char *destination) {
    size_t i;

    for (i = 0U; i < index->count; ++i) {
        char source_path[GIT_PATH_CAPACITY];
        char dest_path[GIT_PATH_CAPACITY];
        PlatformDirEntry info;

        if (tool_path_is_unsafe_relative(index->entries[i].path)) {
            tool_write_error("git", "refusing unsafe checkout path: ", index->entries[i].path);
            return -1;
        }
        if (git_join(source_path, sizeof(source_path), source_repo->work_tree, index->entries[i].path) != 0 ||
            git_join(dest_path, sizeof(dest_path), destination, index->entries[i].path) != 0) {
            return -1;
        }
        if (platform_get_path_info(source_path, &info) != 0 || info.is_dir) {
            tool_write_error("git", "source tracked file is unavailable: ", index->entries[i].path);
            return -1;
        }
        if (!git_index_mode_is_regular(index->entries[i].mode)) {
            tool_write_error("git", "unsupported checkout mode: ", index->entries[i].path);
            return -1;
        }
        if (git_ensure_parent_directory(dest_path) != 0 || tool_copy_file(source_path, dest_path) != 0) {
            tool_write_error("git", "cannot check out file: ", index->entries[i].path);
            return -1;
        }
        (void)platform_change_mode(dest_path, git_worktree_mode_from_regular_index(index->entries[i].mode));
    }
    return 0;
}

static int git_source_tracked_files_are_clean(const GitRepo *source_repo, const GitIndex *index) {
    size_t i;

    for (i = 0U; i < index->count; ++i) {
        int modified = git_entry_is_modified(source_repo, &index->entries[i]);

        if (modified != 0) {
            tool_write_error("git", "source has modified or missing tracked file: ", index->entries[i].path);
            return 0;
        }
    }
    return 1;
}

static int git_init_empty_repo_at(const char *work_tree, GitRepo *repo) {
    char path[GIT_PATH_CAPACITY];

    rt_memset(repo, 0, sizeof(*repo));
    if (git_copy(repo->work_tree, sizeof(repo->work_tree), work_tree) != 0 || git_join(repo->git_dir, sizeof(repo->git_dir), work_tree, ".git") != 0) {
        return -1;
    }
    if (git_make_directory_chain(repo->git_dir) != 0) {
        return -1;
    }
    if (git_join(path, sizeof(path), repo->git_dir, "objects") != 0 || git_make_directory_chain(path) != 0) {
        return -1;
    }
    if (git_join(path, sizeof(path), repo->git_dir, "refs/heads") != 0 || git_make_directory_chain(path) != 0) {
        return -1;
    }
    if (git_join(path, sizeof(path), repo->git_dir, "refs/remotes/origin") != 0 || git_make_directory_chain(path) != 0) {
        return -1;
    }
    return 0;
}

static const char *git_branch_from_ref(const char *ref_name) {
    if (rt_strncmp(ref_name, "refs/heads/", 11U) == 0) {
        return ref_name + 11U;
    }
    return 0;
}

static int git_cmd_clone_remote(const char *remote_url, const char *destination_arg, const char *destination) {
    GitRepo repo;
    char selected_ref[GIT_REF_CAPACITY];
    char local_ref[GIT_REF_CAPACITY];
    const char *branch_name;
    unsigned char selected_oid[CRYPTO_SHA1_DIGEST_SIZE];

    if (platform_make_directory(destination, 0755U) != 0 || git_init_empty_repo_at(destination, &repo) != 0) {
        tool_write_error("git", "cannot create destination: ", destination_arg);
        return 1;
    }
    if (git_fetch_remote_to_repo(&repo, remote_url, 0, selected_ref, sizeof(selected_ref), selected_oid) != 0) {
        tool_write_error("git", "remote clone failed: ", remote_url);
        (void)tool_remove_path(destination, 1);
        return 1;
    }
    branch_name = git_branch_from_ref(selected_ref);
    if (branch_name == 0 || branch_name[0] == '\0') {
        branch_name = "main";
    }
    if (git_copy(local_ref, sizeof(local_ref), "refs/heads/") != 0 || rt_strlen(local_ref) + rt_strlen(branch_name) >= sizeof(local_ref)) {
        (void)tool_remove_path(destination, 1);
        return 1;
    }
    rt_copy_string(local_ref + rt_strlen(local_ref), sizeof(local_ref) - rt_strlen(local_ref), branch_name);
    if (git_write_ref_oid(&repo, local_ref, selected_oid) != 0 || git_write_head_ref(&repo, local_ref) != 0 || git_write_clone_config(&repo, remote_url, branch_name) != 0 || git_checkout_commit_to_worktree(&repo, selected_oid) != 0) {
        tool_write_error("git", "checkout failed: ", destination_arg);
        (void)tool_remove_path(destination, 1);
        return 1;
    }
    rt_write_cstr(1, "Cloned remote repository to ");
    rt_write_line(1, destination_arg);
    return 0;
}

static int git_cmd_clone(int argc, char **argv, int argi) {
    GitRepo source_repo;
    GitIndex index;
    char source_path[GIT_PATH_CAPACITY];
    char destination_arg[GIT_PATH_CAPACITY];
    char destination[GIT_PATH_CAPACITY];
    char dest_git[GIT_PATH_CAPACITY];
    PlatformDirEntry existing;
    int result = 1;

    if (argi >= argc) {
        tool_write_error("git", "clone needs a source", 0);
        return 1;
    }
    if (argi + 2 < argc) {
        tool_write_error("git", "too many clone arguments", 0);
        return 1;
    }
    if (argi + 1 < argc) {
        if (git_copy(destination_arg, sizeof(destination_arg), argv[argi + 1]) != 0) {
            return 1;
        }
    } else if (git_default_clone_destination(argv[argi], destination_arg, sizeof(destination_arg)) != 0) {
        tool_write_error("git", "cannot derive clone destination", 0);
        return 1;
    }
    if (git_is_absolute_path(destination_arg)) {
        if (git_copy(destination, sizeof(destination), destination_arg) != 0) {
            return 1;
        }
    } else {
        char cwd[GIT_PATH_CAPACITY];
        if (platform_get_current_directory(cwd, sizeof(cwd)) != 0 || git_join(destination, sizeof(destination), cwd, destination_arg) != 0) {
            return 1;
        }
    }

    if (platform_get_path_info(destination, &existing) == 0) {
        tool_write_error("git", "destination already exists: ", destination_arg);
        return 1;
    }
    if (git_url_is_http(argv[argi])) {
        return git_cmd_clone_remote(argv[argi], destination_arg, destination);
    }
    if (git_source_arg_to_path(argv[argi], source_path, sizeof(source_path)) != 0) {
        return 1;
    }
    if (git_discover_from(source_path, &source_repo) != 0 || git_load_head(&source_repo) != 0) {
        tool_write_error("git", "source is not a supported local repository: ", argv[argi]);
        return 1;
    }
    if (git_load_index(&source_repo, &index) != 0) {
        tool_write_error("git", "cannot read source index", 0);
        return 1;
    }
    if (!git_source_tracked_files_are_clean(&source_repo, &index)) {
        git_index_destroy(&index);
        return 1;
    }

    if (platform_make_directory(destination, 0755U) != 0) {
        tool_write_error("git", "cannot create destination: ", destination_arg);
        git_index_destroy(&index);
        return 1;
    }
    if (git_join(dest_git, sizeof(dest_git), destination, ".git") != 0 ||
        tool_copy_path(source_repo.git_dir, dest_git, 1, 1, 1) != 0 ||
        git_copy_tracked_files(&source_repo, &index, destination) != 0) {
        tool_write_error("git", "clone failed", destination_arg);
        git_index_destroy(&index);
        return 1;
    }

    rt_write_cstr(1, "Cloned local repository to ");
    rt_write_line(1, destination_arg);
    git_index_destroy(&index);
    result = 0;
    return result;
}

static int git_resolve_revision(GitRepo *repo, const char *name, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], char *head_ref_out, size_t head_ref_size) {
    char ref[GIT_REF_CAPACITY];

    if (head_ref_out != 0 && head_ref_size > 0U) {
        head_ref_out[0] = '\0';
    }
    if (rt_strcmp(name, "HEAD") == 0) {
        if (repo->head_oid[0] == '\0') {
            return -1;
        }
        if (git_parse_oid_hex(repo->head_oid, oid) != 0) {
            return -1;
        }
        if (head_ref_out != 0 && repo->head_ref[0] != '\0') {
            (void)git_copy(head_ref_out, head_ref_size, repo->head_ref);
        }
        return 0;
    }
    if (rt_strlen(name) == GIT_OBJECT_HEX_SIZE && git_parse_oid_hex(name, oid) == 0) {
        return 0;
    }
    if (rt_strncmp(name, "refs/", 5U) == 0) {
        char hex[GIT_OBJECT_HEX_SIZE + 1U];
        if (git_resolve_ref(repo, name, hex, sizeof(hex)) == 0 && git_parse_oid_hex(hex, oid) == 0) {
            if (head_ref_out != 0) {
                (void)git_copy(head_ref_out, head_ref_size, name);
            }
            return 0;
        }
    } else {
        char hex[GIT_OBJECT_HEX_SIZE + 1U];
        if (git_copy(ref, sizeof(ref), "refs/heads/") == 0 && rt_strlen(ref) + rt_strlen(name) < sizeof(ref)) {
            rt_copy_string(ref + rt_strlen(ref), sizeof(ref) - rt_strlen(ref), name);
            if (git_resolve_ref(repo, ref, hex, sizeof(hex)) == 0 && git_parse_oid_hex(hex, oid) == 0) {
                if (head_ref_out != 0) {
                    (void)git_copy(head_ref_out, head_ref_size, ref);
                }
                return 0;
            }
        }
        if (git_copy(ref, sizeof(ref), "refs/remotes/origin/") == 0 && rt_strlen(ref) + rt_strlen(name) < sizeof(ref)) {
            rt_copy_string(ref + rt_strlen(ref), sizeof(ref) - rt_strlen(ref), name);
            if (git_resolve_ref(repo, ref, hex, sizeof(hex)) == 0 && git_parse_oid_hex(hex, oid) == 0) {
                if (git_copy(ref, sizeof(ref), "refs/heads/") == 0 && rt_strlen(ref) + rt_strlen(name) < sizeof(ref)) {
                    rt_copy_string(ref + rt_strlen(ref), sizeof(ref) - rt_strlen(ref), name);
                    (void)git_write_ref_oid(repo, ref, oid);
                    if (head_ref_out != 0) {
                        (void)git_copy(head_ref_out, head_ref_size, ref);
                    }
                }
                return 0;
            }
        }
    }
    return -1;
}

static int git_cmd_fetch(GitRepo *repo, int argc, char **argv, int argi) {
    char remote_url[GIT_PATH_CAPACITY];
    const char *wanted_ref = 0;
    char selected_ref[GIT_REF_CAPACITY];
    unsigned char selected_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];

    if (argi < argc && git_url_is_http(argv[argi])) {
        if (git_copy(remote_url, sizeof(remote_url), argv[argi]) != 0) {
            return 1;
        }
        argi += 1;
    } else if (git_read_origin_url(repo, remote_url, sizeof(remote_url)) != 0) {
        tool_write_error("git", "fetch needs a URL or remote origin", 0);
        return 1;
    }
    if (argi < argc) {
        wanted_ref = argv[argi++];
    }
    if (argi < argc) {
        tool_write_error("git", "too many fetch arguments", 0);
        return 1;
    }
    if (git_fetch_remote_to_repo(repo, remote_url, wanted_ref, selected_ref, sizeof(selected_ref), selected_oid) != 0) {
        tool_write_error("git", "fetch failed: ", remote_url);
        return 1;
    }
    git_format_oid_hex(selected_oid, oid_hex);
    rt_write_cstr(1, "Fetched ");
    rt_write_cstr(1, selected_ref);
    rt_write_cstr(1, " ");
    rt_write_line(1, oid_hex);
    return 0;
}

static int git_cmd_checkout(GitRepo *repo, int argc, char **argv, int argi) {
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    char head_ref[GIT_REF_CAPACITY];
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];

    if (argi >= argc) {
        tool_write_error("git", "checkout needs a ref", 0);
        return 1;
    }
    if (argi + 1 < argc) {
        tool_write_error("git", "too many checkout arguments", 0);
        return 1;
    }
    if (git_resolve_revision(repo, argv[argi], oid, head_ref, sizeof(head_ref)) != 0) {
        tool_write_error("git", "cannot resolve checkout ref: ", argv[argi]);
        return 1;
    }
    if (git_checkout_commit_to_worktree(repo, oid) != 0) {
        tool_write_error("git", "checkout failed: ", argv[argi]);
        return 1;
    }
    if (head_ref[0] != '\0') {
        (void)git_write_head_ref(repo, head_ref);
    } else {
        char path[GIT_PATH_CAPACITY];
        git_format_oid_hex(oid, oid_hex);
        if (git_join(path, sizeof(path), repo->git_dir, "HEAD") == 0) {
            oid_hex[GIT_OBJECT_HEX_SIZE] = '\n';
            (void)git_write_all_file(path, oid_hex, GIT_OBJECT_HEX_SIZE + 1U, 0644U);
            oid_hex[GIT_OBJECT_HEX_SIZE] = '\0';
        }
    }
    git_format_oid_hex(oid, oid_hex);
    rt_write_cstr(1, "Checked out ");
    rt_write_line(1, oid_hex);
    return 0;
}

static int git_cmd_status(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    GitIgnoreList ignores;
    int porcelain = 0;
    int saw_change = 0;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--short") == 0 || rt_strcmp(argv[argi], "-s") == 0 || rt_strcmp(argv[argi], "--porcelain") == 0) {
            porcelain = 1;
        } else {
            tool_write_error("git", "unsupported status option: ", argv[argi]);
            return 1;
        }
        argi += 1;
    }
    if (git_load_index(repo, &index) != 0) {
        tool_write_error("git", "cannot read index", 0);
        return 1;
    }
    if (index.count > 1U && !git_index_is_sorted(&index)) {
        rt_sort(index.entries, index.count, sizeof(index.entries[0]), git_compare_entries_by_path);
    }
    if (git_ignore_load(repo, &ignores) != 0) {
        git_index_destroy(&index);
        tool_write_error("git", "cannot read ignore files", 0);
        return 1;
    }
    if (git_status_tracked(repo, &index, porcelain, &saw_change) != 0 ||
        git_status_untracked(repo, &index, &ignores, porcelain, &saw_change) != 0) {
        git_ignore_destroy(&ignores);
        git_index_destroy(&index);
        return 1;
    }
    git_ignore_destroy(&ignores);
    git_index_destroy(&index);
    if (!porcelain && !saw_change) {
        rt_write_line(1, "nothing to commit, working tree clean");
    }
    return 0;
}

static int git_cmd_branch(GitRepo *repo, int argc, char **argv, int argi) {
    const char *branch;

    if (argi < argc && rt_strcmp(argv[argi], "--show-current") == 0) {
        branch = git_branch_name(repo);
        if (branch != 0) {
            rt_write_line(1, branch);
        } else {
            rt_write_line(1, "");
        }
        return 0;
    }
    tool_write_error("git", "unsupported branch mode", 0);
    return 1;
}

static int git_cmd_rev_parse(GitRepo *repo, int argc, char **argv, int argi) {
    int exit_code = 0;

    if (argi >= argc) {
        tool_write_error("git", "rev-parse needs an argument", 0);
        return 1;
    }
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--show-toplevel") == 0) {
            rt_write_line(1, repo->work_tree);
        } else if (rt_strcmp(argv[argi], "--git-dir") == 0) {
            rt_write_line(1, repo->git_dir);
        } else if (rt_strcmp(argv[argi], "--abbrev-ref") == 0 && argi + 1 < argc && rt_strcmp(argv[argi + 1], "HEAD") == 0) {
            const char *branch = git_branch_name(repo);

            rt_write_line(1, branch != 0 ? branch : "HEAD");
            argi += 1;
        } else if (rt_strcmp(argv[argi], "HEAD") == 0) {
            if (repo->head_oid[0] == '\0') {
                exit_code = 1;
            } else {
                rt_write_line(1, repo->head_oid);
            }
        } else {
            tool_write_error("git", "unsupported rev-parse argument: ", argv[argi]);
            exit_code = 1;
        }
        argi += 1;
    }
    return exit_code;
}

static int git_cmd_ls_files(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    size_t i;
    (void)repo;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--cached") != 0) {
            tool_write_error("git", "unsupported ls-files option: ", argv[argi]);
            return 1;
        }
        argi += 1;
    }
    if (git_load_index(repo, &index) != 0) {
        tool_write_error("git", "cannot read index", 0);
        return 1;
    }
    if (index.count > 1U && !git_index_is_sorted(&index)) {
        rt_sort(index.entries, index.count, sizeof(index.entries[0]), git_compare_entries_by_path);
    }
    for (i = 0U; i < index.count; ++i) {
        rt_write_line(1, index.entries[i].path);
    }
    git_index_destroy(&index);
    return 0;
}

static int git_cmd_hash_object(int argc, char **argv, int argi) {
    int exit_code = 0;
    int write_object = 0;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-w") == 0) {
            write_object = 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            tool_write_error("git", "unsupported hash-object option: ", argv[argi]);
            return 1;
        }
        argi += 1;
    }
    if (write_object) {
        tool_write_error("git", "hash-object -w is not implemented", 0);
        return 1;
    }
    if (argi >= argc) {
        tool_write_error("git", "hash-object needs a file", 0);
        return 1;
    }
    while (argi < argc) {
        PlatformDirEntry info;
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
        char hex[GIT_OBJECT_HEX_SIZE + 1U];

        if (platform_get_path_info(argv[argi], &info) != 0 || info.is_dir || git_blob_hash_path(argv[argi], info.size, oid) != 0) {
            tool_write_error("git", "cannot hash file: ", argv[argi]);
            exit_code = 1;
        } else {
            git_format_oid_hex(oid, hex);
            rt_write_line(1, hex);
        }
        argi += 1;
    }
    return exit_code;
}

static void git_usage(void) {
    rt_write_line(2, "Usage: git <status|branch|rev-parse|ls-files|hash-object|clone|fetch|checkout> [args ...]");
}

int main(int argc, char **argv) {
    GitRepo repo;
    const char *cmd;

    if (argc < 2 || rt_strcmp(argv[1], "--help") == 0 || rt_strcmp(argv[1], "-h") == 0) {
        git_usage();
        return argc < 2 ? 1 : 0;
    }

    cmd = argv[1];
    if (rt_strcmp(cmd, "hash-object") == 0) {
        return git_cmd_hash_object(argc, argv, 2);
    }
    if (rt_strcmp(cmd, "clone") == 0) {
        return git_cmd_clone(argc, argv, 2);
    }

    if (git_discover(&repo) != 0 || git_load_head(&repo) != 0) {
        tool_write_error("git", "not a git repository", 0);
        return 1;
    }

    if (rt_strcmp(cmd, "status") == 0) {
        return git_cmd_status(&repo, argc, argv, 2);
    }
    if (rt_strcmp(cmd, "branch") == 0) {
        return git_cmd_branch(&repo, argc, argv, 2);
    }
    if (rt_strcmp(cmd, "rev-parse") == 0) {
        return git_cmd_rev_parse(&repo, argc, argv, 2);
    }
    if (rt_strcmp(cmd, "ls-files") == 0) {
        return git_cmd_ls_files(&repo, argc, argv, 2);
    }
    if (rt_strcmp(cmd, "fetch") == 0) {
        return git_cmd_fetch(&repo, argc, argv, 2);
    }
    if (rt_strcmp(cmd, "checkout") == 0) {
        return git_cmd_checkout(&repo, argc, argv, 2);
    }

    tool_write_error("git", "unsupported command: ", cmd);
    return 1;
}