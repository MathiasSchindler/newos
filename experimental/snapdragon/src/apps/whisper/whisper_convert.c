#include "platform.h"
#include "whisper_artifact.h"
#include "whisper_tensor_index.h"

enum { HEADER_LIMIT = 1024 * 1024, COPY_SIZE = 64 * 1024, ENTRY_SIZE = 192 };

__declspec(dllimport) const char *__stdcall GetCommandLineA(void);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int status);

static unsigned char json_header[HEADER_LIMIT];
static unsigned char buffer[COPY_SIZE];
static WhisperTensorIndex tensors[WHISPER_TENSOR_MAX_COUNT];

static unsigned long long read_u64(const unsigned char *bytes) {
    unsigned long long value = 0;
    for (unsigned int index = 0; index < 8; ++index) value |= (unsigned long long)bytes[index] << (index * 8);
    return value;
}

static void write_u32(unsigned char *bytes, unsigned int value) {
    for (unsigned int index = 0; index < 4; ++index) bytes[index] = (unsigned char)(value >> (index * 8));
}

static void write_u64(unsigned char *bytes, unsigned long long value) {
    for (unsigned int index = 0; index < 8; ++index) bytes[index] = (unsigned char)(value >> (index * 8));
}

static int read_exact(int fd, void *target, unsigned int size) {
    unsigned char *bytes = (unsigned char *)target;
    while (size != 0) {
        long read_count = platform_read(fd, bytes, size);
        if (read_count <= 0 || (unsigned long)read_count > size) return 0;
        bytes += read_count;
        size -= (unsigned int)read_count;
    }
    return 1;
}

static int write_exact(int fd, const void *source, unsigned int size) {
    const unsigned char *bytes = (const unsigned char *)source;
    while (size != 0) {
        long written = platform_write(fd, bytes, size);
        if (written <= 0 || (unsigned long)written > size) return 0;
        bytes += written;
        size -= (unsigned int)written;
    }
    return 1;
}

static void text(int fd, const char *value) {
    const char *end = value;
    while (*end) ++end;
    if (!write_exact(fd, value, (unsigned int)(end - value))) ExitProcess(3);
}

static void decimal(int fd, unsigned long long value) {
    char digits[21];
    unsigned int position = sizeof(digits) - 1;
    digits[position] = 0;
    do {
        digits[--position] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    text(fd, digits + position);
}

static int same(const char *left, const char *right) {
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

static const char *argument(const char *cursor, char *output, unsigned int capacity) {
    unsigned int length = 0;
    int quoted = 0;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (!*cursor) return 0;
    while (*cursor) {
        char letter = *cursor++;
        if (letter == '"') quoted = !quoted;
        else if (!quoted && (letter == ' ' || letter == '\t')) break;
        else {
            if (length + 1 >= capacity) return 0;
            output[length++] = letter;
        }
    }
    if (quoted || !length) return 0;
    output[length] = 0;
    return cursor;
}

static const WhisperModelConfig *model_for(const char *option) {
    if (same(option, "--model=tiny")) return whisper_model_tiny();
    if (same(option, "--model=base")) return whisper_model_base();
    if (same(option, "--model=small")) return whisper_model_small();
    if (same(option, "--model=medium")) return whisper_model_medium();
    return 0;
}

static int layer_number(const char *name, const char *prefix, unsigned int *layer, const char **suffix) {
    unsigned int value = 0;
    while (*prefix && *name == *prefix) { ++name; ++prefix; }
    if (*prefix || *name < '0' || *name > '9') return 0;
    while (*name >= '0' && *name <= '9') {
        if (value >= 32) return -1;
        value = value * 10 + (unsigned int)(*name++ - '0');
    }
    if (*name != '.') return -1;
    *layer = value;
    *suffix = name + 1;
    return 1;
}

static int check_model(const WhisperModelConfig *model, unsigned int count) {
    unsigned int matched = 0;
    unsigned int encoder_layers = 0;
    unsigned int decoder_layers = 0;
    for (unsigned int index = 0; index < count; ++index) {
        const WhisperTensorIndex *tensor = tensors + index;
        const char *suffix;
        unsigned int layer;
        int result;
        if (same(tensor->name, "model.decoder.embed_tokens.weight")) {
            if (tensor->rank != 2 || tensor->shape[0] != model->vocabulary_size ||
                tensor->shape[1] != model->width) return 0;
            matched |= 1;
        } else if (same(tensor->name, "model.decoder.embed_positions.weight")) {
            if (tensor->rank != 2 || tensor->shape[0] != model->text_context ||
                tensor->shape[1] != model->width) return 0;
            matched |= 2;
        }
        result = layer_number(tensor->name, "model.encoder.layers.", &layer, &suffix);
        if (result < 0 || (result > 0 && layer >= model->encoder_layers)) return 0;
        if (result > 0 && same(suffix, "self_attn.q_proj.weight")) {
            if (tensor->rank != 2 || tensor->shape[0] != model->width ||
                tensor->shape[1] != model->width) return 0;
            encoder_layers |= 1U << layer;
        }
        result = layer_number(tensor->name, "model.decoder.layers.", &layer, &suffix);
        if (result < 0 || (result > 0 && layer >= model->decoder_layers)) return 0;
        if (result > 0 && same(suffix, "self_attn.q_proj.weight")) {
            if (tensor->rank != 2 || tensor->shape[0] != model->width ||
                tensor->shape[1] != model->width) return 0;
            decoder_layers |= 1U << layer;
        }
    }
    return matched == 3 && encoder_layers == (0xffffffffU >> (32 - model->encoder_layers)) &&
        decoder_layers == (0xffffffffU >> (32 - model->decoder_layers));
}

static int convert(const WhisperModelConfig *model, const char *input_path, const char *output_path) {
    unsigned char prefix[8];
    unsigned char record[ENTRY_SIZE];
    unsigned char header[64];
    unsigned long long header_length, file_size, data_size, payload_hash, table_hash;
    unsigned int count = 0;
    char temporary[1032];
    unsigned int length = 0;
    int input = -1, output = -1, created = 0, success = 0;
    unsigned int stage = 0;

    if (!whisper_model_config_valid(model) || same(input_path, output_path) ||
        platform_path_access(output_path, 0) == 0) return 0;
    input = platform_open_read(input_path);
    if (input < 0) goto finish;
    stage = 1;
    long long last_byte = platform_seek(input, 0, PLATFORM_SEEK_END);
    if (last_byte < 8 || platform_seek(input, 0, PLATFORM_SEEK_SET) != 0 ||
        !read_exact(input, prefix, sizeof(prefix))) goto finish;
    file_size = (unsigned long long)last_byte;
    header_length = read_u64(prefix);
    if (header_length == 0 || header_length > HEADER_LIMIT || header_length > file_size - 8 ||
        !read_exact(input, json_header, (unsigned int)header_length)) goto finish;
    stage = 2;
    data_size = file_size - 8 - header_length;
    if (!whisper_tensor_index_parse(json_header, (unsigned int)header_length, tensors,
            WHISPER_TENSOR_MAX_COUNT, &count)) goto finish;
    stage = 3;
    if (!whisper_tensor_index_validate(tensors, count, data_size) ||
        !check_model(model, count)) goto finish;
    stage = 4;

    while (output_path[length] && length < 1023) { temporary[length] = output_path[length]; ++length; }
    if (output_path[length]) goto finish;
    for (unsigned int index = 0; index < sizeof(".partial"); ++index) {
        temporary[length + index] = ".partial"[index];
    }
    output = platform_open_create_exclusive(temporary, 0600);
    if (output < 0) goto finish;
    created = 1;
    stage = 5;
    for (unsigned int index = 0; index < sizeof(header); ++index) header[index] = 0;
    if (!write_exact(output, header, sizeof(header))) goto finish;
    for (unsigned int index = 0; index < count * ENTRY_SIZE; ++index) buffer[index % COPY_SIZE] = 0;
    for (unsigned int remaining = count * ENTRY_SIZE; remaining != 0;) {
        unsigned int bytes = remaining > COPY_SIZE ? COPY_SIZE : remaining;
        if (!write_exact(output, buffer, bytes)) goto finish;
        remaining -= bytes;
    }

    payload_hash = WHISPER_ARTIFACT_HASH_OFFSET_BASIS;
    unsigned long long tensor_hashes[WHISPER_TENSOR_MAX_COUNT];
    for (unsigned int index = 0; index < count; ++index) tensor_hashes[index] = WHISPER_ARTIFACT_HASH_OFFSET_BASIS;
    for (unsigned long long position = 0; position < data_size;) {
        unsigned int bytes = data_size - position > COPY_SIZE ? COPY_SIZE : (unsigned int)(data_size - position);
        if (!read_exact(input, buffer, bytes) || !write_exact(output, buffer, bytes)) goto finish;
        payload_hash = whisper_artifact_hash_update(payload_hash, buffer, bytes);
        for (unsigned int index = 0; index < count; ++index) {
            unsigned long long start = tensors[index].start > position ? tensors[index].start : position;
            unsigned long long end = tensors[index].end < position + bytes ? tensors[index].end : position + bytes;
            if (start < end) tensor_hashes[index] = whisper_artifact_hash_update(
                tensor_hashes[index], buffer + (unsigned int)(start - position), end - start
            );
        }
        position += bytes;
    }
    if (platform_seek(output, sizeof(header), PLATFORM_SEEK_SET) != sizeof(header)) goto finish;
    stage = 6;
    table_hash = WHISPER_ARTIFACT_HASH_OFFSET_BASIS;
    for (unsigned int index = 0; index < count; ++index) {
        for (unsigned int offset = 0; offset < ENTRY_SIZE; ++offset) record[offset] = 0;
        for (unsigned int offset = 0; tensors[index].name[offset] != 0; ++offset) {
            record[offset] = (unsigned char)tensors[index].name[offset];
        }
        write_u32(record + 96, tensors[index].type);
        write_u32(record + 100, tensors[index].rank);
        for (unsigned int dimension = 0; dimension < WHISPER_TENSOR_MAX_RANK; ++dimension) {
            write_u64(record + 104 + dimension * 8, tensors[index].shape[dimension]);
        }
        write_u64(record + 168, tensors[index].start);
        write_u64(record + 176, tensors[index].end);
        write_u64(record + 184, tensor_hashes[index]);
        table_hash = whisper_artifact_hash_update(table_hash, record, sizeof(record));
        if (!write_exact(output, record, sizeof(record))) goto finish;
    }
    header[0] = 'W'; header[1] = 'T'; header[2] = 'I'; header[3] = 'N';
    header[4] = 'D'; header[5] = 'E'; header[6] = 'X'; header[7] = '1';
    write_u32(header + 8, 1);
    write_u32(header + 12, model->model_id);
    write_u32(header + 16, count);
    write_u32(header + 20, ENTRY_SIZE);
    write_u64(header + 24, sizeof(header) + (unsigned long long)count * ENTRY_SIZE);
    write_u64(header + 32, data_size);
    write_u64(header + 40, table_hash);
    write_u64(header + 48, payload_hash);
    if (platform_seek(output, 0, PLATFORM_SEEK_SET) != 0 ||
        !write_exact(output, header, sizeof(header)) || platform_close(output) != 0) goto finish;
    output = -1;
    stage = 7;
    if (platform_create_hard_link(temporary, output_path) != 0) goto finish;
    success = 1;
    text(1, "tensors="); decimal(1, count);
    text(1, "\npayload_bytes="); decimal(1, data_size);
    text(1, "\n");
finish:
    if (!success) { text(2, "conversion_stage="); decimal(2, stage); text(2, "\n"); }
    if (output >= 0) platform_close(output);
    if (created) platform_remove_file(temporary);
    if (input >= 0) platform_close(input);
    return success;
}

static int run(void) {
    char executable[1024], option[1024], input[1024], output[1024], extra[1024];
    const char *cursor = GetCommandLineA();
    const WhisperModelConfig *model;
    if (cursor == 0 || (cursor = argument(cursor, executable, sizeof(executable))) == 0 ||
        (cursor = argument(cursor, option, sizeof(option))) == 0 ||
        (cursor = argument(cursor, input, sizeof(input))) == 0 ||
        (cursor = argument(cursor, output, sizeof(output))) == 0 ||
        argument(cursor, extra, sizeof(extra)) != 0 || (model = model_for(option)) == 0) {
        text(2, "Usage: whisper-convert.exe --model=tiny|base|small|medium <model.safetensors> <new-output.wti>\n");
        return 2;
    }
    if (!convert(model, input, output)) {
        text(2, "Checkpoint invalid, output exists, or conversion failed.\n");
        return 1;
    }
    return 0;
}

void mainCRTStartup(void) {
    ExitProcess((unsigned int)run());
}