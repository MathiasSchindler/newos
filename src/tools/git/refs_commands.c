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

static int git_delete_packed_ref(const GitRepo *repo, const char *ref_name, int *deleted_out) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;
    GitBuffer out;
    int deleted = 0;
    int result = -1;

    *deleted_out = 0;
    rt_memset(&out, 0, sizeof(out));
    if (git_join(path, sizeof(path), repo->git_dir, "packed-refs") != 0 || git_read_file(path, &data, &size) != 0) {
        return 0;
    }
    while (pos < size) {
        size_t start = pos;
        size_t end;
        size_t line_end;
        int matches = 0;

        while (pos < size && data[pos] != '\n') pos += 1U;
        end = pos;
        if (pos < size) pos += 1U;
        line_end = pos;
        while (end > start && data[end - 1U] == '\r') end -= 1U;
        if (end > start + GIT_OBJECT_HEX_SIZE + 1U && data[start] != '#' && data[start] != '^' && data[start + GIT_OBJECT_HEX_SIZE] == ' ') {
            size_t ref_len = end - start - GIT_OBJECT_HEX_SIZE - 1U;

            if (ref_len == rt_strlen(ref_name) && memcmp(data + start + GIT_OBJECT_HEX_SIZE + 1U, ref_name, ref_len) == 0) {
                matches = 1;
            }
        }
        if (matches) {
            deleted = 1;
            if (pos < size && data[pos] == '^') {
                while (pos < size && data[pos] != '\n') pos += 1U;
                if (pos < size) pos += 1U;
            }
            continue;
        }
        if (git_buffer_append(&out, data + start, line_end - start) != 0) {
            goto done;
        }
    }
    if (deleted && git_write_all_file(path, out.data, out.size, 0644U) != 0) {
        goto done;
    }
    *deleted_out = deleted;
    result = 0;
done:
    git_buffer_destroy(&out);
    rt_free(data);
    return result;
}

typedef struct {
    char *name;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
} GitDescribeTag;

typedef struct {
    GitDescribeTag *tags;
    size_t count;
    size_t capacity;
} GitDescribeTagList;

static void git_describe_tag_list_destroy(GitDescribeTagList *list) {
    size_t i;

    if (list == 0) {
        return;
    }
    for (i = 0U; i < list->count; ++i) {
        rt_free(list->tags[i].name);
    }
    rt_free(list->tags);
    rt_memset(list, 0, sizeof(*list));
}

static int git_describe_tag_list_push(GitDescribeTagList *list, const char *name, size_t name_length, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitDescribeTag *new_tags;
    char *copy;
    size_t new_capacity;

    if (list->count == list->capacity) {
        new_capacity = list->capacity == 0U ? 32U : list->capacity * 2U;
        new_tags = (GitDescribeTag *)rt_realloc_array(list->tags, new_capacity, sizeof(list->tags[0]));
        if (new_tags == 0) {
            return -1;
        }
        list->tags = new_tags;
        list->capacity = new_capacity;
    }
    copy = git_strdup_n(name, name_length);
    if (copy == 0) {
        return -1;
    }
    list->tags[list->count].name = copy;
    memcpy(list->tags[list->count].oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
    list->count += 1U;
    return 0;
}

static int git_describe_collect_loose_tags(const GitRepo *repo, const char *relative, GitDescribeTagList *list) {
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
            if (git_describe_collect_loose_tags(repo, child, list) != 0) return -1;
        } else {
            char oid_hex[GIT_OBJECT_HEX_SIZE + 1U];
            unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];

            if (git_read_ref_file(repo, child, oid_hex, sizeof(oid_hex)) == 0 && git_parse_oid_hex(oid_hex, oid) == 0) {
                if (git_describe_tag_list_push(list, child + 10U, rt_strlen(child + 10U), oid) != 0) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

static int git_describe_collect_packed_tags(const GitRepo *repo, GitDescribeTagList *list) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    size_t pos = 0U;
    int result = -1;

    if (git_join(path, sizeof(path), repo->git_dir, "packed-refs") != 0 || git_read_file(path, &data, &size) != 0) {
        return 0;
    }
    while (pos < size) {
        size_t start = pos;
        size_t end;

        while (pos < size && data[pos] != '\n') pos += 1U;
        end = pos;
        if (pos < size) pos += 1U;
        while (end > start && data[end - 1U] == '\r') end -= 1U;
        if (end > start + GIT_OBJECT_HEX_SIZE + 11U && data[start] != '#' && data[start] != '^' && data[start + GIT_OBJECT_HEX_SIZE] == ' ' && memcmp(data + start + GIT_OBJECT_HEX_SIZE + 1U, "refs/tags/", 10U) == 0) {
            unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
            size_t name_start = start + GIT_OBJECT_HEX_SIZE + 11U;

            if (git_parse_oid_hex_n((const char *)data + start, GIT_OBJECT_HEX_SIZE, oid) == 0 && git_describe_tag_list_push(list, (const char *)data + name_start, end - name_start, oid) != 0) {
                goto done;
            }
        }
    }
    result = 0;
done:
    rt_free(data);
    return result;
}

static int git_cmd_tag(GitRepo *repo, int argc, char **argv, int argi) {
    if (argi < argc && (rt_strcmp(argv[argi], "-d") == 0 || rt_strcmp(argv[argi], "--delete") == 0)) {
        int result = 0;

        argi += 1;
        if (argi >= argc) {
            tool_write_error("git", "tag delete needs a name", 0);
            return 1;
        }
        while (argi < argc) {
            char ref_name[GIT_REF_CAPACITY];
            char ref_path[GIT_PATH_CAPACITY];
            PlatformDirEntry info;
            int deleted = 0;
            int packed_deleted = 0;

            if (tool_path_is_unsafe_relative(argv[argi]) || git_copy(ref_name, sizeof(ref_name), "refs/tags/") != 0 || rt_strlen(ref_name) + rt_strlen(argv[argi]) >= sizeof(ref_name)) {
                tool_write_error("git", "bad tag name: ", argv[argi]);
                result = 1;
                argi += 1;
                continue;
            }
            rt_copy_string(ref_name + rt_strlen(ref_name), sizeof(ref_name) - rt_strlen(ref_name), argv[argi]);
            if (git_join(ref_path, sizeof(ref_path), repo->git_dir, ref_name) == 0 && platform_get_path_info(ref_path, &info) == 0 && !info.is_dir) {
                if (platform_remove_file(ref_path) == 0) {
                    deleted = 1;
                }
            }
            if (git_delete_packed_ref(repo, ref_name, &packed_deleted) != 0) {
                result = 1;
            }
            if (!deleted && !packed_deleted) {
                tool_write_error("git", "cannot delete tag: ", argv[argi]);
                result = 1;
            } else {
                rt_write_cstr(1, "Deleted tag ");
                rt_write_line(1, argv[argi]);
            }
            argi += 1;
        }
        return result;
    }
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

