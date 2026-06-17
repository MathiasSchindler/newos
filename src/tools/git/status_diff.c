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
        unsigned int extended_flags = 0U;
        size_t path_start;
        size_t path_length;
        size_t entry_start = pos;
        size_t entry_length;
        size_t header_length;

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
        if ((flags & GIT_INDEX_FLAG_EXTENDED) != 0U) {
            if (version < 3U || path_start + 2U > size) {
                git_index_destroy(index);
                rt_free(data);
                return -1;
            }
            extended_flags = git_read_u16_be_raw(data + path_start);
            entry.intent_to_add = (extended_flags & GIT_INDEX_EXTENDED_INTENT_TO_ADD) != 0U;
            path_start += 2U;
        }
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
        header_length = path_start - entry_start;
        entry_length = header_length + path_length + 1U;
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

static int git_status_style_for_code(const char *code) {
    if (rt_strcmp(code, " M") == 0) {
        return TOOL_STYLE_YELLOW;
    }
    if (rt_strcmp(code, " D") == 0) {
        return TOOL_STYLE_RED;
    }
    if (rt_strcmp(code, " A") == 0) {
        return TOOL_STYLE_GREEN;
    }
    if (rt_strcmp(code, "??") == 0) {
        return TOOL_STYLE_GREEN;
    }
    return TOOL_STYLE_PLAIN;
}

static int git_write_status_text(int fd, int color_mode, int style, const char *text) {
    if (style != TOOL_STYLE_PLAIN && tool_should_use_color_fd(fd, color_mode)) {
        tool_style_begin(fd, color_mode, style);
        if (rt_write_cstr(fd, text) != 0) {
            tool_style_end(fd, color_mode);
            return -1;
        }
        tool_style_end(fd, color_mode);
        return 0;
    }
    return rt_write_cstr(fd, text);
}

static int git_write_status_line(const char *code, const char *path, int short_output, int color_mode) {
    int style = git_status_style_for_code(code);

    if (!short_output) {
        if (rt_strcmp(code, " M") == 0) {
            if (git_write_status_text(1, color_mode, style, "modified: ") != 0) return -1;
        } else if (rt_strcmp(code, " D") == 0) {
            if (git_write_status_text(1, color_mode, style, "deleted: ") != 0) return -1;
        } else if (rt_strcmp(code, " A") == 0) {
            if (git_write_status_text(1, color_mode, style, "new file: ") != 0) return -1;
        } else if (rt_strcmp(code, "??") == 0) {
            if (git_write_status_text(1, color_mode, style, "untracked: ") != 0) return -1;
        } else {
            rt_write_cstr(1, code);
            rt_write_char(1, ' ');
        }
        return rt_write_line(1, path);
    }
    if (style != TOOL_STYLE_PLAIN && tool_should_use_color_fd(1, color_mode)) {
        tool_style_begin(1, color_mode, style);
        if (rt_write_cstr(1, code) != 0 || rt_write_char(1, ' ') != 0 || rt_write_cstr(1, path) != 0) {
            tool_style_end(1, color_mode);
            return -1;
        }
        tool_style_end(1, color_mode);
        return rt_write_char(1, '\n');
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

static int git_status_tracked(const GitRepo *repo, const GitIndex *index, int short_output, int color_mode, int *saw_change) {
    size_t i;

    for (i = 0U; i < index->count; ++i) {
        int modified = git_entry_is_modified(repo, &index->entries[i]);

        if (modified < 0) {
            if (git_write_status_line(" D", index->entries[i].path, short_output, color_mode) != 0) {
                return -1;
            }
            *saw_change = 1;
        } else if (index->entries[i].intent_to_add) {
            if (git_write_status_line(" A", index->entries[i].path, short_output, color_mode) != 0) {
                return -1;
            }
            *saw_change = 1;
        } else if (modified > 0) {
            if (git_write_status_line(" M", index->entries[i].path, short_output, color_mode) != 0) {
                return -1;
            }
            *saw_change = 1;
        }
    }
    return 0;
}

static int git_pathspec_matches(const char *path, char **pathspecs, int pathspec_count);

typedef int (*GitUntrackedPathCallback)(const char *path, void *user_data);

typedef struct {
    const GitRepo *repo;
    const GitIndex *index;
    const GitIgnoreList *ignores;
    char **pathspecs;
    int pathspec_count;
    char **paths;
    size_t count;
    size_t capacity;
} GitUntrackedWalk;

static void git_untracked_walk_destroy(GitUntrackedWalk *walk) {
    size_t i;

    for (i = 0U; i < walk->count; ++i) {
        rt_free(walk->paths[i]);
    }
    rt_free(walk->paths);
    walk->paths = 0;
    walk->count = 0U;
    walk->capacity = 0U;
}

static int git_untracked_walk_push(GitUntrackedWalk *walk, const char *path) {
    char **new_paths;
    char *copy;
    size_t new_capacity;

    if (walk->count == walk->capacity) {
        new_capacity = walk->capacity == 0U ? 32U : walk->capacity * 2U;
        new_paths = (char **)rt_realloc_array(walk->paths, new_capacity, sizeof(walk->paths[0]));
        if (new_paths == 0) {
            return -1;
        }
        walk->paths = new_paths;
        walk->capacity = new_capacity;
    }
    copy = git_strdup_n(path, rt_strlen(path));
    if (copy == 0) {
        return -1;
    }
    walk->paths[walk->count++] = copy;
    return 0;
}

static int git_compare_path_strings(const void *left, const void *right) {
    const char *const *left_path = (const char *const *)left;
    const char *const *right_path = (const char *const *)right;

    return rt_strcmp(*left_path, *right_path);
}

static int git_untracked_walk_callback(const char *path, const PlatformDirEntry *entry, int depth, ToolWalkControl *control, void *user_data) {
    GitUntrackedWalk *walk = (GitUntrackedWalk *)user_data;
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
    if (walk->ignores != 0 && git_ignore_matches(walk->ignores, relative, entry->is_dir)) {
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
    if (!git_pathspec_matches(relative, walk->pathspecs, walk->pathspec_count)) {
        return 0;
    }
    return git_untracked_walk_push(walk, relative);
}

static int git_for_each_untracked_path(const GitRepo *repo, const GitIndex *index, const GitIgnoreList *ignores, char **pathspecs, int pathspec_count, GitUntrackedPathCallback callback, void *callback_data) {
    GitUntrackedWalk walk;
    ToolWalkOptions options;
    size_t i;
    int result = -1;

    rt_memset(&walk, 0, sizeof(walk));
    walk.repo = repo;
    walk.index = index;
    walk.ignores = ignores;
    walk.pathspecs = pathspecs;
    walk.pathspec_count = pathspec_count;
    options.min_depth = 0;
    options.max_depth = -1;
    if (tool_walk_path(repo->work_tree, &options, git_untracked_walk_callback, &walk) != 0) {
        goto done;
    }
    if (walk.count > 1U) {
        rt_sort(walk.paths, walk.count, sizeof(walk.paths[0]), git_compare_path_strings);
    }
    for (i = 0U; i < walk.count; ++i) {
        if (callback(walk.paths[i], callback_data) != 0) {
            goto done;
        }
    }
    result = 0;
done:
    git_untracked_walk_destroy(&walk);
    return result;
}

static int git_status_untracked_emit(const char *path, void *user_data) {
    GitStatusWalk *walk = (GitStatusWalk *)user_data;

    if (git_write_status_line("??", path, walk->porcelain, walk->color_mode) != 0) {
        return -1;
    }
    walk->saw_change = 1;
    return 0;
}

static int git_status_untracked(const GitRepo *repo, const GitIndex *index, const GitIgnoreList *ignores, int short_output, int color_mode, int *saw_change) {
    GitStatusWalk walk;
    int result;

    walk.porcelain = short_output;
    walk.color_mode = color_mode;
    walk.saw_change = 0;
    result = git_for_each_untracked_path(repo, index, ignores, 0, 0, git_status_untracked_emit, &walk);
    if (result != 0) {
        return -1;
    }
    if (walk.saw_change) {
        *saw_change = 1;
    }
    return 0;
}

static void git_diff_stat_list_destroy(GitDiffStatList *list) {
    rt_free(list->entries);
    rt_memset(list, 0, sizeof(*list));
}

static int git_diff_stat_list_push(GitDiffStatList *list, const char *path, size_t insertions, size_t deletions) {
    GitDiffStat *new_entries;
    size_t new_capacity;

    if (insertions == 0U && deletions == 0U) {
        return 0;
    }
    if (list->count == list->capacity) {
        new_capacity = list->capacity == 0U ? 16U : list->capacity * 2U;
        new_entries = (GitDiffStat *)rt_realloc_array(list->entries, new_capacity, sizeof(list->entries[0]));
        if (new_entries == 0) {
            return -1;
        }
        list->entries = new_entries;
        list->capacity = new_capacity;
    }
    list->entries[list->count].path = path;
    list->entries[list->count].insertions = insertions;
    list->entries[list->count].deletions = deletions;
    list->count += 1U;
    return 0;
}

static int git_pathspec_matches(const char *path, char **pathspecs, int pathspec_count) {
    int i;

    if (pathspec_count == 0) {
        return 1;
    }
    for (i = 0; i < pathspec_count; ++i) {
        size_t length = rt_strlen(pathspecs[i]);

        if (rt_strcmp(path, pathspecs[i]) == 0 || (length > 0U && rt_strncmp(path, pathspecs[i], length) == 0 && path[length] == '/')) {
            return 1;
        }
    }
    return 0;
}

static int git_split_diff_lines(const unsigned char *data, size_t size, GitDiffLine **lines_out, size_t *count_out) {
    GitDiffLine *lines;
    size_t count = 0U;
    size_t pos = 0U;
    size_t index = 0U;

    *lines_out = 0;
    *count_out = 0U;
    while (pos < size) {
        count += 1U;
        while (pos < size && data[pos] != '\n') {
            pos += 1U;
        }
        if (pos < size) {
            pos += 1U;
        }
    }
    if (count == 0U) {
        return 0;
    }
    lines = (GitDiffLine *)rt_malloc_array(count, sizeof(lines[0]));
    if (lines == 0) {
        return -1;
    }
    pos = 0U;
    while (pos < size) {
        size_t start = pos;
        size_t end;

        while (pos < size && data[pos] != '\n') {
            pos += 1U;
        }
        end = pos;
        if (end > start && data[end - 1U] == '\r') {
            end -= 1U;
        }
        lines[index].data = data + start;
        lines[index].length = end - start;
        index += 1U;
        if (pos < size) {
            pos += 1U;
        }
    }
    *lines_out = lines;
    *count_out = count;
    return 0;
}

static int git_diff_lines_equal(const GitDiffLine *left, const GitDiffLine *right) {
    return left->length == right->length && memcmp(left->data, right->data, left->length) == 0;
}

static int git_diff_lcs_count(const GitDiffLine *old_lines, size_t old_count, const GitDiffLine *new_lines, size_t new_count, size_t *common_out) {
    size_t *previous;
    size_t *current;
    size_t i;
    size_t j;

    *common_out = 0U;
    if (old_count == 0U || new_count == 0U) {
        return 0;
    }
    previous = (size_t *)rt_malloc_array(new_count + 1U, sizeof(previous[0]));
    current = (size_t *)rt_malloc_array(new_count + 1U, sizeof(current[0]));
    if (previous == 0 || current == 0) {
        rt_free(previous);
        rt_free(current);
        return -1;
    }
    rt_memset(previous, 0, (new_count + 1U) * sizeof(previous[0]));
    rt_memset(current, 0, (new_count + 1U) * sizeof(current[0]));
    for (i = 0U; i < old_count; ++i) {
        for (j = 0U; j < new_count; ++j) {
            if (git_diff_lines_equal(&old_lines[i], &new_lines[j])) {
                current[j + 1U] = previous[j] + 1U;
            } else {
                current[j + 1U] = previous[j + 1U] > current[j] ? previous[j + 1U] : current[j];
            }
        }
        {
            size_t *swap = previous;
            previous = current;
            current = swap;
        }
        rt_memset(current, 0, (new_count + 1U) * sizeof(current[0]));
    }
    *common_out = previous[new_count];
    rt_free(previous);
    rt_free(current);
    return 0;
}

static int git_diff_count_line_changes(const unsigned char *old_data, size_t old_size, const unsigned char *new_data, size_t new_size, size_t *insertions_out, size_t *deletions_out) {
    GitDiffLine *old_lines = 0;
    GitDiffLine *new_lines = 0;
    size_t old_count = 0U;
    size_t new_count = 0U;
    size_t common = 0U;
    int result = -1;

    *insertions_out = 0U;
    *deletions_out = 0U;
    if (git_split_diff_lines(old_data, old_size, &old_lines, &old_count) != 0 || git_split_diff_lines(new_data, new_size, &new_lines, &new_count) != 0) {
        goto done;
    }
    if (git_diff_lcs_count(old_lines, old_count, new_lines, new_count, &common) != 0) {
        goto done;
    }
    *insertions_out = new_count >= common ? new_count - common : 0U;
    *deletions_out = old_count >= common ? old_count - common : 0U;
    result = 0;
done:
    rt_free(old_lines);
    rt_free(new_lines);
    return result;
}

static int git_read_worktree_blob(const GitRepo *repo, const GitIndexEntry *entry, unsigned char **data_out, size_t *size_out) {
    char full_path[GIT_PATH_CAPACITY];
    PlatformDirEntry info;

    *data_out = 0;
    *size_out = 0U;
    if (git_join(full_path, sizeof(full_path), repo->work_tree, entry->path) != 0 || platform_get_path_info(full_path, &info) != 0 || info.is_dir) {
        return -1;
    }
    if (entry->mode == GIT_MODE_SYMLINK) {
        char link_target[GIT_PATH_CAPACITY];
        size_t length;

        if (platform_read_symlink(full_path, link_target, sizeof(link_target)) != 0) {
            return -1;
        }
        length = rt_strlen(link_target);
        *data_out = (unsigned char *)rt_malloc(length == 0U ? 1U : length);
        if (*data_out == 0) {
            return -1;
        }
        memcpy(*data_out, link_target, length);
        *size_out = length;
        return 0;
    }
    if (!git_index_mode_is_regular(entry->mode)) {
        return -1;
    }
    return git_read_file(full_path, data_out, size_out);
}

static int git_collect_diff_stat_entry(const GitRepo *repo, const GitIndexEntry *entry, const GitPack *pack_cache, GitDiffStatList *stats) {
    unsigned char *old_data = 0;
    unsigned char *new_data = 0;
    size_t old_size = 0U;
    size_t new_size = 0U;
    size_t insertions = 0U;
    size_t deletions = 0U;
    int old_type = 0;
    int modified;
    int result = -1;

    modified = git_entry_is_modified(repo, entry);
    if (modified == 0) {
        return 0;
    }
    if (entry->intent_to_add) {
        old_data = 0;
        old_size = 0U;
    } else if (git_read_object(repo, entry->oid, pack_cache, &old_type, &old_data, &old_size) != 0 || old_type != GIT_OBJECT_BLOB) {
        goto done;
    }
    if (modified < 0) {
        if (git_diff_count_line_changes(old_data, old_size, (const unsigned char *)"", 0U, &insertions, &deletions) != 0) {
            goto done;
        }
    } else {
        if (git_read_worktree_blob(repo, entry, &new_data, &new_size) != 0 || git_diff_count_line_changes(old_data, old_size, new_data, new_size, &insertions, &deletions) != 0) {
            goto done;
        }
    }
    result = git_diff_stat_list_push(stats, entry->path, insertions, deletions);
done:
    rt_free(old_data);
    rt_free(new_data);
    return result;
}

static size_t git_decimal_width(size_t value) {
    size_t width = 1U;

    while (value >= 10U) {
        value /= 10U;
        width += 1U;
    }
    return width;
}

static int git_write_spaces(size_t count) {
    while (count > 0U) {
        if (rt_write_char(1, ' ') != 0) {
            return -1;
        }
        count -= 1U;
    }
    return 0;
}

static int git_write_size(size_t value) {
    char digits[32];

    rt_unsigned_to_string((unsigned long long)value, digits, sizeof(digits));
    return rt_write_cstr(1, digits);
}

static void git_diff_scaled_bar(size_t insertions, size_t deletions, size_t max_changes, size_t graph_width, size_t *plus_out, size_t *minus_out) {
    size_t total = insertions + deletions;
    size_t display_total = total;
    size_t pluses;
    size_t minuses;

    if (total == 0U) {
        *plus_out = 0U;
        *minus_out = 0U;
        return;
    }
    if (max_changes <= graph_width) {
        *plus_out = insertions;
        *minus_out = deletions;
        return;
    }
    display_total = (total * graph_width + max_changes / 2U) / max_changes;
    if (display_total == 0U) {
        display_total = 1U;
    }
    if (insertions > 0U && deletions > 0U && display_total < 2U) {
        display_total = 2U;
    }
    pluses = insertions == 0U ? 0U : (insertions * display_total + total / 2U) / total;
    minuses = display_total >= pluses ? display_total - pluses : 0U;
    if (insertions > 0U && pluses == 0U) pluses = 1U;
    if (deletions > 0U && minuses == 0U) minuses = 1U;
    while (pluses + minuses > graph_width) {
        if (pluses >= minuses && pluses > 1U) {
            pluses -= 1U;
        } else if (minuses > 1U) {
            minuses -= 1U;
        } else {
            break;
        }
    }
    *plus_out = pluses;
    *minus_out = minuses;
}

static int git_write_repeated_char_fd(int fd, char ch, size_t count) {
    while (count > 0U) {
        if (rt_write_char(fd, ch) != 0) {
            return -1;
        }
        count -= 1U;
    }
    return 0;
}

static int git_write_diff_stat_bar(size_t pluses, size_t minuses, int color_mode) {
    int use_color = tool_should_use_color_fd(1, color_mode);

    if (pluses > 0U) {
        if (use_color) {
            tool_style_begin(1, color_mode, TOOL_STYLE_GREEN);
        }
        if (git_write_repeated_char_fd(1, '+', pluses) != 0) {
            if (use_color) tool_style_end(1, color_mode);
            return -1;
        }
        if (use_color) {
            tool_style_end(1, color_mode);
        }
    }
    if (minuses > 0U) {
        if (use_color) {
            tool_style_begin(1, color_mode, TOOL_STYLE_RED);
        }
        if (git_write_repeated_char_fd(1, '-', minuses) != 0) {
            if (use_color) tool_style_end(1, color_mode);
            return -1;
        }
        if (use_color) {
            tool_style_end(1, color_mode);
        }
    }
    return 0;
}

static int git_render_diff_stat(const GitDiffStatList *stats, int color_mode) {
    size_t path_width = 0U;
    size_t changes_width = 1U;
    size_t total_insertions = 0U;
    size_t total_deletions = 0U;
    size_t max_changes = 0U;
    size_t graph_width = 47U;
    size_t prefix_width;
    size_t i;

    if (stats->count == 0U) {
        return 0;
    }
    for (i = 0U; i < stats->count; ++i) {
        size_t path_length = rt_strlen(stats->entries[i].path);
        size_t changes = stats->entries[i].insertions + stats->entries[i].deletions;
        size_t width = git_decimal_width(changes);

        if (path_length > path_width) path_width = path_length;
        if (width > changes_width) changes_width = width;
        if (changes > max_changes) max_changes = changes;
        total_insertions += stats->entries[i].insertions;
        total_deletions += stats->entries[i].deletions;
    }
    prefix_width = 1U + path_width + 3U + changes_width + 1U;
    if (prefix_width + graph_width > 70U) {
        graph_width = prefix_width >= 60U ? 10U : 70U - prefix_width;
    }
    for (i = 0U; i < stats->count; ++i) {
        size_t changes = stats->entries[i].insertions + stats->entries[i].deletions;
        size_t pluses;
        size_t minuses;

        git_diff_scaled_bar(stats->entries[i].insertions, stats->entries[i].deletions, max_changes, graph_width, &pluses, &minuses);
        if (rt_write_char(1, ' ') != 0 || rt_write_cstr(1, stats->entries[i].path) != 0 || git_write_spaces(path_width - rt_strlen(stats->entries[i].path)) != 0 || rt_write_cstr(1, " | ") != 0 || git_write_spaces(changes_width - git_decimal_width(changes)) != 0 || git_write_size(changes) != 0 || rt_write_char(1, ' ') != 0 || git_write_diff_stat_bar(pluses, minuses, color_mode) != 0 || rt_write_char(1, '\n') != 0) {
            return -1;
        }
    }
    if (rt_write_char(1, ' ') != 0 || git_write_size(stats->count) != 0 || rt_write_cstr(1, stats->count == 1U ? " file changed" : " files changed") != 0) {
        return -1;
    }
    if (total_insertions > 0U) {
        if (rt_write_cstr(1, ", ") != 0 || git_write_size(total_insertions) != 0 || rt_write_cstr(1, total_insertions == 1U ? " insertion(+)" : " insertions(+)") != 0) {
            return -1;
        }
    }
    if (total_deletions > 0U) {
        if (rt_write_cstr(1, ", ") != 0 || git_write_size(total_deletions) != 0 || rt_write_cstr(1, total_deletions == 1U ? " deletion(-)" : " deletions(-)") != 0) {
            return -1;
        }
    }
    return rt_write_char(1, '\n');
}

