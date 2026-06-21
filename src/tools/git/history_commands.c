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

static int git_describe_distance(GitRepo *repo, const unsigned char target[CRYPTO_SHA1_DIGEST_SIZE], const unsigned char tag_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack, size_t *distance_out) {
    GitOidList target_reachable;
    GitOidList tag_reachable;
    size_t i;
    size_t distance = 0U;
    int result = -1;

    rt_memset(&target_reachable, 0, sizeof(target_reachable));
    rt_memset(&tag_reachable, 0, sizeof(tag_reachable));
    if (git_collect_reachable_commits(repo, target, pack, &target_reachable) != 0) {
        goto done;
    }
    if (!git_oid_list_contains(&target_reachable, tag_oid)) {
        result = 1;
        goto done;
    }
    if (git_collect_reachable_commits(repo, tag_oid, pack, &tag_reachable) != 0) {
        goto done;
    }
    for (i = 0U; i < target_reachable.count; ++i) {
        if (!git_oid_list_contains(&tag_reachable, target_reachable.oids[i])) {
            distance += 1U;
        }
    }
    *distance_out = distance;
    result = 0;
done:
    git_oid_list_destroy(&target_reachable);
    git_oid_list_destroy(&tag_reachable);
    return result;
}

static int git_cmd_describe(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    GitDescribeTagList tags;
    unsigned char target[CRYPTO_SHA1_DIGEST_SIZE];
    char short_hex[GIT_OBJECT_HEX_SIZE + 1U];
    const char *revision = "HEAD";
    const char *best_name = 0;
    size_t best_distance = 0U;
    int have_best = 0;
    int have_pack;
    int result = 1;
    size_t i;

    if (argi < argc) {
        revision = argv[argi++];
    }
    if (argi < argc) {
        tool_write_error("git", "too many describe arguments", 0);
        return 1;
    }
    if (git_resolve_revision(repo, revision, target, 0, 0) != 0) {
        tool_write_error("git", "cannot resolve describe revision: ", revision);
        return 1;
    }
    rt_memset(&tags, 0, sizeof(tags));
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (git_describe_collect_loose_tags(repo, "refs/tags", &tags) != 0 || git_describe_collect_packed_tags(repo, &tags) != 0) {
        goto done;
    }
    for (i = 0U; i < tags.count; ++i) {
        size_t distance = 0U;
        int distance_result;

        if (git_oid_equal(tags.tags[i].oid, target)) {
            rt_write_line(1, tags.tags[i].name);
            result = 0;
            goto done;
        }
        distance_result = git_describe_distance(repo, target, tags.tags[i].oid, have_pack ? &pack : 0, &distance);
        if (distance_result == 0 && (!have_best || distance < best_distance)) {
            best_name = tags.tags[i].name;
            best_distance = distance;
            have_best = 1;
        } else if (distance_result < 0) {
            goto done;
        }
    }
    if (!have_best) {
        tool_write_error("git", "no reachable tag", 0);
        goto done;
    }
    git_format_oid_hex(target, short_hex);
    short_hex[7] = '\0';
    rt_write_cstr(1, best_name);
    rt_write_char(1, '-');
    git_write_size(best_distance);
    rt_write_cstr(1, "-g");
    rt_write_line(1, short_hex);
    result = 0;
done:
    if (have_pack) git_pack_destroy(&pack);
    git_describe_tag_list_destroy(&tags);
    return result;
}

typedef struct {
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    GitCommitInfo info;
    unsigned long long timestamp;
    int emitted;
} GitLogEntry;

static unsigned long long git_commit_identity_timestamp(const char *line) {
    size_t end;
    size_t timestamp_end;
    size_t timestamp_start;
    unsigned long long value = 0ULL;

    if (line == 0) {
        return 0ULL;
    }
    end = rt_strlen(line);
    while (end > 0U && tool_ascii_is_space(line[end - 1U])) {
        end -= 1U;
    }
    while (end > 0U && !tool_ascii_is_space(line[end - 1U])) {
        end -= 1U;
    }
    while (end > 0U && tool_ascii_is_space(line[end - 1U])) {
        end -= 1U;
    }
    timestamp_end = end;
    while (end > 0U && line[end - 1U] >= '0' && line[end - 1U] <= '9') {
        end -= 1U;
    }
    timestamp_start = end;
    if (timestamp_start == timestamp_end) {
        return 0ULL;
    }
    while (timestamp_start < timestamp_end) {
        value = value * 10ULL + (unsigned long long)(line[timestamp_start] - '0');
        timestamp_start += 1U;
    }
    return value;
}

static unsigned long long git_commit_sort_timestamp(const GitCommitInfo *info) {
    unsigned long long value = git_commit_identity_timestamp(info->committer);

    if (value == 0ULL) {
        value = git_commit_identity_timestamp(info->author);
    }
    return value;
}

static int git_log_entry_has_parent(const GitLogEntry *entry, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    size_t parent_index;

    for (parent_index = 0U; parent_index < entry->info.parent_count; ++parent_index) {
        if (git_oid_equal(entry->info.parents[parent_index], oid)) {
            return 1;
        }
    }
    return 0;
}

static int git_log_entry_is_blocked_by_child(const GitLogEntry *entries, size_t count, size_t candidate) {
    size_t i;

    for (i = 0U; i < count; ++i) {
        if (i != candidate && !entries[i].emitted && git_log_entry_has_parent(&entries[i], entries[candidate].oid)) {
            return 1;
        }
    }
    return 0;
}

static size_t git_log_select_next_entry(const GitLogEntry *entries, size_t count) {
    size_t best = count;
    size_t i;

    for (i = 0U; i < count; ++i) {
        if (entries[i].emitted || git_log_entry_is_blocked_by_child(entries, count, i)) {
            continue;
        }
        if (best == count || entries[i].timestamp > entries[best].timestamp) {
            best = i;
        }
    }
    if (best != count) {
        return best;
    }
    for (i = 0U; i < count; ++i) {
        if (!entries[i].emitted) {
            return i;
        }
    }
    return count;
}

static void git_log_entries_destroy(GitLogEntry *entries, size_t count) {
    size_t i;

    if (entries == 0) {
        return;
    }
    for (i = 0U; i < count; ++i) {
        git_commit_info_destroy(&entries[i].info);
    }
    rt_free(entries);
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
    char range_left[GIT_REF_CAPACITY];
    char range_right[GIT_REF_CAPACITY];
    unsigned char exclude_oid[CRYPTO_SHA1_DIGEST_SIZE];
    int have_exclude = 0;
    GitOidList reachable;
    GitOidList excluded;
    GitLogEntry *entries = 0;
    size_t entry_count = 0U;
    size_t emitted = 0U;
    int result = 0;

    rt_memset(&excluded, 0, sizeof(excluded));
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--oneline") == 0) {
            oneline = 1;
        } else if (rt_strcmp(argv[argi], "--date-order") == 0 || rt_strcmp(argv[argi], "--topo-order") == 0) {
            /* The default log walker already enforces this ordering. */
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
        } else if (git_split_revision_range(argv[argi], range_left, sizeof(range_left), range_right, sizeof(range_right)) == 0) {
            start = range_right;
            if (git_resolve_revision(repo, range_left, exclude_oid, 0, 0) != 0) {
                tool_write_error("git", "cannot resolve log range start: ", range_left);
                return 1;
            }
            have_exclude = 1;
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
    if (git_collect_reachable_commits(repo, oid, have_pack ? &pack : 0, &reachable) != 0) {
        if (have_pack) git_pack_destroy(&pack);
        return 1;
    }
    if (have_exclude && git_collect_reachable_commits(repo, exclude_oid, have_pack ? &pack : 0, &excluded) != 0) {
        git_oid_list_destroy(&reachable);
        if (have_pack) git_pack_destroy(&pack);
        return 1;
    }
    entry_count = reachable.count;
    if (entry_count > 0U) {
        entries = (GitLogEntry *)rt_malloc_array(entry_count, sizeof(entries[0]));
        if (entries == 0) {
            git_oid_list_destroy(&reachable);
            if (have_pack) git_pack_destroy(&pack);
            return 1;
        }
        rt_memset(entries, 0, entry_count * sizeof(entries[0]));
        for (emitted = 0U; emitted < entry_count; ++emitted) {
            if (have_exclude && git_oid_list_contains(&excluded, reachable.oids[emitted])) {
                entries[emitted].emitted = 1;
                continue;
            }
            memcpy(entries[emitted].oid, reachable.oids[emitted], CRYPTO_SHA1_DIGEST_SIZE);
            if (git_read_commit_info(repo, entries[emitted].oid, have_pack ? &pack : 0, &entries[emitted].info) != 0) {
                git_log_entries_destroy(entries, entry_count);
                git_oid_list_destroy(&reachable);
                if (have_pack) git_pack_destroy(&pack);
                return 1;
            }
            entries[emitted].timestamp = git_commit_sort_timestamp(&entries[emitted].info);
        }
    }
    emitted = 0U;
    if (have_exclude && entries != 0) {
        size_t i;
        for (i = 0U; i < entry_count; ++i) {
            if (entries[i].emitted) {
                emitted += 1U;
            }
        }
    }
    while (count < max_count && emitted < entry_count) {
        GitCommitInfo *info;
        char hex[GIT_OBJECT_HEX_SIZE + 1U];
        size_t selected = git_log_select_next_entry(entries, entry_count);

        if (selected == entry_count) {
            result = 1;
            break;
        }
        entries[selected].emitted = 1;
        emitted += 1U;
        memcpy(oid, entries[selected].oid, CRYPTO_SHA1_DIGEST_SIZE);
        info = &entries[selected].info;
        git_format_oid_hex(oid, hex);
        if (format != 0) {
            if (git_write_commit_format(format, oid, info) != 0) {
                result = 1;
                break;
            }
        } else if (oneline) {
            char short_hex[8];
            memcpy(short_hex, hex, 7U);
            short_hex[7] = '\0';
            rt_write_cstr(1, short_hex);
            rt_write_char(1, ' ');
            git_write_commit_subject_line(info);
        } else {
            rt_write_cstr(1, "commit ");
            rt_write_line(1, hex);
            if (info->author != 0) {
                git_write_author_and_date(info->author);
            }
            rt_write_char(1, '\n');
            rt_write_cstr(1, "    ");
            git_write_commit_subject_line(info);
            rt_write_char(1, '\n');
        }
        count += 1;
    }
    git_log_entries_destroy(entries, entry_count);
    git_oid_list_destroy(&excluded);
    git_oid_list_destroy(&reachable);
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

static int git_rebase_find_merge_base(GitRepo *repo, const unsigned char head[CRYPTO_SHA1_DIGEST_SIZE], const unsigned char upstream[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack, unsigned char base_out[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitOidList upstream_reachable;
    unsigned char current[CRYPTO_SHA1_DIGEST_SIZE];
    int result = -1;

    rt_memset(&upstream_reachable, 0, sizeof(upstream_reachable));
    if (git_collect_reachable_commits(repo, upstream, pack, &upstream_reachable) != 0) {
        return -1;
    }
    memcpy(current, head, CRYPTO_SHA1_DIGEST_SIZE);
    while (1) {
        GitCommitInfo info;

        if (git_oid_list_contains(&upstream_reachable, current)) {
            memcpy(base_out, current, CRYPTO_SHA1_DIGEST_SIZE);
            result = 0;
            break;
        }
        rt_memset(&info, 0, sizeof(info));
        if (git_read_commit_info(repo, current, pack, &info) != 0 || info.parent_count != 1U) {
            git_commit_info_destroy(&info);
            break;
        }
        memcpy(current, info.parents[0], CRYPTO_SHA1_DIGEST_SIZE);
        git_commit_info_destroy(&info);
    }
    git_oid_list_destroy(&upstream_reachable);
    return result;
}

static int git_rebase_collect_first_parent_commits(GitRepo *repo, const unsigned char head[CRYPTO_SHA1_DIGEST_SIZE], const unsigned char base[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack, GitOidList *commits) {
    unsigned char current[CRYPTO_SHA1_DIGEST_SIZE];

    rt_memset(commits, 0, sizeof(*commits));
    memcpy(current, head, CRYPTO_SHA1_DIGEST_SIZE);
    while (!git_oid_equal(current, base)) {
        GitCommitInfo info;

        if (git_oid_list_push(commits, current) != 0) {
            git_oid_list_destroy(commits);
            return -1;
        }
        rt_memset(&info, 0, sizeof(info));
        if (git_read_commit_info(repo, current, pack, &info) != 0 || info.parent_count != 1U) {
            git_commit_info_destroy(&info);
            git_oid_list_destroy(commits);
            return -1;
        }
        memcpy(current, info.parents[0], CRYPTO_SHA1_DIGEST_SIZE);
        git_commit_info_destroy(&info);
    }
    return 0;
}

static int git_rebase_index_entries_equal(const GitIndexEntry *left, const GitIndexEntry *right) {
    if (left == 0 || right == 0) {
        return left == right;
    }
    return left->mode == right->mode && left->intent_to_add == right->intent_to_add && left->size == right->size && git_oid_equal(left->oid, right->oid);
}

static int git_rebase_copy_entry(const GitIndexEntry *entry, GitIndexEntry *copy) {
    *copy = *entry;
    copy->path = git_strdup_n(entry->path, rt_strlen(entry->path));
    return copy->path == 0 ? -1 : 0;
}

static int git_rebase_apply_entry(GitIndex *index, const GitIndexEntry *entry) {
    GitIndexEntry copy;

    if (git_rebase_copy_entry(entry, &copy) != 0) {
        return -1;
    }
    if (git_index_replace_or_insert(index, &copy) != 0) {
        rt_free(copy.path);
        return -1;
    }
    return 0;
}

static void git_rebase_remove_entry(GitIndex *index, const char *path) {
    size_t position = 0U;

    if (git_index_find_position(index, path, &position) == 0) {
        git_index_remove_at(index, position);
    }
}

static int git_rebase_apply_commit_delta(GitRepo *repo, const unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE], const unsigned char new_parent_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack, unsigned char new_commit_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitCommitInfo info;
    GitIndex base_index;
    GitIndex commit_index;
    GitIndex rebased_index;
    unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char parent_tree_oid[CRYPTO_SHA1_DIGEST_SIZE];
    size_t base_pos = 0U;
    size_t commit_pos = 0U;
    int result = -1;

    rt_memset(&info, 0, sizeof(info));
    rt_memset(&base_index, 0, sizeof(base_index));
    rt_memset(&commit_index, 0, sizeof(commit_index));
    rt_memset(&rebased_index, 0, sizeof(rebased_index));
    if (git_read_commit_info(repo, commit_oid, pack, &info) != 0 || info.parent_count != 1U ||
        git_load_commit_tree_index(repo, info.parents[0], pack, &base_index) != 0 ||
        git_load_commit_tree_index(repo, commit_oid, pack, &commit_index) != 0 ||
        git_load_commit_tree_index(repo, new_parent_oid, pack, &rebased_index) != 0) {
        goto done;
    }
    while (base_pos < base_index.count || commit_pos < commit_index.count) {
        GitIndexEntry *base_entry = base_pos < base_index.count ? &base_index.entries[base_pos] : 0;
        GitIndexEntry *commit_entry = commit_pos < commit_index.count ? &commit_index.entries[commit_pos] : 0;
        int cmp;

        if (base_entry == 0) {
            cmp = 1;
        } else if (commit_entry == 0) {
            cmp = -1;
        } else {
            cmp = rt_strcmp(base_entry->path, commit_entry->path);
        }
        if (cmp == 0) {
            if (!git_rebase_index_entries_equal(base_entry, commit_entry)) {
                GitIndexEntry *rebased_entry = git_index_find(&rebased_index, commit_entry->path);

                if (!git_rebase_index_entries_equal(base_entry, rebased_entry) && !git_rebase_index_entries_equal(commit_entry, rebased_entry)) {
                    result = -2;
                    goto done;
                }
                if (git_rebase_apply_entry(&rebased_index, commit_entry) != 0) {
                    goto done;
                }
            }
            base_pos += 1U;
            commit_pos += 1U;
        } else if (cmp < 0) {
            GitIndexEntry *rebased_entry = git_index_find(&rebased_index, base_entry->path);

            if (!git_rebase_index_entries_equal(base_entry, rebased_entry) && rebased_entry != 0) {
                result = -2;
                goto done;
            }
            git_rebase_remove_entry(&rebased_index, base_entry->path);
            base_pos += 1U;
        } else {
            GitIndexEntry *rebased_entry = git_index_find(&rebased_index, commit_entry->path);

            if (rebased_entry != 0 && !git_rebase_index_entries_equal(commit_entry, rebased_entry)) {
                result = -2;
                goto done;
            }
            if (git_rebase_apply_entry(&rebased_index, commit_entry) != 0) {
                goto done;
            }
            commit_pos += 1U;
        }
    }
    if (git_write_tree_from_index(repo, &rebased_index, tree_oid) != 0 || git_commit_tree_oid(repo, new_parent_oid, pack, parent_tree_oid) != 0) {
        goto done;
    }
    if (git_oid_equal(tree_oid, parent_tree_oid)) {
        memcpy(new_commit_oid, new_parent_oid, CRYPTO_SHA1_DIGEST_SIZE);
    } else if (git_write_commit_from_tree_with_parent(repo, tree_oid, new_parent_oid, info.message != 0 && info.message[0] != '\0' ? info.message : "rebase", new_commit_oid) != 0) {
        goto done;
    }
    result = 0;
done:
    git_index_destroy(&rebased_index);
    git_index_destroy(&commit_index);
    git_index_destroy(&base_index);
    git_commit_info_destroy(&info);
    return result;
}

static int git_cmd_rebase(GitRepo *repo, int argc, char **argv, int argi) {
    GitPack pack;
    GitOidList commits;
    unsigned char head_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char upstream_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char base_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char new_parent_oid[CRYPTO_SHA1_DIGEST_SIZE];
    char new_hex[GIT_OBJECT_HEX_SIZE + 1U];
    const char *upstream;
    int have_pack;
    size_t i;
    int result = 1;

    if (argi < argc && rt_strcmp(argv[argi], "--") == 0) {
        argi += 1;
    }
    if (argi + 1 != argc) {
        tool_write_error("git", "rebase needs one revision", 0);
        return 1;
    }
    upstream = argv[argi];
    if (repo->head_oid[0] == '\0' || git_parse_oid_hex(repo->head_oid, head_oid) != 0 || git_resolve_revision(repo, upstream, upstream_oid, 0, 0) != 0) {
        tool_write_error("git", "cannot resolve rebase revision: ", upstream);
        return 1;
    }
    have_pack = git_load_pack_cache(repo, &pack) == 0;
    if (git_oid_equal(head_oid, upstream_oid) || git_commit_is_ancestor_of(repo, upstream_oid, head_oid, have_pack ? &pack : 0)) {
        rt_write_line(1, "Current branch is up to date.");
        result = 0;
        goto done_pack;
    }
    if (git_commit_is_ancestor_of(repo, head_oid, upstream_oid, have_pack ? &pack : 0)) {
        result = git_fast_forward_to_oid(repo, upstream_oid, have_pack ? &pack : 0, upstream, "rebase");
        goto done_pack;
    }
    if (git_rebase_find_merge_base(repo, head_oid, upstream_oid, have_pack ? &pack : 0, base_oid) != 0 ||
        git_rebase_collect_first_parent_commits(repo, head_oid, base_oid, have_pack ? &pack : 0, &commits) != 0) {
        tool_write_error("git", "rebase supports linear first-parent branches only", 0);
        goto done_pack;
    }
    memcpy(new_parent_oid, upstream_oid, CRYPTO_SHA1_DIGEST_SIZE);
    i = commits.count;
    while (i > 0U) {
        unsigned char rebased_oid[CRYPTO_SHA1_DIGEST_SIZE];
        int replay_result;

        i -= 1U;
        replay_result = git_rebase_apply_commit_delta(repo, commits.oids[i], new_parent_oid, have_pack ? &pack : 0, rebased_oid);
        if (replay_result != 0) {
            tool_write_error("git", replay_result == -2 ? "rebase conflict on changed path" : "cannot replay commit during rebase", 0);
            git_oid_list_destroy(&commits);
            goto done_pack;
        }
        memcpy(new_parent_oid, rebased_oid, CRYPTO_SHA1_DIGEST_SIZE);
    }
    git_oid_list_destroy(&commits);
    if (git_checkout_commit_to_worktree(repo, new_parent_oid, have_pack ? &pack : 0) != 0 || git_update_head_to_oid(repo, new_parent_oid, "rebase") != 0) {
        tool_write_error("git", "cannot update branch after rebase", 0);
        goto done_pack;
    }
    git_format_oid_hex(new_parent_oid, new_hex);
    rt_write_cstr(1, "Successfully rebased and updated ");
    rt_write_line(1, new_hex);
    result = 0;
done_pack:
    if (have_pack) git_pack_destroy(&pack);
    return result;
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

