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
               rt_strncmp(arg, "git://", 6U) == 0 || git_url_is_ssh(arg)) {
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
    const char *scp_colon = 0;
    size_t i;

    while (end > 0U && source[end - 1U] == '/') {
        end -= 1U;
    }
    start = end;
    while (start > 0U && source[start - 1U] != '/') {
        start -= 1U;
    }
    if (git_url_is_ssh(source)) {
        for (i = 0U; i < end && source[i] != '/'; ++i) {
            if (source[i] == ':') {
                scp_colon = source + i;
            }
        }
        if (scp_colon != 0 && (size_t)(scp_colon - source + 1) < end) {
            start = (size_t)(scp_colon - source + 1);
            for (i = start; i < end; ++i) {
                if (source[i] == '/') {
                    start = i + 1U;
                }
            }
        }
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

static int git_copy_tracked_files(const GitRepo *source_repo, GitIndex *index, const char *destination) {
    char last_parent[GIT_PATH_CAPACITY];
    size_t i;

    last_parent[0] = '\0';
    for (i = 0U; i < index->count; ++i) {
        char source_path[GIT_PATH_CAPACITY];
        char dest_path[GIT_PATH_CAPACITY];
        char parent[GIT_PATH_CAPACITY];
        PlatformDirEntry info;
        PlatformDirEntry dest_info;
        unsigned int checkout_mode;

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
        if (git_copy(parent, sizeof(parent), dest_path) != 0 || git_path_parent(parent) != 0) {
            return -1;
        }
        if (rt_strcmp(parent, last_parent) != 0) {
            if (git_make_directory_chain(parent) != 0 || git_copy(last_parent, sizeof(last_parent), parent) != 0) {
                tool_write_error("git", "cannot create checkout directory: ", parent);
                return -1;
            }
        }
        if (tool_copy_file(source_path, dest_path) != 0) {
            tool_write_error("git", "cannot check out file: ", index->entries[i].path);
            return -1;
        }
        checkout_mode = git_worktree_mode_from_regular_index(index->entries[i].mode);
        if (checkout_mode != 0644U) {
            (void)platform_change_mode(dest_path, checkout_mode);
        }
        if (platform_get_path_info_quick(dest_path, &dest_info) == 0) {
            index->entries[i].mtime_seconds = (unsigned long long)dest_info.mtime;
            index->entries[i].mtime_nanos = dest_info.mtime_nanos;
            index->entries[i].size = dest_info.size;
            index->entries[i].has_worktree_info = 0;
        }
    }
    return 0;
}

static int git_copy_local_objects_hardlink(const char *source_path, const char *dest_path) {
    PlatformDirEntry source_info;

    if (platform_get_path_info(source_path, &source_info) != 0) {
        return -1;
    }
    if (!source_info.is_dir) {
        if (platform_create_hard_link(source_path, dest_path) == 0) {
            return 0;
        }
        return tool_copy_path(source_path, dest_path, 0, 1, 1);
    }

    {
        enum { GIT_LOCAL_COPY_ENTRY_CAPACITY = 1024 };
        PlatformDirEntry entries[GIT_LOCAL_COPY_ENTRY_CAPACITY];
        size_t count = 0U;
        size_t i;
        int is_directory = 0;
        int result = -1;

        if (platform_path_is_directory(dest_path, &is_directory) != 0) {
            if (platform_make_directory(dest_path, source_info.mode & 07777U) != 0) {
                return -1;
            }
        } else if (!is_directory) {
            return -1;
        }
        if (platform_collect_entries(source_path, 1, entries, GIT_LOCAL_COPY_ENTRY_CAPACITY, &count, &is_directory) != 0 || !is_directory) {
            return -1;
        }
        for (i = 0U; i < count; ++i) {
            char child_source[GIT_PATH_CAPACITY];
            char child_dest[GIT_PATH_CAPACITY];

            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
                continue;
            }
            if (git_join(child_source, sizeof(child_source), source_path, entries[i].name) != 0 ||
                git_join(child_dest, sizeof(child_dest), dest_path, entries[i].name) != 0 ||
                git_copy_local_objects_hardlink(child_source, child_dest) != 0) {
                goto done;
            }
        }
        result = 0;
done:
        platform_free_entries(entries, count);
        if (result == 0) {
            (void)platform_change_mode(dest_path, source_info.mode & 07777U);
        }
        return result;
    }
}

static int git_copy_git_dir_without_objects(const char *source_path, const char *dest_path, int skip_objects) {
    PlatformDirEntry source_info;

    if (platform_get_path_info(source_path, &source_info) != 0 || !source_info.is_dir) {
        return -1;
    }

    {
        enum { GIT_LOCAL_COPY_ENTRY_CAPACITY = 1024 };
        PlatformDirEntry entries[GIT_LOCAL_COPY_ENTRY_CAPACITY];
        size_t count = 0U;
        size_t i;
        int is_directory = 0;
        int result = -1;

        if (platform_path_is_directory(dest_path, &is_directory) != 0) {
            if (platform_make_directory(dest_path, source_info.mode & 07777U) != 0) {
                return -1;
            }
        } else if (!is_directory) {
            return -1;
        }
        if (platform_collect_entries(source_path, 1, entries, GIT_LOCAL_COPY_ENTRY_CAPACITY, &count, &is_directory) != 0 || !is_directory) {
            return -1;
        }
        for (i = 0U; i < count; ++i) {
            char child_source[GIT_PATH_CAPACITY];
            char child_dest[GIT_PATH_CAPACITY];

            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0 ||
                (skip_objects && rt_strcmp(entries[i].name, "objects") == 0)) {
                continue;
            }
            if (git_join(child_source, sizeof(child_source), source_path, entries[i].name) != 0 ||
                git_join(child_dest, sizeof(child_dest), dest_path, entries[i].name) != 0) {
                goto done;
            }
            if (entries[i].is_dir) {
                if (git_copy_git_dir_without_objects(child_source, child_dest, 0) != 0) {
                    goto done;
                }
            } else if (tool_copy_path(child_source, child_dest, 0, 1, 1) != 0) {
                goto done;
            }
        }
        result = 0;
done:
        platform_free_entries(entries, count);
        if (result == 0) {
            (void)platform_change_mode(dest_path, source_info.mode & 07777U);
        }
        return result;
    }
}

static int git_copy_local_git_dir(const GitRepo *source_repo, const GitRepo *dest_repo) {
    char source_objects[GIT_PATH_CAPACITY];
    char dest_objects[GIT_PATH_CAPACITY];
    PlatformDirEntry source_info;

    if (git_copy_git_dir_without_objects(source_repo->git_dir, dest_repo->git_dir, 1) != 0 ||
        git_join(source_objects, sizeof(source_objects), source_repo->git_dir, "objects") != 0 ||
        git_join(dest_objects, sizeof(dest_objects), dest_repo->git_dir, "objects") != 0) {
        return -1;
    }
    if (platform_get_path_info(source_objects, &source_info) != 0) {
        return platform_make_directory(dest_objects, 0755U);
    }
    if (!source_info.is_dir) {
        return -1;
    }
    if (git_copy_local_objects_hardlink(source_objects, dest_objects) != 0) {
        return -1;
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

static int git_cmd_clone_remote(const char *remote_url, const char *destination_arg, const char *destination, const GitFetchOptions *fetch_options) {
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
    if (git_fetch_remote_to_repo(&repo, remote_url, 0, fetch_options, selected_ref, sizeof(selected_ref), selected_oid, &pack) != 0) {
        tool_write_error("git", "remote clone failed: ", remote_url);
        (void)tool_remove_path(destination, 1);
        goto done;
    }
    if (fetch_options != 0 && fetch_options->filter_blob_none) {
        GitFetchOptions hydrate_options = *fetch_options;

        hydrate_options.filter_blob_none = 0;
        git_pack_destroy(&pack);
        rt_memset(&pack, 0, sizeof(pack));
        git_progress_line("Hydrating checkout objects...");
        if (git_fetch_remote_to_repo(&repo, remote_url, selected_ref, &hydrate_options, selected_ref, sizeof(selected_ref), selected_oid, &pack) != 0) {
            tool_write_error("git", "remote checkout hydration failed: ", remote_url);
            (void)tool_remove_path(destination, 1);
            goto done;
        }
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
    GitRepo dest_repo;
    GitIndex index;
    GitFetchOptions fetch_options;
    char source_path[GIT_PATH_CAPACITY];
    char destination_arg[GIT_PATH_CAPACITY];
    char destination[GIT_PATH_CAPACITY];
    PlatformDirEntry existing;
    int result = 1;

    rt_memset(&fetch_options, 0, sizeof(fetch_options));
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strncmp(argv[argi], "--depth=", 8U) == 0) {
            unsigned long long depth;
            if (rt_parse_uint(argv[argi] + 8U, &depth) != 0 || depth == 0ULL || depth > (unsigned long long)((size_t)-1)) {
                tool_write_error("git", "unsupported clone depth: ", argv[argi] + 8U);
                return 1;
            }
            fetch_options.depth = (size_t)depth;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--depth") == 0) {
            unsigned long long depth;
            if (argi + 1 >= argc || rt_parse_uint(argv[argi + 1], &depth) != 0 || depth == 0ULL || depth > (unsigned long long)((size_t)-1)) {
                tool_write_error("git", "clone --depth needs a positive number", 0);
                return 1;
            }
            fetch_options.depth = (size_t)depth;
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--filter=blob:none") == 0) {
            fetch_options.filter_blob_none = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--filter") == 0) {
            if (argi + 1 >= argc || rt_strcmp(argv[argi + 1], "blob:none") != 0) {
                tool_write_error("git", "unsupported clone filter", 0);
                return 1;
            }
            fetch_options.filter_blob_none = 1;
            argi += 2;
        } else {
            break;
        }
    }
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
    if (git_url_is_remote(argv[argi])) {
        return git_cmd_clone_remote(argv[argi], destination_arg, destination, &fetch_options);
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
    rt_memset(&dest_repo, 0, sizeof(dest_repo));
    if (git_copy(dest_repo.work_tree, sizeof(dest_repo.work_tree), destination) != 0 ||
        git_join(dest_repo.git_dir, sizeof(dest_repo.git_dir), destination, ".git") != 0 ||
        git_copy_local_git_dir(&source_repo, &dest_repo) != 0 ||
        git_copy_tracked_files(&source_repo, &index, destination) != 0 ||
        git_write_index_file(&dest_repo, &index) != 0) {
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
        if (git_copy(ref, sizeof(ref), "refs/tags/") == 0 && rt_strlen(ref) + rt_strlen(name) < sizeof(ref)) {
            rt_copy_string(ref + rt_strlen(ref), sizeof(ref) - rt_strlen(ref), name);
            if (git_resolve_ref(repo, ref, hex, sizeof(hex)) == 0 && git_parse_oid_hex(hex, oid) == 0) {
                if (head_ref_out != 0) {
                    (void)git_copy(head_ref_out, head_ref_size, ref);
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
    return tool_byte_buffer_append_char(out, '\t') != 0 ||
           tool_byte_buffer_append_cstr(out, var) != 0 ||
           tool_byte_buffer_append_cstr(out, " = ") != 0 ||
           tool_byte_buffer_append_cstr(out, value) != 0 ||
           tool_byte_buffer_append_char(out, '\n') != 0 ? -1 : 0;
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
        if (git_buffer_append(&out, data + start, end - start) != 0 || tool_byte_buffer_append_char(&out, '\n') != 0) {
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
        if (out.size > 0U && out.data[out.size - 1U] != '\n' && tool_byte_buffer_append_char(&out, '\n') != 0) {
            goto done;
        }
        if (tool_byte_buffer_append_cstr(&out, section) != 0 || tool_byte_buffer_append_char(&out, '\n') != 0 || git_config_append_assignment(&out, var, value) != 0) {
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
    GitFetchOptions fetch_options;
    char remote_url[GIT_PATH_CAPACITY];
    const char *wanted_ref = 0;
    char selected_ref[GIT_REF_CAPACITY];
    unsigned char selected_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];

    rt_memset(&fetch_options, 0, sizeof(fetch_options));
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strncmp(argv[argi], "--depth=", 8U) == 0) {
            unsigned long long depth;
            if (rt_parse_uint(argv[argi] + 8U, &depth) != 0 || depth == 0ULL || depth > (unsigned long long)((size_t)-1)) {
                tool_write_error("git", "unsupported fetch depth: ", argv[argi] + 8U);
                return 1;
            }
            fetch_options.depth = (size_t)depth;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--depth") == 0) {
            unsigned long long depth;
            if (argi + 1 >= argc || rt_parse_uint(argv[argi + 1], &depth) != 0 || depth == 0ULL || depth > (unsigned long long)((size_t)-1)) {
                tool_write_error("git", "fetch --depth needs a positive number", 0);
                return 1;
            }
            fetch_options.depth = (size_t)depth;
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--filter=blob:none") == 0) {
            fetch_options.filter_blob_none = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--filter") == 0) {
            if (argi + 1 >= argc || rt_strcmp(argv[argi + 1], "blob:none") != 0) {
                tool_write_error("git", "unsupported fetch filter", 0);
                return 1;
            }
            fetch_options.filter_blob_none = 1;
            argi += 2;
        } else {
            tool_write_error("git", "unsupported fetch option: ", argv[argi]);
            return 1;
        }
    }
    if (argi < argc && git_url_is_remote(argv[argi])) {
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
    if (git_fetch_remote_to_repo(repo, remote_url, wanted_ref, &fetch_options, selected_ref, sizeof(selected_ref), selected_oid, 0) != 0) {
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
    GitFetchOptions fetch_options;
    char remote_url[GIT_PATH_CAPACITY];
    const char *wanted_ref = 0;
    char selected_ref[GIT_REF_CAPACITY];
    unsigned char selected_oid[CRYPTO_SHA1_DIGEST_SIZE];
    GitPack pack;
    int have_pack = 0;
    int result;

    rt_memset(&fetch_options, 0, sizeof(fetch_options));
    rt_memset(&pack, 0, sizeof(pack));
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strncmp(argv[argi], "--depth=", 8U) == 0) {
            unsigned long long depth;
            if (rt_parse_uint(argv[argi] + 8U, &depth) != 0 || depth == 0ULL || depth > (unsigned long long)((size_t)-1)) {
                tool_write_error("git", "unsupported pull depth: ", argv[argi] + 8U);
                return 1;
            }
            fetch_options.depth = (size_t)depth;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--depth") == 0) {
            unsigned long long depth;
            if (argi + 1 >= argc || rt_parse_uint(argv[argi + 1], &depth) != 0 || depth == 0ULL || depth > (unsigned long long)((size_t)-1)) {
                tool_write_error("git", "pull --depth needs a positive number", 0);
                return 1;
            }
            fetch_options.depth = (size_t)depth;
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--filter=blob:none") == 0 || rt_strcmp(argv[argi], "--filter") == 0) {
            tool_write_error("git", "pull filter is not supported for checkout", 0);
            return 1;
        } else {
            tool_write_error("git", "unsupported pull option: ", argv[argi]);
            return 1;
        }
    }
    if (argi < argc && git_url_is_remote(argv[argi])) {
        if (git_copy(remote_url, sizeof(remote_url), argv[argi]) != 0) return 1;
        argi += 1;
    } else if (git_read_origin_url(repo, remote_url, sizeof(remote_url)) != 0) {
        tool_write_error("git", "pull needs a network URL or remote origin", 0);
        return 1;
    }
    if (argi < argc) {
        wanted_ref = argv[argi++];
    }
    if (argi < argc) {
        tool_write_error("git", "too many pull arguments", 0);
        return 1;
    }
    if (git_fetch_remote_to_repo(repo, remote_url, wanted_ref, &fetch_options, selected_ref, sizeof(selected_ref), selected_oid, &pack) != 0) {
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
    if (git_url_is_remote(arg) || rt_strncmp(arg, "git://", 6U) == 0 || rt_strncmp(arg, "file://", 7U) == 0 || git_is_absolute_path(arg) || tool_path_has_separator(arg)) {
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
    if (git_resolve_revision(repo, src_name, new_oid, 0, 0) != 0 || git_push_ref_name(dst_name, dst_ref, sizeof(dst_ref)) != 0) {
        tool_write_error("git", "cannot resolve push refspec", 0);
        return 1;
    }
    if (git_url_is_remote(remote_url)) {
        GitUrl base_url;
        GitRemoteRefs refs;
        GitRemoteRef *remote_ref;

        rt_memset(&refs, 0, sizeof(refs));
        if (git_discover_receive_refs(remote_url, &base_url, &refs) != 0) {
            tool_write_error("git", "cannot discover remote receive-pack refs: ", remote_url);
            return 1;
        }
        remote_ref = git_remote_find_ref(&refs, dst_ref);
        if (remote_ref != 0) {
            memcpy(old_oid, remote_ref->oid, CRYPTO_SHA1_DIGEST_SIZE);
            have_old = 1;
        }
        git_remote_refs_destroy(&refs);
        if (have_old && !force) {
            GitPack local_pack;
            int have_local_pack = git_load_pack_cache(repo, &local_pack) == 0;

            if (!git_commit_is_ancestor_of(repo, old_oid, new_oid, have_local_pack ? &local_pack : 0)) {
                if (have_local_pack) {
                    git_pack_destroy(&local_pack);
                }
                tool_write_error("git", "push rejected: non-fast-forward", 0);
                return 1;
            }
            if (have_local_pack) {
                git_pack_destroy(&local_pack);
            }
        }
        if (git_receive_pack_push(repo, remote_url, dst_ref, old_oid, have_old, new_oid) != 0) {
            tool_write_error("git", "network push failed: ", remote_url);
            return 1;
        }
        rt_write_cstr(1, "Pushed ");
        rt_write_cstr(1, src_name);
        rt_write_cstr(1, " to ");
        rt_write_line(1, dst_ref);
        return 0;
    }
    if (rt_strncmp(remote_url, "git://", 6U) == 0) {
        tool_write_error("git", "network push transport is not implemented: ", remote_url);
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
    GitUntrackedWalk untracked;
    int have_pack = 0;
    int have_head_index = 0;
    int have_untracked = 0;
    int head_matches_index = 0;
    int short_output = 0;
    int porcelain_format = 0;
    int nul_terminate = 0;
    int color_mode = TOOL_COLOR_AUTO;
    int saw_change = 0;
    int result = 1;
    GitOutputBuffer out;

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
    git_output_init(&out, 1);
    if (git_load_index(repo, &index) != 0) {
        tool_write_error("git", "cannot read index", 0);
        return 1;
    }
    if (index.count > 1U && !git_index_is_sorted(&index)) {
        rt_sort(index.entries, index.count, sizeof(index.entries[0]), git_compare_entries_by_path);
    }
    if (git_index_cache_tree_matches_head(repo, &index, 0, &head_matches_index) != 0 || !head_matches_index) {
        have_pack = git_load_pack_cache(repo, &pack) == 0;
        have_head_index = git_load_head_tree_index_metadata(repo, have_pack ? &pack : 0, &head_index) == 0;
    }
    if (git_ignore_load(repo, &ignores) != 0) {
        if (have_head_index) git_index_destroy(&head_index);
        if (have_pack) git_pack_destroy(&pack);
        git_index_destroy(&index);
        tool_write_error("git", "cannot read ignore files", 0);
        return 1;
    }
    if (git_untracked_walk_collect(repo, &index, &ignores, 0, 0, 1, 1, &untracked) != 0) {
        goto done;
    }
    have_untracked = 1;
    if ((head_matches_index ? git_status_tracked_index_matches_head(&out, repo, &index, short_output, color_mode, nul_terminate, &saw_change) : have_head_index ? git_status_tracked_with_head(&out, repo, &head_index, &index, short_output, color_mode, nul_terminate, &saw_change) : git_status_tracked(&out, repo, &index, short_output, color_mode, nul_terminate, &saw_change)) != 0 ||
        git_status_emit_untracked_walk(&out, &untracked, short_output, color_mode, nul_terminate, &saw_change) != 0) {
        goto done;
    }
    if (!short_output && !saw_change) {
        git_output_line(&out, "nothing to commit, working tree clean");
    }
    result = git_output_flush(&out) == 0 ? 0 : 1;
done:
    if (have_untracked) git_untracked_walk_destroy(&untracked);
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
    GitOutputBuffer out;

    git_output_init(&out, 1);
    rt_memset(&pack, 0, sizeof(pack));
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
    if (compare_commits) {
        have_pack = git_load_pack_cache(repo, &pack) == 0;
        if (git_load_commit_tree_index_metadata(repo, old_commit_oid, have_pack ? &pack : 0, &old_tree_index) != 0 ||
            git_load_commit_tree_index_metadata(repo, new_commit_oid, have_pack ? &pack : 0, &new_tree_index) != 0) {
            tool_write_error("git", "cannot read commit tree", 0);
            goto done;
        }
        have_old_tree_index = 1;
        have_new_tree_index = 1;
        if (git_diff_index_pair(&out, repo, &old_tree_index, &new_tree_index, have_pack ? &pack : 0, output_mode, color_mode, nul_terminate, argv + pathspec_start, pathspec_count, &stats, &change_count) != 0) {
            tool_write_error("git", "cannot diff commits", 0);
            goto done;
        }
    } else if (cached_mode) {
        int head_matches_index = 0;

        if (git_index_cache_tree_matches_head(repo, &index, 0, &head_matches_index) != 0 || !head_matches_index) {
            have_pack = git_load_pack_cache(repo, &pack) == 0;
            have_head_index = git_load_head_tree_index_metadata(repo, have_pack ? &pack : 0, &head_index) == 0;
            if (!have_head_index) {
                tool_write_error("git", "cannot read HEAD tree", 0);
                goto done;
            }
            if (git_diff_index_pair(&out, repo, &head_index, &index, have_pack ? &pack : 0, output_mode, color_mode, nul_terminate, argv + pathspec_start, pathspec_count, &stats, &change_count) != 0) {
                tool_write_error("git", "cannot diff staged changes", 0);
                goto done;
            }
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
                if (!have_pack) {
                    have_pack = git_load_pack_cache(repo, &pack) == 0;
                }
                if (git_collect_diff_stat_entry(repo, &index.entries[i], modified, have_pack ? &pack : 0, &stats) != 0) {
                    tool_write_error("git", "cannot diff path: ", index.entries[i].path);
                    goto done;
                }
            } else if (output_mode == GIT_DIFF_OUTPUT_NAME_ONLY || output_mode == GIT_DIFF_OUTPUT_NAME_STATUS) {
                if (git_render_diff_path_mode(&out, output_mode, modified < 0 ? 'D' : 'M', index.entries[i].path, nul_terminate) != 0) {
                    goto done;
                }
            } else if (output_mode == GIT_DIFF_OUTPUT_PATCH) {
                if (!have_pack) {
                    have_pack = git_load_pack_cache(repo, &pack) == 0;
                }
                if (git_render_worktree_diff_patch_entry(repo, &index.entries[i], have_pack ? &pack : 0, color_mode) != 0) {
                    tool_write_error("git", "cannot diff path: ", index.entries[i].path);
                    goto done;
                }
            }
        }
    }
    if (output_mode == GIT_DIFF_OUTPUT_STAT && git_render_diff_stat(&out, &stats, color_mode) != 0) {
        goto done;
    }
    if (git_output_flush(&out) != 0) {
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

#include "refs_commands.c"

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

static int git_write_index_mode(GitOutputBuffer *out, unsigned int mode) {
    char digits[7];
    int pos;

    for (pos = 5; pos >= 0; --pos) {
        digits[pos] = (char)('0' + (mode & 7U));
        mode >>= 3U;
    }
    digits[6] = '\0';
    return git_output_cstr(out, digits);
}

#include "index_commands.c"

#include "commit_commands.c"

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

static int git_write_two_digits(char *out, unsigned int value) {
    out[0] = (char)('0' + (value / 10U) % 10U);
    out[1] = (char)('0' + value % 10U);
    return 2;
}

/* Print the native "Author:" line plus a formatted "Date:" line from a commit
 * identity line of the form "NAME <EMAIL> TIMESTAMP TZ". */
static int git_write_author_and_date(const char *author) {
    static const char *const weekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const char *name;
    const char *email;
    size_t name_len;
    size_t email_len;
    size_t len;
    size_t tz_start;
    size_t ts_end;
    size_t ts_start;
    long long timestamp = 0;
    int tz_sign = 1;
    int tz_hours = 0;
    int tz_minutes = 0;
    long long adjusted;
    long long days;
    long long secs;
    int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    int weekday;
    char line[96];
    size_t pos = 0U;
    char year_digits[16];
    size_t year_len = 0U;
    unsigned long long year_value;

    if (author == 0) {
        return 0;
    }
    git_author_name_email(author, &name, &name_len, &email, &email_len);
    if (rt_write_cstr(1, "Author: ") != 0 || rt_write_all(1, name, name_len) != 0 ||
        rt_write_cstr(1, " <") != 0 || rt_write_all(1, email, email_len) != 0 ||
        rt_write_cstr(1, ">\n") != 0) {
        return -1;
    }
    len = rt_strlen(author);
    while (len > 0U && tool_ascii_is_blank(author[len - 1U])) {
        len -= 1U;
    }
    tz_start = len;
    while (tz_start > 0U && !tool_ascii_is_blank(author[tz_start - 1U])) {
        tz_start -= 1U;
    }
    ts_end = tz_start;
    while (ts_end > 0U && tool_ascii_is_blank(author[ts_end - 1U])) {
        ts_end -= 1U;
    }
    ts_start = ts_end;
    while (ts_start > 0U && author[ts_start - 1U] >= '0' && author[ts_start - 1U] <= '9') {
        ts_start -= 1U;
    }
    if (ts_start == ts_end) {
        /* No parseable timestamp; fall back to the raw identity line. */
        return rt_write_cstr(1, "Date:   ") == 0 && rt_write_line(1, author + (ts_end >= len ? len : ts_end)) == 0 ? 0 : -1;
    }
    while (ts_start < ts_end) {
        timestamp = timestamp * 10 + (long long)(author[ts_start] - '0');
        ts_start += 1U;
    }
    {
        size_t k = tz_start;
        if (k < len && (author[k] == '+' || author[k] == '-')) {
            if (author[k] == '-') {
                tz_sign = -1;
            }
            k += 1U;
        }
        if (len >= k + 4U) {
            tz_hours = (author[k] - '0') * 10 + (author[k + 1U] - '0');
            tz_minutes = (author[k + 2U] - '0') * 10 + (author[k + 3U] - '0');
        }
    }
    adjusted = timestamp + (long long)tz_sign * ((long long)tz_hours * 3600LL + (long long)tz_minutes * 60LL);
    days = adjusted / 86400LL;
    secs = adjusted - days * 86400LL;
    if (secs < 0LL) {
        secs += 86400LL;
        days -= 1LL;
    }
    weekday = (int)(((days % 7LL) + 4LL + 7LL) % 7LL);
    tool_civil_from_days(days, &year, &month, &day);
    hour = (unsigned int)(secs / 3600LL);
    minute = (unsigned int)((secs / 60LL) % 60LL);
    second = (unsigned int)(secs % 60LL);
    if (month < 1U) {
        month = 1U;
    }
    if (month > 12U) {
        month = 12U;
    }
    rt_copy_string(line, sizeof(line), "Date:   ");
    pos = rt_strlen(line);
    memcpy(line + pos, weekdays[weekday], 3U);
    pos += 3U;
    line[pos++] = ' ';
    memcpy(line + pos, months[month - 1U], 3U);
    pos += 3U;
    line[pos++] = ' ';
    if (day < 10U) {
        line[pos++] = (char)('0' + day);
    } else {
        pos += (size_t)git_write_two_digits(line + pos, day);
    }
    line[pos++] = ' ';
    pos += (size_t)git_write_two_digits(line + pos, hour);
    line[pos++] = ':';
    pos += (size_t)git_write_two_digits(line + pos, minute);
    line[pos++] = ':';
    pos += (size_t)git_write_two_digits(line + pos, second);
    line[pos++] = ' ';
    year_value = year < 0 ? (unsigned long long)(-(long long)year) : (unsigned long long)year;
    rt_unsigned_to_string(year_value, year_digits, sizeof(year_digits));
    year_len = rt_strlen(year_digits);
    if (year < 0) {
        line[pos++] = '-';
    }
    memcpy(line + pos, year_digits, year_len);
    pos += year_len;
    line[pos++] = ' ';
    if (tz_start < len) {
        memcpy(line + pos, author + tz_start, len - tz_start);
        pos += len - tz_start;
    }
    line[pos++] = '\n';
    return rt_write_all(1, line, pos);
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

#include "history_commands.c"

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
    GitIndex tree_index;
    GitIndexEntry *path_entry;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char *blob = 0;
    size_t blob_size = 0U;
    char hex[GIT_OBJECT_HEX_SIZE + 1U];
    int have_pack;
    int stat_mode = 0;
    const char *revision = "HEAD";
    const char *pathspec = 0;
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
    {
        const char *colon = revision;
        while (*colon != '\0' && *colon != ':') {
            colon += 1;
        }
        if (*colon == ':' && colon != revision && colon[1] != '\0') {
            size_t revision_length = (size_t)(colon - revision);
            if (revision_length >= sizeof(hex)) {
                tool_write_error("git", "show revision is too long", 0);
                return 1;
            }
            memcpy(hex, revision, revision_length);
            hex[revision_length] = '\0';
            revision = hex;
            pathspec = colon + 1;
            if (stat_mode) {
                tool_write_error("git", "show --stat does not support REV:PATH", 0);
                return 1;
            }
        }
    }
    if (git_resolve_revision(repo, revision, oid, 0, 0) != 0) {
        tool_write_error("git", "cannot resolve show revision: ", revision);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (pathspec != 0) {
        int type = 0;

        rt_memset(&tree_index, 0, sizeof(tree_index));
        if (git_load_commit_tree_index(repo, oid, have_pack ? &pack : 0, &tree_index) != 0 || (path_entry = git_index_find(&tree_index, pathspec)) == 0 || git_read_object(repo, path_entry->oid, have_pack ? &pack : 0, &type, &blob, &blob_size) != 0 || type != GIT_OBJECT_BLOB) {
            tool_write_error("git", "cannot show path: ", pathspec);
            git_index_destroy(&tree_index);
            goto done;
        }
        if (rt_write_all(1, blob, blob_size) == 0) {
            result = 0;
        }
        rt_free(blob);
        git_index_destroy(&tree_index);
        goto done;
    }
    if (git_read_commit_info(repo, oid, have_pack ? &pack : 0, &info) != 0) {
        tool_write_error("git", "cannot read commit: ", revision);
        goto done;
    }
    git_format_oid_hex(oid, hex);
    rt_write_cstr(1, "commit ");
    rt_write_line(1, hex);
    if (info.author != 0) {
        git_write_author_and_date(info.author);
    }
    rt_write_char(1, '\n');
    rt_write_cstr(1, "    ");
    git_write_commit_subject_line(&info);
    rt_write_char(1, '\n');
    if (info.has_parent) {
        GitIndex old_index;
        GitIndex new_index;
        GitDiffStatList stats;
        GitOutputBuffer diff_out;

        rt_memset(&stats, 0, sizeof(stats));
        git_output_init(&diff_out, 1);
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
        if (git_diff_index_pair(&diff_out, repo, &old_index, &new_index, have_pack ? &pack : 0, stat_mode ? GIT_DIFF_OUTPUT_STAT : GIT_DIFF_OUTPUT_PATCH, TOOL_COLOR_AUTO, 0, 0, 0, &stats, &change_count) == 0) {
            if (stat_mode) {
                (void)git_render_diff_stat(&diff_out, &stats, TOOL_COLOR_AUTO);
            }
            (void)git_output_flush(&diff_out);
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

static int git_path_arg_to_relative(const GitRepo *repo, const char *arg, char *relative, size_t relative_size, char *full_path, size_t full_path_size) {
    if (git_is_absolute_path(arg)) {
        if (git_copy(full_path, full_path_size, arg) != 0 || git_relative_path(repo, full_path, relative, relative_size) != 0) {
            return -1;
        }
        return relative[0] == '\0' || tool_path_is_unsafe_relative(relative) ? -1 : 0;
    }
    if (git_join(full_path, full_path_size, repo->work_tree, arg) != 0 || git_copy(relative, relative_size, arg) != 0) {
        return -1;
    }
    while (relative[0] == '.' && relative[1] == '/') {
        memmove(relative, relative + 2, rt_strlen(relative + 2) + 1U);
    }
    return relative[0] == '\0' || tool_path_is_unsafe_relative(relative) ? -1 : 0;
}

static int git_cmd_mv(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    GitAddContext context;
    char source_relative[GIT_PATH_CAPACITY];
    char dest_relative[GIT_PATH_CAPACITY];
    char source_full[GIT_PATH_CAPACITY];
    char dest_full[GIT_PATH_CAPACITY];
    int dest_is_dir = 0;
    char *source_specs[1];
    size_t i = 0U;
    int removed = 0;
    int result = 1;

    if (argi < argc && rt_strcmp(argv[argi], "--") == 0) {
        argi += 1;
    }
    if (argi + 2 != argc) {
        tool_write_error("git", "mv needs SOURCE and DEST", 0);
        return 1;
    }
    if (git_path_arg_to_relative(repo, argv[argi], source_relative, sizeof(source_relative), source_full, sizeof(source_full)) != 0 ||
        git_path_arg_to_relative(repo, argv[argi + 1], dest_relative, sizeof(dest_relative), dest_full, sizeof(dest_full)) != 0) {
        tool_write_error("git", "mv path is outside the worktree", 0);
        return 1;
    }
    if (platform_path_is_directory(dest_full, &dest_is_dir) == 0 && dest_is_dir) {
        const char *base = tool_base_name(source_relative);
        if (git_join(dest_full, sizeof(dest_full), dest_full, base) != 0 || git_relative_path(repo, dest_full, dest_relative, sizeof(dest_relative)) != 0 || tool_path_is_unsafe_relative(dest_relative)) {
            tool_write_error("git", "mv destination path is invalid", 0);
            return 1;
        }
    }
    if (git_load_index(repo, &index) != 0) {
        tool_write_error("git", "cannot read index", 0);
        return 1;
    }
    source_specs[0] = source_relative;
    for (i = 0U; i < index.count; ++i) {
        if (git_pathspec_matches(index.entries[i].path, source_specs, 1)) {
            removed = 1;
            break;
        }
    }
    if (!removed) {
        tool_write_error("git", "source is not tracked: ", source_relative);
        goto done;
    }
    if (git_ensure_parent_directory(dest_full) != 0 ||
        (platform_rename_path(source_full, dest_full) != 0 && (tool_copy_path(source_full, dest_full, 1, 1, 1) != 0 || tool_remove_path(source_full, 1) != 0))) {
        tool_write_error("git", "cannot move path: ", source_relative);
        goto done;
    }
    i = 0U;
    while (i < index.count) {
        if (git_pathspec_matches(index.entries[i].path, source_specs, 1)) {
            git_index_remove_at(&index, i);
            continue;
        }
        i += 1U;
    }
    context.repo = repo;
    context.index = &index;
    context.ignores = 0;
    context.added = 0U;
    if (git_add_stage_one_path(&context, dest_relative) != 0 || git_write_index_file(repo, &index) != 0) {
        tool_write_error("git", "cannot update index for moved path: ", dest_relative);
        goto done;
    }
    result = 0;
done:
    git_index_destroy(&index);
    return result;
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
