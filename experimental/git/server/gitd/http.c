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

static void gitd_log_message(const GitdOptions *options, const char *level, const char *message, const char *detail) {
    if (options != 0 && options->quiet) return;
    (void)rt_write_cstr(2, "gitd ");
    (void)rt_write_cstr(2, level != 0 ? level : "info");
    (void)rt_write_cstr(2, ": ");
    (void)rt_write_cstr(2, message != 0 ? message : "event");
    if (detail != 0 && detail[0] != '\0') {
        (void)rt_write_cstr(2, ": ");
        (void)rt_write_cstr(2, detail);
    }
    (void)rt_write_char(2, '\n');
}

static void gitd_log_request_result(const GitdOptions *options, const GitdRequest *request, const GitdTransport *transport, int result) {
    int status = transport != 0 ? transport->last_status : 0;

    if (options != 0 && options->quiet) return;
    if (status == 0 && result != 0) status = 500;
    if (status == 0) status = 0;
    (void)rt_write_cstr(2, "gitd request: ");
    (void)rt_write_cstr(2, request != 0 ? request->method : "?");
    (void)rt_write_char(2, ' ');
    (void)rt_write_cstr(2, request != 0 ? request->target : "?");
    (void)rt_write_cstr(2, " -> ");
    (void)rt_write_uint(2, (unsigned long long)status);
    (void)rt_write_cstr(2, " response-bytes=");
    (void)rt_write_uint(2, (unsigned long long)(transport != 0 ? transport->last_response_bytes : 0U));
    if (result != 0) (void)rt_write_cstr(2, " dispatch-error");
    (void)rt_write_char(2, '\n');
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
        tool_byte_buffer_append_cstr(header, "\r\nAccess-Control-Allow-Methods: GET, HEAD, POST, OPTIONS") != 0 ||
        tool_byte_buffer_append_cstr(header, "\r\nAccess-Control-Allow-Headers: content-type, authorization") != 0 ||
        tool_byte_buffer_append_cstr(header, "\r\nAccess-Control-Max-Age: 600") != 0 ||
        tool_byte_buffer_append_cstr(header, "\r\nAllow: GET, HEAD, POST, OPTIONS") != 0 ||
        tool_byte_buffer_append_cstr(header, "\r\nX-Content-Type-Options: nosniff") != 0 ||
        tool_byte_buffer_append_cstr(header, "\r\nConnection: close\r\n\r\n") != 0) {
        return -1;
    }
    return 0;
}

static int gitd_send_body(GitdTransport *transport, int status, const char *content_type, const unsigned char *body, size_t body_size) {
    GitBuffer header;
    int result = -1;

    if (transport != 0) {
        transport->last_status = status;
        transport->last_response_bytes = body_size;
    }
    rt_memset(&header, 0, sizeof(header));
    if (gitd_header_append_common(&header, status, content_type, body_size) != 0) {
        goto done;
    }
    if (gitd_transport_write_all(transport, header.data, header.size) != 0) {
        goto done;
    }
    if (body_size > 0U && transport != 0 && !transport->head_only && gitd_transport_write_all(transport, body, body_size) != 0) {
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
