#include "whisper_decoder.h"
#include "whisper_artifact.h"
#include "whisper_frontend.h"
#include "math.h"
#include "concurrency.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long usize;
typedef float f32x4 __attribute__((vector_size(16)));
typedef _Float16 f16x4 __attribute__((vector_size(8)));

#define DECODER_SOT 50258U
#define DECODER_GERMAN 50261U
#define DECODER_TRANSCRIBE 50359U
#define DECODER_NO_SPEECH 50362U
#define DECODER_NO_TIMESTAMPS 50363U
#define DECODER_EOT 50257U
#define DECODER_TEXT_TOKEN_LIMIT 50364U

__declspec(dllimport) int CloseHandle(void *handle);
__declspec(dllimport) void *CreateFileA(
    const char *name, u32 access, u32 sharing, void *security,
    u32 creation, u32 attributes, void *template_file
);
__declspec(dllimport) int ReadFile(
    void *handle, void *buffer, u32 size, u32 *read, void *overlapped
);
__declspec(dllimport) int QueryPerformanceCounter(long long *value);
__declspec(dllimport) int QueryPerformanceFrequency(long long *value);
#if defined(WHISPER_DECODER_TEST_ALLOCATOR)
void *whisper_decoder_test_allocate(
    void *address, usize size, u32 allocation_type, u32 protect
);
int whisper_decoder_test_free(void *address, usize size, u32 free_type);
#define DECODER_ALLOCATE whisper_decoder_test_allocate
#define DECODER_FREE whisper_decoder_test_free
#else
__declspec(dllimport) void *VirtualAlloc(
    void *address, usize size, u32 allocation_type, u32 protect
);
__declspec(dllimport) int VirtualFree(void *address, usize size, u32 free_type);
#define DECODER_ALLOCATE VirtualAlloc
#define DECODER_FREE VirtualFree
#endif

typedef struct DecoderLayerWeights {
    u16 *self_norm_weight;
    u16 *self_norm_bias;
    u16 *self_k_weight;
    u16 *self_q_weight;
    u16 *self_q_bias;
    u16 *self_v_weight;
    u16 *self_v_bias;
    u16 *self_out_weight;
    u16 *self_out_bias;
    u16 *cross_norm_weight;
    u16 *cross_norm_bias;
    u16 *cross_k_weight;
    u16 *cross_q_weight;
    u16 *cross_q_bias;
    u16 *cross_v_weight;
    u16 *cross_v_bias;
    u16 *cross_out_weight;
    u16 *cross_out_bias;
    u16 *final_norm_weight;
    u16 *final_norm_bias;
    u16 *fc1_weight;
    u16 *fc1_bias;
    u16 *fc2_weight;
    u16 *fc2_bias;
} DecoderLayerWeights;

typedef struct DecoderWeights {
    u16 *token_embedding;
    u16 *position_embedding;
    u16 *encoder_norm_weight;
    u16 *encoder_norm_bias;
    u16 *decoder_norm_weight;
    u16 *decoder_norm_bias;
    DecoderLayerWeights *layers;
} DecoderWeights;

struct WhisperDecoder {
    WhisperDecoderCancelled cancelled;
    WhisperDecoderTrace trace;
    void *trace_context;
    void *cancel_context;
    WhisperModelConfig model;
    void *weight_allocation;
    u16 *weight_storage;
    void *token_allocation;
    u32 *token_offsets;
    u8 *token_bytes;
    u32 token_byte_count;
    void *scratch_allocation;
    DecoderWeights weights;
    float *encoder_hidden;
    float *key_cache;
    float *value_cache;
    u16 *cross_key_cache_storage;
    u16 *cross_value_cache_storage;
    const u16 *cross_key_cache;
    const u16 *cross_value_cache;
    int cross_cache_head_major;
    float *hidden;
    float *normalized;
    float *query;
    float *key;
    float *value;
    float *attended;
    float *projected;
    float *mlp_hidden;
    float *attention_scores;
    u16 *generated_tokens;
    u16 *selected_tokens;
    u16 *prefix_tokens;
    float *prefix_hidden;
    u32 prefix_count;
    float *gumbel_cache;
    u32 gumbel_seeds[WHISPER_DECODER_MAX_TOKENS];
    u8 gumbel_valid[WHISPER_DECODER_MAX_TOKENS];
    WhisperDecoderProfile profile;
    WhisperDecoderMlpOffload mlp_offload;
    void *mlp_offload_context;
    WhisperDecoderCrossAttentionOffload cross_attention_offload;
    void *cross_attention_offload_context;
    WhisperDecoderFusedCrossMlpOffload fused_cross_mlp_offload;
    void *fused_cross_mlp_offload_context;
    WhisperDecoderLogitsOffload logits_offload;
    void *logits_offload_context;
    WhisperDecoderSelfAttentionOffload self_attention_offload;
    void *self_attention_offload_context;
    u64 counter_frequency;
    RtTaskPool pool;
    int pool_ready;
    float logit_maxima[RT_TASK_POOL_MAX_WORKERS];
    u32 logit_tokens[RT_TASK_POOL_MAX_WORKERS];
    u8 excluded_tokens[(DECODER_TEXT_TOKEN_LIMIT + 7U) / 8U];
};

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
    void *handle = whisper_artifact_open_read(primary);
#ifndef WHISPER_RUNTIME_ONLY
    void *invalid = (void *)(usize)-1;
    if (handle == invalid) handle = CreateFileA(fallback, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
#else
    (void)fallback;
#endif
    return handle;
}

static int make_decoder_bundle_path(
    char *path,
    u32 capacity,
    const char *prefix,
    const WhisperModelConfig *model,
    const char *file
) {
    u32 used = 0U;
    const char *parts[4] = {prefix, model->name, "/decoder-fp16/", file};
    u32 part;
    for (part = 0U; part < 4U; ++part) {
        const char *text = parts[part];
        while (*text != '\0') {
            if (used + 1U >= capacity) return 0;
            path[used++] = *text++;
        }
    }
    path[used] = '\0';
    return 1;
}

static u16 *take_weights(u16 **cursor, u64 count) {
    u16 *result = *cursor;
    *cursor += count;
    return result;
}

static int reserve_aligned(u64 *total, u64 count, u64 element_size) {
    u64 aligned;
    u64 bytes;
    if (!whisper_model_size_add(*total, 15U, &aligned)) return 0;
    aligned &= ~15ULL;
    if (!whisper_model_size_multiply(count, element_size, &bytes) ||
        !whisper_model_size_add(aligned, bytes, total)) return 0;
    return 1;
}

static void *take_scratch(u8 *base, u64 *offset, u64 count, u64 element_size) {
    u64 aligned = (*offset + 15U) & ~15ULL;
    void *result = base + aligned;
    *offset = aligned + count * element_size;
    return result;
}

static int allocate_scratch(WhisperDecoder *decoder) {
    const WhisperModelConfig *model = &decoder->model;
    u64 total = 0U;
    u64 offset = 0U;
    u64 self_cache_values;
    u64 cross_cache_values;
    u64 encoder_values;
    u64 score_values;
    u64 prefix_values;
    u8 *base;
    if (!whisper_model_size_multiply(
            model->decoder_layers, model->text_context, &self_cache_values
        ) || !whisper_model_size_multiply(
            self_cache_values, model->width, &self_cache_values
        ) || !whisper_model_size_multiply(
            model->decoder_layers, model->encoder_frames, &cross_cache_values
        ) || !whisper_model_size_multiply(
            cross_cache_values, model->width, &cross_cache_values
        ) || !whisper_model_size_multiply(
            model->encoder_frames, model->width, &encoder_values
        ) || !whisper_model_size_multiply(
            model->attention_heads, model->encoder_frames, &score_values
        ) || !whisper_model_size_multiply(
            model->text_context, model->width, &prefix_values
        ) || !reserve_aligned(
            &total, model->decoder_layers, sizeof(DecoderLayerWeights)
        ) || !reserve_aligned(&total, encoder_values, sizeof(float)) ||
        !reserve_aligned(&total, self_cache_values, sizeof(float)) ||
        !reserve_aligned(&total, self_cache_values, sizeof(float)) ||
        !reserve_aligned(&total, cross_cache_values, sizeof(u16)) ||
        !reserve_aligned(&total, cross_cache_values, sizeof(u16)) ||
        !reserve_aligned(&total, model->width, sizeof(float)) ||
        !reserve_aligned(&total, model->width, sizeof(float)) ||
        !reserve_aligned(&total, model->width, sizeof(float)) ||
        !reserve_aligned(&total, model->width, sizeof(float)) ||
        !reserve_aligned(&total, model->width, sizeof(float)) ||
        !reserve_aligned(&total, model->width, sizeof(float)) ||
        !reserve_aligned(&total, model->width, sizeof(float)) ||
        !reserve_aligned(&total, model->ffn_width, sizeof(float)) ||
        !reserve_aligned(&total, score_values, sizeof(float)) ||
        !reserve_aligned(&total, model->text_context, sizeof(u16)) ||
        !reserve_aligned(&total, model->text_context, sizeof(u16)) ||
        !reserve_aligned(&total, model->text_context, sizeof(u16)) ||
        !reserve_aligned(&total, prefix_values, sizeof(float)) ||
        total > (u64)(usize)-1) {
        return 0;
    }
    decoder->scratch_allocation = DECODER_ALLOCATE(
        0, (usize)total, 0x3000U, 0x04U
    );
    if (decoder->scratch_allocation == 0) return 0;
    base = (u8 *)decoder->scratch_allocation;
    decoder->weights.layers = take_scratch(
        base, &offset, model->decoder_layers, sizeof(DecoderLayerWeights)
    );
    decoder->encoder_hidden = take_scratch(base, &offset, encoder_values, sizeof(float));
    decoder->key_cache = take_scratch(base, &offset, self_cache_values, sizeof(float));
    decoder->value_cache = take_scratch(base, &offset, self_cache_values, sizeof(float));
    decoder->cross_key_cache_storage = take_scratch(
        base, &offset, cross_cache_values, sizeof(u16)
    );
    decoder->cross_value_cache_storage = take_scratch(
        base, &offset, cross_cache_values, sizeof(u16)
    );
    decoder->hidden = take_scratch(base, &offset, model->width, sizeof(float));
    decoder->normalized = take_scratch(base, &offset, model->width, sizeof(float));
    decoder->query = take_scratch(base, &offset, model->width, sizeof(float));
    decoder->key = take_scratch(base, &offset, model->width, sizeof(float));
    decoder->value = take_scratch(base, &offset, model->width, sizeof(float));
    decoder->attended = take_scratch(base, &offset, model->width, sizeof(float));
    decoder->projected = take_scratch(base, &offset, model->width, sizeof(float));
    decoder->mlp_hidden = take_scratch(base, &offset, model->ffn_width, sizeof(float));
    decoder->attention_scores = take_scratch(base, &offset, score_values, sizeof(float));
    decoder->generated_tokens = take_scratch(
        base, &offset, model->text_context, sizeof(u16)
    );
    decoder->selected_tokens = take_scratch(
        base, &offset, model->text_context, sizeof(u16)
    );
    decoder->prefix_tokens = take_scratch(
        base, &offset, model->text_context, sizeof(u16)
    );
    decoder->prefix_hidden = take_scratch(base, &offset, prefix_values, sizeof(float));
    return offset == total;
}

static int bind_weights(WhisperDecoder *decoder) {
    const WhisperModelConfig *model = &decoder->model;
    DecoderWeights *weights = &decoder->weights;
    u16 *cursor = decoder->weight_storage;
    u64 width_squared = (u64)model->width * model->width;
    u32 layer;
    weights->token_embedding = take_weights(
        &cursor, (u64)model->vocabulary_size * model->width
    );
    weights->position_embedding = take_weights(
        &cursor, (u64)model->text_context * model->width
    );
    weights->encoder_norm_weight = take_weights(&cursor, model->width);
    weights->encoder_norm_bias = take_weights(&cursor, model->width);
    weights->decoder_norm_weight = take_weights(&cursor, model->width);
    weights->decoder_norm_bias = take_weights(&cursor, model->width);
    for (layer = 0U; layer < model->decoder_layers; ++layer) {
        DecoderLayerWeights *item = &weights->layers[layer];
        item->self_norm_weight = take_weights(&cursor, model->width);
        item->self_norm_bias = take_weights(&cursor, model->width);
        item->self_k_weight = take_weights(&cursor, width_squared);
        item->self_q_weight = take_weights(&cursor, width_squared);
        item->self_q_bias = take_weights(&cursor, model->width);
        item->self_v_weight = take_weights(&cursor, width_squared);
        item->self_v_bias = take_weights(&cursor, model->width);
        item->self_out_weight = take_weights(&cursor, width_squared);
        item->self_out_bias = take_weights(&cursor, model->width);
        item->cross_norm_weight = take_weights(&cursor, model->width);
        item->cross_norm_bias = take_weights(&cursor, model->width);
        item->cross_k_weight = take_weights(&cursor, width_squared);
        item->cross_q_weight = take_weights(&cursor, width_squared);
        item->cross_q_bias = take_weights(&cursor, model->width);
        item->cross_v_weight = take_weights(&cursor, width_squared);
        item->cross_v_bias = take_weights(&cursor, model->width);
        item->cross_out_weight = take_weights(&cursor, width_squared);
        item->cross_out_bias = take_weights(&cursor, model->width);
        item->final_norm_weight = take_weights(&cursor, model->width);
        item->final_norm_bias = take_weights(&cursor, model->width);
        item->fc1_weight = take_weights(
            &cursor, (u64)model->ffn_width * model->width
        );
        item->fc1_bias = take_weights(&cursor, model->ffn_width);
        item->fc2_weight = take_weights(
            &cursor, (u64)model->width * model->ffn_width
        );
        item->fc2_bias = take_weights(&cursor, model->width);
    }
    return cursor == decoder->weight_storage +
        whisper_model_decoder_float_count(model);
}

WhisperDecoder *whisper_decoder_load_with_workers(
    const WhisperModelConfig *config,
    u32 worker_count
) {
    WhisperDecoder *decoder;
    void *invalid = (void *)(usize)-1;
    void *handle;
    u8 header_bytes[WHISPER_ARTIFACT_HEADER_SIZE];
    WhisperArtifactHeader header;
    u64 expected_weight_bytes;
    u64 token_payload_bytes;
    u32 offset_bytes;
    u32 index;
    char weight_path[192];
    char weight_fallback[160];
    char token_path[192];
    char token_fallback[160];
    long long counter_frequency;
    if (!whisper_model_config_valid(config) ||
        whisper_model_decoder_float_count(config) == 0U ||
        !whisper_model_size_multiply(
            whisper_model_decoder_float_count(config), sizeof(u16),
            &expected_weight_bytes
        ) || expected_weight_bytes > 0xffffffffULL ||
        !make_decoder_bundle_path(
            weight_path, sizeof(weight_path),
            "experimental/snapdragon/models/whisper-", config, "weights-fp16.bin"
        ) || !make_decoder_bundle_path(
            weight_fallback, sizeof(weight_fallback),
            "../models/whisper-", config, "weights-fp16.bin"
        ) || !make_decoder_bundle_path(
            token_path, sizeof(token_path),
            "experimental/snapdragon/models/whisper-", config, "token-bytes.bin"
        ) || !make_decoder_bundle_path(
            token_fallback, sizeof(token_fallback),
            "../models/whisper-", config, "token-bytes.bin"
        )) {
        return 0;
    }
    decoder = DECODER_ALLOCATE(0, sizeof(*decoder), 0x3000U, 0x04U);
    if (decoder == 0) return 0;
    decoder->model = *config;
    if (!QueryPerformanceFrequency(&counter_frequency) || counter_frequency <= 0) {
        goto failure;
    }
    decoder->counter_frequency = (u64)counter_frequency;
    if (!allocate_scratch(decoder)) goto failure;
    handle = open_bundle(weight_path, weight_fallback);
    if (handle == invalid) goto failure;
    if (!read_exact(handle, header_bytes, sizeof(header_bytes)) ||
        !whisper_artifact_decode_header(header_bytes, &header) ||
        !whisper_artifact_header_valid(
            &header, config, WHISPER_ARTIFACT_PAYLOAD_DECODER_WEIGHTS,
            WHISPER_ARTIFACT_ELEMENT_F16,
            whisper_model_decoder_float_count(config),
            expected_weight_bytes
        )) {
        CloseHandle(handle);
        goto failure;
    }
    decoder->weight_allocation = DECODER_ALLOCATE(
        0, (usize)expected_weight_bytes, 0x3000U, 0x04U
    );
    decoder->weight_storage = (u16 *)decoder->weight_allocation;
    if (decoder->weight_storage == 0 || !read_exact(
            handle, decoder->weight_storage, (u32)expected_weight_bytes)) {
        CloseHandle(handle);
        goto failure;
    }
    CloseHandle(handle);
    if (!whisper_artifact_payload_valid(
            &header, decoder->weight_storage, expected_weight_bytes
        ) || !bind_weights(decoder)) goto failure;

    handle = open_bundle(token_path, token_fallback);
    if (handle == invalid) goto failure;
    offset_bytes = (config->vocabulary_size + 1U) * sizeof(u32);
    if (!read_exact(handle, header_bytes, sizeof(header_bytes)) ||
        !whisper_artifact_decode_header(header_bytes, &header) ||
        header.payload_size < offset_bytes || header.payload_size > 0xffffffffULL ||
        !whisper_artifact_header_valid(
            &header, config, WHISPER_ARTIFACT_PAYLOAD_TOKEN_BYTES,
            WHISPER_ARTIFACT_ELEMENT_U8, header.payload_size, header.payload_size
        )) {
        CloseHandle(handle);
        goto failure;
    }
    token_payload_bytes = header.payload_size;
    decoder->token_byte_count = (u32)(token_payload_bytes - offset_bytes);
    decoder->token_allocation = DECODER_ALLOCATE(
        0, (usize)token_payload_bytes, 0x3000U, 0x04U
    );
    decoder->token_offsets = (u32 *)decoder->token_allocation;
    if (decoder->token_offsets == 0 ||
        !read_exact(handle, decoder->token_offsets, (u32)token_payload_bytes)) {
        CloseHandle(handle);
        goto failure;
    }
    CloseHandle(handle);
    if (!whisper_artifact_payload_valid(
            &header, decoder->token_offsets, token_payload_bytes
        )) goto failure;
    decoder->token_bytes = (u8 *)decoder->token_offsets + offset_bytes;
    if (decoder->token_offsets[0] != 0U ||
        decoder->token_offsets[config->vocabulary_size] !=
            decoder->token_byte_count) goto failure;
    for (index = 0U; index < config->vocabulary_size; ++index) {
        if (decoder->token_offsets[index] > decoder->token_offsets[index + 1U] ||
            decoder->token_offsets[index + 1U] > decoder->token_byte_count) {
            goto failure;
        }
    }
    (void)rt_task_pool_init(&decoder->pool, worker_count);
    decoder->pool_ready = 1;
    return decoder;

failure:
    whisper_decoder_shutdown(decoder);
    return 0;
}

WhisperDecoder *whisper_decoder_load(const WhisperModelConfig *config) {
    return whisper_decoder_load_with_workers(config, 0U);
}

static float dot_product(const float *left, const float *right, u32 count) {
    f32x4 sum = {0.0f, 0.0f, 0.0f, 0.0f};
    u32 index;
    for (index = 0U; index + 4U <= count; index += 4U) {
        sum += *(const f32x4 *)(left + index) * *(const f32x4 *)(right + index);
    }
    return sum[0] + sum[1] + sum[2] + sum[3];
}

static float dot_product_fp16(const u16 *left, const float *right, u32 count) {
    f32x4 sum0 = {0.0f, 0.0f, 0.0f, 0.0f};
    f32x4 sum1 = {0.0f, 0.0f, 0.0f, 0.0f};
    f32x4 sum2 = {0.0f, 0.0f, 0.0f, 0.0f};
    f32x4 sum3 = {0.0f, 0.0f, 0.0f, 0.0f};
    u32 index;
    for (index = 0U; index + 16U <= count; index += 16U) {
        sum0 += __builtin_convertvector(*(const f16x4 *)(left + index), f32x4) *
            *(const f32x4 *)(right + index);
        sum1 += __builtin_convertvector(*(const f16x4 *)(left + index + 4U), f32x4) *
            *(const f32x4 *)(right + index + 4U);
        sum2 += __builtin_convertvector(*(const f16x4 *)(left + index + 8U), f32x4) *
            *(const f32x4 *)(right + index + 8U);
        sum3 += __builtin_convertvector(*(const f16x4 *)(left + index + 12U), f32x4) *
            *(const f32x4 *)(right + index + 12U);
    }
    for (; index + 4U <= count; index += 4U) {
        sum0 += __builtin_convertvector(*(const f16x4 *)(left + index), f32x4) *
            *(const f32x4 *)(right + index);
    }
    sum0 += sum1;
    sum2 += sum3;
    sum0 += sum2;
    return sum0[0] + sum0[1] + sum0[2] + sum0[3];
}

typedef struct MatrixVectorContext {
    const u16 *matrix;
    const float *input;
    const u16 *bias;
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
        context->output[row] = dot_product_fp16(
            context->matrix + row * context->columns,
            context->input,
            context->columns
        ) + (context->bias == 0 ? 0.0f :
            whisper_frontend_half_to_float(context->bias[row]));
    }
    return 0;
}

static void matrix_vector_serial(
    const u16 *matrix,
    const float *input,
    const u16 *bias,
    float *output,
    u32 rows,
    u32 columns
) {
    u32 row;
    for (row = 0U; row < rows; ++row) {
        output[row] = dot_product_fp16(matrix + row * columns, input, columns) +
            (bias == 0 ? 0.0f : whisper_frontend_half_to_float(bias[row]));
    }
}

static void matrix_vector_parallel(
    WhisperDecoder *decoder,
    const u16 *matrix,
    const float *input,
    const u16 *bias,
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
        &decoder->pool, rows, 1U, matrix_vector_range, &context
    );
}

static void matrix_vector_fp16(
    const u16 *matrix,
    const float *input,
    const u16 *bias,
    u16 *output,
    u32 rows,
    u32 columns
) {
    u32 row;
    for (row = 0U; row < rows; ++row) {
        float value = dot_product_fp16(matrix + row * columns, input, columns) +
            (bias == 0 ? 0.0f : whisper_frontend_half_to_float(bias[row]));
        output[row] = whisper_frontend_float_to_half(value);
    }
}

static void layer_norm(
    const float *input,
    const u16 *scale,
    const u16 *bias,
    float *output,
    u32 width
) {
    double sum = 0.0;
    double squared_sum = 0.0;
    double inverse;
    u32 index;
    for (index = 0U; index < width; ++index) sum += input[index];
    sum /= width;
    for (index = 0U; index < width; ++index) {
        double centered = input[index] - sum;
        squared_sum += centered * centered;
    }
    inverse = 1.0 / math_sqrt(squared_sum / width + 1.0e-5);
    for (index = 0U; index < width; ++index) {
        output[index] = (float)(((input[index] - sum) * inverse) *
            whisper_frontend_half_to_float(scale[index]) +
            whisper_frontend_half_to_float(bias[index]));
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

static void normalize_encoder(WhisperDecoder *decoder, const u16 *encoder_output) {
    const WhisperModelConfig *model = &decoder->model;
    u32 frame;
    for (frame = 0U; frame < model->encoder_frames; ++frame) {
        u32 channel;
        for (channel = 0U; channel < model->width; ++channel) {
            decoder->hidden[channel] = whisper_frontend_half_to_float(
                encoder_output[frame * model->width + channel]
            );
        }
        layer_norm(
            decoder->hidden, decoder->weights.encoder_norm_weight,
            decoder->weights.encoder_norm_bias,
            decoder->encoder_hidden + frame * model->width, model->width
        );
    }
}

static int prepare_cross_attention_cache_range(
    size_t begin,
    size_t end,
    unsigned int worker_index,
    void *arg
) {
    WhisperDecoder *decoder = (WhisperDecoder *)arg;
    const WhisperModelConfig *model = &decoder->model;
    size_t item_index;
    (void)worker_index;
    for (item_index = begin; item_index < end; ++item_index) {
        u32 layer = (u32)(item_index / model->encoder_frames);
        u32 frame = (u32)(item_index % model->encoder_frames);
        const DecoderLayerWeights *item = &decoder->weights.layers[layer];
        const float *encoder = decoder->encoder_hidden + frame * model->width;
        matrix_vector_fp16(
            item->cross_k_weight, encoder, 0,
            decoder->cross_key_cache_storage +
                ((u64)layer * model->encoder_frames + frame) * model->width,
            model->width, model->width
        );
        matrix_vector_fp16(
            item->cross_v_weight, encoder, item->cross_v_bias,
            decoder->cross_value_cache_storage +
                ((u64)layer * model->encoder_frames + frame) * model->width,
            model->width, model->width
        );
    }
    return 0;
}

static void prepare_cross_attention_cache(WhisperDecoder *decoder) {
    (void)rt_parallel_for(
        &decoder->pool,
        (size_t)decoder->model.decoder_layers * decoder->model.encoder_frames,
        16U, prepare_cross_attention_cache_range, decoder
    );
    decoder->cross_key_cache = decoder->cross_key_cache_storage;
    decoder->cross_value_cache = decoder->cross_value_cache_storage;
    decoder->cross_cache_head_major = 0;
}

static void import_cross_attention_cache(
    WhisperDecoder *decoder,
    const u16 *keys,
    const u16 *values
) {
    decoder->cross_key_cache = keys;
    decoder->cross_value_cache = values;
    decoder->cross_cache_head_major = 1;
}

static int self_attention(
    WhisperDecoder *decoder,
    u32 layer,
    u32 position,
    const DecoderLayerWeights *item,
    u64 *npu_execute_ticks
) {
    const WhisperModelConfig *model = &decoder->model;
    u32 head_width = model->width / model->attention_heads;
    u32 head;
    u32 index;
    u64 cache_offset = ((u64)layer * model->text_context + position) * model->width;
    *npu_execute_ticks = 0U;
    int offload_calls = decoder->self_attention_offload != 0 ?
        decoder->self_attention_offload(
            decoder->self_attention_offload_context, layer, position,
            decoder->hidden, decoder->projected, npu_execute_ticks
        ) : 0;
    if (offload_calls > 0) {
        for (index = 0U; index < model->width; ++index) {
            decoder->hidden[index] += decoder->projected[index];
        }
        return offload_calls;
    }
    layer_norm(
        decoder->hidden, item->self_norm_weight, item->self_norm_bias,
        decoder->normalized, model->width
    );
    matrix_vector_serial(
        item->self_q_weight, decoder->normalized, item->self_q_bias,
        decoder->query, model->width, model->width
    );
    matrix_vector_serial(
        item->self_k_weight, decoder->normalized, 0,
        decoder->key, model->width, model->width
    );
    matrix_vector_serial(
        item->self_v_weight, decoder->normalized, item->self_v_bias,
        decoder->value, model->width, model->width
    );
    for (index = 0U; index < model->width; ++index) {
        decoder->key_cache[cache_offset + index] = decoder->key[index];
        decoder->value_cache[cache_offset + index] = decoder->value[index];
    }
    for (head = 0U; head < model->attention_heads; ++head) {
        u32 token;
        u32 lane;
        const float *head_query = decoder->query + head * head_width;
        float *head_scores = decoder->attention_scores +
            (u64)head * model->encoder_frames;
        for (token = 0U; token <= position; ++token) {
            u64 token_offset =
                ((u64)layer * model->text_context + token) * model->width;
            head_scores[token] = dot_product(
                head_query,
                decoder->key_cache + token_offset + head * head_width,
                head_width
            ) * 0.125f;
        }
        softmax(head_scores, position + 1U);
        for (lane = 0U; lane < head_width; ++lane) {
            double sum = 0.0;
            for (token = 0U; token <= position; ++token) {
                u64 token_offset =
                    ((u64)layer * model->text_context + token) * model->width;
                sum += (double)head_scores[token] *
                    decoder->value_cache[token_offset + head * head_width + lane];
            }
            decoder->attended[head * head_width + lane] = (float)sum;
        }
    }
    matrix_vector_serial(
        item->self_out_weight, decoder->attended, item->self_out_bias,
        decoder->projected, model->width, model->width
    );
    for (index = 0U; index < model->width; ++index) {
        decoder->hidden[index] += decoder->projected[index];
    }
    return 0;
}

typedef struct CrossAttentionContext {
    WhisperDecoder *decoder;
    u32 layer;
} CrossAttentionContext;

static int cross_attention_head_range(
    size_t begin,
    size_t end,
    unsigned int worker_index,
    void *arg
) {
    const CrossAttentionContext *context = (const CrossAttentionContext *)arg;
    WhisperDecoder *decoder = context->decoder;
    const WhisperModelConfig *model = &decoder->model;
    u32 head_width = model->width / model->attention_heads;
    size_t head_index;
    (void)worker_index;
    for (head_index = begin; head_index < end; ++head_index) {
        u32 head = (u32)head_index;
        u32 layer = context->layer;
        u32 frame;
        u32 lane;
        const u16 *layer_keys = decoder->cross_key_cache +
            (u64)layer * model->encoder_frames * model->width;
        const u16 *layer_values = decoder->cross_value_cache +
            (u64)layer * model->encoder_frames * model->width;
        float *head_scores = decoder->attention_scores +
            (u64)head * model->encoder_frames;
        for (frame = 0U; frame < model->encoder_frames; ++frame) {
            if (decoder->cross_cache_head_major) {
                double sum = 0.0;
                for (lane = 0U; lane < head_width; ++lane) {
                    sum += (double)whisper_frontend_half_to_float(
                        layer_keys[
                            ((u64)head * head_width + lane) *
                            model->encoder_frames + frame
                        ]
                    ) * decoder->query[head * head_width + lane];
                }
                head_scores[frame] = (float)sum * 0.125f;
            } else {
                head_scores[frame] = dot_product_fp16(
                    layer_keys + (u64)frame * model->width + head * head_width,
                    decoder->query + head * head_width,
                    head_width
                ) * 0.125f;
            }
        }
        softmax(head_scores, model->encoder_frames);
        for (lane = 0U; lane < head_width; ++lane) {
            decoder->attended[head * head_width + lane] = 0.0f;
        }
        for (frame = 0U; frame < model->encoder_frames; ++frame) {
            const u16 *frame_value = decoder->cross_cache_head_major
                ? layer_values + ((u64)head * model->encoder_frames + frame) * head_width
                : layer_values + (u64)frame * model->width + head * head_width;
            f32x4 factor = {
                head_scores[frame], head_scores[frame],
                head_scores[frame], head_scores[frame]
            };
            for (lane = 0U; lane < head_width; lane += 4U) {
                f32x4 converted = __builtin_convertvector(
                    *(const f16x4 *)(frame_value + lane), f32x4
                );
                *(f32x4 *)(decoder->attended + head * head_width + lane) +=
                    factor * converted;
            }
        }
    }
    return 0;
}

static int cross_attention(
    WhisperDecoder *decoder,
    u32 layer,
    const DecoderLayerWeights *item,
    u64 *npu_execute_ticks
) {
    const WhisperModelConfig *model = &decoder->model;
    CrossAttentionContext context;
    u32 index;
    *npu_execute_ticks = 0U;
    if (decoder->cross_attention_offload != 0 &&
        decoder->cross_attention_offload(
            decoder->cross_attention_offload_context, layer,
            decoder->hidden, decoder->projected, npu_execute_ticks
        )) {
        for (index = 0U; index < model->width; ++index) {
            decoder->hidden[index] += decoder->projected[index];
        }
        return 1;
    }
    layer_norm(
        decoder->hidden, item->cross_norm_weight, item->cross_norm_bias,
        decoder->normalized, model->width
    );
    matrix_vector_parallel(
        decoder, item->cross_q_weight, decoder->normalized,
        item->cross_q_bias, decoder->query, model->width, model->width
    );
    context.decoder = decoder;
    context.layer = layer;
    (void)rt_parallel_for(
        &decoder->pool, model->attention_heads, 1U,
        cross_attention_head_range, &context
    );
    matrix_vector_parallel(
        decoder, item->cross_out_weight, decoder->attended,
        item->cross_out_bias, decoder->projected, model->width, model->width
    );
    for (index = 0U; index < model->width; ++index) {
        decoder->hidden[index] += decoder->projected[index];
    }
    return 0;
}

static int feed_forward(
    WhisperDecoder *decoder,
    u32 layer,
    const DecoderLayerWeights *item,
    u64 *npu_execute_ticks
) {
    const WhisperModelConfig *model = &decoder->model;
    u32 index;
    *npu_execute_ticks = 0U;
    layer_norm(
        decoder->hidden, item->final_norm_weight, item->final_norm_bias,
        decoder->normalized, model->width
    );
    if (decoder->mlp_offload != 0 && decoder->mlp_offload(
            decoder->mlp_offload_context, layer,
            decoder->normalized, decoder->projected, npu_execute_ticks
        )) {
        for (index = 0U; index < model->width; ++index) {
            decoder->hidden[index] += decoder->projected[index];
        }
        return 1;
    }
    matrix_vector_parallel(
        decoder, item->fc1_weight, decoder->normalized, item->fc1_bias,
        decoder->mlp_hidden, model->ffn_width, model->width
    );
    for (index = 0U; index < model->ffn_width; ++index) {
        decoder->mlp_hidden[index] = gelu(decoder->mlp_hidden[index]);
    }
    matrix_vector_parallel(
        decoder, item->fc2_weight, decoder->mlp_hidden, item->fc2_bias,
        decoder->projected, model->width, model->ffn_width
    );
    for (index = 0U; index < model->width; ++index) {
        decoder->hidden[index] += decoder->projected[index];
    }
    return 0;
}

static void record_npu_calls(
    WhisperDecoder *decoder,
    WhisperDecoderNpuCalls *calls,
    u64 ticks,
    u32 graph_submissions
) {
    ++calls->offload_calls;
    calls->graph_submissions += graph_submissions;
    if (ticks > calls->maximum_ticks) calls->maximum_ticks = ticks;
    if (ticks >= decoder->counter_frequency / 100U) ++calls->over_10ms;
    if (ticks >= decoder->counter_frequency / 10U) ++calls->over_100ms;
    if (ticks >= decoder->counter_frequency) ++calls->over_1000ms;
}

static const u16 suppressed_tokens[] = {
        1, 2, 7, 8, 9, 10, 14, 25, 26, 27, 28, 29, 31, 58, 59, 60, 61, 62, 63,
        90, 91, 92, 93, 359, 503, 522, 542, 873, 893, 902, 918, 922, 931, 1350,
        1853, 1982, 2460, 2627, 3246, 3253, 3268, 3536, 3846, 3961, 4183, 4667,
        6585, 6647, 7273, 9061, 9383, 10428, 10929, 11938, 12033, 12331, 12562,
        13793, 14157, 14635, 15265, 15618, 16553, 16604, 18362, 18956, 20075,
        21675, 22520, 26130, 26161, 26435, 28279, 29464, 31650, 32302, 32470,
        36865, 42863, 47425, 49870, 50254, 50258, 50358, 50359, 50360, 50361,
        50363
};

static void exclude_token(WhisperDecoder *decoder, u32 token) {
    if (token < DECODER_TEXT_TOKEN_LIMIT) {
        decoder->excluded_tokens[token >> 3U] |= (u8)(1U << (token & 7U));
    }
}

static int token_excluded(const WhisperDecoder *decoder, u32 token) {
    return token >= DECODER_TEXT_TOKEN_LIMIT ||
        (decoder->excluded_tokens[token >> 3U] & (1U << (token & 7U))) != 0U;
}

static void prepare_token_exclusions(WhisperDecoder *decoder, int first_generated) {
    u32 index;
    for (index = 0U; index < sizeof(decoder->excluded_tokens); ++index) {
        decoder->excluded_tokens[index] = 0U;
    }
    for (index = 0U; index < sizeof(suppressed_tokens) / sizeof(suppressed_tokens[0]); ++index) {
        exclude_token(decoder, suppressed_tokens[index]);
    }
    if (!first_generated) exclude_token(decoder, DECODER_NO_SPEECH);
    if (first_generated) exclude_token(decoder, 220U);
}

typedef struct LogitContext {
    WhisperDecoder *decoder;
    const u16 *offloaded_logits;
    float *gumbel_values;
    int fill_gumbel_values;
    u32 partition_count;
    u32 forbidden_count;
    u32 position;
    u32 sample_seed;
    float temperature;
    u16 forbidden_tokens[WHISPER_DECODER_MAX_TOKENS];
} LogitContext;

static u64 sample_hash(u64 value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

static float sample_gumbel(const LogitContext *context, u32 token) {
    u64 mixed = sample_hash(
        ((u64)context->sample_seed << 32U) ^
        ((u64)context->position << 16U) ^ token
    );
    double uniform = (double)((mixed >> 11U) + 1U) /
        (double)(0x20000000000000ULL + 1ULL);
    return (float)-math_log(-math_log(uniform));
}

static float *prepare_gumbel_cache(WhisperDecoder *decoder, u32 position) {
    u64 count;
    u64 bytes;
    if (position >= decoder->model.text_context || position >= WHISPER_DECODER_MAX_TOKENS) return 0;
    if (decoder->gumbel_cache == 0) {
        if (!whisper_model_size_multiply(decoder->model.text_context, decoder->model.vocabulary_size, &count) ||
            !whisper_model_size_multiply(count, sizeof(float), &bytes) || bytes > (u64)(usize)-1) return 0;
        decoder->gumbel_cache = DECODER_ALLOCATE(0, (usize)bytes, 0x3000U, 0x04U);
        if (decoder->gumbel_cache == 0) return 0;
    }
    return decoder->gumbel_cache + (u64)position * decoder->model.vocabulary_size;
}

static void collect_no_repeat_tokens(u32 position, LogitContext *context) {
    const u16 *generated_tokens = context->decoder->generated_tokens;
    u32 prefix_size = context->temperature > 0.0f ? 2U : 3U;
    u32 start;
    context->forbidden_count = 0U;
    if (position < 4U + prefix_size - 1U) return;
    for (start = 4U; start + prefix_size <= position; ++start) {
        u32 index;
        u16 candidate;
        u32 prefix_index;
        for (prefix_index = 0U; prefix_index < prefix_size; ++prefix_index) {
            if (generated_tokens[start + prefix_index] !=
                generated_tokens[position + 1U - prefix_size + prefix_index]) {
                break;
            }
        }
        if (prefix_index != prefix_size) continue;
        candidate = generated_tokens[start + prefix_size];
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
    WhisperDecoder *decoder = context->decoder;
    const WhisperModelConfig *model = &decoder->model;
    size_t partition;
    (void)worker_index;
    for (partition = begin; partition < end; ++partition) {
        u32 token = (u32)(((u64)model->vocabulary_size * partition) /
            context->partition_count);
        u32 token_end = (u32)(((u64)model->vocabulary_size * (partition + 1U)) /
            context->partition_count);
        float maximum = -3.402823466e+38f;
        u32 best = DECODER_EOT;
        if (context->fill_gumbel_values) {
            u32 sample_token;
            for (sample_token = token; sample_token < token_end; ++sample_token) {
                context->gumbel_values[sample_token] = sample_gumbel(context, sample_token);
            }
        }
        for (; token < token_end; ++token) {
            float logit;
            if (token_excluded(decoder, token)) continue;
            logit = context->offloaded_logits != 0
                ? whisper_frontend_half_to_float(context->offloaded_logits[token])
                : dot_product_fp16(
                    decoder->weights.token_embedding + (u64)token * model->width,
                    decoder->normalized,
                    model->width
                );
            if (context->temperature > 0.0f) {
                float noise = context->gumbel_values != 0
                    ? context->gumbel_values[token] : sample_gumbel(context, token);
                logit += context->temperature * noise;
            }
            if (logit > maximum || (logit == maximum && token < best)) {
                maximum = logit;
                best = token;
            }
        }
        decoder->logit_maxima[partition] = maximum;
        decoder->logit_tokens[partition] = best;
    }
    return 0;
}

static int restore_prefix_hidden(WhisperDecoder *decoder, u32 token, u32 position) {
    u32 index;
#if defined(WHISPER_DECODER_DISABLE_PREFIX_REUSE)
    decoder->prefix_count = 0U;
#endif
    if (position >= decoder->prefix_count || decoder->prefix_tokens[position] != token) {
        decoder->prefix_count = position;
        return 0;
    }
    for (index = 0U; index < decoder->model.width; ++index) {
        decoder->hidden[index] = decoder->prefix_hidden[(u64)position * decoder->model.width + index];
    }
    ++decoder->profile.prefix_reused_steps;
    return 1;
}

static void cache_prefix_hidden(WhisperDecoder *decoder, u32 token, u32 position) {
    u32 index;
    for (index = 0U; index < decoder->model.width; ++index) {
        decoder->prefix_hidden[(u64)position * decoder->model.width + index] = decoder->hidden[index];
    }
    decoder->prefix_tokens[position] = (u16)token;
    decoder->prefix_count = position + 1U;
}

static u32 decoder_step(
    WhisperDecoder *decoder,
    u32 token,
    u32 position,
    int first_generated,
    float temperature,
    u32 sample_seed,
    int select_output
) {
    const WhisperModelConfig *model = &decoder->model;
    const u16 *embedding = decoder->weights.token_embedding +
        (u64)token * model->width;
    const u16 *position_values = decoder->weights.position_embedding +
        (u64)position * model->width;
    float maximum = -3.402823466e+38f;
    u32 best = DECODER_EOT;
    u32 layer;
    u32 index;
    int fused_cross_mlp;
    u64 npu_execute_ticks;
    const u16 *offloaded_logits = 0;
    LogitContext logit_context;
    long long start;
    long long end;
    if (decoder->trace) decoder->trace(decoder->trace_context, 0U, 1U, position, ~0U);
    if (restore_prefix_hidden(decoder, token, position)) goto select_next_token;
    for (index = 0U; index < model->width; ++index) {
        decoder->hidden[index] = whisper_frontend_half_to_float(embedding[index]) +
            whisper_frontend_half_to_float(position_values[index]);
    }
    for (layer = 0U; layer < model->decoder_layers; ++layer) {
        if (decoder->trace) decoder->trace(decoder->trace_context, 1U, 1U, position, layer);
        QueryPerformanceCounter(&start);
        index = (u32)self_attention(
            decoder, layer, position, &decoder->weights.layers[layer],
            &npu_execute_ticks
        );
        QueryPerformanceCounter(&end);
        decoder->profile.npu_self_attention_execute_ticks += npu_execute_ticks;
        if (index != 0U) {
            record_npu_calls(
                decoder, &decoder->profile.npu_self_attention_calls,
                npu_execute_ticks, index
            );
            decoder->profile.npu_self_attention_ticks += (u64)(end - start);
        } else {
            decoder->profile.self_attention_ticks += (u64)(end - start);
        }
        fused_cross_mlp = 0;
        if (decoder->trace) decoder->trace(decoder->trace_context, 1U, 0U, position, layer);
        if (decoder->trace) decoder->trace(decoder->trace_context, 2U, 1U, position, layer);
        if (decoder->fused_cross_mlp_offload != 0) {
            QueryPerformanceCounter(&start);
            fused_cross_mlp = decoder->fused_cross_mlp_offload(
                decoder->fused_cross_mlp_offload_context, layer,
                decoder->hidden, decoder->projected, &npu_execute_ticks
            );
            QueryPerformanceCounter(&end);
            if (fused_cross_mlp) {
                decoder->profile.npu_fused_cross_mlp_execute_ticks +=
                    npu_execute_ticks;
                decoder->profile.npu_fused_cross_mlp_ticks += (u64)(end - start);
                record_npu_calls(
                    decoder, &decoder->profile.npu_fused_cross_mlp_calls,
                    npu_execute_ticks, 1U
                );
                for (index = 0U; index < model->width; ++index) {
                    decoder->hidden[index] = decoder->projected[index];
                }
            }
        }
        if (!fused_cross_mlp) {
            QueryPerformanceCounter(&start);
            index = (u32)cross_attention(
                decoder, layer, &decoder->weights.layers[layer], &npu_execute_ticks
            );
            QueryPerformanceCounter(&end);
            decoder->profile.npu_cross_attention_execute_ticks += npu_execute_ticks;
            if (index != 0U) {
                record_npu_calls(
                    decoder, &decoder->profile.npu_cross_attention_calls,
                    npu_execute_ticks, 1U
                );
                decoder->profile.npu_cross_attention_ticks += (u64)(end - start);
            } else {
                decoder->profile.cross_attention_ticks += (u64)(end - start);
            }
            QueryPerformanceCounter(&start);
            index = (u32)feed_forward(
                decoder, layer, &decoder->weights.layers[layer], &npu_execute_ticks
            );
            QueryPerformanceCounter(&end);
            decoder->profile.npu_mlp_execute_ticks += npu_execute_ticks;
            if (index != 0U) {
                record_npu_calls(
                    decoder, &decoder->profile.npu_mlp_calls,
                    npu_execute_ticks, 1U
                );
                decoder->profile.npu_feed_forward_ticks += (u64)(end - start);
            } else {
                decoder->profile.feed_forward_ticks += (u64)(end - start);
            }
        }
        if (decoder->trace) decoder->trace(decoder->trace_context, 2U, 0U, position, layer);
    }
    cache_prefix_hidden(decoder, token, position);
select_next_token:
    if (!select_output) {
        ++decoder->profile.decoder_steps;
        if (decoder->trace) decoder->trace(decoder->trace_context, 0U, 0U, position, ~0U);
        return DECODER_EOT;
    }
    npu_execute_ticks = 0U;
    if (decoder->trace) decoder->trace(decoder->trace_context, 3U, 1U, position, ~0U);
    QueryPerformanceCounter(&start);
    if (decoder->logits_offload != 0 && decoder->logits_offload(
            decoder->logits_offload_context, decoder->hidden,
            &offloaded_logits, &npu_execute_ticks
        )) {
        QueryPerformanceCounter(&end);
        decoder->profile.npu_logits_ticks += (u64)(end - start);
        decoder->profile.npu_logits_execute_ticks += npu_execute_ticks;
        record_npu_calls(
            decoder, &decoder->profile.npu_logits_calls,
            npu_execute_ticks, 1U
        );
        QueryPerformanceCounter(&start);
    } else {
        layer_norm(
            decoder->hidden, decoder->weights.decoder_norm_weight,
            decoder->weights.decoder_norm_bias, decoder->normalized, model->width
        );
    }
    if (decoder->trace) decoder->trace(decoder->trace_context, 3U, 0U, position, ~0U);
    if (decoder->trace) decoder->trace(decoder->trace_context, 4U, 1U, position, ~0U);
    logit_context.decoder = decoder;
    logit_context.offloaded_logits = offloaded_logits;
    logit_context.gumbel_values = temperature > 0.0f
        ? prepare_gumbel_cache(decoder, position) : 0;
    logit_context.fill_gumbel_values = logit_context.gumbel_values != 0 &&
        (!decoder->gumbel_valid[position] || decoder->gumbel_seeds[position] != sample_seed);
    logit_context.partition_count = rt_task_pool_width(&decoder->pool);
    logit_context.position = position;
    logit_context.sample_seed = sample_seed;
    logit_context.temperature = temperature;
    prepare_token_exclusions(decoder, first_generated);
    collect_no_repeat_tokens(position, &logit_context);
    for (index = 0U; index < logit_context.forbidden_count; ++index) {
        exclude_token(decoder, logit_context.forbidden_tokens[index]);
    }
    if (logit_context.partition_count > RT_TASK_POOL_MAX_WORKERS) {
        logit_context.partition_count = RT_TASK_POOL_MAX_WORKERS;
    }
    (void)rt_parallel_for(
        &decoder->pool, logit_context.partition_count, 1U,
        logit_partition_range, &logit_context
    );
    if (logit_context.gumbel_values != 0) {
        decoder->gumbel_valid[position] = 1U;
        decoder->gumbel_seeds[position] = sample_seed;
    }
    for (index = 0U; index < logit_context.partition_count; ++index) {
        if (decoder->logit_maxima[index] > maximum ||
            (decoder->logit_maxima[index] == maximum &&
             decoder->logit_tokens[index] < best)) {
            maximum = decoder->logit_maxima[index];
            best = decoder->logit_tokens[index];
        }
    }
    QueryPerformanceCounter(&end);
    decoder->profile.logits_ticks += (u64)(end - start);
    decoder->profile.decoder_steps += 1U;
    if (decoder->trace) decoder->trace(decoder->trace_context, 4U, 0U, position, ~0U);
    if (decoder->trace) decoder->trace(decoder->trace_context, 0U, 0U, position, ~0U);
    return best;
}

#if defined(WHISPER_DECODER_TEST_ALLOCATOR)
static u32 test_logits_calls;

static int test_prompt_logits_offload(
    void *context, const float *hidden, const u16 **logits, u64 *ticks
) {
    static u16 values[DECODER_TEXT_TOKEN_LIMIT + 1501U];
    (void)context;
    (void)hidden;
    ++test_logits_calls;
    *logits = values;
    *ticks = 0U;
    return 1;
}

int whisper_decoder_test_prompt_logits(WhisperDecoder *decoder) {
    u32 index;
    u32 steps = decoder->profile.decoder_steps;
    for (index = 0U; index < decoder->model.width; ++index) decoder->hidden[index] = 0.0f;
    cache_prefix_hidden(decoder, DECODER_SOT, 0U);
    decoder->logits_offload = test_prompt_logits_offload;
    test_logits_calls = 0U;
    if (decoder_step(decoder, DECODER_SOT, 0U, 0, 0.0f, 0U, 0) != DECODER_EOT ||
        test_logits_calls != 0U || decoder->profile.decoder_steps != steps + 1U) return 1;
    if (decoder_step(decoder, DECODER_SOT, 0U, 0, 0.0f, 0U, 1) != 0U ||
        test_logits_calls != 1U || decoder->profile.decoder_steps != steps + 2U) return 2;
    decoder->logits_offload = 0;
    decoder->prefix_count = 0U;
    return 0;
}

int whisper_decoder_test_logit_partitions(WhisperDecoder *decoder) {
    static u16 logits[DECODER_TEXT_TOKEN_LIMIT + 1501U];
    LogitContext context = {0};
    u32 scenario;
    u32 token;
    context.decoder = decoder;
    context.offloaded_logits = logits;
    if (decoder->model.vocabulary_size > sizeof(logits) / sizeof(logits[0])) return 1;
    for (scenario = 0U; scenario < 4U; ++scenario) {
        u32 expected;
        float expected_maximum;
        float maximum = -3.402823466e+38f;
        u32 best = DECODER_EOT;
        prepare_token_exclusions(decoder, scenario & 1U);
        for (token = 0U; token < decoder->model.vocabulary_size; ++token) {
            logits[token] = scenario == 0U ? 0U :
                whisper_frontend_float_to_half((float)(token % 127U) - 63.0f);
        }
        logits[3] = 0x7e00U;
        logits[4] = 0xfc00U;
        logits[5] = 0x8000U;
        if (scenario == 2U) {
            logits[100] = 0x7c00U;
            logits[200] = 0x7c00U;
            exclude_token(decoder, 100U);
        }
        context.temperature = scenario == 3U ? 0.2f : 0.0f;
        context.gumbel_values = scenario == 3U ? prepare_gumbel_cache(decoder, 0U) : 0;
        if (scenario == 3U && context.gumbel_values == 0) return 2;
        context.partition_count = 1U;
        (void)logit_partition_range(0U, 1U, 0U, &context);
        expected = decoder->logit_tokens[0];
        expected_maximum = decoder->logit_maxima[0];
        context.partition_count = 12U;
        (void)logit_partition_range(0U, 12U, 0U, &context);
        for (token = 0U; token < 12U; ++token) {
            if (decoder->logit_maxima[token] > maximum ||
                (decoder->logit_maxima[token] == maximum && decoder->logit_tokens[token] < best)) {
                maximum = decoder->logit_maxima[token];
                best = decoder->logit_tokens[token];
            }
        }
        if (best != expected || maximum != expected_maximum) return 3;
    }
    return 0;
}

int whisper_decoder_test_gumbel_cache(WhisperDecoder *decoder, int allocation_fails) {
    float *values = prepare_gumbel_cache(decoder, 0U);
    LogitContext context = {0};
    u32 token;
    if (allocation_fails) return values != 0;
    if (values == 0 || prepare_gumbel_cache(decoder, decoder->model.text_context) != 0) return 1;
    context.sample_seed = 0x51f15e5eU;
    context.position = 0U;
    for (token = 0U; token < decoder->model.vocabulary_size; ++token) {
        values[token] = sample_gumbel(&context, token);
    }
    if (prepare_gumbel_cache(decoder, 0U) != values) return 2;
    for (token = 0U; token < decoder->model.vocabulary_size; ++token) {
        if (values[token] != sample_gumbel(&context, token)) return 3;
    }
    return 0;
}

int whisper_decoder_test_prefix_cache(WhisperDecoder *decoder) {
    u32 index;
    decoder->prefix_count = 0U;
    if (restore_prefix_hidden(decoder, 10U, 0U)) return 1;
    for (index = 0U; index < decoder->model.width; ++index) decoder->hidden[index] = (float)index;
    cache_prefix_hidden(decoder, 10U, 0U);
    for (index = 0U; index < decoder->model.width; ++index) decoder->hidden[index] = (float)index + 1.0f;
    cache_prefix_hidden(decoder, 11U, 1U);
    if (!restore_prefix_hidden(decoder, 10U, 0U)) return 2;
    for (index = 0U; index < decoder->model.width; ++index) {
        if (decoder->hidden[index] != (float)index) return 3;
    }
    if (!restore_prefix_hidden(decoder, 11U, 1U)) return 4;
    if (restore_prefix_hidden(decoder, 12U, 0U) || decoder->prefix_count != 0U) return 5;
    cache_prefix_hidden(decoder, 12U, 0U);
    if (restore_prefix_hidden(decoder, 11U, 1U)) return 6;
    decoder->prefix_count = 0U;
    return 0;
}

int whisper_decoder_test_token_exclusions(WhisperDecoder *decoder) {
    u32 first_generated;
    for (first_generated = 0U; first_generated < 2U; ++first_generated) {
        u32 token;
        prepare_token_exclusions(decoder, (int)first_generated);
        for (token = 0U; token < decoder->model.vocabulary_size; ++token) {
            u32 index;
            int expected = token >= DECODER_TEXT_TOKEN_LIMIT ||
                (token == DECODER_NO_SPEECH && !first_generated) ||
                (token == 220U && first_generated);
            for (index = 0U; index < sizeof(suppressed_tokens) / sizeof(suppressed_tokens[0]); ++index) {
                if (token == suppressed_tokens[index]) expected = 1;
            }
            if (token_excluded(decoder, token) != expected) return 1;
        }
        exclude_token(decoder, 0U);
        exclude_token(decoder, DECODER_TEXT_TOKEN_LIMIT - 1U);
        exclude_token(decoder, decoder->model.vocabulary_size);
        if (!token_excluded(decoder, 0U) || token_excluded(decoder, 3U)) return 2;
    }
    prepare_token_exclusions(decoder, 1);
    return token_excluded(decoder, 0U) ? 3 : 0;
}
#endif

static void write_token(
    WhisperDecoder *decoder,
    u32 token,
    WhisperDecoderWrite write_output
) {
    u32 start;
    u32 end;
    if (token >= decoder->model.vocabulary_size || write_output == 0) return;
    start = decoder->token_offsets[token];
    end = decoder->token_offsets[token + 1U];
    if (end > start && end <= decoder->token_byte_count) {
        write_output((const char *)decoder->token_bytes + start, end - start);
    }
}

static u32 repetition_penalty(const u16 *tokens, u32 count, int penalize_bigrams) {
    u32 duplicate_bigrams = 0U;
    u32 duplicate_trigrams = 0U;
    u32 index;
    if (penalize_bigrams) {
        for (index = 1U; index < count; ++index) {
            u32 previous;
            for (previous = 1U; previous < index; ++previous) {
                if (tokens[previous - 1U] == tokens[index - 1U] &&
                    tokens[previous] == tokens[index]) {
                    ++duplicate_bigrams;
                    break;
                }
            }
        }
    }
    for (index = 2U; index < count; ++index) {
        u32 previous;
        for (previous = 2U; previous < index; ++previous) {
            if (tokens[previous - 2U] == tokens[index - 2U] &&
                tokens[previous - 1U] == tokens[index - 1U] &&
                tokens[previous] == tokens[index]) {
                ++duplicate_trigrams;
                break;
            }
        }
    }
    return duplicate_bigrams + duplicate_trigrams * 4U;
}

static int decode_attempt(
    WhisperDecoder *decoder,
    u32 maximum_tokens,
    float temperature,
    u32 sample_seed
) {
    static const u16 prompt[] = {
        DECODER_SOT, DECODER_GERMAN, DECODER_TRANSCRIBE, DECODER_NO_TIMESTAMPS
    };
    u32 position = 0U;
    u32 next = DECODER_EOT;
    u32 index;
    for (index = 0U; index < sizeof(prompt) / sizeof(prompt[0]); ++index) {
        if (decoder->cancelled != 0 && decoder->cancelled(decoder->cancel_context)) {
            return WHISPER_DECODER_CANCELLED;
        }
        decoder->generated_tokens[position] = prompt[index];
        next = decoder_step(
            decoder, prompt[index], position,
            index + 1U == sizeof(prompt) / sizeof(prompt[0]),
            temperature, sample_seed,
            index + 1U == sizeof(prompt) / sizeof(prompt[0])
        );
        ++position;
    }
    for (index = 0U; index < maximum_tokens && next != DECODER_EOT &&
        next != DECODER_NO_SPEECH; ++index) {
        if (decoder->cancelled != 0 && decoder->cancelled(decoder->cancel_context)) {
            return WHISPER_DECODER_CANCELLED;
        }
        decoder->generated_tokens[position] = (u16)next;
        next = decoder_step(
            decoder, next, position, 0, temperature, sample_seed, 1
        );
        ++position;
    }
    return (int)index;
}

static int whisper_decoder_transcribe_impl(
    WhisperDecoder *decoder,
    const u16 *encoder_output,
    const u16 *cross_keys,
    const u16 *cross_values,
    u32 maximum_tokens,
    WhisperDecoderWrite write_output
) {
    static const float temperatures[] = {0.0f, 0.2f, 0.4f, 0.6f};
    enum { PROMPT_TOKENS = 4 };
    u32 attempt_count = decoder != 0 &&
        decoder->model.model_id != WHISPER_MODEL_ID_TINY ?
        (u32)(sizeof(temperatures) / sizeof(temperatures[0])) : 1U;
    u32 selected_count = 0U;
    u32 selected_penalty = ~0U;
    u32 attempt;
    u32 index;
    long long start;
    long long end;
    if (decoder == 0 || decoder->weight_storage == 0 ||
        decoder->token_offsets == 0 ||
        (encoder_output == 0 && (cross_keys == 0 || cross_values == 0))) return -1;
    if (maximum_tokens > decoder->model.text_context - PROMPT_TOKENS) {
        maximum_tokens = decoder->model.text_context -
            PROMPT_TOKENS;
    }
    if (decoder->cancelled != 0 && decoder->cancelled(decoder->cancel_context)) {
        return WHISPER_DECODER_CANCELLED;
    }
    decoder->profile.encoder_normalize_ticks = 0U;
    decoder->profile.cross_cache_ticks = 0U;
    decoder->profile.self_attention_ticks = 0U;
    decoder->profile.npu_self_attention_ticks = 0U;
    decoder->profile.npu_self_attention_execute_ticks = 0U;
    decoder->profile.cross_attention_ticks = 0U;
    decoder->profile.npu_cross_attention_ticks = 0U;
    decoder->profile.npu_cross_attention_execute_ticks = 0U;
    decoder->profile.npu_fused_cross_mlp_ticks = 0U;
    decoder->profile.npu_fused_cross_mlp_execute_ticks = 0U;
    decoder->profile.feed_forward_ticks = 0U;
    decoder->profile.npu_feed_forward_ticks = 0U;
    decoder->profile.npu_mlp_execute_ticks = 0U;
    decoder->profile.logits_ticks = 0U;
    decoder->profile.npu_logits_ticks = 0U;
    decoder->profile.npu_logits_execute_ticks = 0U;
    decoder->profile.npu_self_attention_calls = (WhisperDecoderNpuCalls){0};
    decoder->profile.npu_cross_attention_calls = (WhisperDecoderNpuCalls){0};
    decoder->profile.npu_fused_cross_mlp_calls = (WhisperDecoderNpuCalls){0};
    decoder->profile.npu_mlp_calls = (WhisperDecoderNpuCalls){0};
    decoder->profile.npu_logits_calls = (WhisperDecoderNpuCalls){0};
    decoder->profile.decoder_steps = 0U;
    decoder->profile.prefix_reused_steps = 0U;
    decoder->prefix_count = 0U;
    decoder->profile.worker_count = rt_task_pool_width(&decoder->pool);
    if (cross_keys != 0 && cross_values != 0) {
        QueryPerformanceCounter(&start);
        import_cross_attention_cache(decoder, cross_keys, cross_values);
        QueryPerformanceCounter(&end);
        decoder->profile.cross_cache_ticks = (u64)(end - start);
    } else {
        QueryPerformanceCounter(&start);
        normalize_encoder(decoder, encoder_output);
        QueryPerformanceCounter(&end);
        decoder->profile.encoder_normalize_ticks = (u64)(end - start);
        QueryPerformanceCounter(&start);
        prepare_cross_attention_cache(decoder);
        QueryPerformanceCounter(&end);
        decoder->profile.cross_cache_ticks = (u64)(end - start);
    }
    for (attempt = 0U; attempt < attempt_count; ++attempt) {
        int decoded = decode_attempt(
            decoder, maximum_tokens, temperatures[attempt], 0x51f15e5dU + attempt
        );
        if (decoded < 0) return decoded;
        u32 count = decoded < 0 ? 0U : (u32)decoded;
        u32 penalty = repetition_penalty(
            decoder->generated_tokens + PROMPT_TOKENS, count,
            decoder->model.model_id != WHISPER_MODEL_ID_SMALL
        );
        if (penalty < selected_penalty) {
            selected_count = count;
            selected_penalty = penalty;
            for (index = 0U; index < count; ++index) {
                decoder->selected_tokens[index] =
                    decoder->generated_tokens[PROMPT_TOKENS + index];
            }
        }
        if (penalty < 4U) break;
    }
    for (index = 0U; index < selected_count; ++index) {
        if (decoder->cancelled != 0 && decoder->cancelled(decoder->cancel_context)) {
            return WHISPER_DECODER_CANCELLED;
        }
        write_token(decoder, decoder->selected_tokens[index], write_output);
    }
    return (int)selected_count;
}

void whisper_decoder_set_trace(WhisperDecoder *decoder, WhisperDecoderTrace trace, void *context) {
    if (decoder == 0) return;
    decoder->trace = trace;
    decoder->trace_context = context;
}

void whisper_decoder_set_cancellation(
    WhisperDecoder *decoder, WhisperDecoderCancelled cancelled, void *context
) {
    if (decoder == 0) return;
    decoder->cancelled = cancelled;
    decoder->cancel_context = context;
}

int whisper_decoder_transcribe(
    WhisperDecoder *decoder,
    const u16 *encoder_output,
    u32 maximum_tokens,
    WhisperDecoderWrite write_output
) {
    return whisper_decoder_transcribe_impl(
        decoder, encoder_output, 0, 0, maximum_tokens, write_output
    );
}

int whisper_decoder_transcribe_with_cross_cache(
    WhisperDecoder *decoder,
    const u16 *cross_keys,
    const u16 *cross_values,
    u32 maximum_tokens,
    WhisperDecoderWrite write_output
) {
    return whisper_decoder_transcribe_impl(
        decoder, 0, cross_keys, cross_values, maximum_tokens, write_output
    );
}

const WhisperDecoderProfile *whisper_decoder_get_profile(
    const WhisperDecoder *decoder
) {
    return decoder == 0 ? 0 : &decoder->profile;
}

void whisper_decoder_set_mlp_offload(
    WhisperDecoder *decoder,
    WhisperDecoderMlpOffload offload,
    void *context
) {
    if (decoder == 0) return;
    decoder->mlp_offload = offload;
    decoder->mlp_offload_context = context;
}

void whisper_decoder_set_cross_attention_offload(
    WhisperDecoder *decoder,
    WhisperDecoderCrossAttentionOffload offload,
    void *context
) {
    if (decoder == 0) return;
    decoder->cross_attention_offload = offload;
    decoder->cross_attention_offload_context = context;
}

void whisper_decoder_set_fused_cross_mlp_offload(
    WhisperDecoder *decoder,
    WhisperDecoderFusedCrossMlpOffload offload,
    void *context
) {
    if (decoder == 0) return;
    decoder->fused_cross_mlp_offload = offload;
    decoder->fused_cross_mlp_offload_context = context;
}

void whisper_decoder_set_logits_offload(
    WhisperDecoder *decoder,
    WhisperDecoderLogitsOffload offload,
    void *context
) {
    if (decoder == 0) return;
    decoder->logits_offload = offload;
    decoder->logits_offload_context = context;
}

void whisper_decoder_set_self_attention_offload(
    WhisperDecoder *decoder,
    WhisperDecoderSelfAttentionOffload offload,
    void *context
) {
    if (decoder == 0) return;
    decoder->self_attention_offload = offload;
    decoder->self_attention_offload_context = context;
}

void whisper_decoder_shutdown(WhisperDecoder *decoder) {
    if (decoder == 0) return;
    if (decoder->pool_ready) {
        rt_task_pool_destroy(&decoder->pool);
        decoder->pool_ready = 0;
    }
    if (decoder->scratch_allocation != 0) {
        DECODER_FREE(decoder->scratch_allocation, 0U, 0x8000U);
        decoder->scratch_allocation = 0;
    }
    if (decoder->gumbel_cache != 0) {
        DECODER_FREE(decoder->gumbel_cache, 0U, 0x8000U);
        decoder->gumbel_cache = 0;
    }
    if (decoder->token_allocation != 0) {
        DECODER_FREE(decoder->token_allocation, 0U, 0x8000U);
        decoder->token_allocation = 0;
    }
    if (decoder->weight_allocation != 0) {
        DECODER_FREE(decoder->weight_allocation, 0U, 0x8000U);
        decoder->weight_allocation = 0;
    }
    DECODER_FREE(decoder, 0U, 0x8000U);
}
