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
    GitIgnoreList *ignores;
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

static int git_add_patch_append_lines(GitBuffer *buffer, GitDiffLine *lines, size_t start, size_t end) {
    size_t i;

    for (i = start; i < end; ++i) {
        if (git_buffer_append(buffer, lines[i].data, lines[i].length) != 0 || tool_byte_buffer_append_char(buffer, '\n') != 0) {
            return -1;
        }
    }
    return 0;
}

static int git_add_patch_prompt_hunk(const char *path, GitDiffLine *old_lines, GitDiffLine *new_lines, size_t old_start, size_t old_end, size_t new_start, size_t new_end) {
    size_t i;

    rt_write_cstr(1, "diff --git a/");
    rt_write_cstr(1, path);
    rt_write_cstr(1, " b/");
    rt_write_line(1, path);
    rt_write_cstr(1, "@@ -");
    git_write_diff_hunk_range(old_start, old_end - old_start);
    rt_write_cstr(1, " +");
    git_write_diff_hunk_range(new_start, new_end - new_start);
    rt_write_line(1, " @@");
    for (i = old_start; i < old_end; ++i) {
        git_write_diff_line(&old_lines[i], '-', TOOL_COLOR_AUTO);
    }
    for (i = new_start; i < new_end; ++i) {
        git_write_diff_line(&new_lines[i], '+', TOOL_COLOR_AUTO);
    }
    return tool_prompt_yes_no("stage this hunk in ", path);
}

static int git_add_patch_prompt_file(const char *path, const unsigned char *old_data, size_t old_size, const unsigned char *new_data, size_t new_size, int old_exists, int new_exists, unsigned int old_mode, unsigned int new_mode) {
    if (git_write_diff_patch(path, old_data, old_size, new_data, new_size, old_exists, new_exists, old_mode, new_mode, TOOL_COLOR_AUTO) != 0) {
        return 0;
    }
    return tool_prompt_yes_no("stage this change in ", path);
}

static int git_add_patch_prompt_delete_path(GitAddContext *context, GitIndexEntry *entry, const GitPack *pack_cache) {
    unsigned char *old_data = 0;
    size_t old_size = 0U;
    int accept = 0;

    if (git_read_index_blob(context->repo, entry, pack_cache, &old_data, &old_size) != 0) {
        return -1;
    }
    accept = git_add_patch_prompt_file(entry->path, old_data, old_size, (const unsigned char *)"", 0U, 1, 0, entry->mode, 0U);
    rt_free(old_data);
    return accept;
}

static int git_add_patch_stage_untracked_path(const char *path, void *user_data) {
    GitAddContext *context = (GitAddContext *)user_data;
    GitIndexEntry preview;
    PlatformDirEntry info;
    unsigned char *new_data = 0;
    size_t new_size = 0U;
    char full_path[GIT_PATH_CAPACITY];
    int accept;
    int result;

    rt_memset(&preview, 0, sizeof(preview));
    preview.path = (char *)path;
    if (git_join(full_path, sizeof(full_path), context->repo->work_tree, path) != 0 || platform_get_path_info(full_path, &info) != 0 || info.is_dir) {
        return -1;
    }
    preview.mode = (info.mode & GIT_MODE_TYPE_MASK) == GIT_MODE_SYMLINK ? GIT_MODE_SYMLINK : git_regular_index_mode_from_worktree(info.mode);
    if (git_read_worktree_blob(context->repo, &preview, &new_data, &new_size) != 0) {
        return -1;
    }
    accept = git_add_patch_prompt_file(path, (const unsigned char *)"", 0U, new_data, new_size, 0, 1, 0U, preview.mode);
    rt_free(new_data);
    if (!accept) {
        return 0;
    }
    result = git_add_stage_blob_path(context, path);
    return result;
}

static int git_add_patch_stage_modified_path(GitAddContext *context, GitIndexEntry *entry, const GitPack *pack_cache) {
    unsigned char *old_data = 0;
    unsigned char *new_data = 0;
    size_t old_size = 0U;
    size_t new_size = 0U;
    GitDiffLine *old_lines = 0;
    GitDiffLine *new_lines = 0;
    size_t old_count = 0U;
    size_t new_count = 0U;
    size_t old_pos = 0U;
    size_t new_pos = 0U;
    size_t copied_old = 0U;
    GitBuffer staged;
    GitIndexEntry updated;
    PlatformDirEntry info;
    char full_path[GIT_PATH_CAPACITY];
    int staged_any = 0;
    int result = -1;

    rt_memset(&staged, 0, sizeof(staged));
    rt_memset(&updated, 0, sizeof(updated));
    if (entry->intent_to_add || !git_index_mode_is_regular(entry->mode) || git_read_index_blob(context->repo, entry, pack_cache, &old_data, &old_size) != 0 || git_read_worktree_blob(context->repo, entry, &new_data, &new_size) != 0) {
        goto done;
    }
    if (old_size == new_size && (old_size == 0U || memcmp(old_data, new_data, old_size) == 0)) {
        result = 0;
        goto done;
    }
    if (git_split_diff_lines(old_data, old_size, &old_lines, &old_count) != 0 || git_split_diff_lines(new_data, new_size, &new_lines, &new_count) != 0) {
        goto done;
    }
    while (old_pos < old_count || new_pos < new_count) {
        size_t common = 0U;
        size_t best_old = old_count;
        size_t best_new = new_count;
        size_t old_start;
        size_t new_start;
        size_t old_end;
        size_t new_end;
        int accept;

        while (old_pos < old_count && new_pos < new_count && git_diff_lines_equal(&old_lines[old_pos], &new_lines[new_pos])) {
            old_pos += 1U;
            new_pos += 1U;
        }
        if (old_pos >= old_count && new_pos >= new_count) {
            break;
        }
        old_start = old_pos;
        new_start = new_pos;
        if (old_pos >= old_count) {
            old_end = old_pos;
            new_end = new_count;
        } else if (new_pos >= new_count) {
            old_end = old_count;
            new_end = new_pos;
        } else {
            size_t old_scan;
            size_t new_scan;

            for (old_scan = old_pos; old_scan < old_count && old_scan < old_pos + 64U; ++old_scan) {
                for (new_scan = new_pos; new_scan < new_count && new_scan < new_pos + 64U; ++new_scan) {
                    if (git_diff_lines_equal(&old_lines[old_scan], &new_lines[new_scan])) {
                        size_t score = 0U;

                        while (old_scan + score < old_count && new_scan + score < new_count && git_diff_lines_equal(&old_lines[old_scan + score], &new_lines[new_scan + score])) {
                            score += 1U;
                        }
                        if (score > common) {
                            common = score;
                            best_old = old_scan;
                            best_new = new_scan;
                        }
                    }
                }
            }
            if (common == 0U) {
                old_end = old_count;
                new_end = new_count;
            } else {
                old_end = best_old;
                new_end = best_new;
            }
        }
        if (git_add_patch_append_lines(&staged, old_lines, copied_old, old_start) != 0) {
            goto done;
        }
        accept = git_add_patch_prompt_hunk(entry->path, old_lines, new_lines, old_start, old_end, new_start, new_end);
        if (accept) {
            if (git_add_patch_append_lines(&staged, new_lines, new_start, new_end) != 0) {
                goto done;
            }
            staged_any = 1;
        } else if (git_add_patch_append_lines(&staged, old_lines, old_start, old_end) != 0) {
            goto done;
        }
        copied_old = old_end;
        old_pos = old_end;
        new_pos = new_end;
    }
    if (git_add_patch_append_lines(&staged, old_lines, copied_old, old_count) != 0) {
        goto done;
    }
    if (!staged_any) {
        result = 0;
        goto done;
    }
    updated = *entry;
    updated.path = git_strdup_n(entry->path, rt_strlen(entry->path));
    if (updated.path == 0 || git_write_loose_object(context->repo, GIT_OBJECT_BLOB, staged.data, staged.size, updated.oid) != 0) {
        rt_free(updated.path);
        goto done;
    }
    updated.size = staged.size;
    if (git_join(full_path, sizeof(full_path), context->repo->work_tree, entry->path) == 0 && platform_get_path_info(full_path, &info) == 0) {
        updated.mode = git_regular_index_mode_from_worktree(info.mode);
        updated.mtime_seconds = (unsigned long long)info.mtime;
        updated.mtime_nanos = info.mtime_nanos;
    }
    updated.intent_to_add = 0;
    if (git_index_replace_or_insert(context->index, &updated) != 0) {
        rt_free(updated.path);
        goto done;
    }
    context->added += 1U;
    result = 0;
done:
    git_buffer_destroy(&staged);
    rt_free(old_lines);
    rt_free(new_lines);
    rt_free(old_data);
    rt_free(new_data);
    return result;
}

static int git_add_patch_matching_paths(GitAddContext *context, char **pathspecs, int pathspec_count) {
    GitPack pack;
    int have_pack;
    size_t i = 0U;
    int result = 0;

    have_pack = git_load_pack_cache(context->repo, &pack) == 0;

    while (i < context->index->count) {
        int modified;

        if (!git_pathspec_matches(context->index->entries[i].path, pathspecs, pathspec_count)) {
            i += 1U;
            continue;
        }
        modified = git_entry_is_modified(context->repo, &context->index->entries[i]);
        if (modified == 0) {
            i += 1U;
            continue;
        }
        if (modified < 0) {
            int accept = git_add_patch_prompt_delete_path(context, &context->index->entries[i], have_pack ? &pack : 0);

            if (accept < 0) {
                result = -1;
                break;
            }
            if (accept) {
                git_index_remove_at(context->index, i);
                context->added += 1U;
                continue;
            }
        } else {
            if (git_add_patch_stage_modified_path(context, &context->index->entries[i], have_pack ? &pack : 0) != 0) {
                result = -1;
                break;
            }
        }
        i += 1U;
    }
    if (result == 0 && git_for_each_untracked_path(context->repo, context->index, context->ignores, pathspecs, pathspec_count, git_add_patch_stage_untracked_path, context) != 0) {
        result = -1;
    }
    if (have_pack) {
        git_pack_destroy(&pack);
    }
    return result;
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
    } else if (patch_mode) {
        if (git_add_patch_matching_paths(&context, argv + pathspec_start, pathspec_count) != 0) {
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
