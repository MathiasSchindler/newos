#include "whisper_artifact.h"

#define WHISPER_ARTIFACT_MAGIC 0x3254464152485357ULL
#define WHISPER_ARTIFACT_VERSION 2U

__declspec(dllimport) void *CreateFileA(
    const char *name, unsigned int access, unsigned int sharing, void *security,
    unsigned int creation, unsigned int attributes, void *template_file
);
#ifdef WHISPER_RUNTIME_ONLY
__declspec(dllimport) unsigned int GetModuleFileNameW(void *, unsigned short *, unsigned int);
__declspec(dllimport) void *CreateFileW(const unsigned short *, unsigned int, unsigned int, void *, unsigned int, unsigned int, void *);
#endif

void *whisper_artifact_open_read(const char *path) {
    static const char prefix[] = "experimental/snapdragon/";
    void *invalid = (void *)(unsigned long long)-1;
#ifdef WHISPER_RUNTIME_ONLY
    unsigned short local[32768];
    unsigned int index, used = GetModuleFileNameW(0, local, 32768);
    if (!used || used >= 32768) return invalid;
    while (used && local[used - 1] != '\\' && local[used - 1] != '/') --used;
    for (index = 0; prefix[index]; ++index) if (path[index] != prefix[index]) return invalid;
    path += index;
    if (path[0] == 'b' && path[1] == 'u' && path[2] == 'i' && path[3] == 'l' && path[4] == 'd' && path[5] == '/') path += 6;
    else {
        if (used + 3 >= 32768) return invalid;
        local[used++] = '.'; local[used++] = '.'; local[used++] = '/';
    }
    while (*path) {
        if (used + 1 >= 32768) return invalid;
        local[used++] = (unsigned char)*path++;
    }
    local[used] = 0;
    return CreateFileW(local, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
#else
    void *handle = CreateFileA(path, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    unsigned int index;
    if (handle != invalid) return handle;
    for (index = 0U; prefix[index] != '\0'; ++index) {
        if (path[index] != prefix[index]) return handle;
    }
    return CreateFileA(path + index, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
#endif
}

static unsigned int read_u32(const unsigned char *bytes) {
    return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8U) |
        ((unsigned int)bytes[2] << 16U) | ((unsigned int)bytes[3] << 24U);
}

static unsigned long long read_u64(const unsigned char *bytes) {
    return (unsigned long long)read_u32(bytes) |
        ((unsigned long long)read_u32(bytes + 4U) << 32U);
}

static void write_u32(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void write_u64(unsigned char *bytes, unsigned long long value) {
    write_u32(bytes, (unsigned int)value);
    write_u32(bytes + 4U, (unsigned int)(value >> 32U));
}

void whisper_artifact_encode_header(
    unsigned char output[WHISPER_ARTIFACT_HEADER_SIZE],
    const WhisperArtifactHeader *header
) {
    unsigned int index;
    for (index = 0U; index < WHISPER_ARTIFACT_HEADER_SIZE; ++index) output[index] = 0U;
    write_u64(output, WHISPER_ARTIFACT_MAGIC);
    write_u32(output + 8U, WHISPER_ARTIFACT_VERSION);
    write_u32(output + 12U, WHISPER_ARTIFACT_HEADER_SIZE);
    write_u32(output + 16U, header->model_id);
    write_u32(output + 20U, header->payload_type);
    write_u32(output + 24U, header->element_type);
    write_u64(output + 32U, header->element_count);
    write_u64(output + 40U, header->payload_size);
    write_u64(output + 48U, header->payload_hash);
    write_u32(output + 56U, header->width);
    write_u32(output + 60U, header->ffn_width);
    write_u32(output + 64U, header->attention_heads);
    write_u32(output + 68U, header->encoder_layers);
    write_u32(output + 72U, header->decoder_layers);
    write_u32(output + 76U, header->vocabulary_size);
    write_u32(output + 80U, header->text_context);
    write_u32(output + 84U, header->mel_bins);
    write_u32(output + 88U, header->encoder_frames);
}

int whisper_artifact_decode_header(
    const unsigned char input[WHISPER_ARTIFACT_HEADER_SIZE],
    WhisperArtifactHeader *header
) {
    if (header == 0 || read_u64(input) != WHISPER_ARTIFACT_MAGIC ||
        read_u32(input + 8U) != WHISPER_ARTIFACT_VERSION ||
        read_u32(input + 12U) != WHISPER_ARTIFACT_HEADER_SIZE ||
        read_u32(input + 28U) != 0U || read_u32(input + 92U) != 0U) {
        return 0;
    }
    header->model_id = read_u32(input + 16U);
    header->payload_type = read_u32(input + 20U);
    header->element_type = read_u32(input + 24U);
    header->element_count = read_u64(input + 32U);
    header->payload_size = read_u64(input + 40U);
    header->payload_hash = read_u64(input + 48U);
    header->width = read_u32(input + 56U);
    header->ffn_width = read_u32(input + 60U);
    header->attention_heads = read_u32(input + 64U);
    header->encoder_layers = read_u32(input + 68U);
    header->decoder_layers = read_u32(input + 72U);
    header->vocabulary_size = read_u32(input + 76U);
    header->text_context = read_u32(input + 80U);
    header->mel_bins = read_u32(input + 84U);
    header->encoder_frames = read_u32(input + 88U);
    return 1;
}

int whisper_artifact_header_valid(
    const WhisperArtifactHeader *header,
    const WhisperModelConfig *model,
    unsigned int payload_type,
    unsigned int element_type,
    unsigned long long element_count,
    unsigned long long payload_size
) {
    if (header == 0 || !whisper_model_config_valid(model)) return 0;
    return header->model_id == model->model_id &&
        header->payload_type == payload_type &&
        header->element_type == element_type &&
        header->element_count == element_count &&
        header->payload_size == payload_size &&
        header->payload_hash != 0U &&
        header->width == model->width &&
        header->ffn_width == model->ffn_width &&
        header->attention_heads == model->attention_heads &&
        header->encoder_layers == model->encoder_layers &&
        header->decoder_layers == model->decoder_layers &&
        header->vocabulary_size == model->vocabulary_size &&
        header->text_context == model->text_context &&
        header->mel_bins == model->mel_bins &&
        header->encoder_frames == model->encoder_frames;
}

int whisper_artifact_payload_valid(
    const WhisperArtifactHeader *header,
    const void *payload,
    unsigned long long payload_size
) {
    if (header == 0 || payload == 0 || payload_size != header->payload_size) return 0;
    return whisper_artifact_hash_update(
        WHISPER_ARTIFACT_HASH_OFFSET_BASIS, payload, payload_size
    ) == header->payload_hash;
}

unsigned long long whisper_artifact_hash_update(
    unsigned long long hash,
    const void *data,
    unsigned long long size
) {
    const unsigned char *bytes = (const unsigned char *)data;
    while (size-- != 0U) {
        hash ^= *bytes++;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}
