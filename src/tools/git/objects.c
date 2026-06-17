static const char *git_object_type_name(int type) {
    if (type == GIT_OBJECT_COMMIT) {
        return "commit";
    }
    if (type == GIT_OBJECT_TREE) {
        return "tree";
    }
    if (type == GIT_OBJECT_BLOB) {
        return "blob";
    }
    if (type == GIT_OBJECT_TAG) {
        return "tag";
    }
    return "unknown";
}

static int git_object_type_from_name(const char *name, size_t length) {
    if (length == 6U && memcmp(name, "commit", 6U) == 0) {
        return GIT_OBJECT_COMMIT;
    }
    if (length == 4U && memcmp(name, "tree", 4U) == 0) {
        return GIT_OBJECT_TREE;
    }
    if (length == 4U && memcmp(name, "blob", 4U) == 0) {
        return GIT_OBJECT_BLOB;
    }
    if (length == 3U && memcmp(name, "tag", 3U) == 0) {
        return GIT_OBJECT_TAG;
    }
    return 0;
}

static int git_hash_object_data(int type, const unsigned char *data, size_t size, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], GitBuffer *full_out) {
    CryptoSha1Context sha1;
    char size_digits[32];

    if (full_out != 0) {
        rt_memset(full_out, 0, sizeof(*full_out));
    }
    rt_unsigned_to_string(size, size_digits, sizeof(size_digits));
    crypto_sha1_init(&sha1);
    crypto_sha1_update(&sha1, (const unsigned char *)git_object_type_name(type), rt_strlen(git_object_type_name(type)));
    crypto_sha1_update(&sha1, (const unsigned char *)" ", 1U);
    crypto_sha1_update(&sha1, (const unsigned char *)size_digits, rt_strlen(size_digits));
    crypto_sha1_update(&sha1, (const unsigned char *)"\0", 1U);
    crypto_sha1_update(&sha1, data, size);
    crypto_sha1_final(&sha1, oid);
    if (full_out != 0) {
        if (git_buffer_append_cstr(full_out, git_object_type_name(type)) != 0 ||
            git_buffer_append_char(full_out, ' ') != 0 ||
            git_buffer_append_cstr(full_out, size_digits) != 0 ||
            git_buffer_append_char(full_out, '\0') != 0 ||
            git_buffer_append(full_out, data, size) != 0) {
            git_buffer_destroy(full_out);
            return -1;
        }
    }
    return 0;
}

static int git_object_path(const GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], char *path, size_t path_size, int want_dir) {
    char hex[GIT_OBJECT_HEX_SIZE + 1U];
    char dir[GIT_PATH_CAPACITY];
    char tail[GIT_OBJECT_HEX_SIZE];

    git_format_oid_hex(oid, hex);
    tail[0] = hex[2];
    memcpy(tail, hex + 2U, GIT_OBJECT_HEX_SIZE - 2U);
    tail[GIT_OBJECT_HEX_SIZE - 2U] = '\0';
    if (git_join(dir, sizeof(dir), repo->git_dir, "objects") != 0) {
        return -1;
    }
    if (git_join(dir, sizeof(dir), dir, "xx") != 0) {
        return -1;
    }
    dir[rt_strlen(dir) - 2U] = hex[0];
    dir[rt_strlen(dir) - 1U] = hex[1];
    if (want_dir) {
        return git_copy(path, path_size, dir);
    }
    return git_join(path, path_size, dir, tail);
}

static int git_read_loose_object(const GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], int *type_out, unsigned char **data_out, size_t *size_out) {
    char path[GIT_PATH_CAPACITY];
    unsigned char *compressed = 0;
    size_t compressed_size = 0U;
    unsigned char *full = 0;
    size_t full_capacity;
    size_t full_size = 0U;
    size_t pos = 0U;
    size_t type_start;
    size_t type_length;
    size_t parsed_size = 0U;

    if (git_object_path(repo, oid, path, sizeof(path), 0) != 0 || git_read_file(path, &compressed, &compressed_size) != 0) {
        return -1;
    }
    full_capacity = compressed_size * 4U + 1024U;
    if (full_capacity < 4096U) {
        full_capacity = 4096U;
    }
    while (full_capacity <= GIT_MAX_OBJECT_SIZE) {
        full = (unsigned char *)rt_malloc(full_capacity);
        if (full == 0) {
            rt_free(compressed);
            return -1;
        }
        if (compression_zlib_inflate(compressed, compressed_size, full, full_capacity, &full_size) == 0) {
            break;
        }
        rt_free(full);
        full = 0;
        full_capacity *= 2U;
    }
    rt_free(compressed);
    if (full == 0) {
        return -1;
    }
    type_start = 0U;
    while (pos < full_size && full[pos] != ' ') {
        pos += 1U;
    }
    if (pos >= full_size) {
        rt_free(full);
        return -1;
    }
    type_length = pos - type_start;
    *type_out = git_object_type_from_name((const char *)full + type_start, type_length);
    if (*type_out == 0) {
        rt_free(full);
        return -1;
    }
    pos += 1U;
    while (pos < full_size && full[pos] >= '0' && full[pos] <= '9') {
        parsed_size = parsed_size * 10U + (size_t)(full[pos] - '0');
        pos += 1U;
    }
    if (pos >= full_size || full[pos] != '\0' || parsed_size != full_size - pos - 1U) {
        rt_free(full);
        return -1;
    }
    pos += 1U;
    *data_out = (unsigned char *)rt_malloc(parsed_size == 0U ? 1U : parsed_size);
    if (*data_out == 0) {
        rt_free(full);
        return -1;
    }
    memcpy(*data_out, full + pos, parsed_size);
    *size_out = parsed_size;
    rt_free(full);
    return 0;
}

static int git_pack_object_copy(const GitPackObject *object, int *type_out, unsigned char **data_out, size_t *size_out) {
    unsigned char *copy;

    if (!object->resolved || object->type < GIT_OBJECT_COMMIT || object->type > GIT_OBJECT_TAG) {
        return -1;
    }
    copy = (unsigned char *)rt_malloc(object->size == 0U ? 1U : object->size);
    if (copy == 0) {
        return -1;
    }
    memcpy(copy, object->data, object->size);
    *type_out = object->type;
    *data_out = copy;
    *size_out = object->size;
    return 0;
}

static int git_read_pack_cache_object(const GitPack *pack, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], int *type_out, unsigned char **data_out, size_t *size_out) {
    size_t i;

    if (pack == 0) {
        return -1;
    }
    for (i = 0U; i < pack->count; ++i) {
        if (pack->objects[i].resolved && git_oid_equal(pack->objects[i].oid, oid)) {
            return git_pack_object_copy(&pack->objects[i], type_out, data_out, size_out);
        }
    }
    return -1;
}

static int git_pack_name_is_pack(const char *name) {
    size_t length = rt_strlen(name);

    return length == 50U && rt_strncmp(name, "pack-", 5U) == 0 && rt_strcmp(name + length - 5U, ".pack") == 0;
}

static int git_read_pack_file_object(const char *path, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], int *type_out, unsigned char **data_out, size_t *size_out) {
    unsigned char *pack_data = 0;
    size_t pack_size = 0U;
    GitPack pack;
    int result = -1;

    rt_memset(&pack, 0, sizeof(pack));
    if (git_read_file(path, &pack_data, &pack_size) != 0) {
        return -1;
    }
    if (git_parse_pack(pack_data, pack_size, &pack) == 0 && git_resolve_pack_deltas(&pack) == 0) {
        result = git_read_pack_cache_object(&pack, oid, type_out, data_out, size_out);
    }
    git_pack_destroy(&pack);
    rt_free(pack_data);
    return result;
}

static int git_read_packed_object(const GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], int *type_out, unsigned char **data_out, size_t *size_out) {
    char pack_dir[GIT_PATH_CAPACITY];
    char pack_path[GIT_PATH_CAPACITY];
    PlatformDirEntry entries[128];
    size_t count = 0U;
    int is_directory = 0;
    size_t i;

    if (git_join(pack_dir, sizeof(pack_dir), repo->git_dir, "objects/pack") != 0 ||
        platform_collect_entries(pack_dir, 0, entries, sizeof(entries) / sizeof(entries[0]), &count, &is_directory) != 0 || !is_directory) {
        return -1;
    }
    for (i = 0U; i < count; ++i) {
        if (!entries[i].is_dir && git_pack_name_is_pack(entries[i].name)) {
            if (git_join(pack_path, sizeof(pack_path), pack_dir, entries[i].name) == 0 && git_read_pack_file_object(pack_path, oid, type_out, data_out, size_out) == 0) {
                return 0;
            }
        }
    }
    return -1;
}

static int git_load_pack_cache(const GitRepo *repo, GitPack *pack) {
    char pack_dir[GIT_PATH_CAPACITY];
    char pack_path[GIT_PATH_CAPACITY];
    PlatformDirEntry entries[128];
    size_t count = 0U;
    int is_directory = 0;
    size_t i;

    rt_memset(pack, 0, sizeof(*pack));
    if (git_join(pack_dir, sizeof(pack_dir), repo->git_dir, "objects/pack") != 0 ||
        platform_collect_entries(pack_dir, 0, entries, sizeof(entries) / sizeof(entries[0]), &count, &is_directory) != 0 || !is_directory) {
        return -1;
    }
    for (i = 0U; i < count; ++i) {
        unsigned char *pack_data = 0;
        size_t pack_size = 0U;

        if (entries[i].is_dir || !git_pack_name_is_pack(entries[i].name) || git_join(pack_path, sizeof(pack_path), pack_dir, entries[i].name) != 0) {
            continue;
        }
        if (git_read_file(pack_path, &pack_data, &pack_size) == 0) {
            if (git_parse_pack(pack_data, pack_size, pack) == 0 && git_resolve_pack_deltas(pack) == 0) {
                rt_free(pack_data);
                return 0;
            }
            git_pack_destroy(pack);
            rt_free(pack_data);
        }
    }
    return -1;
}

static int git_read_object(const GitRepo *repo, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, int *type_out, unsigned char **data_out, size_t *size_out) {
    if (git_read_loose_object(repo, oid, type_out, data_out, size_out) == 0) {
        return 0;
    }
    if (git_read_pack_cache_object(pack_cache, oid, type_out, data_out, size_out) == 0) {
        return 0;
    }
    return git_read_packed_object(repo, oid, type_out, data_out, size_out);
}

static int git_write_loose_object(const GitRepo *repo, int type, const unsigned char *data, size_t size, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitBuffer full;
    unsigned char *compressed = 0;
    size_t compressed_capacity;
    size_t compressed_size = 0U;
    char object_dir[GIT_PATH_CAPACITY];
    char object_path[GIT_PATH_CAPACITY];
    PlatformDirEntry existing;
    int result = -1;

    if (git_hash_object_data(type, data, size, oid, &full) != 0) {
        return -1;
    }
    if (git_object_path(repo, oid, object_path, sizeof(object_path), 0) != 0 || git_object_path(repo, oid, object_dir, sizeof(object_dir), 1) != 0) {
        git_buffer_destroy(&full);
        return -1;
    }
    if (platform_get_path_info(object_path, &existing) == 0 && !existing.is_dir) {
        git_buffer_destroy(&full);
        return 0;
    }
    compressed_capacity = compression_zlib_deflate_bound(full.size);
    compressed = (unsigned char *)rt_malloc(compressed_capacity == 0U ? 1U : compressed_capacity);
    if (compressed == 0) {
        goto done;
    }
    if (compression_zlib_deflate_level(full.data, full.size, compressed, compressed_capacity, &compressed_size, 6) != 0) {
        goto done;
    }
    if (git_make_directory_chain(object_dir) != 0 || git_write_all_file(object_path, compressed, compressed_size, 0444U) != 0) {
        goto done;
    }
    result = 0;
done:
    rt_free(compressed);
    git_buffer_destroy(&full);
    return result;
}

static int git_compare_pack_index_entries(const void *left, const void *right) {
    const GitPackIndexEntry *left_entry = (const GitPackIndexEntry *)left;
    const GitPackIndexEntry *right_entry = (const GitPackIndexEntry *)right;

    return memcmp(left_entry->object->oid, right_entry->object->oid, CRYPTO_SHA1_DIGEST_SIZE);
}

static int git_buffer_append_u32_be(GitBuffer *buffer, unsigned int value) {
    unsigned char word[4];

    git_write_u32_be(word, value);
    return git_buffer_append(buffer, word, sizeof(word));
}

static int git_write_pack_index_file(const char *idx_path, const GitPack *pack, const unsigned char *pack_data, size_t pack_size) {
    GitPackIndexEntry *entries;
    GitBuffer index;
    unsigned int fanout[256];
    unsigned char digest[CRYPTO_SHA1_DIGEST_SIZE];
    CryptoSha1Context sha1;
    size_t i;
    int result = -1;

    if (pack == 0 || pack_data == 0 || pack_size < CRYPTO_SHA1_DIGEST_SIZE) {
        return -1;
    }
    entries = (GitPackIndexEntry *)rt_malloc_array(pack->count == 0U ? 1U : pack->count, sizeof(entries[0]));
    if (entries == 0) {
        return -1;
    }
    rt_memset(&index, 0, sizeof(index));
    rt_memset(fanout, 0, sizeof(fanout));
    for (i = 0U; i < pack->count; ++i) {
        if (!pack->objects[i].resolved || pack->objects[i].offset > 0x7fffffffULL) {
            goto done;
        }
        entries[i].object = &pack->objects[i];
        fanout[pack->objects[i].oid[0]] += 1U;
    }
    rt_sort(entries, pack->count, sizeof(entries[0]), git_compare_pack_index_entries);
    for (i = 1U; i < 256U; ++i) {
        fanout[i] += fanout[i - 1U];
    }
    if (git_buffer_append_u32_be(&index, 0xff744f63U) != 0 || git_buffer_append_u32_be(&index, 2U) != 0) {
        goto done;
    }
    for (i = 0U; i < 256U; ++i) {
        if (git_buffer_append_u32_be(&index, fanout[i]) != 0) {
            goto done;
        }
    }
    for (i = 0U; i < pack->count; ++i) {
        if (git_buffer_append(&index, entries[i].object->oid, CRYPTO_SHA1_DIGEST_SIZE) != 0) {
            goto done;
        }
    }
    for (i = 0U; i < pack->count; ++i) {
        if (git_buffer_append_u32_be(&index, entries[i].object->crc32) != 0) {
            goto done;
        }
    }
    for (i = 0U; i < pack->count; ++i) {
        if (git_buffer_append_u32_be(&index, (unsigned int)entries[i].object->offset) != 0) {
            goto done;
        }
    }
    if (git_buffer_append(&index, pack_data + pack_size - CRYPTO_SHA1_DIGEST_SIZE, CRYPTO_SHA1_DIGEST_SIZE) != 0) {
        goto done;
    }
    crypto_sha1_init(&sha1);
    crypto_sha1_update(&sha1, index.data, index.size);
    crypto_sha1_final(&sha1, digest);
    if (git_buffer_append(&index, digest, sizeof(digest)) != 0) {
        goto done;
    }
    result = git_write_all_file(idx_path, index.data, index.size, 0444U);
done:
    git_buffer_destroy(&index);
    rt_free(entries);
    return result;
}

static int git_write_pack_file(const GitRepo *repo, const unsigned char *pack_data, size_t pack_size, const GitPack *pack) {
    char pack_dir[GIT_PATH_CAPACITY];
    char pack_path[GIT_PATH_CAPACITY];
    char idx_path[GIT_PATH_CAPACITY];
    char pack_name[64];
    char hex[GIT_OBJECT_HEX_SIZE + 1U];
    PlatformDirEntry existing;

    if (pack_size < CRYPTO_SHA1_DIGEST_SIZE + 12U) {
        return -1;
    }
    git_format_oid_hex(pack_data + pack_size - CRYPTO_SHA1_DIGEST_SIZE, hex);
    if (git_join(pack_dir, sizeof(pack_dir), repo->git_dir, "objects/pack") != 0 || git_make_directory_chain(pack_dir) != 0) {
        return -1;
    }
    if (git_copy(pack_name, sizeof(pack_name), "pack-") != 0 || rt_strlen(pack_name) + rt_strlen(hex) + 5U >= sizeof(pack_name)) {
        return -1;
    }
    rt_copy_string(pack_name + rt_strlen(pack_name), sizeof(pack_name) - rt_strlen(pack_name), hex);
    rt_copy_string(pack_name + rt_strlen(pack_name), sizeof(pack_name) - rt_strlen(pack_name), ".pack");
    if (git_join(pack_path, sizeof(pack_path), pack_dir, pack_name) != 0) {
        return -1;
    }
    pack_name[rt_strlen(pack_name) - 4U] = 'i';
    pack_name[rt_strlen(pack_name) - 3U] = 'd';
    pack_name[rt_strlen(pack_name) - 2U] = 'x';
    pack_name[rt_strlen(pack_name) - 1U] = '\0';
    if (git_join(idx_path, sizeof(idx_path), pack_dir, pack_name) != 0) {
        return -1;
    }
    if (platform_get_path_info(pack_path, &existing) == 0 && !existing.is_dir) {
        if (platform_get_path_info(idx_path, &existing) == 0 && !existing.is_dir) {
            return 0;
        }
        return git_write_pack_index_file(idx_path, pack, pack_data, pack_size);
    }
    if (git_write_all_file(pack_path, pack_data, pack_size, 0444U) != 0) {
        return -1;
    }
    return git_write_pack_index_file(idx_path, pack, pack_data, pack_size);
}

static int git_pack_push(GitPack *pack, GitPackObject *object) {
    GitPackObject *new_objects;
    size_t new_capacity;

    if (pack->count == pack->capacity) {
        new_capacity = pack->capacity == 0U ? 64U : pack->capacity * 2U;
        new_objects = (GitPackObject *)rt_realloc_array(pack->objects, new_capacity, sizeof(pack->objects[0]));
        if (new_objects == 0) {
            return -1;
        }
        pack->objects = new_objects;
        pack->capacity = new_capacity;
    }
    pack->objects[pack->count++] = *object;
    return 0;
}

static GitPackObject *git_pack_find_offset(GitPack *pack, unsigned long long offset) {
    size_t i;

    for (i = 0U; i < pack->count; ++i) {
        if (pack->objects[i].offset == offset) {
            return &pack->objects[i];
        }
    }
    return 0;
}

static GitPackObject *git_pack_find_oid(GitPack *pack, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    size_t i;

    for (i = 0U; i < pack->count; ++i) {
        if (pack->objects[i].resolved && git_oid_equal(pack->objects[i].oid, oid)) {
            return &pack->objects[i];
        }
    }
    return 0;
}

static int git_read_varint_delta(const unsigned char *data, size_t size, size_t *pos_io, size_t *value_out) {
    size_t shift = 0U;
    size_t value = 0U;

    while (*pos_io < size) {
        unsigned char ch = data[(*pos_io)++];
        value |= ((size_t)(ch & 0x7fU)) << shift;
        if ((ch & 0x80U) == 0U) {
            *value_out = value;
            return 0;
        }
        shift += 7U;
        if (shift >= sizeof(size_t) * 8U) {
            return -1;
        }
    }
    return -1;
}

static int git_apply_delta(const unsigned char *base, size_t base_size, const unsigned char *delta, size_t delta_size, unsigned char **out_data, size_t *out_size) {
    size_t pos = 0U;
    size_t declared_base = 0U;
    size_t result_size = 0U;
    size_t out_pos = 0U;
    unsigned char *out;

    if (git_read_varint_delta(delta, delta_size, &pos, &declared_base) != 0 || git_read_varint_delta(delta, delta_size, &pos, &result_size) != 0 || declared_base != base_size) {
        return -1;
    }
    out = (unsigned char *)rt_malloc(result_size == 0U ? 1U : result_size);
    if (out == 0) {
        return -1;
    }
    while (pos < delta_size) {
        unsigned char op = delta[pos++];

        if ((op & 0x80U) != 0U) {
            size_t copy_offset = 0U;
            size_t copy_size = 0U;
            if ((op & 0x01U) != 0U && pos < delta_size) copy_offset |= (size_t)delta[pos++];
            if ((op & 0x02U) != 0U && pos < delta_size) copy_offset |= (size_t)delta[pos++] << 8U;
            if ((op & 0x04U) != 0U && pos < delta_size) copy_offset |= (size_t)delta[pos++] << 16U;
            if ((op & 0x08U) != 0U && pos < delta_size) copy_offset |= (size_t)delta[pos++] << 24U;
            if ((op & 0x10U) != 0U && pos < delta_size) copy_size |= (size_t)delta[pos++];
            if ((op & 0x20U) != 0U && pos < delta_size) copy_size |= (size_t)delta[pos++] << 8U;
            if ((op & 0x40U) != 0U && pos < delta_size) copy_size |= (size_t)delta[pos++] << 16U;
            if (copy_size == 0U) copy_size = 0x10000U;
            if (copy_offset > base_size || copy_size > base_size - copy_offset || out_pos > result_size || copy_size > result_size - out_pos) {
                rt_free(out);
                return -1;
            }
            memcpy(out + out_pos, base + copy_offset, copy_size);
            out_pos += copy_size;
        } else if (op != 0U) {
            size_t insert_size = (size_t)op;
            if (pos + insert_size > delta_size || out_pos + insert_size > result_size) {
                rt_free(out);
                return -1;
            }
            memcpy(out + out_pos, delta + pos, insert_size);
            pos += insert_size;
            out_pos += insert_size;
        } else {
            rt_free(out);
            return -1;
        }
    }
    if (out_pos != result_size) {
        rt_free(out);
        return -1;
    }
    *out_data = out;
    *out_size = result_size;
    return 0;
}

static int git_parse_pack(const unsigned char *data, size_t size, GitPack *pack) {
    size_t pos = 12U;
    unsigned int version;
    unsigned int count;
    unsigned int i;

    rt_memset(pack, 0, sizeof(*pack));
    if (size < 12U || memcmp(data, "PACK", 4U) != 0) {
        return -1;
    }
    version = git_read_u32_be_raw(data + 4U);
    count = git_read_u32_be_raw(data + 8U);
    if (version != 2U && version != 3U) {
        return -1;
    }
    for (i = 0U; i < count; ++i) {
        GitPackObject object;
        unsigned char byte;
        unsigned int shift = 4U;
        size_t object_size;
        size_t object_start;
        size_t consumed = 0U;

        if (pos >= size) {
            git_pack_destroy(pack);
            return -1;
        }
        rt_memset(&object, 0, sizeof(object));
        object.offset = (unsigned long long)pos;
        object_start = pos;
        byte = data[pos++];
        object.type = (byte >> 4) & 7U;
        object_size = (size_t)(byte & 0x0fU);
        while ((byte & 0x80U) != 0U) {
            if (pos >= size) {
                git_pack_destroy(pack);
                return -1;
            }
            byte = data[pos++];
            object_size |= ((size_t)(byte & 0x7fU)) << shift;
            shift += 7U;
        }
        object.size = object_size;
        if (object.type == GIT_OBJECT_OFS_DELTA) {
            unsigned long long offset_value;
            if (pos >= size) {
                git_pack_destroy(pack);
                return -1;
            }
            byte = data[pos++];
            offset_value = (unsigned long long)(byte & 0x7fU);
            while ((byte & 0x80U) != 0U) {
                if (pos >= size) {
                    git_pack_destroy(pack);
                    return -1;
                }
                byte = data[pos++];
                offset_value = ((offset_value + 1ULL) << 7) | (unsigned long long)(byte & 0x7fU);
            }
            object.base_offset = object.offset - offset_value;
        } else if (object.type == GIT_OBJECT_REF_DELTA) {
            if (pos + CRYPTO_SHA1_DIGEST_SIZE > size) {
                git_pack_destroy(pack);
                return -1;
            }
            memcpy(object.base_oid, data + pos, CRYPTO_SHA1_DIGEST_SIZE);
            pos += CRYPTO_SHA1_DIGEST_SIZE;
        }
        object.data = (unsigned char *)rt_malloc(object_size == 0U ? 1U : object_size);
        if (object.data == 0 || compression_zlib_inflate_consumed(data + pos, size - pos, object.data, object_size, &object.size, &consumed) != 0 || object.size != object_size) {
            rt_free(object.data);
            git_pack_destroy(pack);
            return -1;
        }
        pos += consumed;
        object.crc32 = compression_crc32(data + object_start, pos - object_start);
        if (object.type >= GIT_OBJECT_COMMIT && object.type <= GIT_OBJECT_TAG) {
            if (git_hash_object_data(object.type, object.data, object.size, object.oid, 0) != 0) {
                rt_free(object.data);
                git_pack_destroy(pack);
                return -1;
            }
            object.resolved = 1;
        }
        if (git_pack_push(pack, &object) != 0) {
            rt_free(object.data);
            git_pack_destroy(pack);
            return -1;
        }
    }
    return 0;
}

static int git_resolve_pack_deltas(GitPack *pack) {
    int progress = 1;

    while (progress) {
        size_t i;
        progress = 0;
        for (i = 0U; i < pack->count; ++i) {
            GitPackObject *object = &pack->objects[i];
            GitPackObject *base;
            unsigned char *resolved_data = 0;
            size_t resolved_size = 0U;

            if (object->resolved || (object->type != GIT_OBJECT_OFS_DELTA && object->type != GIT_OBJECT_REF_DELTA)) {
                continue;
            }
            base = object->type == GIT_OBJECT_OFS_DELTA ? git_pack_find_offset(pack, object->base_offset) : git_pack_find_oid(pack, object->base_oid);
            if (base == 0 || !base->resolved) {
                continue;
            }
            if (git_apply_delta(base->data, base->size, object->data, object->size, &resolved_data, &resolved_size) != 0) {
                return -1;
            }
            rt_free(object->data);
            object->data = resolved_data;
            object->size = resolved_size;
            object->type = base->type;
            if (git_hash_object_data(object->type, object->data, object->size, object->oid, 0) != 0) {
                return -1;
            }
            object->resolved = 1;
            progress = 1;
        }
    }
    {
        size_t i;
        for (i = 0U; i < pack->count; ++i) {
            if (!pack->objects[i].resolved) {
                return -1;
            }
        }
    }
    return 0;
}

static int git_commit_tree_oid(const GitRepo *repo, const unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    int type = 0;
    unsigned char *data = 0;
    size_t size = 0U;
    int result = -1;

    if (git_read_object(repo, commit_oid, pack_cache, &type, &data, &size) != 0 || type != GIT_OBJECT_COMMIT) {
        rt_free(data);
        return -1;
    }
    if (size >= 46U && memcmp(data, "tree ", 5U) == 0 && git_parse_oid_hex_n((const char *)data + 5U, GIT_OBJECT_HEX_SIZE, tree_oid) == 0) {
        result = 0;
    }
    rt_free(data);
    return result;
}

static int git_commit_info_add_parent(GitCommitInfo *info, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    unsigned char (*new_parents)[CRYPTO_SHA1_DIGEST_SIZE];
    size_t new_capacity;

    if (info->parent_count == info->parent_capacity) {
        new_capacity = info->parent_capacity == 0U ? 2U : info->parent_capacity * 2U;
        new_parents = (unsigned char (*)[CRYPTO_SHA1_DIGEST_SIZE])rt_realloc_array(info->parents, new_capacity, sizeof(info->parents[0]));
        if (new_parents == 0) {
            return -1;
        }
        info->parents = new_parents;
        info->parent_capacity = new_capacity;
    }
    memcpy(info->parents[info->parent_count], oid, CRYPTO_SHA1_DIGEST_SIZE);
    if (!info->has_parent) {
        memcpy(info->parent_oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
        info->has_parent = 1;
    }
    info->parent_count += 1U;
    return 0;
}

static int git_parse_commit_info(const unsigned char *data, size_t size, GitCommitInfo *info) {
    size_t pos = 0U;
    size_t message_start = size;

    rt_memset(info, 0, sizeof(*info));
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
        if (end == start) {
            message_start = pos;
            break;
        }
        if (end >= start + 5U && memcmp(data + start, "tree ", 5U) == 0) {
            if (git_parse_oid_hex_n((const char *)data + start + 5U, GIT_OBJECT_HEX_SIZE, info->tree_oid) != 0) {
                git_commit_info_destroy(info);
                return -1;
            }
        } else if (end >= start + 7U && memcmp(data + start, "parent ", 7U) == 0) {
            unsigned char parent_oid[CRYPTO_SHA1_DIGEST_SIZE];

            if (git_parse_oid_hex_n((const char *)data + start + 7U, GIT_OBJECT_HEX_SIZE, parent_oid) != 0 || git_commit_info_add_parent(info, parent_oid) != 0) {
                git_commit_info_destroy(info);
                return -1;
            }
        } else if (end >= start + 7U && memcmp(data + start, "author ", 7U) == 0) {
            info->author = git_strdup_n((const char *)data + start + 7U, end - start - 7U);
            if (info->author == 0) {
                git_commit_info_destroy(info);
                return -1;
            }
        } else if (end >= start + 10U && memcmp(data + start, "committer ", 10U) == 0) {
            info->committer = git_strdup_n((const char *)data + start + 10U, end - start - 10U);
            if (info->committer == 0) {
                git_commit_info_destroy(info);
                return -1;
            }
        }
    }
    if (message_start <= size) {
        info->message = git_strdup_n((const char *)data + message_start, size - message_start);
    } else {
        info->message = git_strdup_n("", 0U);
    }
    if (info->message == 0) {
        git_commit_info_destroy(info);
        return -1;
    }
    return 0;
}

static int git_read_commit_info(const GitRepo *repo, const unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitCommitInfo *info) {
    int type = 0;
    unsigned char *data = 0;
    size_t size = 0U;
    int result;

    if (git_read_object(repo, commit_oid, pack_cache, &type, &data, &size) != 0 || type != GIT_OBJECT_COMMIT) {
        rt_free(data);
        return -1;
    }
    result = git_parse_commit_info(data, size, info);
    rt_free(data);
    return result;
}

static int git_tree_index_append_blob_entry(GitIndex *index, const char *relative, unsigned int mode, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], size_t size) {
    GitIndexEntry entry;

    rt_memset(&entry, 0, sizeof(entry));
    entry.path = git_strdup_n(relative, rt_strlen(relative));
    if (entry.path == 0) {
        return -1;
    }
    entry.mode = mode;
    entry.size = size;
    memcpy(entry.oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
    if (git_index_push(index, &entry) != 0) {
        rt_free(entry.path);
        return -1;
    }
    return 0;
}

static int git_read_tree_index_recursive(const GitRepo *repo, const unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, const char *prefix, GitIndex *index) {
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
        char relative[GIT_PATH_CAPACITY];
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];

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
        while (pos < tree_size && tree[pos] != '\0') {
            pos += 1U;
        }
        if (pos >= tree_size || pos + 1U + CRYPTO_SHA1_DIGEST_SIZE > tree_size) {
            rt_free(tree);
            return -1;
        }
        name_length = pos - name_start;
        pos += 1U;
        memcpy(oid, tree + pos, CRYPTO_SHA1_DIGEST_SIZE);
        pos += CRYPTO_SHA1_DIGEST_SIZE;
        if (prefix[0] != '\0') {
            if (rt_strlen(prefix) + 1U + name_length >= sizeof(relative)) {
                rt_free(tree);
                return -1;
            }
            rt_copy_string(relative, sizeof(relative), prefix);
            relative[rt_strlen(relative) + 1U] = '\0';
            relative[rt_strlen(relative)] = '/';
            memcpy(relative + rt_strlen(prefix) + 1U, tree + name_start, name_length);
            relative[rt_strlen(prefix) + 1U + name_length] = '\0';
        } else {
            if (name_length >= sizeof(relative)) {
                rt_free(tree);
                return -1;
            }
            memcpy(relative, tree + name_start, name_length);
            relative[name_length] = '\0';
        }
        if (tool_path_is_unsafe_relative(relative)) {
            rt_free(tree);
            return -1;
        }
        if (mode == GIT_MODE_TREE || mode == 040000U) {
            if (git_read_tree_index_recursive(repo, oid, pack_cache, relative, index) != 0) {
                rt_free(tree);
                return -1;
            }
        } else if ((mode & GIT_MODE_TYPE_MASK) == GIT_MODE_REGULAR_TYPE || mode == 0100644U || mode == 0100755U || mode == GIT_MODE_SYMLINK || mode == 0120000U) {
            int blob_type = 0;
            unsigned char *blob = 0;
            size_t blob_size = 0U;
            unsigned int index_mode = mode == GIT_MODE_SYMLINK || mode == 0120000U ? GIT_MODE_SYMLINK : ((mode & GIT_MODE_EXEC_BITS) != 0U ? GIT_MODE_REGULAR_EXECUTABLE : GIT_MODE_REGULAR_FILE);

            if (git_read_object(repo, oid, pack_cache, &blob_type, &blob, &blob_size) != 0 || blob_type != GIT_OBJECT_BLOB) {
                rt_free(blob);
                rt_free(tree);
                return -1;
            }
            rt_free(blob);
            if (git_tree_index_append_blob_entry(index, relative, index_mode, oid, blob_size) != 0) {
                rt_free(tree);
                return -1;
            }
        }
    }
    rt_free(tree);
    return 0;
}

static int git_load_commit_tree_index(const GitRepo *repo, const unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, GitIndex *index) {
    unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE];

    rt_memset(index, 0, sizeof(*index));
    if (git_commit_tree_oid(repo, commit_oid, pack_cache, tree_oid) != 0) {
        return -1;
    }
    if (git_read_tree_index_recursive(repo, tree_oid, pack_cache, "", index) != 0) {
        git_index_destroy(index);
        return -1;
    }
    if (index->count > 1U && !git_index_is_sorted(index)) {
        rt_sort(index->entries, index->count, sizeof(index->entries[0]), git_compare_entries_by_path);
    }
    return 0;
}

static int git_write_index_from_commit(const GitRepo *repo, const unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache) {
    GitIndex index;
    int result;

    if (git_load_commit_tree_index(repo, commit_oid, pack_cache, &index) != 0) {
        return -1;
    }
    result = git_write_index_file(repo, &index);
    git_index_destroy(&index);
    return result;
}

static int git_load_head_tree_index(const GitRepo *repo, const GitPack *pack_cache, GitIndex *index) {
    unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE];

    if (repo->head_oid[0] == '\0' || git_parse_oid_hex(repo->head_oid, commit_oid) != 0) {
        rt_memset(index, 0, sizeof(*index));
        return -1;
    }
    return git_load_commit_tree_index(repo, commit_oid, pack_cache, index);
}

static const char *git_tree_mode_text(unsigned int mode) {
    if (mode == GIT_MODE_TREE || mode == 040000U) {
        return "40000";
    }
    if (mode == GIT_MODE_SYMLINK || mode == 0120000U) {
        return "120000";
    }
    if ((mode & GIT_MODE_EXEC_BITS) != 0U) {
        return "100755";
    }
    return "100644";
}

static int git_tree_buffer_append_entry(GitBuffer *tree, unsigned int mode, const char *name, size_t name_length, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    if (git_buffer_append_cstr(tree, git_tree_mode_text(mode)) != 0 ||
        git_buffer_append_char(tree, ' ') != 0 ||
        git_buffer_append(tree, name, name_length) != 0 ||
        git_buffer_append_char(tree, '\0') != 0 ||
        git_buffer_append(tree, oid, CRYPTO_SHA1_DIGEST_SIZE) != 0) {
        return -1;
    }
    return 0;
}

static size_t git_tree_path_segment_length(const char *text) {
    size_t length = 0U;

    while (text[length] != '\0' && text[length] != '/') {
        length += 1U;
    }
    return length;
}

static int git_tree_range_has_committable_entry(const GitIndex *index, size_t start, size_t end) {
    size_t position;

    for (position = start; position < end; ++position) {
        if (!index->entries[position].intent_to_add) {
            return 1;
        }
    }
    return 0;
}

static int git_write_tree_recursive(const GitRepo *repo, const GitIndex *index, size_t start, size_t end, size_t prefix_length, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    GitBuffer tree;
    size_t position = start;
    int result = -1;

    rt_memset(&tree, 0, sizeof(tree));
    while (position < end) {
        const char *local = index->entries[position].path + prefix_length;
        size_t segment_length = git_tree_path_segment_length(local);

        if (segment_length == 0U) {
            goto done;
        }
        if (local[segment_length] == '/') {
            size_t group_end = position + 1U;
            unsigned char child_oid[CRYPTO_SHA1_DIGEST_SIZE];

            while (group_end < end) {
                const char *next_local = index->entries[group_end].path + prefix_length;
                if (rt_strncmp(local, next_local, segment_length) != 0 || next_local[segment_length] != '/') {
                    break;
                }
                group_end += 1U;
            }
            if (git_tree_range_has_committable_entry(index, position, group_end)) {
                if (git_write_tree_recursive(repo, index, position, group_end, prefix_length + segment_length + 1U, child_oid) != 0 ||
                    git_tree_buffer_append_entry(&tree, GIT_MODE_TREE, local, segment_length, child_oid) != 0) {
                    goto done;
                }
            }
            position = group_end;
        } else {
            if (!index->entries[position].intent_to_add &&
                git_tree_buffer_append_entry(&tree, index->entries[position].mode, local, segment_length, index->entries[position].oid) != 0) {
                goto done;
            }
            position += 1U;
        }
    }
    result = git_write_loose_object(repo, GIT_OBJECT_TREE, tree.data, tree.size, oid);
done:
    git_buffer_destroy(&tree);
    return result;
}

static int git_write_tree_from_index(const GitRepo *repo, GitIndex *index, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    if (index->count > 1U) {
        rt_sort(index->entries, index->count, sizeof(index->entries[0]), git_compare_entries_by_path);
    }
    return git_write_tree_recursive(repo, index, 0U, index->count, 0U, oid);
}

static int git_remove_empty_checkout_parents(const GitRepo *repo, const char *relative) {
    char path[GIT_PATH_CAPACITY];
    size_t root_length = rt_strlen(repo->work_tree);

    if (git_join(path, sizeof(path), repo->work_tree, relative) != 0 || git_path_parent(path) != 0) {
        return -1;
    }
    while (rt_strlen(path) > root_length && rt_strncmp(path, repo->work_tree, root_length) == 0) {
        if (platform_remove_directory(path) != 0) {
            break;
        }
        if (git_path_parent(path) != 0) {
            break;
        }
    }
    return 0;
}

static int git_remove_checkout_absent_paths(const GitRepo *repo, const GitIndex *old_index, const GitIndex *target_index) {
    size_t old_pos = 0U;
    size_t target_pos = 0U;

    while (old_pos < old_index->count) {
        int cmp;

        if (target_pos >= target_index->count) {
            cmp = -1;
        } else {
            cmp = rt_strcmp(old_index->entries[old_pos].path, target_index->entries[target_pos].path);
        }
        if (cmp == 0) {
            old_pos += 1U;
            target_pos += 1U;
        } else if (cmp > 0) {
            target_pos += 1U;
        } else {
            char full_path[GIT_PATH_CAPACITY];

            if (git_join(full_path, sizeof(full_path), repo->work_tree, old_index->entries[old_pos].path) != 0) {
                return -1;
            }
            (void)tool_remove_path(full_path, 1);
            (void)git_remove_empty_checkout_parents(repo, old_index->entries[old_pos].path);
            old_pos += 1U;
        }
    }
    return 0;
}

static int git_index_append_checkout_entry(GitCheckoutIndex *checkout, const char *relative, unsigned int mode, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], size_t size) {
    GitIndexEntry entry;

    rt_memset(&entry, 0, sizeof(entry));
    entry.path = git_strdup_n(relative, rt_strlen(relative));
    if (entry.path == 0) {
        return -1;
    }
    entry.mode = mode;
    entry.size = size;
    memcpy(entry.oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
    if (git_index_push(&checkout->entries, &entry) != 0) {
        rt_free(entry.path);
        return -1;
    }
    return 0;
}

static int git_write_index_entry_to_worktree(const GitRepo *repo, const GitIndexEntry *entry, const GitPack *pack_cache) {
    char full_path[GIT_PATH_CAPACITY];
    unsigned char *data = 0;
    size_t size = 0U;
    int type = 0;
    int result = -1;

    if (entry == 0 || tool_path_is_unsafe_relative(entry->path) || git_join(full_path, sizeof(full_path), repo->work_tree, entry->path) != 0) {
        return -1;
    }
    if (entry->intent_to_add) {
        data = (unsigned char *)rt_malloc(1U);
        if (data == 0) {
            return -1;
        }
        size = 0U;
    } else if (git_read_object(repo, entry->oid, pack_cache, &type, &data, &size) != 0 || type != GIT_OBJECT_BLOB) {
        rt_free(data);
        return -1;
    }
    if (entry->mode == GIT_MODE_SYMLINK) {
        char target[GIT_PATH_CAPACITY];

        if (size >= sizeof(target)) {
            goto done;
        }
        memcpy(target, data, size);
        target[size] = '\0';
        (void)tool_remove_path(full_path, 1);
        if (git_ensure_parent_directory(full_path) == 0 && platform_create_symbolic_link(target, full_path) == 0) {
            result = 0;
        }
    } else if (git_index_mode_is_regular(entry->mode)) {
        char target[GIT_PATH_CAPACITY];

        if (platform_read_symlink(full_path, target, sizeof(target)) == 0) {
            (void)platform_remove_file(full_path);
        }
        if (git_ensure_parent_directory(full_path) == 0 && git_write_all_file(full_path, data, size, git_worktree_mode_from_regular_index(entry->mode)) == 0) {
            (void)platform_change_mode(full_path, git_worktree_mode_from_regular_index(entry->mode));
            result = 0;
        }
    }
done:
    rt_free(data);
    return result;
}

static int git_checkout_tree_recursive(const GitRepo *repo, const unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache, const char *prefix, GitCheckoutIndex *checkout) {
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
        char relative[GIT_PATH_CAPACITY];
        char full_path[GIT_PATH_CAPACITY];
        unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];

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
        while (pos < tree_size && tree[pos] != '\0') {
            pos += 1U;
        }
        if (pos >= tree_size || pos + 1U + CRYPTO_SHA1_DIGEST_SIZE > tree_size) {
            rt_free(tree);
            return -1;
        }
        name_length = pos - name_start;
        pos += 1U;
        memcpy(oid, tree + pos, CRYPTO_SHA1_DIGEST_SIZE);
        pos += CRYPTO_SHA1_DIGEST_SIZE;
        if (prefix[0] != '\0') {
            if (rt_strlen(prefix) + 1U + name_length >= sizeof(relative)) {
                rt_free(tree);
                return -1;
            }
            rt_copy_string(relative, sizeof(relative), prefix);
            relative[rt_strlen(relative) + 1U] = '\0';
            relative[rt_strlen(relative)] = '/';
            memcpy(relative + rt_strlen(prefix) + 1U, tree + name_start, name_length);
            relative[rt_strlen(prefix) + 1U + name_length] = '\0';
        } else {
            if (name_length >= sizeof(relative)) {
                rt_free(tree);
                return -1;
            }
            memcpy(relative, tree + name_start, name_length);
            relative[name_length] = '\0';
        }
        if (tool_path_is_unsafe_relative(relative) || git_join(full_path, sizeof(full_path), repo->work_tree, relative) != 0) {
            rt_free(tree);
            return -1;
        }
        if (mode == GIT_MODE_TREE || mode == 040000U) {
            if (git_make_directory_chain(full_path) != 0 || git_checkout_tree_recursive(repo, oid, pack_cache, relative, checkout) != 0) {
                rt_free(tree);
                return -1;
            }
        } else if ((mode & GIT_MODE_TYPE_MASK) == GIT_MODE_REGULAR_TYPE || mode == 0100644U || mode == 0100755U) {
            int blob_type = 0;
            unsigned char *blob = 0;
            size_t blob_size = 0U;
            unsigned int index_mode = (mode & GIT_MODE_EXEC_BITS) != 0U ? GIT_MODE_REGULAR_EXECUTABLE : GIT_MODE_REGULAR_FILE;
            char link_target[GIT_PATH_CAPACITY];

            if (git_read_object(repo, oid, pack_cache, &blob_type, &blob, &blob_size) != 0 || blob_type != GIT_OBJECT_BLOB) {
                rt_free(blob);
                rt_free(tree);
                return -1;
            }
            if (platform_read_symlink(full_path, link_target, sizeof(link_target)) == 0) {
                (void)platform_remove_file(full_path);
            }
            if (git_ensure_parent_directory(full_path) != 0 || git_write_all_file(full_path, blob, blob_size, git_worktree_mode_from_regular_index(index_mode)) != 0) {
                rt_free(blob);
                rt_free(tree);
                return -1;
            }
            (void)platform_change_mode(full_path, git_worktree_mode_from_regular_index(index_mode));
            if (git_index_append_checkout_entry(checkout, relative, index_mode, oid, blob_size) != 0) {
                rt_free(blob);
                rt_free(tree);
                return -1;
            }
            rt_free(blob);
        } else if (mode == GIT_MODE_SYMLINK || mode == 0120000U) {
            int blob_type = 0;
            unsigned char *target = 0;
            size_t target_size = 0U;
            char link_target[GIT_PATH_CAPACITY];

            if (git_read_object(repo, oid, pack_cache, &blob_type, &target, &target_size) != 0 || blob_type != GIT_OBJECT_BLOB || target_size >= sizeof(link_target)) {
                rt_free(target);
                rt_free(tree);
                return -1;
            }
            memcpy(link_target, target, target_size);
            link_target[target_size] = '\0';
            (void)platform_remove_file(full_path);
            if (git_ensure_parent_directory(full_path) != 0 || platform_create_symbolic_link(link_target, full_path) != 0) {
                rt_free(target);
                rt_free(tree);
                return -1;
            }
            if (git_index_append_checkout_entry(checkout, relative, GIT_MODE_SYMLINK, oid, target_size) != 0) {
                rt_free(target);
                rt_free(tree);
                return -1;
            }
            rt_free(target);
        } else if (mode == GIT_MODE_GITLINK || mode == 0160000U) {
            continue;
        } else {
            rt_free(tree);
            return -1;
        }
    }
    rt_free(tree);
    return 0;
}

static int git_index_write_entry(GitBuffer *buffer, const GitIndexEntry *entry) {
    unsigned char header[64];
    size_t path_length = rt_strlen(entry->path);
    size_t header_length = 62U;
    size_t entry_length;
    unsigned short flags;
    unsigned short extended_flags = 0U;

    rt_memset(header, 0, sizeof(header));
    git_write_u32_be(header + 24U, entry->mode);
    git_write_u32_be(header + 36U, (unsigned int)entry->size);
    memcpy(header + 40U, entry->oid, CRYPTO_SHA1_DIGEST_SIZE);
    flags = path_length < 0x0fffU ? (unsigned short)path_length : 0x0fffU;
    if (entry->intent_to_add) {
        flags = (unsigned short)(flags | GIT_INDEX_FLAG_EXTENDED);
        extended_flags = GIT_INDEX_EXTENDED_INTENT_TO_ADD;
        header[62] = (unsigned char)(extended_flags >> 8);
        header[63] = (unsigned char)extended_flags;
        header_length = 64U;
    }
    header[60] = (unsigned char)(flags >> 8);
    header[61] = (unsigned char)flags;
    if (git_buffer_append(buffer, header, header_length) != 0 || git_buffer_append(buffer, entry->path, path_length) != 0 || git_buffer_append_char(buffer, '\0') != 0) {
        return -1;
    }
    entry_length = header_length + path_length + 1U;
    while ((entry_length & 7U) != 0U) {
        if (git_buffer_append_char(buffer, '\0') != 0) {
            return -1;
        }
        entry_length += 1U;
    }
    return 0;
}

static int git_write_index_file(const GitRepo *repo, GitIndex *index) {
    GitBuffer buffer;
    unsigned char word[4];
    unsigned char digest[CRYPTO_SHA1_DIGEST_SIZE];
    CryptoSha1Context sha1;
    char path[GIT_PATH_CAPACITY];
    unsigned int version = 2U;
    size_t i;

    rt_memset(&buffer, 0, sizeof(buffer));
    if (index->count > 1U) {
        rt_sort(index->entries, index->count, sizeof(index->entries[0]), git_compare_entries_by_path);
    }
    for (i = 0U; i < index->count; ++i) {
        if (index->entries[i].intent_to_add) {
            version = 3U;
            break;
        }
    }
    if (git_buffer_append(&buffer, "DIRC", 4U) != 0) {
        return -1;
    }
    git_write_u32_be(word, version);
    if (git_buffer_append(&buffer, word, 4U) != 0) {
        git_buffer_destroy(&buffer);
        return -1;
    }
    git_write_u32_be(word, (unsigned int)index->count);
    if (git_buffer_append(&buffer, word, 4U) != 0) {
        git_buffer_destroy(&buffer);
        return -1;
    }
    for (i = 0U; i < index->count; ++i) {
        if (git_index_write_entry(&buffer, &index->entries[i]) != 0) {
            git_buffer_destroy(&buffer);
            return -1;
        }
    }
    crypto_sha1_init(&sha1);
    crypto_sha1_update(&sha1, buffer.data, buffer.size);
    crypto_sha1_final(&sha1, digest);
    if (git_buffer_append(&buffer, digest, sizeof(digest)) != 0 || git_join(path, sizeof(path), repo->git_dir, "index") != 0 || git_write_all_file(path, buffer.data, buffer.size, 0644U) != 0) {
        git_buffer_destroy(&buffer);
        return -1;
    }
    git_buffer_destroy(&buffer);
    return 0;
}

static int git_checkout_commit_to_worktree(const GitRepo *repo, const unsigned char commit_oid[CRYPTO_SHA1_DIGEST_SIZE], const GitPack *pack_cache) {
    unsigned char tree_oid[CRYPTO_SHA1_DIGEST_SIZE];
    GitIndex old_index;
    GitIndex target_index;
    GitCheckoutIndex checkout;
    int result;
    int have_old_index = 0;

    rt_memset(&old_index, 0, sizeof(old_index));
    rt_memset(&target_index, 0, sizeof(target_index));
    rt_memset(&checkout, 0, sizeof(checkout));
    checkout.repo = repo;
    if (git_commit_tree_oid(repo, commit_oid, pack_cache, tree_oid) != 0) {
        return -1;
    }
    if (git_load_commit_tree_index(repo, commit_oid, pack_cache, &target_index) != 0) {
        return -1;
    }
    have_old_index = git_load_head_tree_index(repo, pack_cache, &old_index) == 0;
    if (have_old_index && git_remove_checkout_absent_paths(repo, &old_index, &target_index) != 0) {
        git_index_destroy(&old_index);
        git_index_destroy(&target_index);
        return -1;
    }
    result = git_checkout_tree_recursive(repo, tree_oid, pack_cache, "", &checkout);
    if (result == 0) {
        result = git_write_index_file(repo, &checkout.entries);
    }
    if (have_old_index) {
        git_index_destroy(&old_index);
    }
    git_index_destroy(&target_index);
    git_index_destroy(&checkout.entries);
    return result;
}

