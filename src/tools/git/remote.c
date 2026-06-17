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
        if (packet_length == GIT_PACKET_FLUSH) {
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
}

static int git_fetch_pack(const GitRepo *repo, const GitUrl *base_url, const unsigned char want_oid[CRYPTO_SHA1_DIGEST_SIZE], GitPack *pack_out) {
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
    if (want_length >= sizeof(want_line) || git_append_pkt_line(&request, want_line) != 0 || git_buffer_append_cstr(&request, "0000") != 0 || git_append_pkt_line(&request, "done\n") != 0) {
        goto done;
    }
    if (git_url_service_path(base_url, "/git-upload-pack", path, sizeof(path)) != 0 || git_remote_url_with_path(base_url, path, &upload_url) != 0) {
        goto done;
    }
    git_progress_line("Downloading pack...");
    if (git_http_request_stream(&upload_url, "POST", "application/x-git-upload-pack-result", "application/x-git-upload-pack-request", request.data, request.size, 0, git_upload_pack_stream_feed, &stream) != 0 || git_upload_pack_stream_finish(&stream) != 0) {
        goto done;
    }
    git_upload_pack_close_remote_progress(&stream);
    git_progress_pack_bytes("Received pack: ", stream.pack_data.size);
    git_progress_line("Unpacking objects...");
    if (git_parse_pack(stream.pack_data.data, stream.pack_data.size, &pack) != 0 || git_resolve_pack_deltas(&pack) != 0) {
        goto done;
    }
    git_progress_count_line("Received objects: ", pack.count);
    git_progress_line("Storing pack...");
    if (git_write_pack_file(repo, stream.pack_data.data, stream.pack_data.size, &pack) != 0) {
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

static int git_fetch_remote_to_repo(const GitRepo *repo, const char *remote_url, const char *wanted_ref, char *selected_ref_out, size_t selected_ref_size, unsigned char selected_oid[CRYPTO_SHA1_DIGEST_SIZE], GitPack *pack_out) {
    GitUrl base_url;
    GitRemoteRefs refs;
    GitRemoteRef *selected;
    char remote_tracking[GIT_REF_CAPACITY];
    int result = -1;

    rt_memset(&refs, 0, sizeof(refs));
    git_progress_line("Resolving remote refs...");
    if (git_discover_remote_refs(remote_url, &base_url, &refs) != 0) {
        return -1;
    }
    selected = git_select_remote_ref(&refs, wanted_ref);
    if (selected == 0) {
        goto done;
    }
    git_progress_pair_line("Fetching ", selected->name);
    if (git_fetch_pack(repo, &base_url, selected->oid, pack_out) != 0) {
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

