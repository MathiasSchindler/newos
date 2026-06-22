#define main git_tool_main
#include "../../../src/tools/git.c"
#undef main

#include "io_loop.h"
#include "crypto/crypto_util.h"
#include "crypto/rsa.h"
#include "tls/tls13_server.h"

#define GITD_REQUEST_HEADER_CAPACITY 16384U
#define GITD_DEFAULT_MAX_BODY_SIZE (64U * 1024U * 1024U)
#define GITD_DEFAULT_MAX_WANTS 256U
#define GITD_DEFAULT_MAX_HAVES 4096U
#define GITD_DEFAULT_MAX_SHALLOWS 256U
#define GITD_DEFAULT_MAX_REF_PREFIXES 64U
#define GITD_DEFAULT_MAX_COMMANDS 64U
#define GITD_DEFAULT_MAX_OBJECTS 200000U
#define GITD_DEFAULT_MAX_PACK_BYTES (256U * 1024U * 1024U)
#define GITD_MAX_DELTA_BASES 64U
#define GITD_MAX_DELTA_BASE_BYTES (8U * 1024U * 1024U)
#define GITD_DELTA_MIN_COPY 16U
#define GITD_DELTA_SAMPLE_STEP 8U
#define GITD_DELTA_HASH_LIMIT 32768U
#define GITD_DELTA_PROBE_LIMIT 8U
#define GITD_DELTA_SIMILARITY_SAMPLES 64U
#define GITD_IO_CHUNK 8192U
#define GITD_SIDEBAND_CHUNK 60000U
#define GITD_PACKET_DELIM 1U
#define GITD_PACKET_RESPONSE_END 2U

typedef enum {
    GITD_UPLOAD_COMMAND_NONE = 0,
    GITD_UPLOAD_COMMAND_LS_REFS,
    GITD_UPLOAD_COMMAND_FETCH,
    GITD_UPLOAD_COMMAND_OBJECT_INFO,
    GITD_UPLOAD_COMMAND_BUNDLE_URI,
    GITD_UPLOAD_COMMAND_UNSUPPORTED
} GitdUploadCommand;

typedef struct {
    char bind_host[PLATFORM_NETWORK_TEXT_CAPACITY];
    char repo_root[GIT_PATH_CAPACITY];
    char tls_cert_path[GIT_PATH_CAPACITY];
    char tls_key_path[GIT_PATH_CAPACITY];
    unsigned int port;
    size_t max_body_size;
    size_t max_wants;
    size_t max_haves;
    size_t max_shallows;
    size_t max_ref_prefixes;
    size_t max_commands;
    size_t max_objects;
    size_t max_pack_bytes;
    int quiet;
    int once;
    int read_only;
    int allow_delete_refs;
    int allow_tags;
    int allow_notes;
    int allow_custom_refs;
} GitdOptions;

typedef struct {
    unsigned char *cert_der;
    size_t cert_der_len;
    CryptoRsaPrivateKey rsa_key;
    int enabled;
} GitdTlsConfig;

typedef struct {
    int fd;
    int use_tls;
    Tls13Server tls;
} GitdTransport;

typedef struct {
    char method[8];
    char target[GIT_PATH_CAPACITY];
    char path[GIT_PATH_CAPACITY];
    char query[512];
    char content_type[128];
    char content_encoding[64];
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
    char **items;
    size_t count;
    size_t capacity;
} GitdStringList;

typedef struct {
    GitdUploadCommand command;
    unsigned char want_oid[CRYPTO_SHA1_DIGEST_SIZE];
    GitOidList wants;
    GitOidList haves;
    GitOidList shallow_oids;
    GitOidList object_info_oids;
    GitdStringList ref_prefixes;
    size_t deepen;
    int have_want;
    int done;
    int sideband;
    int filter_blob_none;
    int ls_refs_symrefs;
    int ls_refs_peel;
    int object_info_size;
    char unsupported_command[64];
} GitdUploadRequest;

typedef struct {
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char *data;
    size_t size;
} GitdBlobBase;

typedef struct {
    GitdBlobBase items[GITD_MAX_DELTA_BASES];
    size_t count;
    size_t total_bytes;
} GitdBlobBaseList;

typedef struct {
    unsigned int hash;
    size_t offset;
    int used;
} GitdDeltaSlot;

typedef struct {
    RtIoLoop loop;
    GitdOptions options;
    GitdTlsConfig tls_config;
    int listener_fd;
    size_t handled_connections;
} GitdServer;

typedef struct {
    GitdServer *server;
    GitdTransport transport;
    GitBuffer raw;
    GitBuffer body;
    GitdRequest request;
    size_t header_end;
    int saw_header;
} GitdConnection;

static int gitd_path_has_parent_reference(const char *path);

static void gitd_usage(const char *program_name) {
    tool_write_usage(program_name, "[-b HOST] [-p PORT] [-r REPO_ROOT] [--tls-cert CERT --tls-key KEY] [--once] [-q] [--read-only] [--branches-only] [--no-delete-refs] [--max-body BYTES]");
}

static int gitd_parse_size_option(const char *text, size_t *value_out) {
    unsigned long long number;

    if (rt_parse_uint(text, &number) != 0 || number == 0ULL || number > (unsigned long long)((size_t)-1)) return -1;
    *value_out = (size_t)number;
    return 0;
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
        case 501: return "Not Implemented";
        default: return "Error";
    }
}

static long gitd_transport_read(GitdTransport *transport, void *buffer, size_t size) {
    return transport->use_tls ? tls13_server_read_app(&transport->tls, (unsigned char *)buffer, size) : platform_read(transport->fd, buffer, size);
}

static int gitd_transport_write_all(GitdTransport *transport, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t done = 0U;

    while (done < size) {
        long result = transport->use_tls ? tls13_server_write_app(&transport->tls, bytes + done, size - done) : platform_write(transport->fd, bytes + done, size - done);
        if (result <= 0) return -1;
        done += (size_t)result;
    }
    return 0;
}

static void gitd_transport_close(GitdTransport *transport) {
    if (transport == 0 || transport->fd < 0) return;
    if (transport->use_tls && transport->tls.handshake_done) (void)tls13_server_close_notify(&transport->tls);
    (void)platform_close(transport->fd);
    transport->fd = -1;
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

static void gitd_string_list_destroy(GitdStringList *list) {
    size_t index;

    if (list == 0) return;
    for (index = 0U; index < list->count; ++index) {
        rt_free(list->items[index]);
    }
    rt_free(list->items);
    rt_memset(list, 0, sizeof(*list));
}

static int gitd_string_list_push(GitdStringList *list, const char *text, size_t length, size_t limit) {
    char **new_items;
    size_t new_capacity;

    if (list->count >= limit) return -1;
    if (list->count == list->capacity) {
        new_capacity = list->capacity == 0U ? 8U : list->capacity * 2U;
        if (new_capacity > limit) new_capacity = limit;
        new_items = (char **)rt_realloc_array(list->items, new_capacity, sizeof(list->items[0]));
        if (new_items == 0) return -1;
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count] = git_strdup_n(text, length);
    if (list->items[list->count] == 0) return -1;
    list->count += 1U;
    return 0;
}

static void gitd_upload_request_destroy(GitdUploadRequest *request) {
    if (request == 0) return;
    git_oid_list_destroy(&request->wants);
    git_oid_list_destroy(&request->haves);
    git_oid_list_destroy(&request->shallow_oids);
    git_oid_list_destroy(&request->object_info_oids);
    gitd_string_list_destroy(&request->ref_prefixes);
    rt_memset(request, 0, sizeof(*request));
}

static int gitd_pem_marker_matches(const unsigned char *data, size_t offset, size_t size, const char *marker) {
    size_t length = rt_strlen(marker);

    return offset + length <= size && memcmp(data + offset, marker, length) == 0;
}

static int gitd_decode_pem_block(const unsigned char *data, size_t size, const char *label, unsigned char **der_out, size_t *der_size_out) {
    char begin_marker[96];
    char end_marker[96];
    size_t begin_length;
    size_t end_length;
    size_t begin = size;
    size_t end = size;
    size_t index;
    unsigned char *der;
    size_t der_capacity;
    size_t der_size = 0U;
    unsigned int value = 0U;
    unsigned int bits = 0U;

    if (data == 0 || label == 0 || der_out == 0 || der_size_out == 0) return -1;
    if (rt_strlen(label) + 32U >= sizeof(begin_marker)) return -1;
    rt_copy_string(begin_marker, sizeof(begin_marker), "-----BEGIN ");
    rt_copy_string(begin_marker + rt_strlen(begin_marker), sizeof(begin_marker) - rt_strlen(begin_marker), label);
    rt_copy_string(begin_marker + rt_strlen(begin_marker), sizeof(begin_marker) - rt_strlen(begin_marker), "-----");
    rt_copy_string(end_marker, sizeof(end_marker), "-----END ");
    rt_copy_string(end_marker + rt_strlen(end_marker), sizeof(end_marker) - rt_strlen(end_marker), label);
    rt_copy_string(end_marker + rt_strlen(end_marker), sizeof(end_marker) - rt_strlen(end_marker), "-----");
    begin_length = rt_strlen(begin_marker);
    end_length = rt_strlen(end_marker);
    for (index = 0U; index + begin_length <= size; ++index) {
        if (gitd_pem_marker_matches(data, index, size, begin_marker)) {
            begin = index + begin_length;
            break;
        }
    }
    if (begin == size) return -1;
    for (index = begin; index + end_length <= size; ++index) {
        if (gitd_pem_marker_matches(data, index, size, end_marker)) {
            end = index;
            break;
        }
    }
    if (end == size || end <= begin) return -1;
    der_capacity = ((end - begin) / 4U + 1U) * 3U;
    der = (unsigned char *)rt_malloc(der_capacity == 0U ? 1U : der_capacity);
    if (der == 0) return -1;
    for (index = begin; index < end; ++index) {
        int digit;
        unsigned char ch = data[index];

        if (ch == '=' || ch == '-' || ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
        digit = tool_base64_value((char)ch);
        if (digit < 0) {
            rt_free(der);
            return -1;
        }
        value = (value << 6U) | (unsigned int)digit;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            if (der_size >= der_capacity) {
                rt_free(der);
                return -1;
            }
            der[der_size++] = (unsigned char)((value >> bits) & 0xffU);
        }
    }
    *der_out = der;
    *der_size_out = der_size;
    return der_size > 0U ? 0 : -1;
}

static int gitd_load_pem_or_der_file(const char *path, const char *label, unsigned char **der_out, size_t *der_size_out) {
    unsigned char *data = 0;
    size_t size = 0U;

    if (git_read_file(path, &data, &size) != 0) return -1;
    if (gitd_decode_pem_block(data, size, label, der_out, der_size_out) == 0) {
        rt_free(data);
        return 0;
    }
    *der_out = data;
    *der_size_out = size;
    return size > 0U ? 0 : -1;
}

static int gitd_load_tls_config(const GitdOptions *options, GitdTlsConfig *config) {
    unsigned char *key_der = 0;
    size_t key_der_size = 0U;
    int result = -1;

    rt_memset(config, 0, sizeof(*config));
    if (options->tls_cert_path[0] == '\0' && options->tls_key_path[0] == '\0') return 0;
    if (options->tls_cert_path[0] == '\0' || options->tls_key_path[0] == '\0') return -1;
    if (gitd_load_pem_or_der_file(options->tls_cert_path, "CERTIFICATE", &config->cert_der, &config->cert_der_len) != 0) goto done;
    if (gitd_load_pem_or_der_file(options->tls_key_path, "RSA PRIVATE KEY", &key_der, &key_der_size) != 0) goto done;
    if (crypto_rsa2048_parse_private_key_der(&config->rsa_key, key_der, key_der_size) != 0) goto done;
    config->enabled = 1;
    result = 0;
done:
    rt_free(key_der);
    if (result != 0) {
        rt_free(config->cert_der);
        rt_memset(config, 0, sizeof(*config));
    }
    return result;
}

static void gitd_tls_config_destroy(GitdTlsConfig *config) {
    if (config == 0) return;
    rt_free(config->cert_der);
    crypto_secure_bzero(&config->rsa_key, sizeof(config->rsa_key));
    rt_memset(config, 0, sizeof(*config));
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

static int gitd_send_body(GitdTransport *transport, int status, const char *content_type, const unsigned char *body, size_t body_size) {
    GitBuffer header;
    int result = -1;

    rt_memset(&header, 0, sizeof(header));
    if (gitd_header_append_common(&header, status, content_type, body_size) != 0) {
        goto done;
    }
    if (gitd_transport_write_all(transport, header.data, header.size) != 0) {
        goto done;
    }
    if (body_size > 0U && gitd_transport_write_all(transport, body, body_size) != 0) {
        goto done;
    }
    result = 0;
done:
    git_buffer_destroy(&header);
    return result;
}

static int gitd_send_text(GitdTransport *transport, int status, const char *message) {
    return gitd_send_body(transport, status, "text/plain; charset=utf-8", (const unsigned char *)message, message != 0 ? rt_strlen(message) : 0U);
}

static int gitd_send_options(GitdTransport *transport) {
    return gitd_send_body(transport, 204, "text/plain; charset=utf-8", 0, 0U);
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
        } else if (git_header_name_equals(headers + line_start, name_end - line_start, "Content-Encoding")) {
            size_t copy = value_end - value_start;
            if (copy >= sizeof(request->content_encoding)) copy = sizeof(request->content_encoding) - 1U;
            memcpy(request->content_encoding, headers + value_start, copy);
            request->content_encoding[copy] = '\0';
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
    bytes = gitd_transport_read(&connection->transport, buffer, sizeof(buffer));
    if (bytes <= 0) return -1;
    if (!connection->saw_header) {
        size_t already;
        if (git_buffer_append(&connection->raw, buffer, (size_t)bytes) != 0) return -1;
        if (connection->raw.size > GITD_REQUEST_HEADER_CAPACITY + connection->server->options.max_body_size) return -1;
        if (gitd_find_header_end(connection->raw.data, connection->raw.size, &connection->header_end) != 0) {
            if (connection->raw.size > GITD_REQUEST_HEADER_CAPACITY) return -1;
            return 0;
        }
        connection->saw_header = 1;
        if (gitd_parse_request_line(connection->raw.data, connection->header_end, &connection->request) != 0 || gitd_parse_headers(connection->raw.data, connection->header_end, &connection->request) != 0) return -1;
        if (rt_strcmp(connection->request.method, "POST") == 0 && (!connection->request.has_content_length || connection->request.content_length > connection->server->options.max_body_size)) return -1;
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

static unsigned int gitd_read_le32(const unsigned char *data) {
    return (unsigned int)data[0] | ((unsigned int)data[1] << 8U) | ((unsigned int)data[2] << 16U) | ((unsigned int)data[3] << 24U);
}

static int gitd_decode_gzip_body(const GitBuffer *body, size_t max_body_size, GitBuffer *decoded) {
    size_t pos = 10U;
    unsigned int flags;
    unsigned int isize;
    unsigned char *out;
    size_t out_size = 0U;
    size_t deflate_size;

    rt_memset(decoded, 0, sizeof(*decoded));
    if (body->size < 18U || body->data[0] != 0x1fU || body->data[1] != 0x8bU || body->data[2] != 8U) return -1;
    flags = body->data[3];
    if ((flags & 0xe0U) != 0U) return -1;
    if ((flags & 0x04U) != 0U) {
        size_t extra_length;
        if (pos + 2U > body->size) return -1;
        extra_length = (size_t)body->data[pos] | ((size_t)body->data[pos + 1U] << 8U);
        pos += 2U + extra_length;
        if (pos > body->size) return -1;
    }
    if ((flags & 0x08U) != 0U) {
        while (pos < body->size && body->data[pos] != 0U) pos += 1U;
        if (pos >= body->size) return -1;
        pos += 1U;
    }
    if ((flags & 0x10U) != 0U) {
        while (pos < body->size && body->data[pos] != 0U) pos += 1U;
        if (pos >= body->size) return -1;
        pos += 1U;
    }
    if ((flags & 0x02U) != 0U) pos += 2U;
    if (pos + 8U > body->size) return -1;
    deflate_size = body->size - pos - 8U;
    isize = gitd_read_le32(body->data + body->size - 4U);
    if ((size_t)isize > max_body_size) return -1;
    out = (unsigned char *)rt_malloc((size_t)isize == 0U ? 1U : (size_t)isize);
    if (out == 0) return -1;
    if (compression_deflate_inflate_raw(body->data + pos, deflate_size, out, (size_t)isize, &out_size) != 0 || out_size != (size_t)isize) {
        rt_free(out);
        return -1;
    }
    decoded->data = out;
    decoded->size = out_size;
    decoded->capacity = (size_t)isize;
    return 0;
}

static int gitd_request_body_payload(const GitdOptions *options, const GitdRequest *request, const GitBuffer *body, GitBuffer *decoded, const GitBuffer **payload_out) {
    rt_memset(decoded, 0, sizeof(*decoded));
    *payload_out = body;
    if (request->content_encoding[0] == '\0' || git_header_value_contains((const unsigned char *)request->content_encoding, rt_strlen(request->content_encoding), "identity")) return 0;
    if (git_header_value_contains((const unsigned char *)request->content_encoding, rt_strlen(request->content_encoding), "gzip")) {
        if (gitd_decode_gzip_body(body, options->max_body_size, decoded) != 0) return -1;
        *payload_out = decoded;
        return 0;
    }
    return -1;
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

static int gitd_append_v2_upload_pack_advertisement(GitBuffer *out) {
    return git_append_pkt_line(out, "version 2\n") != 0 ||
           git_append_pkt_line(out, "agent=newos-gitd\n") != 0 ||
           git_append_pkt_line(out, "ls-refs=unborn\n") != 0 ||
           git_append_pkt_line(out, "fetch=shallow filter wait-for-done\n") != 0 ||
           git_append_pkt_line(out, "object-info\n") != 0 ||
           git_append_pkt_line(out, "bundle-uri\n") != 0 ||
           git_append_pkt_line(out, "server-option\n") != 0 ||
           git_append_pkt_line(out, "object-format=sha1\n") != 0 ||
           tool_byte_buffer_append_cstr(out, "0000") != 0 ? -1 : 0;
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

static int gitd_handle_info_refs(GitdTransport *transport, const GitdOptions *options, const GitdRequest *request) {
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

    if (rt_strcmp(request->method, "GET") != 0) return gitd_send_text(transport, 405, "method not allowed\n");
    if (rt_strcmp(request->query, "service=git-upload-pack") == 0) {
        service = "git-upload-pack";
        content_type = "application/x-git-upload-pack-advertisement";
    } else if (rt_strcmp(request->query, "service=git-receive-pack") == 0) {
        if (options->read_only) return gitd_send_text(transport, 403, "receive-pack disabled\n");
        service = "git-receive-pack";
        content_type = "application/x-git-receive-pack-advertisement";
        receive_pack = 1;
    } else {
        return gitd_send_text(transport, 400, "expected git service query\n");
    }
    if (gitd_strip_suffix(request->path, "/info/refs", repo_path, sizeof(repo_path)) != 0 || gitd_repo_from_path(options, repo_path, &repo) != 0) return gitd_send_text(transport, 404, "repository not found\n");
    rt_memset(&refs, 0, sizeof(refs));
    rt_memset(&body, 0, sizeof(body));
    if (!receive_pack && git_header_value_contains((const unsigned char *)request->git_protocol, rt_strlen(request->git_protocol), "version=2")) {
        if (gitd_append_v2_upload_pack_advertisement(&body) != 0) goto done;
        result = gitd_send_body(transport, 200, content_type, body.data, body.size);
        goto done;
    }
    if (gitd_collect_refs(&repo, &refs) != 0 || gitd_append_service_advertisement(&body, service) != 0) goto done;
    if (repo.head_oid[0] != '\0' && git_parse_oid_hex_n(repo.head_oid, GIT_OBJECT_HEX_SIZE, head_oid) == 0) {
        have_head = 1;
    } else if (refs.count > 0U) {
        memcpy(head_oid, refs.refs[0].oid, sizeof(head_oid));
        have_head = 1;
    }
    if (receive_pack) {
        rt_copy_string(caps, sizeof(caps), options->allow_delete_refs ? "report-status side-band-64k delete-refs agent=newos-gitd" : "report-status side-band-64k agent=newos-gitd");
    } else {
        rt_copy_string(caps, sizeof(caps), "multi_ack multi_ack_detailed side-band-64k agent=newos-gitd");
    }
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
    result = gitd_send_body(transport, 200, content_type, body.data, body.size);
done:
    git_buffer_destroy(&body);
    gitd_ref_list_destroy(&refs);
    if (result != 0) return gitd_send_text(transport, 500, "cannot advertise refs\n");
    return 0;
}

static size_t gitd_trim_pkt_line_length(const unsigned char *payload, size_t payload_length) {
    while (payload_length > 0U && (payload[payload_length - 1U] == '\n' || payload[payload_length - 1U] == '\r')) payload_length -= 1U;
    return payload_length;
}

static int gitd_pkt_line_equals(const unsigned char *payload, size_t payload_length, const char *text) {
    payload_length = gitd_trim_pkt_line_length(payload, payload_length);
    return payload_length == rt_strlen(text) && memcmp(payload, text, payload_length) == 0;
}

static int gitd_pkt_line_starts_with(const unsigned char *payload, size_t payload_length, const char *prefix) {
    size_t prefix_length = rt_strlen(prefix);

    payload_length = gitd_trim_pkt_line_length(payload, payload_length);
    return payload_length >= prefix_length && memcmp(payload, prefix, prefix_length) == 0;
}

static int gitd_parse_size_token(const unsigned char *data, size_t length, size_t *value_out) {
    size_t i;
    size_t value = 0U;

    if (length == 0U) return -1;
    for (i = 0U; i < length; ++i) {
        if (data[i] < '0' || data[i] > '9') return -1;
        value = value * 10U + (size_t)(data[i] - '0');
    }
    *value_out = value;
    return 0;
}

static int gitd_upload_request_parse_fetch_line(const GitdOptions *options, GitdUploadRequest *upload, const unsigned char *payload, size_t payload_length, int v2) {
    size_t line_length = gitd_trim_pkt_line_length(payload, payload_length);

    if (line_length >= 45U && memcmp(payload, "want ", 5U) == 0) {
        unsigned char want_oid[CRYPTO_SHA1_DIGEST_SIZE];

        if (git_parse_oid_hex_n((const char *)payload + 5U, GIT_OBJECT_HEX_SIZE, want_oid) != 0 || git_oid_list_push_unique(&upload->wants, want_oid) != 0 || upload->wants.count > options->max_wants) return -1;
        if (!upload->have_want) memcpy(upload->want_oid, want_oid, sizeof(upload->want_oid));
        upload->have_want = 1;
    } else if (line_length >= 45U && memcmp(payload, "have ", 5U) == 0) {
        unsigned char have_oid[CRYPTO_SHA1_DIGEST_SIZE];
        if (git_parse_oid_hex_n((const char *)payload + 5U, GIT_OBJECT_HEX_SIZE, have_oid) != 0 || git_oid_list_push_unique(&upload->haves, have_oid) != 0 || upload->haves.count > options->max_haves) return -1;
    } else if (gitd_pkt_line_equals(payload, payload_length, "done")) {
        upload->done = 1;
    } else if (gitd_pkt_line_starts_with(payload, payload_length, "deepen ")) {
        if (gitd_parse_size_token(payload + 7U, line_length - 7U, &upload->deepen) != 0 || upload->deepen == 0U) return -1;
    } else if (gitd_pkt_line_starts_with(payload, payload_length, "filter ")) {
        if (line_length == 16U && memcmp(payload, "filter blob:none", 16U) == 0) {
            upload->filter_blob_none = 1;
        } else {
            return -1;
        }
    }
    if (!v2) {
        size_t i;
        for (i = 0U; i + 13U <= line_length; ++i) {
            if (memcmp(payload + i, "side-band-64k", 13U) == 0) upload->sideband = 1;
        }
    }
    return 0;
}

static int gitd_parse_upload_pack_v1_request(const GitdOptions *options, const GitBuffer *body, GitdUploadRequest *upload) {
    size_t pos = 0U;

    rt_memset(upload, 0, sizeof(*upload));
    upload->command = GITD_UPLOAD_COMMAND_FETCH;
    while (pos < body->size) {
        size_t packet_length;
        const unsigned char *payload;
        size_t payload_length;

        if (git_pkt_length(body->data + pos, body->size - pos, &packet_length) != 0) return -1;
        pos += 4U;
        if (packet_length == GIT_PACKET_FLUSH || packet_length == GITD_PACKET_DELIM || packet_length == GITD_PACKET_RESPONSE_END) continue;
        if (packet_length < 4U || pos + packet_length - 4U > body->size) return -1;
        payload = body->data + pos;
        payload_length = packet_length - 4U;
        pos += payload_length;
        if (gitd_upload_request_parse_fetch_line(options, upload, payload, payload_length, 0) != 0) return -1;
    }
    return upload->have_want || (!upload->done && upload->haves.count > 0U) ? 0 : -1;
}

static int gitd_parse_upload_pack_v2_request(const GitdOptions *options, const GitBuffer *body, GitdUploadRequest *upload) {
    size_t pos = 0U;
    int in_arguments = 0;

    rt_memset(upload, 0, sizeof(*upload));
    upload->sideband = 1;
    while (pos < body->size) {
        size_t packet_length;
        const unsigned char *payload;
        size_t payload_length;

        if (git_pkt_length(body->data + pos, body->size - pos, &packet_length) != 0) return -1;
        pos += 4U;
        if (packet_length == GIT_PACKET_FLUSH || packet_length == GITD_PACKET_RESPONSE_END) break;
        if (packet_length == GITD_PACKET_DELIM) {
            in_arguments = 1;
            continue;
        }
        if (packet_length < 4U || pos + packet_length - 4U > body->size) return -1;
        payload = body->data + pos;
        payload_length = packet_length - 4U;
        pos += payload_length;
        if (!in_arguments) {
            if (gitd_pkt_line_equals(payload, payload_length, "command=ls-refs")) upload->command = GITD_UPLOAD_COMMAND_LS_REFS;
            else if (gitd_pkt_line_equals(payload, payload_length, "command=fetch")) upload->command = GITD_UPLOAD_COMMAND_FETCH;
            else if (gitd_pkt_line_equals(payload, payload_length, "command=object-info")) upload->command = GITD_UPLOAD_COMMAND_OBJECT_INFO;
            else if (gitd_pkt_line_equals(payload, payload_length, "command=bundle-uri")) upload->command = GITD_UPLOAD_COMMAND_BUNDLE_URI;
            else if (gitd_pkt_line_starts_with(payload, payload_length, "command=")) {
                size_t command_length = gitd_trim_pkt_line_length(payload, payload_length) - 8U;
                upload->command = GITD_UPLOAD_COMMAND_UNSUPPORTED;
                if (command_length >= sizeof(upload->unsupported_command)) command_length = sizeof(upload->unsupported_command) - 1U;
                memcpy(upload->unsupported_command, payload + 8U, command_length);
                upload->unsupported_command[command_length] = '\0';
            } else if (gitd_pkt_line_starts_with(payload, payload_length, "server-option=")) {
                continue;
            } else if (gitd_pkt_line_equals(payload, payload_length, "agent=git/newos") || gitd_pkt_line_starts_with(payload, payload_length, "agent=") || gitd_pkt_line_equals(payload, payload_length, "object-format=sha1")) {
                continue;
            } else {
                return -1;
            }
        } else if (upload->command == GITD_UPLOAD_COMMAND_OBJECT_INFO) {
            size_t line_length = gitd_trim_pkt_line_length(payload, payload_length);

            if (gitd_pkt_line_equals(payload, payload_length, "size")) upload->object_info_size = 1;
            else if (line_length == 44U && memcmp(payload, "oid ", 4U) == 0) {
                unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
                if (git_parse_oid_hex_n((const char *)payload + 4U, GIT_OBJECT_HEX_SIZE, oid) != 0 || git_oid_list_push_unique(&upload->object_info_oids, oid) != 0 || upload->object_info_oids.count > options->max_wants) return -1;
            } else {
                return -1;
            }
        } else if (upload->command == GITD_UPLOAD_COMMAND_BUNDLE_URI) {
            return -1;
        } else if (upload->command == GITD_UPLOAD_COMMAND_FETCH) {
            if (gitd_upload_request_parse_fetch_line(options, upload, payload, payload_length, 1) != 0) return -1;
        } else if (upload->command == GITD_UPLOAD_COMMAND_LS_REFS) {
            size_t line_length = gitd_trim_pkt_line_length(payload, payload_length);

            if (gitd_pkt_line_equals(payload, payload_length, "symrefs")) upload->ls_refs_symrefs = 1;
            else if (gitd_pkt_line_equals(payload, payload_length, "peel")) upload->ls_refs_peel = 1;
            else if (gitd_pkt_line_equals(payload, payload_length, "unborn")) continue;
            else if (gitd_pkt_line_starts_with(payload, payload_length, "ref-prefix ")) {
                if (gitd_string_list_push(&upload->ref_prefixes, (const char *)payload + 11U, line_length - 11U, options->max_ref_prefixes) != 0) return -1;
            } else {
                return -1;
            }
        }
    }
    if (upload->command == GITD_UPLOAD_COMMAND_UNSUPPORTED) return 0;
    if (upload->command == GITD_UPLOAD_COMMAND_LS_REFS) return 0;
    if (upload->command == GITD_UPLOAD_COMMAND_OBJECT_INFO) return upload->object_info_size ? 0 : -1;
    if (upload->command == GITD_UPLOAD_COMMAND_BUNDLE_URI) return 0;
    return upload->command == GITD_UPLOAD_COMMAND_FETCH && upload->have_want ? 0 : -1;
}

static int gitd_collect_tree_objects_filtered(GitRepo *repo, const unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitOidList *objects, int include_blobs) {
    unsigned char *tree = 0;
    size_t tree_size = 0U;
    size_t pos = 0U;
    int type = 0;
    int result = -1;

    if (git_oid_list_push_unique(objects, tree_oid) != 0 || git_read_object(repo, tree_oid, pack_cache, &type, &tree, &tree_size) != 0 || type != GIT_OBJECT_TREE) goto done;
    while (pos < tree_size) {
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
        unsigned int mode = 0U;
        size_t name_start;

        while (pos < tree_size && tree[pos] >= '0' && tree[pos] <= '7') {
            mode = (mode << 3U) + (unsigned int)(tree[pos] - '0');
            pos += 1U;
        }
        if (pos >= tree_size || tree[pos] != ' ') goto done;
        pos += 1U;
        name_start = pos;
        while (pos < tree_size && tree[pos] != '\0') pos += 1U;
        if (pos >= tree_size || pos == name_start || pos + 1U + CRYPTO_SHA1_DIGEST_SIZE > tree_size) goto done;
        pos += 1U;
        memcpy(oid, tree + pos, CRYPTO_SHA1_DIGEST_SIZE);
        pos += CRYPTO_SHA1_DIGEST_SIZE;
        if ((mode & GIT_MODE_TYPE_MASK) == GIT_MODE_TREE) {
            if (gitd_collect_tree_objects_filtered(repo, oid, pack_cache, objects, include_blobs) != 0) goto done;
        } else if (include_blobs && ((mode & GIT_MODE_TYPE_MASK) == GIT_MODE_REGULAR_TYPE || (mode & GIT_MODE_TYPE_MASK) == GIT_MODE_SYMLINK)) {
            if (git_oid_list_push_unique(objects, oid) != 0) goto done;
        } else if ((mode & GIT_MODE_TYPE_MASK) != GIT_MODE_REGULAR_TYPE && (mode & GIT_MODE_TYPE_MASK) != GIT_MODE_SYMLINK && (mode & GIT_MODE_TYPE_MASK) != GIT_MODE_GITLINK) {
            goto done;
        }
    }
    result = 0;
done:
    rt_free(tree);
    return result;
}

static int gitd_collect_commit_objects_filtered(GitRepo *repo, const unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, const GitOidList *excluded, size_t depth_remaining, int include_blobs, GitOidList *objects, GitOidList *visited, GitOidList *shallow_oids) {
    GitCommitInfo info;
    size_t i;
    int result = -1;

    if (git_oid_list_contains(visited, commit_oid) || (excluded != 0 && git_oid_list_contains(excluded, commit_oid))) return 0;
    if (git_oid_list_push_unique(visited, commit_oid) != 0 || git_oid_list_push_unique(objects, commit_oid) != 0) return -1;
    if (git_read_commit_info(repo, commit_oid, pack_cache, &info) != 0) return -1;
    if (gitd_collect_tree_objects_filtered(repo, info.tree_oid, pack_cache, objects, include_blobs) != 0) goto done;
    if (depth_remaining == 1U) {
        if (info.parent_count > 0U && shallow_oids != 0 && git_oid_list_push_unique(shallow_oids, commit_oid) != 0) goto done;
    } else {
        size_t next_depth = depth_remaining > 1U ? depth_remaining - 1U : 0U;
        for (i = 0U; i < info.parent_count; ++i) {
            if (gitd_collect_commit_objects_filtered(repo, info.parents[i], pack_cache, excluded, next_depth, include_blobs, objects, visited, shallow_oids) != 0) goto done;
        }
    }
    result = 0;
done:
    git_commit_info_destroy(&info);
    return result;
}

static int gitd_collect_wanted_objects(GitRepo *repo, const GitPack *pack_cache, GitdUploadRequest *upload, const GitOidList *excluded, GitOidList *objects, GitOidList *visited) {
    size_t i;
    int include_blobs = !upload->filter_blob_none;

    for (i = 0U; i < upload->wants.count; ++i) {
        int type = 0;
        unsigned char *data = 0;
        size_t size = 0U;

        if (git_read_object(repo, upload->wants.oids[i], pack_cache, &type, &data, &size) != 0) return -1;
        rt_free(data);
        if (type == GIT_OBJECT_COMMIT) {
            if (gitd_collect_commit_objects_filtered(repo, upload->wants.oids[i], pack_cache, excluded, upload->deepen, include_blobs, objects, visited, &upload->shallow_oids) != 0) return -1;
        } else if (type == GIT_OBJECT_TREE) {
            if (gitd_collect_tree_objects_filtered(repo, upload->wants.oids[i], pack_cache, objects, include_blobs) != 0) return -1;
        } else if (type == GIT_OBJECT_BLOB || type == GIT_OBJECT_TAG) {
            if (git_oid_list_push_unique(objects, upload->wants.oids[i]) != 0) return -1;
        } else {
            return -1;
        }
    }
    return 0;
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

static int gitd_append_delta_copy(GitBuffer *buffer, size_t offset, size_t size) {
    while (size > 0U) {
        size_t chunk = size > 0x10000U ? 0x10000U : size;
        unsigned char opcode = 0x80U;
        unsigned char bytes[7];
        size_t count = 1U;

        if ((offset & 0xffU) != 0U) {
            opcode |= 0x01U;
            bytes[count++] = (unsigned char)(offset & 0xffU);
        }
        if ((offset & 0xff00U) != 0U) {
            opcode |= 0x02U;
            bytes[count++] = (unsigned char)((offset >> 8U) & 0xffU);
        }
        if ((offset & 0xff0000U) != 0U) {
            opcode |= 0x04U;
            bytes[count++] = (unsigned char)((offset >> 16U) & 0xffU);
        }
        if ((offset & 0xff000000U) != 0U) {
            opcode |= 0x08U;
            bytes[count++] = (unsigned char)((offset >> 24U) & 0xffU);
        }
        if (chunk != 0x10000U) {
            if ((chunk & 0xffU) != 0U) {
                opcode |= 0x10U;
                bytes[count++] = (unsigned char)(chunk & 0xffU);
            }
            if ((chunk & 0xff00U) != 0U) {
                opcode |= 0x20U;
                bytes[count++] = (unsigned char)((chunk >> 8U) & 0xffU);
            }
            if ((chunk & 0xff0000U) != 0U) {
                opcode |= 0x40U;
                bytes[count++] = (unsigned char)((chunk >> 16U) & 0xffU);
            }
        }
        bytes[0] = opcode;
        if (git_buffer_append(buffer, bytes, count) != 0) return -1;
        offset += chunk;
        size -= chunk;
    }
    return 0;
}

static int gitd_append_delta_insert(GitBuffer *buffer, const unsigned char *data, size_t size) {
    size_t pos = 0U;

    while (pos < size) {
        size_t chunk = size - pos;

        if (chunk > 127U) chunk = 127U;
        if (tool_byte_buffer_append_byte(buffer, (unsigned char)chunk) != 0 || git_buffer_append(buffer, data + pos, chunk) != 0) return -1;
        pos += chunk;
    }
    return 0;
}

static unsigned int gitd_delta_hash_block(const unsigned char *data) {
    unsigned int hash = 2166136261U;
    size_t index;

    for (index = 0U; index < GITD_DELTA_MIN_COPY; ++index) {
        hash ^= (unsigned int)data[index];
        hash *= 16777619U;
    }
    return hash == 0U ? 1U : hash;
}

static size_t gitd_delta_table_size(size_t base_size) {
    size_t chunks;
    size_t slots = 1024U;

    if (base_size < GITD_DELTA_MIN_COPY) return 0U;
    chunks = 1U + (base_size - GITD_DELTA_MIN_COPY) / GITD_DELTA_SAMPLE_STEP;
    while (slots < chunks * 2U && slots < GITD_DELTA_HASH_LIMIT) slots *= 2U;
    if (slots > GITD_DELTA_HASH_LIMIT) slots = GITD_DELTA_HASH_LIMIT;
    return slots;
}

static GitdDeltaSlot *gitd_build_delta_table(const unsigned char *base, size_t base_size, size_t *slot_count_out) {
    GitdDeltaSlot *slots;
    size_t slot_count = gitd_delta_table_size(base_size);
    size_t offset;

    *slot_count_out = slot_count;
    if (slot_count == 0U) return 0;
    slots = (GitdDeltaSlot *)rt_malloc_array(slot_count, sizeof(slots[0]));
    if (slots == 0) return 0;
    rt_memset(slots, 0, slot_count * sizeof(slots[0]));
    for (offset = 0U; offset + GITD_DELTA_MIN_COPY <= base_size; offset += GITD_DELTA_SAMPLE_STEP) {
        unsigned int hash = gitd_delta_hash_block(base + offset);
        size_t slot = (size_t)hash & (slot_count - 1U);
        size_t probe;

        for (probe = 0U; probe < GITD_DELTA_PROBE_LIMIT; ++probe) {
            GitdDeltaSlot *candidate = &slots[(slot + probe) & (slot_count - 1U)];
            if (!candidate->used) {
                candidate->used = 1;
                candidate->hash = hash;
                candidate->offset = offset;
                break;
            }
        }
    }
    return slots;
}

static size_t gitd_delta_match_size(const unsigned char *base, size_t base_size, size_t base_offset, const unsigned char *target, size_t target_size, size_t target_offset) {
    size_t length = 0U;

    while (base_offset + length < base_size && target_offset + length < target_size && base[base_offset + length] == target[target_offset + length]) length += 1U;
    return length;
}

static int gitd_find_delta_match(const GitdDeltaSlot *slots, size_t slot_count, const unsigned char *base, size_t base_size, const unsigned char *target, size_t target_size, size_t target_offset, size_t *offset_out, size_t *size_out) {
    unsigned int hash;
    size_t slot;
    size_t probe;
    size_t best_size = 0U;
    size_t best_offset = 0U;

    if (slots == 0 || target_offset + GITD_DELTA_MIN_COPY > target_size) return 0;
    hash = gitd_delta_hash_block(target + target_offset);
    slot = (size_t)hash & (slot_count - 1U);
    for (probe = 0U; probe < GITD_DELTA_PROBE_LIMIT; ++probe) {
        const GitdDeltaSlot *candidate = &slots[(slot + probe) & (slot_count - 1U)];
        size_t match_size;

        if (!candidate->used) break;
        if (candidate->hash != hash || candidate->offset + GITD_DELTA_MIN_COPY > base_size || memcmp(base + candidate->offset, target + target_offset, GITD_DELTA_MIN_COPY) != 0) continue;
        match_size = gitd_delta_match_size(base, base_size, candidate->offset, target, target_size, target_offset);
        if (match_size > best_size) {
            best_size = match_size;
            best_offset = candidate->offset;
        }
    }
    if (best_size < GITD_DELTA_MIN_COPY) return 0;
    *offset_out = best_offset;
    *size_out = best_size;
    return 1;
}

static int gitd_build_blob_delta(const unsigned char *base, size_t base_size, const unsigned char *target, size_t target_size, GitBuffer *delta_out) {
    GitdDeltaSlot *slots = 0;
    size_t slot_count = 0U;
    size_t target_pos = 0U;
    size_t insert_start = 0U;
    int result = -1;

    rt_memset(delta_out, 0, sizeof(*delta_out));
    if (gitd_append_delta_varint(delta_out, base_size) != 0 || gitd_append_delta_varint(delta_out, target_size) != 0) goto fail;
    slots = gitd_build_delta_table(base, base_size, &slot_count);
    while (target_pos < target_size) {
        size_t copy_offset = 0U;
        size_t copy_size = 0U;

        if (gitd_find_delta_match(slots, slot_count, base, base_size, target, target_size, target_pos, &copy_offset, &copy_size)) {
            if (target_pos > insert_start && gitd_append_delta_insert(delta_out, target + insert_start, target_pos - insert_start) != 0) goto fail;
            if (gitd_append_delta_copy(delta_out, copy_offset, copy_size) != 0) goto fail;
            target_pos += copy_size;
            insert_start = target_pos;
        } else {
            target_pos += 1U;
        }
    }
    if (target_size > insert_start && gitd_append_delta_insert(delta_out, target + insert_start, target_size - insert_start) != 0) goto fail;
    result = 0;
fail:
    rt_free(slots);
    if (result == 0) return 0;
    git_buffer_destroy(delta_out);
    return -1;
}

static size_t gitd_blob_sample_step(size_t size) {
    size_t step;

    if (size <= GITD_DELTA_MIN_COPY * GITD_DELTA_SIMILARITY_SAMPLES) return GITD_DELTA_MIN_COPY;
    step = size / GITD_DELTA_SIMILARITY_SAMPLES;
    if (step < GITD_DELTA_MIN_COPY) step = GITD_DELTA_MIN_COPY;
    return step;
}

static int gitd_blob_has_matching_sample(const unsigned char *data, size_t size, unsigned int hash) {
    size_t step = gitd_blob_sample_step(size);
    size_t offset;

    for (offset = 0U; offset + GITD_DELTA_MIN_COPY <= size; offset += step) {
        if (gitd_delta_hash_block(data + offset) == hash) return 1;
    }
    return 0;
}

static void gitd_blob_base_list_destroy(GitdBlobBaseList *list) {
    size_t index;

    if (list == 0) return;
    for (index = 0U; index < list->count; ++index) {
        rt_free(list->items[index].data);
    }
    rt_memset(list, 0, sizeof(*list));
}

static size_t gitd_blob_similarity_score(const unsigned char *left, size_t left_size, const unsigned char *right, size_t right_size) {
    size_t prefix = 0U;
    size_t suffix = 0U;
    size_t sampled = 0U;

    while (prefix < left_size && prefix < right_size && left[prefix] == right[prefix]) prefix += 1U;
    while (suffix < left_size - prefix && suffix < right_size - prefix && left[left_size - 1U - suffix] == right[right_size - 1U - suffix]) suffix += 1U;
    if (left_size >= GITD_DELTA_MIN_COPY && right_size >= GITD_DELTA_MIN_COPY) {
        size_t step = gitd_blob_sample_step(left_size);
        size_t offset;

        for (offset = 0U; offset + GITD_DELTA_MIN_COPY <= left_size; offset += step) {
            if (gitd_blob_has_matching_sample(right, right_size, gitd_delta_hash_block(left + offset))) sampled += GITD_DELTA_MIN_COPY;
        }
    }
    return prefix + suffix + sampled;
}

static GitdBlobBase *gitd_choose_blob_delta_base(GitdBlobBaseList *bases, const unsigned char *data, size_t size) {
    GitdBlobBase *best = 0;
    size_t best_score = 0U;
    size_t index;

    for (index = 0U; index < bases->count; ++index) {
        size_t score = gitd_blob_similarity_score(bases->items[index].data, bases->items[index].size, data, size);
        if (score > best_score) {
            best_score = score;
            best = &bases->items[index];
        }
    }
    return best_score >= 8U ? best : 0;
}

static int gitd_blob_base_list_take(GitdBlobBaseList *bases, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], unsigned char **data_io, size_t size) {
    GitdBlobBase *slot;

    if (*data_io == 0 || bases->count >= GITD_MAX_DELTA_BASES || size > GITD_MAX_DELTA_BASE_BYTES || bases->total_bytes > GITD_MAX_DELTA_BASE_BYTES - size) return 0;
    slot = &bases->items[bases->count++];
    memcpy(slot->oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
    slot->data = *data_io;
    slot->size = size;
    bases->total_bytes += size;
    *data_io = 0;
    return 0;
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
    GitdBlobBaseList blob_bases;
    size_t i;
    int result = -1;

    rt_memset(&pack, 0, sizeof(pack));
    rt_memset(&blob_bases, 0, sizeof(blob_bases));
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
        if (type == GIT_OBJECT_BLOB) {
            GitBuffer delta;
            GitdBlobBase *base = gitd_choose_blob_delta_base(&blob_bases, data, size);

            rt_memset(&delta, 0, sizeof(delta));
            if (base != 0 && gitd_build_blob_delta(base->data, base->size, data, size, &delta) == 0 && delta.size + CRYPTO_SHA1_DIGEST_SIZE + 8U < size) {
                if (git_pack_append_object_header(&pack, GIT_OBJECT_REF_DELTA, delta.size) != 0 || git_buffer_append(&pack, base->oid, sizeof(base->oid)) != 0 || gitd_append_compressed_pack_payload(&pack, delta.data, delta.size, oid_hex) != 0) {
                    git_buffer_destroy(&delta);
                    rt_free(data);
                    goto done;
                }
                git_buffer_destroy(&delta);
            } else {
                git_buffer_destroy(&delta);
                if (git_pack_append_object_header(&pack, type, size) != 0 || gitd_append_compressed_pack_payload(&pack, data, size, oid_hex) != 0) {
                    rt_free(data);
                    goto done;
                }
            }
            if (gitd_blob_base_list_take(&blob_bases, objects->oids[i], &data, size) != 0) {
                rt_free(data);
                goto done;
            }
        } else {
            if (git_pack_append_object_header(&pack, type, size) != 0 || gitd_append_compressed_pack_payload(&pack, data, size, oid_hex) != 0) {
                rt_free(data);
                goto done;
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
    gitd_blob_base_list_destroy(&blob_bases);
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

static int gitd_ls_refs_prefix_matches(const GitdUploadRequest *upload, const char *ref_name) {
    size_t index;

    if (upload->ref_prefixes.count == 0U) return 1;
    for (index = 0U; index < upload->ref_prefixes.count; ++index) {
        const char *prefix = upload->ref_prefixes.items[index];
        size_t prefix_length = rt_strlen(prefix);

        if (prefix_length == 0U) return 1;
        if (rt_strcmp(prefix, ref_name) == 0) return 1;
        if (rt_strncmp(ref_name, prefix, prefix_length) == 0) return 1;
    }
    return 0;
}

static int gitd_append_v2_ref_line(GitBuffer *out, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const char *name, const char *symref_target, int include_symrefs) {
    char hex[GIT_OBJECT_HEX_SIZE + 1U];
    GitBuffer line;
    int result;

    git_format_oid_hex(oid, hex);
    rt_memset(&line, 0, sizeof(line));
    if (tool_byte_buffer_append_cstr(&line, hex) != 0 || tool_byte_buffer_append_char(&line, ' ') != 0 || tool_byte_buffer_append_cstr(&line, name) != 0) {
        git_buffer_destroy(&line);
        return -1;
    }
    if (include_symrefs && symref_target != 0 && symref_target[0] != '\0') {
        if (tool_byte_buffer_append_cstr(&line, " symref-target:") != 0 || tool_byte_buffer_append_cstr(&line, symref_target) != 0) {
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

static int gitd_append_v2_ls_refs_response(GitRepo *repo, const GitdUploadRequest *upload, GitBuffer *out) {
    GitdRefList refs;
    unsigned char head_oid[CRYPTO_SHA1_DIGEST_SIZE];
    size_t i;
    int have_head = 0;
    int result = -1;

    rt_memset(&refs, 0, sizeof(refs));
    if (gitd_collect_refs(repo, &refs) != 0) goto done;
    if (repo->head_oid[0] != '\0' && git_parse_oid_hex_n(repo->head_oid, GIT_OBJECT_HEX_SIZE, head_oid) == 0) {
        have_head = 1;
    } else if (refs.count > 0U) {
        memcpy(head_oid, refs.refs[0].oid, sizeof(head_oid));
        have_head = 1;
    }
    if (have_head) {
        if (gitd_ls_refs_prefix_matches(upload, "HEAD") && gitd_append_v2_ref_line(out, head_oid, "HEAD", repo->head_ref, upload->ls_refs_symrefs) != 0) goto done;
        for (i = 0U; i < refs.count; ++i) {
            if (gitd_ls_refs_prefix_matches(upload, refs.refs[i].name) && gitd_append_v2_ref_line(out, refs.refs[i].oid, refs.refs[i].name, 0, upload->ls_refs_symrefs) != 0) goto done;
        }
    }
    if (tool_byte_buffer_append_cstr(out, "0000") != 0) goto done;
    result = 0;
done:
    gitd_ref_list_destroy(&refs);
    return result;
}

static int gitd_append_v2_bundle_uri_response(GitBuffer *out) {
    return tool_byte_buffer_append_cstr(out, "0000");
}

static int gitd_append_v2_object_info_response(GitRepo *repo, const GitPack *pack_cache, const GitdUploadRequest *upload, GitBuffer *out) {
    size_t index;

    if (!upload->object_info_size) return tool_byte_buffer_append_cstr(out, "0000");
    for (index = 0U; index < upload->object_info_oids.count; ++index) {
        int type = 0;
        unsigned char *data = 0;
        size_t size = 0U;

        if (git_read_object(repo, upload->object_info_oids.oids[index], pack_cache, &type, &data, &size) == 0) {
            char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
            char size_text[32];
            GitBuffer line;
            int append_result;

            git_format_oid_hex(upload->object_info_oids.oids[index], oid_hex);
            rt_unsigned_to_string(size, size_text, sizeof(size_text));
            rt_memset(&line, 0, sizeof(line));
            append_result = tool_byte_buffer_append_cstr(&line, oid_hex) != 0 ||
                            tool_byte_buffer_append_char(&line, ' ') != 0 ||
                            tool_byte_buffer_append_cstr(&line, size_text) != 0 ||
                            tool_byte_buffer_append_char(&line, '\n') != 0 ||
                            git_append_pkt_data(out, line.data, line.size) != 0 ? -1 : 0;
            git_buffer_destroy(&line);
            rt_free(data);
            if (append_result != 0) return -1;
        }
    }
    return tool_byte_buffer_append_cstr(out, "0000");
}

static int gitd_append_v2_fetch_response(GitBuffer *out, const GitdUploadRequest *upload, const GitBuffer *pack) {
    size_t pos = 0U;
    size_t i;

    if (upload->shallow_oids.count > 0U) {
        if (git_append_pkt_line(out, "shallow-info\n") != 0) return -1;
        for (i = 0U; i < upload->shallow_oids.count; ++i) {
            char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
            GitBuffer line;

            git_format_oid_hex(upload->shallow_oids.oids[i], oid_hex);
            rt_memset(&line, 0, sizeof(line));
            if (tool_byte_buffer_append_cstr(&line, "shallow ") != 0 || tool_byte_buffer_append_cstr(&line, oid_hex) != 0 || tool_byte_buffer_append_char(&line, '\n') != 0 || git_append_pkt_data(out, line.data, line.size) != 0) {
                git_buffer_destroy(&line);
                return -1;
            }
            git_buffer_destroy(&line);
        }
        if (tool_byte_buffer_append_cstr(out, "0001") != 0) return -1;
    }
    if (git_append_pkt_line(out, "packfile\n") != 0) return -1;
    while (pos < pack->size) {
        size_t chunk = pack->size - pos;
        GitBuffer payload;
        int result;

        if (chunk > GITD_SIDEBAND_CHUNK) chunk = GITD_SIDEBAND_CHUNK;
        rt_memset(&payload, 0, sizeof(payload));
        if (tool_byte_buffer_append_byte(&payload, 1U) != 0 || git_buffer_append(&payload, pack->data + pos, chunk) != 0) {
            git_buffer_destroy(&payload);
            return -1;
        }
        result = git_append_pkt_data(out, payload.data, payload.size);
        git_buffer_destroy(&payload);
        if (result != 0) return -1;
        pos += chunk;
    }
    return tool_byte_buffer_append_cstr(out, "0000");
}

static int gitd_object_exists(GitRepo *repo, const GitPack *pack_cache, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    int type = 0;
    unsigned char *data = 0;
    size_t size = 0U;

    if (git_read_object(repo, oid, pack_cache, &type, &data, &size) != 0) return 0;
    rt_free(data);
    return type >= GIT_OBJECT_COMMIT && type <= GIT_OBJECT_TAG;
}

static int gitd_append_v1_negotiation_response(GitRepo *repo, const GitPack *pack_cache, const GitdUploadRequest *upload, GitBuffer *out) {
    size_t index;
    int acknowledged = 0;

    for (index = 0U; index < upload->haves.count; ++index) {
        if (gitd_object_exists(repo, pack_cache, upload->haves.oids[index])) {
            char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
            GitBuffer line;
            int append_result;

            git_format_oid_hex(upload->haves.oids[index], oid_hex);
            rt_memset(&line, 0, sizeof(line));
            append_result = tool_byte_buffer_append_cstr(&line, "ACK ") != 0 ||
                            tool_byte_buffer_append_cstr(&line, oid_hex) != 0 ||
                            tool_byte_buffer_append_cstr(&line, " common\n") != 0 ||
                            git_append_pkt_data(out, line.data, line.size) != 0 ? -1 : 0;
            git_buffer_destroy(&line);
            if (append_result != 0) return -1;
            acknowledged = 1;
        }
    }
    if (!acknowledged && git_append_pkt_line(out, "NAK\n") != 0) return -1;
    return tool_byte_buffer_append_cstr(out, "0000");
}

static int gitd_handle_upload_pack_command(GitdTransport *transport, const GitdOptions *options, GitRepo *repo, GitdUploadRequest *upload, int v2) {
    GitPack pack_cache;
    GitOidList objects;
    GitOidList visited;
    GitOidList excluded;
    GitBuffer pack;
    GitBuffer response;
    int have_pack = 0;
    int result = -1;

    if (upload->command == GITD_UPLOAD_COMMAND_LS_REFS) {
        rt_memset(&response, 0, sizeof(response));
        if (gitd_append_v2_ls_refs_response(repo, upload, &response) != 0) {
            git_buffer_destroy(&response);
            return gitd_send_text(transport, 500, "cannot list refs\n");
        }
        result = gitd_send_body(transport, 200, "application/x-git-upload-pack-result", response.data, response.size);
        git_buffer_destroy(&response);
        return result;
    }
    if (upload->command == GITD_UPLOAD_COMMAND_BUNDLE_URI) {
        rt_memset(&response, 0, sizeof(response));
        result = gitd_append_v2_bundle_uri_response(&response) == 0 ? gitd_send_body(transport, 200, "application/x-git-upload-pack-result", response.data, response.size) : -1;
        git_buffer_destroy(&response);
        return result;
    }
    if (upload->command == GITD_UPLOAD_COMMAND_UNSUPPORTED) {
        return gitd_send_text(transport, 501, "unsupported protocol v2 command\n");
    }
    rt_memset(&pack_cache, 0, sizeof(pack_cache));
    rt_memset(&objects, 0, sizeof(objects));
    rt_memset(&visited, 0, sizeof(visited));
    rt_memset(&excluded, 0, sizeof(excluded));
    rt_memset(&pack, 0, sizeof(pack));
    rt_memset(&response, 0, sizeof(response));
    have_pack = git_load_pack_cache(repo, &pack_cache) == 0;
    if (upload->command == GITD_UPLOAD_COMMAND_OBJECT_INFO) {
        result = gitd_append_v2_object_info_response(repo, have_pack ? &pack_cache : 0, upload, &response) == 0 ? gitd_send_body(transport, 200, "application/x-git-upload-pack-result", response.data, response.size) : -1;
        goto done;
    }
    if (!upload->done && !v2) {
        result = gitd_append_v1_negotiation_response(repo, have_pack ? &pack_cache : 0, upload, &response) == 0 ? gitd_send_body(transport, 200, "application/x-git-upload-pack-result", response.data, response.size) : -1;
        goto done;
    }
    if (gitd_collect_excluded_haves(repo, have_pack ? &pack_cache : 0, &upload->haves, &excluded) != 0) goto done;
    if (gitd_collect_wanted_objects(repo, have_pack ? &pack_cache : 0, upload, excluded.count > 0U ? &excluded : 0, &objects, &visited) != 0 || objects.count > options->max_objects || upload->shallow_oids.count > options->max_shallows) goto done;
    if (gitd_build_pack(repo, have_pack ? &pack_cache : 0, &objects, &pack) != 0 || pack.size > options->max_pack_bytes) goto done;
    if (v2) {
        if (gitd_append_v2_fetch_response(&response, upload, &pack) != 0) goto done;
    } else if (gitd_append_sideband_pack(&response, &pack, upload->sideband) != 0) {
        goto done;
    }
    result = gitd_send_body(transport, 200, "application/x-git-upload-pack-result", response.data, response.size);
done:
    if (have_pack) git_pack_destroy(&pack_cache);
    git_oid_list_destroy(&objects);
    git_oid_list_destroy(&visited);
    git_oid_list_destroy(&excluded);
    git_buffer_destroy(&pack);
    git_buffer_destroy(&response);
    if (result != 0) return gitd_send_text(transport, 500, "cannot build upload pack\n");
    return 0;
}

static int gitd_handle_upload_pack(GitdTransport *transport, const GitdOptions *options, const GitdRequest *request, const GitBuffer *body) {
    char repo_path[GIT_PATH_CAPACITY];
    GitRepo repo;
    GitdUploadRequest upload;
    GitBuffer decoded_body;
    const GitBuffer *payload;
    int v2;
    int result;

    if (rt_strcmp(request->method, "POST") != 0) return gitd_send_text(transport, 405, "method not allowed\n");
    if (!git_header_value_contains((const unsigned char *)request->content_type, rt_strlen(request->content_type), "application/x-git-upload-pack-request")) return gitd_send_text(transport, 415, "expected git-upload-pack request\n");
    if (gitd_strip_suffix(request->path, "/git-upload-pack", repo_path, sizeof(repo_path)) != 0 || gitd_repo_from_path(options, repo_path, &repo) != 0) return gitd_send_text(transport, 404, "repository not found\n");
    if (gitd_request_body_payload(options, request, body, &decoded_body, &payload) != 0) return gitd_send_text(transport, 415, "unsupported request content encoding\n");
    v2 = git_header_value_contains((const unsigned char *)request->git_protocol, rt_strlen(request->git_protocol), "version=2");
    if ((v2 ? gitd_parse_upload_pack_v2_request(options, payload, &upload) : gitd_parse_upload_pack_v1_request(options, payload, &upload)) != 0) {
        git_buffer_destroy(&decoded_body);
        return gitd_send_text(transport, 400, "malformed upload-pack request\n");
    }
    result = gitd_handle_upload_pack_command(transport, options, &repo, &upload, v2);
    gitd_upload_request_destroy(&upload);
    git_buffer_destroy(&decoded_body);
    return result;
}

static int gitd_parse_receive_pack_request(const GitdOptions *options, const GitBuffer *body, GitdReceiveRequest *receive) {
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
        if (gitd_receive_request_push(receive, &command) != 0 || receive->count > options->max_commands) return -1;
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

static int gitd_lock_path_for(const char *path, char *lock_path, size_t lock_path_size) {
    size_t length = rt_strlen(path);

    if (length + 6U >= lock_path_size) return -1;
    rt_copy_string(lock_path, lock_path_size, path);
    rt_copy_string(lock_path + length, lock_path_size - length, ".lock");
    return 0;
}

static int gitd_write_file_locked(const char *path, const void *data, size_t size, unsigned int mode) {
    char lock_path[GIT_PATH_CAPACITY];
    char parent[GIT_PATH_CAPACITY];
    int fd = -1;
    int result = -1;

    if (gitd_lock_path_for(path, lock_path, sizeof(lock_path)) != 0 || git_copy(parent, sizeof(parent), path) != 0 || git_path_parent(parent) != 0 || git_make_directory_chain(parent) != 0) return -1;
    fd = platform_open_create_exclusive(lock_path, mode);
    if (fd < 0) return -1;
    if (rt_write_all(fd, data, size) != 0) goto done;
    if (platform_close(fd) != 0) {
        fd = -1;
        goto done;
    }
    fd = -1;
    if (platform_rename_path(lock_path, path) != 0) goto done;
    result = 0;
done:
    if (fd >= 0) (void)platform_close(fd);
    if (result != 0) (void)platform_remove_file(lock_path);
    return result;
}

static int gitd_write_ref_oid_locked(GitRepo *repo, const GitdReceiveCommand *command) {
    char path[GIT_PATH_CAPACITY];
    char text[GIT_OBJECT_HEX_SIZE + 2U];
    unsigned char current_oid[CRYPTO_SHA1_DIGEST_SIZE];
    int exists = 0;

    if (gitd_current_ref_oid(repo, command->ref_name, current_oid, &exists) != 0) return -1;
    if (exists != !gitd_oid_is_zero(command->old_oid)) return -1;
    if (exists && !git_oid_equal(current_oid, command->old_oid)) return -1;
    if (git_join(path, sizeof(path), repo->git_dir, command->ref_name) != 0) return -1;
    git_format_oid_hex(command->new_oid, text);
    text[GIT_OBJECT_HEX_SIZE] = '\n';
    text[GIT_OBJECT_HEX_SIZE + 1U] = '\0';
    return gitd_write_file_locked(path, text, GIT_OBJECT_HEX_SIZE + 1U, 0644U);
}

static int gitd_delete_packed_ref_locked(GitRepo *repo, const char *ref_name, int *deleted_out) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;
    GitBuffer out;
    int deleted = 0;
    int result = -1;

    *deleted_out = 0;
    rt_memset(&out, 0, sizeof(out));
    if (git_join(path, sizeof(path), repo->git_dir, "packed-refs") != 0 || git_read_file(path, &data, &size) != 0) return 0;
    while (pos < size) {
        size_t start = pos;
        size_t end;
        size_t line_end;
        int matches = 0;

        while (pos < size && data[pos] != '\n') pos += 1U;
        end = pos;
        if (pos < size) pos += 1U;
        line_end = pos;
        while (end > start && data[end - 1U] == '\r') end -= 1U;
        if (end > start + GIT_OBJECT_HEX_SIZE + 1U && data[start] != '#' && data[start] != '^' && data[start + GIT_OBJECT_HEX_SIZE] == ' ') {
            size_t ref_length = end - start - GIT_OBJECT_HEX_SIZE - 1U;

            if (ref_length == rt_strlen(ref_name) && memcmp(data + start + GIT_OBJECT_HEX_SIZE + 1U, ref_name, ref_length) == 0) matches = 1;
        }
        if (matches) {
            deleted = 1;
            if (pos < size && data[pos] == '^') {
                while (pos < size && data[pos] != '\n') pos += 1U;
                if (pos < size) pos += 1U;
            }
            continue;
        }
        if (git_buffer_append(&out, data + start, line_end - start) != 0) goto done;
    }
    if (deleted && gitd_write_file_locked(path, out.data, out.size, 0644U) != 0) goto done;
    *deleted_out = deleted;
    result = 0;
done:
    git_buffer_destroy(&out);
    rt_free(data);
    return result;
}

static int gitd_delete_ref_locked(GitRepo *repo, const GitdReceiveCommand *command) {
    char path[GIT_PATH_CAPACITY];
    char lock_path[GIT_PATH_CAPACITY];
    unsigned char current_oid[CRYPTO_SHA1_DIGEST_SIZE];
    PlatformDirEntry entry;
    int exists = 0;
    int deleted = 0;
    int fd = -1;
    int result = -1;

    if (gitd_current_ref_oid(repo, command->ref_name, current_oid, &exists) != 0 || !exists || !git_oid_equal(current_oid, command->old_oid)) return -1;
    if (git_join(path, sizeof(path), repo->git_dir, command->ref_name) != 0 || gitd_lock_path_for(path, lock_path, sizeof(lock_path)) != 0 || git_ensure_parent_directory(lock_path) != 0) return -1;
    fd = platform_open_create_exclusive(lock_path, 0644U);
    if (fd < 0) return -1;
    if (platform_close(fd) != 0) {
        fd = -1;
        goto done;
    }
    fd = -1;
    if (platform_remove_file(path) == 0) deleted = 1;
    if (platform_get_path_info(path, &entry) == 0 && !entry.is_dir) goto done;
    if (gitd_delete_packed_ref_locked(repo, command->ref_name, &deleted) != 0) goto done;
    if (platform_get_path_info(path, &entry) == 0 && !entry.is_dir) goto done;
    result = 0;
done:
    if (fd >= 0) (void)platform_close(fd);
    (void)platform_remove_file(lock_path);
    return result;
}

static const char *gitd_validate_receive_command(const GitdOptions *options, GitRepo *repo, const GitPack *pack_cache, const GitdReceiveCommand *command) {
    unsigned char current_oid[CRYPTO_SHA1_DIGEST_SIZE];
    int exists = 0;
    int type = 0;
    unsigned char *data = 0;
    size_t size = 0U;

    if (options->read_only) return "push disabled";
    if (!gitd_ref_is_safe(command->ref_name)) return "unsafe ref name";
    if (gitd_oid_is_zero(command->new_oid) && !options->allow_delete_refs) return "delete denied";
    if (!gitd_ref_is_branch(command->ref_name)) {
        if (rt_strncmp(command->ref_name, "refs/tags/", 10U) == 0) {
            if (!options->allow_tags) return "tag update denied";
        } else if (rt_strncmp(command->ref_name, "refs/notes/", 11U) == 0) {
            if (!options->allow_notes) return "notes update denied";
        } else if (!options->allow_custom_refs) {
            return "ref namespace denied";
        }
    }
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

static int gitd_apply_receive_commands(const GitdOptions *options, GitRepo *repo, const GitPack *pack_cache, const GitdReceiveRequest *receive, const char **error_out, const char **error_ref_out) {
    size_t i;

    for (i = 0U; i < receive->count; ++i) {
        const char *error = gitd_validate_receive_command(options, repo, pack_cache, &receive->commands[i]);
        if (error != 0) {
            *error_out = error;
            *error_ref_out = receive->commands[i].ref_name;
            return -1;
        }
    }
    for (i = 0U; i < receive->count; ++i) {
        if (gitd_oid_is_zero(receive->commands[i].new_oid)) {
            if (gitd_delete_ref_locked(repo, &receive->commands[i]) != 0) {
                *error_out = "cannot delete ref";
                *error_ref_out = receive->commands[i].ref_name;
                return -1;
            }
            continue;
        }
        if (gitd_write_ref_oid_locked(repo, &receive->commands[i]) != 0) {
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

static int gitd_send_receive_status(GitdTransport *transport, const GitdReceiveRequest *receive, const char *error, const char *error_ref) {
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
    result = gitd_send_body(transport, 200, "application/x-git-receive-pack-result", response.data, response.size);
done:
    git_buffer_destroy(&payload);
    git_buffer_destroy(&status);
    git_buffer_destroy(&response);
    return result;
}

static int gitd_handle_receive_pack(GitdTransport *transport, const GitdOptions *options, const GitdRequest *request, const GitBuffer *body) {
    char repo_path[GIT_PATH_CAPACITY];
    GitRepo repo;
    GitdReceiveRequest receive;
    GitPack received_pack;
    GitBuffer decoded_body;
    const GitBuffer *payload;
    const char *error = 0;
    const char *error_ref = 0;
    int result;

    if (rt_strcmp(request->method, "POST") != 0) return gitd_send_text(transport, 405, "method not allowed\n");
    if (!git_header_value_contains((const unsigned char *)request->content_type, rt_strlen(request->content_type), "application/x-git-receive-pack-request")) return gitd_send_text(transport, 415, "expected git-receive-pack request\n");
    if (gitd_strip_suffix(request->path, "/git-receive-pack", repo_path, sizeof(repo_path)) != 0 || gitd_repo_from_path(options, repo_path, &repo) != 0) return gitd_send_text(transport, 404, "repository not found\n");
    if (gitd_request_body_payload(options, request, body, &decoded_body, &payload) != 0) return gitd_send_text(transport, 415, "unsupported request content encoding\n");
    rt_memset(&receive, 0, sizeof(receive));
    rt_memset(&received_pack, 0, sizeof(received_pack));
    if (gitd_parse_receive_pack_request(options, payload, &receive) != 0) {
        gitd_receive_request_destroy(&receive);
        git_buffer_destroy(&decoded_body);
        return gitd_send_text(transport, 400, "malformed receive-pack request\n");
    }
    if (receive.pack_size > options->max_pack_bytes) {
        error = "pack too large";
        error_ref = receive.count > 0U ? receive.commands[0].ref_name : "refs/heads/unknown";
    } else
    if (gitd_receive_request_is_delete_only(&receive)) {
        if (gitd_apply_receive_commands(options, &repo, 0, &receive, &error, &error_ref) != 0) {
            /* error fields are set by validation. */
        }
    } else if (gitd_store_received_pack(&repo, &receive, &received_pack) != 0) {
        error = "unpack failed";
        error_ref = receive.count > 0U ? receive.commands[0].ref_name : "refs/heads/unknown";
    } else if (received_pack.count > options->max_objects) {
        error = "too many objects";
        error_ref = receive.count > 0U ? receive.commands[0].ref_name : "refs/heads/unknown";
    } else if (gitd_apply_receive_commands(options, &repo, &received_pack, &receive, &error, &error_ref) != 0) {
        /* error fields are set by validation. */
    }
    result = gitd_send_receive_status(transport, &receive, error, error_ref);
    git_pack_destroy(&received_pack);
    gitd_receive_request_destroy(&receive);
    git_buffer_destroy(&decoded_body);
    return result;
}

static int gitd_dispatch_request(GitdTransport *transport, const GitdOptions *options, const GitdRequest *request, const GitBuffer *body) {
    int result;

    if (rt_strcmp(request->method, "OPTIONS") == 0) {
        result = gitd_send_options(transport);
    } else if (rt_strcmp(request->path, "/health") == 0 || rt_strcmp(request->path, "/_status") == 0) {
        result = gitd_send_text(transport, 200, "ok\n");
    } else if (rt_strlen(request->path) >= 10U && rt_strcmp(request->path + rt_strlen(request->path) - 10U, "/info/refs") == 0) {
        result = gitd_handle_info_refs(transport, options, request);
    } else if (rt_strlen(request->path) >= 16U && rt_strcmp(request->path + rt_strlen(request->path) - 16U, "/git-upload-pack") == 0) {
        result = gitd_handle_upload_pack(transport, options, request, body);
    } else if (rt_strlen(request->path) >= 17U && rt_strcmp(request->path + rt_strlen(request->path) - 17U, "/git-receive-pack") == 0) {
        result = gitd_handle_receive_pack(transport, options, request, body);
    } else {
        result = gitd_send_text(transport, 404, "not found\n");
    }
    return result;
}

static void gitd_connection_destroy(GitdConnection *connection) {
    if (connection == 0) return;
    if (connection->transport.fd >= 0) {
        (void)rt_io_loop_remove(&connection->server->loop, connection->transport.fd);
        gitd_transport_close(&connection->transport);
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
        (void)gitd_send_text(&connection->transport, 400, "bad request\n");
        gitd_connection_destroy(connection);
        return;
    }
    if (!complete) {
        return;
    }
    (void)rt_io_loop_remove(&connection->server->loop, fd);
    (void)gitd_dispatch_request(&connection->transport, &connection->server->options, &connection->request, &connection->body);
    connection->server->handled_connections += 1U;
    if (connection->server->options.once) {
        rt_io_loop_stop(&connection->server->loop);
    }
    gitd_transport_close(&connection->transport);
    gitd_connection_destroy(connection);
}

static int gitd_connection_add(GitdServer *server, int client_fd) {
    GitdConnection *connection;

    connection = (GitdConnection *)rt_malloc(sizeof(*connection));
    if (connection == 0) {
        (void)platform_close(client_fd);
        return -1;
    }
    rt_memset(connection, 0, sizeof(*connection));
    connection->server = server;
    connection->transport.fd = client_fd;
    if (server->tls_config.enabled) {
        Tls13ServerCredentials credentials;

        credentials.cert_der = server->tls_config.cert_der;
        credentials.cert_der_len = server->tls_config.cert_der_len;
        credentials.rsa_key = &server->tls_config.rsa_key;
        connection->transport.use_tls = 1;
        tls13_server_init(&connection->transport.tls, client_fd, &credentials, 30000U);
        if (tls13_server_handshake(&connection->transport.tls) != 0) {
            connection->transport.fd = -1;
            (void)platform_close(client_fd);
            rt_free(connection);
            return -1;
        }
    }
    if (rt_io_loop_add(&server->loop, client_fd, RT_IO_READ, gitd_connection_ready, connection) != 0) {
        gitd_transport_close(&connection->transport);
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
    options->max_body_size = GITD_DEFAULT_MAX_BODY_SIZE;
    options->max_wants = GITD_DEFAULT_MAX_WANTS;
    options->max_haves = GITD_DEFAULT_MAX_HAVES;
    options->max_shallows = GITD_DEFAULT_MAX_SHALLOWS;
    options->max_ref_prefixes = GITD_DEFAULT_MAX_REF_PREFIXES;
    options->max_commands = GITD_DEFAULT_MAX_COMMANDS;
    options->max_objects = GITD_DEFAULT_MAX_OBJECTS;
    options->max_pack_bytes = GITD_DEFAULT_MAX_PACK_BYTES;
    options->allow_delete_refs = 1;
    options->allow_tags = 1;
    options->allow_notes = 1;
    options->allow_custom_refs = 1;
    tool_opt_init(&opt, argc, argv, tool_base_name(argv[0]), "[-b HOST] [-p PORT] [-r REPO_ROOT] [--tls-cert CERT --tls-key KEY] [--once] [-q] [--read-only] [--branches-only] [--no-delete-refs] [--max-body BYTES]");
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
        } else if (rt_strcmp(opt.flag, "--tls-cert") == 0) {
            if (tool_opt_require_value(&opt) != 0) return -1;
            rt_copy_string(options->tls_cert_path, sizeof(options->tls_cert_path), opt.value);
        } else if (rt_strcmp(opt.flag, "--tls-key") == 0) {
            if (tool_opt_require_value(&opt) != 0) return -1;
            rt_copy_string(options->tls_key_path, sizeof(options->tls_key_path), opt.value);
        } else if (rt_strcmp(opt.flag, "--once") == 0) {
            options->once = 1;
        } else if (rt_strcmp(opt.flag, "--read-only") == 0) {
            options->read_only = 1;
        } else if (rt_strcmp(opt.flag, "--branches-only") == 0) {
            options->allow_tags = 0;
            options->allow_notes = 0;
            options->allow_custom_refs = 0;
        } else if (rt_strcmp(opt.flag, "--no-delete-refs") == 0) {
            options->allow_delete_refs = 0;
        } else if (rt_strcmp(opt.flag, "--max-body") == 0) {
            if (tool_opt_require_value(&opt) != 0 || gitd_parse_size_option(opt.value, &options->max_body_size) != 0) return -1;
        } else if (rt_strcmp(opt.flag, "--max-wants") == 0) {
            if (tool_opt_require_value(&opt) != 0 || gitd_parse_size_option(opt.value, &options->max_wants) != 0) return -1;
        } else if (rt_strcmp(opt.flag, "--max-haves") == 0) {
            if (tool_opt_require_value(&opt) != 0 || gitd_parse_size_option(opt.value, &options->max_haves) != 0) return -1;
        } else if (rt_strcmp(opt.flag, "--max-ref-prefixes") == 0) {
            if (tool_opt_require_value(&opt) != 0 || gitd_parse_size_option(opt.value, &options->max_ref_prefixes) != 0) return -1;
        } else if (rt_strcmp(opt.flag, "--max-commands") == 0) {
            if (tool_opt_require_value(&opt) != 0 || gitd_parse_size_option(opt.value, &options->max_commands) != 0) return -1;
        } else if (rt_strcmp(opt.flag, "--max-objects") == 0) {
            if (tool_opt_require_value(&opt) != 0 || gitd_parse_size_option(opt.value, &options->max_objects) != 0) return -1;
        } else if (rt_strcmp(opt.flag, "--max-pack-bytes") == 0) {
            if (tool_opt_require_value(&opt) != 0 || gitd_parse_size_option(opt.value, &options->max_pack_bytes) != 0) return -1;
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
    if ((options->tls_cert_path[0] == '\0') != (options->tls_key_path[0] == '\0')) return -1;
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
    (void)gitd_connection_add(server, client_fd);
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
    if (gitd_load_tls_config(&server.options, &server.tls_config) != 0) {
        tool_write_error("gitd", "cannot load TLS certificate/key", 0);
        return 1;
    }
    if (platform_open_tcp_listener(server.options.bind_host, server.options.port, &server.listener_fd) != 0) {
        tool_write_error("gitd", "cannot listen on port", 0);
        gitd_tls_config_destroy(&server.tls_config);
        return 1;
    }
    if (!server.options.quiet) {
        char port_text[32];
        rt_unsigned_to_string(server.options.port, port_text, sizeof(port_text));
        rt_write_cstr(2, server.tls_config.enabled ? "gitd listening on https://" : "gitd listening on http://");
        rt_write_cstr(2, server.options.bind_host);
        rt_write_cstr(2, ":");
        rt_write_cstr(2, port_text);
        rt_write_cstr(2, "/ from ");
        rt_write_line(2, server.options.repo_root);
    }
    if (gitd_run_server(&server) != 0) {
        (void)platform_close(server.listener_fd);
        gitd_tls_config_destroy(&server.tls_config);
        return 1;
    }
    (void)platform_close(server.listener_fd);
    gitd_tls_config_destroy(&server.tls_config);
    return 0;
}