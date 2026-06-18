static int git_buffer_append_unsigned(GitBuffer *buffer, unsigned long long value) {
    char digits[32];

    rt_unsigned_to_string(value, digits, sizeof(digits));
    return git_buffer_append_cstr(buffer, digits);
}

static const char *git_identity_value(const char *primary, const char *fallback, const char *default_value) {
    const char *value = platform_getenv(primary);

    if (value != 0 && value[0] != '\0') {
        return value;
    }
    if (fallback != 0) {
        value = platform_getenv(fallback);
        if (value != 0 && value[0] != '\0') {
            return value;
        }
    }
    return default_value;
}

static int git_commit_append_identity_line(GitBuffer *commit, const char *prefix, const char *name_env, const char *email_env, long long epoch_seconds) {
    const char *name = git_identity_value(name_env, "USER", "newos");
    const char *email = git_identity_value(email_env, 0, "newos@example.invalid");
    unsigned long long timestamp = epoch_seconds < 0 ? 0ULL : (unsigned long long)epoch_seconds;

    if (git_buffer_append_cstr(commit, prefix) != 0 ||
        git_buffer_append_char(commit, ' ') != 0 ||
        git_buffer_append_cstr(commit, name) != 0 ||
        git_buffer_append_cstr(commit, " <") != 0 ||
        git_buffer_append_cstr(commit, email) != 0 ||
        git_buffer_append_cstr(commit, "> ") != 0 ||
        git_buffer_append_unsigned(commit, timestamp) != 0 ||
        git_buffer_append_cstr(commit, " +0000\n") != 0) {
        return -1;
    }
    return 0;
}

static int git_write_commit_from_tree_with_parent(GitRepo *repo, const unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE], const unsigned char *parent_oid_or_null, const char *message, unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitBuffer commit;
    char tree_hex[GIT_OBJECT_HEX_SIZE + 1U];
    char parent_hex[GIT_OBJECT_HEX_SIZE + 1U];
    int result = -1;

    rt_memset(&commit, 0, sizeof(commit));
    git_format_oid_hex(tree_oid, tree_hex);
    if (git_buffer_append_cstr(&commit, "tree ") != 0 || git_buffer_append_cstr(&commit, tree_hex) != 0 || git_buffer_append_char(&commit, '\n') != 0) {
        goto done;
    }
    if (parent_oid_or_null != 0) {
        git_format_oid_hex(parent_oid_or_null, parent_hex);
    }
    if (parent_oid_or_null != 0 && (git_buffer_append_cstr(&commit, "parent ") != 0 || git_buffer_append_cstr(&commit, parent_hex) != 0 || git_buffer_append_char(&commit, '\n') != 0)) {
        goto done;
    }
    if (git_commit_append_identity_line(&commit, "author", "GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", platform_get_epoch_time()) != 0 ||
        git_commit_append_identity_line(&commit, "committer", "GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL", platform_get_epoch_time()) != 0 ||
        git_buffer_append_char(&commit, '\n') != 0 || git_buffer_append_cstr(&commit, message) != 0 || git_buffer_append_char(&commit, '\n') != 0) {
        goto done;
    }
    result = git_write_loose_object(repo, GIT_OBJECT_COMMIT, commit.data, commit.size, commit_oid);
done:
    git_buffer_destroy(&commit);
    return result;
}

static int git_write_commit_from_tree(GitRepo *repo, const unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE], const char *message, unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    unsigned char parent_oid[CRYPTO_SHA1_DIGEST_SIZE];

    if (repo->head_oid[0] != '\0' && git_parse_oid_hex(repo->head_oid, parent_oid) == 0) {
        return git_write_commit_from_tree_with_parent(repo, tree_oid, parent_oid, message, commit_oid);
    }
    return git_write_commit_from_tree_with_parent(repo, tree_oid, 0, message, commit_oid);
}

static int git_write_detached_head_oid(const GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    char path[GIT_PATH_CAPACITY];
    char text[GIT_OBJECT_HEX_SIZE + 2U];

    if (git_join(path, sizeof(path), repo->git_dir, "HEAD") != 0) {
        return -1;
    }
    git_format_oid_hex(oid, text);
    text[GIT_OBJECT_HEX_SIZE] = '\n';
    text[GIT_OBJECT_HEX_SIZE + 1U] = '\0';
    return git_write_all_file(path, text, GIT_OBJECT_HEX_SIZE + 1U, 0644U);
}

static void git_zero_oid_hex(char hex[GIT_OBJECT_HEX_SIZE + 1U]) {
    size_t i;

    for (i = 0U; i < GIT_OBJECT_HEX_SIZE; ++i) {
        hex[i] = '0';
    }
    hex[GIT_OBJECT_HEX_SIZE] = '\0';
}

static void git_reflog_old_ref_hex(const GitRepo *repo, const char *ref_name, char old_hex[GIT_OBJECT_HEX_SIZE + 1U]) {
    if (ref_name != 0 && ref_name[0] != '\0' && git_resolve_ref(repo, ref_name, old_hex, GIT_OBJECT_HEX_SIZE + 1U) == 0) {
        old_hex[GIT_OBJECT_HEX_SIZE] = '\0';
        return;
    }
    git_zero_oid_hex(old_hex);
}

static int git_reflog_append_hex(const GitRepo *repo, const char *log_name, const char old_hex[GIT_OBJECT_HEX_SIZE + 1U], const unsigned char new_oid[CRYPTO_SHA1_DIGEST_SIZE], const char *message) {
    char path[GIT_PATH_CAPACITY];
    char parent[GIT_PATH_CAPACITY];
    char new_hex[GIT_OBJECT_HEX_SIZE + 1U];
    GitBuffer line;
    const char *name;
    const char *email;
    int fd;
    int result = -1;

    rt_memset(&line, 0, sizeof(line));
    git_format_oid_hex(new_oid, new_hex);
    name = git_identity_value("GIT_COMMITTER_NAME", "USER", "newos");
    email = git_identity_value("GIT_COMMITTER_EMAIL", 0, "newos@example.invalid");
    if (git_join(path, sizeof(path), repo->git_dir, "logs") != 0 || git_join(path, sizeof(path), path, log_name) != 0 || git_copy(parent, sizeof(parent), path) != 0 || git_path_parent(parent) != 0 || git_make_directory_chain(parent) != 0) {
        return -1;
    }
    if (git_buffer_append_cstr(&line, old_hex) != 0 || git_buffer_append_char(&line, ' ') != 0 || git_buffer_append_cstr(&line, new_hex) != 0 || git_buffer_append_char(&line, ' ') != 0 ||
        git_buffer_append_cstr(&line, name) != 0 || git_buffer_append_cstr(&line, " <") != 0 || git_buffer_append_cstr(&line, email) != 0 || git_buffer_append_cstr(&line, "> ") != 0 ||
        git_buffer_append_unsigned(&line, (unsigned long long)platform_get_epoch_time()) != 0 || git_buffer_append_cstr(&line, " +0000\t") != 0 || git_buffer_append_cstr(&line, message != 0 ? message : "update") != 0 || git_buffer_append_char(&line, '\n') != 0) {
        goto done;
    }
    fd = platform_open_append(path, 0644U);
    if (fd < 0) {
        goto done;
    }
    result = rt_write_all(fd, line.data, line.size);
    if (platform_close(fd) != 0) {
        result = -1;
    }
done:
    git_buffer_destroy(&line);
    return result;
}

static int git_write_ref_oid_reflog(const GitRepo *repo, const char *ref_name, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const char *message, int update_head_log) {
    char old_hex[GIT_OBJECT_HEX_SIZE + 1U];

    git_reflog_old_ref_hex(repo, ref_name, old_hex);
    if (git_write_ref_oid(repo, ref_name, oid) != 0) {
        return -1;
    }
    (void)git_reflog_append_hex(repo, ref_name, old_hex, oid, message);
    if (update_head_log) {
        (void)git_reflog_append_hex(repo, "HEAD", old_hex, oid, message);
    }
    return 0;
}

static int git_write_detached_head_oid_reflog(const GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const char *message) {
    char old_hex[GIT_OBJECT_HEX_SIZE + 1U];

    if (repo->head_oid[0] != '\0') {
        if (git_copy(old_hex, sizeof(old_hex), repo->head_oid) != 0) {
            git_zero_oid_hex(old_hex);
        }
    } else {
        git_zero_oid_hex(old_hex);
    }
    if (git_write_detached_head_oid(repo, oid) != 0) {
        return -1;
    }
    (void)git_reflog_append_hex(repo, "HEAD", old_hex, oid, message);
    return 0;
}

static int git_cmd_reflog(GitRepo *repo, int argc, char **argv, int argi) {
    const char *name = "HEAD";
    char path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t end;
    unsigned int ordinal = 0U;

    if (argi < argc) {
        name = argv[argi++];
    }
    if (argi < argc) {
        tool_write_error("git", "reflog needs at most one ref", 0);
        return 1;
    }
    if (git_join(path, sizeof(path), repo->git_dir, "logs") != 0 || git_join(path, sizeof(path), path, name) != 0 || git_read_file(path, &data, &size) != 0) {
        return 1;
    }
    end = size;
    while (end > 0U) {
        size_t start = end;
        size_t message = end;
        char ordinal_text[32];

        while (start > 0U && data[start - 1U] == '\n') {
            start -= 1U;
            end -= 1U;
        }
        while (start > 0U && data[start - 1U] != '\n') {
            start -= 1U;
        }
        if (end <= start) {
            break;
        }
        message = start;
        while (message < end && data[message] != '\t') {
            message += 1U;
        }
        if (end - start >= GIT_OBJECT_HEX_SIZE * 2U + 1U) {
            rt_write_all(1, data + start + GIT_OBJECT_HEX_SIZE + 1U, 7U);
            rt_write_cstr(1, " ");
            rt_write_cstr(1, name);
            rt_write_cstr(1, "@{");
            rt_unsigned_to_string(ordinal, ordinal_text, sizeof(ordinal_text));
            rt_write_cstr(1, ordinal_text);
            rt_write_cstr(1, "}: ");
            if (message < end) {
                rt_write_all(1, data + message + 1U, end - message - 1U);
            }
            rt_write_char(1, '\n');
            ordinal += 1U;
        }
        end = start;
    }
    rt_free(data);
    return 0;
}

static int git_delete_loose_ref(const GitRepo *repo, const char *ref_name) {
    char path[GIT_PATH_CAPACITY];

    if (tool_path_is_unsafe_relative(ref_name) || git_join(path, sizeof(path), repo->git_dir, ref_name) != 0) {
        return -1;
    }
    return platform_remove_file(path);
}

typedef struct {
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    size_t line_start;
    size_t line_end;
    size_t message_start;
    size_t message_end;
} GitStashLogEntry;

typedef struct {
    unsigned char *data;
    size_t size;
    GitStashLogEntry *entries;
    size_t count;
} GitStashLog;

static void git_stash_log_destroy(GitStashLog *log) {
    if (log == 0) {
        return;
    }
    rt_free(log->data);
    rt_free(log->entries);
    rt_memset(log, 0, sizeof(*log));
}

static int git_stash_log_path(const GitRepo *repo, char *path, size_t path_size) {
    return git_join(path, path_size, repo->git_dir, "logs/refs/stash");
}

static int git_stash_log_push_entry(GitStashLog *log, const GitStashLogEntry *entry) {
    GitStashLogEntry *new_entries;
    size_t new_capacity = log->count == 0U ? 8U : log->count * 2U;

    if ((log->count & (log->count - 1U)) == 0U) {
        new_entries = (GitStashLogEntry *)rt_realloc_array(log->entries, new_capacity, sizeof(log->entries[0]));
        if (new_entries == 0) {
            return -1;
        }
        log->entries = new_entries;
    }
    log->entries[log->count++] = *entry;
    return 0;
}

static int git_read_stash_log(const GitRepo *repo, GitStashLog *log) {
    char path[GIT_PATH_CAPACITY];
    size_t pos = 0U;

    rt_memset(log, 0, sizeof(*log));
    if (git_stash_log_path(repo, path, sizeof(path)) != 0 || git_read_file(path, &log->data, &log->size) != 0) {
        return -1;
    }
    while (pos < log->size) {
        GitStashLogEntry entry;
        size_t line_start = pos;
        size_t line_end;
        size_t message;

        while (pos < log->size && log->data[pos] != '\n') {
            pos += 1U;
        }
        line_end = pos;
        if (pos < log->size) {
            pos += 1U;
        }
        if (line_end <= line_start + GIT_OBJECT_HEX_SIZE * 2U + 1U) {
            continue;
        }
        rt_memset(&entry, 0, sizeof(entry));
        if (git_parse_oid_hex_n((const char *)log->data + line_start + GIT_OBJECT_HEX_SIZE + 1U, GIT_OBJECT_HEX_SIZE, entry.oid) != 0) {
            continue;
        }
        message = line_start;
        while (message < line_end && log->data[message] != '\t') {
            message += 1U;
        }
        entry.line_start = line_start;
        entry.line_end = pos;
        entry.message_start = message < line_end ? message + 1U : line_end;
        entry.message_end = line_end;
        if (git_stash_log_push_entry(log, &entry) != 0) {
            git_stash_log_destroy(log);
            return -1;
        }
    }
    return log->count > 0U ? 0 : -1;
}

static int git_parse_stash_selector(const char *text, size_t *ordinal_out) {
    size_t i = 0U;
    size_t value = 0U;
    int saw_digit = 0;

    if (text == 0 || rt_strcmp(text, "stash") == 0) {
        *ordinal_out = 0U;
        return 0;
    }
    if (rt_strncmp(text, "stash@{", 7U) != 0) {
        return -1;
    }
    i = 7U;
    while (text[i] >= '0' && text[i] <= '9') {
        saw_digit = 1;
        value = value * 10U + (size_t)(text[i] - '0');
        i += 1U;
    }
    if (!saw_digit || text[i] != '}' || text[i + 1U] != '\0') {
        return -1;
    }
    *ordinal_out = value;
    return 0;
}

static GitStashLogEntry *git_stash_log_entry_by_ordinal(GitStashLog *log, size_t ordinal) {
    if (ordinal >= log->count) {
        return 0;
    }
    return &log->entries[log->count - 1U - ordinal];
}

static int git_stash_rewrite_without(const GitRepo *repo, GitStashLog *log, size_t ordinal) {
    GitBuffer out;
    char log_path[GIT_PATH_CAPACITY];
    size_t remove_index;
    size_t i;
    int result = -1;

    if (ordinal >= log->count || git_stash_log_path(repo, log_path, sizeof(log_path)) != 0) {
        return -1;
    }
    remove_index = log->count - 1U - ordinal;
    if (log->count == 1U) {
        (void)git_delete_loose_ref(repo, "refs/stash");
        (void)platform_remove_file(log_path);
        return 0;
    }
    rt_memset(&out, 0, sizeof(out));
    for (i = 0U; i < log->count; ++i) {
        if (i == remove_index) {
            continue;
        }
        if (git_buffer_append(&out, log->data + log->entries[i].line_start, log->entries[i].line_end - log->entries[i].line_start) != 0) {
            goto done;
        }
    }
    if (git_write_all_file(log_path, out.data, out.size, 0644U) != 0) {
        goto done;
    }
    if (remove_index == log->count - 1U) {
        if (git_write_ref_oid(repo, "refs/stash", log->entries[log->count - 2U].oid) != 0) {
            goto done;
        }
    }
    result = 0;
done:
    git_buffer_destroy(&out);
    return result;
}

static int git_stash_save(GitRepo *repo) {
    GitIndex index;
    GitPack pack;
    GitAddContext context;
    unsigned char head_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char head_tree_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char stash_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char stash_hex[GIT_OBJECT_HEX_SIZE + 1U];
    char message[GIT_REF_CAPACITY + 32U];
    int have_pack;
    size_t i = 0U;
    int result = 1;

    if (repo->head_oid[0] == '\0' || git_parse_oid_hex(repo->head_oid, head_oid) != 0) {
        tool_write_error("git", "stash needs HEAD", 0);
        return 1;
    }
    if (git_load_index(repo, &index) != 0) {
        tool_write_error("git", "cannot read index", 0);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    context.repo = repo;
    context.index = &index;
    context.ignores = 0;
    context.added = 0U;
    while (i < index.count) {
        int modified = git_entry_is_modified(repo, &index.entries[i]);

        if (modified < 0) {
            git_index_remove_at(&index, i);
            context.added += 1U;
            continue;
        }
        if (modified > 0) {
            char path[GIT_PATH_CAPACITY];
            if (git_copy(path, sizeof(path), index.entries[i].path) != 0 || git_add_stage_blob_path(&context, path) != 0) {
                tool_write_error("git", "cannot stage stash path: ", index.entries[i].path);
                goto done;
            }
        }
        i += 1U;
    }
    if (git_write_tree_from_index(repo, &index, tree_oid) != 0 || git_commit_tree_oid(repo, head_oid, have_pack ? &pack : 0, head_tree_oid) != 0) {
        tool_write_error("git", "cannot write stash tree", 0);
        goto done;
    }
    if (git_oid_equal(tree_oid, head_tree_oid)) {
        rt_write_line(1, "No local changes to save");
        result = 0;
        goto done;
    }
    if (git_copy(message, sizeof(message), "WIP on ") != 0 || git_copy(message + rt_strlen(message), sizeof(message) - rt_strlen(message), git_branch_name(repo) != 0 ? git_branch_name(repo) : "detached HEAD") != 0) {
        goto done;
    }
    if (git_write_commit_from_tree_with_parent(repo, tree_oid, head_oid, message, stash_oid) != 0 || git_write_ref_oid_reflog(repo, "refs/stash", stash_oid, message, 0) != 0) {
        tool_write_error("git", "cannot write stash", 0);
        goto done;
    }
    if (git_checkout_commit_to_worktree(repo, head_oid, have_pack ? &pack : 0) != 0) {
        tool_write_error("git", "cannot restore HEAD after stash", 0);
        goto done;
    }
    git_format_oid_hex(stash_oid, stash_hex);
    rt_write_cstr(1, "Saved working directory and index state ");
    rt_write_line(1, stash_hex);
    result = 0;
done:
    if (have_pack) git_pack_destroy(&pack);
    git_index_destroy(&index);
    return result;
}

static int git_cmd_stash(GitRepo *repo, int argc, char **argv, int argi) {
    const char *subcommand = "push";
    const char *selector = 0;
    unsigned char stash_oid[CRYPTO_SHA1_DIGEST_SIZE];
    size_t ordinal = 0U;
    GitStashLog log;
    GitStashLogEntry *entry;
    GitPack pack;
    int have_pack;
    int result = 1;

    if (argi < argc) {
        subcommand = argv[argi++];
    }
    if (argi < argc) {
        selector = argv[argi++];
    }
    if (argi < argc) {
        tool_write_error("git", "too many stash arguments", 0);
        return 1;
    }
    if (rt_strcmp(subcommand, "push") == 0 || rt_strcmp(subcommand, "save") == 0) {
        if (selector != 0) {
            tool_write_error("git", "stash save does not accept a selector", 0);
            return 1;
        }
        return git_stash_save(repo);
    }
    if (git_parse_stash_selector(selector, &ordinal) != 0) {
        tool_write_error("git", "invalid stash selector: ", selector);
        return 1;
    }
    if (git_read_stash_log(repo, &log) != 0 || (entry = git_stash_log_entry_by_ordinal(&log, ordinal)) == 0) {
        if (rt_strcmp(subcommand, "list") == 0) {
            git_stash_log_destroy(&log);
            return 0;
        }
        git_stash_log_destroy(&log);
        tool_write_error("git", "no stash entries found", 0);
        return 1;
    }
    memcpy(stash_oid, entry->oid, CRYPTO_SHA1_DIGEST_SIZE);
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (rt_strcmp(subcommand, "list") == 0) {
        size_t i;

        (void)have_pack;
        for (i = 0U; i < log.count; ++i) {
            GitStashLogEntry *listed = git_stash_log_entry_by_ordinal(&log, i);
            char number[32];

            rt_write_cstr(1, "stash@{");
            rt_unsigned_to_string((unsigned long long)i, number, sizeof(number));
            rt_write_cstr(1, number);
            rt_write_cstr(1, "}: ");
            if (listed->message_start < listed->message_end) {
                rt_write_all(1, log.data + listed->message_start, listed->message_end - listed->message_start);
            }
            rt_write_char(1, '\n');
        }
        result = 0;
    } else if (rt_strcmp(subcommand, "apply") == 0 || rt_strcmp(subcommand, "pop") == 0) {
        if (git_checkout_commit_to_worktree(repo, stash_oid, have_pack ? &pack : 0) != 0) {
            tool_write_error("git", "cannot apply stash", 0);
            goto done;
        }
        rt_write_cstr(1, "Applied stash@{");
        git_write_size(ordinal);
        rt_write_line(1, "}");
        if (rt_strcmp(subcommand, "pop") == 0 && git_stash_rewrite_without(repo, &log, ordinal) != 0) {
            tool_write_error("git", "cannot drop stash", 0);
            goto done;
        }
        result = 0;
    } else if (rt_strcmp(subcommand, "drop") == 0) {
        if (git_stash_rewrite_without(repo, &log, ordinal) != 0) {
            tool_write_error("git", "cannot drop stash", 0);
            goto done;
        }
        rt_write_cstr(1, "Dropped stash@{");
        git_write_size(ordinal);
        rt_write_line(1, "}");
        result = 0;
    } else {
        tool_write_error("git", "unsupported stash command: ", subcommand);
    }
done:
    if (have_pack) git_pack_destroy(&pack);
    git_stash_log_destroy(&log);
    return result;
}

static int git_hook_path(const GitRepo *repo, const char *name, char *path, size_t path_size) {
    char hooks[GIT_PATH_CAPACITY];

    return git_join(hooks, sizeof(hooks), repo->git_dir, "hooks") != 0 || git_join(path, path_size, hooks, name) != 0 ? -1 : 0;
}

static int git_run_hook(GitRepo *repo, const char *name, const char *arg) {
    char path[GIT_PATH_CAPACITY];
    PlatformDirEntry entry;
    char *hook_argv[3];
    int pid;
    int status;

    if (git_hook_path(repo, name, path, sizeof(path)) != 0 || platform_get_path_info(path, &entry) != 0) {
        return 0;
    }
    if (entry.is_dir || (entry.mode & 0111U) == 0U) {
        return 0;
    }
    hook_argv[0] = path;
    hook_argv[1] = (char *)arg;
    hook_argv[2] = 0;
    if (arg == 0) {
        hook_argv[1] = 0;
    }
    if (platform_spawn_process(hook_argv, -1, -1, 0, 0, 0, &pid) != 0 || platform_wait_process(pid, &status) != 0 || status != 0) {
        tool_write_error("git", "hook failed: ", name);
        return -1;
    }
    return 0;
}

static int git_commit_message_path(const GitRepo *repo, char *path, size_t path_size) {
    return git_join(path, path_size, repo->git_dir, "COMMIT_EDITMSG");
}

static char *git_read_commit_message_path(const char *path) {
    unsigned char *data = 0;
    size_t size = 0U;
    char *message;

    if (git_read_file(path, &data, &size) != 0) {
        return 0;
    }
    message = git_strdup_n((const char *)data, size);
    rt_free(data);
    if (message == 0) {
        return 0;
    }
    tool_trim_whitespace(message);
    if (message[0] == '\0') {
        rt_free(message);
        return 0;
    }
    return message;
}

static char *git_commit_message_from_editor(GitRepo *repo, char *message_path, size_t message_path_size) {
    const char *editor = platform_getenv("GIT_EDITOR");
    char *editor_argv[3];
    int pid;
    int status;

    if (editor == 0 || editor[0] == '\0') editor = platform_getenv("VISUAL");
    if (editor == 0 || editor[0] == '\0') editor = platform_getenv("EDITOR");
    if (editor == 0 || editor[0] == '\0') {
        tool_write_error("git", "commit needs -m MESSAGE or GIT_EDITOR", 0);
        return 0;
    }
    if (git_commit_message_path(repo, message_path, message_path_size) != 0 || git_write_all_file(message_path, "", 0U, 0644U) != 0) {
        return 0;
    }
    editor_argv[0] = (char *)editor;
    editor_argv[1] = message_path;
    editor_argv[2] = 0;
    if (platform_spawn_process(editor_argv, -1, -1, 0, 0, 0, &pid) != 0 || platform_wait_process(pid, &status) != 0 || status != 0) {
        tool_write_error("git", "editor failed: ", editor);
        return 0;
    }
    return git_read_commit_message_path(message_path);
}

static int git_cmd_commit(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    GitPack pack;
    GitBuffer commit;
    unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char parent_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char parent_tree_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char tree_hex[GIT_OBJECT_HEX_SIZE + 1U];
    char parent_hex[GIT_OBJECT_HEX_SIZE + 1U];
    char commit_hex[GIT_OBJECT_HEX_SIZE + 1U];
    const char *message = 0;
    char *owned_message = 0;
    char message_path[GIT_PATH_CAPACITY];
    int have_parent = 0;
    int have_pack = 0;
    int allow_empty = 0;
    int no_verify = 0;
    int result = 1;

    message_path[0] = '\0';

    while (argi < argc) {
        if ((rt_strcmp(argv[argi], "-m") == 0 || rt_strcmp(argv[argi], "--message") == 0) && argi + 1 < argc) {
            message = argv[argi + 1];
            argi += 2;
        } else if (rt_strncmp(argv[argi], "--message=", 10U) == 0) {
            message = argv[argi] + 10U;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--allow-empty") == 0) {
            allow_empty = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--no-verify") == 0) {
            no_verify = 1;
            argi += 1;
        } else {
            tool_write_error("git", "unsupported commit option: ", argv[argi]);
            return 1;
        }
    }
    if (message == 0) {
        owned_message = git_commit_message_from_editor(repo, message_path, sizeof(message_path));
        message = owned_message;
    } else if (git_commit_message_path(repo, message_path, sizeof(message_path)) == 0) {
        (void)git_write_all_file(message_path, message, rt_strlen(message), 0644U);
    }
    if (message == 0 || message[0] == '\0') {
        tool_write_error("git", "empty commit message", 0);
        rt_free(owned_message);
        return 1;
    }
    if (!no_verify && (git_run_hook(repo, "pre-commit", 0) != 0 || (message_path[0] != '\0' && git_run_hook(repo, "commit-msg", message_path) != 0))) {
        rt_free(owned_message);
        return 1;
    }
    if (!no_verify && message_path[0] != '\0') {
        char *hook_message = git_read_commit_message_path(message_path);
        if (hook_message != 0) {
            rt_free(owned_message);
            owned_message = hook_message;
            message = owned_message;
        }
    }
    if (git_load_index(repo, &index) != 0) {
        tool_write_error("git", "cannot read index", 0);
        rt_free(owned_message);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (git_write_tree_from_index(repo, &index, tree_oid) != 0) {
        tool_write_error("git", "cannot write tree", 0);
        goto done;
    }
    if (repo->head_oid[0] != '\0' && git_parse_oid_hex(repo->head_oid, parent_oid) == 0) {
        have_parent = 1;
        if (!allow_empty && git_commit_tree_oid(repo, parent_oid, have_pack ? &pack : 0, parent_tree_oid) == 0 && git_oid_equal(tree_oid, parent_tree_oid)) {
            rt_write_line(1, "nothing to commit, working tree clean");
            goto done;
        }
    }

    rt_memset(&commit, 0, sizeof(commit));
    git_format_oid_hex(tree_oid, tree_hex);
    if (git_buffer_append_cstr(&commit, "tree ") != 0 || git_buffer_append_cstr(&commit, tree_hex) != 0 || git_buffer_append_char(&commit, '\n') != 0) {
        goto commit_done;
    }
    if (have_parent) {
        git_format_oid_hex(parent_oid, parent_hex);
        if (git_buffer_append_cstr(&commit, "parent ") != 0 || git_buffer_append_cstr(&commit, parent_hex) != 0 || git_buffer_append_char(&commit, '\n') != 0) {
            goto commit_done;
        }
    }
    if (git_commit_append_identity_line(&commit, "author", "GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", platform_get_epoch_time()) != 0 ||
        git_commit_append_identity_line(&commit, "committer", "GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL", platform_get_epoch_time()) != 0 ||
        git_buffer_append_char(&commit, '\n') != 0 ||
        git_buffer_append_cstr(&commit, message) != 0 ||
        git_buffer_append_char(&commit, '\n') != 0) {
        goto commit_done;
    }
    if (git_write_loose_object(repo, GIT_OBJECT_COMMIT, commit.data, commit.size, commit_oid) != 0) {
        goto commit_done;
    }
    if (repo->head_ref[0] != '\0') {
        if (git_write_ref_oid_reflog(repo, repo->head_ref, commit_oid, "commit", 1) != 0) {
            goto commit_done;
        }
    } else if (git_write_detached_head_oid_reflog(repo, commit_oid, "commit") != 0) {
        goto commit_done;
    }
    git_format_oid_hex(commit_oid, commit_hex);
    rt_write_cstr(1, "Committed ");
    rt_write_line(1, commit_hex);
    result = 0;
commit_done:
    git_buffer_destroy(&commit);
    if (result != 0) {
        tool_write_error("git", "commit failed", 0);
    }
done:
    if (have_pack) {
        git_pack_destroy(&pack);
    }
    git_index_destroy(&index);
    rt_free(owned_message);
    return result;
}

static int git_cmd_cherry_pick(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    GitCommitInfo info;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char head_oid[CRYPTO_SHA1_DIGEST_SIZE];
    int have_pack;
    int result;

    rt_memset(&info, 0, sizeof(info));
    if (argi + 1 != argc) {
        tool_write_error("git", "cherry-pick needs one revision", 0);
        return 1;
    }
    if (repo->head_oid[0] == '\0' || git_parse_oid_hex(repo->head_oid, head_oid) != 0 || git_resolve_revision(repo, argv[argi], oid, 0, 0) != 0) {
        tool_write_error("git", "cannot resolve cherry-pick revision: ", argv[argi]);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (git_read_commit_info(repo, oid, have_pack ? &pack : 0, &info) != 0 || info.parent_count != 1U || !git_oid_equal(info.parents[0], head_oid)) {
        tool_write_error("git", "cherry-pick currently supports direct child commits only", 0);
        git_commit_info_destroy(&info);
        if (have_pack) git_pack_destroy(&pack);
        return 1;
    }
    result = git_fast_forward_to_oid(repo, oid, have_pack ? &pack : 0, argv[argi], "cherry-pick");
    git_commit_info_destroy(&info);
    if (have_pack) git_pack_destroy(&pack);
    return result;
}

static int git_cmd_revert(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    GitCommitInfo info;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char head_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char parent_tree_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char revert_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char revert_hex[GIT_OBJECT_HEX_SIZE + 1U];
    int have_pack;
    int result = 1;

    rt_memset(&info, 0, sizeof(info));
    if (argi + 1 != argc) {
        tool_write_error("git", "revert needs one revision", 0);
        return 1;
    }
    if (repo->head_oid[0] == '\0' || git_parse_oid_hex(repo->head_oid, head_oid) != 0 || git_resolve_revision(repo, argv[argi], oid, 0, 0) != 0 || !git_oid_equal(head_oid, oid)) {
        tool_write_error("git", "revert currently supports HEAD only", 0);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (git_read_commit_info(repo, oid, have_pack ? &pack : 0, &info) != 0 || info.parent_count != 1U) {
        tool_write_error("git", "cannot revert root or merge commit", 0);
        goto done;
    }
    if (git_commit_tree_oid(repo, info.parents[0], have_pack ? &pack : 0, parent_tree_oid) != 0 || git_checkout_commit_to_worktree(repo, info.parents[0], have_pack ? &pack : 0) != 0) {
        tool_write_error("git", "cannot restore parent tree for revert", 0);
        goto done;
    }
    if (git_write_commit_from_tree(repo, parent_tree_oid, "Revert HEAD", revert_oid) != 0 || git_update_head_to_oid(repo, revert_oid, "revert") != 0) {
        tool_write_error("git", "revert failed", 0);
        goto done;
    }
    git_format_oid_hex(revert_oid, revert_hex);
    rt_write_cstr(1, "Reverted ");
    rt_write_line(1, revert_hex);
    result = 0;
done:
    git_commit_info_destroy(&info);
    if (have_pack) git_pack_destroy(&pack);
    return result;
}

