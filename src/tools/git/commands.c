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
    GitIgnoreList ignores;
    int short_output = 0;
    int porcelain_format = 0;
    int color_mode = TOOL_COLOR_AUTO;
    int saw_change = 0;

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
    if (git_ignore_load(repo, &ignores) != 0) {
        git_index_destroy(&index);
        tool_write_error("git", "cannot read ignore files", 0);
        return 1;
    }
    if (git_status_tracked(repo, &index, short_output, color_mode, &saw_change) != 0 ||
        git_status_untracked(repo, &index, &ignores, short_output, color_mode, &saw_change) != 0) {
        git_ignore_destroy(&ignores);
        git_index_destroy(&index);
        return 1;
    }
    git_ignore_destroy(&ignores);
    git_index_destroy(&index);
    if (!short_output && !saw_change) {
        rt_write_line(1, "nothing to commit, working tree clean");
    }
    return 0;
}

static int git_cmd_diff(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    GitPack pack;
    GitDiffStatList stats;
    int have_pack = 0;
    int stat_mode = 0;
    int saw_separator = 0;
    int color_mode = TOOL_COLOR_AUTO;
    int pathspec_start;
    int pathspec_count;
    size_t i;
    int result = 1;

    rt_memset(&stats, 0, sizeof(stats));
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--stat") == 0) {
            stat_mode = 1;
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
    (void)saw_separator;
    if (!stat_mode) {
        tool_write_error("git", "only diff --stat is implemented", 0);
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
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    for (i = 0U; i < index.count; ++i) {
        if (!git_pathspec_matches(index.entries[i].path, argv + pathspec_start, pathspec_count)) {
            continue;
        }
        if (git_collect_diff_stat_entry(repo, &index.entries[i], have_pack ? &pack : 0, &stats) != 0) {
            tool_write_error("git", "cannot diff path: ", index.entries[i].path);
            goto done;
        }
    }
    if (git_render_diff_stat(&stats, color_mode) != 0) {
        goto done;
    }
    result = 0;
done:
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
    size_t added;
} GitAddIntentContext;

static void git_empty_blob_oid(unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    static const unsigned char empty_blob[CRYPTO_SHA1_DIGEST_SIZE] = {
        0xe6U, 0x9dU, 0xe2U, 0x9bU, 0xb2U, 0xd1U, 0xd6U, 0x43U, 0x4bU, 0x8bU,
        0x29U, 0xaeU, 0x77U, 0x5aU, 0xd8U, 0xc2U, 0xe4U, 0x8cU, 0x53U, 0x91U
    };

    memcpy(oid, empty_blob, sizeof(empty_blob));
}

static int git_add_intent_path(const char *path, void *user_data) {
    GitAddIntentContext *context = (GitAddIntentContext *)user_data;
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

static int git_cmd_add(GitRepo *repo, int argc, char **argv, int argi) {
    GitIndex index;
    GitIgnoreList ignores;
    GitAddIntentContext context;
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
    if (!intent_to_add) {
        tool_write_error("git", "only add -N is implemented", 0);
        return 1;
    }
    if (argi >= argc) {
        tool_write_error("git", "add -N needs a path", 0);
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
    context.added = 0U;
    if (git_for_each_untracked_path(repo, &index, &ignores, argv + pathspec_start, pathspec_count, git_add_intent_path, &context) != 0) {
        goto done;
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

