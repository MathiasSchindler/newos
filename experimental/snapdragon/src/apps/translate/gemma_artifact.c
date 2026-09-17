#include "gemma_artifact.h"

#include "crypto/sha256.h"

#define GEMMA_ARTIFACT_MAGIC 0x33305452414d4547ULL
#define GEMMA_ARTIFACT_VERSION 1U

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

static void copy_bytes(unsigned char *output, const unsigned char *input, unsigned int size) {
    unsigned int index;
    for (index = 0U; index < size; ++index) output[index] = input[index];
}

static int bytes_equal(const unsigned char *left, const unsigned char *right, unsigned int size) {
    unsigned int difference = 0U;
    unsigned int index;
    for (index = 0U; index < size; ++index) difference |= left[index] ^ right[index];
    return difference == 0U;
}

static int digest_nonzero(const unsigned char digest[GEMMA_ARTIFACT_SHA256_SIZE]) {
    unsigned int value = 0U;
    unsigned int index;
    for (index = 0U; index < GEMMA_ARTIFACT_SHA256_SIZE; ++index) value |= digest[index];
    return value != 0U;
}

static unsigned long long string_length(const char *text) {
    unsigned long long length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0') ++length;
    return length;
}

void gemma_artifact_encode_header(
    unsigned char output[GEMMA_ARTIFACT_HEADER_SIZE],
    const GemmaArtifactHeader *header
) {
    unsigned int index;
    for (index = 0U; index < GEMMA_ARTIFACT_HEADER_SIZE; ++index) output[index] = 0U;
    write_u64(output, GEMMA_ARTIFACT_MAGIC);
    write_u32(output + 8U, GEMMA_ARTIFACT_VERSION);
    write_u32(output + 12U, GEMMA_ARTIFACT_HEADER_SIZE);
    write_u32(output + 16U, header->kind);
    write_u32(output + 20U, header->element_type);
    write_u32(output + 24U, header->quantization);
    write_u32(output + 28U, header->layout);
    write_u32(output + 32U, header->rank);
    write_u32(output + 36U, header->group_size);
    write_u32(output + 40U, header->quantization_axis);
    write_u64(output + 48U, header->element_count);
    write_u64(output + 56U, header->payload_size);
    write_u64(output + 64U, header->data_size);
    write_u64(output + 72U, header->scale_count);
    write_u64(output + 80U, header->scale_offset);
    write_u64(output + 88U, header->tensor_id);
    for (index = 0U; index < GEMMA_ARTIFACT_MAX_RANK; ++index) {
        write_u64(output + 96U + index * 8U, header->dimensions[index]);
    }
    copy_bytes(output + 160U, header->model_sha256, GEMMA_ARTIFACT_SHA256_SIZE);
    copy_bytes(output + 192U, header->name_sha256, GEMMA_ARTIFACT_SHA256_SIZE);
    copy_bytes(output + 224U, header->payload_sha256, GEMMA_ARTIFACT_SHA256_SIZE);
}

int gemma_artifact_decode_header(
    const unsigned char input[GEMMA_ARTIFACT_HEADER_SIZE],
    GemmaArtifactHeader *header
) {
    unsigned int index;
    if (header == 0 || read_u64(input) != GEMMA_ARTIFACT_MAGIC ||
        read_u32(input + 8U) != GEMMA_ARTIFACT_VERSION ||
        read_u32(input + 12U) != GEMMA_ARTIFACT_HEADER_SIZE ||
        read_u32(input + 44U) != 0U) {
        return 0;
    }
    header->kind = read_u32(input + 16U);
    header->element_type = read_u32(input + 20U);
    header->quantization = read_u32(input + 24U);
    header->layout = read_u32(input + 28U);
    header->rank = read_u32(input + 32U);
    header->group_size = read_u32(input + 36U);
    header->quantization_axis = read_u32(input + 40U);
    header->element_count = read_u64(input + 48U);
    header->payload_size = read_u64(input + 56U);
    header->data_size = read_u64(input + 64U);
    header->scale_count = read_u64(input + 72U);
    header->scale_offset = read_u64(input + 80U);
    header->tensor_id = read_u64(input + 88U);
    for (index = 0U; index < GEMMA_ARTIFACT_MAX_RANK; ++index) {
        header->dimensions[index] = read_u64(input + 96U + index * 8U);
    }
    copy_bytes(header->model_sha256, input + 160U, GEMMA_ARTIFACT_SHA256_SIZE);
    copy_bytes(header->name_sha256, input + 192U, GEMMA_ARTIFACT_SHA256_SIZE);
    copy_bytes(header->payload_sha256, input + 224U, GEMMA_ARTIFACT_SHA256_SIZE);
    return 1;
}

void gemma_artifact_name_sha256(
    const char *name,
    unsigned char output[GEMMA_ARTIFACT_SHA256_SIZE]
) {
    crypto_sha256_hash((const unsigned char *)name, (size_t)string_length(name), output);
}

void gemma_artifact_model_sha256(
    const GemmaModelConfig *model,
    unsigned char output[GEMMA_ARTIFACT_SHA256_SIZE]
) {
    static const unsigned char separator = '@';
    CryptoSha256Context context;
    crypto_sha256_init(&context);
    crypto_sha256_update(
        &context, (const unsigned char *)model->repository,
        (size_t)string_length(model->repository)
    );
    crypto_sha256_update(&context, &separator, 1U);
    crypto_sha256_update(
        &context, (const unsigned char *)model->revision,
        (size_t)string_length(model->revision)
    );
    crypto_sha256_final(&context, output);
}

unsigned long long gemma_artifact_tensor_id(const char *name) {
    const unsigned char *bytes = (const unsigned char *)name;
    unsigned long long hash = 0xcbf29ce484222325ULL;
    if (name == 0) return 0U;
    while (*bytes != 0U) {
        hash ^= *bytes++;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static int checked_multiply(
    unsigned long long left,
    unsigned long long right,
    unsigned long long *result
) {
    if (right != 0U && left > ~0ULL / right) return 0;
    *result = left * right;
    return 1;
}

int gemma_artifact_header_valid(
    const GemmaArtifactHeader *header,
    const GemmaModelConfig *model
) {
    unsigned char expected_model[GEMMA_ARTIFACT_SHA256_SIZE];
    unsigned long long elements = 1U;
    unsigned long long expected_data;
    unsigned long long expected_scales;
    unsigned int index;
    if (header == 0 || !gemma_model_config_valid(model) ||
        header->kind < GEMMA_ARTIFACT_KIND_TENSOR ||
        header->kind > GEMMA_ARTIFACT_KIND_QNN_CONTEXT ||
        header->rank == 0U || header->rank > GEMMA_ARTIFACT_MAX_RANK ||
        header->tensor_id == 0U || !digest_nonzero(header->name_sha256) ||
        !digest_nonzero(header->payload_sha256)) {
        return 0;
    }
    gemma_artifact_model_sha256(model, expected_model);
    if (!bytes_equal(header->model_sha256, expected_model, sizeof(expected_model))) return 0;
    for (index = 0U; index < header->rank; ++index) {
        if (header->dimensions[index] == 0U ||
            !checked_multiply(elements, header->dimensions[index], &elements)) return 0;
    }
    for (; index < GEMMA_ARTIFACT_MAX_RANK; ++index) {
        if (header->dimensions[index] != 0U) return 0;
    }
    if (elements != header->element_count || header->scale_offset != header->data_size) {
        return 0;
    }
    if (header->quantization == GEMMA_ARTIFACT_QUANTIZATION_NONE) {
        unsigned long long element_size;
        if (header->group_size != 0U || header->quantization_axis != GEMMA_ARTIFACT_NO_AXIS ||
            header->scale_count != 0U) return 0;
        element_size = header->element_type == GEMMA_ARTIFACT_ELEMENT_F16 ? 2U :
            header->element_type == GEMMA_ARTIFACT_ELEMENT_U8 ? 1U : 0U;
        if (element_size == 0U || !checked_multiply(elements, element_size, &expected_data)) {
            return 0;
        }
        return header->layout != 0U && header->data_size == expected_data &&
            header->payload_size == expected_data;
    }
    if (header->quantization != GEMMA_ARTIFACT_QUANTIZATION_SYMMETRIC_GROUP ||
        header->kind != GEMMA_ARTIFACT_KIND_TENSOR || header->rank != 2U ||
        header->layout != GEMMA_ARTIFACT_LAYOUT_ROW_MAJOR ||
        header->group_size != header->dimensions[1] ||
        header->quantization_axis != 1U) return 0;
    expected_data = header->element_type == GEMMA_ARTIFACT_ELEMENT_S4 ?
        (elements + 1U) / 2U : header->element_type == GEMMA_ARTIFACT_ELEMENT_S8 ?
            elements : 0U;
    if (expected_data == 0U) return 0;
    expected_scales = header->dimensions[0];
    return header->scale_count == expected_scales &&
        header->data_size == expected_data &&
        expected_scales <= (~0ULL - expected_data) / 2U &&
        header->payload_size == expected_data + expected_scales * 2U;
}

int gemma_artifact_header_matches_name(
    const GemmaArtifactHeader *header,
    const char *name
) {
    unsigned char expected[GEMMA_ARTIFACT_SHA256_SIZE];
    if (header == 0 || name == 0 || header->tensor_id != gemma_artifact_tensor_id(name)) {
        return 0;
    }
    gemma_artifact_name_sha256(name, expected);
    return bytes_equal(header->name_sha256, expected, sizeof(expected));
}

int gemma_artifact_payload_valid(
    const GemmaArtifactHeader *header,
    const void *payload,
    unsigned long long payload_size
) {
    unsigned char digest[GEMMA_ARTIFACT_SHA256_SIZE];
    if (header == 0 || payload == 0 || payload_size != header->payload_size ||
        payload_size > (unsigned long long)(size_t)-1) return 0;
    crypto_sha256_hash((const unsigned char *)payload, (size_t)payload_size, digest);
    return bytes_equal(digest, header->payload_sha256, sizeof(digest));
}

int gemma_artifact_unpack_s4(
    const unsigned char *packed,
    unsigned long long element_count,
    signed char *output,
    unsigned long long output_capacity
) {
    unsigned long long index;
    if (packed == 0 || output == 0 || output_capacity < element_count) return 0;
    for (index = 0U; index < element_count; ++index) {
        unsigned int nibble = (packed[index / 2U] >> ((index & 1U) * 4U)) & 15U;
        output[index] = (signed char)(nibble < 8U ? nibble : nibble - 16U);
    }
    return 1;
}