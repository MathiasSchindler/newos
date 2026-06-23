#define main git_tool_main
#include "../../../src/tools/git.c"
#undef main

#include "io_loop.h"
#include "crypto/crypto_util.h"
#include "crypto/rsa.h"
#include "tls/tls13_server.h"

#include "gitd/internal.h"

static void gitd_usage(const char *program_name) {
    tool_write_usage(program_name, "[-b HOST] [-p PORT] [-r REPO_ROOT] [--tls-cert CERT --tls-key KEY] [--once] [-q] [--read-only] [--branches-only] [--no-delete-refs] [--max-body BYTES]");
}

static int gitd_parse_size_option(const char *text, size_t *value_out) {
    unsigned long long number;

    if (rt_parse_uint(text, &number) != 0 || number == 0ULL || number > (unsigned long long)((size_t)-1)) return -1;
    *value_out = (size_t)number;
    return 0;
}

#include "gitd/http.c"

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

static void gitd_object_cache_destroy(GitdObjectCache *cache) {
    size_t index;

    if (cache == 0) return;
    for (index = 0U; index < cache->count; ++index) {
        rt_free(cache->entries[index].data);
    }
    rt_free(cache->entries);
    rt_memset(cache, 0, sizeof(*cache));
}

static GitdObjectCacheEntry *gitd_object_cache_find(GitdObjectCache *cache, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    size_t index;

    if (cache == 0) return 0;
    for (index = 0U; index < cache->count; ++index) {
        if (git_oid_equal(cache->entries[index].oid, oid)) return &cache->entries[index];
    }
    return 0;
}

static void gitd_object_cache_evict_oldest(GitdObjectCache *cache) {
    size_t index;

    if (cache == 0 || cache->count == 0U) return;
    if (cache->total_bytes >= cache->entries[0].size) cache->total_bytes -= cache->entries[0].size;
    else cache->total_bytes = 0U;
    rt_free(cache->entries[0].data);
    for (index = 1U; index < cache->count; ++index) {
        cache->entries[index - 1U] = cache->entries[index];
    }
    cache->count -= 1U;
    if (cache->entries != 0) rt_memset(&cache->entries[cache->count], 0, sizeof(cache->entries[cache->count]));
}

static void gitd_object_cache_store(GitdObjectCache *cache, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], int type, const unsigned char *data, size_t size) {
    GitdObjectCacheEntry *entry;
    unsigned char *copy;

    if (cache == 0 || data == 0 || size > GITD_OBJECT_CACHE_MAX_BYTES || gitd_object_cache_find(cache, oid) != 0) return;
    while (cache->count > 0U && (cache->count >= GITD_OBJECT_CACHE_MAX_ENTRIES || cache->total_bytes > GITD_OBJECT_CACHE_MAX_BYTES - size)) {
        gitd_object_cache_evict_oldest(cache);
    }
    if (cache->count >= GITD_OBJECT_CACHE_MAX_ENTRIES || cache->total_bytes > GITD_OBJECT_CACHE_MAX_BYTES - size) return;
    if (cache->count == cache->capacity) {
        size_t new_capacity = cache->capacity == 0U ? 64U : cache->capacity * 2U;
        GitdObjectCacheEntry *new_entries;

        if (new_capacity > GITD_OBJECT_CACHE_MAX_ENTRIES) new_capacity = GITD_OBJECT_CACHE_MAX_ENTRIES;
        if (new_capacity <= cache->capacity) return;
        new_entries = (GitdObjectCacheEntry *)rt_realloc_array(cache->entries, new_capacity, sizeof(cache->entries[0]));
        if (new_entries == 0) return;
        cache->entries = new_entries;
        cache->capacity = new_capacity;
    }
    copy = (unsigned char *)rt_malloc(size == 0U ? 1U : size);
    if (copy == 0) return;
    memcpy(copy, data, size);
    entry = &cache->entries[cache->count++];
    rt_memset(entry, 0, sizeof(*entry));
    memcpy(entry->oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
    entry->type = type;
    entry->data = copy;
    entry->size = size;
    cache->total_bytes += size;
}

static int gitd_read_object_cached(const GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitdObjectCache *object_cache, int *type_out, unsigned char **data_out, size_t *size_out) {
    GitdObjectCacheEntry *entry = gitd_object_cache_find(object_cache, oid);

    if (entry != 0) {
        unsigned char *copy = (unsigned char *)rt_malloc(entry->size == 0U ? 1U : entry->size);
        if (copy == 0) return -1;
        memcpy(copy, entry->data, entry->size);
        *type_out = entry->type;
        *data_out = copy;
        *size_out = entry->size;
        return 0;
    }
    if (git_read_object(repo, oid, pack_cache, type_out, data_out, size_out) != 0) return -1;
    gitd_object_cache_store(object_cache, oid, *type_out, *data_out, *size_out);
    return 0;
}

static int gitd_read_commit_info_cached(const GitRepo *repo, const unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitdObjectCache *object_cache, GitCommitInfo *info) {
    int type = 0;
    unsigned char *data = 0;
    size_t size = 0U;
    int result;

    if (gitd_read_object_cached(repo, commit_oid, pack_cache, object_cache, &type, &data, &size) != 0 || type != GIT_OBJECT_COMMIT) {
        rt_free(data);
        return -1;
    }
    result = git_parse_commit_info(data, size, info);
    rt_free(data);
    return result;
}

#include "gitd/tls_config.c"

#include "gitd/refs.c"

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

    if (rt_strcmp(request->method, "GET") != 0 && rt_strcmp(request->method, "HEAD") != 0) return gitd_send_text(transport, 405, "method not allowed\n");
    if (rt_strcmp(request->query, "service=git-upload-pack") == 0) {
        service = "git-upload-pack";
        content_type = "application/x-git-upload-pack-advertisement";
    } else if (rt_strcmp(request->query, "service=git-receive-pack") == 0) {
        if (options->read_only) return gitd_send_text(transport, 403, "receive-pack disabled\n");
        service = "git-receive-pack";
        content_type = "application/x-git-receive-pack-advertisement";
        receive_pack = 1;
    } else {
        return gitd_send_text(transport, 400, request->query[0] == '\0' ? "missing git service query\n" : "unsupported git service\n");
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
        rt_copy_string(caps, sizeof(caps), options->allow_delete_refs ? "report-status side-band-64k no-thin atomic delete-refs agent=newos-gitd" : "report-status side-band-64k no-thin atomic agent=newos-gitd");
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
            unsigned char peeled_oid[CRYPTO_SHA1_DIGEST_SIZE];
            const unsigned char *known_peeled_oid = gitd_ref_known_peeled_oid(&refs.refs[i]);
            int peel_result;

            if (gitd_append_ref_advertisement(&body, refs.refs[i].oid, refs.refs[i].name, 0) != 0) goto done;
            if (!receive_pack && gitd_ref_is_tag(refs.refs[i].name) && known_peeled_oid != 0) {
                if (gitd_append_peeled_ref_advertisement(&body, known_peeled_oid, refs.refs[i].name) != 0) goto done;
                continue;
            }
            peel_result = !receive_pack && gitd_ref_is_tag(refs.refs[i].name) ? gitd_peel_tag(&repo, 0, refs.refs[i].oid, peeled_oid) : 0;
            if (peel_result < 0 || (peel_result > 0 && gitd_append_peeled_ref_advertisement(&body, peeled_oid, refs.refs[i].name) != 0)) goto done;
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

static int gitd_upload_parse_fail(GitdUploadRequest *upload, const char *message) {
    if (upload != 0 && upload->parse_error == 0) upload->parse_error = message;
    return -1;
}

static int gitd_receive_parse_fail(GitdReceiveRequest *receive, const char *message) {
    if (receive != 0 && receive->parse_error == 0) receive->parse_error = message;
    return -1;
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

        if (git_parse_oid_hex_n((const char *)payload + 5U, GIT_OBJECT_HEX_SIZE, want_oid) != 0) return gitd_upload_parse_fail(upload, "malformed want line\n");
        if (upload->wants.count >= options->max_wants) return gitd_upload_parse_fail(upload, "too many wants\n");
        if (git_oid_list_push_unique(&upload->wants, want_oid) != 0) return gitd_upload_parse_fail(upload, "cannot store want\n");
        if (!upload->have_want) memcpy(upload->want_oid, want_oid, sizeof(upload->want_oid));
        upload->have_want = 1;
    } else if (line_length >= 45U && memcmp(payload, "have ", 5U) == 0) {
        unsigned char have_oid[CRYPTO_SHA1_DIGEST_SIZE];
        if (git_parse_oid_hex_n((const char *)payload + 5U, GIT_OBJECT_HEX_SIZE, have_oid) != 0) return gitd_upload_parse_fail(upload, "malformed have line\n");
        if (upload->haves.count >= options->max_haves) return gitd_upload_parse_fail(upload, "too many haves\n");
        if (git_oid_list_push_unique(&upload->haves, have_oid) != 0) return gitd_upload_parse_fail(upload, "cannot store have\n");
    } else if (gitd_pkt_line_equals(payload, payload_length, "done")) {
        upload->done = 1;
    } else if (gitd_pkt_line_starts_with(payload, payload_length, "deepen ")) {
        if (gitd_parse_size_token(payload + 7U, line_length - 7U, &upload->deepen) != 0 || upload->deepen == 0U) return gitd_upload_parse_fail(upload, "malformed deepen value\n");
    } else if (gitd_pkt_line_starts_with(payload, payload_length, "filter ")) {
        if (line_length == 16U && memcmp(payload, "filter blob:none", 16U) == 0) {
            upload->filter_blob_none = 1;
        } else {
            return gitd_upload_parse_fail(upload, "unsupported filter\n");
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

        if (git_pkt_length(body->data + pos, body->size - pos, &packet_length) != 0) return gitd_upload_parse_fail(upload, "malformed upload-pack pkt-line\n");
        pos += 4U;
        if (packet_length == GIT_PACKET_FLUSH || packet_length == GITD_PACKET_DELIM || packet_length == GITD_PACKET_RESPONSE_END) continue;
        if (packet_length < 4U || pos + packet_length - 4U > body->size) return gitd_upload_parse_fail(upload, "malformed upload-pack request\n");
        payload = body->data + pos;
        payload_length = packet_length - 4U;
        pos += payload_length;
        if (gitd_upload_request_parse_fetch_line(options, upload, payload, payload_length, 0) != 0) return -1;
    }
    return upload->have_want || (!upload->done && upload->haves.count > 0U) ? 0 : gitd_upload_parse_fail(upload, "malformed upload-pack request\n");
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

        if (git_pkt_length(body->data + pos, body->size - pos, &packet_length) != 0) return gitd_upload_parse_fail(upload, "malformed upload-pack pkt-line\n");
        pos += 4U;
        if (packet_length == GIT_PACKET_FLUSH || packet_length == GITD_PACKET_RESPONSE_END) break;
        if (packet_length == GITD_PACKET_DELIM) {
            in_arguments = 1;
            continue;
        }
        if (packet_length < 4U || pos + packet_length - 4U > body->size) return gitd_upload_parse_fail(upload, "malformed upload-pack request\n");
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
                return gitd_upload_parse_fail(upload, "malformed protocol v2 command section\n");
            }
        } else if (upload->command == GITD_UPLOAD_COMMAND_OBJECT_INFO) {
            size_t line_length = gitd_trim_pkt_line_length(payload, payload_length);

            if (gitd_pkt_line_equals(payload, payload_length, "size")) upload->object_info_size = 1;
            else if (line_length == 44U && memcmp(payload, "oid ", 4U) == 0) {
                unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
                if (git_parse_oid_hex_n((const char *)payload + 4U, GIT_OBJECT_HEX_SIZE, oid) != 0) return gitd_upload_parse_fail(upload, "malformed object-info oid\n");
                if (upload->object_info_oids.count >= options->max_wants) return gitd_upload_parse_fail(upload, "too many object-info oids\n");
                if (git_oid_list_push_unique(&upload->object_info_oids, oid) != 0) return gitd_upload_parse_fail(upload, "cannot store object-info oid\n");
            } else {
                return gitd_upload_parse_fail(upload, "malformed object-info request\n");
            }
        } else if (upload->command == GITD_UPLOAD_COMMAND_BUNDLE_URI) {
            return gitd_upload_parse_fail(upload, "malformed bundle-uri request\n");
        } else if (upload->command == GITD_UPLOAD_COMMAND_FETCH) {
            if (gitd_upload_request_parse_fetch_line(options, upload, payload, payload_length, 1) != 0) return -1;
        } else if (upload->command == GITD_UPLOAD_COMMAND_LS_REFS) {
            size_t line_length = gitd_trim_pkt_line_length(payload, payload_length);

            if (gitd_pkt_line_equals(payload, payload_length, "symrefs")) upload->ls_refs_symrefs = 1;
            else if (gitd_pkt_line_equals(payload, payload_length, "peel")) upload->ls_refs_peel = 1;
            else if (gitd_pkt_line_equals(payload, payload_length, "unborn")) continue;
            else if (gitd_pkt_line_starts_with(payload, payload_length, "ref-prefix ")) {
                if (upload->ref_prefixes.count >= options->max_ref_prefixes) return gitd_upload_parse_fail(upload, "too many ref-prefixes\n");
                if (gitd_string_list_push(&upload->ref_prefixes, (const char *)payload + 11U, line_length - 11U, options->max_ref_prefixes) != 0) return gitd_upload_parse_fail(upload, "cannot store ref-prefix\n");
            } else {
                return gitd_upload_parse_fail(upload, "malformed ls-refs request\n");
            }
        }
    }
    if (upload->command == GITD_UPLOAD_COMMAND_UNSUPPORTED) return 0;
    if (upload->command == GITD_UPLOAD_COMMAND_LS_REFS) return 0;
    if (upload->command == GITD_UPLOAD_COMMAND_OBJECT_INFO) return upload->object_info_size ? 0 : -1;
    if (upload->command == GITD_UPLOAD_COMMAND_BUNDLE_URI) return 0;
    return upload->command == GITD_UPLOAD_COMMAND_FETCH && upload->have_want ? 0 : gitd_upload_parse_fail(upload, "malformed upload-pack request\n");
}

static int gitd_collect_tree_objects_filtered(GitRepo *repo, const unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitdObjectCache *object_cache, GitOidList *objects, int include_blobs) {
    unsigned char *tree = 0;
    size_t tree_size = 0U;
    size_t pos = 0U;
    int type = 0;
    int result = -1;

    if (git_oid_list_push_unique(objects, tree_oid) != 0 || gitd_read_object_cached(repo, tree_oid, pack_cache, object_cache, &type, &tree, &tree_size) != 0 || type != GIT_OBJECT_TREE) goto done;
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
            if (gitd_collect_tree_objects_filtered(repo, oid, pack_cache, object_cache, objects, include_blobs) != 0) goto done;
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

static int gitd_collect_object_closure_filtered(GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitdObjectCache *object_cache, const GitOidList *excluded, size_t depth_remaining, int include_blobs, GitOidList *objects, GitOidList *visited, GitOidList *shallow_oids);

static int gitd_collect_commit_objects_filtered(GitRepo *repo, const unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitdObjectCache *object_cache, const GitOidList *excluded, size_t depth_remaining, int include_blobs, GitOidList *objects, GitOidList *visited, GitOidList *shallow_oids) {
    GitCommitInfo info;
    size_t i;
    int result = -1;

    if (git_oid_list_contains(visited, commit_oid) || (excluded != 0 && git_oid_list_contains(excluded, commit_oid))) return 0;
    if (git_oid_list_push_unique(visited, commit_oid) != 0 || git_oid_list_push_unique(objects, commit_oid) != 0) return -1;
    if (gitd_read_commit_info_cached(repo, commit_oid, pack_cache, object_cache, &info) != 0) return -1;
    if (gitd_collect_tree_objects_filtered(repo, info.tree_oid, pack_cache, object_cache, objects, include_blobs) != 0) goto done;
    if (depth_remaining == 1U) {
        if (info.parent_count > 0U && shallow_oids != 0 && git_oid_list_push_unique(shallow_oids, commit_oid) != 0) goto done;
    } else {
        size_t next_depth = depth_remaining > 1U ? depth_remaining - 1U : 0U;
        for (i = 0U; i < info.parent_count; ++i) {
            if (gitd_collect_commit_objects_filtered(repo, info.parents[i], pack_cache, object_cache, excluded, next_depth, include_blobs, objects, visited, shallow_oids) != 0) goto done;
        }
    }
    result = 0;
done:
    git_commit_info_destroy(&info);
    return result;
}

static int gitd_collect_tag_objects_filtered(GitRepo *repo, const unsigned char tag_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitdObjectCache *object_cache, const GitOidList *excluded, size_t depth_remaining, int include_blobs, GitOidList *objects, GitOidList *visited, GitOidList *shallow_oids) {
    unsigned char target_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char *data = 0;
    size_t size = 0U;
    int type = 0;
    int target_type = 0;
    int result = -1;

    if (git_oid_list_contains(visited, tag_oid)) return 0;
    if (git_oid_list_push_unique(visited, tag_oid) != 0 || git_oid_list_push_unique(objects, tag_oid) != 0) return -1;
    if (gitd_read_object_cached(repo, tag_oid, pack_cache, object_cache, &type, &data, &size) != 0 || type != GIT_OBJECT_TAG) goto done;
    if (gitd_parse_tag_target(data, size, target_oid, &target_type) != 0) goto done;
    (void)target_type;
    result = gitd_collect_object_closure_filtered(repo, target_oid, pack_cache, object_cache, excluded, depth_remaining, include_blobs, objects, visited, shallow_oids);
done:
    rt_free(data);
    return result;
}

static int gitd_collect_object_closure_filtered(GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitdObjectCache *object_cache, const GitOidList *excluded, size_t depth_remaining, int include_blobs, GitOidList *objects, GitOidList *visited, GitOidList *shallow_oids) {
    unsigned char *data = 0;
    size_t size = 0U;
    int type = 0;

    if (gitd_read_object_cached(repo, oid, pack_cache, object_cache, &type, &data, &size) != 0) return -1;
    rt_free(data);
    if (type == GIT_OBJECT_COMMIT) return gitd_collect_commit_objects_filtered(repo, oid, pack_cache, object_cache, excluded, depth_remaining, include_blobs, objects, visited, shallow_oids);
    if (type == GIT_OBJECT_TREE) return gitd_collect_tree_objects_filtered(repo, oid, pack_cache, object_cache, objects, include_blobs);
    if (type == GIT_OBJECT_BLOB) return include_blobs ? git_oid_list_push_unique(objects, oid) : 0;
    if (type == GIT_OBJECT_TAG) return gitd_collect_tag_objects_filtered(repo, oid, pack_cache, object_cache, excluded, depth_remaining, include_blobs, objects, visited, shallow_oids);
    return -1;
}

static int gitd_collect_explicit_want_filtered(GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitdObjectCache *object_cache, const GitOidList *excluded, size_t depth_remaining, int include_blobs, GitOidList *objects, GitOidList *visited, GitOidList *shallow_oids) {
    unsigned char *data = 0;
    size_t size = 0U;
    int type = 0;

    if (gitd_read_object_cached(repo, oid, pack_cache, object_cache, &type, &data, &size) != 0) return -1;
    rt_free(data);
    if (type == GIT_OBJECT_BLOB) return git_oid_list_push_unique(objects, oid);
    return gitd_collect_object_closure_filtered(repo, oid, pack_cache, object_cache, excluded, depth_remaining, include_blobs, objects, visited, shallow_oids);
}

static int gitd_collect_wanted_objects(GitRepo *repo, const GitPack *pack_cache, GitdObjectCache *object_cache, GitdUploadRequest *upload, const GitOidList *excluded, GitOidList *objects, GitOidList *visited) {
    size_t i;
    int include_blobs = !upload->filter_blob_none;

    for (i = 0U; i < upload->wants.count; ++i) {
        if (gitd_collect_explicit_want_filtered(repo, upload->wants.oids[i], pack_cache, object_cache, excluded, upload->deepen, include_blobs, objects, visited, &upload->shallow_oids) != 0) return -1;
    }
    return 0;
}

static int gitd_collect_reachable_commits_cached(GitRepo *repo, const unsigned char start[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitdObjectCache *object_cache, GitOidList *reachable) {
    GitOidList stack;
    int result = -1;

    rt_memset(reachable, 0, sizeof(*reachable));
    rt_memset(&stack, 0, sizeof(stack));
    if (git_oid_list_push(&stack, start) != 0) goto done;
    while (stack.count > 0U) {
        unsigned char current[CRYPTO_SHA1_DIGEST_SIZE];
        GitCommitInfo info;
        size_t parent_index;

        memcpy(current, stack.oids[stack.count - 1U], CRYPTO_SHA1_DIGEST_SIZE);
        stack.count -= 1U;
        if (git_oid_list_contains(reachable, current)) continue;
        if (gitd_read_commit_info_cached(repo, current, pack_cache, object_cache, &info) != 0) goto done;
        if (git_oid_list_push(reachable, current) != 0) {
            git_commit_info_destroy(&info);
            goto done;
        }
        for (parent_index = 0U; parent_index < info.parent_count; ++parent_index) {
            if (git_oid_list_push_unique(&stack, info.parents[parent_index]) != 0) {
                git_commit_info_destroy(&info);
                goto done;
            }
        }
        git_commit_info_destroy(&info);
    }
    result = 0;
done:
    git_oid_list_destroy(&stack);
    if (result != 0) git_oid_list_destroy(reachable);
    return result;
}

static int gitd_collect_excluded_haves(GitRepo *repo, const GitPack *pack_cache, GitdObjectCache *object_cache, const GitOidList *haves, GitOidList *excluded) {
    size_t i;

    rt_memset(excluded, 0, sizeof(*excluded));
    for (i = 0U; i < haves->count; ++i) {
        GitOidList reachable;
        size_t j;

        rt_memset(&reachable, 0, sizeof(reachable));
        if (gitd_collect_reachable_commits_cached(repo, haves->oids[i], pack_cache, object_cache, &reachable) != 0) {
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

#include "gitd/blob_delta.c"

static int gitd_compress_pack_payload(const unsigned char *payload, size_t payload_size, const char *oid_hex, GitBuffer *compressed_out);

static int gitd_append_compressed_pack_payload(GitBuffer *pack, const unsigned char *payload, size_t payload_size, const char *oid_hex) {
    GitBuffer compressed;
    int result;

    rt_memset(&compressed, 0, sizeof(compressed));
    if (gitd_compress_pack_payload(payload, payload_size, oid_hex, &compressed) != 0) return -1;
    result = git_buffer_append(pack, compressed.data, compressed.size);
    if (result != 0) tool_write_error("gitd", "cannot append compressed pack object: ", oid_hex);
    git_buffer_destroy(&compressed);
    return result;
}

static int gitd_compress_pack_payload(const unsigned char *payload, size_t payload_size, const char *oid_hex, GitBuffer *compressed_out) {
    unsigned char *compressed = 0;
    size_t compressed_capacity = compression_zlib_deflate_bound(payload_size);
    size_t compressed_size = 0U;
    int result = -1;

    rt_memset(compressed_out, 0, sizeof(*compressed_out));

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
    compressed_out->data = compressed;
    compressed_out->size = compressed_size;
    compressed_out->capacity = compressed_capacity;
    compressed = 0;
    result = 0;
done:
    rt_free(compressed);
    return result;
}

static size_t gitd_pack_object_header_size(size_t size) {
    size_t count = 1U;

    size >>= 4U;
    while (size != 0U) {
        count += 1U;
        size >>= 7U;
    }
    return count;
}

static int gitd_build_pack(GitRepo *repo, const GitPack *pack_cache, GitdObjectCache *object_cache, const GitOidList *objects, GitBuffer *pack_out) {
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
        if (gitd_read_object_cached(repo, objects->oids[i], pack_cache, object_cache, &type, &data, &size) != 0) {
            tool_write_error("gitd", "cannot read object for upload pack: ", oid_hex);
            goto done;
        }
        if (type < GIT_OBJECT_COMMIT || type > GIT_OBJECT_TAG) {
            tool_write_error("gitd", "unsupported object type for upload pack: ", oid_hex);
            rt_free(data);
            goto done;
        }
        if (type == GIT_OBJECT_BLOB) {
            GitBuffer compressed_full;
            GitBuffer best_delta;
            GitdBlobBase *candidates[GITD_DELTA_CANDIDATES];
            GitdBlobBase *best_base = 0;
            size_t candidate_count;
            size_t candidate_index;
            size_t best_delta_size = 0U;
            size_t best_pack_size;

            rt_memset(&compressed_full, 0, sizeof(compressed_full));
            rt_memset(&best_delta, 0, sizeof(best_delta));
            if (gitd_compress_pack_payload(data, size, oid_hex, &compressed_full) != 0) {
                rt_free(data);
                goto done;
            }
            best_pack_size = gitd_pack_object_header_size(size) + compressed_full.size;
            candidate_count = gitd_collect_blob_delta_candidates(&blob_bases, data, size, candidates, GITD_DELTA_CANDIDATES);
            for (candidate_index = 0U; candidate_index < candidate_count; ++candidate_index) {
                GitBuffer delta;
                GitBuffer compressed_delta;
                GitdBlobBase *candidate = candidates[candidate_index];

                rt_memset(&delta, 0, sizeof(delta));
                rt_memset(&compressed_delta, 0, sizeof(compressed_delta));
                if (candidate != 0 && gitd_build_blob_delta(candidate->data, candidate->size, data, size, &delta) == 0 && delta.size + CRYPTO_SHA1_DIGEST_SIZE + 8U < size && gitd_compress_pack_payload(delta.data, delta.size, oid_hex, &compressed_delta) == 0) {
                    size_t delta_pack_size = gitd_pack_object_header_size(delta.size) + CRYPTO_SHA1_DIGEST_SIZE + compressed_delta.size;

                    if (delta_pack_size < best_pack_size) {
                        git_buffer_destroy(&best_delta);
                        best_delta = compressed_delta;
                        rt_memset(&compressed_delta, 0, sizeof(compressed_delta));
                        best_delta_size = delta.size;
                        best_pack_size = delta_pack_size;
                        best_base = candidate;
                    }
                }
                git_buffer_destroy(&compressed_delta);
                git_buffer_destroy(&delta);
            }
            if (best_base != 0) {
                if (git_pack_append_object_header(&pack, GIT_OBJECT_REF_DELTA, best_delta_size) != 0 || git_buffer_append(&pack, best_base->oid, sizeof(best_base->oid)) != 0 || git_buffer_append(&pack, best_delta.data, best_delta.size) != 0) {
                    git_buffer_destroy(&best_delta);
                    git_buffer_destroy(&compressed_full);
                    rt_free(data);
                    goto done;
                }
            } else if (git_pack_append_object_header(&pack, type, size) != 0 || git_buffer_append(&pack, compressed_full.data, compressed_full.size) != 0) {
                git_buffer_destroy(&best_delta);
                git_buffer_destroy(&compressed_full);
                rt_free(data);
                goto done;
            }
            git_buffer_destroy(&best_delta);
            git_buffer_destroy(&compressed_full);
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

static int gitd_append_v2_ref_line(GitBuffer *out, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const char *name, const char *symref_target, int include_symrefs, const unsigned char *peeled_oid) {
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
    if (peeled_oid != 0) {
        char peeled_hex[GIT_OBJECT_HEX_SIZE + 1U];
        git_format_oid_hex(peeled_oid, peeled_hex);
        if (tool_byte_buffer_append_cstr(&line, " peeled:") != 0 || tool_byte_buffer_append_cstr(&line, peeled_hex) != 0) {
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
        if (gitd_ls_refs_prefix_matches(upload, "HEAD") && gitd_append_v2_ref_line(out, head_oid, "HEAD", repo->head_ref, upload->ls_refs_symrefs, 0) != 0) goto done;
        for (i = 0U; i < refs.count; ++i) {
            unsigned char peeled_oid[CRYPTO_SHA1_DIGEST_SIZE];
            const unsigned char *peeled = gitd_ref_known_peeled_oid(&refs.refs[i]);
            int peel_result;

            if (!gitd_ls_refs_prefix_matches(upload, refs.refs[i].name)) continue;
            if (!upload->ls_refs_peel || !gitd_ref_is_tag(refs.refs[i].name)) peeled = 0;
            peel_result = upload->ls_refs_peel && gitd_ref_is_tag(refs.refs[i].name) && peeled == 0 ? gitd_peel_tag(repo, 0, refs.refs[i].oid, peeled_oid) : 0;
            if (peel_result < 0) goto done;
            if (peel_result > 0) peeled = peeled_oid;
            if (gitd_append_v2_ref_line(out, refs.refs[i].oid, refs.refs[i].name, 0, upload->ls_refs_symrefs, peeled) != 0) goto done;
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

static int gitd_append_v2_object_info_response(GitRepo *repo, const GitPack *pack_cache, GitdObjectCache *object_cache, const GitdUploadRequest *upload, GitBuffer *out) {
    size_t index;

    if (!upload->object_info_size) return tool_byte_buffer_append_cstr(out, "0000");
    if (git_append_pkt_line(out, "size\n") != 0) return -1;
    for (index = 0U; index < upload->object_info_oids.count; ++index) {
        int type = 0;
        unsigned char *data = 0;
        size_t size = 0U;

        if (gitd_read_object_cached(repo, upload->object_info_oids.oids[index], pack_cache, object_cache, &type, &data, &size) == 0) {
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

static int gitd_object_exists(GitRepo *repo, const GitPack *pack_cache, GitdObjectCache *object_cache, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]);

static int gitd_append_v2_acknowledgments(GitRepo *repo, const GitPack *pack_cache, GitdObjectCache *object_cache, const GitdUploadRequest *upload, GitBuffer *out) {
    size_t index;
    int acknowledged = 0;

    if (upload->haves.count == 0U) return 0;
    if (git_append_pkt_line(out, "acknowledgments\n") != 0) return -1;
    for (index = 0U; index < upload->haves.count; ++index) {
        if (gitd_object_exists(repo, pack_cache, object_cache, upload->haves.oids[index])) {
            char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
            GitBuffer line;
            int append_result;

            git_format_oid_hex(upload->haves.oids[index], oid_hex);
            rt_memset(&line, 0, sizeof(line));
            append_result = tool_byte_buffer_append_cstr(&line, "ACK ") != 0 ||
                            tool_byte_buffer_append_cstr(&line, oid_hex) != 0 ||
                            tool_byte_buffer_append_char(&line, '\n') != 0 ||
                            git_append_pkt_data(out, line.data, line.size) != 0 ? -1 : 0;
            git_buffer_destroy(&line);
            if (append_result != 0) return -1;
            acknowledged = 1;
        }
    }
    if (!acknowledged && git_append_pkt_line(out, "NAK\n") != 0) return -1;
    if (git_append_pkt_line(out, "ready\n") != 0) return -1;
    return tool_byte_buffer_append_cstr(out, "0001");
}

static int gitd_append_v2_fetch_response(GitRepo *repo, const GitPack *pack_cache, GitdObjectCache *object_cache, GitBuffer *out, const GitdUploadRequest *upload, const GitBuffer *pack) {
    size_t pos = 0U;
    size_t i;

    if (gitd_append_v2_acknowledgments(repo, pack_cache, object_cache, upload, out) != 0) return -1;
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

static int gitd_object_exists(GitRepo *repo, const GitPack *pack_cache, GitdObjectCache *object_cache, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    int type = 0;
    unsigned char *data = 0;
    size_t size = 0U;

    if (gitd_read_object_cached(repo, oid, pack_cache, object_cache, &type, &data, &size) != 0) return 0;
    rt_free(data);
    return type >= GIT_OBJECT_COMMIT && type <= GIT_OBJECT_TAG;
}

static int gitd_append_v1_negotiation_response(GitRepo *repo, const GitPack *pack_cache, GitdObjectCache *object_cache, const GitdUploadRequest *upload, GitBuffer *out) {
    size_t index;
    int acknowledged = 0;

    for (index = 0U; index < upload->haves.count; ++index) {
        if (gitd_object_exists(repo, pack_cache, object_cache, upload->haves.oids[index])) {
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
    GitdObjectCache object_cache;
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
    rt_memset(&object_cache, 0, sizeof(object_cache));
    rt_memset(&pack, 0, sizeof(pack));
    rt_memset(&response, 0, sizeof(response));
    have_pack = git_load_pack_cache(repo, &pack_cache) == 0;
    if (upload->command == GITD_UPLOAD_COMMAND_OBJECT_INFO) {
        result = gitd_append_v2_object_info_response(repo, have_pack ? &pack_cache : 0, &object_cache, upload, &response) == 0 ? gitd_send_body(transport, 200, "application/x-git-upload-pack-result", response.data, response.size) : -1;
        goto done;
    }
    if (!upload->done && !v2) {
        result = gitd_append_v1_negotiation_response(repo, have_pack ? &pack_cache : 0, &object_cache, upload, &response) == 0 ? gitd_send_body(transport, 200, "application/x-git-upload-pack-result", response.data, response.size) : -1;
        goto done;
    }
    if (gitd_collect_excluded_haves(repo, have_pack ? &pack_cache : 0, &object_cache, &upload->haves, &excluded) != 0) goto done;
    if (gitd_collect_wanted_objects(repo, have_pack ? &pack_cache : 0, &object_cache, upload, excluded.count > 0U ? &excluded : 0, &objects, &visited) != 0 || objects.count > options->max_objects || upload->shallow_oids.count > options->max_shallows) goto done;
    if (gitd_build_pack(repo, have_pack ? &pack_cache : 0, &object_cache, &objects, &pack) != 0 || pack.size > options->max_pack_bytes) goto done;
    if (v2) {
        if (gitd_append_v2_fetch_response(repo, have_pack ? &pack_cache : 0, &object_cache, &response, upload, &pack) != 0) goto done;
    } else if (gitd_append_sideband_pack(&response, &pack, upload->sideband) != 0) {
        goto done;
    }
    result = gitd_send_body(transport, 200, "application/x-git-upload-pack-result", response.data, response.size);
done:
    if (have_pack) git_pack_destroy(&pack_cache);
    git_oid_list_destroy(&objects);
    git_oid_list_destroy(&visited);
    git_oid_list_destroy(&excluded);
    gitd_object_cache_destroy(&object_cache);
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
        const char *parse_error = upload.parse_error != 0 ? upload.parse_error : "malformed upload-pack request\n";
        gitd_log_message(options, "warn", "upload-pack rejected", parse_error);
        git_buffer_destroy(&decoded_body);
        gitd_upload_request_destroy(&upload);
        return gitd_send_text(transport, 400, parse_error);
    }
    result = gitd_handle_upload_pack_command(transport, options, &repo, &upload, v2);
    gitd_upload_request_destroy(&upload);
    git_buffer_destroy(&decoded_body);
    return result;
}

#include "gitd/receive_pack.c"

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
        gitd_log_message(&connection->server->options, "warn", "bad request", 0);
        (void)gitd_send_text(&connection->transport, 400, "bad request\n");
        gitd_connection_destroy(connection);
        return;
    }
    if (!complete) {
        return;
    }
    (void)rt_io_loop_remove(&connection->server->loop, fd);
    connection->transport.head_only = rt_strcmp(connection->request.method, "HEAD") == 0;
    connection->transport.last_status = 0;
    connection->transport.last_response_bytes = 0U;
    {
        int dispatch_result = gitd_dispatch_request(&connection->transport, &connection->server->options, &connection->request, &connection->body);
        gitd_log_request_result(&connection->server->options, &connection->request, &connection->transport, dispatch_result);
    }
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
        gitd_log_message(&server->options, "error", "out of memory accepting connection", 0);
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
            gitd_log_message(&server->options, "warn", "TLS handshake failed", tls13_server_last_error(&connection->transport.tls));
            connection->transport.fd = -1;
            (void)platform_close(client_fd);
            rt_free(connection);
            return -1;
        }
    }
    if (rt_io_loop_add(&server->loop, client_fd, RT_IO_READ, gitd_connection_ready, connection) != 0) {
        gitd_log_message(&server->options, "error", "cannot register accepted connection", 0);
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