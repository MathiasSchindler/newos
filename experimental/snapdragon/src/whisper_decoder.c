#include "whisper_decoder.h"
#include "whisper_frontend.h"
#include "../../../src/shared/math.h"
#include "../../../src/shared/concurrency.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long usize;
typedef float f32x4 __attribute__((vector_size(16)));
typedef _Float16 f16x4 __attribute__((vector_size(8)));

enum {
    DECODER_LAYERS = 4,
    DECODER_WIDTH = 384,
    DECODER_HEADS = 6,
    DECODER_HEAD_WIDTH = 64,
    DECODER_MLP_WIDTH = 1536,
    DECODER_ENCODER_FRAMES = 1500,
    DECODER_VOCAB_SIZE = 51865,
    DECODER_FLOAT_COUNT = 29553024
};

#define DECODER_WEIGHT_MAGIC 0x3154574345445757ULL
#define DECODER_TOKEN_MAGIC 0x314b4f5443454457ULL
#define DECODER_SOT 50258U
#define DECODER_GERMAN 50261U
#define DECODER_TRANSCRIBE 50359U
#define DECODER_NO_SPEECH 50362U
#define DECODER_NO_TIMESTAMPS 50363U
#define DECODER_EOT 50257U

__declspec(dllimport) int CloseHandle(void *handle);
__declspec(dllimport) void *CreateFileA(
    const char *name, u32 access, u32 sharing, void *security,
    u32 creation, u32 attributes, void *template_file
);
__declspec(dllimport) int ReadFile(
    void *handle, void *buffer, u32 size, u32 *read, void *overlapped
);
__declspec(dllimport) int QueryPerformanceCounter(long long *value);
__declspec(dllimport) void *VirtualAlloc(
    void *address, usize size, u32 allocation_type, u32 protect
);

typedef struct DecoderLayerWeights {
    float *self_norm_weight;
    float *self_norm_bias;
    float *self_k_weight;
    float *self_q_weight;
    float *self_q_bias;
    float *self_v_weight;
    float *self_v_bias;
    float *self_out_weight;
    float *self_out_bias;
    float *cross_norm_weight;
    float *cross_norm_bias;
    float *cross_k_weight;
    float *cross_q_weight;
    float *cross_q_bias;
    float *cross_v_weight;
    float *cross_v_bias;
    float *cross_out_weight;
    float *cross_out_bias;
    float *final_norm_weight;
    float *final_norm_bias;
    float *fc1_weight;
    float *fc1_bias;
    float *fc2_weight;
    float *fc2_bias;
} DecoderLayerWeights;

typedef struct DecoderWeights {
    float *token_embedding;
    float *position_embedding;
    float *encoder_norm_weight;
    float *encoder_norm_bias;
    float *decoder_norm_weight;
    float *decoder_norm_bias;
    DecoderLayerWeights layers[DECODER_LAYERS];
} DecoderWeights;

static float *weight_storage;
static DecoderWeights weights;
static u32 *token_offsets;
static u8 *token_bytes;
static u32 token_byte_count;
static float encoder_hidden[DECODER_ENCODER_FRAMES * DECODER_WIDTH] __attribute__((aligned(16)));
static float key_cache[DECODER_LAYERS][WHISPER_DECODER_MAX_TOKENS][DECODER_WIDTH] __attribute__((aligned(16)));
static float value_cache[DECODER_LAYERS][WHISPER_DECODER_MAX_TOKENS][DECODER_WIDTH] __attribute__((aligned(16)));
static u16 cross_key_cache_storage[DECODER_LAYERS][DECODER_ENCODER_FRAMES][DECODER_WIDTH] __attribute__((aligned(16)));
static u16 cross_value_cache_storage[DECODER_LAYERS][DECODER_ENCODER_FRAMES][DECODER_WIDTH] __attribute__((aligned(16)));
static const u16 *cross_key_cache;
static const u16 *cross_value_cache;
static float hidden[DECODER_WIDTH] __attribute__((aligned(16)));
static float normalized[DECODER_WIDTH] __attribute__((aligned(16)));
static float query[DECODER_WIDTH] __attribute__((aligned(16)));
static float key[DECODER_WIDTH] __attribute__((aligned(16)));
static float value[DECODER_WIDTH] __attribute__((aligned(16)));
static float attended[DECODER_WIDTH] __attribute__((aligned(16)));
static float projected[DECODER_WIDTH] __attribute__((aligned(16)));
static float mlp_hidden[DECODER_MLP_WIDTH] __attribute__((aligned(16)));
static float attention_scores[DECODER_HEADS][DECODER_ENCODER_FRAMES] __attribute__((aligned(16)));
static u16 generated_tokens[WHISPER_DECODER_MAX_TOKENS];
static WhisperDecoderProfile decoder_profile;
static RtTaskPool decoder_pool;
static int decoder_pool_ready;
static float logit_maxima[RT_TASK_POOL_MAX_WORKERS];
static u32 logit_tokens[RT_TASK_POOL_MAX_WORKERS];

static u32 read_u32_le(const u8 *bytes) {
    return (u32)bytes[0] | ((u32)bytes[1] << 8U) |
        ((u32)bytes[2] << 16U) | ((u32)bytes[3] << 24U);
}

static u64 read_u64_le(const u8 *bytes) {
    return (u64)read_u32_le(bytes) | ((u64)read_u32_le(bytes + 4U) << 32U);
}

static int read_exact(void *handle, void *buffer, u32 size) {
    u8 *bytes = buffer;
    while (size != 0U) {
        u32 count = 0U;
        if (!ReadFile(handle, bytes, size, &count, 0) || count == 0U) return 0;
        bytes += count;
        size -= count;
    }
    return 1;
}

static void *open_bundle(const char *primary, const char *fallback) {
    void *invalid = (void *)(usize)-1;
    void *handle = CreateFileA(primary, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    if (handle == invalid) handle = CreateFileA(fallback, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    return handle;
}

static float *take_weights(float **cursor, u32 count) {
    float *result = *cursor;
    *cursor += count;
    return result;
}

static int bind_weights(void) {
    float *cursor = weight_storage;
    u32 layer;
    weights.token_embedding = take_weights(&cursor, DECODER_VOCAB_SIZE * DECODER_WIDTH);
    weights.position_embedding = take_weights(&cursor, WHISPER_DECODER_MAX_TOKENS * DECODER_WIDTH);
    weights.encoder_norm_weight = take_weights(&cursor, DECODER_WIDTH);
    weights.encoder_norm_bias = take_weights(&cursor, DECODER_WIDTH);
    weights.decoder_norm_weight = take_weights(&cursor, DECODER_WIDTH);
    weights.decoder_norm_bias = take_weights(&cursor, DECODER_WIDTH);
    for (layer = 0U; layer < DECODER_LAYERS; ++layer) {
        DecoderLayerWeights *item = &weights.layers[layer];
        item->self_norm_weight = take_weights(&cursor, DECODER_WIDTH);
        item->self_norm_bias = take_weights(&cursor, DECODER_WIDTH);
        item->self_k_weight = take_weights(&cursor, DECODER_WIDTH * DECODER_WIDTH);
        item->self_q_weight = take_weights(&cursor, DECODER_WIDTH * DECODER_WIDTH);
        item->self_q_bias = take_weights(&cursor, DECODER_WIDTH);
        item->self_v_weight = take_weights(&cursor, DECODER_WIDTH * DECODER_WIDTH);
        item->self_v_bias = take_weights(&cursor, DECODER_WIDTH);
        item->self_out_weight = take_weights(&cursor, DECODER_WIDTH * DECODER_WIDTH);
        item->self_out_bias = take_weights(&cursor, DECODER_WIDTH);
        item->cross_norm_weight = take_weights(&cursor, DECODER_WIDTH);
        item->cross_norm_bias = take_weights(&cursor, DECODER_WIDTH);
        item->cross_k_weight = take_weights(&cursor, DECODER_WIDTH * DECODER_WIDTH);
        item->cross_q_weight = take_weights(&cursor, DECODER_WIDTH * DECODER_WIDTH);
        item->cross_q_bias = take_weights(&cursor, DECODER_WIDTH);
        item->cross_v_weight = take_weights(&cursor, DECODER_WIDTH * DECODER_WIDTH);
        item->cross_v_bias = take_weights(&cursor, DECODER_WIDTH);
        item->cross_out_weight = take_weights(&cursor, DECODER_WIDTH * DECODER_WIDTH);
        item->cross_out_bias = take_weights(&cursor, DECODER_WIDTH);
        item->final_norm_weight = take_weights(&cursor, DECODER_WIDTH);
        item->final_norm_bias = take_weights(&cursor, DECODER_WIDTH);
        item->fc1_weight = take_weights(&cursor, DECODER_MLP_WIDTH * DECODER_WIDTH);
        item->fc1_bias = take_weights(&cursor, DECODER_MLP_WIDTH);
        item->fc2_weight = take_weights(&cursor, DECODER_WIDTH * DECODER_MLP_WIDTH);
        item->fc2_bias = take_weights(&cursor, DECODER_WIDTH);
    }
    return cursor == weight_storage + DECODER_FLOAT_COUNT;
}

int whisper_decoder_load(void) {
    void *invalid = (void *)(usize)-1;
    void *handle;
    u8 header[20];
    u32 offset_bytes;
    if (weight_storage != 0 && token_offsets != 0) return 1;
    handle = open_bundle(
        "experimental/snapdragon/models/whisper-tiny/decoder-f32/weights-f32.bin",
        "../models/whisper-tiny/decoder-f32/weights-f32.bin"
    );
    if (handle == invalid) return 0;
    if (!read_exact(handle, header, 16U) || read_u64_le(header) != DECODER_WEIGHT_MAGIC ||
        read_u32_le(header + 8U) != 1U || read_u32_le(header + 12U) != DECODER_FLOAT_COUNT) {
        CloseHandle(handle);
        return -1;
    }
    weight_storage = VirtualAlloc(
        0, (usize)DECODER_FLOAT_COUNT * sizeof(float), 0x3000U, 0x04U
    );
    if (weight_storage == 0 || !read_exact(
            handle, weight_storage, DECODER_FLOAT_COUNT * sizeof(float))) {
        CloseHandle(handle);
        return -1;
    }
    CloseHandle(handle);
    if (!bind_weights()) return -1;

    handle = open_bundle(
        "experimental/snapdragon/models/whisper-tiny/decoder-f32/token-bytes.bin",
        "../models/whisper-tiny/decoder-f32/token-bytes.bin"
    );
    if (handle == invalid) return 0;
    if (!read_exact(handle, header, 20U) || read_u64_le(header) != DECODER_TOKEN_MAGIC ||
        read_u32_le(header + 8U) != 1U || read_u32_le(header + 12U) != DECODER_VOCAB_SIZE) {
        CloseHandle(handle);
        return -1;
    }
    token_byte_count = read_u32_le(header + 16U);
    offset_bytes = (DECODER_VOCAB_SIZE + 1U) * sizeof(u32);
    token_offsets = VirtualAlloc(0, offset_bytes + token_byte_count, 0x3000U, 0x04U);
    if (token_offsets == 0 || !read_exact(handle, token_offsets, offset_bytes + token_byte_count)) {
        CloseHandle(handle);
        return -1;
    }
    CloseHandle(handle);
    token_bytes = (u8 *)token_offsets + offset_bytes;
    if (token_offsets[DECODER_VOCAB_SIZE] != token_byte_count) return -1;
    (void)rt_task_pool_init(&decoder_pool, 0U);
    decoder_pool_ready = 1;
    return 1;
}

static float dot_product(const float *left, const float *right, u32 count) {
    f32x4 sum = {0.0f, 0.0f, 0.0f, 0.0f};
    u32 index;
    for (index = 0U; index + 4U <= count; index += 4U) {
        sum += *(const f32x4 *)(left + index) * *(const f32x4 *)(right + index);
    }
    return sum[0] + sum[1] + sum[2] + sum[3];
}

static float dot_product_unrolled(const float *left, const float *right, u32 count) {
    f32x4 sum0 = {0.0f, 0.0f, 0.0f, 0.0f};
    f32x4 sum1 = {0.0f, 0.0f, 0.0f, 0.0f};
    f32x4 sum2 = {0.0f, 0.0f, 0.0f, 0.0f};
    f32x4 sum3 = {0.0f, 0.0f, 0.0f, 0.0f};
    u32 index;
    for (index = 0U; index + 16U <= count; index += 16U) {
        sum0 += *(const f32x4 *)(left + index) *
            *(const f32x4 *)(right + index);
        sum1 += *(const f32x4 *)(left + index + 4U) *
            *(const f32x4 *)(right + index + 4U);
        sum2 += *(const f32x4 *)(left + index + 8U) *
            *(const f32x4 *)(right + index + 8U);
        sum3 += *(const f32x4 *)(left + index + 12U) *
            *(const f32x4 *)(right + index + 12U);
    }
    for (; index + 4U <= count; index += 4U) {
        sum0 += *(const f32x4 *)(left + index) * *(const f32x4 *)(right + index);
    }
    sum0 += sum1;
    sum2 += sum3;
    sum0 += sum2;
    return sum0[0] + sum0[1] + sum0[2] + sum0[3];
}

typedef struct MatrixVectorContext {
    const float *matrix;
    const float *input;
    const float *bias;
    float *output;
    u32 columns;
} MatrixVectorContext;

static int matrix_vector_range(
    size_t begin,
    size_t end,
    unsigned int worker_index,
    void *arg
) {
    const MatrixVectorContext *context = (const MatrixVectorContext *)arg;
    size_t row;
    (void)worker_index;
    for (row = begin; row < end; ++row) {
        context->output[row] = dot_product(
            context->matrix + row * context->columns,
            context->input,
            context->columns
        ) + (context->bias == 0 ? 0.0f : context->bias[row]);
    }
    return 0;
}

static void matrix_vector_serial(
    const float *matrix,
    const float *input,
    const float *bias,
    float *output,
    u32 rows,
    u32 columns
) {
    u32 row;
    for (row = 0U; row < rows; ++row) {
        output[row] = dot_product(matrix + row * columns, input, columns) +
            (bias == 0 ? 0.0f : bias[row]);
    }
}

static void matrix_vector_parallel(
    const float *matrix,
    const float *input,
    const float *bias,
    float *output,
    u32 rows,
    u32 columns
) {
    MatrixVectorContext context;
    context.matrix = matrix;
    context.input = input;
    context.bias = bias;
    context.output = output;
    context.columns = columns;
    (void)rt_parallel_for(
        &decoder_pool, rows, 1U, matrix_vector_range, &context
    );
}

static void matrix_vector_fp16(
    const float *matrix,
    const float *input,
    const float *bias,
    u16 *output,
    u32 rows,
    u32 columns
) {
    u32 row;
    for (row = 0U; row < rows; ++row) {
        float value = dot_product(matrix + row * columns, input, columns) +
            (bias == 0 ? 0.0f : bias[row]);
        output[row] = whisper_frontend_float_to_half(value);
    }
}

static void layer_norm(
    const float *input,
    const float *scale,
    const float *bias,
    float *output
) {
    double sum = 0.0;
    double squared_sum = 0.0;
    double inverse;
    u32 index;
    for (index = 0U; index < DECODER_WIDTH; ++index) sum += input[index];
    sum /= DECODER_WIDTH;
    for (index = 0U; index < DECODER_WIDTH; ++index) {
        double centered = input[index] - sum;
        squared_sum += centered * centered;
    }
    inverse = 1.0 / math_sqrt(squared_sum / DECODER_WIDTH + 1.0e-5);
    for (index = 0U; index < DECODER_WIDTH; ++index) {
        output[index] = (float)(((input[index] - sum) * inverse) * scale[index] + bias[index]);
    }
}

static void softmax(float *values, u32 count) {
    float maximum = values[0];
    double sum = 0.0;
    u32 index;
    for (index = 1U; index < count; ++index) {
        if (values[index] > maximum) maximum = values[index];
    }
    for (index = 0U; index < count; ++index) {
        values[index] = (float)math_exp((double)values[index] - maximum);
        sum += values[index];
    }
    for (index = 0U; index < count; ++index) values[index] = (float)(values[index] / sum);
}

static float gelu(float input) {
    float absolute = input < 0.0f ? -input : input;
    float factor;
    float polynomial;
    float erf;
    absolute *= 0.7071067811865475244f;
    factor = 1.0f / (1.0f + 0.3275911f * absolute);
    polynomial = 1.061405429f * factor - 1.453152027f;
    polynomial = polynomial * factor + 1.421413741f;
    polynomial = polynomial * factor - 0.284496736f;
    polynomial = polynomial * factor + 0.254829592f;
    erf = 1.0f - polynomial * factor * (float)math_exp(-(double)absolute * absolute);
    if (input < 0.0f) erf = -erf;
    return input * 0.5f * (1.0f + erf);
}

static void normalize_encoder(const u16 *encoder_output) {
    u32 frame;
    for (frame = 0U; frame < DECODER_ENCODER_FRAMES; ++frame) {
        u32 channel;
        for (channel = 0U; channel < DECODER_WIDTH; ++channel) {
            hidden[channel] = whisper_frontend_half_to_float(
                encoder_output[frame * DECODER_WIDTH + channel]
            );
        }
        layer_norm(
            hidden, weights.encoder_norm_weight, weights.encoder_norm_bias,
            encoder_hidden + frame * DECODER_WIDTH
        );
    }
}

static int prepare_cross_attention_cache_range(
    size_t begin,
    size_t end,
    unsigned int worker_index,
    void *arg
) {
    size_t item_index;
    (void)worker_index;
    (void)arg;
    for (item_index = begin; item_index < end; ++item_index) {
        u32 layer = (u32)(item_index / DECODER_ENCODER_FRAMES);
        u32 frame = (u32)(item_index % DECODER_ENCODER_FRAMES);
        const DecoderLayerWeights *item = &weights.layers[layer];
        const float *encoder = encoder_hidden + frame * DECODER_WIDTH;
        matrix_vector_fp16(
            item->cross_k_weight, encoder, 0,
            cross_key_cache_storage[layer][frame], DECODER_WIDTH, DECODER_WIDTH
        );
        matrix_vector_fp16(
            item->cross_v_weight, encoder, item->cross_v_bias,
            cross_value_cache_storage[layer][frame], DECODER_WIDTH, DECODER_WIDTH
        );
    }
    return 0;
}

static void prepare_cross_attention_cache(void) {
    (void)rt_parallel_for(
        &decoder_pool, DECODER_LAYERS * DECODER_ENCODER_FRAMES, 16U,
        prepare_cross_attention_cache_range, 0
    );
    cross_key_cache = &cross_key_cache_storage[0][0][0];
    cross_value_cache = &cross_value_cache_storage[0][0][0];
}

static void import_cross_attention_cache(const u16 *keys, const u16 *values) {
    cross_key_cache = keys;
    cross_value_cache = values;
}

static float dot_product_fp16(const float *left, const u16 *right, u32 count) {
    f32x4 sum = {0.0f, 0.0f, 0.0f, 0.0f};
    u32 index;
    for (index = 0U; index + 4U <= count; index += 4U) {
        f32x4 converted = __builtin_convertvector(
            *(const f16x4 *)(right + index), f32x4
        );
        sum += *(const f32x4 *)(left + index) * converted;
    }
    return sum[0] + sum[1] + sum[2] + sum[3];
}

static void self_attention(u32 layer, u32 position, const DecoderLayerWeights *item) {
    u32 head;
    u32 index;
    layer_norm(hidden, item->self_norm_weight, item->self_norm_bias, normalized);
    matrix_vector_serial(item->self_q_weight, normalized, item->self_q_bias, query, DECODER_WIDTH, DECODER_WIDTH);
    matrix_vector_serial(item->self_k_weight, normalized, 0, key, DECODER_WIDTH, DECODER_WIDTH);
    matrix_vector_serial(item->self_v_weight, normalized, item->self_v_bias, value, DECODER_WIDTH, DECODER_WIDTH);
    for (index = 0U; index < DECODER_WIDTH; ++index) {
        key_cache[layer][position][index] = key[index];
        value_cache[layer][position][index] = value[index];
    }
    for (head = 0U; head < DECODER_HEADS; ++head) {
        u32 token;
        u32 lane;
        const float *head_query = query + head * DECODER_HEAD_WIDTH;
        for (token = 0U; token <= position; ++token) {
            attention_scores[head][token] = dot_product(
                head_query,
                key_cache[layer][token] + head * DECODER_HEAD_WIDTH,
                DECODER_HEAD_WIDTH
            ) * 0.125f;
        }
        softmax(attention_scores[head], position + 1U);
        for (lane = 0U; lane < DECODER_HEAD_WIDTH; ++lane) {
            double sum = 0.0;
            for (token = 0U; token <= position; ++token) {
                sum += (double)attention_scores[head][token] *
                    value_cache[layer][token][head * DECODER_HEAD_WIDTH + lane];
            }
            attended[head * DECODER_HEAD_WIDTH + lane] = (float)sum;
        }
    }
    matrix_vector_serial(
        item->self_out_weight, attended, item->self_out_bias,
        projected, DECODER_WIDTH, DECODER_WIDTH
    );
    for (index = 0U; index < DECODER_WIDTH; ++index) hidden[index] += projected[index];
}

typedef struct CrossAttentionContext {
    u32 layer;
} CrossAttentionContext;

static int cross_attention_head_range(
    size_t begin,
    size_t end,
    unsigned int worker_index,
    void *arg
) {
    const CrossAttentionContext *context = (const CrossAttentionContext *)arg;
    size_t head_index;
    (void)worker_index;
    for (head_index = begin; head_index < end; ++head_index) {
        u32 head = (u32)head_index;
        u32 layer = context->layer;
        u32 frame;
        u32 lane;
        const u16 *layer_keys = cross_key_cache +
            layer * DECODER_ENCODER_FRAMES * DECODER_WIDTH;
        const u16 *layer_values = cross_value_cache +
            layer * DECODER_ENCODER_FRAMES * DECODER_WIDTH;
        for (frame = 0U; frame < DECODER_ENCODER_FRAMES; ++frame) {
            attention_scores[head][frame] = dot_product_fp16(
                query + head * DECODER_HEAD_WIDTH,
                layer_keys + frame * DECODER_WIDTH + head * DECODER_HEAD_WIDTH,
                DECODER_HEAD_WIDTH
            ) * 0.125f;
        }
        softmax(attention_scores[head], DECODER_ENCODER_FRAMES);
        for (lane = 0U; lane < DECODER_HEAD_WIDTH; ++lane) {
            attended[head * DECODER_HEAD_WIDTH + lane] = 0.0f;
        }
        for (frame = 0U; frame < DECODER_ENCODER_FRAMES; ++frame) {
            const u16 *frame_value =
                layer_values + frame * DECODER_WIDTH + head * DECODER_HEAD_WIDTH;
            f32x4 factor = {
                attention_scores[head][frame], attention_scores[head][frame],
                attention_scores[head][frame], attention_scores[head][frame]
            };
            for (lane = 0U; lane < DECODER_HEAD_WIDTH; lane += 4U) {
                f32x4 converted = __builtin_convertvector(
                    *(const f16x4 *)(frame_value + lane), f32x4
                );
                *(f32x4 *)(attended + head * DECODER_HEAD_WIDTH + lane) +=
                    factor * converted;
            }
        }
    }
    return 0;
}

static void cross_attention(u32 layer, const DecoderLayerWeights *item) {
    CrossAttentionContext context;
    u32 index;
    layer_norm(hidden, item->cross_norm_weight, item->cross_norm_bias, normalized);
    matrix_vector_parallel(item->cross_q_weight, normalized, item->cross_q_bias, query, DECODER_WIDTH, DECODER_WIDTH);
    context.layer = layer;
    (void)rt_parallel_for(
        &decoder_pool, DECODER_HEADS, 1U, cross_attention_head_range, &context
    );
    matrix_vector_parallel(
        item->cross_out_weight, attended, item->cross_out_bias,
        projected, DECODER_WIDTH, DECODER_WIDTH
    );
    for (index = 0U; index < DECODER_WIDTH; ++index) hidden[index] += projected[index];
}

static void feed_forward(const DecoderLayerWeights *item) {
    u32 index;
    layer_norm(hidden, item->final_norm_weight, item->final_norm_bias, normalized);
    matrix_vector_parallel(
        item->fc1_weight, normalized, item->fc1_bias,
        mlp_hidden, DECODER_MLP_WIDTH, DECODER_WIDTH
    );
    for (index = 0U; index < DECODER_MLP_WIDTH; ++index) mlp_hidden[index] = gelu(mlp_hidden[index]);
    matrix_vector_parallel(
        item->fc2_weight, mlp_hidden, item->fc2_bias,
        projected, DECODER_WIDTH, DECODER_MLP_WIDTH
    );
    for (index = 0U; index < DECODER_WIDTH; ++index) hidden[index] += projected[index];
}

static int suppressed_token(u32 token, int first_generated) {
    static const u16 suppressed[] = {
        1, 2, 7, 8, 9, 10, 14, 25, 26, 27, 28, 29, 31, 58, 59, 60, 61, 62, 63,
        90, 91, 92, 93, 359, 503, 522, 542, 873, 893, 902, 918, 922, 931, 1350,
        1853, 1982, 2460, 2627, 3246, 3253, 3268, 3536, 3846, 3961, 4183, 4667,
        6585, 6647, 7273, 9061, 9383, 10428, 10929, 11938, 12033, 12331, 12562,
        13793, 14157, 14635, 15265, 15618, 16553, 16604, 18362, 18956, 20075,
        21675, 22520, 26130, 26161, 26435, 28279, 29464, 31650, 32302, 32470,
        36865, 42863, 47425, 49870, 50254, 50258, 50358, 50359, 50360, 50361,
        50363
    };
    u32 index;
    if (token >= 50364U) return 1;
    if (token == DECODER_NO_SPEECH) return !first_generated;
    if (first_generated && token == 220U) return 1;
    for (index = 0U; index < sizeof(suppressed) / sizeof(suppressed[0]); ++index) {
        if (token == suppressed[index]) return 1;
    }
    return 0;
}

typedef struct LogitContext {
    int first_generated;
    u32 partition_count;
    u32 forbidden_count;
    u16 forbidden_tokens[WHISPER_DECODER_MAX_TOKENS];
} LogitContext;

static void collect_no_repeat_tokens(u32 position, LogitContext *context) {
    u32 start;
    context->forbidden_count = 0U;
    if (position < 6U) return;
    for (start = 4U; start + 3U <= position; ++start) {
        u32 index;
        u16 candidate;
        if (generated_tokens[start] != generated_tokens[position - 2U] ||
            generated_tokens[start + 1U] != generated_tokens[position - 1U] ||
            generated_tokens[start + 2U] != generated_tokens[position]) {
            continue;
        }
        candidate = generated_tokens[start + 3U];
        for (index = 0U; index < context->forbidden_count; ++index) {
            if (context->forbidden_tokens[index] == candidate) break;
        }
        if (index == context->forbidden_count) {
            context->forbidden_tokens[context->forbidden_count++] = candidate;
        }
    }
}

static int logit_partition_range(
    size_t begin,
    size_t end,
    unsigned int worker_index,
    void *arg
) {
    const LogitContext *context = (const LogitContext *)arg;
    size_t partition;
    (void)worker_index;
    for (partition = begin; partition < end; ++partition) {
        u32 token = (u32)((DECODER_VOCAB_SIZE * partition) / context->partition_count);
        u32 token_end = (u32)((DECODER_VOCAB_SIZE * (partition + 1U)) /
            context->partition_count);
        float maximum = -3.402823466e+38f;
        u32 best = DECODER_EOT;
        for (; token < token_end; ++token) {
            float logit;
            u32 forbidden_index;
            if (suppressed_token(token, context->first_generated)) continue;
            for (forbidden_index = 0U; forbidden_index < context->forbidden_count;
                 ++forbidden_index) {
                if (token == context->forbidden_tokens[forbidden_index]) break;
            }
            if (forbidden_index != context->forbidden_count) continue;
            logit = dot_product_unrolled(
                weights.token_embedding + token * DECODER_WIDTH,
                normalized,
                DECODER_WIDTH
            );
            if (logit > maximum || (logit == maximum && token < best)) {
                maximum = logit;
                best = token;
            }
        }
        logit_maxima[partition] = maximum;
        logit_tokens[partition] = best;
    }
    return 0;
}

static u32 decoder_step(u32 token, u32 position, int first_generated) {
    const float *embedding = weights.token_embedding + token * DECODER_WIDTH;
    const float *position_values = weights.position_embedding + position * DECODER_WIDTH;
    float maximum = -3.402823466e+38f;
    u32 best = DECODER_EOT;
    u32 layer;
    u32 index;
    LogitContext logit_context;
    long long start;
    long long end;
    for (index = 0U; index < DECODER_WIDTH; ++index) {
        hidden[index] = embedding[index] + position_values[index];
    }
    for (layer = 0U; layer < DECODER_LAYERS; ++layer) {
        QueryPerformanceCounter(&start);
        self_attention(layer, position, &weights.layers[layer]);
        QueryPerformanceCounter(&end);
        decoder_profile.self_attention_ticks += (u64)(end - start);
        QueryPerformanceCounter(&start);
        cross_attention(layer, &weights.layers[layer]);
        QueryPerformanceCounter(&end);
        decoder_profile.cross_attention_ticks += (u64)(end - start);
        QueryPerformanceCounter(&start);
        feed_forward(&weights.layers[layer]);
        QueryPerformanceCounter(&end);
        decoder_profile.feed_forward_ticks += (u64)(end - start);
    }
    QueryPerformanceCounter(&start);
    layer_norm(hidden, weights.decoder_norm_weight, weights.decoder_norm_bias, normalized);
    logit_context.first_generated = first_generated;
    logit_context.partition_count = rt_task_pool_width(&decoder_pool);
    collect_no_repeat_tokens(position, &logit_context);
    if (logit_context.partition_count > RT_TASK_POOL_MAX_WORKERS) {
        logit_context.partition_count = RT_TASK_POOL_MAX_WORKERS;
    }
    (void)rt_parallel_for(
        &decoder_pool, logit_context.partition_count, 1U,
        logit_partition_range, &logit_context
    );
    for (index = 0U; index < logit_context.partition_count; ++index) {
        if (logit_maxima[index] > maximum ||
            (logit_maxima[index] == maximum && logit_tokens[index] < best)) {
            maximum = logit_maxima[index];
            best = logit_tokens[index];
        }
    }
    QueryPerformanceCounter(&end);
    decoder_profile.logits_ticks += (u64)(end - start);
    decoder_profile.decoder_steps += 1U;
    return best;
}

static void write_token(u32 token, WhisperDecoderWrite write_output) {
    u32 start;
    u32 end;
    if (token >= DECODER_VOCAB_SIZE || write_output == 0) return;
    start = token_offsets[token];
    end = token_offsets[token + 1U];
    if (end > start && end <= token_byte_count) {
        write_output((const char *)token_bytes + start, end - start);
    }
}

static int whisper_decoder_transcribe_impl(
    const u16 *encoder_output,
    const u16 *cross_keys,
    const u16 *cross_values,
    u32 maximum_tokens,
    WhisperDecoderWrite write_output
) {
    static const u16 prompt[] = {
        DECODER_SOT, DECODER_GERMAN, DECODER_TRANSCRIBE, DECODER_NO_TIMESTAMPS
    };
    u32 position = 0U;
    u32 next = DECODER_EOT;
    u32 index;
    long long start;
    long long end;
    if (weight_storage == 0 || token_offsets == 0 ||
        (encoder_output == 0 && (cross_keys == 0 || cross_values == 0))) return -1;
    if (maximum_tokens > WHISPER_DECODER_MAX_TOKENS - sizeof(prompt) / sizeof(prompt[0])) {
        maximum_tokens = WHISPER_DECODER_MAX_TOKENS - sizeof(prompt) / sizeof(prompt[0]);
    }
    decoder_profile.encoder_normalize_ticks = 0U;
    decoder_profile.cross_cache_ticks = 0U;
    decoder_profile.self_attention_ticks = 0U;
    decoder_profile.cross_attention_ticks = 0U;
    decoder_profile.feed_forward_ticks = 0U;
    decoder_profile.logits_ticks = 0U;
    decoder_profile.decoder_steps = 0U;
    decoder_profile.worker_count = rt_task_pool_width(&decoder_pool);
    if (cross_keys != 0 && cross_values != 0) {
        QueryPerformanceCounter(&start);
        import_cross_attention_cache(cross_keys, cross_values);
        QueryPerformanceCounter(&end);
        decoder_profile.cross_cache_ticks = (u64)(end - start);
    } else {
        QueryPerformanceCounter(&start);
        normalize_encoder(encoder_output);
        QueryPerformanceCounter(&end);
        decoder_profile.encoder_normalize_ticks = (u64)(end - start);
        QueryPerformanceCounter(&start);
        prepare_cross_attention_cache();
        QueryPerformanceCounter(&end);
        decoder_profile.cross_cache_ticks = (u64)(end - start);
    }
    for (index = 0U; index < sizeof(prompt) / sizeof(prompt[0]); ++index) {
        generated_tokens[position] = prompt[index];
        next = decoder_step(prompt[index], position, index + 1U == sizeof(prompt) / sizeof(prompt[0]));
        ++position;
    }
        for (index = 0U; index < maximum_tokens && next != DECODER_EOT &&
            next != DECODER_NO_SPEECH; ++index) {
        generated_tokens[position] = (u16)next;
        write_token(next, write_output);
        next = decoder_step(next, position, 0);
        ++position;
    }
    return (int)index;
}

int whisper_decoder_transcribe(
    const u16 *encoder_output,
    u32 maximum_tokens,
    WhisperDecoderWrite write_output
) {
    return whisper_decoder_transcribe_impl(
        encoder_output, 0, 0, maximum_tokens, write_output
    );
}

int whisper_decoder_transcribe_with_cross_cache(
    const u16 *cross_keys,
    const u16 *cross_values,
    u32 maximum_tokens,
    WhisperDecoderWrite write_output
) {
    return whisper_decoder_transcribe_impl(
        0, cross_keys, cross_values, maximum_tokens, write_output
    );
}

const WhisperDecoderProfile *whisper_decoder_get_profile(void) {
    return &decoder_profile;
}

void whisper_decoder_shutdown(void) {
    if (!decoder_pool_ready) return;
    rt_task_pool_destroy(&decoder_pool);
    decoder_pool_ready = 0;
}
