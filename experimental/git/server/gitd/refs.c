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

static int gitd_append_peeled_ref_advertisement(GitBuffer *out, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const char *name) {
    char peeled_name[GIT_REF_CAPACITY + 4U];
    size_t name_length = rt_strlen(name);

    if (name_length + 3U >= sizeof(peeled_name)) return -1;
    memcpy(peeled_name, name, name_length);
    memcpy(peeled_name + name_length, "^{}", 4U);
    return gitd_append_ref_advertisement(out, oid, peeled_name, 0);
}

static int gitd_parse_tag_target(const unsigned char *data, size_t size, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], int *type_out) {
    size_t pos = 0U;
    int have_oid = 0;
    int have_type = 0;

    while (pos < size) {
        size_t start = pos;
        size_t end;
        while (pos < size && data[pos] != '\n') pos += 1U;
        end = pos;
        if (pos < size) pos += 1U;
        while (end > start && data[end - 1U] == '\r') end -= 1U;
        if (end == start) break;
        if (end == start + 7U + GIT_OBJECT_HEX_SIZE && memcmp(data + start, "object ", 7U) == 0) {
            if (git_parse_oid_hex_n((const char *)data + start + 7U, GIT_OBJECT_HEX_SIZE, oid) != 0) return -1;
            have_oid = 1;
        } else if (end > start + 5U && memcmp(data + start, "type ", 5U) == 0) {
            *type_out = git_object_type_from_name((const char *)data + start + 5U, end - start - 5U);
            if (*type_out == 0) return -1;
            have_type = 1;
        }
    }
    return have_oid && have_type ? 0 : -1;
}

static int gitd_peel_tag(GitRepo *repo, const GitPack *pack_cache, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], unsigned char peeled_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    unsigned char current[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned int depth;

    memcpy(current, oid, sizeof(current));
    for (depth = 0U; depth < 16U; ++depth) {
        unsigned char target[CRYPTO_SHA1_DIGEST_SIZE];
        unsigned char *data = 0;
        size_t size = 0U;
        int type = 0;
        int target_type = 0;

        if (git_read_object(repo, current, pack_cache, &type, &data, &size) != 0) return -1;
        if (type != GIT_OBJECT_TAG) {
            rt_free(data);
            if (depth == 0U) return 0;
            memcpy(peeled_oid, current, CRYPTO_SHA1_DIGEST_SIZE);
            return 1;
        }
        if (gitd_parse_tag_target(data, size, target, &target_type) != 0) {
            rt_free(data);
            return -1;
        }
        rt_free(data);
        (void)target_type;
        memcpy(current, target, sizeof(current));
    }
    return -1;
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
    char alternate[GIT_PATH_CAPACITY];
    char head_path[GIT_PATH_CAPACITY];
    PlatformDirEntry head_entry;
    const char *path = repo_url_path;
    size_t relative_length;

    if (path[0] == '/') path += 1;
    if (path[0] == '\0' || gitd_path_has_parent_reference(path)) return -1;
    if (git_copy(relative, sizeof(relative), path) != 0) return -1;
    while (relative[0] != '\0' && relative[rt_strlen(relative) - 1U] == '/') relative[rt_strlen(relative) - 1U] = '\0';
    rt_memset(repo, 0, sizeof(*repo));
    if (git_join(repo->git_dir, sizeof(repo->git_dir), options->repo_root, relative) != 0 || git_copy(repo->work_tree, sizeof(repo->work_tree), repo->git_dir) != 0) return -1;
    if (git_join(head_path, sizeof(head_path), repo->git_dir, "HEAD") != 0) return -1;
    if (platform_get_path_info(head_path, &head_entry) != 0 || head_entry.is_dir) {
        relative_length = rt_strlen(relative);
        if (relative_length < 4U || rt_strcmp(relative + relative_length - 4U, ".git") != 0) {
            if (relative_length + 5U >= sizeof(alternate)) return -1;
            rt_copy_string(alternate, sizeof(alternate), relative);
            rt_copy_string(alternate + relative_length, sizeof(alternate) - relative_length, ".git");
            if (git_join(repo->git_dir, sizeof(repo->git_dir), options->repo_root, alternate) != 0 || git_copy(repo->work_tree, sizeof(repo->work_tree), repo->git_dir) != 0) return -1;
            if (git_join(head_path, sizeof(head_path), repo->git_dir, "HEAD") != 0) return -1;
        }
        if (platform_get_path_info(head_path, &head_entry) != 0 || head_entry.is_dir) return -1;
    }
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
