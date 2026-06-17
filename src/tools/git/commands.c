static int git_path_parent(char *path) {
    size_t length = rt_strlen(path);

    while (length > 0U && path[length - 1U] != '/') {
        length -= 1U;
    }
    if (length == 0U) {
        path[0] = '\0';
        return 0;
    }
    if (length == 1U) {
        path[1] = '\0';
        return 0;
    }
    path[length - 1U] = '\0';
    return 0;
}

static int git_make_directory_chain(const char *path) {
    char buffer[GIT_PATH_CAPACITY];
    size_t i;

    if (path == 0 || path[0] == '\0') {
        return 0;
    }
    if (git_copy(buffer, sizeof(buffer), path) != 0) {
        return -1;
    }
    for (i = 1U; buffer[i] != '\0'; ++i) {
        if (buffer[i] == '/') {
            buffer[i] = '\0';
            if (buffer[0] != '\0') {
                int is_directory = 0;
                if (platform_path_is_directory(buffer, &is_directory) != 0) {
                    if (platform_make_directory(buffer, 0755U) != 0) {
                        return -1;
                    }
                } else if (!is_directory) {
                    return -1;
                }
            }
            buffer[i] = '/';
        }
    }
    {
        int is_directory = 0;
        if (platform_path_is_directory(buffer, &is_directory) != 0) {
            return platform_make_directory(buffer, 0755U);
        }
        return is_directory ? 0 : -1;
    }
}

static int git_ensure_parent_directory(const char *path) {
    char parent[GIT_PATH_CAPACITY];

    if (git_copy(parent, sizeof(parent), path) != 0 || git_path_parent(parent) != 0) {
        return -1;
    }
    return git_make_directory_chain(parent);
}

static void git_zero_oid_hex(char hex[GIT_OBJECT_HEX_SIZE + 1U]);
static int git_reflog_append_hex(const GitRepo *repo, const char *log_name, const char old_hex[GIT_OBJECT_HEX_SIZE + 1U], const unsigned char new_oid[CRYPTO_SHA1_DIGEST_SIZE], const char *message);
static int git_write_ref_oid_reflog(const GitRepo *repo, const char *ref_name, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const char *message, int update_head_log);
static int git_write_detached_head_oid_reflog(const GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const char *message);
static int git_commit_is_ancestor_of(GitRepo *repo, const unsigned char ancestor[CRYPTO_SHA1_DIGEST_SIZE], const unsigned char descendant[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack);

static int git_source_arg_to_path(const char *arg, char *buffer, size_t buffer_size) {
    char cwd[GIT_PATH_CAPACITY];
    const char *path = arg;

    if (rt_strncmp(arg, "file://", 7U) == 0) {
        path = arg + 7U;
    } else if (rt_strncmp(arg, "http://", 7U) == 0 || rt_strncmp(arg, "https://", 8U) == 0 ||
               rt_strncmp(arg, "git://", 6U) == 0 || rt_strncmp(arg, "ssh://", 6U) == 0) {
        tool_write_error("git", "clone transport is not implemented: ", arg);
        return -1;
    }

    if (git_is_absolute_path(path)) {
        return git_copy(buffer, buffer_size, path);
    }
    if (platform_get_current_directory(cwd, sizeof(cwd)) != 0) {
        return -1;
    }
    return git_join(buffer, buffer_size, cwd, path);
}

static int git_default_clone_destination(const char *source, char *buffer, size_t buffer_size) {
    size_t end = rt_strlen(source);
    size_t start;
    size_t length;

    while (end > 0U && source[end - 1U] == '/') {
        end -= 1U;
    }
    start = end;
    while (start > 0U && source[start - 1U] != '/') {
        start -= 1U;
    }
    length = end - start;
    if (length > 4U && source[start + length - 4U] == '.' && source[start + length - 3U] == 'g' &&
        source[start + length - 2U] == 'i' && source[start + length - 1U] == 't') {
        length -= 4U;
    }
    if (length == 0U || length >= buffer_size) {
        return -1;
    }
    memcpy(buffer, source + start, length);
    buffer[length] = '\0';
    return 0;
}

static int git_copy_tracked_files(const GitRepo *source_repo, const GitIndex *index, const char *destination) {
    size_t i;

    for (i = 0U; i < index->count; ++i) {
        char source_path[GIT_PATH_CAPACITY];
        char dest_path[GIT_PATH_CAPACITY];
        PlatformDirEntry info;

        if (tool_path_is_unsafe_relative(index->entries[i].path)) {
            tool_write_error("git", "refusing unsafe checkout path: ", index->entries[i].path);
            return -1;
        }
        if (git_join(source_path, sizeof(source_path), source_repo->work_tree, index->entries[i].path) != 0 ||
            git_join(dest_path, sizeof(dest_path), destination, index->entries[i].path) != 0) {
            return -1;
        }
        if (platform_get_path_info(source_path, &info) != 0 || info.is_dir) {
            tool_write_error("git", "source tracked file is unavailable: ", index->entries[i].path);
            return -1;
        }
        if (!git_index_mode_is_regular(index->entries[i].mode)) {
            tool_write_error("git", "unsupported checkout mode: ", index->entries[i].path);
            return -1;
        }
        if (git_ensure_parent_directory(dest_path) != 0 || tool_copy_file(source_path, dest_path) != 0) {
            tool_write_error("git", "cannot check out file: ", index->entries[i].path);
            return -1;
        }
        (void)platform_change_mode(dest_path, git_worktree_mode_from_regular_index(index->entries[i].mode));
    }
    return 0;
}

static int git_source_tracked_files_are_clean(const GitRepo *source_repo, const GitIndex *index) {
    size_t i;

    for (i = 0U; i < index->count; ++i) {
        int modified = git_entry_is_modified(source_repo, &index->entries[i]);

        if (modified != 0) {
            tool_write_error("git", "source has modified or missing tracked file: ", index->entries[i].path);
            return 0;
        }
    }
    return 1;
}

static int git_init_empty_repo_at(const char *work_tree, GitRepo *repo) {
    char path[GIT_PATH_CAPACITY];

    rt_memset(repo, 0, sizeof(*repo));
    if (git_copy(repo->work_tree, sizeof(repo->work_tree), work_tree) != 0 || git_join(repo->git_dir, sizeof(repo->git_dir), work_tree, ".git") != 0) {
        return -1;
    }
    if (git_make_directory_chain(repo->git_dir) != 0) {
        return -1;
    }
    if (git_join(path, sizeof(path), repo->git_dir, "objects") != 0 || git_make_directory_chain(path) != 0) {
        return -1;
    }
    if (git_join(path, sizeof(path), repo->git_dir, "refs/heads") != 0 || git_make_directory_chain(path) != 0) {
        return -1;
    }
    if (git_join(path, sizeof(path), repo->git_dir, "refs/tags") != 0 || git_make_directory_chain(path) != 0) {
        return -1;
    }
    if (git_join(path, sizeof(path), repo->git_dir, "refs/remotes/origin") != 0 || git_make_directory_chain(path) != 0) {
        return -1;
    }
    if (git_join(path, sizeof(path), repo->git_dir, "info") != 0 || git_make_directory_chain(path) != 0) {
        return -1;
    }
    return 0;
}

static int git_write_default_config(const GitRepo *repo) {
    char path[GIT_PATH_CAPACITY];
    const char text[] = "[core]\n\trepositoryformatversion = 0\n\tfilemode = true\n\tbare = false\n\tlogallrefupdates = true\n";

    if (git_join(path, sizeof(path), repo->git_dir, "config") != 0) {
        return -1;
    }
    return git_write_all_file(path, text, sizeof(text) - 1U, 0644U);
}

static int git_cmd_init(int argc, char **argv, int argi) {
    GitRepo repo;
    char cwd[GIT_PATH_CAPACITY];
    char target[GIT_PATH_CAPACITY];
    char head_path[GIT_PATH_CAPACITY];
    char exclude_path[GIT_PATH_CAPACITY];
    PlatformDirEntry existing;

    if (argi + 1 < argc) {
        tool_write_error("git", "too many init arguments", 0);
        return 1;
    }
    if (platform_get_current_directory(cwd, sizeof(cwd)) != 0) {
        return 1;
    }
    if (argi < argc) {
        if (git_is_absolute_path(argv[argi])) {
            if (git_copy(target, sizeof(target), argv[argi]) != 0) {
                return 1;
            }
        } else if (git_join(target, sizeof(target), cwd, argv[argi]) != 0) {
            return 1;
        }
        if (git_make_directory_chain(target) != 0) {
            tool_write_error("git", "cannot create init path: ", argv[argi]);
            return 1;
        }
    } else if (git_copy(target, sizeof(target), cwd) != 0) {
        return 1;
    }
    if (git_init_empty_repo_at(target, &repo) != 0 ||
        git_join(head_path, sizeof(head_path), repo.git_dir, "HEAD") != 0 ||
        git_write_all_file(head_path, "ref: refs/heads/main\n", sizeof("ref: refs/heads/main\n") - 1U, 0644U) != 0 ||
        git_write_default_config(&repo) != 0) {
        tool_write_error("git", "init failed", 0);
        return 1;
    }
    if (git_join(exclude_path, sizeof(exclude_path), repo.git_dir, "info/exclude") == 0 && platform_get_path_info(exclude_path, &existing) != 0) {
        (void)git_write_all_file(exclude_path, "", 0U, 0644U);
    }
    rt_write_cstr(1, "Initialized empty Git repository in ");
    rt_write_cstr(1, repo.git_dir);
    rt_write_line(1, "/");
    return 0;
}

static const char *git_branch_from_ref(const char *ref_name) {
    if (rt_strncmp(ref_name, "refs/heads/", 11U) == 0) {
        return ref_name + 11U;
    }
    return 0;
}

static int git_cmd_clone_remote(const char *remote_url, const char *destination_arg, const char *destination) {
    GitRepo repo;
    GitPack pack;
    char selected_ref[GIT_REF_CAPACITY];
    char local_ref[GIT_REF_CAPACITY];
    const char *branch_name;
    unsigned char selected_oid[CRYPTO_SHA1_DIGEST_SIZE];
    int result = 1;

    rt_memset(&pack, 0, sizeof(pack));
    git_progress_clone_into(destination_arg);
    if (platform_make_directory(destination, 0755U) != 0 || git_init_empty_repo_at(destination, &repo) != 0) {
        tool_write_error("git", "cannot create destination: ", destination_arg);
        return 1;
    }
    if (git_fetch_remote_to_repo(&repo, remote_url, 0, selected_ref, sizeof(selected_ref), selected_oid, &pack) != 0) {
        tool_write_error("git", "remote clone failed: ", remote_url);
        (void)tool_remove_path(destination, 1);
        goto done;
    }
    branch_name = git_branch_from_ref(selected_ref);
    if (branch_name == 0 || branch_name[0] == '\0') {
        branch_name = "main";
    }
    if (git_copy(local_ref, sizeof(local_ref), "refs/heads/") != 0 || rt_strlen(local_ref) + rt_strlen(branch_name) >= sizeof(local_ref)) {
        (void)tool_remove_path(destination, 1);
        goto done;
    }
    rt_copy_string(local_ref + rt_strlen(local_ref), sizeof(local_ref) - rt_strlen(local_ref), branch_name);
    git_progress_line("Checking out files...");
    if (git_write_ref_oid(&repo, local_ref, selected_oid) != 0 || git_write_head_ref(&repo, local_ref) != 0 || git_write_clone_config(&repo, remote_url, branch_name) != 0 || git_checkout_commit_to_worktree(&repo, selected_oid, &pack) != 0) {
        tool_write_error("git", "checkout failed: ", destination_arg);
        (void)tool_remove_path(destination, 1);
        goto done;
    }
    rt_write_cstr(1, "Cloned remote repository to ");
    rt_write_line(1, destination_arg);
    result = 0;
done:
    git_pack_destroy(&pack);
    return result;
}

static int git_cmd_clone(int argc, char **argv, int argi) {
    GitRepo source_repo;
    GitIndex index;
    char source_path[GIT_PATH_CAPACITY];
    char destination_arg[GIT_PATH_CAPACITY];
    char destination[GIT_PATH_CAPACITY];
    char dest_git[GIT_PATH_CAPACITY];
    PlatformDirEntry existing;
    int result = 1;

    if (argi >= argc) {
        tool_write_error("git", "clone needs a source", 0);
        return 1;
    }
    if (argi + 2 < argc) {
        tool_write_error("git", "too many clone arguments", 0);
        return 1;
    }
    if (argi + 1 < argc) {
        if (git_copy(destination_arg, sizeof(destination_arg), argv[argi + 1]) != 0) {
            return 1;
        }
    } else if (git_default_clone_destination(argv[argi], destination_arg, sizeof(destination_arg)) != 0) {
        tool_write_error("git", "cannot derive clone destination", 0);
        return 1;
    }
    if (git_is_absolute_path(destination_arg)) {
        if (git_copy(destination, sizeof(destination), destination_arg) != 0) {
            return 1;
        }
    } else {
        char cwd[GIT_PATH_CAPACITY];
        if (platform_get_current_directory(cwd, sizeof(cwd)) != 0 || git_join(destination, sizeof(destination), cwd, destination_arg) != 0) {
            return 1;
        }
    }

    if (platform_get_path_info(destination, &existing) == 0) {
        tool_write_error("git", "destination already exists: ", destination_arg);
        return 1;
    }
    if (git_url_is_http(argv[argi])) {
        return git_cmd_clone_remote(argv[argi], destination_arg, destination);
    }
    if (git_source_arg_to_path(argv[argi], source_path, sizeof(source_path)) != 0) {
        return 1;
    }
    if (git_discover_from(source_path, &source_repo) != 0 || git_load_head(&source_repo) != 0) {
        tool_write_error("git", "source is not a supported local repository: ", argv[argi]);
        return 1;
    }
    if (git_load_index(&source_repo, &index) != 0) {
        tool_write_error("git", "cannot read source index", 0);
        return 1;
    }
    if (!git_source_tracked_files_are_clean(&source_repo, &index)) {
        git_index_destroy(&index);
        return 1;
    }

    if (platform_make_directory(destination, 0755U) != 0) {
        tool_write_error("git", "cannot create destination: ", destination_arg);
        git_index_destroy(&index);
        return 1;
    }
    if (git_join(dest_git, sizeof(dest_git), destination, ".git") != 0 ||
        tool_copy_path(source_repo.git_dir, dest_git, 1, 1, 1) != 0 ||
        git_copy_tracked_files(&source_repo, &index, destination) != 0) {
        tool_write_error("git", "clone failed", destination_arg);
        git_index_destroy(&index);
        return 1;
    }

    rt_write_cstr(1, "Cloned local repository to ");
    rt_write_line(1, destination_arg);
    git_index_destroy(&index);
    result = 0;
    return result;
}

static int git_resolve_revision(GitRepo *repo, const char *name, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], char *head_ref_out, size_t head_ref_size) {
    char ref[GIT_REF_CAPACITY];

    if (head_ref_out != 0 && head_ref_size > 0U) {
        head_ref_out[0] = '\0';
    }
    if (rt_strcmp(name, "HEAD") == 0) {
        if (repo->head_oid[0] == '\0') {
            return -1;
        }
        if (git_parse_oid_hex(repo->head_oid, oid) != 0) {
            return -1;
        }
        if (head_ref_out != 0 && repo->head_ref[0] != '\0') {
            (void)git_copy(head_ref_out, head_ref_size, repo->head_ref);
        }
        return 0;
    }
    if (rt_strlen(name) == GIT_OBJECT_HEX_SIZE && git_parse_oid_hex(name, oid) == 0) {
        return 0;
    }
    if (rt_strncmp(name, "refs/", 5U) == 0) {
        char hex[GIT_OBJECT_HEX_SIZE + 1U];
        if (git_resolve_ref(repo, name, hex, sizeof(hex)) == 0 && git_parse_oid_hex(hex, oid) == 0) {
            if (head_ref_out != 0) {
                (void)git_copy(head_ref_out, head_ref_size, name);
            }
            return 0;
        }
    } else {
        char hex[GIT_OBJECT_HEX_SIZE + 1U];
        if (git_copy(ref, sizeof(ref), "refs/heads/") == 0 && rt_strlen(ref) + rt_strlen(name) < sizeof(ref)) {
            rt_copy_string(ref + rt_strlen(ref), sizeof(ref) - rt_strlen(ref), name);
            if (git_resolve_ref(repo, ref, hex, sizeof(hex)) == 0 && git_parse_oid_hex(hex, oid) == 0) {
                if (head_ref_out != 0) {
                    (void)git_copy(head_ref_out, head_ref_size, ref);
                }
                return 0;
            }
        }
        if (git_copy(ref, sizeof(ref), "refs/remotes/origin/") == 0 && rt_strlen(ref) + rt_strlen(name) < sizeof(ref)) {
            rt_copy_string(ref + rt_strlen(ref), sizeof(ref) - rt_strlen(ref), name);
            if (git_resolve_ref(repo, ref, hex, sizeof(hex)) == 0 && git_parse_oid_hex(hex, oid) == 0) {
                if (git_copy(ref, sizeof(ref), "refs/heads/") == 0 && rt_strlen(ref) + rt_strlen(name) < sizeof(ref)) {
                    rt_copy_string(ref + rt_strlen(ref), sizeof(ref) - rt_strlen(ref), name);
                    (void)git_write_ref_oid(repo, ref, oid);
                    if (head_ref_out != 0) {
                        (void)git_copy(head_ref_out, head_ref_size, ref);
                    }
                }
                return 0;
            }
        }
    }
    return -1;
}

static void git_trim_slice(const unsigned char *data, size_t *start_io, size_t *end_io) {
    while (*start_io < *end_io && (data[*start_io] == ' ' || data[*start_io] == '\t')) {
        *start_io += 1U;
    }
    while (*end_io > *start_io && (data[*end_io - 1U] == ' ' || data[*end_io - 1U] == '\t' || data[*end_io - 1U] == '\r')) {
        *end_io -= 1U;
    }
}

static int git_slice_equals_cstr(const unsigned char *data, size_t start, size_t end, const char *text) {
    size_t length = rt_strlen(text);

    return end >= start && end - start == length && memcmp(data + start, text, length) == 0;
}

static int git_config_key_to_section_var(const char *key, char *section, size_t section_size, char *var, size_t var_size) {
    if (rt_strncmp(key, "user.", 5U) == 0 && key[5] != '\0') {
        if (git_copy(section, section_size, "[user]") != 0 || git_copy(var, var_size, key + 5U) != 0) {
            return -1;
        }
        return 0;
    }
    if (rt_strncmp(key, "remote.", 7U) == 0) {
        const char *name = key + 7U;
        const char *dot = name;
        size_t name_length;

        while (*dot != '\0' && *dot != '.') {
            dot += 1;
        }
        name_length = (size_t)(dot - name);
        if (name_length == 0U || *dot != '.' || dot[1] == '\0' || name_length + 12U >= section_size || tool_path_is_unsafe_relative(name)) {
            return -1;
        }
        if (git_copy(section, section_size, "[remote \"") != 0) {
            return -1;
        }
        memcpy(section + 9U, name, name_length);
        section[9U + name_length] = '"';
        section[10U + name_length] = ']';
        section[11U + name_length] = '\0';
        if (git_copy(var, var_size, dot + 1U) != 0) {
            return -1;
        }
        return 0;
    }
    return -1;
}

static int git_config_line_has_var(const unsigned char *data, size_t start, size_t end, const char *var, size_t *value_start_out, size_t *value_end_out) {
    size_t eq = start;
    size_t name_start = start;
    size_t name_end;
    size_t value_start;
    size_t value_end;

    while (eq < end && data[eq] != '=') {
        eq += 1U;
    }
    if (eq >= end) {
        return 0;
    }
    name_end = eq;
    git_trim_slice(data, &name_start, &name_end);
    if (!git_slice_equals_cstr(data, name_start, name_end, var)) {
        return 0;
    }
    value_start = eq + 1U;
    value_end = end;
    git_trim_slice(data, &value_start, &value_end);
    *value_start_out = value_start;
    *value_end_out = value_end;
    return 1;
}

static int git_config_get_value(const GitRepo *repo, const char *key, char *buffer, size_t buffer_size) {
    char path[GIT_PATH_CAPACITY];
    char section[128];
    char var[64];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;
    int in_section = 0;

    if (git_config_key_to_section_var(key, section, sizeof(section), var, sizeof(var)) != 0 ||
        git_join(path, sizeof(path), repo->git_dir, "config") != 0 ||
        git_read_file(path, &data, &size) != 0) {
        return -1;
    }
    while (pos < size) {
        size_t start = pos;
        size_t end;
        size_t trim_start;
        size_t trim_end;

        while (pos < size && data[pos] != '\n') pos += 1U;
        end = pos;
        if (pos < size) pos += 1U;
        trim_start = start;
        trim_end = end;
        git_trim_slice(data, &trim_start, &trim_end);
        if (trim_start < trim_end && data[trim_start] == '[') {
            in_section = git_slice_equals_cstr(data, trim_start, trim_end, section);
        } else if (in_section) {
            size_t value_start = 0U;
            size_t value_end = 0U;
            if (git_config_line_has_var(data, trim_start, trim_end, var, &value_start, &value_end)) {
                if (value_end < value_start || value_end - value_start >= buffer_size) {
                    rt_free(data);
                    return -1;
                }
                memcpy(buffer, data + value_start, value_end - value_start);
                buffer[value_end - value_start] = '\0';
                rt_free(data);
                return 0;
            }
        }
    }
    rt_free(data);
    return -1;
}

static int git_config_append_assignment(GitBuffer *out, const char *var, const char *value) {
    return git_buffer_append_char(out, '\t') != 0 ||
           git_buffer_append_cstr(out, var) != 0 ||
           git_buffer_append_cstr(out, " = ") != 0 ||
           git_buffer_append_cstr(out, value) != 0 ||
           git_buffer_append_char(out, '\n') != 0 ? -1 : 0;
}

static int git_config_set_value(const GitRepo *repo, const char *key, const char *value) {
    char path[GIT_PATH_CAPACITY];
    char section[128];
    char var[64];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;
    GitBuffer out;
    int in_section = 0;
    int saw_section = 0;
    int updated = 0;
    int result = -1;

    if (git_config_key_to_section_var(key, section, sizeof(section), var, sizeof(var)) != 0 || git_join(path, sizeof(path), repo->git_dir, "config") != 0) {
        return -1;
    }
    (void)git_read_file(path, &data, &size);
    rt_memset(&out, 0, sizeof(out));
    while (pos < size) {
        size_t start = pos;
        size_t end;
        size_t trim_start;
        size_t trim_end;
        int has_newline = 0;

        while (pos < size && data[pos] != '\n') pos += 1U;
        end = pos;
        if (pos < size) {
            has_newline = 1;
            pos += 1U;
        }
        trim_start = start;
        trim_end = end;
        git_trim_slice(data, &trim_start, &trim_end);
        if (trim_start < trim_end && data[trim_start] == '[') {
            if (in_section && !updated && git_config_append_assignment(&out, var, value) != 0) {
                goto done;
            }
            in_section = git_slice_equals_cstr(data, trim_start, trim_end, section);
            if (in_section) {
                saw_section = 1;
            }
        } else if (in_section) {
            size_t value_start = 0U;
            size_t value_end = 0U;
            if (git_config_line_has_var(data, trim_start, trim_end, var, &value_start, &value_end)) {
                if (!updated && git_config_append_assignment(&out, var, value) != 0) {
                    goto done;
                }
                updated = 1;
                continue;
            }
        }
        if (git_buffer_append(&out, data + start, end - start) != 0 || git_buffer_append_char(&out, '\n') != 0) {
            goto done;
        }
        (void)has_newline;
    }
    if (in_section && !updated) {
        if (git_config_append_assignment(&out, var, value) != 0) {
            goto done;
        }
        updated = 1;
    }
    if (!saw_section) {
        if (out.size > 0U && out.data[out.size - 1U] != '\n' && git_buffer_append_char(&out, '\n') != 0) {
            goto done;
        }
        if (git_buffer_append_cstr(&out, section) != 0 || git_buffer_append_char(&out, '\n') != 0 || git_config_append_assignment(&out, var, value) != 0) {
            goto done;
        }
    }
    if (git_write_all_file(path, out.data, out.size, 0644U) != 0) {
        goto done;
    }
    result = 0;
done:
    git_buffer_destroy(&out);
    rt_free(data);
    return result;
}

static int git_cmd_config(GitRepo *repo, int argc, char **argv, int argi) {
    char value[GIT_PATH_CAPACITY];

    if (argi >= argc) {
        tool_write_error("git", "config needs a key", 0);
        return 1;
    }
    if (argi + 2 < argc) {
        tool_write_error("git", "too many config arguments", 0);
        return 1;
    }
    if (argi + 1 < argc) {
        if (git_config_set_value(repo, argv[argi], argv[argi + 1]) != 0) {
            tool_write_error("git", "unsupported config key: ", argv[argi]);
            return 1;
        }
        return 0;
    }
    if (git_config_get_value(repo, argv[argi], value, sizeof(value)) != 0) {
        return 1;
    }
    rt_write_line(1, value);
    return 0;
}

static int git_remote_header_name(const unsigned char *data, size_t start, size_t end, char *name, size_t name_size) {
    size_t length;

    if (end < start + 11U || memcmp(data + start, "[remote \"", 9U) != 0 || data[end - 2U] != '"' || data[end - 1U] != ']') {
        return -1;
    }
    length = end - start - 11U;
    if (length == 0U || length >= name_size) {
        return -1;
    }
    memcpy(name, data + start + 9U, length);
    name[length] = '\0';
    return 0;
}

static int git_remote_list(GitRepo *repo, int verbose) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;
    char remote_name[128];
    int in_remote = 0;

    remote_name[0] = '\0';
    if (git_join(path, sizeof(path), repo->git_dir, "config") != 0 || git_read_file(path, &data, &size) != 0) {
        return 0;
    }
    while (pos < size) {
        size_t start = pos;
        size_t end;
        size_t trim_start;
        size_t trim_end;

        while (pos < size && data[pos] != '\n') pos += 1U;
        end = pos;
        if (pos < size) pos += 1U;
        trim_start = start;
        trim_end = end;
        git_trim_slice(data, &trim_start, &trim_end);
        if (trim_start < trim_end && data[trim_start] == '[') {
            in_remote = git_remote_header_name(data, trim_start, trim_end, remote_name, sizeof(remote_name)) == 0;
            if (in_remote && !verbose) {
                rt_write_line(1, remote_name);
            }
        } else if (verbose && in_remote) {
            size_t value_start = 0U;
            size_t value_end = 0U;
            if (git_config_line_has_var(data, trim_start, trim_end, "url", &value_start, &value_end)) {
                rt_write_cstr(1, remote_name);
                rt_write_char(1, '\t');
                rt_write_all(1, data + value_start, value_end - value_start);
                rt_write_line(1, " (fetch)");
                rt_write_cstr(1, remote_name);
                rt_write_char(1, '\t');
                rt_write_all(1, data + value_start, value_end - value_start);
                rt_write_line(1, " (push)");
            }
        }
    }
    rt_free(data);
    return 0;
}

static int git_remote_validate_name(const char *name) {
    size_t i;

    if (name == 0 || name[0] == '\0' || tool_path_is_unsafe_relative(name)) {
        return 0;
    }
    for (i = 0U; name[i] != '\0'; ++i) {
        if (name[i] == '/') {
            return 0;
        }
    }
    return 1;
}

static int git_remote_set_url(GitRepo *repo, const char *name, const char *url, int add_fetch) {
    char key[160];
    char fetchspec[GIT_REF_CAPACITY];
    char refs_dir[GIT_PATH_CAPACITY];

    if (!git_remote_validate_name(name) || rt_strlen(name) + 12U >= sizeof(key)) {
        return -1;
    }
    rt_copy_string(key, sizeof(key), "remote.");
    rt_copy_string(key + rt_strlen(key), sizeof(key) - rt_strlen(key), name);
    rt_copy_string(key + rt_strlen(key), sizeof(key) - rt_strlen(key), ".url");
    if (git_config_set_value(repo, key, url) != 0) {
        return -1;
    }
    key[rt_strlen(key) - 3U] = 'f';
    key[rt_strlen(key) - 2U] = 'e';
    key[rt_strlen(key) - 1U] = 't';
    rt_copy_string(key + rt_strlen(key), sizeof(key) - rt_strlen(key), "ch");
    if (add_fetch) {
        if (git_copy(fetchspec, sizeof(fetchspec), "+refs/heads/*:refs/remotes/") != 0 || rt_strlen(fetchspec) + rt_strlen(name) + 2U >= sizeof(fetchspec)) {
            return -1;
        }
        rt_copy_string(fetchspec + rt_strlen(fetchspec), sizeof(fetchspec) - rt_strlen(fetchspec), name);
        rt_copy_string(fetchspec + rt_strlen(fetchspec), sizeof(fetchspec) - rt_strlen(fetchspec), "/*");
        if (git_config_set_value(repo, key, fetchspec) != 0) {
            return -1;
        }
    }
    if (git_join(refs_dir, sizeof(refs_dir), repo->git_dir, "refs/remotes") == 0 && git_join(refs_dir, sizeof(refs_dir), refs_dir, name) == 0) {
        (void)git_make_directory_chain(refs_dir);
    }
    return 0;
}

static int git_cmd_remote(GitRepo *repo, int argc, char **argv, int argi) {
    if (argi >= argc) {
        return git_remote_list(repo, 0);
    }
    if (rt_strcmp(argv[argi], "-v") == 0) {
        if (argi + 1 < argc) {
            tool_write_error("git", "too many remote arguments", 0);
            return 1;
        }
        return git_remote_list(repo, 1);
    }
    if (rt_strcmp(argv[argi], "add") == 0 || rt_strcmp(argv[argi], "set-url") == 0) {
        int add_fetch = rt_strcmp(argv[argi], "add") == 0;
        if (argi + 3 != argc) {
            tool_write_error("git", add_fetch ? "remote add needs NAME URL" : "remote set-url needs NAME URL", 0);
            return 1;
        }
        if (git_remote_set_url(repo, argv[argi + 1], argv[argi + 2], add_fetch) != 0) {
            tool_write_error("git", "cannot update remote: ", argv[argi + 1]);
            return 1;
        }
        return 0;
    }
    tool_write_error("git", "unsupported remote command: ", argv[argi]);
    return 1;
}

static int git_cmd_fetch(GitRepo *repo, int argc, char **argv, int argi) {
    char remote_url[GIT_PATH_CAPACITY];
    const char *wanted_ref = 0;
    char selected_ref[GIT_REF_CAPACITY];
    unsigned char selected_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];

    if (argi < argc && git_url_is_http(argv[argi])) {
        if (git_copy(remote_url, sizeof(remote_url), argv[argi]) != 0) {
            return 1;
        }
        argi += 1;
    } else if (git_read_origin_url(repo, remote_url, sizeof(remote_url)) != 0) {
        tool_write_error("git", "fetch needs a URL or remote origin", 0);
        return 1;
    }
    if (argi < argc) {
        wanted_ref = argv[argi++];
    }
    if (argi < argc) {
        tool_write_error("git", "too many fetch arguments", 0);
        return 1;
    }
    if (git_fetch_remote_to_repo(repo, remote_url, wanted_ref, selected_ref, sizeof(selected_ref), selected_oid, 0) != 0) {
        tool_write_error("git", "fetch failed: ", remote_url);
        return 1;
    }
    git_format_oid_hex(selected_oid, oid_hex);
    rt_write_cstr(1, "Fetched ");
    rt_write_cstr(1, selected_ref);
    rt_write_cstr(1, " ");
    rt_write_line(1, oid_hex);
    return 0;
}

static int git_update_head_to_oid(GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const char *message) {
    if (repo->head_ref[0] != '\0') {
        return git_write_ref_oid_reflog(repo, repo->head_ref, oid, message, 1);
    }
    return git_write_detached_head_oid_reflog(repo, oid, message);
}

static int git_fast_forward_to_oid(GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack, const char *display_name, const char *message) {
    unsigned char head_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];

    if (repo->head_oid[0] != '\0' && git_parse_oid_hex(repo->head_oid, head_oid) == 0) {
        if (git_oid_equal(head_oid, oid)) {
            rt_write_line(1, "Already up to date.");
            return 0;
        }
        if (!git_commit_is_ancestor_of(repo, head_oid, oid, pack)) {
            tool_write_error("git", "non-fast-forward merge is not implemented: ", display_name);
            return 1;
        }
    }
    if (git_checkout_commit_to_worktree(repo, oid, pack) != 0 || git_update_head_to_oid(repo, oid, message) != 0) {
        tool_write_error("git", "fast-forward failed: ", display_name);
        return 1;
    }
    git_format_oid_hex(oid, oid_hex);
    rt_write_cstr(1, "Fast-forward to ");
    rt_write_line(1, oid_hex);
    return 0;
}

static int git_cmd_merge(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    const char *revision = 0;
    int have_pack;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--ff-only") == 0) {
            argi += 1;
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_error("git", "unsupported merge option: ", argv[argi]);
            return 1;
        } else if (revision == 0) {
            revision = argv[argi++];
        } else {
            tool_write_error("git", "merge needs one revision", 0);
            return 1;
        }
    }
    if (revision == 0 || git_resolve_revision(repo, revision, oid, 0, 0) != 0) {
        tool_write_error("git", "cannot resolve merge revision: ", revision != 0 ? revision : "");
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    {
        int result = git_fast_forward_to_oid(repo, oid, have_pack ? &pack : 0, revision, "merge");
        if (have_pack) git_pack_destroy(&pack);
        return result;
    }
}

static int git_cmd_pull(GitRepo *repo, int argc, char **argv, int argi) {
    char remote_url[GIT_PATH_CAPACITY];
    const char *wanted_ref = 0;
    char selected_ref[GIT_REF_CAPACITY];
    unsigned char selected_oid[CRYPTO_SHA1_DIGEST_SIZE];
    GitPack pack;
    int have_pack = 0;
    int result;

    rt_memset(&pack, 0, sizeof(pack));
    if (argi < argc && git_url_is_http(argv[argi])) {
        if (git_copy(remote_url, sizeof(remote_url), argv[argi]) != 0) return 1;
        argi += 1;
    } else if (git_read_origin_url(repo, remote_url, sizeof(remote_url)) != 0) {
        tool_write_error("git", "pull needs an HTTP(S) URL or remote origin", 0);
        return 1;
    }
    if (argi < argc) {
        wanted_ref = argv[argi++];
    }
    if (argi < argc) {
        tool_write_error("git", "too many pull arguments", 0);
        return 1;
    }
    if (git_fetch_remote_to_repo(repo, remote_url, wanted_ref, selected_ref, sizeof(selected_ref), selected_oid, &pack) != 0) {
        tool_write_error("git", "pull fetch failed: ", remote_url);
        return 1;
    }
    have_pack = 1;
    result = git_fast_forward_to_oid(repo, selected_oid, &pack, selected_ref, "pull");
    if (have_pack) git_pack_destroy(&pack);
    return result;
}

static int git_local_repo_from_arg(const char *arg, GitRepo *repo) {
    char path[GIT_PATH_CAPACITY];
    char objects[GIT_PATH_CAPACITY];
    char refs[GIT_PATH_CAPACITY];
    int is_directory = 0;

    if (git_source_arg_to_path(arg, path, sizeof(path)) != 0) {
        return -1;
    }
    if (git_discover_from(path, repo) == 0 && git_load_head(repo) == 0) {
        return 0;
    }
    rt_memset(repo, 0, sizeof(*repo));
    if (git_copy(repo->work_tree, sizeof(repo->work_tree), path) != 0 || git_copy(repo->git_dir, sizeof(repo->git_dir), path) != 0 || git_join(objects, sizeof(objects), path, "objects") != 0 || git_join(refs, sizeof(refs), path, "refs") != 0) {
        return -1;
    }
    if (platform_path_is_directory(objects, &is_directory) != 0 || !is_directory || platform_path_is_directory(refs, &is_directory) != 0 || !is_directory || git_load_head(repo) != 0) {
        return -1;
    }
    return 0;
}

static int git_push_ref_name(const char *text, char *buffer, size_t buffer_size) {
    if (rt_strncmp(text, "refs/", 5U) == 0) {
        return tool_path_is_unsafe_relative(text) ? -1 : git_copy(buffer, buffer_size, text);
    }
    if (tool_path_is_unsafe_relative(text) || git_copy(buffer, buffer_size, "refs/heads/") != 0 || rt_strlen(buffer) + rt_strlen(text) >= buffer_size) {
        return -1;
    }
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), text);
    return 0;
}

static int git_push_resolve_remote_arg(GitRepo *repo, const char *arg, char *remote_url, size_t remote_url_size) {
    char key[160];

    if (arg == 0) {
        return git_read_origin_url(repo, remote_url, remote_url_size);
    }
    if (git_url_is_http(arg) || rt_strncmp(arg, "ssh://", 6U) == 0 || rt_strncmp(arg, "git://", 6U) == 0 || rt_strncmp(arg, "file://", 7U) == 0 || git_is_absolute_path(arg) || tool_path_has_separator(arg)) {
        return git_copy(remote_url, remote_url_size, arg);
    }
    if (rt_strlen(arg) + 12U >= sizeof(key)) {
        return -1;
    }
    rt_copy_string(key, sizeof(key), "remote.");
    rt_copy_string(key + rt_strlen(key), sizeof(key) - rt_strlen(key), arg);
    rt_copy_string(key + rt_strlen(key), sizeof(key) - rt_strlen(key), ".url");
    return git_config_get_value(repo, key, remote_url, remote_url_size);
}

static int git_copy_objects_to_repo(const GitRepo *source, const GitRepo *dest) {
    char source_objects[GIT_PATH_CAPACITY];
    char dest_objects[GIT_PATH_CAPACITY];

    return git_join(source_objects, sizeof(source_objects), source->git_dir, "objects") != 0 || git_join(dest_objects, sizeof(dest_objects), dest->git_dir, "objects") != 0 || tool_copy_path(source_objects, dest_objects, 1, 1, 1) != 0 ? -1 : 0;
}

static int git_cmd_push(GitRepo *repo, int argc, char **argv, int argi) {
    const char *remote_arg = 0;
    const char *refspec = 0;
    const char *src_name = "HEAD";
    const char *dst_name = 0;
    char remote_url[GIT_PATH_CAPACITY];
    char src_buf[GIT_REF_CAPACITY];
    char dst_ref[GIT_REF_CAPACITY];
    unsigned char new_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char old_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char old_hex[GIT_OBJECT_HEX_SIZE + 1U];
    GitRepo remote_repo;
    GitPack remote_pack;
    int have_old = 0;
    int have_remote_pack = 0;
    int force = 0;
    int result = 1;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--force") == 0 || rt_strcmp(argv[argi], "-f") == 0) {
            force = 1;
            argi += 1;
        } else if (remote_arg == 0) {
            remote_arg = argv[argi++];
        } else if (refspec == 0) {
            refspec = argv[argi++];
        } else {
            tool_write_error("git", "too many push arguments", 0);
            return 1;
        }
    }
    if (refspec != 0) {
        const char *colon = refspec;
        while (*colon != '\0' && *colon != ':') colon += 1;
        if (*colon == ':') {
            if ((size_t)(colon - refspec) >= sizeof(src_buf)) return 1;
            memcpy(src_buf, refspec, (size_t)(colon - refspec));
            src_buf[colon - refspec] = '\0';
            src_name = src_buf[0] != '\0' ? src_buf : "HEAD";
            dst_name = colon[1] != '\0' ? colon + 1 : src_name;
        } else {
            src_name = refspec;
            dst_name = refspec;
        }
    } else if (repo->head_ref[0] != '\0') {
        dst_name = repo->head_ref;
    } else {
        tool_write_error("git", "push needs a refspec for detached HEAD", 0);
        return 1;
    }
    if (git_push_resolve_remote_arg(repo, remote_arg, remote_url, sizeof(remote_url)) != 0) {
        tool_write_error("git", "push needs a local remote or configured origin", 0);
        return 1;
    }
    if (git_url_is_http(remote_url) || rt_strncmp(remote_url, "ssh://", 6U) == 0 || rt_strncmp(remote_url, "git://", 6U) == 0) {
        tool_write_error("git", "network push transport is not implemented: ", remote_url);
        return 1;
    }
    if (git_resolve_revision(repo, src_name, new_oid, 0, 0) != 0 || git_push_ref_name(dst_name, dst_ref, sizeof(dst_ref)) != 0) {
        tool_write_error("git", "cannot resolve push refspec", 0);
        return 1;
    }
    if (git_local_repo_from_arg(remote_url, &remote_repo) != 0) {
        tool_write_error("git", "remote is not a supported local repository: ", remote_url);
        return 1;
    }
    if (git_resolve_ref(&remote_repo, dst_ref, old_hex, sizeof(old_hex)) == 0 && git_parse_oid_hex(old_hex, old_oid) == 0) {
        have_old = 1;
    }
    if (git_copy_objects_to_repo(repo, &remote_repo) != 0) {
        tool_write_error("git", "cannot copy objects to remote", 0);
        return 1;
    }
    have_remote_pack = git_load_pack_cache(&remote_repo, &remote_pack) == 0;
    if (have_old && !force && !git_commit_is_ancestor_of(&remote_repo, old_oid, new_oid, have_remote_pack ? &remote_pack : 0)) {
        tool_write_error("git", "push rejected: non-fast-forward", 0);
        goto done;
    }
    if (git_write_ref_oid_reflog(&remote_repo, dst_ref, new_oid, "push", 0) != 0) {
        tool_write_error("git", "cannot update remote ref: ", dst_ref);
        goto done;
    }
    rt_write_cstr(1, "Pushed ");
    rt_write_cstr(1, src_name);
    rt_write_cstr(1, " to ");
    rt_write_line(1, dst_ref);
    result = 0;
done:
    if (have_remote_pack) git_pack_destroy(&remote_pack);
    return result;
}

static int git_create_branch_at(GitRepo *repo, const char *name, const char *start, unsigned char oid_out[CRYPTO_SHA1_DIGEST_SIZE], char *ref_out, size_t ref_out_size) {
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    char ref_name[GIT_REF_CAPACITY];

    if (tool_path_is_unsafe_relative(name) || git_resolve_revision(repo, start, oid, 0, 0) != 0 || git_copy(ref_name, sizeof(ref_name), "refs/heads/") != 0 || rt_strlen(ref_name) + rt_strlen(name) >= sizeof(ref_name)) {
        return -1;
    }
    rt_copy_string(ref_name + rt_strlen(ref_name), sizeof(ref_name) - rt_strlen(ref_name), name);
    if (git_write_ref_oid_reflog(repo, ref_name, oid, "branch", 0) != 0) {
        return -1;
    }
    if (oid_out != 0) {
        memcpy(oid_out, oid, CRYPTO_SHA1_DIGEST_SIZE);
    }
    if (ref_out != 0 && git_copy(ref_out, ref_out_size, ref_name) != 0) {
        return -1;
    }
    return 0;
}

static int git_checkout_resolved(GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const char *head_ref, const char *display_name) {
    GitPack pack;
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
    char old_hex[GIT_OBJECT_HEX_SIZE + 1U];
    int have_pack;

    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (git_checkout_commit_to_worktree(repo, oid, have_pack ? &pack : 0) != 0) {
        if (have_pack) {
            git_pack_destroy(&pack);
        }
        tool_write_error("git", "checkout failed: ", display_name);
        return 1;
    }
    if (have_pack) {
        git_pack_destroy(&pack);
    }
    if (repo->head_oid[0] != '\0') {
        if (git_copy(old_hex, sizeof(old_hex), repo->head_oid) != 0) {
            git_zero_oid_hex(old_hex);
        }
    } else {
        git_zero_oid_hex(old_hex);
    }
    if (head_ref != 0 && head_ref[0] != '\0') {
        (void)git_write_head_ref(repo, head_ref);
        (void)git_reflog_append_hex(repo, "HEAD", old_hex, oid, "checkout");
    } else {
        char path[GIT_PATH_CAPACITY];
        git_format_oid_hex(oid, oid_hex);
        if (git_join(path, sizeof(path), repo->git_dir, "HEAD") == 0) {
            oid_hex[GIT_OBJECT_HEX_SIZE] = '\n';
            (void)git_write_all_file(path, oid_hex, GIT_OBJECT_HEX_SIZE + 1U, 0644U);
            oid_hex[GIT_OBJECT_HEX_SIZE] = '\0';
            (void)git_reflog_append_hex(repo, "HEAD", old_hex, oid, "checkout");
        }
    }
    git_format_oid_hex(oid, oid_hex);
    rt_write_cstr(1, "Checked out ");
    rt_write_line(1, oid_hex);
    return 0;
}

static int git_cmd_checkout(GitRepo *repo, int argc, char **argv, int argi) {
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    char head_ref[GIT_REF_CAPACITY];

    if (argi >= argc) {
        tool_write_error("git", "checkout needs a ref", 0);
        return 1;
    }
    if (rt_strcmp(argv[argi], "-b") == 0) {
        const char *start = "HEAD";

        if (argi + 1 >= argc) {
            tool_write_error("git", "checkout -b needs a branch name", 0);
            return 1;
        }
        if (argi + 2 < argc) {
            start = argv[argi + 2];
        }
        if (argi + 3 < argc || git_create_branch_at(repo, argv[argi + 1], start, oid, head_ref, sizeof(head_ref)) != 0) {
            tool_write_error("git", "cannot create branch: ", argv[argi + 1]);
            return 1;
        }
        return git_checkout_resolved(repo, oid, head_ref, argv[argi + 1]);
    }
    if (argi + 1 < argc) {
        tool_write_error("git", "too many checkout arguments", 0);
        return 1;
    }
    if (git_resolve_revision(repo, argv[argi], oid, head_ref, sizeof(head_ref)) != 0) {
        tool_write_error("git", "cannot resolve checkout ref: ", argv[argi]);
        return 1;
    }
    return git_checkout_resolved(repo, oid, head_ref, argv[argi]);
}

static int git_cmd_switch(GitRepo *repo, int argc, char **argv, int argi) {
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    char head_ref[GIT_REF_CAPACITY];

    if (argi >= argc) {
        tool_write_error("git", "switch needs a branch", 0);
        return 1;
    }
    if (rt_strcmp(argv[argi], "-c") == 0 || rt_strcmp(argv[argi], "--create") == 0) {
        const char *start = "HEAD";

        if (argi + 1 >= argc) {
            tool_write_error("git", "switch -c needs a branch name", 0);
            return 1;
        }
        if (argi + 2 < argc) {
            start = argv[argi + 2];
        }
        if (argi + 3 < argc || git_create_branch_at(repo, argv[argi + 1], start, oid, head_ref, sizeof(head_ref)) != 0) {
            tool_write_error("git", "cannot create branch: ", argv[argi + 1]);
            return 1;
        }
        return git_checkout_resolved(repo, oid, head_ref, argv[argi + 1]);
    }
    if (argi + 1 < argc) {
        tool_write_error("git", "too many switch arguments", 0);
        return 1;
    }
    if (git_resolve_revision(repo, argv[argi], oid, head_ref, sizeof(head_ref)) != 0 || head_ref[0] == '\0') {
        tool_write_error("git", "cannot switch to branch: ", argv[argi]);
        return 1;
    }
    return git_checkout_resolved(repo, oid, head_ref, argv[argi]);
}

static int git_cmd_status(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    GitIndex head_index;
    GitIgnoreList ignores;
    GitPack pack;
    int have_pack = 0;
    int have_head_index = 0;
    int short_output = 0;
    int porcelain_format = 0;
    int nul_terminate = 0;
    int color_mode = TOOL_COLOR_AUTO;
    int saw_change = 0;
    int result = 1;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--short") == 0 || rt_strcmp(argv[argi], "-s") == 0) {
            short_output = 1;
        } else if (rt_strcmp(argv[argi], "--porcelain") == 0 || rt_strcmp(argv[argi], "--porcelain=v1") == 0) {
            short_output = 1;
            porcelain_format = 1;
        } else if (rt_strcmp(argv[argi], "-z") == 0) {
            short_output = 1;
            porcelain_format = 1;
            nul_terminate = 1;
        } else if (rt_strcmp(argv[argi], "--color") == 0) {
            color_mode = TOOL_COLOR_ALWAYS;
        } else if (rt_strncmp(argv[argi], "--color=", 8U) == 0) {
            if (tool_parse_color_mode(argv[argi] + 8U, &color_mode) != 0) {
                tool_write_error("git", "unsupported status color mode: ", argv[argi] + 8U);
                return 1;
            }
        } else if (rt_strcmp(argv[argi], "--no-color") == 0) {
            color_mode = TOOL_COLOR_NEVER;
        } else {
            tool_write_error("git", "unsupported status option: ", argv[argi]);
            return 1;
        }
        argi += 1;
    }
    if (porcelain_format) {
        color_mode = TOOL_COLOR_NEVER;
    }
    if (git_load_index(repo, &index) != 0) {
        tool_write_error("git", "cannot read index", 0);
        return 1;
    }
    if (index.count > 1U && !git_index_is_sorted(&index)) {
        rt_sort(index.entries, index.count, sizeof(index.entries[0]), git_compare_entries_by_path);
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    have_head_index = git_load_head_tree_index(repo, have_pack ? &pack : 0, &head_index) == 0;
    if (git_ignore_load(repo, &ignores) != 0) {
        if (have_head_index) git_index_destroy(&head_index);
        if (have_pack) git_pack_destroy(&pack);
        git_index_destroy(&index);
        tool_write_error("git", "cannot read ignore files", 0);
        return 1;
    }
    if ((have_head_index ? git_status_tracked_with_head(repo, &head_index, &index, short_output, color_mode, nul_terminate, &saw_change) : git_status_tracked(repo, &index, short_output, color_mode, nul_terminate, &saw_change)) != 0 ||
        git_status_untracked(repo, &index, &ignores, short_output, color_mode, nul_terminate, &saw_change) != 0) {
        goto done;
    }
    if (!short_output && !saw_change) {
        rt_write_line(1, "nothing to commit, working tree clean");
    }
    result = 0;
done:
    git_ignore_destroy(&ignores);
    if (have_head_index) git_index_destroy(&head_index);
    if (have_pack) git_pack_destroy(&pack);
    git_index_destroy(&index);
    return result;
}

static int git_split_revision_range(const char *text, char *left, size_t left_size, char *right, size_t right_size) {
    size_t position = 0U;

    while (text[position] != '\0') {
        if (text[position] == '.' && text[position + 1U] == '.') {
            if (position == 0U || text[position + 2U] == '\0') {
                return -1;
            }
            if (position >= left_size || rt_strlen(text + position + 2U) >= right_size) {
                return -1;
            }
            memcpy(left, text, position);
            left[position] = '\0';
            rt_copy_string(right, right_size, text + position + 2U);
            return 0;
        }
        position += 1U;
    }
    return -1;
}

static int git_cmd_diff(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    GitIndex head_index;
    GitIndex old_tree_index;
    GitIndex new_tree_index;
    GitPack pack;
    GitDiffStatList stats;
    int have_pack = 0;
    int have_head_index = 0;
    int have_old_tree_index = 0;
    int have_new_tree_index = 0;
    GitDiffOutputMode output_mode = GIT_DIFF_OUTPUT_PATCH;
    int cached_mode = 0;
    int exit_code_mode = 0;
    int nul_terminate = 0;
    int saw_separator = 0;
    int compare_commits = 0;
    int color_mode = TOOL_COLOR_AUTO;
    int pathspec_start;
    int pathspec_count;
    unsigned char old_commit_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char new_commit_oid[CRYPTO_SHA1_DIGEST_SIZE];
    size_t i;
    int result = 1;
    int change_count = 0;

    rt_memset(&stats, 0, sizeof(stats));
    rt_memset(&old_tree_index, 0, sizeof(old_tree_index));
    rt_memset(&new_tree_index, 0, sizeof(new_tree_index));
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--stat") == 0) {
            output_mode = GIT_DIFF_OUTPUT_STAT;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--name-only") == 0) {
            output_mode = GIT_DIFF_OUTPUT_NAME_ONLY;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--name-status") == 0) {
            output_mode = GIT_DIFF_OUTPUT_NAME_STATUS;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--quiet") == 0) {
            output_mode = GIT_DIFF_OUTPUT_QUIET;
            exit_code_mode = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-z") == 0) {
            nul_terminate = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--exit-code") == 0) {
            exit_code_mode = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--cached") == 0 || rt_strcmp(argv[argi], "--staged") == 0) {
            cached_mode = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--color") == 0) {
            color_mode = TOOL_COLOR_ALWAYS;
            argi += 1;
        } else if (rt_strncmp(argv[argi], "--color=", 8U) == 0) {
            if (tool_parse_color_mode(argv[argi] + 8U, &color_mode) != 0) {
                tool_write_error("git", "unsupported diff color mode: ", argv[argi] + 8U);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--no-color") == 0) {
            color_mode = TOOL_COLOR_NEVER;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            saw_separator = 1;
            argi += 1;
            break;
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_error("git", "unsupported diff option: ", argv[argi]);
            return 1;
        } else {
            break;
        }
    }
    pathspec_start = argi;
    pathspec_count = argc - argi;
    if (!cached_mode && !saw_separator && argi < argc) {
        int separator_index = -1;
        int scan;
        char range_left[GIT_REF_CAPACITY];
        char range_right[GIT_REF_CAPACITY];

        for (scan = argi; scan < argc; ++scan) {
            if (rt_strcmp(argv[scan], "--") == 0) {
                separator_index = scan;
                break;
            }
        }
        if (git_split_revision_range(argv[argi], range_left, sizeof(range_left), range_right, sizeof(range_right)) == 0 &&
            git_resolve_revision(repo, range_left, old_commit_oid, 0, 0) == 0 &&
            git_resolve_revision(repo, range_right, new_commit_oid, 0, 0) == 0) {
            compare_commits = 1;
            pathspec_start = separator_index >= 0 ? separator_index + 1 : argi + 1;
            pathspec_count = argc - pathspec_start;
        } else if ((separator_index < 0 ? argc - argi : separator_index - argi) >= 2 &&
                   git_resolve_revision(repo, argv[argi], old_commit_oid, 0, 0) == 0 &&
                   git_resolve_revision(repo, argv[argi + 1], new_commit_oid, 0, 0) == 0) {
            compare_commits = 1;
            pathspec_start = separator_index >= 0 ? separator_index + 1 : argi + 2;
            pathspec_count = argc - pathspec_start;
        } else if (separator_index >= 0) {
            pathspec_start = separator_index + 1;
            pathspec_count = argc - pathspec_start;
        }
    }
    if (git_load_index(repo, &index) != 0) {
        tool_write_error("git", "cannot read index", 0);
        return 1;
    }
    if (index.count > 1U && !git_index_is_sorted(&index)) {
        rt_sort(index.entries, index.count, sizeof(index.entries[0]), git_compare_entries_by_path);
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (compare_commits) {
        if (git_load_commit_tree_index(repo, old_commit_oid, have_pack ? &pack : 0, &old_tree_index) != 0 ||
            git_load_commit_tree_index(repo, new_commit_oid, have_pack ? &pack : 0, &new_tree_index) != 0) {
            tool_write_error("git", "cannot read commit tree", 0);
            goto done;
        }
        have_old_tree_index = 1;
        have_new_tree_index = 1;
        if (git_diff_index_pair(repo, &old_tree_index, &new_tree_index, have_pack ? &pack : 0, output_mode, color_mode, nul_terminate, argv + pathspec_start, pathspec_count, &stats, &change_count) != 0) {
            tool_write_error("git", "cannot diff commits", 0);
            goto done;
        }
    } else if (cached_mode) {
        have_head_index = git_load_head_tree_index(repo, have_pack ? &pack : 0, &head_index) == 0;
        if (!have_head_index) {
            tool_write_error("git", "cannot read HEAD tree", 0);
            goto done;
        }
        if (git_diff_index_pair(repo, &head_index, &index, have_pack ? &pack : 0, output_mode, color_mode, nul_terminate, argv + pathspec_start, pathspec_count, &stats, &change_count) != 0) {
            tool_write_error("git", "cannot diff staged changes", 0);
            goto done;
        }
    } else {
        for (i = 0U; i < index.count; ++i) {
            int modified;

            if (!git_pathspec_matches(index.entries[i].path, argv + pathspec_start, pathspec_count)) {
                continue;
            }
            modified = git_entry_is_modified(repo, &index.entries[i]);
            if (modified == 0) {
                continue;
            }
            change_count += 1;
            if (output_mode == GIT_DIFF_OUTPUT_QUIET) {
                break;
            }
            if (output_mode == GIT_DIFF_OUTPUT_STAT) {
                if (git_collect_diff_stat_entry(repo, &index.entries[i], have_pack ? &pack : 0, &stats) != 0) {
                    tool_write_error("git", "cannot diff path: ", index.entries[i].path);
                    goto done;
                }
            } else if (output_mode == GIT_DIFF_OUTPUT_NAME_ONLY || output_mode == GIT_DIFF_OUTPUT_NAME_STATUS) {
                if (git_render_diff_path_mode(output_mode, modified < 0 ? 'D' : 'M', index.entries[i].path, nul_terminate) != 0) {
                    goto done;
                }
            } else if (output_mode == GIT_DIFF_OUTPUT_PATCH && git_render_worktree_diff_patch_entry(repo, &index.entries[i], have_pack ? &pack : 0, color_mode) != 0) {
                tool_write_error("git", "cannot diff path: ", index.entries[i].path);
                goto done;
            }
        }
    }
    if (output_mode == GIT_DIFF_OUTPUT_STAT && git_render_diff_stat(&stats, color_mode) != 0) {
        goto done;
    }
    result = exit_code_mode && change_count > 0 ? 1 : 0;
done:
    if (have_new_tree_index) {
        git_index_destroy(&new_tree_index);
    }
    if (have_old_tree_index) {
        git_index_destroy(&old_tree_index);
    }
    if (have_head_index) {
        git_index_destroy(&head_index);
    }
    if (have_pack) {
        git_pack_destroy(&pack);
    }
    git_diff_stat_list_destroy(&stats);
    git_index_destroy(&index);
    return result;
}

static int git_cmd_branch(GitRepo *repo, int argc, char **argv, int argi) {
    const char *branch;

    if (argi < argc && rt_strcmp(argv[argi], "--show-current") == 0) {
        branch = git_branch_name(repo);
        if (branch != 0) {
            rt_write_line(1, branch);
        } else {
            rt_write_line(1, "");
        }
        return 0;
    }
    if (argi < argc && (rt_strcmp(argv[argi], "-d") == 0 || rt_strcmp(argv[argi], "-D") == 0 || rt_strcmp(argv[argi], "--delete") == 0)) {
        char ref_path[GIT_PATH_CAPACITY];
        char ref_name[GIT_REF_CAPACITY];

        argi += 1;
        if (argi >= argc) {
            tool_write_error("git", "branch delete needs a name", 0);
            return 1;
        }
        if (tool_path_is_unsafe_relative(argv[argi]) || git_copy(ref_name, sizeof(ref_name), "refs/heads/") != 0 || rt_strlen(ref_name) + rt_strlen(argv[argi]) >= sizeof(ref_name)) {
            tool_write_error("git", "bad branch name: ", argv[argi]);
            return 1;
        }
        rt_copy_string(ref_name + rt_strlen(ref_name), sizeof(ref_name) - rt_strlen(ref_name), argv[argi]);
        if (git_join(ref_path, sizeof(ref_path), repo->git_dir, ref_name) != 0 || platform_remove_file(ref_path) != 0) {
            tool_write_error("git", "cannot delete branch: ", argv[argi]);
            return 1;
        }
        rt_write_cstr(1, "Deleted branch ");
        rt_write_line(1, argv[argi]);
        return 0;
    }
    if (argi < argc) {
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
        const char *start = "HEAD";
        char ref_name[GIT_REF_CAPACITY];

        if (argi + 1 < argc) {
            start = argv[argi + 1];
        }
        if (argi + 2 < argc) {
            tool_write_error("git", "too many branch arguments", 0);
            return 1;
        }
        if (tool_path_is_unsafe_relative(argv[argi]) || git_resolve_revision(repo, start, oid, 0, 0) != 0 || git_copy(ref_name, sizeof(ref_name), "refs/heads/") != 0 || rt_strlen(ref_name) + rt_strlen(argv[argi]) >= sizeof(ref_name)) {
            tool_write_error("git", "cannot create branch: ", argv[argi]);
            return 1;
        }
        rt_copy_string(ref_name + rt_strlen(ref_name), sizeof(ref_name) - rt_strlen(ref_name), argv[argi]);
        if (git_write_ref_oid(repo, ref_name, oid) != 0) {
            tool_write_error("git", "cannot write branch: ", argv[argi]);
            return 1;
        }
        return 0;
    }
    {
        char heads_dir[GIT_PATH_CAPACITY];
        PlatformDirEntry entries[256];
        size_t count = 0U;
        int is_directory = 0;
        size_t i;

        branch = git_branch_name(repo);
        if (git_join(heads_dir, sizeof(heads_dir), repo->git_dir, "refs/heads") != 0 || platform_collect_entries(heads_dir, 0, entries, sizeof(entries) / sizeof(entries[0]), &count, &is_directory) != 0 || !is_directory) {
            return 0;
        }
        for (i = 0U; i < count; ++i) {
            if (entries[i].is_dir) {
                continue;
            }
            rt_write_cstr(1, branch != 0 && rt_strcmp(branch, entries[i].name) == 0 ? "* " : "  ");
            rt_write_line(1, entries[i].name);
        }
    }
    return 0;
}

static int git_cmd_rev_parse(GitRepo *repo, int argc, char **argv, int argi) {
    int exit_code = 0;
    int verify = 0;
    int short_length = 0;

    if (argi >= argc) {
        tool_write_error("git", "rev-parse needs an argument", 0);
        return 1;
    }
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--show-toplevel") == 0) {
            rt_write_line(1, repo->work_tree);
        } else if (rt_strcmp(argv[argi], "--git-dir") == 0) {
            rt_write_line(1, repo->git_dir);
        } else if (rt_strcmp(argv[argi], "--is-inside-work-tree") == 0) {
            rt_write_line(1, "true");
        } else if (rt_strcmp(argv[argi], "--verify") == 0) {
            verify = 1;
        } else if (rt_strcmp(argv[argi], "--short") == 0) {
            short_length = 7;
        } else if (rt_strncmp(argv[argi], "--short=", 8U) == 0) {
            long long value = 0;
            if (tool_parse_int_arg(argv[argi] + 8U, &value, "git", "short length") != 0 || value < 1 || value > GIT_OBJECT_HEX_SIZE) {
                return 1;
            }
            short_length = (int)value;
        } else if (rt_strcmp(argv[argi], "--abbrev-ref") == 0 && argi + 1 < argc && rt_strcmp(argv[argi + 1], "HEAD") == 0) {
            const char *branch = git_branch_name(repo);

            rt_write_line(1, branch != 0 ? branch : "HEAD");
            argi += 1;
        } else {
            unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
            char hex[GIT_OBJECT_HEX_SIZE + 1U];

            if (git_resolve_revision(repo, argv[argi], oid, 0, 0) != 0) {
                exit_code = 1;
                if (!verify) {
                    tool_write_error("git", "unsupported rev-parse argument: ", argv[argi]);
                }
            } else {
                git_format_oid_hex(oid, hex);
                if (short_length > 0) {
                    hex[short_length] = '\0';
                }
                rt_write_line(1, hex);
            }
        }
        argi += 1;
    }
    return exit_code;
}

static int git_resolve_objectish(GitRepo *repo, const char *name, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    if (git_resolve_revision(repo, name, oid, 0, 0) == 0) {
        return 0;
    }
    if (rt_strlen(name) == GIT_OBJECT_HEX_SIZE && git_parse_oid_hex(name, oid) == 0) {
        return 0;
    }
    return -1;
}

static int git_resolve_treeish(GitRepo *repo, const char *name, const GitPack *pack_cache, unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    int type = 0;
    unsigned char *data = 0;
    size_t size = 0U;
    int result = -1;

    if (git_resolve_objectish(repo, name, oid) != 0 || git_read_object(repo, oid, pack_cache, &type, &data, &size) != 0) {
        rt_free(data);
        return -1;
    }
    if (type == GIT_OBJECT_TREE) {
        memcpy(tree_oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
        result = 0;
    } else if (type == GIT_OBJECT_COMMIT && size >= 46U && memcmp(data, "tree ", 5U) == 0 && git_parse_oid_hex_n((const char *)data + 5U, GIT_OBJECT_HEX_SIZE, tree_oid) == 0) {
        result = 0;
    }
    rt_free(data);
    return result;
}

static const char *git_ls_tree_mode_text(unsigned int mode) {
    if (mode == GIT_MODE_TREE || mode == 040000U) return "040000";
    if (mode == GIT_MODE_SYMLINK || mode == 0120000U) return "120000";
    if (mode == GIT_MODE_GITLINK || mode == 0160000U) return "160000";
    return (mode & GIT_MODE_EXEC_BITS) != 0U ? "100755" : "100644";
}

static const char *git_tree_entry_type_name(unsigned int mode) {
    if (mode == GIT_MODE_TREE || mode == 040000U) return "tree";
    if (mode == GIT_MODE_GITLINK || mode == 0160000U) return "commit";
    return "blob";
}

static int git_print_tree_entries(const GitRepo *repo, const unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, const char *prefix, int recursive, int name_only, int print_trees) {
    int type = 0;
    unsigned char *tree = 0;
    size_t tree_size = 0U;
    size_t pos = 0U;

    if (git_read_object(repo, tree_oid, pack_cache, &type, &tree, &tree_size) != 0 || type != GIT_OBJECT_TREE) {
        rt_free(tree);
        return -1;
    }
    while (pos < tree_size) {
        unsigned int mode = 0U;
        size_t name_start;
        size_t name_length;
        char path[GIT_PATH_CAPACITY];
        char hex[GIT_OBJECT_HEX_SIZE + 1U];
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
        int is_tree;

        while (pos < tree_size && tree[pos] >= '0' && tree[pos] <= '7') {
            mode = mode * 8U + (unsigned int)(tree[pos] - '0');
            pos += 1U;
        }
        if (pos >= tree_size || tree[pos] != ' ') {
            rt_free(tree);
            return -1;
        }
        pos += 1U;
        name_start = pos;
        while (pos < tree_size && tree[pos] != '\0') pos += 1U;
        if (pos >= tree_size || pos + 1U + CRYPTO_SHA1_DIGEST_SIZE > tree_size) {
            rt_free(tree);
            return -1;
        }
        name_length = pos - name_start;
        pos += 1U;
        memcpy(oid, tree + pos, CRYPTO_SHA1_DIGEST_SIZE);
        pos += CRYPTO_SHA1_DIGEST_SIZE;
        if (prefix[0] != '\0') {
            if (rt_strlen(prefix) + 1U + name_length >= sizeof(path)) {
                rt_free(tree);
                return -1;
            }
            rt_copy_string(path, sizeof(path), prefix);
            path[rt_strlen(path) + 1U] = '\0';
            path[rt_strlen(path)] = '/';
            memcpy(path + rt_strlen(prefix) + 1U, tree + name_start, name_length);
            path[rt_strlen(prefix) + 1U + name_length] = '\0';
        } else {
            if (name_length >= sizeof(path)) {
                rt_free(tree);
                return -1;
            }
            memcpy(path, tree + name_start, name_length);
            path[name_length] = '\0';
        }
        is_tree = mode == GIT_MODE_TREE || mode == 040000U;
        if (!recursive || !is_tree || print_trees) {
            if (name_only) {
                if (rt_write_line(1, path) != 0) {
                    rt_free(tree);
                    return -1;
                }
            } else {
                git_format_oid_hex(oid, hex);
                if (rt_write_cstr(1, git_ls_tree_mode_text(mode)) != 0 || rt_write_char(1, ' ') != 0 || rt_write_cstr(1, git_tree_entry_type_name(mode)) != 0 || rt_write_char(1, ' ') != 0 || rt_write_cstr(1, hex) != 0 || rt_write_char(1, '\t') != 0 || rt_write_line(1, path) != 0) {
                    rt_free(tree);
                    return -1;
                }
            }
        }
        if (recursive && is_tree && git_print_tree_entries(repo, oid, pack_cache, path, recursive, name_only, print_trees) != 0) {
            rt_free(tree);
            return -1;
        }
    }
    rt_free(tree);
    return 0;
}

static int git_cmd_cat_file(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char *data = 0;
    size_t size = 0U;
    int type = 0;
    int have_pack;
    int result = 1;

    if (argi + 2 != argc || (rt_strcmp(argv[argi], "-t") != 0 && rt_strcmp(argv[argi], "-s") != 0 && rt_strcmp(argv[argi], "-p") != 0)) {
        tool_write_error("git", "cat-file needs -t, -s, or -p and an object", 0);
        return 1;
    }
    if (git_resolve_objectish(repo, argv[argi + 1], oid) != 0) {
        tool_write_error("git", "cannot resolve object: ", argv[argi + 1]);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (git_read_object(repo, oid, have_pack ? &pack : 0, &type, &data, &size) != 0) {
        tool_write_error("git", "cannot read object: ", argv[argi + 1]);
        goto done;
    }
    if (rt_strcmp(argv[argi], "-t") == 0) {
        rt_write_line(1, git_object_type_name(type));
    } else if (rt_strcmp(argv[argi], "-s") == 0) {
        git_write_size(size);
        rt_write_char(1, '\n');
    } else if (type == GIT_OBJECT_TREE) {
        if (git_print_tree_entries(repo, oid, have_pack ? &pack : 0, "", 0, 0, 1) != 0) {
            goto done;
        }
    } else if (rt_write_all(1, data, size) != 0) {
        goto done;
    }
    result = 0;
done:
    rt_free(data);
    if (have_pack) git_pack_destroy(&pack);
    return result;
}

static int git_cmd_ls_tree(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE];
    const char *revision = "HEAD";
    int recursive = 0;
    int name_only = 0;
    int have_pack;
    int result;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-r") == 0) {
            recursive = 1;
        } else if (rt_strcmp(argv[argi], "--name-only") == 0) {
            name_only = 1;
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_error("git", "unsupported ls-tree option: ", argv[argi]);
            return 1;
        } else {
            revision = argv[argi];
        }
        argi += 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (git_resolve_treeish(repo, revision, have_pack ? &pack : 0, tree_oid) != 0) {
        if (have_pack) git_pack_destroy(&pack);
        tool_write_error("git", "cannot resolve tree: ", revision);
        return 1;
    }
    result = git_print_tree_entries(repo, tree_oid, have_pack ? &pack : 0, "", recursive, name_only, 0) == 0 ? 0 : 1;
    if (have_pack) git_pack_destroy(&pack);
    return result;
}

static int git_ref_filter_matches(const char *ref_name, int heads_only, int tags_only, const char *verify_ref) {
    if (verify_ref != 0) {
        return rt_strcmp(ref_name, verify_ref) == 0;
    }
    if (heads_only && rt_strncmp(ref_name, "refs/heads/", 11U) != 0) {
        return 0;
    }
    if (tags_only && rt_strncmp(ref_name, "refs/tags/", 10U) != 0) {
        return 0;
    }
    return 1;
}

static int git_show_ref_line(const char *oid_hex, const char *ref_name) {
    return rt_write_cstr(1, oid_hex) != 0 || rt_write_char(1, ' ') != 0 || rt_write_line(1, ref_name) != 0 ? -1 : 0;
}

static int git_show_ref_loose_dir(const GitRepo *repo, const char *relative, int heads_only, int tags_only, const char *verify_ref, int *count) {
    char dir[GIT_PATH_CAPACITY];
    PlatformDirEntry entries[256];
    size_t entry_count = 0U;
    int is_directory = 0;
    size_t i;

    if (git_join(dir, sizeof(dir), repo->git_dir, relative) != 0 || platform_collect_entries(dir, 0, entries, sizeof(entries) / sizeof(entries[0]), &entry_count, &is_directory) != 0 || !is_directory) {
        return 0;
    }
    for (i = 0U; i < entry_count; ++i) {
        char child[GIT_REF_CAPACITY];
        char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];

        if (git_copy(child, sizeof(child), relative) != 0 || rt_strlen(child) + 1U + rt_strlen(entries[i].name) >= sizeof(child)) {
            return -1;
        }
        rt_copy_string(child + rt_strlen(child), sizeof(child) - rt_strlen(child), "/");
        rt_copy_string(child + rt_strlen(child), sizeof(child) - rt_strlen(child), entries[i].name);
        if (entries[i].is_dir) {
            if (git_show_ref_loose_dir(repo, child, heads_only, tags_only, verify_ref, count) != 0) {
                return -1;
            }
        } else if (git_ref_filter_matches(child, heads_only, tags_only, verify_ref) && git_read_ref_file(repo, child, oid_hex, sizeof(oid_hex)) == 0) {
            if (git_show_ref_line(oid_hex, child) != 0) {
                return -1;
            }
            *count += 1;
        }
    }
    return 0;
}

static int git_show_ref_packed(const GitRepo *repo, int heads_only, int tags_only, const char *verify_ref, int *count) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;

    if (git_join(path, sizeof(path), repo->git_dir, "packed-refs") != 0 || git_read_file(path, &data, &size) != 0) {
        return 0;
    }
    while (pos < size) {
        size_t start = pos;
        size_t end;
        size_t line_len;

        while (pos < size && data[pos] != '\n') pos += 1U;
        end = pos;
        if (pos < size) pos += 1U;
        while (end > start && data[end - 1U] == '\r') end -= 1U;
        line_len = end - start;
        if (line_len > GIT_OBJECT_HEX_SIZE + 1U && data[start] != '#' && data[start] != '^' && data[start + GIT_OBJECT_HEX_SIZE] == ' ') {
            char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
            char ref_name[GIT_REF_CAPACITY];
            size_t ref_len = line_len - GIT_OBJECT_HEX_SIZE - 1U;

            if (ref_len < sizeof(ref_name)) {
                memcpy(oid_hex, data + start, GIT_OBJECT_HEX_SIZE);
                oid_hex[GIT_OBJECT_HEX_SIZE] = '\0';
                memcpy(ref_name, data + start + GIT_OBJECT_HEX_SIZE + 1U, ref_len);
                ref_name[ref_len] = '\0';
                if (git_ref_filter_matches(ref_name, heads_only, tags_only, verify_ref)) {
                    if (git_show_ref_line(oid_hex, ref_name) != 0) {
                        rt_free(data);
                        return -1;
                    }
                    *count += 1;
                }
            }
        }
    }
    rt_free(data);
    return 0;
}

static int git_cmd_show_ref(GitRepo *repo, int argc, char **argv, int argi) {
    int heads_only = 0;
    int tags_only = 0;
    const char *verify_ref = 0;
    int count = 0;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--heads") == 0) {
            heads_only = 1;
        } else if (rt_strcmp(argv[argi], "--tags") == 0) {
            tags_only = 1;
        } else if (rt_strcmp(argv[argi], "--verify") == 0 && argi + 1 < argc) {
            verify_ref = argv[++argi];
        } else {
            tool_write_error("git", "unsupported show-ref argument: ", argv[argi]);
            return 1;
        }
        argi += 1;
    }
    if (git_show_ref_loose_dir(repo, "refs", heads_only, tags_only, verify_ref, &count) != 0 || git_show_ref_packed(repo, heads_only, tags_only, verify_ref, &count) != 0) {
        return 1;
    }
    return count > 0 ? 0 : 1;
}

static int git_tag_list_loose(const GitRepo *repo, const char *relative, int *count) {
    char dir[GIT_PATH_CAPACITY];
    PlatformDirEntry entries[256];
    size_t entry_count = 0U;
    int is_directory = 0;
    size_t i;

    if (git_join(dir, sizeof(dir), repo->git_dir, relative) != 0 || platform_collect_entries(dir, 0, entries, sizeof(entries) / sizeof(entries[0]), &entry_count, &is_directory) != 0 || !is_directory) {
        return 0;
    }
    for (i = 0U; i < entry_count; ++i) {
        char child[GIT_REF_CAPACITY];

        if (git_copy(child, sizeof(child), relative) != 0 || rt_strlen(child) + 1U + rt_strlen(entries[i].name) >= sizeof(child)) {
            return -1;
        }
        rt_copy_string(child + rt_strlen(child), sizeof(child) - rt_strlen(child), "/");
        rt_copy_string(child + rt_strlen(child), sizeof(child) - rt_strlen(child), entries[i].name);
        if (entries[i].is_dir) {
            if (git_tag_list_loose(repo, child, count) != 0) return -1;
        } else {
            rt_write_line(1, child + 10U);
            *count += 1;
        }
    }
    return 0;
}

static int git_tag_list_packed(const GitRepo *repo, int *count) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;

    if (git_join(path, sizeof(path), repo->git_dir, "packed-refs") != 0 || git_read_file(path, &data, &size) != 0) {
        return 0;
    }
    while (pos < size) {
        size_t start = pos;
        size_t end;
        while (pos < size && data[pos] != '\n') pos += 1U;
        end = pos;
        if (pos < size) pos += 1U;
        if (end > start + GIT_OBJECT_HEX_SIZE + 11U && data[start] != '#' && data[start] != '^' && data[start + GIT_OBJECT_HEX_SIZE] == ' ' && memcmp(data + start + GIT_OBJECT_HEX_SIZE + 1U, "refs/tags/", 10U) == 0) {
            rt_write_all(1, data + start + GIT_OBJECT_HEX_SIZE + 11U, end - start - GIT_OBJECT_HEX_SIZE - 11U);
            rt_write_char(1, '\n');
            *count += 1;
        }
    }
    rt_free(data);
    return 0;
}

static int git_cmd_tag(GitRepo *repo, int argc, char **argv, int argi) {
    if (argi >= argc) {
        int count = 0;
        if (git_tag_list_loose(repo, "refs/tags", &count) != 0 || git_tag_list_packed(repo, &count) != 0) {
            return 1;
        }
        (void)count;
        return 0;
    }
    if (argi + 2 < argc) {
        tool_write_error("git", "too many tag arguments", 0);
        return 1;
    }
    {
        const char *start = argi + 1 < argc ? argv[argi + 1] : "HEAD";
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
        char ref_name[GIT_REF_CAPACITY];

        if (tool_path_is_unsafe_relative(argv[argi]) || git_resolve_revision(repo, start, oid, 0, 0) != 0 || git_copy(ref_name, sizeof(ref_name), "refs/tags/") != 0 || rt_strlen(ref_name) + rt_strlen(argv[argi]) >= sizeof(ref_name)) {
            tool_write_error("git", "cannot create tag: ", argv[argi]);
            return 1;
        }
        rt_copy_string(ref_name + rt_strlen(ref_name), sizeof(ref_name) - rt_strlen(ref_name), argv[argi]);
        if (git_write_ref_oid(repo, ref_name, oid) != 0) {
            tool_write_error("git", "cannot write tag: ", argv[argi]);
            return 1;
        }
    }
    return 0;
}

static size_t git_patch_line_end(const unsigned char *data, size_t size, size_t start) {
    size_t end = start;

    while (end < size && data[end] != '\n') {
        end += 1U;
    }
    if (end < size) {
        end += 1U;
    }
    return end;
}

static size_t git_file_line_end(const unsigned char *data, size_t size, size_t start) {
    size_t end = start;

    while (end < size && data[end] != '\n') {
        end += 1U;
    }
    if (end < size) {
        end += 1U;
    }
    return end;
}

static int git_parse_hunk_start(const unsigned char *line, size_t line_size, size_t *old_start_out) {
    size_t pos = 4U;
    size_t value = 0U;

    if (line_size < 5U || memcmp(line, "@@ -", 4U) != 0) {
        return -1;
    }
    while (pos < line_size && line[pos] >= '0' && line[pos] <= '9') {
        value = value * 10U + (size_t)(line[pos] - '0');
        pos += 1U;
    }
    if (value == 0U) {
        value = 1U;
    }
    *old_start_out = value;
    return 0;
}

static int git_copy_old_lines_until(GitBuffer *out, const unsigned char *old_data, size_t old_size, size_t *old_pos_io, size_t *old_line_io, size_t target_line) {
    while (*old_line_io < target_line && *old_pos_io < old_size) {
        size_t end = git_file_line_end(old_data, old_size, *old_pos_io);
        if (git_buffer_append(out, old_data + *old_pos_io, end - *old_pos_io) != 0) {
            return -1;
        }
        *old_pos_io = end;
        *old_line_io += 1U;
    }
    return 0;
}

static int git_apply_one_file(const GitRepo *repo, const char *path, const unsigned char *patch, size_t patch_size, size_t *pos_io, int delete_file, int check_only) {
    char full_path[GIT_PATH_CAPACITY];
    unsigned char *old_data = 0;
    size_t old_size = 0U;
    size_t old_pos = 0U;
    size_t old_line = 1U;
    GitBuffer out;
    int result = -1;

    if (path[0] == '\0' || tool_path_is_unsafe_relative(path) || git_join(full_path, sizeof(full_path), repo->work_tree, path) != 0) {
        return -1;
    }
    (void)git_read_file(full_path, &old_data, &old_size);
    rt_memset(&out, 0, sizeof(out));
    while (*pos_io < patch_size) {
        size_t line_start = *pos_io;
        size_t line_end = git_patch_line_end(patch, patch_size, line_start);
        size_t line_size = line_end - line_start;
        size_t hunk_old_start;

        if (line_size >= 10U && memcmp(patch + line_start, "diff --git", 10U) == 0) {
            break;
        }
        if (line_size < 3U || memcmp(patch + line_start, "@@ ", 3U) != 0) {
            *pos_io = line_end;
            continue;
        }
        if (git_parse_hunk_start(patch + line_start, line_size, &hunk_old_start) != 0 || git_copy_old_lines_until(&out, old_data, old_size, &old_pos, &old_line, hunk_old_start) != 0) {
            goto done;
        }
        *pos_io = line_end;
        while (*pos_io < patch_size) {
            size_t hline_start = *pos_io;
            size_t hline_end = git_patch_line_end(patch, patch_size, hline_start);
            size_t hline_size = hline_end - hline_start;
            unsigned char prefix;

            if ((hline_size >= 3U && memcmp(patch + hline_start, "@@ ", 3U) == 0) || (hline_size >= 10U && memcmp(patch + hline_start, "diff --git", 10U) == 0)) {
                break;
            }
            if (hline_size == 0U) {
                *pos_io = hline_end;
                continue;
            }
            prefix = patch[hline_start];
            if (prefix == ' ' || prefix == '-') {
                size_t old_end = git_file_line_end(old_data, old_size, old_pos);
                size_t old_line_size = old_end - old_pos;
                size_t patch_line_size = hline_size - 1U;

                if (old_pos >= old_size || old_line_size != patch_line_size || memcmp(old_data + old_pos, patch + hline_start + 1U, patch_line_size) != 0) {
                    goto done;
                }
                old_pos = old_end;
                old_line += 1U;
            }
            if (prefix == ' ' || prefix == '+') {
                if (git_buffer_append(&out, patch + hline_start + 1U, hline_size - 1U) != 0) {
                    goto done;
                }
            } else if (prefix != '-' && prefix != '\\') {
                goto done;
            }
            *pos_io = hline_end;
        }
    }
    if (!delete_file && old_pos < old_size && git_buffer_append(&out, old_data + old_pos, old_size - old_pos) != 0) {
        goto done;
    }
    if (check_only) {
        result = 0;
        goto done;
    }
    if (delete_file) {
        (void)platform_remove_file(full_path);
        (void)git_remove_empty_checkout_parents(repo, path);
    } else if (git_ensure_parent_directory(full_path) != 0 || git_write_all_file(full_path, out.data, out.size, 0644U) != 0) {
        goto done;
    }
    result = 0;
done:
    git_buffer_destroy(&out);
    rt_free(old_data);
    return result;
}

static int git_patch_path_from_line(const unsigned char *line, size_t line_size, char *path, size_t path_size, int *is_null) {
    size_t start = 0U;
    size_t end = line_size;

    *is_null = 0;
    while (end > start && (line[end - 1U] == '\n' || line[end - 1U] == '\r')) end -= 1U;
    if (end >= 9U && memcmp(line, "--- ", 4U) == 0) {
        start = 4U;
    } else if (end >= 9U && memcmp(line, "+++ ", 4U) == 0) {
        start = 4U;
    } else {
        return -1;
    }
    while (start < end && line[start] == ' ') start += 1U;
    if (end - start == 9U && memcmp(line + start, "/dev/null", 9U) == 0) {
        path[0] = '\0';
        *is_null = 1;
        return 0;
    }
    if (end > start + 2U && (line[start] == 'a' || line[start] == 'b') && line[start + 1U] == '/') {
        start += 2U;
    }
    if (end <= start || end - start >= path_size) {
        return -1;
    }
    memcpy(path, line + start, end - start);
    path[end - start] = '\0';
    return 0;
}

static int git_cmd_apply(GitRepo *repo, int argc, char **argv, int argi) {
    unsigned char *patch = 0;
    size_t patch_size = 0U;
    size_t pos = 0U;
    const char *patch_path = 0;
    int check_only = 0;
    int result = 1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--check") == 0) {
            check_only = 1;
        } else {
            tool_write_error("git", "unsupported apply option: ", argv[argi]);
            return 1;
        }
        argi += 1;
    }
    if (argi < argc) {
        patch_path = argv[argi++];
    }
    if (argi < argc) {
        tool_write_error("git", "too many apply arguments", 0);
        return 1;
    }
    if (git_read_file(patch_path, &patch, &patch_size) != 0) {
        tool_write_error("git", "cannot read patch", patch_path);
        return 1;
    }
    while (pos < patch_size) {
        size_t line_start = pos;
        size_t line_end = git_patch_line_end(patch, patch_size, line_start);
        char old_path[GIT_PATH_CAPACITY];
        char new_path[GIT_PATH_CAPACITY];
        int old_null = 0;
        int new_null = 0;

        if (line_end - line_start < 10U || memcmp(patch + line_start, "diff --git", 10U) != 0) {
            pos = line_end;
            continue;
        }
        pos = line_end;
        old_path[0] = '\0';
        new_path[0] = '\0';
        while (pos < patch_size) {
            size_t meta_start = pos;
            size_t meta_end = git_patch_line_end(patch, patch_size, meta_start);
            size_t meta_size = meta_end - meta_start;

            if (meta_size >= 4U && memcmp(patch + meta_start, "--- ", 4U) == 0) {
                if (git_patch_path_from_line(patch + meta_start, meta_size, old_path, sizeof(old_path), &old_null) != 0) goto done;
                pos = meta_end;
                continue;
            }
            if (meta_size >= 4U && memcmp(patch + meta_start, "+++ ", 4U) == 0) {
                if (git_patch_path_from_line(patch + meta_start, meta_size, new_path, sizeof(new_path), &new_null) != 0) goto done;
                pos = meta_end;
                break;
            }
            if (meta_size >= 10U && memcmp(patch + meta_start, "diff --git", 10U) == 0) {
                break;
            }
            pos = meta_end;
        }
        if (new_null && old_path[0] != '\0') {
            if (git_apply_one_file(repo, old_path, patch, patch_size, &pos, 1, check_only) != 0) goto done;
        } else if (new_path[0] != '\0') {
            if (git_apply_one_file(repo, new_path, patch, patch_size, &pos, 0, check_only) != 0) goto done;
        }
    }
    result = 0;
done:
    rt_free(patch);
    if (result != 0) {
        tool_write_error("git", "apply failed", 0);
    }
    return result;
}

static int git_write_index_mode(unsigned int mode) {
    char digits[7];
    int pos;

    for (pos = 5; pos >= 0; --pos) {
        digits[pos] = (char)('0' + (mode & 7U));
        mode >>= 3U;
    }
    digits[6] = '\0';
    return rt_write_cstr(1, digits);
}

static int git_ls_files_write_record(const GitIndexEntry *entry, const char *path, int stage_mode, int nul_terminate) {
    if (stage_mode) {
        char hex[GIT_OBJECT_HEX_SIZE + 1U];

        if (entry == 0) {
            return -1;
        }
        git_format_oid_hex(entry->oid, hex);
        if (git_write_index_mode(entry->mode) != 0 || rt_write_char(1, ' ') != 0 || rt_write_cstr(1, hex) != 0 || rt_write_cstr(1, " 0\t") != 0 || rt_write_cstr(1, path) != 0 || git_write_record_terminator(nul_terminate) != 0) {
            return -1;
        }
        return 0;
    }
    return rt_write_cstr(1, path) != 0 || git_write_record_terminator(nul_terminate) != 0 ? -1 : 0;
}

typedef struct {
    int nul_terminate;
} GitLsFilesUntrackedContext;

static int git_ls_files_write_path(const char *path, void *user_data) {
    GitLsFilesUntrackedContext *context = (GitLsFilesUntrackedContext *)user_data;

    return git_ls_files_write_record(0, path, 0, context->nul_terminate);
}

static int git_cmd_ls_files(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    GitIgnoreList ignores;
    int show_cached = 0;
    int show_others = 0;
    int show_modified = 0;
    int show_deleted = 0;
    int show_stage = 0;
    int nul_terminate = 0;
    int saw_selector = 0;
    int exclude_standard = 1;
    int pathspec_start;
    int pathspec_count;
    int result = 1;
    size_t i;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--cached") == 0) {
            show_cached = 1;
            saw_selector = 1;
        } else if (rt_strcmp(argv[argi], "--others") == 0 || rt_strcmp(argv[argi], "-o") == 0) {
            show_others = 1;
            saw_selector = 1;
        } else if (rt_strcmp(argv[argi], "--modified") == 0 || rt_strcmp(argv[argi], "-m") == 0) {
            show_modified = 1;
            saw_selector = 1;
        } else if (rt_strcmp(argv[argi], "--deleted") == 0 || rt_strcmp(argv[argi], "-d") == 0) {
            show_deleted = 1;
            saw_selector = 1;
        } else if (rt_strcmp(argv[argi], "--stage") == 0 || rt_strcmp(argv[argi], "-s") == 0) {
            show_stage = 1;
        } else if (rt_strcmp(argv[argi], "-z") == 0) {
            nul_terminate = 1;
        } else if (rt_strcmp(argv[argi], "--exclude-standard") == 0) {
            exclude_standard = 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_error("git", "unsupported ls-files option: ", argv[argi]);
            return 1;
        } else {
            break;
        }
        argi += 1;
    }
    if (!saw_selector) {
        show_cached = 1;
    }
    pathspec_start = argi;
    pathspec_count = argc - argi;
    if (git_load_index(repo, &index) != 0) {
        tool_write_error("git", "cannot read index", 0);
        return 1;
    }
    if (index.count > 1U && !git_index_is_sorted(&index)) {
        rt_sort(index.entries, index.count, sizeof(index.entries[0]), git_compare_entries_by_path);
    }
    if (show_cached) {
        for (i = 0U; i < index.count; ++i) {
            if (git_pathspec_matches(index.entries[i].path, argv + pathspec_start, pathspec_count) && git_ls_files_write_record(&index.entries[i], index.entries[i].path, show_stage, nul_terminate) != 0) {
                goto done;
            }
        }
    }
    if (show_modified || show_deleted) {
        for (i = 0U; i < index.count; ++i) {
            int modified;

            if (!git_pathspec_matches(index.entries[i].path, argv + pathspec_start, pathspec_count)) {
                continue;
            }
            modified = git_entry_is_modified(repo, &index.entries[i]);
            if ((show_modified && modified > 0) || (show_deleted && modified < 0)) {
                if (git_ls_files_write_record(&index.entries[i], index.entries[i].path, show_stage, nul_terminate) != 0) {
                    goto done;
                }
            }
        }
    }
    if (show_stage && !show_cached && !show_modified && !show_deleted) {
        for (i = 0U; i < index.count; ++i) {
            if (git_pathspec_matches(index.entries[i].path, argv + pathspec_start, pathspec_count) && git_ls_files_write_record(&index.entries[i], index.entries[i].path, 1, nul_terminate) != 0) {
                goto done;
            }
        }
    }
    if (show_others) {
        GitIgnoreList *ignore_ptr = 0;
        GitLsFilesUntrackedContext untracked_context;

        rt_memset(&ignores, 0, sizeof(ignores));
        if (exclude_standard) {
            if (git_ignore_load(repo, &ignores) != 0) {
                tool_write_error("git", "cannot read ignore files", 0);
                goto done;
            }
            ignore_ptr = &ignores;
        }
        untracked_context.nul_terminate = nul_terminate;
        if (git_for_each_untracked_path(repo, &index, ignore_ptr, argv + pathspec_start, pathspec_count, git_ls_files_write_path, &untracked_context) != 0) {
            if (exclude_standard) {
                git_ignore_destroy(&ignores);
            }
            goto done;
        }
        if (exclude_standard) {
            git_ignore_destroy(&ignores);
        }
    }
    result = 0;
done:
    git_index_destroy(&index);
    return result;
}

typedef struct {
    const GitRepo *repo;
    GitIndex *index;
    const GitIgnoreList *ignores;
    size_t added;
} GitAddContext;

static void git_empty_blob_oid(unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    static const unsigned char empty_blob[CRYPTO_SHA1_DIGEST_SIZE] = {
        0xe6U, 0x9dU, 0xe2U, 0x9bU, 0xb2U, 0xd1U, 0xd6U, 0x43U, 0x4bU, 0x8bU,
        0x29U, 0xaeU, 0x77U, 0x5aU, 0xd8U, 0xc2U, 0xe4U, 0x8cU, 0x53U, 0x91U
    };

    memcpy(oid, empty_blob, sizeof(empty_blob));
}

static int git_add_intent_path(const char *path, void *user_data) {
    GitAddContext *context = (GitAddContext *)user_data;
    GitIndexEntry entry;
    PlatformDirEntry info;
    char full_path[GIT_PATH_CAPACITY];

    if (tool_path_is_unsafe_relative(path) || git_join(full_path, sizeof(full_path), context->repo->work_tree, path) != 0) {
        return -1;
    }
    if (platform_get_path_info(full_path, &info) != 0 || info.is_dir) {
        return -1;
    }
    rt_memset(&entry, 0, sizeof(entry));
    entry.path = git_strdup_n(path, rt_strlen(path));
    if (entry.path == 0) {
        return -1;
    }
    if ((info.mode & GIT_MODE_TYPE_MASK) == GIT_MODE_SYMLINK) {
        entry.mode = GIT_MODE_SYMLINK;
    } else {
        entry.mode = git_regular_index_mode_from_worktree(info.mode);
    }
    entry.intent_to_add = 1;
    entry.size = 0U;
    git_empty_blob_oid(entry.oid);
    if (git_index_push(context->index, &entry) != 0) {
        rt_free(entry.path);
        return -1;
    }
    context->added += 1U;
    return 0;
}

static int git_add_stage_blob_path(GitAddContext *context, const char *relative) {
    GitIndexEntry entry;
    PlatformDirEntry info;
    unsigned char *data = 0;
    size_t data_size = 0U;
    char full_path[GIT_PATH_CAPACITY];
    int result = -1;

    if (tool_path_is_unsafe_relative(relative) || git_join(full_path, sizeof(full_path), context->repo->work_tree, relative) != 0) {
        return -1;
    }
    if (platform_get_path_info(full_path, &info) != 0 || info.is_dir) {
        return -1;
    }
    rt_memset(&entry, 0, sizeof(entry));
    entry.path = git_strdup_n(relative, rt_strlen(relative));
    if (entry.path == 0) {
        return -1;
    }
    if ((info.mode & GIT_MODE_TYPE_MASK) == GIT_MODE_SYMLINK) {
        char target[GIT_PATH_CAPACITY];

        if (platform_read_symlink(full_path, target, sizeof(target)) != 0) {
            goto done;
        }
        data_size = rt_strlen(target);
        data = (unsigned char *)rt_malloc(data_size == 0U ? 1U : data_size);
        if (data == 0) {
            goto done;
        }
        memcpy(data, target, data_size);
        entry.mode = GIT_MODE_SYMLINK;
    } else {
        if (git_read_file(full_path, &data, &data_size) != 0) {
            goto done;
        }
        entry.mode = git_regular_index_mode_from_worktree(info.mode);
    }
    entry.size = data_size;
    entry.mtime_seconds = (unsigned long long)info.mtime;
    entry.mtime_nanos = info.mtime_nanos;
    if (git_write_loose_object(context->repo, GIT_OBJECT_BLOB, data, data_size, entry.oid) != 0 || git_index_replace_or_insert(context->index, &entry) != 0) {
        goto done;
    }
    context->added += 1U;
    result = 0;
done:
    if (result != 0) {
        rt_free(entry.path);
    }
    rt_free(data);
    return result;
}

static int git_add_stage_walk_callback(const char *path, const PlatformDirEntry *entry, int depth, ToolWalkControl *control, void *user_data) {
    GitAddContext *context = (GitAddContext *)user_data;
    char relative[GIT_PATH_CAPACITY];

    (void)depth;
    if (git_relative_path(context->repo, path, relative, sizeof(relative)) != 0 || relative[0] == '\0') {
        return 0;
    }
    if (rt_strcmp(relative, ".git") == 0 || rt_strncmp(relative, ".git/", 5U) == 0) {
        control->prune = 1;
        return 0;
    }
    if (context->ignores != 0 && git_ignore_matches(context->ignores, relative, entry->is_dir) && git_index_find(context->index, relative) == 0) {
        if (entry->is_dir) {
            control->prune = 1;
        }
        return 0;
    }
    if (entry->is_dir) {
        return 0;
    }
    return git_add_stage_blob_path(context, relative);
}

static int git_add_stage_one_path(GitAddContext *context, const char *arg) {
    char full_path[GIT_PATH_CAPACITY];
    char relative[GIT_PATH_CAPACITY];
    PlatformDirEntry info;

    if (git_is_absolute_path(arg)) {
        if (git_copy(full_path, sizeof(full_path), arg) != 0 || git_relative_path(context->repo, full_path, relative, sizeof(relative)) != 0) {
            return -1;
        }
    } else {
        if (git_join(full_path, sizeof(full_path), context->repo->work_tree, arg) != 0 || git_copy(relative, sizeof(relative), arg) != 0) {
            return -1;
        }
        while (relative[0] == '.' && relative[1] == '/') {
            memmove(relative, relative + 2, rt_strlen(relative + 2) + 1U);
        }
    }
    if (platform_get_path_info(full_path, &info) == 0) {
        if (info.is_dir) {
            ToolWalkOptions options;

            options.min_depth = 0;
            options.max_depth = -1;
            return tool_walk_path(full_path, &options, git_add_stage_walk_callback, context);
        }
        if (context->ignores != 0 && git_ignore_matches(context->ignores, relative, 0) && git_index_find(context->index, relative) == 0) {
            return 0;
        }
        return git_add_stage_blob_path(context, relative);
    }
    return 0;
}

static void git_add_stage_matching_deletions(GitAddContext *context, char **pathspecs, int pathspec_count) {
    size_t i = 0U;

    while (i < context->index->count) {
        char full_path[GIT_PATH_CAPACITY];
        PlatformDirEntry info;

        if (!git_pathspec_matches(context->index->entries[i].path, pathspecs, pathspec_count) || git_join(full_path, sizeof(full_path), context->repo->work_tree, context->index->entries[i].path) != 0 || platform_get_path_info(full_path, &info) == 0) {
            i += 1U;
            continue;
        }
        git_index_remove_at(context->index, i);
        context->added += 1U;
    }
}

static int git_cmd_add(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    GitIgnoreList ignores;
    GitAddContext context;
    int intent_to_add = 0;
    int patch_mode = 0;
    int pathspec_start;
    int pathspec_count;
    int result = 1;
    char *default_pathspec[1];

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-N") == 0 || rt_strcmp(argv[argi], "--intent-to-add") == 0) {
            intent_to_add = 1;
        } else if (rt_strcmp(argv[argi], "-p") == 0 || rt_strcmp(argv[argi], "--patch") == 0) {
            patch_mode = 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_error("git", "unsupported add option: ", argv[argi]);
            return 1;
        } else {
            break;
        }
        argi += 1;
    }
    default_pathspec[0] = ".";
    if (argi >= argc && !patch_mode) {
        tool_write_error("git", "add needs a path", 0);
        return 1;
    }
    pathspec_start = argi;
    pathspec_count = argc - argi;
    if (patch_mode && pathspec_count == 0) {
        pathspec_start = 0;
        pathspec_count = 1;
        argv = default_pathspec;
        argc = 1;
    }
    if (git_load_index(repo, &index) != 0) {
        tool_write_error("git", "cannot read index", 0);
        return 1;
    }
    if (index.count > 1U && !git_index_is_sorted(&index)) {
        rt_sort(index.entries, index.count, sizeof(index.entries[0]), git_compare_entries_by_path);
    }
    if (git_ignore_load(repo, &ignores) != 0) {
        git_index_destroy(&index);
        tool_write_error("git", "cannot read ignore files", 0);
        return 1;
    }
    context.repo = repo;
    context.index = &index;
    context.ignores = &ignores;
    context.added = 0U;
    if (intent_to_add) {
        if (git_for_each_untracked_path(repo, &index, &ignores, argv + pathspec_start, pathspec_count, git_add_intent_path, &context) != 0) {
            goto done;
        }
    } else {
        int i;

        for (i = pathspec_start; i < argc; ++i) {
            if (patch_mode && !tool_prompt_yes_no("stage ", argv[i])) {
                continue;
            }
            if (git_add_stage_one_path(&context, argv[i]) != 0) {
                tool_write_error("git", "cannot add path: ", argv[i]);
                goto done;
            }
        }
        git_add_stage_matching_deletions(&context, argv + pathspec_start, pathspec_count);
    }
    if (context.added == 0U) {
        result = 0;
        goto done;
    }
    if (git_write_index_file(repo, &index) != 0) {
        tool_write_error("git", "cannot write index", 0);
        goto done;
    }
    result = 0;
done:
    git_ignore_destroy(&ignores);
    git_index_destroy(&index);
    return result;
}

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

static int git_write_commit_from_tree(GitRepo *repo, const unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE], const char *message, unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitBuffer commit;
    unsigned char parent_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char tree_hex[GIT_OBJECT_HEX_SIZE + 1U];
    char parent_hex[GIT_OBJECT_HEX_SIZE + 1U];
    int have_parent = 0;
    int result = -1;

    rt_memset(&commit, 0, sizeof(commit));
    git_format_oid_hex(tree_oid, tree_hex);
    if (repo->head_oid[0] != '\0' && git_parse_oid_hex(repo->head_oid, parent_oid) == 0) {
        have_parent = 1;
        git_format_oid_hex(parent_oid, parent_hex);
    }
    if (git_buffer_append_cstr(&commit, "tree ") != 0 || git_buffer_append_cstr(&commit, tree_hex) != 0 || git_buffer_append_char(&commit, '\n') != 0) {
        goto done;
    }
    if (have_parent && (git_buffer_append_cstr(&commit, "parent ") != 0 || git_buffer_append_cstr(&commit, parent_hex) != 0 || git_buffer_append_char(&commit, '\n') != 0)) {
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

static const char *git_commit_subject(const GitCommitInfo *info) {
    char *message = info->message;

    if (message == 0) {
        return "";
    }
    while (*message == '\n' || *message == '\r') {
        message += 1;
    }
    return message;
}

static int git_write_commit_subject_line(const GitCommitInfo *info) {
    const char *subject = git_commit_subject(info);
    size_t i = 0U;

    while (subject[i] != '\0' && subject[i] != '\n' && subject[i] != '\r') {
        if (rt_write_char(1, subject[i]) != 0) {
            return -1;
        }
        i += 1U;
    }
    return rt_write_char(1, '\n');
}

static size_t git_commit_subject_length(const GitCommitInfo *info) {
    const char *subject = git_commit_subject(info);
    size_t length = 0U;

    while (subject[length] != '\0' && subject[length] != '\n' && subject[length] != '\r') {
        length += 1U;
    }
    return length;
}

static void git_author_name_email(const char *author, const char **name_out, size_t *name_len_out, const char **email_out, size_t *email_len_out) {
    const char *lt;
    const char *gt;
    size_t name_len;

    *name_out = "";
    *name_len_out = 0U;
    *email_out = "";
    *email_len_out = 0U;
    if (author == 0) {
        return;
    }
    lt = author;
    while (*lt != '\0' && *lt != '<') lt += 1;
    name_len = (size_t)(lt - author);
    while (name_len > 0U && (author[name_len - 1U] == ' ' || author[name_len - 1U] == '\t')) name_len -= 1U;
    *name_out = author;
    *name_len_out = name_len;
    if (*lt == '<') {
        gt = lt + 1;
        while (*gt != '\0' && *gt != '>') gt += 1;
        *email_out = lt + 1;
        *email_len_out = (size_t)(gt - (lt + 1));
    }
}

static int git_write_commit_format(const char *format, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const GitCommitInfo *info) {
    char hex[GIT_OBJECT_HEX_SIZE + 1U];
    size_t i = 0U;

    git_format_oid_hex(oid, hex);
    while (format[i] != '\0') {
        if (format[i] != '%') {
            if (rt_write_char(1, format[i]) != 0) return -1;
            i += 1U;
            continue;
        }
        i += 1U;
        if (format[i] == '\0') {
            return rt_write_char(1, '%');
        }
        if (format[i] == 'H') {
            if (rt_write_cstr(1, hex) != 0) return -1;
        } else if (format[i] == 'h') {
            if (rt_write_all(1, hex, 7U) != 0) return -1;
        } else if (format[i] == 's') {
            if (rt_write_all(1, git_commit_subject(info), git_commit_subject_length(info)) != 0) return -1;
        } else if (format[i] == 'T') {
            char tree_hex[GIT_OBJECT_HEX_SIZE + 1U];
            git_format_oid_hex(info->tree_oid, tree_hex);
            if (rt_write_cstr(1, tree_hex) != 0) return -1;
        } else if (format[i] == 'P') {
            size_t parent_index;
            for (parent_index = 0U; parent_index < info->parent_count; ++parent_index) {
                char parent_hex[GIT_OBJECT_HEX_SIZE + 1U];
                if (parent_index > 0U && rt_write_char(1, ' ') != 0) return -1;
                git_format_oid_hex(info->parents[parent_index], parent_hex);
                if (rt_write_cstr(1, parent_hex) != 0) return -1;
            }
        } else if (format[i] == 'a' && format[i + 1U] == 'n') {
            const char *name;
            const char *email;
            size_t name_len;
            size_t email_len;
            git_author_name_email(info->author, &name, &name_len, &email, &email_len);
            (void)email;
            (void)email_len;
            if (rt_write_all(1, name, name_len) != 0) return -1;
            i += 1U;
        } else if (format[i] == 'a' && format[i + 1U] == 'e') {
            const char *name;
            const char *email;
            size_t name_len;
            size_t email_len;
            git_author_name_email(info->author, &name, &name_len, &email, &email_len);
            (void)name;
            (void)name_len;
            if (rt_write_all(1, email, email_len) != 0) return -1;
            i += 1U;
        } else if (format[i] == '%') {
            if (rt_write_char(1, '%') != 0) return -1;
        } else if (format[i] == 'n') {
            if (rt_write_char(1, '\n') != 0) return -1;
        } else {
            if (rt_write_char(1, '%') != 0 || rt_write_char(1, format[i]) != 0) return -1;
        }
        i += 1U;
    }
    return rt_write_char(1, '\n');
}

typedef struct {
    unsigned char (*oids)[CRYPTO_SHA1_DIGEST_SIZE];
    size_t count;
    size_t capacity;
} GitOidList;

static void git_oid_list_destroy(GitOidList *list) {
    if (list == 0) {
        return;
    }
    rt_free(list->oids);
    rt_memset(list, 0, sizeof(*list));
}

static int git_oid_list_contains(const GitOidList *list, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    size_t i;

    for (i = 0U; i < list->count; ++i) {
        if (git_oid_equal(list->oids[i], oid)) {
            return 1;
        }
    }
    return 0;
}

static int git_oid_list_push(GitOidList *list, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    unsigned char (*new_oids)[CRYPTO_SHA1_DIGEST_SIZE];
    size_t new_capacity;

    if (list->count == list->capacity) {
        new_capacity = list->capacity == 0U ? 64U : list->capacity * 2U;
        new_oids = (unsigned char (*)[CRYPTO_SHA1_DIGEST_SIZE])rt_realloc_array(list->oids, new_capacity, sizeof(list->oids[0]));
        if (new_oids == 0) {
            return -1;
        }
        list->oids = new_oids;
        list->capacity = new_capacity;
    }
    memcpy(list->oids[list->count], oid, CRYPTO_SHA1_DIGEST_SIZE);
    list->count += 1U;
    return 0;
}

static int git_oid_list_push_unique(GitOidList *list, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    return git_oid_list_contains(list, oid) ? 0 : git_oid_list_push(list, oid);
}

static int git_collect_reachable_commits(GitRepo *repo, const unsigned char start[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack, GitOidList *reachable) {
    GitOidList stack;
    int result = -1;

    rt_memset(reachable, 0, sizeof(*reachable));
    rt_memset(&stack, 0, sizeof(stack));
    if (git_oid_list_push(&stack, start) != 0) {
        goto done;
    }
    while (stack.count > 0U) {
        unsigned char current[CRYPTO_SHA1_DIGEST_SIZE];
        GitCommitInfo info;
        size_t parent_index;

        memcpy(current, stack.oids[stack.count - 1U], CRYPTO_SHA1_DIGEST_SIZE);
        stack.count -= 1U;
        if (git_oid_list_contains(reachable, current)) {
            continue;
        }
        if (git_read_commit_info(repo, current, pack, &info) != 0) {
            goto done;
        }
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
    if (result != 0) {
        git_oid_list_destroy(reachable);
    }
    return result;
}

static int git_commit_is_ancestor_of(GitRepo *repo, const unsigned char ancestor[CRYPTO_SHA1_DIGEST_SIZE], const unsigned char descendant[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack) {
    GitOidList reachable;
    int result;

    if (git_oid_equal(ancestor, descendant)) {
        return 1;
    }
    if (git_collect_reachable_commits(repo, descendant, pack, &reachable) != 0) {
        return 0;
    }
    result = git_oid_list_contains(&reachable, ancestor);
    git_oid_list_destroy(&reachable);
    return result;
}

static int git_cmd_log(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    int have_pack;
    int oneline = 0;
    const char *format = 0;
    int max_count = 32;
    int count = 0;
    const char *start = "HEAD";
    GitOidList stack;
    GitOidList seen;
    int result = 0;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--oneline") == 0) {
            oneline = 1;
        } else if (rt_strncmp(argv[argi], "--format=", 9U) == 0) {
            format = argv[argi] + 9U;
            if (rt_strcmp(format, "oneline") == 0) {
                format = "%H %s";
            }
        } else if (rt_strcmp(argv[argi], "--format") == 0 && argi + 1 < argc) {
            format = argv[++argi];
            if (rt_strcmp(format, "oneline") == 0) {
                format = "%H %s";
            }
        } else if ((rt_strcmp(argv[argi], "-n") == 0 || rt_strcmp(argv[argi], "--max-count") == 0) && argi + 1 < argc) {
            long long value = 0;
            if (tool_parse_int_arg(argv[argi + 1], &value, "git", "log count") != 0 || value < 0) {
                return 1;
            }
            max_count = (int)value;
            argi += 1;
        } else if (rt_strncmp(argv[argi], "-", 1U) == 0 && argv[argi][1] >= '0' && argv[argi][1] <= '9') {
            long long value = 0;
            if (tool_parse_int_arg(argv[argi] + 1U, &value, "git", "log count") != 0 || value < 0) {
                return 1;
            }
            max_count = (int)value;
        } else if (rt_strncmp(argv[argi], "--max-count=", 12U) == 0) {
            long long value = 0;
            if (tool_parse_int_arg(argv[argi] + 12U, &value, "git", "log count") != 0 || value < 0) {
                return 1;
            }
            max_count = (int)value;
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_error("git", "unsupported log option: ", argv[argi]);
            return 1;
        } else {
            start = argv[argi];
        }
        argi += 1;
    }
    if (git_resolve_revision(repo, start, oid, 0, 0) != 0) {
        tool_write_error("git", "cannot resolve log revision: ", start);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    rt_memset(&stack, 0, sizeof(stack));
    rt_memset(&seen, 0, sizeof(seen));
    if (git_oid_list_push(&stack, oid) != 0) {
        if (have_pack) git_pack_destroy(&pack);
        return 1;
    }
    while (count < max_count && stack.count > 0U) {
        GitCommitInfo info;
        char hex[GIT_OBJECT_HEX_SIZE + 1U];
        size_t parent_index;

        memcpy(oid, stack.oids[stack.count - 1U], CRYPTO_SHA1_DIGEST_SIZE);
        stack.count -= 1U;
        if (git_oid_list_contains(&seen, oid)) {
            continue;
        }
        if (git_read_commit_info(repo, oid, have_pack ? &pack : 0, &info) != 0) {
            result = 1;
            break;
        }
        if (git_oid_list_push(&seen, oid) != 0) {
            git_commit_info_destroy(&info);
            result = 1;
            break;
        }
        git_format_oid_hex(oid, hex);
        if (format != 0) {
            if (git_write_commit_format(format, oid, &info) != 0) {
                git_commit_info_destroy(&info);
                result = 1;
                break;
            }
        } else if (oneline) {
            char short_hex[8];
            memcpy(short_hex, hex, 7U);
            short_hex[7] = '\0';
            rt_write_cstr(1, short_hex);
            rt_write_char(1, ' ');
            git_write_commit_subject_line(&info);
        } else {
            rt_write_cstr(1, "commit ");
            rt_write_line(1, hex);
            if (info.author != 0) {
                rt_write_cstr(1, "Author: ");
                rt_write_line(1, info.author);
            }
            rt_write_char(1, '\n');
            rt_write_cstr(1, "    ");
            git_write_commit_subject_line(&info);
            rt_write_char(1, '\n');
        }
        count += 1;
        for (parent_index = info.parent_count; parent_index > 0U; --parent_index) {
            if (git_oid_list_push_unique(&stack, info.parents[parent_index - 1U]) != 0) {
                git_commit_info_destroy(&info);
                result = 1;
                goto log_done;
            }
        }
        git_commit_info_destroy(&info);
    }
log_done:
    git_oid_list_destroy(&stack);
    git_oid_list_destroy(&seen);
    if (have_pack) {
        git_pack_destroy(&pack);
    }
    return result;
}

static int git_cmd_merge_base(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    unsigned char left[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char right[CRYPTO_SHA1_DIGEST_SIZE];
    int is_ancestor = 0;
    int have_pack;
    GitOidList left_reachable;
    GitOidList right_reachable;
    size_t i;

    if (argi < argc && rt_strcmp(argv[argi], "--is-ancestor") == 0) {
        is_ancestor = 1;
        argi += 1;
    }
    if (argi + 2 != argc || git_resolve_revision(repo, argv[argi], left, 0, 0) != 0 || git_resolve_revision(repo, argv[argi + 1], right, 0, 0) != 0) {
        tool_write_error("git", "merge-base needs two revisions", 0);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (is_ancestor) {
        int ok = git_commit_is_ancestor_of(repo, left, right, have_pack ? &pack : 0);
        if (have_pack) git_pack_destroy(&pack);
        return ok ? 0 : 1;
    }
    if (git_collect_reachable_commits(repo, left, have_pack ? &pack : 0, &left_reachable) != 0 || git_collect_reachable_commits(repo, right, have_pack ? &pack : 0, &right_reachable) != 0) {
        if (have_pack) git_pack_destroy(&pack);
        return 1;
    }
    for (i = 0U; i < right_reachable.count; ++i) {
        if (git_oid_list_contains(&left_reachable, right_reachable.oids[i])) {
            char hex[GIT_OBJECT_HEX_SIZE + 1U];
            git_format_oid_hex(right_reachable.oids[i], hex);
            rt_write_line(1, hex);
            git_oid_list_destroy(&left_reachable);
            git_oid_list_destroy(&right_reachable);
            if (have_pack) git_pack_destroy(&pack);
            return 0;
        }
    }
    git_oid_list_destroy(&left_reachable);
    git_oid_list_destroy(&right_reachable);
    if (have_pack) git_pack_destroy(&pack);
    return 1;
}

static int git_cmd_rev_list(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    char left_name[GIT_REF_CAPACITY];
    char right_name[GIT_REF_CAPACITY];
    unsigned char left[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char right[CRYPTO_SHA1_DIGEST_SIZE];
    int count_only = 0;
    int have_pack;
    unsigned long long count = 0ULL;
    char digits[32];
    GitOidList left_reachable;
    GitOidList right_reachable;
    size_t i;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--count") == 0) {
            count_only = 1;
        } else {
            tool_write_error("git", "unsupported rev-list option: ", argv[argi]);
            return 1;
        }
        argi += 1;
    }
    if (!count_only || argi + 1 != argc || git_split_revision_range(argv[argi], left_name, sizeof(left_name), right_name, sizeof(right_name)) != 0 || git_resolve_revision(repo, left_name, left, 0, 0) != 0 || git_resolve_revision(repo, right_name, right, 0, 0) != 0) {
        tool_write_error("git", "rev-list supports --count A..B", 0);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (git_collect_reachable_commits(repo, left, have_pack ? &pack : 0, &left_reachable) != 0 || git_collect_reachable_commits(repo, right, have_pack ? &pack : 0, &right_reachable) != 0) {
        if (have_pack) git_pack_destroy(&pack);
        return 1;
    }
    for (i = 0U; i < right_reachable.count; ++i) {
        if (!git_oid_list_contains(&left_reachable, right_reachable.oids[i])) {
            count += 1ULL;
        }
    }
    git_oid_list_destroy(&left_reachable);
    git_oid_list_destroy(&right_reachable);
    if (have_pack) git_pack_destroy(&pack);
    rt_unsigned_to_string(count, digits, sizeof(digits));
    rt_write_line(1, digits);
    return 0;
}

static int git_write_ref_format(const char *format, const char *ref_name, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    char hex[GIT_OBJECT_HEX_SIZE + 1U];
    size_t i = 0U;

    git_format_oid_hex(oid, hex);
    while (format[i] != '\0') {
        if (format[i] == '%' && format[i + 1U] == '(') {
            size_t start = i + 2U;
            size_t end = start;
            while (format[end] != '\0' && format[end] != ')') end += 1U;
            if (format[end] == ')') {
                size_t len = end - start;
                if (len == 7U && memcmp(format + start, "refname", 7U) == 0) {
                    if (rt_write_cstr(1, ref_name) != 0) return -1;
                } else if (len == 13U && memcmp(format + start, "refname:short", 13U) == 0) {
                    const char *short_name = ref_name;
                    if (rt_strncmp(ref_name, "refs/heads/", 11U) == 0) short_name = ref_name + 11U;
                    else if (rt_strncmp(ref_name, "refs/tags/", 10U) == 0) short_name = ref_name + 10U;
                    else if (rt_strncmp(ref_name, "refs/remotes/", 13U) == 0) short_name = ref_name + 13U;
                    if (rt_write_cstr(1, short_name) != 0) return -1;
                } else if (len == 10U && memcmp(format + start, "objectname", 10U) == 0) {
                    if (rt_write_cstr(1, hex) != 0) return -1;
                } else if (len == 16U && memcmp(format + start, "objectname:short", 16U) == 0) {
                    if (rt_write_all(1, hex, 7U) != 0) return -1;
                } else {
                    if (rt_write_all(1, format + i, end - i + 1U) != 0) return -1;
                }
                i = end + 1U;
                continue;
            }
        }
        if (rt_write_char(1, format[i]) != 0) return -1;
        i += 1U;
    }
    return rt_write_char(1, '\n');
}

static int git_for_each_ref_loose_dir(const GitRepo *repo, const char *relative, const char *format, const char *prefix, int *count) {
    char dir[GIT_PATH_CAPACITY];
    PlatformDirEntry entries[256];
    size_t entry_count = 0U;
    int is_directory = 0;
    size_t i;

    if (git_join(dir, sizeof(dir), repo->git_dir, relative) != 0 || platform_collect_entries(dir, 0, entries, sizeof(entries) / sizeof(entries[0]), &entry_count, &is_directory) != 0 || !is_directory) {
        return 0;
    }
    for (i = 0U; i < entry_count; ++i) {
        char child[GIT_REF_CAPACITY];
        char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];

        if (git_copy(child, sizeof(child), relative) != 0 || rt_strlen(child) + 1U + rt_strlen(entries[i].name) >= sizeof(child)) return -1;
        rt_copy_string(child + rt_strlen(child), sizeof(child) - rt_strlen(child), "/");
        rt_copy_string(child + rt_strlen(child), sizeof(child) - rt_strlen(child), entries[i].name);
        if (entries[i].is_dir) {
            if (git_for_each_ref_loose_dir(repo, child, format, prefix, count) != 0) return -1;
        } else if ((prefix == 0 || rt_strncmp(child, prefix, rt_strlen(prefix)) == 0) && git_read_ref_file(repo, child, oid_hex, sizeof(oid_hex)) == 0 && git_parse_oid_hex(oid_hex, oid) == 0) {
            if (git_write_ref_format(format, child, oid) != 0) return -1;
            *count += 1;
        }
    }
    return 0;
}

static int git_for_each_ref_packed(const GitRepo *repo, const char *format, const char *prefix, int *count) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;

    if (git_join(path, sizeof(path), repo->git_dir, "packed-refs") != 0 || git_read_file(path, &data, &size) != 0) return 0;
    while (pos < size) {
        size_t start = pos;
        size_t end;
        size_t line_len;
        while (pos < size && data[pos] != '\n') pos += 1U;
        end = pos;
        if (pos < size) pos += 1U;
        while (end > start && data[end - 1U] == '\r') end -= 1U;
        line_len = end - start;
        if (line_len > GIT_OBJECT_HEX_SIZE + 1U && data[start] != '#' && data[start] != '^' && data[start + GIT_OBJECT_HEX_SIZE] == ' ') {
            char ref_name[GIT_REF_CAPACITY];
            unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
            size_t ref_len = line_len - GIT_OBJECT_HEX_SIZE - 1U;
            if (ref_len < sizeof(ref_name)) {
                memcpy(ref_name, data + start + GIT_OBJECT_HEX_SIZE + 1U, ref_len);
                ref_name[ref_len] = '\0';
                if ((prefix == 0 || rt_strncmp(ref_name, prefix, rt_strlen(prefix)) == 0) && git_parse_oid_hex_n((const char *)data + start, GIT_OBJECT_HEX_SIZE, oid) == 0) {
                    if (git_write_ref_format(format, ref_name, oid) != 0) {
                        rt_free(data);
                        return -1;
                    }
                    *count += 1;
                }
            }
        }
    }
    rt_free(data);
    return 0;
}

static int git_cmd_for_each_ref(GitRepo *repo, int argc, char **argv, int argi) {
    const char *format = "%(objectname) %(refname)";
    const char *prefix = "refs";
    int count = 0;

    while (argi < argc) {
        if (rt_strncmp(argv[argi], "--format=", 9U) == 0) {
            format = argv[argi] + 9U;
        } else if (rt_strcmp(argv[argi], "--format") == 0 && argi + 1 < argc) {
            format = argv[++argi];
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_error("git", "unsupported for-each-ref option: ", argv[argi]);
            return 1;
        } else {
            prefix = argv[argi];
        }
        argi += 1;
    }
    if (git_for_each_ref_loose_dir(repo, "refs", format, prefix, &count) != 0 || git_for_each_ref_packed(repo, format, prefix, &count) != 0) {
        return 1;
    }
    (void)count;
    return 0;
}

static int git_cmd_symbolic_ref(GitRepo *repo, int argc, char **argv, int argi) {
    int short_output = 0;
    const char *name;

    if (argi < argc && rt_strcmp(argv[argi], "--short") == 0) {
        short_output = 1;
        argi += 1;
    }
    if (argi >= argc || argi + 2 < argc) {
        tool_write_error("git", "symbolic-ref needs NAME [REF]", 0);
        return 1;
    }
    name = argv[argi++];
    if (rt_strcmp(name, "HEAD") != 0) {
        tool_write_error("git", "symbolic-ref currently supports HEAD", 0);
        return 1;
    }
    if (argi < argc) {
        if (rt_strncmp(argv[argi], "refs/", 5U) != 0 || tool_path_is_unsafe_relative(argv[argi]) || git_write_head_ref(repo, argv[argi]) != 0) {
            tool_write_error("git", "cannot write symbolic ref: ", argv[argi]);
            return 1;
        }
        return 0;
    }
    if (repo->head_ref[0] == '\0') {
        return 1;
    }
    if (short_output && rt_strncmp(repo->head_ref, "refs/heads/", 11U) == 0) {
        rt_write_line(1, repo->head_ref + 11U);
    } else {
        rt_write_line(1, repo->head_ref);
    }
    return 0;
}

static int git_cmd_update_ref(GitRepo *repo, int argc, char **argv, int argi) {
    if (argi < argc && rt_strcmp(argv[argi], "-d") == 0) {
        char path[GIT_PATH_CAPACITY];
        argi += 1;
        if (argi + 1 != argc || rt_strncmp(argv[argi], "refs/", 5U) != 0 || tool_path_is_unsafe_relative(argv[argi]) || git_join(path, sizeof(path), repo->git_dir, argv[argi]) != 0) {
            tool_write_error("git", "update-ref -d needs a ref", 0);
            return 1;
        }
        return platform_remove_file(path) == 0 ? 0 : 1;
    }
    if (argi + 2 != argc || rt_strncmp(argv[argi], "refs/", 5U) != 0 || tool_path_is_unsafe_relative(argv[argi])) {
        tool_write_error("git", "update-ref needs REF NEW", 0);
        return 1;
    }
    {
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
        if (git_resolve_revision(repo, argv[argi + 1], oid, 0, 0) != 0 && (rt_strlen(argv[argi + 1]) != GIT_OBJECT_HEX_SIZE || git_parse_oid_hex(argv[argi + 1], oid) != 0)) {
            tool_write_error("git", "cannot resolve update-ref object: ", argv[argi + 1]);
            return 1;
        }
        if (git_write_ref_oid_reflog(repo, argv[argi], oid, "update-ref", rt_strcmp(repo->head_ref, argv[argi]) == 0) != 0) {
            tool_write_error("git", "cannot write ref: ", argv[argi]);
            return 1;
        }
    }
    return 0;
}

static int git_cmd_show(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    GitCommitInfo info;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    char hex[GIT_OBJECT_HEX_SIZE + 1U];
    int have_pack;
    int stat_mode = 0;
    const char *revision = "HEAD";
    int result = 1;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--stat") == 0) {
            stat_mode = 1;
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_error("git", "unsupported show option: ", argv[argi]);
            return 1;
        } else {
            revision = argv[argi];
        }
        argi += 1;
    }
    if (git_resolve_revision(repo, revision, oid, 0, 0) != 0) {
        tool_write_error("git", "cannot resolve show revision: ", revision);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (git_read_commit_info(repo, oid, have_pack ? &pack : 0, &info) != 0) {
        tool_write_error("git", "cannot read commit: ", revision);
        goto done;
    }
    git_format_oid_hex(oid, hex);
    rt_write_cstr(1, "commit ");
    rt_write_line(1, hex);
    if (info.author != 0) {
        rt_write_cstr(1, "Author: ");
        rt_write_line(1, info.author);
    }
    rt_write_char(1, '\n');
    rt_write_cstr(1, "    ");
    git_write_commit_subject_line(&info);
    rt_write_char(1, '\n');
    if (info.has_parent) {
        GitIndex old_index;
        GitIndex new_index;
        GitDiffStatList stats;

        rt_memset(&stats, 0, sizeof(stats));
        if (git_load_commit_tree_index(repo, info.parent_oid, have_pack ? &pack : 0, &old_index) != 0) {
            git_diff_stat_list_destroy(&stats);
            git_commit_info_destroy(&info);
            goto done;
        }
        if (git_load_commit_tree_index(repo, oid, have_pack ? &pack : 0, &new_index) != 0) {
            git_diff_stat_list_destroy(&stats);
            git_index_destroy(&old_index);
            git_commit_info_destroy(&info);
            goto done;
        }
        int change_count = 0;
        if (git_diff_index_pair(repo, &old_index, &new_index, have_pack ? &pack : 0, stat_mode ? GIT_DIFF_OUTPUT_STAT : GIT_DIFF_OUTPUT_PATCH, TOOL_COLOR_AUTO, 0, 0, 0, &stats, &change_count) == 0) {
            if (stat_mode) {
                (void)git_render_diff_stat(&stats, TOOL_COLOR_AUTO);
            }
            result = 0;
        }
        git_diff_stat_list_destroy(&stats);
        git_index_destroy(&old_index);
        git_index_destroy(&new_index);
    } else {
        result = 0;
    }
    git_commit_info_destroy(&info);
done:
    if (have_pack) {
        git_pack_destroy(&pack);
    }
    return result;
}

static int git_cmd_blame(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    GitCommitInfo info;
    GitIndex tree_index;
    GitIndexEntry *entry;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char *blob = 0;
    size_t blob_size = 0U;
    GitDiffLine *lines = 0;
    size_t line_count = 0U;
    unsigned char (*line_oids)[CRYPTO_SHA1_DIGEST_SIZE] = 0;
    char **line_authors = 0;
    unsigned long long line_number = 1ULL;
    int type = 0;
    int have_pack;
    const char *revision = "HEAD";
    const char *path = 0;
    int result = 1;
    char short_hex[8];
    size_t line_index;

    rt_memset(&info, 0, sizeof(info));
    rt_memset(&tree_index, 0, sizeof(tree_index));

    if (argi < argc && rt_strcmp(argv[argi], "--") != 0 && argi + 1 < argc) {
        revision = argv[argi++];
    }
    if (argi < argc && rt_strcmp(argv[argi], "--") == 0) {
        argi += 1;
    }
    if (argi + 1 != argc) {
        tool_write_error("git", "blame needs a path", 0);
        return 1;
    }
    path = argv[argi];
    if (git_resolve_revision(repo, revision, oid, 0, 0) != 0) {
        tool_write_error("git", "cannot resolve blame revision: ", revision);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (git_read_commit_info(repo, oid, have_pack ? &pack : 0, &info) != 0 || git_load_commit_tree_index(repo, oid, have_pack ? &pack : 0, &tree_index) != 0) {
        goto done;
    }
    entry = git_index_find(&tree_index, path);
    if (entry == 0 || git_read_object(repo, entry->oid, have_pack ? &pack : 0, &type, &blob, &blob_size) != 0 || type != GIT_OBJECT_BLOB) {
        tool_write_error("git", "cannot blame path: ", path);
        goto done;
    }
    if (git_split_diff_lines(blob, blob_size, &lines, &line_count) != 0) {
        goto done;
    }
    if (line_count > 0U) {
        line_oids = (unsigned char (*)[CRYPTO_SHA1_DIGEST_SIZE])rt_malloc_array(line_count, sizeof(line_oids[0]));
        line_authors = (char **)rt_malloc_array(line_count, sizeof(line_authors[0]));
        if (line_oids == 0 || line_authors == 0) {
            goto done;
        }
        rt_memset(line_authors, 0, line_count * sizeof(line_authors[0]));
    }
    for (line_index = 0U; line_index < line_count; ++line_index) {
        memcpy(line_oids[line_index], oid, CRYPTO_SHA1_DIGEST_SIZE);
        line_authors[line_index] = git_strdup_n(info.author != 0 ? info.author : "unknown", rt_strlen(info.author != 0 ? info.author : "unknown"));
        if (line_authors[line_index] == 0) {
            goto done;
        }
    }
    {
        unsigned char current_oid[CRYPTO_SHA1_DIGEST_SIZE];
        GitCommitInfo current_info;

        memcpy(current_oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
        rt_memset(&current_info, 0, sizeof(current_info));
        while (git_read_commit_info(repo, current_oid, have_pack ? &pack : 0, &current_info) == 0 && current_info.parent_count == 1U) {
            GitCommitInfo parent_info;
            GitIndex parent_index;
            GitIndexEntry *parent_entry;
            unsigned char *parent_blob = 0;
            size_t parent_blob_size = 0U;
            GitDiffLine *parent_lines = 0;
            size_t parent_line_count = 0U;
            int parent_type = 0;

            rt_memset(&parent_info, 0, sizeof(parent_info));
            rt_memset(&parent_index, 0, sizeof(parent_index));
            if (git_read_commit_info(repo, current_info.parents[0], have_pack ? &pack : 0, &parent_info) != 0 || git_load_commit_tree_index(repo, current_info.parents[0], have_pack ? &pack : 0, &parent_index) != 0) {
                git_commit_info_destroy(&parent_info);
                git_index_destroy(&parent_index);
                git_commit_info_destroy(&current_info);
                break;
            }
            parent_entry = git_index_find(&parent_index, path);
            if (parent_entry == 0 || git_read_object(repo, parent_entry->oid, have_pack ? &pack : 0, &parent_type, &parent_blob, &parent_blob_size) != 0 || parent_type != GIT_OBJECT_BLOB || git_split_diff_lines(parent_blob, parent_blob_size, &parent_lines, &parent_line_count) != 0) {
                rt_free(parent_blob);
                rt_free(parent_lines);
                git_commit_info_destroy(&parent_info);
                git_index_destroy(&parent_index);
                git_commit_info_destroy(&current_info);
                break;
            }
            for (line_index = 0U; line_index < line_count && line_index < parent_line_count; ++line_index) {
                if (git_oid_equal(line_oids[line_index], current_oid) && git_diff_lines_equal(&lines[line_index], &parent_lines[line_index])) {
                    char *author_copy = git_strdup_n(parent_info.author != 0 ? parent_info.author : "unknown", rt_strlen(parent_info.author != 0 ? parent_info.author : "unknown"));
                    if (author_copy == 0) {
                        rt_free(parent_blob);
                        rt_free(parent_lines);
                        git_commit_info_destroy(&parent_info);
                        git_index_destroy(&parent_index);
                        git_commit_info_destroy(&current_info);
                        goto done;
                    }
                    memcpy(line_oids[line_index], current_info.parents[0], CRYPTO_SHA1_DIGEST_SIZE);
                    rt_free(line_authors[line_index]);
                    line_authors[line_index] = author_copy;
                }
            }
            memcpy(current_oid, current_info.parents[0], CRYPTO_SHA1_DIGEST_SIZE);
            rt_free(parent_blob);
            rt_free(parent_lines);
            git_commit_info_destroy(&parent_info);
            git_index_destroy(&parent_index);
            git_commit_info_destroy(&current_info);
            rt_memset(&current_info, 0, sizeof(current_info));
        }
        git_commit_info_destroy(&current_info);
    }
    for (line_index = 0U; line_index < line_count; ++line_index) {
        char number[32];
        char hex[GIT_OBJECT_HEX_SIZE + 1U];

        git_format_oid_hex(line_oids[line_index], hex);
        memcpy(short_hex, hex, 7U);
        short_hex[7] = '\0';
        rt_write_cstr(1, short_hex);
        rt_write_cstr(1, " (");
        rt_write_cstr(1, line_authors[line_index] != 0 ? line_authors[line_index] : "unknown");
        rt_write_cstr(1, " ");
        rt_unsigned_to_string(line_number, number, sizeof(number));
        rt_write_cstr(1, number);
        rt_write_cstr(1, ") ");
        rt_write_all(1, lines[line_index].data, lines[line_index].length);
        rt_write_char(1, '\n');
        line_number += 1ULL;
    }
    result = 0;
done:
    if (line_authors != 0) {
        for (line_index = 0U; line_index < line_count; ++line_index) {
            rt_free(line_authors[line_index]);
        }
    }
    rt_free(line_authors);
    rt_free(line_oids);
    rt_free(lines);
    rt_free(blob);
    git_index_destroy(&tree_index);
    git_commit_info_destroy(&info);
    if (have_pack) git_pack_destroy(&pack);
    return result;
}

static int git_checkout_index_paths_to_worktree(const GitRepo *repo, const GitIndex *index, char **pathspecs, int pathspec_count, const GitPack *pack_cache) {
    size_t i;

    for (i = 0U; i < index->count; ++i) {
        if (git_pathspec_matches(index->entries[i].path, pathspecs, pathspec_count) && git_write_index_entry_to_worktree(repo, &index->entries[i], pack_cache) != 0) {
            return -1;
        }
    }
    return 0;
}

static int git_cmd_reset(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
    const char *revision = "HEAD";
    int hard = 0;
    int mixed = 1;
    int have_pack;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--hard") == 0) {
            hard = 1;
            mixed = 0;
        } else if (rt_strcmp(argv[argi], "--mixed") == 0) {
            mixed = 1;
            hard = 0;
        } else if (rt_strcmp(argv[argi], "--soft") == 0) {
            mixed = 0;
            hard = 0;
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_error("git", "unsupported reset option: ", argv[argi]);
            return 1;
        } else {
            revision = argv[argi];
        }
        argi += 1;
    }
    if (git_resolve_revision(repo, revision, oid, 0, 0) != 0) {
        tool_write_error("git", "cannot resolve reset revision: ", revision);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (hard) {
        if (git_checkout_commit_to_worktree(repo, oid, have_pack ? &pack : 0) != 0) {
            if (have_pack) git_pack_destroy(&pack);
            tool_write_error("git", "hard reset failed", 0);
            return 1;
        }
    } else if (mixed && git_write_index_from_commit(repo, oid, have_pack ? &pack : 0) != 0) {
        if (have_pack) git_pack_destroy(&pack);
        tool_write_error("git", "mixed reset failed", 0);
        return 1;
    }
    if (repo->head_ref[0] != '\0') {
        if (git_write_ref_oid_reflog(repo, repo->head_ref, oid, "reset", 1) != 0) {
            if (have_pack) git_pack_destroy(&pack);
            return 1;
        }
    } else if (git_write_detached_head_oid_reflog(repo, oid, "reset") != 0) {
        if (have_pack) git_pack_destroy(&pack);
        return 1;
    }
    git_format_oid_hex(oid, oid_hex);
    rt_write_cstr(1, "HEAD is now at ");
    rt_write_line(1, oid_hex);
    if (have_pack) git_pack_destroy(&pack);
    return 0;
}

static int git_cmd_restore(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    GitIndex source_index;
    GitPack pack;
    unsigned char source_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char **pathspecs;
    int pathspec_count;
    int staged = 0;
    int worktree = 1;
    const char *source = 0;
    int have_pack;
    int have_source_index = 0;
    int result = 1;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--staged") == 0) {
            staged = 1;
            worktree = 0;
        } else if (rt_strcmp(argv[argi], "--worktree") == 0) {
            worktree = 1;
        } else if (rt_strcmp(argv[argi], "--source") == 0 && argi + 1 < argc) {
            source = argv[++argi];
        } else if (rt_strncmp(argv[argi], "--source=", 9U) == 0) {
            source = argv[argi] + 9U;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_error("git", "unsupported restore option: ", argv[argi]);
            return 1;
        } else {
            break;
        }
        argi += 1;
    }
    if (argi >= argc) {
        tool_write_error("git", "restore needs a path", 0);
        return 1;
    }
    pathspecs = argv + argi;
    pathspec_count = argc - argi;
    if (git_load_index(repo, &index) != 0) {
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (source != 0) {
        if (git_resolve_revision(repo, source, source_oid, 0, 0) != 0 || git_load_commit_tree_index(repo, source_oid, have_pack ? &pack : 0, &source_index) != 0) {
            tool_write_error("git", "cannot read restore source: ", source);
            goto done;
        }
        have_source_index = 1;
    }
    if (staged) {
        GitIndex *source_ptr = have_source_index ? &source_index : 0;
        int i;

        if (source_ptr == 0 && git_load_head_tree_index(repo, have_pack ? &pack : 0, &source_index) == 0) {
            source_ptr = &source_index;
            have_source_index = 1;
        }
        if (source_ptr == 0) {
            tool_write_error("git", "cannot restore staged paths without HEAD", 0);
            goto done;
        }
        for (i = 0; i < pathspec_count; ++i) {
            size_t position = 0U;
            GitIndexEntry *source_entry = git_index_find(source_ptr, pathspecs[i]);
            if (source_entry == 0) {
                if (git_index_find_position(&index, pathspecs[i], &position) == 0) {
                    git_index_remove_at(&index, position);
                }
            } else {
                GitIndexEntry copy = *source_entry;
                copy.path = git_strdup_n(source_entry->path, rt_strlen(source_entry->path));
                if (copy.path == 0 || git_index_replace_or_insert(&index, &copy) != 0) {
                    rt_free(copy.path);
                    goto done;
                }
            }
        }
        if (git_write_index_file(repo, &index) != 0) {
            goto done;
        }
    }
    if (worktree) {
        GitIndex *source_ptr = have_source_index ? &source_index : &index;
        if (git_checkout_index_paths_to_worktree(repo, source_ptr, pathspecs, pathspec_count, have_pack ? &pack : 0) != 0) {
            tool_write_error("git", "cannot restore worktree paths", 0);
            goto done;
        }
    }
    result = 0;
done:
    if (have_source_index) git_index_destroy(&source_index);
    if (have_pack) git_pack_destroy(&pack);
    git_index_destroy(&index);
    return result;
}

static int git_cmd_rm(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    int cached = 0;
    int recursive = 0;
    int pathspec_start;
    int pathspec_count;
    size_t i = 0U;
    int removed = 0;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--cached") == 0) {
            cached = 1;
        } else if (rt_strcmp(argv[argi], "-r") == 0) {
            recursive = 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_error("git", "unsupported rm option: ", argv[argi]);
            return 1;
        } else {
            break;
        }
        argi += 1;
    }
    if (argi >= argc) {
        tool_write_error("git", "rm needs a path", 0);
        return 1;
    }
    pathspec_start = argi;
    pathspec_count = argc - argi;
    if (git_load_index(repo, &index) != 0) {
        return 1;
    }
    (void)recursive;
    while (i < index.count) {
        if (git_pathspec_matches(index.entries[i].path, argv + pathspec_start, pathspec_count)) {
            char full_path[GIT_PATH_CAPACITY];
            if (!cached && git_join(full_path, sizeof(full_path), repo->work_tree, index.entries[i].path) == 0) {
                (void)tool_remove_path(full_path, 1);
            }
            git_index_remove_at(&index, i);
            removed = 1;
            continue;
        }
        i += 1U;
    }
    if (removed && git_write_index_file(repo, &index) != 0) {
        git_index_destroy(&index);
        return 1;
    }
    git_index_destroy(&index);
    return 0;
}

typedef struct {
    const GitRepo *repo;
    int dry_run;
} GitCleanContext;

static int git_clean_remove_path(const char *path, void *user_data) {
    GitCleanContext *context = (GitCleanContext *)user_data;
    char full_path[GIT_PATH_CAPACITY];

    if (context->dry_run) {
        rt_write_cstr(1, "Would remove ");
        return rt_write_line(1, path);
    }
    rt_write_cstr(1, "Removing ");
    if (rt_write_line(1, path) != 0 || git_join(full_path, sizeof(full_path), context->repo->work_tree, path) != 0) {
        return -1;
    }
    (void)tool_remove_path(full_path, 1);
    (void)git_remove_empty_checkout_parents(context->repo, path);
    return 0;
}

static int git_cmd_clean(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    GitIgnoreList ignores;
    GitCleanContext context;
    int force = 0;
    int dry_run = 0;
    int exclude_standard = 1;
    int pathspec_start;
    int pathspec_count;
    int result = 1;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-f") == 0 || rt_strcmp(argv[argi], "--force") == 0) {
            force = 1;
        } else if (rt_strcmp(argv[argi], "-n") == 0 || rt_strcmp(argv[argi], "--dry-run") == 0) {
            dry_run = 1;
        } else if (rt_strcmp(argv[argi], "-x") == 0) {
            exclude_standard = 0;
        } else if (rt_strcmp(argv[argi], "--exclude-standard") == 0) {
            exclude_standard = 1;
        } else if (rt_strcmp(argv[argi], "-X") == 0) {
            tool_write_error("git", "clean -X is not implemented", 0);
            return 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else if (argv[argi][0] == '-' && argv[argi][1] != '\0') {
            tool_write_error("git", "unsupported clean option: ", argv[argi]);
            return 1;
        } else {
            break;
        }
        argi += 1;
    }
    if (!force && !dry_run) {
        tool_write_error("git", "clean requires -f or -n", 0);
        return 1;
    }
    pathspec_start = argi;
    pathspec_count = argc - argi;
    if (git_load_index(repo, &index) != 0) {
        return 1;
    }
    rt_memset(&ignores, 0, sizeof(ignores));
    if (exclude_standard && git_ignore_load(repo, &ignores) != 0) {
        git_index_destroy(&index);
        return 1;
    }
    context.repo = repo;
    context.dry_run = dry_run;
    if (git_for_each_untracked_path(repo, &index, exclude_standard ? &ignores : 0, argv + pathspec_start, pathspec_count, git_clean_remove_path, &context) != 0) {
        goto done;
    }
    result = 0;
done:
    if (exclude_standard) git_ignore_destroy(&ignores);
    git_index_destroy(&index);
    return result;
}

static int git_cmd_hash_object(int argc, char **argv, int argi) {
    int exit_code = 0;
    int write_object = 0;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-w") == 0) {
            write_object = 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            tool_write_error("git", "unsupported hash-object option: ", argv[argi]);
            return 1;
        }
        argi += 1;
    }
    if (write_object) {
        tool_write_error("git", "hash-object -w is not implemented", 0);
        return 1;
    }
    if (argi >= argc) {
        tool_write_error("git", "hash-object needs a file", 0);
        return 1;
    }
    while (argi < argc) {
        PlatformDirEntry info;
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
        char hex[GIT_OBJECT_HEX_SIZE + 1U];

        if (platform_get_path_info(argv[argi], &info) != 0 || info.is_dir || git_blob_hash_path(argv[argi], info.size, oid) != 0) {
            tool_write_error("git", "cannot hash file: ", argv[argi]);
            exit_code = 1;
        } else {
            git_format_oid_hex(oid, hex);
            rt_write_line(1, hex);
        }
        argi += 1;
    }
    return exit_code;
}

