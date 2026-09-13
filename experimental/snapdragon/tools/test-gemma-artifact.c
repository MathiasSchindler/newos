#include "../src/tools/gemma/gemma_artifact.h"

#include "crypto/sha256.h"

static int bytes_equal(const unsigned char *left, const unsigned char *right, unsigned int size) {
    unsigned int index;
    for (index = 0U; index < size; ++index) {
        if (left[index] != right[index]) return 0;
    }
    return 1;
}

int main(void) {
    static const char name[] = "language_model.model.layers.0.mlp.up_proj.weight";
    static const unsigned char payload[66] = {0x10, 0x32, 0x54, 0x76, 0x00, 0x3c};
    GemmaArtifactHeader header = {0};
    GemmaArtifactHeader decoded;
    unsigned char encoded[GEMMA_ARTIFACT_HEADER_SIZE];
    unsigned char changed[sizeof(payload)];
    signed char unpacked[8];
    static const signed char expected_unpacked[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    unsigned int index;
    const GemmaModelConfig *model = gemma_model_translategemma_4b();

    header.kind = GEMMA_ARTIFACT_KIND_TENSOR;
    header.element_type = GEMMA_ARTIFACT_ELEMENT_S4;
    header.quantization = GEMMA_ARTIFACT_QUANTIZATION_SYMMETRIC_GROUP;
    header.layout = GEMMA_ARTIFACT_LAYOUT_ROW_MAJOR;
    header.rank = 2U;
    header.group_size = 128U;
    header.quantization_axis = 1U;
    header.element_count = 128U;
    header.data_size = 64U;
    header.scale_count = 1U;
    header.scale_offset = 64U;
    header.payload_size = sizeof(payload);
    header.tensor_id = gemma_artifact_tensor_id(name);
    header.dimensions[0] = 1U;
    header.dimensions[1] = 128U;
    gemma_artifact_model_sha256(model, header.model_sha256);
    gemma_artifact_name_sha256(name, header.name_sha256);
    crypto_sha256_hash(payload, sizeof(payload), header.payload_sha256);

    gemma_artifact_encode_header(encoded, &header);
    if (!gemma_artifact_decode_header(encoded, &decoded)) return 1;
    if (!bytes_equal(header.name_sha256, decoded.name_sha256, 32U) ||
        !gemma_artifact_header_valid(&decoded, model) ||
        !gemma_artifact_header_matches_name(&decoded, name) ||
        !gemma_artifact_payload_valid(&decoded, payload, sizeof(payload))) return 2;
    if (gemma_artifact_header_matches_name(&decoded, "language_model.wrong.weight")) return 12;
    encoded[0] ^= 1U;
    if (gemma_artifact_decode_header(encoded, &decoded)) return 3;
    gemma_artifact_encode_header(encoded, &header);
    encoded[44] = 1U;
    if (gemma_artifact_decode_header(encoded, &decoded)) return 4;
    gemma_artifact_encode_header(encoded, &header);
    if (!gemma_artifact_decode_header(encoded + 0U, &decoded)) return 5;
    decoded.dimensions[1] = 127U;
    if (gemma_artifact_header_valid(&decoded, model)) return 6;
    if (!gemma_artifact_decode_header(encoded, &decoded)) return 7;
    decoded.payload_size = ~0ULL;
    if (gemma_artifact_header_valid(&decoded, model)) return 8;
    if (!gemma_artifact_decode_header(encoded, &decoded)) return 9;
    for (index = 0U; index < sizeof(payload); ++index) changed[index] = payload[index];
    changed[0] ^= 1U;
    if (gemma_artifact_payload_valid(&decoded, changed, sizeof(changed))) return 10;
    decoded.model_sha256[0] ^= 1U;
    if (gemma_artifact_header_valid(&decoded, model)) return 11;
    if (!gemma_artifact_unpack_s4(payload, 8U, unpacked, 8U)) return 13;
    for (index = 0U; index < 8U; ++index) {
        if (unpacked[index] != expected_unpacked[index]) return 14;
    }
    changed[0] = 0x98U;
    if (!gemma_artifact_unpack_s4(changed, 2U, unpacked, 8U) ||
        unpacked[0] != -8 || unpacked[1] != -7) return 15;
    if (gemma_artifact_unpack_s4(changed, 2U, unpacked, 1U)) return 16;
    return 0;
}