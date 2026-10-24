#include "nm_fp32.h"

#ifdef NM_FP32_HVX
#include <hexagon_protos.h>
#include <hexagon_types.h>
#include <hvx_hexagon_protos.h>
#endif

typedef struct NmShape {
    uint32_t rank, dims[4];
    size_t count;
} NmShape;
typedef struct NmParameter {
    const uint32_t *values;
    uint32_t count;
} NmParameter;

static float from_bits(uint32_t bits) {
    union { uint32_t bits; float value; } result;
    result.bits = bits;
    return result.value;
}
static int finite(float value) {
    union { uint32_t bits; float value; } bits;
    bits.value = value;
    return (bits.bits & 0x7f800000U) != 0x7f800000U;
}
static float absolute(float value) { return value < 0.0f ? -value : value; }
static float exponential(float value) {
    float reduced, polynomial, factor;
    int power;
    if (value < -103.97208f) return 0.0f;
    if (value > 88.72283f) return from_bits(0x7f800000U);
    power = (int)(value * 1.4426950408889634f + (value < 0.0f ? -0.5f : 0.5f));
    reduced = (value - (float)power * 0.693145751953125f) - (float)power * 1.428606765330187e-6f;
    polynomial = 1.0f / 40320.0f;
    polynomial = 1.0f / 5040.0f + reduced * polynomial;
    polynomial = 1.0f / 720.0f + reduced * polynomial;
    polynomial = 1.0f / 120.0f + reduced * polynomial;
    polynomial = 1.0f / 24.0f + reduced * polynomial;
    polynomial = 1.0f / 6.0f + reduced * polynomial;
    polynomial = 0.5f + reduced * polynomial;
    polynomial = 1.0f + reduced * polynomial;
    polynomial = 1.0f + reduced * polynomial;
    if (power < -126) {
        factor = from_bits((uint32_t)(power + 151) << 23);
        return (polynomial * factor) * 0x1p-24f;
    }
    if (power == 128) return (polynomial * 0x1p127f) * 2.0f;
    factor = from_bits((uint32_t)(power + 127) << 23);
    return polynomial * factor;
}
static float sigmoid(float value) {
    float ratio = exponential(-absolute(value));
    return value >= 0.0f ? 1.0f / (1.0f + ratio) : ratio / (1.0f + ratio);
}
static float reciprocal_root(float value) {
    union { uint32_t bits; float value; } result;
    float normalized, root = 1.5f;
    int exponent;
    unsigned int index;
    result.value = value;
    exponent = (int)((result.bits >> 23) & 255U) - 127;
    if (exponent == -127) {
        result.value = value * 0x1p24f;
        exponent = (int)((result.bits >> 23) & 255U) - 127 - 24;
    }
    result.bits = (result.bits & 0x007fffffU) | 0x3f800000U;
    normalized = result.value;
    if (exponent & 1) { normalized *= 2.0f; --exponent; }
    for (index = 0; index < 6; ++index) root = 0.5f * (root + normalized / root);
    result.bits = (uint32_t)(127 - exponent / 2) << 23;
    return (1.0f / root) * result.value;
}
static void add_compensated(float value, float *sum, float *correction) {
    float adjusted = value - *correction;
    float next = *sum + adjusted;
    *correction = (next - *sum) - adjusted;
    *sum = next;
}
static int shape(NmShape *out, const uint32_t *record, uint32_t rank) {
    uint32_t index;
    out->rank = rank;
    out->count = 1;
    if (rank < 1 || rank > 4) return 0;
    for (index = 0; index < 4; ++index) {
        out->dims[index] = record[index];
        if (index < rank) {
            if (!record[index] || out->count > (size_t)0x7fffffffU / record[index]) return 0;
            out->count *= record[index];
        } else if (record[index]) return 0;
    }
    return 1;
}
static int same(const NmShape *a, const NmShape *b) {
    uint32_t index;
    if (a->rank != b->rank) return 0;
    for (index = 0; index < a->rank; ++index) if (a->dims[index] != b->dims[index]) return 0;
    return 1;
}
static size_t broadcast(size_t index, const NmShape *output, const NmShape *input) {
    size_t result = 0, stride = 1;
    uint32_t axis = output->rank;
    while (axis--) {
        uint32_t coordinate = (uint32_t)(index % output->dims[axis]);
        index /= output->dims[axis];
        if (axis + input->rank >= output->rank) {
            uint32_t input_axis = axis + input->rank - output->rank;
            if (input->dims[input_axis] != 1) result += coordinate * stride;
            stride *= input->dims[input_axis];
        }
    }
    return result;
}
static int can_broadcast(const NmShape *input, const NmShape *out) {
    uint32_t axis;
    if (input->rank > out->rank) return 0;
    for (axis = 0; axis < input->rank; ++axis)
        if (input->dims[axis] != 1 && input->dims[axis] != out->dims[axis + out->rank - input->rank]) return 0;
    return 1;
}
static int copy_span(float *output, const float *input, size_t count) {
    size_t index = 0;
#ifdef NM_FP32_HVX
    for (; index + 31 < count; index += 32)
        *(HVX_UVector *)(void *)(output + index) = *(const HVX_UVector *)(const void *)(input + index);
#endif
    for (; index < count; ++index) output[index] = input[index];
    for (index = 0; index < count; ++index) if (!finite(input[index])) return 2;
    return 0;
}
static void fill_span(float *output, float value, size_t count) {
    size_t index = 0;
#ifdef NM_FP32_HVX
    union { uint32_t bits; float value; } scalar_value;
    HVX_Vector vector;
    scalar_value.value = value;
    vector = Q6_V_vsplat_R((int)scalar_value.bits);
    for (; index + 31 < count; index += 32)
        *(HVX_UVector *)(void *)(output + index) = vector;
#endif
    for (; index < count; ++index) output[index] = value;
}
static uint32_t scalar(const NmParameter *parameters, uint32_t code, uint32_t fallback) {
    return parameters[code].count == 1 ? parameters[code].values[0] : fallback;
}
static int parameters_read(NmParameter *parameters, const uint32_t *metadata, size_t words) {
    size_t cursor = NM_FP32_META_BASE;
    uint32_t index, count = metadata[33];
    if (count > NM_FP32_MAX_PARAMS) return 0;
    for (index = 0; index <= NM_FP32_WEIGHT_TILES; ++index) {
        parameters[index].values = 0;
        parameters[index].count = 0;
    }
    for (index = 0; index < count; ++index) {
        uint32_t code, length;
        if (cursor + 2 > words) return 0;
        code = metadata[cursor++];
        length = metadata[cursor++];
        if (!code || code > NM_FP32_WEIGHT_TILES || !length || length > 16 ||
            parameters[code].values || length > words - cursor) return 0;
        if ((code == NM_FP32_AXIS || code == NM_FP32_EPSILON || code == NM_FP32_SCHEME ||
             code == NM_FP32_PAD_VALUE || code == NM_FP32_TRANSPOSE_A || code == NM_FP32_TRANSPOSE_B ||
             code == NM_FP32_BETA || code == NM_FP32_KEEP_DIMS ||
             code == NM_FP32_WEIGHT_TILES) && length != 1) return 0;
        if ((code == NM_FP32_EPSILON || code == NM_FP32_PAD_VALUE || code == NM_FP32_BETA) &&
            !finite(from_bits(metadata[cursor]))) return 0;
        parameters[code].values = metadata + cursor;
        parameters[code].count = length;
        cursor += length;
    }
    return cursor == words;
}
static int matrix(float *out, const float *a, const float *b, const float *bias,
                  const NmShape *as, const NmShape *bs, const NmShape *os,
                  size_t start, size_t end, int fully_connected,
                  unsigned int slice, unsigned int slices, int *blockwise, int packed) {
    size_t m, k, n, batches, index;
    uint32_t axis;
    if (as->rank < 2 || bs->rank < 2 || os->rank != as->rank) return 0;
    m = as->dims[as->rank - 2];
    k = as->dims[as->rank - 1];
    n = bs->dims[bs->rank - (fully_connected ? 2 : 1)];
    if (bs->dims[bs->rank - (fully_connected ? 1 : 2)] != k ||
        os->dims[os->rank - 2] != m || os->dims[os->rank - 1] != n) return 0;
    if (fully_connected && bs->rank != 2) return 0;
    if (packed && (fully_connected || n % 32)) return 0;
    if (!fully_connected && as->rank != bs->rank) return 0;
    batches = as->count / (m * k);
    for (axis = 0; axis + 2 < as->rank; ++axis)
        if (as->dims[axis] != os->dims[axis] || (!fully_connected && as->dims[axis] != bs->dims[axis])) return 0;
    if (batches * m * n != os->count) return 0;
#ifdef NM_FP32_HVX
    if (!fully_connected && n >= 32 && (!bias || !(n % 32))) {
        size_t column_blocks = (n + 31) / 32, row_blocks = (m + 3) / 4;
        size_t jobs = batches * row_blocks * column_blocks, job;
        *blockwise = 1;
        for (job = slice; job < jobs; job += slices) {
            size_t column = (job % column_blocks) * 32;
            size_t row = ((job / column_blocks) % row_blocks) * 4;
            size_t batch = job / column_blocks / row_blocks, inner, r, valid_rows = m - row;
            const float *left = a + (batch * m + row) * k;
            const float *right = b + batch * k * n + (packed ? (column / 32) * k * 32 : column);
            size_t weight_stride = packed ? 32 : n;
            size_t valid_columns = n - column, bulk_end;
            HVX_Vector s00, s01, s02, s03, s10, s11, s12, s13;
            HVX_Vector s20, s21, s22, s23, s30, s31, s32, s33;
            HVX_Vector rows[4];
            if (valid_rows > 4) valid_rows = 4;
            if (valid_columns > 32) valid_columns = 32;
            bulk_end = (k - (valid_columns < 32)) & ~(size_t)3;
            s00 = s01 = s02 = s03 = Q6_V_vzero();
            s10 = s11 = s12 = s13 = Q6_V_vzero();
            s20 = s21 = s22 = s23 = Q6_V_vzero();
            s30 = s31 = s32 = s33 = Q6_V_vzero();
#define NM_MAC_ROW(sum, row_, offset_) do { \
                union { uint32_t bits; float value; } factor; \
                HVX_Vector product; \
                factor.value = left[(row_) * k + inner + (offset_)]; \
                product = Q6_Vqf32_vmpy_VsfVsf(weights, Q6_V_vsplat_R((int)factor.bits)); \
                sum = Q6_Vqf32_vadd_Vqf32Vqf32(sum, product); \
            } while (0)
#define NM_MAC_FOUR(a_, b_, c_, d_, offset_, vector_) do { \
                HVX_Vector weights = *(const vector_ *)(const void *)(right + (inner + (offset_)) * weight_stride); \
                NM_MAC_ROW(a_, 0, offset_); NM_MAC_ROW(b_, 1, offset_); \
                NM_MAC_ROW(c_, 2, offset_); NM_MAC_ROW(d_, 3, offset_); \
            } while (0)
#define NM_MAC_SHORT(a_, b_, c_, d_, offset_) do { \
                HVX_Vector weights; \
                if (valid_columns < 32 && inner + (offset_) + 1 == k) { \
                    union { HVX_Vector vector; float lanes[32]; } tail; \
                    size_t lane; \
                    for (lane = 0; lane < 32; ++lane) \
                        tail.lanes[lane] = lane < valid_columns ? \
                            right[(inner + (offset_)) * weight_stride + lane] : 0.0f; \
                    weights = tail.vector; \
                } else weights = *(const HVX_UVector *)(const void *)(right + (inner + (offset_)) * weight_stride); \
                NM_MAC_ROW(a_, 0, offset_); \
                if (valid_rows > 1) NM_MAC_ROW(b_, 1, offset_); \
                if (valid_rows > 2) NM_MAC_ROW(c_, 2, offset_); \
                if (valid_rows > 3) NM_MAC_ROW(d_, 3, offset_); \
            } while (0)
#define NM_DENSE_WINDOWS(vector_) do { \
                inner = 0; \
                while (inner < bulk_end) { \
                    size_t limit = bulk_end; \
                    if (packed && limit - inner > 128) { \
                        size_t ahead; \
                        limit = inner + 128; \
                        ahead = k - limit; \
                        if (ahead > 128) ahead = 128; \
                        Q6_l2fetch_AP((void *)(right + limit * 32), \
                                     HEXAGON_V64_CREATE_H(0, 128, 128, ahead)); \
                    } \
                    for (; inner < limit; inner += 4) { \
                        NM_MAC_FOUR(s00, s10, s20, s30, 0, vector_); \
                        NM_MAC_FOUR(s01, s11, s21, s31, 1, vector_); \
                        NM_MAC_FOUR(s02, s12, s22, s32, 2, vector_); \
                        NM_MAC_FOUR(s03, s13, s23, s33, 3, vector_); \
                    } \
                } \
            } while (0)
            if (valid_rows == 4) {
                if (!((uintptr_t)right & 127U) && !(weight_stride & 31U)) {
                    NM_DENSE_WINDOWS(HVX_Vector);
                } else {
                    NM_DENSE_WINDOWS(HVX_UVector);
                }
            } else {
                for (inner = 0; inner < bulk_end; inner += 4) {
                    NM_MAC_SHORT(s00, s10, s20, s30, 0);
                    NM_MAC_SHORT(s01, s11, s21, s31, 1);
                    NM_MAC_SHORT(s02, s12, s22, s32, 2);
                    NM_MAC_SHORT(s03, s13, s23, s33, 3);
                }
            }
            for (; inner < k; ++inner) switch (inner & 3) {
            case 0: NM_MAC_SHORT(s00, s10, s20, s30, 0); break;
            case 1: NM_MAC_SHORT(s01, s11, s21, s31, 0); break;
            case 2: NM_MAC_SHORT(s02, s12, s22, s32, 0); break;
            default: NM_MAC_SHORT(s03, s13, s23, s33, 0); break;
            }
#undef NM_MAC_SHORT
#undef NM_DENSE_WINDOWS
#undef NM_MAC_FOUR
#undef NM_MAC_ROW
#define NM_REDUCE(a_, b_, c_, d_) Q6_Vqf32_vadd_Vqf32Vqf32( \
                Q6_Vqf32_vadd_Vqf32Vqf32(a_, b_), Q6_Vqf32_vadd_Vqf32Vqf32(c_, d_))
            rows[0] = NM_REDUCE(s00, s01, s02, s03);
            rows[1] = NM_REDUCE(s10, s11, s12, s13);
            rows[2] = NM_REDUCE(s20, s21, s22, s23);
            rows[3] = NM_REDUCE(s30, s31, s32, s33);
#undef NM_REDUCE
            for (r = 0; r < valid_rows; ++r) {
                float *destination = out + (batch * m + row + r) * n + column;
                size_t lane;
                HVX_Vector value = rows[r];
                if (bias) value = Q6_Vqf32_vadd_Vqf32Vsf(value,
                    *(const HVX_UVector *)(const void *)(bias + column));
                if (valid_columns == 32) {
                    *(HVX_UVector *)(void *)destination = Q6_Vsf_equals_Vqf32(value);
                } else {
                    union { HVX_Vector vector; float lanes[32]; } tail;
                    tail.vector = Q6_Vsf_equals_Vqf32(value);
                    for (lane = 0; lane < valid_columns; ++lane) destination[lane] = tail.lanes[lane];
                }
                for (lane = 0; lane < valid_columns; ++lane) if (!finite(destination[lane])) return 2;
            }
        }
        return 1;
    }
#else
    (void)slice;
    (void)slices;
    (void)blockwise;
#endif
    for (index = start; index < end; ++index) {
        size_t column = index % n, row = (index / n) % m, batch = index / (m * n), inner;
        float partial[4] = {0, 0, 0, 0};
        const float *left = a + (batch * m + row) * k;
        const float *right = b + (fully_connected ? column * k : batch * k * n +
                                 (packed ? (column / 32) * k * 32 + column % 32 : column));
        for (inner = 0; inner < k; ++inner)
            partial[inner & 3] += left[inner] * right[fully_connected ? inner : inner * (packed ? 32 : n)];
        out[index] = ((partial[0] + partial[1]) + (partial[2] + partial[3])) + (bias ? bias[column] : 0.0f);
    }
    return 1;
}

int nm_fp32_execute(const uint32_t *metadata, size_t words, const void *const *inputs,
                    const size_t *counts, size_t ninputs, float *out, size_t output_count,
                    unsigned int slice, unsigned int slices) {
    NmShape input[4], output;
    NmParameter parameters[NM_FP32_WEIGHT_TILES + 1];
    size_t start, end, index, inner;
    uint32_t op, i;
    const float *a, *b = 0;
    if (!metadata || words < NM_FP32_META_BASE || metadata[0] != NM_FP32_META_MAGIC ||
        (metadata[1] != NM_FP32_META_VERSION && metadata[1] != NM_FP32_PACKED_VERSION) ||
        !ninputs || ninputs > NM_FP32_MAX_INPUTS ||
        metadata[3] != ninputs || !inputs || !counts || !out || !slices || slice >= slices ||
        !shape(&output, metadata + 5, metadata[4]) || output.count != output_count ||
        !parameters_read(parameters, metadata, words)) return 1;
    for (i = 0; i < 4; ++i) {
        const uint32_t *record = metadata + 9 + i * 6;
        if (i < ninputs) {
            if (!inputs[i] || !shape(input + i, record + 2, record[1]) || input[i].count != counts[i] ||
                (record[0] != NM_FP32_DTYPE_FLOAT && record[0] != NM_FP32_DTYPE_INT)) return 1;
        } else for (inner = 0; inner < 6; ++inner) if (record[inner]) return 1;
    }
    op = metadata[2];
    if (parameters[NM_FP32_WEIGHT_TILES].values &&
        (metadata[1] != NM_FP32_PACKED_VERSION || op != NM_FP32_MATMUL ||
         parameters[NM_FP32_WEIGHT_TILES].count != 1 || parameters[NM_FP32_WEIGHT_TILES].values[0] != 32))
        return 1;
    for (i = 0; i < ninputs; ++i) {
        uint32_t dtype = metadata[9 + i * 6];
        if (dtype != NM_FP32_DTYPE_FLOAT && !(op == NM_FP32_GATHER_OP && i == 1 && dtype == NM_FP32_DTYPE_INT))
            return 1;
    }
    start = (output_count / slices) * slice + (output_count % slices) * slice / slices;
    end = (output_count / slices) * (slice + 1) + (output_count % slices) * (slice + 1) / slices;
    a = (const float *)inputs[0];
    if (ninputs > 1) b = (const float *)inputs[1];
    if (op == NM_FP32_MATMUL || op == NM_FP32_FC) {
        const float *bias = ninputs == 3 ? (const float *)inputs[2] : 0;
        int blockwise = 0, status;
        if ((op == NM_FP32_MATMUL && ninputs != 2) || (op == NM_FP32_FC && ninputs != 2 && ninputs != 3) ||
            scalar(parameters, NM_FP32_TRANSPOSE_A, 0) || scalar(parameters, NM_FP32_TRANSPOSE_B, 0) ||
            (bias && counts[2] != output.dims[output.rank - 1])) return 1;
        status = matrix(out, a, b, bias, input, input + 1, &output, start, end,
                        op == NM_FP32_FC, slice, slices, &blockwise,
                        parameters[NM_FP32_WEIGHT_TILES].values != 0);
        if (!status) return 1;
        if (status == 2) return 2;
        if (blockwise) {
            start = end = 0;
        }
    } else if (op == NM_FP32_ADD || op == NM_FP32_MULTIPLY || op == NM_FP32_SUBTRACT) {
        if (ninputs != 2 || !can_broadcast(input, &output) || !can_broadcast(input + 1, &output)) return 1;
#ifdef NM_FP32_HVX
        if (!(output.dims[output.rank - 1] % 32) &&
            (input[0].dims[input[0].rank - 1] == 1 || !(input[0].dims[input[0].rank - 1] % 32)) &&
            (input[1].dims[input[1].rank - 1] == 1 || !(input[1].dims[input[1].rank - 1] % 32))) {
            for (index = (size_t)slice * 32; index < output_count; index += (size_t)slices * 32) {
                HVX_Vector x, y, result;
                size_t ai = input[0].count == output_count ? index : broadcast(index, &output, input);
                size_t bi = input[1].count == output_count ? index : broadcast(index, &output, input + 1);
                size_t lane;
                if (input[0].dims[input[0].rank - 1] == 1) {
                    union { uint32_t bits; float value; } scalar_value;
                    scalar_value.value = a[ai]; x = Q6_V_vsplat_R((int)scalar_value.bits);
                } else x = *(const HVX_UVector *)(const void *)(a + ai);
                if (input[1].dims[input[1].rank - 1] == 1) {
                    union { uint32_t bits; float value; } scalar_value;
                    scalar_value.value = b[bi]; y = Q6_V_vsplat_R((int)scalar_value.bits);
                } else y = *(const HVX_UVector *)(const void *)(b + bi);
                result = op == NM_FP32_ADD ? Q6_Vqf32_vadd_VsfVsf(x, y) :
                    op == NM_FP32_MULTIPLY ? Q6_Vqf32_vmpy_VsfVsf(x, y) : Q6_Vqf32_vsub_VsfVsf(x, y);
                *(HVX_UVector *)(void *)(out + index) = Q6_Vsf_equals_Vqf32(result);
                for (lane = 0; lane < 32; ++lane) if (!finite(out[index + lane])) return 2;
            }
            start = end = 0;
        }
#endif
        for (index = start; index < end; ++index) {
            float x = a[input[0].count == output_count ? index : broadcast(index, &output, input)];
            float y = b[input[1].count == output_count ? index : broadcast(index, &output, input + 1)];
            out[index] = op == NM_FP32_ADD ? x + y : op == NM_FP32_MULTIPLY ? x * y : x - y;
        }
    } else if (op == NM_FP32_RESHAPE) {
        if (ninputs != 1 || input[0].count != output_count) return 1;
        if (copy_span(out + start, a + start, end - start)) return 2;
        start = end = 0;
    } else if (op == NM_FP32_RELU || op == NM_FP32_SIGMOID || op == NM_FP32_TANH || op == NM_FP32_RSQRT) {
        if (ninputs != 1 || !same(input, &output)) return 1;
        for (index = start; index < end; ++index) {
            float value = a[index];
            if (!finite(value)) return 2;
            if (op == NM_FP32_RELU) out[index] = value < 0.0f ? 0.0f : value;
            else if (op == NM_FP32_SIGMOID) out[index] = sigmoid(value);
            else if (op == NM_FP32_RSQRT) {
                if (value <= 0.0f) return 2;
                out[index] = reciprocal_root(value);
            } else {
                float ratio = exponential(-2.0f * absolute(value));
                float result = (1.0f - ratio) / (1.0f + ratio);
                out[index] = value < 0.0f ? -result : result;
            }
        }
    } else if (op == NM_FP32_NORM || op == NM_FP32_SOFTMAX) {
        size_t width = input[0].dims[input[0].rank - 1], rows = output_count / width, row;
        float epsilon = from_bits(scalar(parameters, NM_FP32_EPSILON, 0x3727c5acU));
        float beta = from_bits(scalar(parameters, NM_FP32_BETA, 0x3f800000U));
        if (!same(input, &output)) return 1;
        if (op == NM_FP32_NORM) {
            if (ninputs != 3 || counts[1] != width || counts[2] != width || !finite(epsilon) || epsilon <= 0 ||
                parameters[NM_FP32_AXES].count != 1 ||
                parameters[NM_FP32_AXES].values[0] != input[0].rank - 1) return 1;
        } else if (ninputs != 1 || !finite(beta) || beta <= 0 ||
                   scalar(parameters, NM_FP32_AXIS, input[0].rank - 1) != input[0].rank - 1) return 1;
        for (row = slice; row < rows; row += slices) {
            const float *source = a + row * width;
            float sum = 0, correction = 0, mean, variance = 0, inverse, maximum = source[0];
            for (inner = 0; inner < width; ++inner) {
                if (!finite(source[inner])) return 2;
                if (op == NM_FP32_NORM) add_compensated(source[inner], &sum, &correction);
                else if (source[inner] > maximum) maximum = source[inner];
            }
            if (op == NM_FP32_NORM) {
                mean = sum / (float)width;
                correction = 0;
                for (inner = 0; inner < width; ++inner) {
                    float centered = source[inner] - mean;
                    add_compensated(centered * centered, &variance, &correction);
                }
                inverse = reciprocal_root(variance / (float)width + epsilon);
                for (inner = 0; inner < width; ++inner)
                    out[row * width + inner] = (source[inner] - mean) * inverse * b[inner] +
                        ((const float *)inputs[2])[inner];
            } else {
                for (inner = 0; inner < width; ++inner) {
                    float value = exponential((source[inner] - maximum) * beta);
                    out[row * width + inner] = value;
                    add_compensated(value, &sum, &correction);
                }
                if (!finite(sum) || sum <= 0) return 2;
                inverse = 1.0f / sum;
                for (inner = 0; inner < width; ++inner) out[row * width + inner] *= inverse;
            }
        }
        start = end = 0;
    } else if (op == NM_FP32_TRANSPOSE || op == NM_FP32_SLICE || op == NM_FP32_PAD) {
        uint32_t rank = output.rank, permutation[4] = {0}, before[4] = {0};
        if (ninputs != 1 || input[0].rank != rank) return 1;
        if (op == NM_FP32_TRANSPOSE) {
            uint32_t used = 0;
            if (parameters[NM_FP32_PERM].count != rank) return 1;
            for (i = 0; i < rank; ++i) {
                permutation[i] = parameters[NM_FP32_PERM].values[i];
                if (permutation[i] >= rank || (used & (1U << permutation[i])) ||
                    output.dims[i] != input[0].dims[permutation[i]]) return 1;
                used |= 1U << permutation[i];
            }
        } else if (op == NM_FP32_SLICE) {
            if (parameters[NM_FP32_RANGES].count != rank * 3) return 1;
            for (i = 0; i < rank; ++i) {
                const uint32_t *range = parameters[NM_FP32_RANGES].values + i * 3;
                if (range[2] != 1 || range[0] >= range[1] || range[1] > input[0].dims[i] ||
                    output.dims[i] != range[1] - range[0]) return 1;
                before[i] = range[0];
            }
        } else {
            if (parameters[NM_FP32_PAD_AMOUNT].count != rank * 2 || scalar(parameters, NM_FP32_SCHEME, 0)) return 1;
            for (i = 0; i < rank; ++i) {
                const uint32_t *pad = parameters[NM_FP32_PAD_AMOUNT].values + i * 2;
                if ((uint64_t)pad[0] + pad[1] + input[0].dims[i] != output.dims[i]) return 1;
                before[i] = pad[0];
            }
        }
        if (op == NM_FP32_SLICE) {
            uint32_t changed = 0, axis = 0;
            size_t tail = 1, outer = 1, block;
            for (i = 0; i < rank; ++i) if (before[i] || input[0].dims[i] != output.dims[i]) { ++changed; axis = i; }
            if (changed <= 1) {
                for (i = axis + 1; i < rank; ++i) tail *= output.dims[i];
                for (i = 0; i < axis; ++i) outer *= output.dims[i];
                for (block = slice; block < outer; block += slices) {
                    size_t src = (block * input[0].dims[axis] + before[axis]) * tail;
                    size_t dst = block * output.dims[axis] * tail;
                    if (copy_span(out + dst, a + src, output.dims[axis] * tail)) return 2;
                }
                start = end = 0;
            }
        } else if (op == NM_FP32_PAD) {
            size_t width = output.dims[rank - 1], input_width = input[0].dims[rank - 1];
            size_t rows = output_count / width, block;
            float value = from_bits(scalar(parameters, NM_FP32_PAD_VALUE, 0));
            for (block = slice; block < rows; block += slices) {
                uint32_t coordinates[4] = {0};
                size_t position = block, source = 0;
                int padding = 0;
                float *destination = out + block * width;
                for (i = rank - 1; i; --i) {
                    coordinates[i - 1] = (uint32_t)(position % output.dims[i - 1]);
                    position /= output.dims[i - 1];
                }
                for (i = 0; i + 1 < rank; ++i) {
                    if (coordinates[i] < before[i] ||
                        coordinates[i] - before[i] >= input[0].dims[i]) padding = 1;
                    else source = source * input[0].dims[i] + coordinates[i] - before[i];
                }
                if (padding) fill_span(destination, value, width);
                else {
                    fill_span(destination, value, before[rank - 1]);
                    if (copy_span(destination + before[rank - 1], a + source * input_width, input_width)) return 2;
                    fill_span(destination + before[rank - 1] + input_width, value,
                              width - before[rank - 1] - input_width);
                }
            }
            start = end = 0;
        } else if (op == NM_FP32_TRANSPOSE) {
            uint32_t suffix = rank, prefix_identity = 1;
            size_t tail = 1, block;
            while (suffix && permutation[suffix - 1] == suffix - 1) tail *= output.dims[--suffix];
            if (tail > 1) {
                size_t blocks = output_count / tail;
                for (block = slice; block < blocks; block += slices) {
                    uint32_t coordinates[4] = {0}, mapped[4] = {0};
                    size_t position = block, source = 0;
                    for (i = suffix; i; --i) {
                        coordinates[i - 1] = (uint32_t)(position % output.dims[i - 1]);
                        position /= output.dims[i - 1];
                    }
                    for (i = 0; i < suffix; ++i) mapped[permutation[i]] = coordinates[i];
                    for (i = 0; i < suffix; ++i) source = source * input[0].dims[i] + mapped[i];
                    if (copy_span(out + block * tail, a + source * tail, tail)) return 2;
                }
                start = end = 0;
            } else if (rank >= 2 && permutation[rank - 2] == rank - 1 && permutation[rank - 1] == rank - 2) {
                size_t rows = input[0].dims[rank - 2], columns = input[0].dims[rank - 1];
                size_t rb = (rows + 15) / 16, cb = (columns + 15) / 16;
                size_t batches = output_count / (rows * columns), jobs = batches * rb * cb;
                for (i = 0; i + 2 < rank; ++i) if (permutation[i] != i) prefix_identity = 0;
                if (prefix_identity) {
                    for (block = slice; block < jobs; block += slices) {
                        size_t batch = block / (rb * cb), r0 = ((block / cb) % rb) * 16, c0 = (block % cb) * 16;
                        size_t r, c;
                        for (r = r0; r < rows && r < r0 + 16; ++r)
                            for (c = c0; c < columns && c < c0 + 16; ++c) {
                                float value = a[batch * rows * columns + r * columns + c];
                                if (!finite(value)) return 2;
                                out[batch * rows * columns + c * rows + r] = value;
                            }
                    }
                    start = end = 0;
                }
            }
        }
        for (index = start; index < end; ++index) {
            uint32_t coordinates[4] = {0}, input_coordinates[4] = {0};
            size_t position = index, source = 0;
            int padding = 0;
            for (i = rank; i; --i) { coordinates[i - 1] = (uint32_t)(position % output.dims[i - 1]); position /= output.dims[i - 1]; }
            for (i = 0; i < rank; ++i) {
                if (op == NM_FP32_TRANSPOSE) input_coordinates[permutation[i]] = coordinates[i];
                else if (op == NM_FP32_SLICE) input_coordinates[i] = coordinates[i] + before[i];
                else if (coordinates[i] < before[i] || coordinates[i] - before[i] >= input[0].dims[i]) padding = 1;
                else input_coordinates[i] = coordinates[i] - before[i];
            }
            for (i = 0; i < rank; ++i) source = source * input[0].dims[i] + input_coordinates[i];
            out[index] = padding ? from_bits(scalar(parameters, NM_FP32_PAD_VALUE, 0)) : a[source];
        }
    } else if (op == NM_FP32_CONCAT) {
        uint32_t axis = scalar(parameters, NM_FP32_AXIS, 0);
        size_t tail = 1, axis_total = 0;
        if (axis >= output.rank) return 1;
        for (i = axis + 1; i < output.rank; ++i) tail *= output.dims[i];
        for (i = 0; i < ninputs; ++i) {
            uint32_t dimension;
            if (input[i].rank != output.rank) return 1;
            for (dimension = 0; dimension < output.rank; ++dimension)
                if (dimension != axis && input[i].dims[dimension] != output.dims[dimension]) return 1;
            axis_total += input[i].dims[axis];
        }
        if (axis_total != output.dims[axis]) return 1;
        {
            size_t outer, outer_count = output_count / (axis_total * tail);
            for (outer = slice; outer < outer_count; outer += slices) {
                size_t destination = outer * axis_total * tail;
                for (i = 0; i < ninputs; ++i) {
                    size_t span = input[i].dims[axis] * tail;
                    if (copy_span(out + destination, (const float *)inputs[i] + outer * span, span)) return 2;
                    destination += span;
                }
            }
            start = end = 0;
        }
    } else if (op == NM_FP32_GATHER_OP) {
        const int32_t *indices = (const int32_t *)inputs[1];
        size_t width, rows;
        if (ninputs != 2 || input[0].rank != 2 || input[1].rank != 1 || output.rank != 2 ||
            scalar(parameters, NM_FP32_AXIS, 0)) return 1;
        rows = input[0].dims[0]; width = input[0].dims[1];
        if (output.dims[0] != counts[1] || output.dims[1] != width) return 1;
        for (index = start; index < end; ++index) {
            int32_t row = indices[index / width];
            if (row < 0 || (size_t)row >= rows) return 1;
            out[index] = a[(size_t)row * width + index % width];
        }
    } else if (op == NM_FP32_CONV || op == NM_FP32_DEPTHWISE) {
        size_t kh, kw, channels, filters;
        uint32_t stride[2], dilation[2], pad[2] = {0};
        int pointwise_done = 0;
        if (ninputs < 2 || ninputs > 3 || input[0].rank != 4 || input[1].rank != 4 || output.rank != 4 ||
            parameters[NM_FP32_STRIDE].count != 2 || parameters[NM_FP32_DILATION].count != 2 ||
            parameters[NM_FP32_PAD_AMOUNT].count != 4) return 1;
        for (i = 0; i < 2; ++i) {
            stride[i] = parameters[NM_FP32_STRIDE].values[i];
            dilation[i] = parameters[NM_FP32_DILATION].values[i];
            pad[i] = parameters[NM_FP32_PAD_AMOUNT].values[i * 2];
            if (!stride[i] || !dilation[i]) return 1;
        }
        kh = input[1].dims[0]; kw = input[1].dims[1];
        channels = input[0].dims[3]; filters = input[1].dims[3];
        if (output.dims[0] != input[0].dims[0] || output.dims[3] != filters ||
            (op == NM_FP32_CONV && input[1].dims[2] != channels) ||
            (op == NM_FP32_DEPTHWISE && (input[1].dims[2] != 1 || filters % channels)) ||
            (ninputs == 3 && counts[2] != filters)) return 1;
        for (i = 0; i < 2; ++i) {
            uint64_t extent = (uint64_t)input[0].dims[i + 1] + parameters[NM_FP32_PAD_AMOUNT].values[i * 2] +
                parameters[NM_FP32_PAD_AMOUNT].values[i * 2 + 1];
            uint64_t kernel = (uint64_t)(input[1].dims[i] - 1) * dilation[i] + 1;
            if (extent < kernel || (extent - kernel) / stride[i] + 1 != output.dims[i + 1]) return 1;
        }
        if (op == NM_FP32_CONV && kh == 1 && kw == 1 && stride[0] == 1 && stride[1] == 1 &&
            !parameters[NM_FP32_PAD_AMOUNT].values[0] && !parameters[NM_FP32_PAD_AMOUNT].values[1] &&
            !parameters[NM_FP32_PAD_AMOUNT].values[2] && !parameters[NM_FP32_PAD_AMOUNT].values[3]) {
            size_t rows = input[0].count / channels;
            NmShape left = {2, {0}, 0}, weight = {2, {0}, 0}, destination = {2, {0}, 0};
            int blockwise = 0, status;
            left.dims[0] = (uint32_t)rows; left.dims[1] = (uint32_t)channels; left.count = input[0].count;
            weight.dims[0] = (uint32_t)channels; weight.dims[1] = (uint32_t)filters; weight.count = input[1].count;
            destination.dims[0] = (uint32_t)rows; destination.dims[1] = (uint32_t)filters; destination.count = output_count;
            status = matrix(out, a, b, ninputs == 3 ? (const float *)inputs[2] : 0,
                            &left, &weight, &destination, start, end, 0, slice, slices, &blockwise, 0);
            if (!status) return 1;
            if (status == 2) return 2;
            if (blockwise) start = end = 0;
            pointwise_done = 1;
        }
#ifdef NM_FP32_HVX
        if (op == NM_FP32_DEPTHWISE && filters == channels && !(channels % 32)) {
            size_t job, jobs = output_count / 32;
            for (job = slice; job < jobs; job += slices) {
                size_t index0 = job * 32, channel = index0 % channels;
                size_t x = (index0 / channels) % output.dims[2];
                size_t y = (index0 / channels / output.dims[2]) % output.dims[1];
                size_t batch = index0 / channels / output.dims[2] / output.dims[1], ky, kx, term = 0, lane;
                HVX_Vector s0 = Q6_V_vzero(), s1 = Q6_V_vzero(), s2 = Q6_V_vzero(), s3 = Q6_V_vzero();
                for (ky = 0; ky < kh; ++ky) for (kx = 0; kx < kw; ++kx) {
                    int64_t sy = (int64_t)(y * stride[0] + ky * dilation[0]) - pad[0];
                    int64_t sx = (int64_t)(x * stride[1] + kx * dilation[1]) - pad[1];
                    HVX_Vector value, weight, product;
                    if (sy < 0 || sx < 0 || sy >= input[0].dims[1] || sx >= input[0].dims[2]) continue;
                    value = *(const HVX_UVector *)(const void *)(a +
                        ((batch * input[0].dims[1] + (size_t)sy) * input[0].dims[2] + (size_t)sx) * channels + channel);
                    weight = *(const HVX_UVector *)(const void *)(b + (ky * kw + kx) * channels + channel);
                    product = Q6_Vqf32_vmpy_VsfVsf(value, weight);
                    switch (term++ & 3) {
                    case 0: s0 = Q6_Vqf32_vadd_Vqf32Vqf32(s0, product); break;
                    case 1: s1 = Q6_Vqf32_vadd_Vqf32Vqf32(s1, product); break;
                    case 2: s2 = Q6_Vqf32_vadd_Vqf32Vqf32(s2, product); break;
                    default: s3 = Q6_Vqf32_vadd_Vqf32Vqf32(s3, product); break;
                    }
                }
                {
                    HVX_Vector value = Q6_Vqf32_vadd_Vqf32Vqf32(
                        Q6_Vqf32_vadd_Vqf32Vqf32(s0, s1), Q6_Vqf32_vadd_Vqf32Vqf32(s2, s3));
                    if (ninputs == 3) value = Q6_Vqf32_vadd_Vqf32Vsf(value,
                        *(const HVX_UVector *)(const void *)((const float *)inputs[2] + channel));
                    *(HVX_UVector *)(void *)(out + index0) = Q6_Vsf_equals_Vqf32(value);
                }
                for (lane = 0; lane < 32; ++lane) if (!finite(out[index0 + lane])) return 2;
            }
            start = end = 0;
        }
#endif
        for (index = start; index < end && !pointwise_done; ++index) {
            size_t filter = index % filters, x = (index / filters) % output.dims[2];
            size_t y = (index / filters / output.dims[2]) % output.dims[1];
            size_t batch = index / filters / output.dims[2] / output.dims[1], ky, kx, channel;
            float partial[4] = {0, 0, 0, 0};
            size_t term = 0;
            for (ky = 0; ky < kh; ++ky) for (kx = 0; kx < kw; ++kx) {
                int64_t sy = (int64_t)(y * stride[0] + ky * dilation[0]) - pad[0];
                int64_t sx = (int64_t)(x * stride[1] + kx * dilation[1]) - pad[1];
                if (sy < 0 || sx < 0 || sy >= input[0].dims[1] || sx >= input[0].dims[2]) continue;
                if (op == NM_FP32_DEPTHWISE) {
                    channel = filter / (filters / channels);
                    partial[term++ & 3] += a[((batch * input[0].dims[1] + (size_t)sy) * input[0].dims[2] + (size_t)sx) * channels + channel] *
                        b[(ky * kw + kx) * filters + filter];
                } else for (channel = 0; channel < channels; ++channel)
                    partial[term++ & 3] += a[((batch * input[0].dims[1] + (size_t)sy) * input[0].dims[2] + (size_t)sx) * channels + channel] *
                        b[((ky * kw + kx) * channels + channel) * filters + filter];
            }
            out[index] = (partial[0] + partial[1]) + (partial[2] + partial[3]) +
                (ninputs == 3 ? ((const float *)inputs[2])[filter] : 0.0f);
        }
    } else return 1;
    for (index = start; index < end; ++index) if (!finite(out[index])) return 2;
    if (op == NM_FP32_NORM || op == NM_FP32_SOFTMAX) {
        size_t width = output.dims[output.rank - 1], rows = output_count / width, row;
        for (row = slice; row < rows; row += slices)
            for (inner = 0; inner < width; ++inner) if (!finite(out[row * width + inner])) return 2;
    }
    return 0;
}
