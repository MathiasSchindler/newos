#include "whisper_indexed.h"
#include "whisper_artifact.h"
#include "platform.h"

enum { INDEX_HEADER_SIZE = 64, INDEX_ENTRY_SIZE = 192, INDEX_READ_SIZE = 65536 };

static unsigned char check_buffer[INDEX_READ_SIZE];

static unsigned int read_u32(const unsigned char *bytes) {
    return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8) |
        ((unsigned int)bytes[2] << 16) | ((unsigned int)bytes[3] << 24);
}

static unsigned long long read_u64(const unsigned char *bytes) {
    return (unsigned long long)read_u32(bytes) | ((unsigned long long)read_u32(bytes + 4) << 32);
}

static int read_exact(int fd, void *buffer, unsigned int length) {
    unsigned char *target = (unsigned char *)buffer;
    while (length) {
        long amount = platform_read(fd, target, length);
        if (amount <= 0 || (unsigned long)amount > length) return 0;
        target += amount;
        length -= (unsigned int)amount;
    }
    return 1;
}

static int same(const char *left, const char *right) {
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

void whisper_indexed_close(WhisperIndexed *model) {
    if (model->fd >= 0) platform_close(model->fd);
    model->fd = -1;
    model->count = 0;
    model->data_offset = 0;
}

int whisper_indexed_open(WhisperIndexed *model, const char *path, const WhisperModelConfig *config) {
    unsigned char header[INDEX_HEADER_SIZE];
    unsigned char entry[INDEX_ENTRY_SIZE];
    unsigned long long data_size, data_offset, file_size;
    unsigned long long table_hash = WHISPER_ARTIFACT_HASH_OFFSET_BASIS;
    unsigned long long payload_hash = WHISPER_ARTIFACT_HASH_OFFSET_BASIS;
    unsigned int count;
    long long length;

    model->fd = -1;
    model->count = 0;
    model->data_offset = 0;
    if (path == 0 || config == 0 || !whisper_model_config_valid(config)) return 0;
    model->fd = platform_open_read(path);
    if (model->fd < 0) return 0;
    length = platform_seek(model->fd, 0, PLATFORM_SEEK_END);
    if (length < INDEX_HEADER_SIZE || platform_seek(model->fd, 0, PLATFORM_SEEK_SET) != 0 ||
        !read_exact(model->fd, header, sizeof(header))) goto fail;
    file_size = (unsigned long long)length;
    if (header[0] != 'W' || header[1] != 'T' || header[2] != 'I' || header[3] != 'N' ||
        header[4] != 'D' || header[5] != 'E' || header[6] != 'X' || header[7] != '1' ||
        read_u32(header + 8) != 1 || read_u32(header + 12) != config->model_id ||
        read_u32(header + 20) != INDEX_ENTRY_SIZE || read_u64(header + 56) != 0) goto fail;
    count = read_u32(header + 16);
    data_offset = read_u64(header + 24);
    data_size = read_u64(header + 32);
    if (count == 0 || count > WHISPER_TENSOR_MAX_COUNT ||
        data_offset != INDEX_HEADER_SIZE + (unsigned long long)count * INDEX_ENTRY_SIZE ||
        data_offset > file_size || data_size != file_size - data_offset) goto fail;
    for (unsigned int index = 0; index < count; ++index) {
        WhisperTensorIndex *tensor = model->tensors + index;
        unsigned long long elements = 1;
        unsigned int name_length = 0;
        if (!read_exact(model->fd, entry, sizeof(entry))) goto fail;
        table_hash = whisper_artifact_hash_update(table_hash, entry, sizeof(entry));
        while (name_length < WHISPER_TENSOR_NAME_SIZE && entry[name_length]) {
            if (entry[name_length] < 0x21 || entry[name_length] >= 0x7f) goto fail;
            tensor->name[name_length] = (char)entry[name_length];
            ++name_length;
        }
        if (name_length == 0 || name_length == WHISPER_TENSOR_NAME_SIZE) goto fail;
        tensor->name[name_length] = 0;
        for (unsigned int position = name_length + 1; position < WHISPER_TENSOR_NAME_SIZE; ++position) {
            if (entry[position]) goto fail;
        }
        for (unsigned int previous = 0; previous < index; ++previous) {
            if (same(tensor->name, model->tensors[previous].name)) goto fail;
        }
        tensor->type = read_u32(entry + 96);
        tensor->rank = read_u32(entry + 100);
        if ((tensor->type != WHISPER_TENSOR_F32 && tensor->type != WHISPER_TENSOR_F16) ||
            tensor->rank > WHISPER_TENSOR_MAX_RANK) goto fail;
        for (unsigned int dimension = 0; dimension < WHISPER_TENSOR_MAX_RANK; ++dimension) {
            unsigned long long value = read_u64(entry + 104 + 8 * dimension);
            if (dimension < tensor->rank) {
                if (!value || elements > 0xffffffffffffffffULL / value) goto fail;
                elements *= value;
            } else if (value) goto fail;
            tensor->shape[dimension] = value;
        }
        tensor->start = read_u64(entry + 168);
        tensor->end = read_u64(entry + 176);
        model->hashes[index] = read_u64(entry + 184);
        unsigned int stride = tensor->type == WHISPER_TENSOR_F32 ? 4 : 2;
        if (tensor->start >= tensor->end || tensor->end > data_size ||
            elements > 0xffffffffffffffffULL / stride ||
            tensor->end - tensor->start != elements * stride) goto fail;
    }
    if (table_hash != read_u64(header + 40) ||
        !whisper_tensor_index_validate(model->tensors, count, data_size)) goto fail;
    for (unsigned long long remaining = data_size; remaining;) {
        unsigned int size = remaining > INDEX_READ_SIZE ? INDEX_READ_SIZE : (unsigned int)remaining;
        if (!read_exact(model->fd, check_buffer, size)) goto fail;
        payload_hash = whisper_artifact_hash_update(payload_hash, check_buffer, size);
        remaining -= size;
    }
    if (payload_hash != read_u64(header + 48)) goto fail;
    model->count = count;
    model->data_offset = data_offset;
    return 1;
fail:
    whisper_indexed_close(model);
    return 0;
}

const WhisperTensorIndex *whisper_indexed_find(const WhisperIndexed *model, const char *name) {
    if (model == 0 || name == 0 || model->fd < 0) return 0;
    for (unsigned int index = 0; index < model->count; ++index) {
        if (same(name, model->tensors[index].name)) return model->tensors + index;
    }
    return 0;
}

int whisper_indexed_read(
    const WhisperIndexed *model, const WhisperTensorIndex *tensor,
    void *buffer, unsigned long long capacity
) {
    unsigned int index = 0;
    unsigned long long size;
    if (model == 0 || tensor == 0 || buffer == 0 || model->fd < 0) return 0;
    while (index < model->count && tensor != model->tensors + index) ++index;
    if (index == model->count) return 0;
    size = tensor->end - tensor->start;
    if (size > capacity || size > 0xffffffffULL ||
        model->data_offset + tensor->start > 0x7fffffffffffffffULL ||
        platform_seek(model->fd, (long long)(model->data_offset + tensor->start), PLATFORM_SEEK_SET) !=
            (long long)(model->data_offset + tensor->start) ||
        !read_exact(model->fd, buffer, (unsigned int)size)) return 0;
    return whisper_artifact_hash_update(WHISPER_ARTIFACT_HASH_OFFSET_BASIS, buffer, size) ==
        model->hashes[index];
}