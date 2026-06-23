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
            return receive->count > 0U ? 0 : gitd_receive_parse_fail(receive, "missing receive-pack commands\n");
        }
        if (packet_length < 4U || pos + packet_length - 4U > body->size) return gitd_receive_parse_fail(receive, "malformed receive-pack command\n");
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
        if (old_end != GIT_OBJECT_HEX_SIZE || new_end - new_start != GIT_OBJECT_HEX_SIZE || ref_start >= ref_end || ref_end - ref_start >= GIT_REF_CAPACITY) return gitd_receive_parse_fail(receive, "malformed receive-pack command\n");
        rt_memset(&command, 0, sizeof(command));
        if (git_parse_oid_hex_n((const char *)payload + old_start, GIT_OBJECT_HEX_SIZE, command.old_oid) != 0 ||
            git_parse_oid_hex_n((const char *)payload + new_start, GIT_OBJECT_HEX_SIZE, command.new_oid) != 0) return gitd_receive_parse_fail(receive, "malformed receive-pack oid\n");
        memcpy(command.ref_name, payload + ref_start, ref_end - ref_start);
        command.ref_name[ref_end - ref_start] = '\0';
        if (receive->count >= options->max_commands) return gitd_receive_parse_fail(receive, "too many receive-pack commands\n");
        if (gitd_receive_request_push(receive, &command) != 0) return gitd_receive_parse_fail(receive, "cannot store receive-pack command\n");
    }
    return gitd_receive_parse_fail(receive, "missing receive-pack flush\n");
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
        const char *parse_error = receive.parse_error != 0 ? receive.parse_error : "malformed receive-pack request\n";
        gitd_log_message(options, "warn", "receive-pack rejected", parse_error);
        gitd_receive_request_destroy(&receive);
        git_buffer_destroy(&decoded_body);
        return gitd_send_text(transport, 400, parse_error);
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
        gitd_log_message(options, "error", "receive-pack unpack failed", error_ref);
    } else if (received_pack.count > options->max_objects) {
        error = "too many objects";
        error_ref = receive.count > 0U ? receive.commands[0].ref_name : "refs/heads/unknown";
        gitd_log_message(options, "warn", "receive-pack too many objects", error_ref);
    } else if (gitd_apply_receive_commands(options, &repo, &received_pack, &receive, &error, &error_ref) != 0) {
        /* error fields are set by validation. */
        gitd_log_message(options, "warn", error != 0 ? error : "receive-pack rejected", error_ref);
    }
    result = gitd_send_receive_status(transport, &receive, error, error_ref);
    git_pack_destroy(&received_pack);
    gitd_receive_request_destroy(&receive);
    git_buffer_destroy(&decoded_body);
    return result;
}
