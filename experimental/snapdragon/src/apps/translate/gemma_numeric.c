#include "gemma_numeric.h"

float gemma_numeric_f16(unsigned short bits) {
    unsigned int exponent = (bits >> 10) & 31U, mantissa = bits & 1023U;
    union { unsigned int bits; float value; } result;
    unsigned int sign = (unsigned int)(bits & 0x8000U) << 16;
    if (!exponent) {
        if (!mantissa) { result.bits = sign; return result.value; }
        exponent = 113U;
        while (!(mantissa & 1024U)) { mantissa <<= 1; --exponent; }
        mantissa &= 1023U;
    } else if (exponent == 31U) exponent = 255U;
    else exponent += 112U;
    result.bits = sign | (exponent << 23) | (mantissa << 13);
    return result.value;
}

static int finite(float value) {
    union { float value; unsigned int bits; } representation;
    representation.value = value;
    return (representation.bits & 0x7f800000U) != 0x7f800000U;
}

int gemma_numeric_dequantize(const unsigned char *weights, unsigned int bits,
    unsigned int count, unsigned short scale, float *output) {
    unsigned int index;
    float multiplier = gemma_numeric_f16(scale);
    if (!weights || !output || !count || (bits != 4U && bits != 8U) ||
        !finite(multiplier) || multiplier <= 0.0f) return 0;
    for (index = 0; index < count; ++index) {
        unsigned int raw = bits == 4U ? (weights[index / 2U] >> ((index & 1U) * 4U)) & 15U : weights[index];
        int value = raw >= (1U << (bits - 1U)) ? (int)raw - (int)(1U << bits) : (int)raw;
        output[index] = (float)value * multiplier;
    }
    return 1;
}

int gemma_numeric_rope(const float *input, const float *cosine, const float *sine,
    unsigned int width, float *output) {
    unsigned int index, half = width / 2U;
    if (!input || !cosine || !sine || !output || !width || (width & 1U) || width > 256U) return 0;
    for (index = 0; index < half; ++index) {
        float left = input[index], right = input[index + half];
        output[index] = left * cosine[index] - right * sine[index];
        output[index + half] = right * cosine[index + half] + left * sine[index + half];
    }
    return 1;
}

int gemma_numeric_visible(unsigned int layer, unsigned int query, unsigned int key) {
    if (layer >= 34U || query >= 2048U || key > query) return 0;
    return (layer + 1U) % 6U == 0U || query - key < 1024U;
}

int gemma_numeric_mask(unsigned int layer, unsigned int query_start, unsigned int query_count,
    unsigned int key_start, unsigned int key_count, float *output, unsigned int capacity) {
    unsigned int query, key;
    if (!output || layer >= 34U || query_start >= 2048U || key_start >= 2048U ||
        !query_count || !key_count || query_count > 2048U - query_start ||
        key_count > 2048U - key_start || query_count * key_count > capacity) return 0;
    for (query = 0; query < query_count; ++query) {
        for (key = 0; key < key_count; ++key) {
            output[query * key_count + key] = gemma_numeric_visible(layer, query_start + query,
                key_start + key) ? 0.0f : -65504.0f;
        }
    }
    return 1;
}

int gemma_numeric_kv_offset(unsigned int layer, unsigned int head,
    unsigned int position, unsigned int channel, unsigned long long *offset) {
    unsigned int capacity;
    if (!offset || layer >= 34U || head >= 4U || position >= 2048U || channel >= 256U) return 0;
    capacity = (layer + 1U) % 6U == 0U ? 2048U : 1024U;
    *offset = ((unsigned long long)head * capacity + position % capacity) * 256U + channel;
    return 1;
}

int gemma_numeric_argmax(const float *values, unsigned int count, unsigned int *token) {
    unsigned int index, best = 0;
    if (!values || !token || !count || count > 262208U) return 0;
    for (index = 0; index < count; ++index) {
        if (!finite(values[index])) return 0;
        if (values[index] > values[best]) best = index;
    }
    *token = best;
    return 1;
}