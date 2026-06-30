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

typedef struct {
    GitBuffer *response;
    GitHttpBodyCallback callback;
    void *callback_user_data;
} GitSshOutput;

static int git_ssh_output_callback(const unsigned char *data, size_t size, int extended, void *user_data) {
    GitSshOutput *output = (GitSshOutput *)user_data;

    if (extended) {
        if (size != 0U) {
            (void)rt_write_cstr(2, "remote: ");
            (void)rt_write_all(2, data, size);
            if (data[size - 1U] != '\n') {
                (void)rt_write_char(2, '\n');
            }
        }
        return 0;
    }
    if (output == 0) {
        return -1;
    }
    if (output->callback != 0) {
        return output->callback(data, size, output->callback_user_data);
    }
    return output->response != 0 ? git_buffer_append(output->response, data, size) : -1;
}

static int git_shell_quote_append(GitBuffer *out, const char *text) {
    size_t i;

    if (out == 0 || text == 0 ||
        tool_byte_buffer_append_char(out, '\'') != 0) {
        return -1;
    }
    for (i = 0U; text[i] != '\0'; ++i) {
        if (text[i] == '\'') {
            if (tool_byte_buffer_append_cstr(out, "'\\''") != 0) {
                return -1;
            }
        } else if (tool_byte_buffer_append_char(out, text[i]) != 0) {
            return -1;
        }
    }
    return tool_byte_buffer_append_char(out, '\'');
}

static int git_ssh_build_command(const GitUrl *url, const char *service, char *buffer, size_t buffer_size) {
    GitBuffer command;
    int result = -1;

    rt_memset(&command, 0, sizeof(command));
    if (url == 0 || service == 0 || url->scheme != GIT_SCHEME_SSH || url->path[0] == '\0') {
        return -1;
    }
    if (tool_byte_buffer_append_cstr(&command, service) != 0 ||
        tool_byte_buffer_append_char(&command, ' ') != 0 ||
        git_shell_quote_append(&command, url->path) != 0 ||
        tool_byte_buffer_append_char(&command, '\0') != 0 ||
        command.size > buffer_size) {
        goto done;
    }
    memcpy(buffer, command.data, command.size);
    result = 0;
done:
    git_buffer_destroy(&command);
    return result;
}

static int git_ssh_exec_url(const GitUrl *url, const char *service, const unsigned char *request, size_t request_size, GitBuffer *response, GitHttpBodyCallback callback, void *callback_user_data) {
    SshClientExecConfig config;
    GitSshOutput output;
    char command[1400];
    char default_user[SSH_USER_CAPACITY];
    const char *user = url != 0 && url->user[0] != '\0' ? url->user : 0;
    int exit_status = 255;

    if (url == 0 || url->scheme != GIT_SCHEME_SSH || git_ssh_build_command(url, service, command, sizeof(command)) != 0) {
        return -1;
    }
    if (user == 0) {
        const char *env_user = platform_getenv("USER");
        if (env_user == 0 || env_user[0] == '\0' || rt_strlen(env_user) >= sizeof(default_user) ||
            !ssh_destination_user_is_safe(env_user)) {
            tool_write_error("git", "ssh remote needs a user: ", url->host);
            return -1;
        }
        rt_copy_string(default_user, sizeof(default_user), env_user);
        user = default_user;
    }

    rt_memset(&config, 0, sizeof(config));
    rt_memset(&output, 0, sizeof(output));
    output.response = response;
    output.callback = callback;
    output.callback_user_data = callback_user_data;

    config.client.host = url->host;
    config.client.user = user;
    config.client.port = url->port == 0U ? SSH_DEFAULT_PORT : url->port;
    config.client.password = platform_getenv("NEWOS_GIT_SSH_PASSWORD");
    config.client.identity_path = platform_getenv("NEWOS_GIT_SSH_IDENTITY");
    config.client.verbose = platform_getenv("NEWOS_GIT_SSH_VERBOSE") != 0;
    config.command = command;
    config.input = request;
    config.input_size = request_size;
    config.output_callback = git_ssh_output_callback;
    config.output_user_data = &output;

    if (ssh_client_exec(&config, &exit_status) != 0 || exit_status != 0) {
        return -1;
    }
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
                size_t cap_end = cap_start;
                const char symref[] = "symref=HEAD:";
                while (cap_end < payload_length && payload[cap_end] != '\n' && payload[cap_end] != '\r') {
                    cap_end += 1U;
                }
                if (cap_end > cap_start) {
                    size_t cap_length = cap_end - cap_start;
                    if (cap_length >= sizeof(refs->capabilities)) {
                        cap_length = sizeof(refs->capabilities) - 1U;
                    }
                    memcpy(refs->capabilities, payload + cap_start, cap_length);
                    refs->capabilities[cap_length] = '\0';
                }
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

static int git_parse_v2_capabilities(const GitBuffer *body, GitRemoteRefs *refs) {
    size_t pos = 0U;
    int saw_version = 0;

    while (pos < body->size) {
        size_t packet_length;
        const unsigned char *payload;
        size_t payload_length;

        if (git_pkt_length(body->data + pos, body->size - pos, &packet_length) != 0) return -1;
        pos += 4U;
        if (packet_length == GIT_PACKET_FLUSH) {
            if (saw_version) break;
            continue;
        }
        if (packet_length < 4U || pos + packet_length - 4U > body->size) return -1;
        payload = body->data + pos;
        payload_length = packet_length - 4U;
        pos += payload_length;
        while (payload_length > 0U && (payload[payload_length - 1U] == '\n' || payload[payload_length - 1U] == '\r')) payload_length -= 1U;
        if (!saw_version && payload_length >= 10U && memcmp(payload, "# service=", 10U) == 0) {
            continue;
        }
        if (!saw_version) {
            if (payload_length != 9U || memcmp(payload, "version 2", 9U) != 0) return -1;
            saw_version = 1;
            continue;
        }
        if (payload_length + rt_strlen(refs->capabilities) + 2U < sizeof(refs->capabilities)) {
            size_t used = rt_strlen(refs->capabilities);
            if (used != 0U) {
                refs->capabilities[used++] = ' ';
                refs->capabilities[used] = '\0';
            }
            memcpy(refs->capabilities + used, payload, payload_length);
            refs->capabilities[used + payload_length] = '\0';
        }
    }
    refs->protocol_version = 2;
    return saw_version && git_header_value_contains((const unsigned char *)refs->capabilities, rt_strlen(refs->capabilities), "ls-refs") && git_header_value_contains((const unsigned char *)refs->capabilities, rt_strlen(refs->capabilities), "fetch") ? 0 : -1;
}

static int git_parse_v2_ls_refs_response(const GitBuffer *body, GitRemoteRefs *refs) {
    size_t pos = 0U;

    while (pos < body->size) {
        size_t packet_length;
        const unsigned char *payload;
        size_t payload_length;

        if (git_pkt_length(body->data + pos, body->size - pos, &packet_length) != 0) {
            git_remote_refs_destroy(refs);
            return -1;
        }
        pos += 4U;
        if (packet_length == GIT_PACKET_FLUSH || packet_length == GIT_PACKET_RESPONSE_END) break;
        if (packet_length == GIT_PACKET_DELIM) continue;
        if (packet_length < 4U || pos + packet_length - 4U > body->size) {
            git_remote_refs_destroy(refs);
            return -1;
        }
        payload = body->data + pos;
        payload_length = packet_length - 4U;
        pos += payload_length;
        while (payload_length > 0U && (payload[payload_length - 1U] == '\n' || payload[payload_length - 1U] == '\r')) payload_length -= 1U;
        if (payload_length > GIT_OBJECT_HEX_SIZE + 1U && payload[GIT_OBJECT_HEX_SIZE] == ' ') {
            unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
            size_t name_start = GIT_OBJECT_HEX_SIZE + 1U;
            size_t name_end = name_start;
            size_t attr_pos;

            if (git_parse_oid_hex_n((const char *)payload, GIT_OBJECT_HEX_SIZE, oid) != 0) continue;
            while (name_end < payload_length && payload[name_end] != ' ') name_end += 1U;
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
            attr_pos = name_end;
            while (attr_pos < payload_length) {
                const char symref[] = " symref-target:";
                if (attr_pos + sizeof(symref) - 1U <= payload_length && memcmp(payload + attr_pos, symref, sizeof(symref) - 1U) == 0) {
                    size_t start = attr_pos + sizeof(symref) - 1U;
                    size_t end = start;
                    while (end < payload_length && payload[end] != ' ') end += 1U;
                    if (end > start && end - start < sizeof(refs->head_ref)) {
                        memcpy(refs->head_ref, payload + start, end - start);
                        refs->head_ref[end - start] = '\0';
                    }
                    break;
                }
                attr_pos += 1U;
            }
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

static int git_discover_remote_refs_v2(const char *remote_url, GitUrl *base_url, GitRemoteRefs *refs) {
    GitUrl info_url;
    GitUrl upload_url;
    GitBuffer body;
    GitBuffer request;
    char path[1024];
    int result = -1;

    rt_memset(&body, 0, sizeof(body));
    rt_memset(&request, 0, sizeof(request));
    rt_memset(refs, 0, sizeof(*refs));
    if (git_url_is_ssh(remote_url)) {
        return -1;
    }
    if (git_parse_url(remote_url, base_url) != 0 || git_url_service_path(base_url, "/info/refs?service=git-upload-pack", path, sizeof(path)) != 0 || git_remote_url_with_path(base_url, path, &info_url) != 0) goto done;
    if (git_http_request_ex(&info_url, "GET", "application/x-git-upload-pack-advertisement", 0, "version=2", 0, 0U, &body) != 0 || git_parse_v2_capabilities(&body, refs) != 0) goto done;
    git_buffer_destroy(&body);
    rt_memset(&body, 0, sizeof(body));
    if (git_append_pkt_line(&request, "command=ls-refs\n") != 0 ||
        git_append_pkt_line(&request, "agent=newos-git\n") != 0 ||
        git_append_pkt_line(&request, "object-format=sha1\n") != 0 ||
        tool_byte_buffer_append_cstr(&request, "0001") != 0 ||
        git_append_pkt_line(&request, "peel\n") != 0 ||
        git_append_pkt_line(&request, "symrefs\n") != 0 ||
        git_append_pkt_line(&request, "unborn\n") != 0 ||
        git_append_pkt_line(&request, "ref-prefix HEAD\n") != 0 ||
        git_append_pkt_line(&request, "ref-prefix refs/heads/\n") != 0 ||
        git_append_pkt_line(&request, "ref-prefix refs/tags/\n") != 0 ||
        tool_byte_buffer_append_cstr(&request, "0000") != 0) goto done;
    if (git_url_service_path(base_url, "/git-upload-pack", path, sizeof(path)) != 0 || git_remote_url_with_path(base_url, path, &upload_url) != 0) goto done;
    if (git_http_request_ex(&upload_url, "POST", "application/x-git-upload-pack-result", "application/x-git-upload-pack-request", "version=2", request.data, request.size, &body) != 0 || git_parse_v2_ls_refs_response(&body, refs) != 0) goto done;
    refs->protocol_version = 2;
    result = 0;
done:
    if (result != 0) git_remote_refs_destroy(refs);
    git_buffer_destroy(&body);
    git_buffer_destroy(&request);
    return result;
}

static int git_discover_remote_refs_v1(const char *remote_url, GitUrl *base_url, GitRemoteRefs *refs) {
    GitUrl info_url;
    GitBuffer body;
    char path[1024];
    int result;

    if (git_url_is_ssh(remote_url)) {
        static const unsigned char flush_request[] = { '0', '0', '0', '0' };
        rt_memset(&body, 0, sizeof(body));
        if (git_parse_url(remote_url, base_url) != 0 ||
            git_ssh_exec_url(base_url, "git-upload-pack", flush_request, sizeof(flush_request), &body, 0, 0) != 0) {
            return -1;
        }
        result = git_parse_advertised_refs(&body, refs);
        if (result == 0) refs->protocol_version = 1;
        git_buffer_destroy(&body);
        return result;
    }
    if (git_parse_url(remote_url, base_url) != 0 || git_url_service_path(base_url, "/info/refs?service=git-upload-pack", path, sizeof(path)) != 0 || git_remote_url_with_path(base_url, path, &info_url) != 0) {
        return -1;
    }
    if (git_http_request(&info_url, "GET", "application/x-git-upload-pack-advertisement", 0, 0, 0U, &body) != 0) {
        return -1;
    }
    result = git_parse_advertised_refs(&body, refs);
    if (result == 0) refs->protocol_version = 1;
    git_buffer_destroy(&body);
    return result;
}

static int git_discover_remote_refs(const char *remote_url, GitUrl *base_url, GitRemoteRefs *refs, int require_v2) {
    if (git_url_is_ssh(remote_url)) {
        return git_discover_remote_refs_v1(remote_url, base_url, refs);
    }
    if (require_v2) {
        return git_discover_remote_refs_v2(remote_url, base_url, refs);
    }
    if (git_discover_remote_refs_v1(remote_url, base_url, refs) == 0) {
        return 0;
    }
    return git_discover_remote_refs_v2(remote_url, base_url, refs);
}

static int git_discover_receive_refs(const char *remote_url, GitUrl *base_url, GitRemoteRefs *refs) {
    GitUrl info_url;
    GitBuffer body;
    char path[1024];
    int result;

    if (git_url_is_ssh(remote_url)) {
        static const unsigned char flush_request[] = { '0', '0', '0', '0' };
        rt_memset(&body, 0, sizeof(body));
        if (git_parse_url(remote_url, base_url) != 0 ||
            git_ssh_exec_url(base_url, "git-receive-pack", flush_request, sizeof(flush_request), &body, 0, 0) != 0) {
            return -1;
        }
        result = git_parse_advertised_refs(&body, refs);
        git_buffer_destroy(&body);
        return result;
    }
    if (git_parse_url(remote_url, base_url) != 0 || git_url_service_path(base_url, "/info/refs?service=git-receive-pack", path, sizeof(path)) != 0 || git_remote_url_with_path(base_url, path, &info_url) != 0) {
        return -1;
    }
    if (git_http_request(&info_url, "GET", "application/x-git-receive-pack-advertisement", 0, 0, 0U, &body) != 0) {
        return -1;
    }
    result = git_parse_advertised_refs(&body, refs);
    git_buffer_destroy(&body);
    return result;
}

static void git_upload_pack_close_remote_progress(GitUploadPackStream *stream) {
    if (stream != 0 && stream->remote_progress_open) {
        (void)rt_write_char(2, '\n');
        stream->remote_progress_open = 0;
    }
}

static void git_write_remote_progress_payload(GitUploadPackStream *stream, const unsigned char *payload, size_t payload_length) {
    size_t start = 0U;
    size_t i;

    if (payload_length == 0U) {
        return;
    }
    for (i = 0U; i < payload_length; ++i) {
        if (payload[i] == '\r') {
            start = i + 1U;
        }
    }
    if (start >= payload_length) {
        return;
    }
    if (start > 0U) {
        (void)rt_write_char(2, '\r');
    } else {
        git_upload_pack_close_remote_progress(stream);
    }
    (void)rt_write_cstr(2, "remote: ");
    (void)rt_write_all(2, payload + start, payload_length - start);
    if (payload[payload_length - 1U] == '\n') {
        if (stream != 0) {
            stream->remote_progress_open = 0;
        }
    } else if (start > 0U || payload[payload_length - 1U] == '\r') {
        if (stream != 0) {
            stream->remote_progress_open = 1;
        }
    } else {
        (void)rt_write_cstr(2, "\n");
        if (stream != 0) {
            stream->remote_progress_open = 0;
        }
    }
}

static void git_upload_pack_note_pack_bytes(GitUploadPackStream *stream) {
    if (stream->next_progress_bytes == 0U) {
        stream->next_progress_bytes = GIT_PACK_PROGRESS_STEP;
    }
    while (stream->pack_data.size >= stream->next_progress_bytes) {
        git_upload_pack_close_remote_progress(stream);
        git_progress_pack_bytes("Receiving pack: ", stream->pack_data.size);
        stream->printed_progress = 1;
        if (stream->next_progress_bytes > ((size_t)-1) - GIT_PACK_PROGRESS_STEP) {
            stream->next_progress_bytes = (size_t)-1;
            break;
        }
        stream->next_progress_bytes += GIT_PACK_PROGRESS_STEP;
    }
}

static int git_upload_pack_stream_payload(GitUploadPackStream *stream, const unsigned char *payload, size_t payload_length) {
    if (payload_length == 4U && memcmp(payload, "NAK\n", 4U) == 0) {
        return 0;
    }
    if (payload_length >= 48U && memcmp(payload, "shallow ", 8U) == 0) {
        size_t line_length = payload_length;

        while (line_length > 0U && (payload[line_length - 1U] == '\n' || payload[line_length - 1U] == '\r')) line_length -= 1U;
        if (line_length == 48U) {
            unsigned char parsed_oid[CRYPTO_SHA1_DIGEST_SIZE];

            if (git_parse_oid_hex_n((const char *)payload + 8U, GIT_OBJECT_HEX_SIZE, parsed_oid) != 0) return -1;
            return git_buffer_append(&stream->shallow_text, payload + 8U, GIT_OBJECT_HEX_SIZE) != 0 || tool_byte_buffer_append_char(&stream->shallow_text, '\n') != 0 ? -1 : 0;
        }
    }
    if ((payload_length == 9U && memcmp(payload, "packfile\n", 9U) == 0) || (payload_length == 13U && memcmp(payload, "shallow-info\n", 13U) == 0)) {
        return 0;
    }
    if (payload_length > 0U) {
        unsigned char channel = payload[0];
        if (channel == 1U) {
            if (git_buffer_append(&stream->pack_data, payload + 1U, payload_length - 1U) != 0) {
                return -1;
            }
            git_upload_pack_note_pack_bytes(stream);
            return 0;
        }
        if (channel == 2U) {
            if (payload_length > 1U) {
                git_write_remote_progress_payload(stream, payload + 1U, payload_length - 1U);
            }
            return 0;
        }
        if (channel == 3U) {
            return -1;
        }
        if (memcmp(payload, "PACK", payload_length < 4U ? payload_length : 4U) == 0) {
            if (git_buffer_append(&stream->pack_data, payload, payload_length) != 0) {
                return -1;
            }
            git_upload_pack_note_pack_bytes(stream);
            return 0;
        }
    }
    return 0;
}

static int git_upload_pack_stream_feed(const unsigned char *data, size_t size, void *user_data) {
    GitUploadPackStream *stream = (GitUploadPackStream *)user_data;
    size_t consumed = 0U;

    if (git_buffer_append(&stream->pending, data, size) != 0) {
        return -1;
    }
    while (consumed < stream->pending.size) {
        size_t packet_length;
        const unsigned char *payload;
        size_t payload_length;

        if (stream->pending.size - consumed < 4U) {
            break;
        }
        if (git_pkt_length(stream->pending.data + consumed, stream->pending.size - consumed, &packet_length) != 0) {
            return -1;
        }
        if (packet_length == GIT_PACKET_FLUSH || packet_length == GIT_PACKET_DELIM || packet_length == GIT_PACKET_RESPONSE_END) {
            consumed += 4U;
            continue;
        }
        if (packet_length < 4U) {
            return -1;
        }
        if (stream->pending.size - consumed < packet_length) {
            break;
        }
        payload = stream->pending.data + consumed + 4U;
        payload_length = packet_length - 4U;
        if (git_upload_pack_stream_payload(stream, payload, payload_length) != 0) {
            return -1;
        }
        consumed += packet_length;
    }
    git_buffer_discard_prefix(&stream->pending, consumed);
    return 0;
}

static int git_upload_pack_stream_finish(const GitUploadPackStream *stream) {
    return stream->pending.size == 0U && stream->pack_data.size >= 4U && memcmp(stream->pack_data.data, "PACK", 4U) == 0 ? 0 : -1;
}

static void git_upload_pack_stream_destroy(GitUploadPackStream *stream) {
    git_buffer_destroy(&stream->pending);
    git_buffer_destroy(&stream->pack_data);
    git_buffer_destroy(&stream->shallow_text);
}

static int git_write_shallow_file(const GitRepo *repo, const GitBuffer *shallow_text) {
    char path[GIT_PATH_CAPACITY];

    if (shallow_text == 0 || shallow_text->size == 0U) return 0;
    if (git_join(path, sizeof(path), repo->git_dir, "shallow") != 0) return -1;
    return git_write_all_file(path, shallow_text->data, shallow_text->size, 0644U);
}

static int git_fetch_pack_v1(const GitRepo *repo, const GitUrl *base_url, const unsigned char want_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitFetchOptions *options, GitPack *pack_out) {
    GitUrl upload_url;
    GitBuffer request;
    GitUploadPackStream stream;
    GitPack pack;
    char path[1024];
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
    char want_line[256];
    size_t want_length = 0U;
    int result = -1;

    rt_memset(&request, 0, sizeof(request));
    rt_memset(&stream, 0, sizeof(stream));
    rt_memset(&pack, 0, sizeof(pack));
    git_format_oid_hex(want_oid, oid_hex);
    want_length = tool_buffer_append_cstr(want_line, sizeof(want_line), want_length, "want ");
    want_length = tool_buffer_append_cstr(want_line, sizeof(want_line), want_length, oid_hex);
    want_length = tool_buffer_append_cstr(want_line, sizeof(want_line), want_length, " multi_ack_detailed side-band-64k ofs-delta agent=newos-git\n");
    if (want_length >= sizeof(want_line) || git_append_pkt_line(&request, want_line) != 0) {
        goto done;
    }
    if (options != 0 && options->depth > 0U) {
        char depth_text[32];
        char depth_line[64];
        size_t depth_length = 0U;

        rt_unsigned_to_string((unsigned long long)options->depth, depth_text, sizeof(depth_text));
        depth_length = tool_buffer_append_cstr(depth_line, sizeof(depth_line), depth_length, "deepen ");
        depth_length = tool_buffer_append_cstr(depth_line, sizeof(depth_line), depth_length, depth_text);
        depth_length = tool_buffer_append_char(depth_line, sizeof(depth_line), depth_length, '\n');
        if (depth_length >= sizeof(depth_line) || git_append_pkt_line(&request, depth_line) != 0) {
            goto done;
        }
    }
    if (tool_byte_buffer_append_cstr(&request, "0000") != 0 || git_append_pkt_line(&request, "done\n") != 0) {
        goto done;
    }
    git_progress_line("Downloading pack...");
    if (base_url->scheme == GIT_SCHEME_SSH) {
        if (git_ssh_exec_url(base_url, "git-upload-pack", request.data, request.size, 0, git_upload_pack_stream_feed, &stream) != 0 ||
            git_upload_pack_stream_finish(&stream) != 0) {
            goto done;
        }
    } else {
        if (git_url_service_path(base_url, "/git-upload-pack", path, sizeof(path)) != 0 || git_remote_url_with_path(base_url, path, &upload_url) != 0) {
            goto done;
        }
        if (git_http_request_stream(&upload_url, "POST", "application/x-git-upload-pack-result", "application/x-git-upload-pack-request", request.data, request.size, 0, git_upload_pack_stream_feed, &stream) != 0 || git_upload_pack_stream_finish(&stream) != 0) {
            goto done;
        }
    }
    git_upload_pack_close_remote_progress(&stream);
    git_progress_pack_bytes("Received pack: ", stream.pack_data.size);
    git_progress_line("Unpacking objects...");
    if (git_parse_pack(stream.pack_data.data, stream.pack_data.size, &pack) != 0 || git_resolve_pack_deltas(&pack) != 0) {
        goto done;
    }
    git_progress_count_line("Received objects: ", pack.count);
    git_progress_line("Storing pack...");
    if (git_write_pack_file(repo, stream.pack_data.data, stream.pack_data.size, &pack) != 0 || git_write_shallow_file(repo, &stream.shallow_text) != 0) {
        goto done;
    }
    if (pack_out != 0) {
        *pack_out = pack;
        rt_memset(&pack, 0, sizeof(pack));
    }
    result = 0;
done:
    git_buffer_destroy(&request);
    git_upload_pack_stream_destroy(&stream);
    git_pack_destroy(&pack);
    return result;
}

static int git_fetch_pack_v2(const GitRepo *repo, const GitUrl *base_url, const unsigned char want_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitFetchOptions *options, GitPack *pack_out) {
    GitUrl upload_url;
    GitBuffer request;
    GitUploadPackStream stream;
    GitPack pack;
    char path[1024];
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
    char line[256];
    int result = -1;

    rt_memset(&request, 0, sizeof(request));
    rt_memset(&stream, 0, sizeof(stream));
    rt_memset(&pack, 0, sizeof(pack));
    if (base_url->scheme == GIT_SCHEME_SSH) return -1;
    git_format_oid_hex(want_oid, oid_hex);
    if (git_append_pkt_line(&request, "command=fetch\n") != 0 ||
        git_append_pkt_line(&request, "agent=newos-git\n") != 0 ||
        git_append_pkt_line(&request, "object-format=sha1\n") != 0 ||
        tool_byte_buffer_append_cstr(&request, "0001") != 0 ||
        git_append_pkt_line(&request, "ofs-delta\n") != 0) goto done;
    {
        size_t used = 0U;
        used = tool_buffer_append_cstr(line, sizeof(line), used, "want ");
        used = tool_buffer_append_cstr(line, sizeof(line), used, oid_hex);
        used = tool_buffer_append_char(line, sizeof(line), used, '\n');
        if (used >= sizeof(line) || git_append_pkt_line(&request, line) != 0) goto done;
    }
    if (options != 0 && options->depth > 0U) {
        size_t used = 0U;
        char depth_text[32];

        rt_unsigned_to_string((unsigned long long)options->depth, depth_text, sizeof(depth_text));
        used = tool_buffer_append_cstr(line, sizeof(line), used, "deepen ");
        used = tool_buffer_append_cstr(line, sizeof(line), used, depth_text);
        used = tool_buffer_append_char(line, sizeof(line), used, '\n');
        if (used >= sizeof(line) || git_append_pkt_line(&request, line) != 0) goto done;
    }
    if (options != 0 && options->filter_blob_none) {
        if (git_append_pkt_line(&request, "filter blob:none\n") != 0) goto done;
    }
    if (git_append_pkt_line(&request, "done\n") != 0 || tool_byte_buffer_append_cstr(&request, "0000") != 0) goto done;
    if (git_url_service_path(base_url, "/git-upload-pack", path, sizeof(path)) != 0 || git_remote_url_with_path(base_url, path, &upload_url) != 0) goto done;
    git_progress_line("Downloading pack...");
    if (git_http_request_stream_ex(&upload_url, "POST", "application/x-git-upload-pack-result", "application/x-git-upload-pack-request", "version=2", request.data, request.size, 0, git_upload_pack_stream_feed, &stream) != 0 || git_upload_pack_stream_finish(&stream) != 0) goto done;
    git_upload_pack_close_remote_progress(&stream);
    git_progress_pack_bytes("Received pack: ", stream.pack_data.size);
    git_progress_line("Unpacking objects...");
    if (git_parse_pack(stream.pack_data.data, stream.pack_data.size, &pack) != 0 || git_resolve_pack_deltas(&pack) != 0) goto done;
    git_progress_count_line("Received objects: ", pack.count);
    git_progress_line("Storing pack...");
    if (git_write_pack_file(repo, stream.pack_data.data, stream.pack_data.size, &pack) != 0 || git_write_shallow_file(repo, &stream.shallow_text) != 0) goto done;
    if (pack_out != 0) {
        *pack_out = pack;
        rt_memset(&pack, 0, sizeof(pack));
    }
    result = 0;
done:
    git_buffer_destroy(&request);
    git_upload_pack_stream_destroy(&stream);
    git_pack_destroy(&pack);
    return result;
}

static int git_fetch_pack(const GitRepo *repo, const GitUrl *base_url, int protocol_version, const unsigned char want_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitFetchOptions *options, GitPack *pack_out) {
    if (protocol_version == 2) return git_fetch_pack_v2(repo, base_url, want_oid, options, pack_out);
    if (options != 0 && options->filter_blob_none) return -1;
    return git_fetch_pack_v1(repo, base_url, want_oid, options, pack_out);
}

static int git_repo_has_object(const GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    unsigned char *data = 0;
    size_t size = 0U;
    int type = 0;

    if (git_read_object(repo, oid, 0, &type, &data, &size) != 0) {
        return 0;
    }
    rt_free(data);
    return 1;
}

static int git_push_collect_tree_objects(GitRepo *repo, const unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitOidList *objects) {
    unsigned char *tree = 0;
    size_t tree_size = 0U;
    size_t pos = 0U;
    int type = 0;
    int result = -1;

    if (git_oid_list_push_unique(objects, tree_oid) != 0 || git_read_object(repo, tree_oid, pack_cache, &type, &tree, &tree_size) != 0 || type != GIT_OBJECT_TREE) {
        goto done;
    }
    while (pos < tree_size) {
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
        unsigned int mode = 0U;
        size_t name_start;

        while (pos < tree_size && tree[pos] >= '0' && tree[pos] <= '7') {
            mode = (mode << 3U) + (unsigned int)(tree[pos] - '0');
            pos += 1U;
        }
        if (pos >= tree_size || tree[pos] != ' ') {
            goto done;
        }
        pos += 1U;
        name_start = pos;
        while (pos < tree_size && tree[pos] != '\0') {
            pos += 1U;
        }
        if (pos >= tree_size || pos == name_start || pos + 1U + CRYPTO_SHA1_DIGEST_SIZE > tree_size) {
            goto done;
        }
        pos += 1U;
        memcpy(oid, tree + pos, CRYPTO_SHA1_DIGEST_SIZE);
        pos += CRYPTO_SHA1_DIGEST_SIZE;
        if ((mode & GIT_MODE_TYPE_MASK) == GIT_MODE_TREE) {
            if (git_push_collect_tree_objects(repo, oid, pack_cache, objects) != 0) {
                goto done;
            }
        } else if ((mode & GIT_MODE_TYPE_MASK) == GIT_MODE_REGULAR_TYPE || (mode & GIT_MODE_TYPE_MASK) == GIT_MODE_SYMLINK) {
            if (git_oid_list_push_unique(objects, oid) != 0) {
                goto done;
            }
        } else if ((mode & GIT_MODE_TYPE_MASK) != GIT_MODE_GITLINK) {
            goto done;
        }
    }
    result = 0;
done:
    rt_free(tree);
    return result;
}

static int git_push_collect_commit_objects(GitRepo *repo, const unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, const GitOidList *excluded, GitOidList *objects, GitOidList *visited) {
    GitCommitInfo info;
    size_t i;
    int result = -1;

    if (git_oid_list_contains(visited, commit_oid) || (excluded != 0 && git_oid_list_contains(excluded, commit_oid))) {
        return 0;
    }
    if (git_oid_list_push_unique(visited, commit_oid) != 0 || git_oid_list_push_unique(objects, commit_oid) != 0) {
        return -1;
    }
    if (git_read_commit_info(repo, commit_oid, pack_cache, &info) != 0) {
        return -1;
    }
    if (git_push_collect_tree_objects(repo, info.tree_oid, pack_cache, objects) != 0) {
        goto done;
    }
    for (i = 0U; i < info.parent_count; ++i) {
        if (git_push_collect_commit_objects(repo, info.parents[i], pack_cache, excluded, objects, visited) != 0) {
            goto done;
        }
    }
    result = 0;
done:
    git_commit_info_destroy(&info);
    return result;
}

static int git_pack_append_object_header(GitBuffer *pack, int type, size_t size) {
    unsigned char byte = (unsigned char)(((unsigned int)type << 4U) | (unsigned int)(size & 0x0fU));

    size >>= 4U;
    while (size != 0U) {
        if (tool_byte_buffer_append_byte(pack, byte | 0x80U) != 0) {
            return -1;
        }
        byte = (unsigned char)(size & 0x7fU);
        size >>= 7U;
    }
    return tool_byte_buffer_append_byte(pack, byte);
}

static int git_push_build_pack(GitRepo *repo, const GitPack *pack_cache, const GitOidList *objects, GitBuffer *pack_out) {
    GitBuffer pack;
    CryptoSha1Context sha1;
    unsigned char digest[CRYPTO_SHA1_DIGEST_SIZE];
    size_t i;
    int result = -1;

    rt_memset(&pack, 0, sizeof(pack));
    if (tool_byte_buffer_append_cstr(&pack, "PACK") != 0 || tool_byte_buffer_append_u32_be(&pack, 2U) != 0 || tool_byte_buffer_append_u32_be(&pack, (unsigned long long)objects->count) != 0) {
        goto done;
    }
    for (i = 0U; i < objects->count; ++i) {
        int type = 0;
        unsigned char *data = 0;
        size_t size = 0U;
        unsigned char *compressed = 0;
        size_t compressed_capacity;
        size_t compressed_size = 0U;
        char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];

        git_format_oid_hex(objects->oids[i], oid_hex);
        if (git_read_object(repo, objects->oids[i], pack_cache, &type, &data, &size) != 0) {
            tool_write_error("git", "cannot read object for push pack: ", oid_hex);
            goto done;
        }
        if (type < GIT_OBJECT_COMMIT || type > GIT_OBJECT_TAG) {
            tool_write_error("git", "unsupported object type for push pack: ", oid_hex);
            rt_free(data);
            goto done;
        }
        if (git_pack_append_object_header(&pack, type, size) != 0) {
            tool_write_error("git", "cannot encode object header for push pack: ", oid_hex);
            rt_free(data);
            goto done;
        }
        compressed_capacity = compression_zlib_deflate_bound(size);
        compressed = (unsigned char *)rt_malloc(compressed_capacity == 0U ? 1U : compressed_capacity);
        if (compressed == 0) {
            tool_write_error("git", "out of memory while compressing push object: ", oid_hex);
            rt_free(data);
            goto done;
        }
        if (compression_zlib_deflate_level(data, size, compressed, compressed_capacity, &compressed_size, 6) != 0) {
            rt_free(compressed);
            compressed_capacity = compression_zlib_store_bound(size);
            compressed = (unsigned char *)rt_malloc(compressed_capacity == 0U ? 1U : compressed_capacity);
            if (compressed == 0) {
                tool_write_error("git", "out of memory while storing push object: ", oid_hex);
                rt_free(data);
                goto done;
            }
            if (compression_zlib_store(data, size, compressed, compressed_capacity, &compressed_size) != 0) {
                tool_write_error("git", "cannot store object for push pack: ", oid_hex);
                rt_free(compressed);
                rt_free(data);
                goto done;
            }
        }
        if (git_buffer_append(&pack, compressed, compressed_size) != 0) {
            tool_write_error("git", "cannot append compressed object to push pack: ", oid_hex);
            rt_free(compressed);
            rt_free(data);
            goto done;
        }
        rt_free(compressed);
        rt_free(data);
    }
    crypto_sha1_init(&sha1);
    crypto_sha1_update(&sha1, pack.data, pack.size);
    crypto_sha1_final(&sha1, digest);
    if (git_buffer_append(&pack, digest, sizeof(digest)) != 0) {
        goto done;
    }
    *pack_out = pack;
    rt_memset(&pack, 0, sizeof(pack));
    result = 0;
done:
    git_buffer_destroy(&pack);
    return result;
}

static int git_receive_pack_append_command(GitBuffer *request, const unsigned char old_oid[CRYPTO_SHA1_DIGEST_SIZE], const unsigned char new_oid[CRYPTO_SHA1_DIGEST_SIZE], const char *dst_ref) {
    char old_hex[GIT_OBJECT_HEX_SIZE + 1U];
    char new_hex[GIT_OBJECT_HEX_SIZE + 1U];
    GitBuffer line;
    int result;

    rt_memset(&line, 0, sizeof(line));
    git_format_oid_hex(old_oid, old_hex);
    git_format_oid_hex(new_oid, new_hex);
    if (tool_byte_buffer_append_cstr(&line, old_hex) != 0 || tool_byte_buffer_append_char(&line, ' ') != 0 || tool_byte_buffer_append_cstr(&line, new_hex) != 0 || tool_byte_buffer_append_char(&line, ' ') != 0 || tool_byte_buffer_append_cstr(&line, dst_ref) != 0 || tool_byte_buffer_append_char(&line, '\0') != 0 || tool_byte_buffer_append_cstr(&line, "report-status side-band-64k agent=newos-git") != 0 || tool_byte_buffer_append_char(&line, '\n') != 0) {
        git_buffer_destroy(&line);
        return -1;
    }
    result = git_append_pkt_data(request, line.data, line.size);
    git_buffer_destroy(&line);
    return result;
}

static int git_receive_pack_parse_status_payload(const unsigned char *payload, size_t payload_length, const char *dst_ref, int *unpack_ok, int *ref_ok) {
    size_t pos = 0U;

    while (pos < payload_length) {
        size_t start = pos;
        size_t end;

        while (pos < payload_length && payload[pos] != '\n') {
            pos += 1U;
        }
        end = pos;
        if (pos < payload_length) {
            pos += 1U;
        }
        if (end > start && payload[end - 1U] == '\r') {
            end -= 1U;
        }
        if (end >= start + 9U && memcmp(payload + start, "unpack ok", 9U) == 0) {
            *unpack_ok = 1;
        } else if (end >= start + 3U && memcmp(payload + start, "ok ", 3U) == 0) {
            size_t ref_start = start + 3U;
            if (end - ref_start == rt_strlen(dst_ref) && memcmp(payload + ref_start, dst_ref, end - ref_start) == 0) {
                *ref_ok = 1;
            }
        } else if (end >= start + 3U && memcmp(payload + start, "ng ", 3U) == 0) {
            rt_write_cstr(2, "remote: ");
            (void)rt_write_all(2, payload + start, end - start);
            rt_write_char(2, '\n');
            return -1;
        }
    }
    return 0;
}

static int git_receive_pack_check_status(const GitUploadPackStream *stream, const char *dst_ref) {
    size_t pos = 0U;
    int unpack_ok = 0;
    int ref_ok = 0;

    while (pos < stream->pack_data.size) {
        size_t packet_length;
        const unsigned char *payload;
        size_t payload_length;

        if (stream->pack_data.size - pos < 4U || git_pkt_length(stream->pack_data.data + pos, stream->pack_data.size - pos, &packet_length) != 0) {
            return -1;
        }
        pos += 4U;
        if (packet_length == GIT_PACKET_FLUSH) {
            continue;
        }
        if (packet_length < 4U || pos + packet_length - 4U > stream->pack_data.size) {
            return -1;
        }
        payload = stream->pack_data.data + pos;
        payload_length = packet_length - 4U;
        pos += payload_length;
        if (git_receive_pack_parse_status_payload(payload, payload_length, dst_ref, &unpack_ok, &ref_ok) != 0) {
            return -1;
        }
    }
    return unpack_ok && ref_ok ? 0 : -1;
}

static int git_receive_pack_push(GitRepo *repo, const char *remote_url, const char *dst_ref, const unsigned char old_oid[CRYPTO_SHA1_DIGEST_SIZE], int have_old, const unsigned char new_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitUrl base_url;
    GitUrl receive_url;
    GitRemoteRefs refs;
    GitPack pack_cache;
    GitOidList excluded;
    GitOidList objects;
    GitOidList visited;
    GitBuffer pack_data;
    GitBuffer request;
    GitUploadPackStream response;
    unsigned char zero_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char path[1024];
    int have_pack = 0;
    int result = -1;

    rt_memset(&refs, 0, sizeof(refs));
    rt_memset(&pack_cache, 0, sizeof(pack_cache));
    rt_memset(&excluded, 0, sizeof(excluded));
    rt_memset(&objects, 0, sizeof(objects));
    rt_memset(&visited, 0, sizeof(visited));
    rt_memset(&pack_data, 0, sizeof(pack_data));
    rt_memset(&request, 0, sizeof(request));
    rt_memset(&response, 0, sizeof(response));
    rt_memset(zero_oid, 0, sizeof(zero_oid));

    git_progress_line("Resolving remote refs...");
    if (git_discover_receive_refs(remote_url, &base_url, &refs) != 0) {
        return -1;
    }
    have_pack = git_load_pack_cache(repo, &pack_cache) == 0;
    if (have_old && git_collect_reachable_commits(repo, old_oid, have_pack ? &pack_cache : 0, &excluded) != 0) {
        git_oid_list_destroy(&excluded);
        rt_memset(&excluded, 0, sizeof(excluded));
    }
    git_progress_line("Counting objects...");
    if (git_push_collect_commit_objects(repo, new_oid, have_pack ? &pack_cache : 0, &excluded, &objects, &visited) != 0) {
        goto done;
    }
    git_progress_count_line("Writing objects: ", objects.count);
    if (git_push_build_pack(repo, have_pack ? &pack_cache : 0, &objects, &pack_data) != 0) {
        goto done;
    }
    if (git_receive_pack_append_command(&request, have_old ? old_oid : zero_oid, new_oid, dst_ref) != 0 || tool_byte_buffer_append_cstr(&request, "0000") != 0 || git_buffer_append(&request, pack_data.data, pack_data.size) != 0) {
        goto done;
    }
    git_progress_pack_bytes("Sending pack: ", pack_data.size);
    if (base_url.scheme == GIT_SCHEME_SSH) {
        if (git_ssh_exec_url(&base_url, "git-receive-pack", request.data, request.size, 0, git_upload_pack_stream_feed, &response) != 0 ||
            response.pending.size != 0U ||
            git_receive_pack_check_status(&response, dst_ref) != 0) {
            goto done;
        }
    } else {
        if (git_url_service_path(&base_url, "/git-receive-pack", path, sizeof(path)) != 0 || git_remote_url_with_path(&base_url, path, &receive_url) != 0) {
            goto done;
        }
        if (git_http_request_stream(&receive_url, "POST", "application/x-git-receive-pack-result", "application/x-git-receive-pack-request", request.data, request.size, 0, git_upload_pack_stream_feed, &response) != 0 || response.pending.size != 0U || git_receive_pack_check_status(&response, dst_ref) != 0) {
            goto done;
        }
    }
    result = 0;
done:
    git_remote_refs_destroy(&refs);
    if (have_pack) git_pack_destroy(&pack_cache);
    git_oid_list_destroy(&excluded);
    git_oid_list_destroy(&objects);
    git_oid_list_destroy(&visited);
    git_buffer_destroy(&pack_data);
    git_buffer_destroy(&request);
    git_upload_pack_stream_destroy(&response);
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
    if (git_join(path, sizeof(path), repo->git_dir, "HEAD") != 0 || tool_byte_buffer_append_cstr(&text, "ref: ") != 0 || tool_byte_buffer_append_cstr(&text, ref_name) != 0 || tool_byte_buffer_append_char(&text, '\n') != 0) {
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
    if (git_join(path, sizeof(path), repo->git_dir, "FETCH_HEAD") != 0 || tool_byte_buffer_append_cstr(&text, oid_hex) != 0 || tool_byte_buffer_append_cstr(&text, "\t\t") != 0 || tool_byte_buffer_append_cstr(&text, ref_name) != 0 || tool_byte_buffer_append_cstr(&text, "\t") != 0 || tool_byte_buffer_append_cstr(&text, remote_url) != 0 || tool_byte_buffer_append_char(&text, '\n') != 0) {
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
        tool_byte_buffer_append_cstr(&text, "[core]\n\trepositoryformatversion = 0\n\tfilemode = true\n\tbare = false\n\tlogallrefupdates = true\n") != 0 ||
        tool_byte_buffer_append_cstr(&text, "[remote \"origin\"]\n\turl = ") != 0 || tool_byte_buffer_append_cstr(&text, remote_url) != 0 ||
        tool_byte_buffer_append_cstr(&text, "\n\tfetch = +refs/heads/*:refs/remotes/origin/*\n[branch \"") != 0 || tool_byte_buffer_append_cstr(&text, branch_name) != 0 ||
        tool_byte_buffer_append_cstr(&text, "\"]\n\tremote = origin\n\tmerge = refs/heads/") != 0 || tool_byte_buffer_append_cstr(&text, branch_name) != 0 || tool_byte_buffer_append_char(&text, '\n') != 0) {
        git_buffer_destroy(&text);
        return -1;
    }
    result = git_write_all_file(path, text.data, text.size, 0644U);
    git_buffer_destroy(&text);
    return result;
}

static int git_fetch_remote_to_repo(const GitRepo *repo, const char *remote_url, const char *wanted_ref, const GitFetchOptions *options, char *selected_ref_out, size_t selected_ref_size, unsigned char selected_oid[CRYPTO_SHA1_DIGEST_SIZE], GitPack *pack_out) {
    GitUrl base_url;
    GitRemoteRefs refs;
    GitRemoteRef *selected;
    char remote_tracking[GIT_REF_CAPACITY];
    int result = -1;

    rt_memset(&refs, 0, sizeof(refs));
    git_progress_line("Resolving remote refs...");
    if (git_discover_remote_refs(remote_url, &base_url, &refs, options != 0 && options->filter_blob_none) != 0) {
        return -1;
    }
    selected = git_select_remote_ref(&refs, wanted_ref);
    if (selected == 0) {
        goto done;
    }
    if (selected_ref_out != 0 && git_copy(selected_ref_out, selected_ref_size, selected->name) != 0) {
        goto done;
    }
    memcpy(selected_oid, selected->oid, CRYPTO_SHA1_DIGEST_SIZE);
    if (pack_out == 0 && git_repo_has_object(repo, selected->oid)) {
        if (git_remote_tracking_ref_name(selected->name, remote_tracking, sizeof(remote_tracking)) == 0) {
            (void)git_write_ref_oid(repo, remote_tracking, selected->oid);
        }
        (void)git_write_fetch_head(repo, remote_url, selected->name, selected->oid);
        result = 0;
        goto done;
    }
    git_progress_pair_line("Fetching ", selected->name);
    if (git_fetch_pack(repo, &base_url, refs.protocol_version, selected->oid, options, pack_out) != 0) {
        goto done;
    }
    if (git_remote_tracking_ref_name(selected->name, remote_tracking, sizeof(remote_tracking)) == 0) {
        (void)git_write_ref_oid(repo, remote_tracking, selected->oid);
    }
    (void)git_write_fetch_head(repo, remote_url, selected->name, selected->oid);
    result = 0;
done:
    git_remote_refs_destroy(&refs);
    return result;
}
