#include "gemma_numeric.h"
#include "gemma_artifact.h"

static float inputs[1024];
static float output[256];

static unsigned int read_u32(const unsigned char *bytes) {
    return bytes[0] | ((unsigned int)bytes[1] << 8) |
        ((unsigned int)bytes[2] << 16) | ((unsigned int)bytes[3] << 24);
}

static float read_float(const unsigned char *bytes) {
    union { unsigned int bits; float value; } converted;
    converted.bits = read_u32(bytes);
    return converted.value;
}

static int close_float(float left, float right) {
    float difference = left - right;
    if (difference < 0) difference = -difference;
    if (right < 0) right = -right;
    return difference <= 0.000001f * (1.0f + right);
}

int gemma_numeric_test(const unsigned char *artifact, unsigned int size, unsigned int *passed) {
    GemmaArtifactHeader header;
    unsigned int offset = 268U, cases, case_index;
    *passed = 0;
    if (size < offset || !gemma_artifact_decode_header(artifact, &header) ||
        !gemma_artifact_header_valid(&header, gemma_model_translategemma_4b()) ||
        !gemma_artifact_header_matches_name(&header, "fixture/numeric-scalars-v1") ||
        header.kind != GEMMA_ARTIFACT_KIND_FIXTURE || header.rank != 1U ||
        header.layout != GEMMA_ARTIFACT_LAYOUT_OPAQUE || header.element_type != GEMMA_ARTIFACT_ELEMENT_U8 ||
        header.payload_size != size - 256U ||
        !gemma_artifact_payload_valid(&header, artifact + 256U, size - 256U) ||
        read_u32(artifact + 256U) != 0x354e4d47U || read_u32(artifact + 260U) != 1U) return 0;
    cases = read_u32(artifact + 264U);
    if (!cases || cases > 100000U) return 0;
    for (case_index = 0; case_index < cases; ++case_index) {
        unsigned int operation, count, auxiliary, bytes, index;
        const unsigned char *data;
        if (size - offset < 16U) return 0;
        operation = read_u32(artifact + offset); count = read_u32(artifact + offset + 4U);
        auxiliary = read_u32(artifact + offset + 8U); bytes = read_u32(artifact + offset + 12U);
        offset += 16U;
        if (bytes > size - offset) return 0;
        data = artifact + offset; offset += bytes;
        if (operation == 1U) {
            unsigned int packed;
            unsigned short scale;
            if (!count || count > 256U || (auxiliary != 4U && auxiliary != 8U)) return 0;
            packed = (count * auxiliary + 7U) / 8U;
            if (bytes != 2U + packed + count * 4U) return 0;
            scale = (unsigned short)(data[0] | ((unsigned int)data[1] << 8));
            if (!gemma_numeric_dequantize(data + 2U, auxiliary, count, scale, output)) return 0;
            for (index = 0; index < count; ++index)
                if (output[index] != read_float(data + 2U + packed + index * 4U)) return 0;
            if (auxiliary == 4U) {
                signed char unpacked[256];
                if (!gemma_artifact_unpack_s4(data + 2U, count, unpacked, 256U)) return 0;
                for (index = 0; index < count; ++index)
                    if ((float)unpacked[index] * gemma_numeric_f16(scale) != output[index]) return 0;
            }
            if (gemma_numeric_dequantize(data + 2U, auxiliary, count, 0x7c00U, output) ||
                gemma_numeric_dequantize(data + 2U, auxiliary, count, 0U, output)) return 0;
        } else if (operation == 2U) {
            if (count != 256U || bytes != count * 16U) return 0;
            for (index = 0; index < count * 3U; ++index) inputs[index] = read_float(data + index * 4U);
            if (!gemma_numeric_rope(inputs, inputs + count, inputs + count * 2U, count, output)) return 0;
            for (index = 0; index < count; ++index)
                if (!close_float(output[index], read_float(data + (count * 3U + index) * 4U))) return 0;
            if (!gemma_numeric_rope(inputs, inputs + count, inputs + count * 2U, count, inputs)) return 0;
            for (index = 0; index < count; ++index)
                if (!close_float(inputs[index], output[index])) return 0;
        } else if (operation == 3U) {
            unsigned int layer, query, key;
            int success;
            if (bytes != 16U || (unsigned int)gemma_numeric_visible(read_u32(data), read_u32(data + 4),
                read_u32(data + 8)) != read_u32(data + 12)) return 0;
            layer = read_u32(data); query = read_u32(data + 4); key = read_u32(data + 8);
            success = gemma_numeric_mask(layer, query, 1, key, 1, output, 1);
            if (layer >= 34U || query >= 2048U || key >= 2048U) {
                if (success) return 0;
            } else if (!success || output[0] != (read_u32(data + 12) ? 0.0f : -65504.0f)) return 0;
            if (gemma_numeric_mask(layer, query, 1, key, 1, output, 0) ||
                gemma_numeric_mask(layer, query, 0xffffffffU, key, 1, output, 256U)) return 0;
        } else if (operation == 4U) {
            unsigned long long actual = 0, expected;
            int success;
            if (bytes != 28U) return 0;
            expected = read_u32(data + 20) | ((unsigned long long)read_u32(data + 24) << 32);
            success = gemma_numeric_kv_offset(read_u32(data), read_u32(data + 4), read_u32(data + 8),
                                              read_u32(data + 12), &actual);
            if ((unsigned int)success != read_u32(data + 16) || (success && actual != expected)) return 0;
        } else if (operation == 5U) {
            unsigned int token = 0xffffffffU;
            int success;
            if (count > 1024U || bytes != count * 4U) return 0;
            for (index = 0; index < count; ++index) inputs[index] = read_float(data + index * 4U);
            success = gemma_numeric_argmax(inputs, count, &token);
            if ((auxiliary == 0xffffffffU && success) ||
                (auxiliary != 0xffffffffU && (!success || token != auxiliary))) return 0;
        } else if (operation == 6U) {
            if (count != 65536U || bytes != count * 4U) return 0;
            for (index = 0; index < count; ++index) {
                union { float value; unsigned int bits; } actual;
                unsigned int expected = read_u32(data + index * 4U);
                actual.value = gemma_numeric_f16((unsigned short)index);
                if ((index & 0x7c00U) == 0x7c00U && (index & 1023U)) {
                    if ((actual.bits & 0x7f800000U) != 0x7f800000U || !(actual.bits & 0x7fffffU)) return 0;
                } else if (actual.bits != expected) return 0;
            }
        } else return 0;
        ++*passed;
    }
    return offset == size;
}