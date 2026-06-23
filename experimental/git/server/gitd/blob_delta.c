static int gitd_append_delta_varint(GitBuffer *buffer, size_t value) {
    unsigned char byte;

    byte = (unsigned char)(value & 0x7fU);
    value >>= 7U;
    while (value != 0U) {
        if (tool_byte_buffer_append_byte(buffer, byte | 0x80U) != 0) return -1;
        byte = (unsigned char)(value & 0x7fU);
        value >>= 7U;
    }
    return tool_byte_buffer_append_byte(buffer, byte);
}

static int gitd_append_delta_copy(GitBuffer *buffer, size_t offset, size_t size) {
    while (size > 0U) {
        size_t chunk = size > 0x10000U ? 0x10000U : size;
        unsigned char opcode = 0x80U;
        unsigned char bytes[7];
        size_t count = 1U;

        if ((offset & 0xffU) != 0U) {
            opcode |= 0x01U;
            bytes[count++] = (unsigned char)(offset & 0xffU);
        }
        if ((offset & 0xff00U) != 0U) {
            opcode |= 0x02U;
            bytes[count++] = (unsigned char)((offset >> 8U) & 0xffU);
        }
        if ((offset & 0xff0000U) != 0U) {
            opcode |= 0x04U;
            bytes[count++] = (unsigned char)((offset >> 16U) & 0xffU);
        }
        if ((offset & 0xff000000U) != 0U) {
            opcode |= 0x08U;
            bytes[count++] = (unsigned char)((offset >> 24U) & 0xffU);
        }
        if (chunk != 0x10000U) {
            if ((chunk & 0xffU) != 0U) {
                opcode |= 0x10U;
                bytes[count++] = (unsigned char)(chunk & 0xffU);
            }
            if ((chunk & 0xff00U) != 0U) {
                opcode |= 0x20U;
                bytes[count++] = (unsigned char)((chunk >> 8U) & 0xffU);
            }
            if ((chunk & 0xff0000U) != 0U) {
                opcode |= 0x40U;
                bytes[count++] = (unsigned char)((chunk >> 16U) & 0xffU);
            }
        }
        bytes[0] = opcode;
        if (git_buffer_append(buffer, bytes, count) != 0) return -1;
        offset += chunk;
        size -= chunk;
    }
    return 0;
}

static int gitd_append_delta_insert(GitBuffer *buffer, const unsigned char *data, size_t size) {
    size_t pos = 0U;

    while (pos < size) {
        size_t chunk = size - pos;

        if (chunk > 127U) chunk = 127U;
        if (tool_byte_buffer_append_byte(buffer, (unsigned char)chunk) != 0 || git_buffer_append(buffer, data + pos, chunk) != 0) return -1;
        pos += chunk;
    }
    return 0;
}

static unsigned int gitd_delta_hash_block(const unsigned char *data) {
    unsigned int hash = 2166136261U;
    size_t index;

    for (index = 0U; index < GITD_DELTA_MIN_COPY; ++index) {
        hash ^= (unsigned int)data[index];
        hash *= 16777619U;
    }
    return hash == 0U ? 1U : hash;
}

static size_t gitd_delta_table_size(size_t base_size) {
    size_t chunks;
    size_t slots = 1024U;

    if (base_size < GITD_DELTA_MIN_COPY) return 0U;
    chunks = 1U + (base_size - GITD_DELTA_MIN_COPY) / GITD_DELTA_SAMPLE_STEP;
    while (slots < chunks * 2U && slots < GITD_DELTA_HASH_LIMIT) slots *= 2U;
    if (slots > GITD_DELTA_HASH_LIMIT) slots = GITD_DELTA_HASH_LIMIT;
    return slots;
}

static GitdDeltaSlot *gitd_build_delta_table(const unsigned char *base, size_t base_size, size_t *slot_count_out) {
    GitdDeltaSlot *slots;
    size_t slot_count = gitd_delta_table_size(base_size);
    size_t offset;

    *slot_count_out = slot_count;
    if (slot_count == 0U) return 0;
    slots = (GitdDeltaSlot *)rt_malloc_array(slot_count, sizeof(slots[0]));
    if (slots == 0) return 0;
    rt_memset(slots, 0, slot_count * sizeof(slots[0]));
    for (offset = 0U; offset + GITD_DELTA_MIN_COPY <= base_size; offset += GITD_DELTA_SAMPLE_STEP) {
        unsigned int hash = gitd_delta_hash_block(base + offset);
        size_t slot = (size_t)hash & (slot_count - 1U);
        size_t probe;

        for (probe = 0U; probe < GITD_DELTA_PROBE_LIMIT; ++probe) {
            GitdDeltaSlot *candidate = &slots[(slot + probe) & (slot_count - 1U)];
            if (!candidate->used) {
                candidate->used = 1;
                candidate->hash = hash;
                candidate->offset = offset;
                break;
            }
        }
    }
    return slots;
}

static size_t gitd_delta_match_size(const unsigned char *base, size_t base_size, size_t base_offset, const unsigned char *target, size_t target_size, size_t target_offset) {
    size_t length = 0U;

    while (base_offset + length < base_size && target_offset + length < target_size && base[base_offset + length] == target[target_offset + length]) length += 1U;
    return length;
}

static int gitd_find_delta_match(const GitdDeltaSlot *slots, size_t slot_count, const unsigned char *base, size_t base_size, const unsigned char *target, size_t target_size, size_t target_offset, size_t *offset_out, size_t *size_out) {
    unsigned int hash;
    size_t slot;
    size_t probe;
    size_t best_size = 0U;
    size_t best_offset = 0U;

    if (slots == 0 || target_offset + GITD_DELTA_MIN_COPY > target_size) return 0;
    hash = gitd_delta_hash_block(target + target_offset);
    slot = (size_t)hash & (slot_count - 1U);
    for (probe = 0U; probe < GITD_DELTA_PROBE_LIMIT; ++probe) {
        const GitdDeltaSlot *candidate = &slots[(slot + probe) & (slot_count - 1U)];
        size_t match_size;

        if (!candidate->used) break;
        if (candidate->hash != hash || candidate->offset + GITD_DELTA_MIN_COPY > base_size || memcmp(base + candidate->offset, target + target_offset, GITD_DELTA_MIN_COPY) != 0) continue;
        match_size = gitd_delta_match_size(base, base_size, candidate->offset, target, target_size, target_offset);
        if (match_size > best_size) {
            best_size = match_size;
            best_offset = candidate->offset;
        }
    }
    if (best_size < GITD_DELTA_MIN_COPY) return 0;
    *offset_out = best_offset;
    *size_out = best_size;
    return 1;
}

static int gitd_build_blob_delta(const unsigned char *base, size_t base_size, const unsigned char *target, size_t target_size, GitBuffer *delta_out) {
    GitdDeltaSlot *slots = 0;
    size_t slot_count = 0U;
    size_t target_pos = 0U;
    size_t insert_start = 0U;
    int result = -1;

    rt_memset(delta_out, 0, sizeof(*delta_out));
    if (gitd_append_delta_varint(delta_out, base_size) != 0 || gitd_append_delta_varint(delta_out, target_size) != 0) goto fail;
    slots = gitd_build_delta_table(base, base_size, &slot_count);
    while (target_pos < target_size) {
        size_t copy_offset = 0U;
        size_t copy_size = 0U;

        if (gitd_find_delta_match(slots, slot_count, base, base_size, target, target_size, target_pos, &copy_offset, &copy_size)) {
            if (target_pos > insert_start && gitd_append_delta_insert(delta_out, target + insert_start, target_pos - insert_start) != 0) goto fail;
            if (gitd_append_delta_copy(delta_out, copy_offset, copy_size) != 0) goto fail;
            target_pos += copy_size;
            insert_start = target_pos;
        } else {
            target_pos += 1U;
        }
    }
    if (target_size > insert_start && gitd_append_delta_insert(delta_out, target + insert_start, target_size - insert_start) != 0) goto fail;
    result = 0;
fail:
    rt_free(slots);
    if (result == 0) return 0;
    git_buffer_destroy(delta_out);
    return -1;
}

static size_t gitd_blob_sample_step(size_t size) {
    size_t step;

    if (size <= GITD_DELTA_MIN_COPY * GITD_DELTA_SIMILARITY_SAMPLES) return GITD_DELTA_MIN_COPY;
    step = size / GITD_DELTA_SIMILARITY_SAMPLES;
    if (step < GITD_DELTA_MIN_COPY) step = GITD_DELTA_MIN_COPY;
    return step;
}

static int gitd_blob_has_matching_sample(const unsigned char *data, size_t size, unsigned int hash) {
    size_t step = gitd_blob_sample_step(size);
    size_t offset;

    for (offset = 0U; offset + GITD_DELTA_MIN_COPY <= size; offset += step) {
        if (gitd_delta_hash_block(data + offset) == hash) return 1;
    }
    return 0;
}

static void gitd_blob_base_list_destroy(GitdBlobBaseList *list) {
    size_t index;

    if (list == 0) return;
    for (index = 0U; index < list->count; ++index) {
        rt_free(list->items[index].data);
    }
    rt_memset(list, 0, sizeof(*list));
}

static size_t gitd_blob_similarity_score(const unsigned char *left, size_t left_size, const unsigned char *right, size_t right_size) {
    size_t prefix = 0U;
    size_t suffix = 0U;
    size_t sampled = 0U;

    while (prefix < left_size && prefix < right_size && left[prefix] == right[prefix]) prefix += 1U;
    while (suffix < left_size - prefix && suffix < right_size - prefix && left[left_size - 1U - suffix] == right[right_size - 1U - suffix]) suffix += 1U;
    if (left_size >= GITD_DELTA_MIN_COPY && right_size >= GITD_DELTA_MIN_COPY) {
        size_t step = gitd_blob_sample_step(left_size);
        size_t offset;

        for (offset = 0U; offset + GITD_DELTA_MIN_COPY <= left_size; offset += step) {
            if (gitd_blob_has_matching_sample(right, right_size, gitd_delta_hash_block(left + offset))) sampled += GITD_DELTA_MIN_COPY;
        }
    }
    return prefix + suffix + sampled;
}

static GitdBlobBase *gitd_choose_blob_delta_base(GitdBlobBaseList *bases, const unsigned char *data, size_t size) {
    GitdBlobBase *best = 0;
    size_t best_score = 0U;
    size_t index;

    for (index = 0U; index < bases->count; ++index) {
        size_t score = gitd_blob_similarity_score(bases->items[index].data, bases->items[index].size, data, size);
        if (score > best_score) {
            best_score = score;
            best = &bases->items[index];
        }
    }
    return best_score >= 8U ? best : 0;
}

static int gitd_blob_base_list_take(GitdBlobBaseList *bases, const unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE], unsigned char **data_io, size_t size) {
    GitdBlobBase *slot;

    if (*data_io == 0 || bases->count >= GITD_MAX_DELTA_BASES || size > GITD_MAX_DELTA_BASE_BYTES || bases->total_bytes > GITD_MAX_DELTA_BASE_BYTES - size) return 0;
    slot = &bases->items[bases->count++];
    memcpy(slot->oid, oid, CRYPTO_SHA1_DIGEST_SIZE);
    slot->data = *data_io;
    slot->size = size;
    bases->total_bytes += size;
    *data_io = 0;
    return 0;
}
