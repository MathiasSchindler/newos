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

static int git_http_emit_body(GitBuffer *response, GitHttpBodyCallback callback, void *callback_user_data, const unsigned char *data, size_t size) {
    if (size == 0U) {
        return 0;
    }
    if (response != 0 && git_buffer_append(response, data, size) != 0) {
        return -1;
    }
    if (callback != 0 && callback(data, size, callback_user_data) != 0) {
        return -1;
    }
    return 0;
}

static int git_http_process_plain_body(GitBuffer *response, GitHttpBodyCallback callback, void *callback_user_data, const unsigned char *data, size_t size, int has_content_length, size_t content_length, size_t *body_seen) {
    size_t emit_size = size;

    if (has_content_length) {
        if (*body_seen >= content_length) {
            return 0;
        }
        if (emit_size > content_length - *body_seen) {
            emit_size = content_length - *body_seen;
        }
    }
    if (git_http_emit_body(response, callback, callback_user_data, data, emit_size) != 0) {
        return -1;
    }
    *body_seen += emit_size;
    return 0;
}

static int git_http_process_chunked_body(GitBuffer *response, GitHttpBodyCallback callback, void *callback_user_data, GitBuffer *pending, const unsigned char *data, size_t size, int *done) {
    size_t consumed = 0U;

    if (*done) {
        return 0;
    }
    if (git_buffer_append(pending, data, size) != 0) {
        return -1;
    }
    while (consumed < pending->size) {
        size_t line_end = consumed;
        size_t chunk_header_size;
        size_t chunk_size;

        while (line_end < pending->size && pending->data[line_end] != '\n') {
            line_end += 1U;
        }
        if (line_end >= pending->size) {
            break;
        }
        if (git_parse_chunk_size(pending->data + consumed, pending->size - consumed, &chunk_header_size, &chunk_size) != 0) {
            return -1;
        }
        if (chunk_size == 0U) {
            consumed = pending->size;
            *done = 1;
            break;
        }
        if (pending->size - consumed - chunk_header_size < chunk_size + 2U) {
            break;
        }
        consumed += chunk_header_size;
        if (git_http_emit_body(response, callback, callback_user_data, pending->data + consumed, chunk_size) != 0) {
            return -1;
        }
        consumed += chunk_size;
        if (pending->data[consumed] == '\r') {
            consumed += 1U;
        }
        if (consumed >= pending->size || pending->data[consumed] != '\n') {
            return -1;
        }
        consumed += 1U;
    }
    git_buffer_discard_prefix(pending, consumed);
    return 0;
}

static int git_http_header_value_safe(const char *text) {
    size_t i;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    for (i = 0U; text[i] != '\0'; ++i) {
        if (text[i] == '\r' || text[i] == '\n') {
            return 0;
        }
    }
    return 1;
}

static int git_http_base64_append(GitBuffer *out, const unsigned char *data, size_t size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0U;

    while (i < size) {
        unsigned int b0 = data[i++];
        int have_b1 = i < size;
        unsigned int b1 = have_b1 ? data[i++] : 0U;
        int have_b2 = i < size;
        unsigned int b2 = have_b2 ? data[i++] : 0U;

        if (tool_byte_buffer_append_char(out, alphabet[(b0 >> 2U) & 0x3fU]) != 0 ||
            tool_byte_buffer_append_char(out, alphabet[((b0 << 4U) | (b1 >> 4U)) & 0x3fU]) != 0 ||
            tool_byte_buffer_append_char(out, have_b1 ? alphabet[((b1 << 2U) | (b2 >> 6U)) & 0x3fU] : '=') != 0 ||
            tool_byte_buffer_append_char(out, have_b2 ? alphabet[b2 & 0x3fU] : '=') != 0) {
            return -1;
        }
    }
    return 0;
}

static int git_credential_output_value(const unsigned char *data, size_t size, const char *key, char *buffer, size_t buffer_size) {
    size_t pos = 0U;
    size_t key_length = rt_strlen(key);

    while (pos < size) {
        size_t start = pos;
        size_t end;

        while (pos < size && data[pos] != '\n') {
            pos += 1U;
        }
        end = pos;
        if (pos < size) pos += 1U;
        if (end > start && data[end - 1U] == '\r') {
            end -= 1U;
        }
        if (end > start + key_length + 1U && memcmp(data + start, key, key_length) == 0 && data[start + key_length] == '=') {
            size_t value_length = end - start - key_length - 1U;
            if (value_length >= buffer_size) {
                return -1;
            }
            memcpy(buffer, data + start + key_length + 1U, value_length);
            buffer[value_length] = '\0';
            return 0;
        }
    }
    return -1;
}

static int git_http_credential_helper_auth(const GitUrl *url, char *buffer, size_t buffer_size) {
    const char *helper = platform_getenv("GIT_CREDENTIAL_HELPER");
    int stdin_pipe[2] = { -1, -1 };
    int stdout_pipe[2] = { -1, -1 };
    char *helper_argv[3];
    GitBuffer query;
    GitBuffer output;
    GitBuffer raw;
    GitBuffer encoded;
    char port_text[32];
    char read_buffer[512];
    char username[160];
    char password[256];
    int pid;
    int status = 1;
    long nread;
    int result = -1;

    if (helper == 0 || helper[0] == '\0') {
        return -1;
    }
    rt_memset(&query, 0, sizeof(query));
    rt_memset(&output, 0, sizeof(output));
    rt_memset(&raw, 0, sizeof(raw));
    rt_memset(&encoded, 0, sizeof(encoded));
    rt_unsigned_to_string(url->port, port_text, sizeof(port_text));
    if (tool_byte_buffer_append_cstr(&query, "protocol=") != 0 ||
        tool_byte_buffer_append_cstr(&query, url->scheme == GIT_SCHEME_HTTPS ? "https" : "http") != 0 ||
        tool_byte_buffer_append_cstr(&query, "\nhost=") != 0 ||
        tool_byte_buffer_append_cstr(&query, url->host) != 0 ||
        tool_byte_buffer_append_cstr(&query, "\npath=") != 0 ||
        tool_byte_buffer_append_cstr(&query, url->path) != 0 ||
        tool_byte_buffer_append_cstr(&query, "\nwwwauth[]=Basic\n\n") != 0) {
        goto done;
    }
    if (platform_create_pipe(stdin_pipe) != 0 || platform_create_pipe(stdout_pipe) != 0) {
        goto done;
    }
    helper_argv[0] = (char *)helper;
    helper_argv[1] = "get";
    helper_argv[2] = 0;
    if (platform_spawn_process(helper_argv, stdin_pipe[0], stdout_pipe[1], 0, 0, 0, &pid) != 0) {
        goto done;
    }
    (void)platform_close(stdin_pipe[0]);
    stdin_pipe[0] = -1;
    (void)platform_close(stdout_pipe[1]);
    stdout_pipe[1] = -1;
    if (rt_write_all(stdin_pipe[1], query.data, query.size) != 0) {
        goto wait_done;
    }
    (void)platform_close(stdin_pipe[1]);
    stdin_pipe[1] = -1;
    while ((nread = platform_read(stdout_pipe[0], read_buffer, sizeof(read_buffer))) > 0) {
        if (output.size + (size_t)nread > 4096U || git_buffer_append(&output, read_buffer, (size_t)nread) != 0) {
            goto wait_done;
        }
    }
wait_done:
    (void)platform_wait_process(pid, &status);
    if (status != 0 || git_credential_output_value(output.data, output.size, "username", username, sizeof(username)) != 0 || git_credential_output_value(output.data, output.size, "password", password, sizeof(password)) != 0) {
        goto done;
    }
    if (tool_byte_buffer_append_cstr(&raw, username) != 0 || tool_byte_buffer_append_char(&raw, ':') != 0 || tool_byte_buffer_append_cstr(&raw, password) != 0) {
        goto done;
    }
    if (tool_byte_buffer_append_cstr(&encoded, "Basic ") != 0 || git_http_base64_append(&encoded, raw.data, raw.size) != 0 || encoded.size >= buffer_size) {
        goto done;
    }
    memcpy(buffer, encoded.data, encoded.size);
    buffer[encoded.size] = '\0';
    result = 0;
done:
    if (stdin_pipe[0] >= 0) (void)platform_close(stdin_pipe[0]);
    if (stdin_pipe[1] >= 0) (void)platform_close(stdin_pipe[1]);
    if (stdout_pipe[0] >= 0) (void)platform_close(stdout_pipe[0]);
    if (stdout_pipe[1] >= 0) (void)platform_close(stdout_pipe[1]);
    git_buffer_destroy(&query);
    git_buffer_destroy(&output);
    git_buffer_destroy(&raw);
    git_buffer_destroy(&encoded);
    return result;
}

static int git_http_request_stream_ex(const GitUrl *url, const char *method, const char *accept, const char *content_type, const char *git_protocol, const unsigned char *body, size_t body_size, GitBuffer *response, GitHttpBodyCallback callback, void *callback_user_data) {
    GitHttpConnection connection;
    GitBuffer header;
    GitBuffer chunk_pending;
    char request[4096];
    char length_text[32];
    size_t request_length = 0U;
    char read_buffer[8192];
    long bytes_read;
    int saw_headers = 0;
    int status_code = 0;
    int chunked = 0;
    int chunk_done = 0;
    int has_content_length = 0;
    size_t content_length = 0U;
    size_t body_seen = 0U;
    const char *authorization = platform_getenv("GIT_HTTP_AUTHORIZATION");
    const char *bearer_token = platform_getenv("GIT_HTTPS_TOKEN");
    char helper_authorization[512];
    int result = -1;

    rt_memset(&header, 0, sizeof(header));
    rt_memset(&chunk_pending, 0, sizeof(chunk_pending));
    if (response != 0) {
        rt_memset(response, 0, sizeof(*response));
    }
    if (tool_http_connection_connect(&connection, url->host, url->port, url->scheme == GIT_SCHEME_HTTPS) != 0) {
        return -1;
    }
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, method);
    request_length = tool_buffer_append_char(request, sizeof(request), request_length, ' ');
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, url->path[0] != '\0' ? url->path : "/");
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, " HTTP/1.1\r\nHost: ");
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, url->host);
    if (url->port != tool_http_default_port(url->scheme == GIT_SCHEME_HTTPS)) {
        rt_unsigned_to_string(url->port, length_text, sizeof(length_text));
        request_length = tool_buffer_append_char(request, sizeof(request), request_length, ':');
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, length_text);
    }
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\nUser-Agent: newos-git/0.1\r\nAccept: ");
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, accept != 0 ? accept : "*/*");
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\nConnection: close\r\n");
    if (git_http_header_value_safe(git_protocol)) {
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "Git-Protocol: ");
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, git_protocol);
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\n");
    }
    helper_authorization[0] = '\0';
    if (!git_http_header_value_safe(authorization) && !git_http_header_value_safe(bearer_token)) {
        (void)git_http_credential_helper_auth(url, helper_authorization, sizeof(helper_authorization));
    }
    if (git_http_header_value_safe(authorization)) {
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "Authorization: ");
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, authorization);
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\n");
    } else if (git_http_header_value_safe(bearer_token)) {
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "Authorization: Bearer ");
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, bearer_token);
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\n");
    } else if (git_http_header_value_safe(helper_authorization)) {
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "Authorization: ");
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, helper_authorization);
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\n");
    }
    if (content_type != 0) {
        rt_unsigned_to_string(body_size, length_text, sizeof(length_text));
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "Content-Type: ");
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, content_type);
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\nContent-Length: ");
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, length_text);
        request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\n");
    }
    request_length = tool_buffer_append_cstr(request, sizeof(request), request_length, "\r\n");
    if (request_length >= sizeof(request) || tool_http_connection_write_all(&connection, request, request_length) != 0 || (body_size > 0U && tool_http_connection_write_all(&connection, body, body_size) != 0)) {
        goto done;
    }
    while ((bytes_read = tool_http_connection_read(&connection, read_buffer, sizeof(read_buffer))) > 0) {
        if (!saw_headers) {
            size_t header_offset = 0U;
            if (git_buffer_append(&header, read_buffer, (size_t)bytes_read) != 0) {
                goto done;
            }
            if (tool_find_http_header_end((const char *)header.data, header.size, &header_offset) != 0) {
                continue;
            }
            status_code = git_http_status_code(header.data, header_offset);
            if (status_code < 200 || status_code >= 300 || git_parse_headers(header.data, header_offset, &chunked, &content_length, &has_content_length) != 0) {
                goto done;
            }
            saw_headers = 1;
            if (chunked) {
                if (git_http_process_chunked_body(response, callback, callback_user_data, &chunk_pending, header.data + header_offset, header.size - header_offset, &chunk_done) != 0) {
                    goto done;
                }
            } else if (git_http_process_plain_body(response, callback, callback_user_data, header.data + header_offset, header.size - header_offset, has_content_length, content_length, &body_seen) != 0) {
                goto done;
            }
            header.size = 0U;
        } else if (chunked) {
            if (git_http_process_chunked_body(response, callback, callback_user_data, &chunk_pending, (const unsigned char *)read_buffer, (size_t)bytes_read, &chunk_done) != 0) {
                goto done;
            }
        } else if (git_http_process_plain_body(response, callback, callback_user_data, (const unsigned char *)read_buffer, (size_t)bytes_read, has_content_length, content_length, &body_seen) != 0) {
            goto done;
        }
    }
    if (bytes_read < 0 || !saw_headers || (chunked && !chunk_done) || (has_content_length && body_seen < content_length)) {
        goto done;
    }
    result = 0;
done:
    tool_http_connection_close(&connection);
    git_buffer_destroy(&header);
    git_buffer_destroy(&chunk_pending);
    if (result != 0 && response != 0) {
        git_buffer_destroy(response);
    }
    return result;
}

static int git_http_request_stream(const GitUrl *url, const char *method, const char *accept, const char *content_type, const unsigned char *body, size_t body_size, GitBuffer *response, GitHttpBodyCallback callback, void *callback_user_data) {
    return git_http_request_stream_ex(url, method, accept, content_type, 0, body, body_size, response, callback, callback_user_data);
}

static int git_http_request(const GitUrl *url, const char *method, const char *accept, const char *content_type, const unsigned char *body, size_t body_size, GitBuffer *response) {
    return git_http_request_stream(url, method, accept, content_type, body, body_size, response, 0, 0);
}

static int git_http_request_ex(const GitUrl *url, const char *method, const char *accept, const char *content_type, const char *git_protocol, const unsigned char *body, size_t body_size, GitBuffer *response) {
    return git_http_request_stream_ex(url, method, accept, content_type, git_protocol, body, body_size, response, 0, 0);
}

static int git_append_pkt_data(GitBuffer *buffer, const void *payload, size_t payload_length) {
    static const char digits[] = "0123456789abcdef";
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

static int git_append_pkt_line(GitBuffer *buffer, const char *payload) {
    return git_append_pkt_data(buffer, payload, rt_strlen(payload));
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

