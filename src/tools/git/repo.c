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

static int git_relative_path(const GitRepo *repo, const char *path, char *buffer, size_t buffer_size);
static int git_path_parent(char *path);
static const char *git_path_basename(const char *path);

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

static int git_ignore_add_line(GitIgnoreList *ignores, const char *base, const char *line, size_t line_length) {
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
    pattern.base = git_strdup_n(base, rt_strlen(base));
    if (pattern.base == 0) {
        rt_free(copy);
        return -1;
    }
    if (git_ignore_push(ignores, &pattern) != 0) {
        rt_free(pattern.base);
        rt_free(copy);
        return -1;
    }
    return 0;
}

static int git_ignore_load_file_base(GitIgnoreList *ignores, const char *path, const char *base) {
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
        if (git_ignore_add_line(ignores, base, (const char *)data + start, end - start) != 0) {
            rt_free(data);
            return -1;
        }
    }
    rt_free(data);
    return 0;
}

static int git_ignore_load_file(GitIgnoreList *ignores, const char *path) {
    return git_ignore_load_file_base(ignores, path, "");
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
    const char *local = relative;
    size_t base_length = pattern->base == 0 ? 0U : rt_strlen(pattern->base);

    if (base_length > 0U) {
        if (rt_strncmp(relative, pattern->base, base_length) != 0) {
            return 0;
        }
        local = relative + base_length;
        if (local[0] == '\0') {
            return 0;
        }
    }

    if (pattern->directory_only && !is_directory) {
        return 0;
    }
    if (match_pattern[0] == '/') {
        match_pattern += 1;
        return pattern->has_wildcard ? tool_wildcard_match(match_pattern, local) : rt_strcmp(match_pattern, local) == 0;
    }
    if (pattern->has_slash) {
        return pattern->has_wildcard ? tool_wildcard_match(match_pattern, local) : rt_strcmp(match_pattern, local) == 0;
    }
    return pattern->has_wildcard ? tool_wildcard_match(match_pattern, git_path_basename(local)) : rt_strcmp(match_pattern, git_path_basename(local)) == 0;
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
static int git_index_is_sorted(const GitIndex *index);
static int git_write_index_file(const GitRepo *repo, GitIndex *index);
static int git_parse_pack(const unsigned char *data, size_t size, GitPack *pack);
static int git_resolve_pack_deltas(GitPack *pack);
