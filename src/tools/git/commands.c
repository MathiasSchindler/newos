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
    if (git_join(path, sizeof(path), repo->git_dir, "refs/remotes/origin") != 0 || git_make_directory_chain(path) != 0) {
        return -1;
    }
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

static int git_cmd_checkout(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    char head_ref[GIT_REF_CAPACITY];
    char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
    int have_pack;

    if (argi >= argc) {
        tool_write_error("git", "checkout needs a ref", 0);
        return 1;
    }
    if (argi + 1 < argc) {
        tool_write_error("git", "too many checkout arguments", 0);
        return 1;
    }
    if (git_resolve_revision(repo, argv[argi], oid, head_ref, sizeof(head_ref)) != 0) {
        tool_write_error("git", "cannot resolve checkout ref: ", argv[argi]);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (git_checkout_commit_to_worktree(repo, oid, have_pack ? &pack : 0) != 0) {
        if (have_pack) {
            git_pack_destroy(&pack);
        }
        tool_write_error("git", "checkout failed: ", argv[argi]);
        return 1;
    }
    if (have_pack) {
        git_pack_destroy(&pack);
    }
    if (head_ref[0] != '\0') {
        (void)git_write_head_ref(repo, head_ref);
    } else {
        char path[GIT_PATH_CAPACITY];
        git_format_oid_hex(oid, oid_hex);
        if (git_join(path, sizeof(path), repo->git_dir, "HEAD") == 0) {
            oid_hex[GIT_OBJECT_HEX_SIZE] = '\n';
            (void)git_write_all_file(path, oid_hex, GIT_OBJECT_HEX_SIZE + 1U, 0644U);
            oid_hex[GIT_OBJECT_HEX_SIZE] = '\0';
        }
    }
    git_format_oid_hex(oid, oid_hex);
    rt_write_cstr(1, "Checked out ");
    rt_write_line(1, oid_hex);
    return 0;
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
    int color_mode = TOOL_COLOR_AUTO;
    int saw_change = 0;
    int result = 1;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--short") == 0 || rt_strcmp(argv[argi], "-s") == 0) {
            short_output = 1;
        } else if (rt_strcmp(argv[argi], "--porcelain") == 0) {
            short_output = 1;
            porcelain_format = 1;
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
    if ((have_head_index ? git_status_tracked_with_head(repo, &head_index, &index, short_output, color_mode, &saw_change) : git_status_tracked(repo, &index, short_output, color_mode, &saw_change)) != 0 ||
        git_status_untracked(repo, &index, &ignores, short_output, color_mode, &saw_change) != 0) {
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
    int stat_mode = 0;
    int cached_mode = 0;
    int saw_separator = 0;
    int compare_commits = 0;
    int color_mode = TOOL_COLOR_AUTO;
    int pathspec_start;
    int pathspec_count;
    unsigned char old_commit_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char new_commit_oid[CRYPTO_SHA1_DIGEST_SIZE];
    size_t i;
    int result = 1;

    rt_memset(&stats, 0, sizeof(stats));
    rt_memset(&old_tree_index, 0, sizeof(old_tree_index));
    rt_memset(&new_tree_index, 0, sizeof(new_tree_index));
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--stat") == 0) {
            stat_mode = 1;
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
        if (git_diff_index_pair(repo, &old_tree_index, &new_tree_index, have_pack ? &pack : 0, stat_mode, color_mode, argv + pathspec_start, pathspec_count, &stats) != 0) {
            tool_write_error("git", "cannot diff commits", 0);
            goto done;
        }
    } else if (cached_mode) {
        have_head_index = git_load_head_tree_index(repo, have_pack ? &pack : 0, &head_index) == 0;
        if (!have_head_index) {
            tool_write_error("git", "cannot read HEAD tree", 0);
            goto done;
        }
        if (git_diff_index_pair(repo, &head_index, &index, have_pack ? &pack : 0, stat_mode, color_mode, argv + pathspec_start, pathspec_count, &stats) != 0) {
            tool_write_error("git", "cannot diff staged changes", 0);
            goto done;
        }
    } else {
        for (i = 0U; i < index.count; ++i) {
            if (!git_pathspec_matches(index.entries[i].path, argv + pathspec_start, pathspec_count)) {
                continue;
            }
            if (stat_mode) {
                if (git_collect_diff_stat_entry(repo, &index.entries[i], have_pack ? &pack : 0, &stats) != 0) {
                    tool_write_error("git", "cannot diff path: ", index.entries[i].path);
                    goto done;
                }
            } else if (git_render_worktree_diff_patch_entry(repo, &index.entries[i], have_pack ? &pack : 0, color_mode) != 0) {
                tool_write_error("git", "cannot diff path: ", index.entries[i].path);
                goto done;
            }
        }
    }
    if (stat_mode && git_render_diff_stat(&stats, color_mode) != 0) {
        goto done;
    }
    result = 0;
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
    tool_write_error("git", "unsupported branch mode", 0);
    return 1;
}

static int git_cmd_rev_parse(GitRepo *repo, int argc, char **argv, int argi) {
    int exit_code = 0;

    if (argi >= argc) {
        tool_write_error("git", "rev-parse needs an argument", 0);
        return 1;
    }
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--show-toplevel") == 0) {
            rt_write_line(1, repo->work_tree);
        } else if (rt_strcmp(argv[argi], "--git-dir") == 0) {
            rt_write_line(1, repo->git_dir);
        } else if (rt_strcmp(argv[argi], "--abbrev-ref") == 0 && argi + 1 < argc && rt_strcmp(argv[argi + 1], "HEAD") == 0) {
            const char *branch = git_branch_name(repo);

            rt_write_line(1, branch != 0 ? branch : "HEAD");
            argi += 1;
        } else if (rt_strcmp(argv[argi], "HEAD") == 0) {
            if (repo->head_oid[0] == '\0') {
                exit_code = 1;
            } else {
                rt_write_line(1, repo->head_oid);
            }
        } else {
            tool_write_error("git", "unsupported rev-parse argument: ", argv[argi]);
            exit_code = 1;
        }
        argi += 1;
    }
    return exit_code;
}

static int git_ls_files_write_path(const char *path, void *user_data) {
    (void)user_data;
    return rt_write_line(1, path);
}

static int git_cmd_ls_files(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    GitIgnoreList ignores;
    int show_cached = 0;
    int show_others = 0;
    int saw_selector = 0;
    int exclude_standard = 0;
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
            if (git_pathspec_matches(index.entries[i].path, argv + pathspec_start, pathspec_count) && rt_write_line(1, index.entries[i].path) != 0) {
                goto done;
            }
        }
    }
    if (show_others) {
        GitIgnoreList *ignore_ptr = 0;

        rt_memset(&ignores, 0, sizeof(ignores));
        if (exclude_standard) {
            if (git_ignore_load(repo, &ignores) != 0) {
                tool_write_error("git", "cannot read ignore files", 0);
                goto done;
            }
            ignore_ptr = &ignores;
        }
        if (git_for_each_untracked_path(repo, &index, ignore_ptr, argv + pathspec_start, pathspec_count, git_ls_files_write_path, 0) != 0) {
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
    int pathspec_start;
    int pathspec_count;
    int result = 1;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-N") == 0 || rt_strcmp(argv[argi], "--intent-to-add") == 0) {
            intent_to_add = 1;
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
    if (argi >= argc) {
        tool_write_error("git", "add needs a path", 0);
        return 1;
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
    int have_parent = 0;
    int have_pack = 0;
    int allow_empty = 0;
    int result = 1;

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
        } else {
            tool_write_error("git", "unsupported commit option: ", argv[argi]);
            return 1;
        }
    }
    if (message == 0 || message[0] == '\0') {
        tool_write_error("git", "commit needs -m MESSAGE", 0);
        return 1;
    }
    if (git_load_index(repo, &index) != 0) {
        tool_write_error("git", "cannot read index", 0);
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
        if (git_write_ref_oid(repo, repo->head_ref, commit_oid) != 0) {
            goto commit_done;
        }
    } else if (git_write_detached_head_oid(repo, commit_oid) != 0) {
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

