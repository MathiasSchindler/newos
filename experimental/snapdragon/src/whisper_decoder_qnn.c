#include "whisper_decoder_qnn.h"
#include "whisper_artifact.h"
#include "whisper_frontend.h"

typedef unsigned long long usize;

__declspec(dllimport) int CloseHandle(void *handle);
__declspec(dllimport) void *CreateFileA(
    const char *name, u32 access, u32 sharing, void *security,
    u32 creation, u32 attributes, void *template_file
);
__declspec(dllimport) int ReadFile(
    void *handle, void *buffer, u32 size, u32 *read, void *overlapped
);
__declspec(dllimport) int QueryPerformanceCounter(long long *value);
__declspec(dllimport) void *GetProcAddress(void *module, const char *name);
__declspec(dllimport) void *LoadLibraryA(const char *name);
__declspec(dllimport) int FreeLibrary(void *module);
__declspec(dllimport) void *VirtualAlloc(
    void *address, usize size, u32 allocation_type, u32 protect
);
__declspec(dllimport) int VirtualFree(void *address, usize size, u32 free_type);

enum {
    DECODER_QNN_NAME_CAPACITY = 56,
    DECODER_QNN_PATH_CAPACITY = 192,
    DECODER_QNN_MLP_TENSORS = 8,
    DECODER_QNN_MLP_NODES = 3,
    DECODER_QNN_CROSS_TENSORS = 19,
    DECODER_QNN_CROSS_PARAMETER_TENSORS = 2,
    DECODER_QNN_CROSS_NODES = 10
};

typedef struct DecoderQnnMlpWeights {
    u16 *fc1_weight;
    u16 *fc1_bias;
    u16 *fc2_weight;
    u16 *fc2_bias;
} DecoderQnnMlpWeights;

typedef struct DecoderQnnCrossWeights {
    u16 *norm_weight;
    u16 *norm_bias;
    u16 *q_weight;
    u16 *q_bias;
    u16 *out_weight;
    u16 *out_bias;
} DecoderQnnCrossWeights;

typedef void *(*DecoderRpcMemAlloc)(i32 heap_id, u32 flags, i32 size);
typedef void (*DecoderRpcMemFree)(void *allocation);
typedef i32 (*DecoderRpcMemToFd)(void *allocation);

struct WhisperDecoderQnn {
    WhisperModelConfig model;
    u32 output_count;
    void *runtime_allocation;
    void *builder_allocation;
    void *mlp_builder_allocation;
    void *rpcmem_module;
    void *shared_cache_allocation;
    u16 *keys_cache;
    u16 *values_cache;
    u16 *mlp_input_buffer;
    u16 *mlp_output_buffer;
    u16 *norm_weight;
    u16 *norm_bias;
    u16 *projection_weights[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    u16 *projection_biases[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnGraphHandle graph;
    QnnGraphHandle mlp_graphs[WHISPER_DECODER_QNN_MAX_LAYERS];
    QnnGraphHandle cross_graphs[WHISPER_DECODER_QNN_MAX_LAYERS];
    QnnTensor input;
    QnnTensor mlp_inputs[WHISPER_DECODER_QNN_MAX_LAYERS];
    QnnTensor mlp_outputs[WHISPER_DECODER_QNN_MAX_LAYERS];
    QnnTensor outputs[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnTensor projection_outputs[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnTensor projection_splits[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnTensor execute_outputs[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnTensor cross_inputs[WHISPER_DECODER_QNN_MAX_LAYERS];
    QnnTensor cross_keys[WHISPER_DECODER_QNN_MAX_LAYERS];
    QnnTensor cross_values[WHISPER_DECODER_QNN_MAX_LAYERS];
    QnnTensor cross_outputs[WHISPER_DECODER_QNN_MAX_LAYERS];
    QnnTensor cross_build_tensors[DECODER_QNN_CROSS_TENSORS];
    QnnParam cross_build_parameters[3];
    QnnTensor *cross_registered[
        DECODER_QNN_CROSS_TENSORS + DECODER_QNN_CROSS_PARAMETER_TENSORS
    ];
    QnnMemHandle cache_handles[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnTensor norm_weight_tensor;
    QnnTensor norm_bias_tensor;
    QnnTensor norm_output_tensor;
    QnnTensor projection_weight_tensors[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnTensor projection_bias_tensors[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnParam norm_parameters[2];
    QnnParam projection_transpose_parameters[2];
    QnnTensor *registered[7U + WHISPER_DECODER_QNN_MAX_OUTPUTS * 5U];
    DecoderQnnMlpWeights mlp_weights[WHISPER_DECODER_QNN_MAX_LAYERS];
    DecoderQnnCrossWeights cross_weights[WHISPER_DECODER_QNN_MAX_LAYERS];
    const QnnInterfaceV2 *api;
    QnnContextHandle context;
    DecoderRpcMemFree rpcmem_free;
    int mlp_ready;
    int mlp_disabled;
    int cross_ready;
    int cross_disabled;
    int shared_cache_ready;
    int shared_cache_disabled;
    u32 activation_dimensions[2];
    u32 split_dimensions[3];
    u32 head_dimensions[3];
    u32 key_dimensions[3];
    u32 score_dimensions[3];
    u32 cross_split_dimensions[3];
    u32 cross_head_dimensions[3];
    u32 weight_dimensions[2];
    u32 width_dimensions[1];
    u32 vector_dimensions[1];
    u32 axes[1];
    u32 perm_dimensions[1];
    u32 head_perm[3];
    u32 key_perm[3];
    u32 mlp_activation_dimensions[2];
    u32 mlp_hidden_dimensions[2];
    u32 mlp_fc1_dimensions[2];
    u32 mlp_fc2_dimensions[2];
    u32 mlp_hidden_vector_dimensions[1];
    char graph_name[DECODER_QNN_NAME_CAPACITY];
    char input_name[DECODER_QNN_NAME_CAPACITY];
    char norm_weight_name[DECODER_QNN_NAME_CAPACITY];
    char norm_bias_name[DECODER_QNN_NAME_CAPACITY];
    char norm_output_name[DECODER_QNN_NAME_CAPACITY];
    char norm_axes_name[DECODER_QNN_NAME_CAPACITY];
    char norm_node_name[DECODER_QNN_NAME_CAPACITY];
    char weight_names[WHISPER_DECODER_QNN_MAX_OUTPUTS][DECODER_QNN_NAME_CAPACITY];
    char bias_names[WHISPER_DECODER_QNN_MAX_OUTPUTS][DECODER_QNN_NAME_CAPACITY];
    char output_names[WHISPER_DECODER_QNN_MAX_OUTPUTS][DECODER_QNN_NAME_CAPACITY];
    char node_names[WHISPER_DECODER_QNN_MAX_OUTPUTS][DECODER_QNN_NAME_CAPACITY];
    char projection_output_names[WHISPER_DECODER_QNN_MAX_OUTPUTS][DECODER_QNN_NAME_CAPACITY];
    char projection_split_names[WHISPER_DECODER_QNN_MAX_OUTPUTS][DECODER_QNN_NAME_CAPACITY];
    char projection_reshape_names[WHISPER_DECODER_QNN_MAX_OUTPUTS][DECODER_QNN_NAME_CAPACITY];
    char projection_transpose_names[WHISPER_DECODER_QNN_MAX_OUTPUTS][DECODER_QNN_NAME_CAPACITY];
    char projection_perm_names[2][DECODER_QNN_NAME_CAPACITY];
    char weight_path[DECODER_QNN_PATH_CAPACITY];
    char weight_fallback[DECODER_QNN_PATH_CAPACITY];
    char mlp_weight_path[DECODER_QNN_PATH_CAPACITY];
    char mlp_weight_fallback[DECODER_QNN_PATH_CAPACITY];
    char mlp_graph_names[WHISPER_DECODER_QNN_MAX_LAYERS][DECODER_QNN_NAME_CAPACITY];
    char mlp_tensor_names[WHISPER_DECODER_QNN_MAX_LAYERS]
        [DECODER_QNN_MLP_TENSORS][DECODER_QNN_NAME_CAPACITY];
    char mlp_node_names[WHISPER_DECODER_QNN_MAX_LAYERS]
        [DECODER_QNN_MLP_NODES][DECODER_QNN_NAME_CAPACITY];
    char cross_graph_names[WHISPER_DECODER_QNN_MAX_LAYERS][DECODER_QNN_NAME_CAPACITY];
    char cross_tensor_names[WHISPER_DECODER_QNN_MAX_LAYERS]
        [DECODER_QNN_CROSS_TENSORS + DECODER_QNN_CROSS_PARAMETER_TENSORS]
        [DECODER_QNN_NAME_CAPACITY];
    char cross_node_names[WHISPER_DECODER_QNN_MAX_LAYERS]
        [DECODER_QNN_CROSS_NODES][DECODER_QNN_NAME_CAPACITY];
};

static int append_text(char *output, u32 capacity, u32 *used, const char *text) {
    while (*text != '\0') {
        if (*used + 1U >= capacity) return 0;
        output[(*used)++] = *text++;
    }
    output[*used] = '\0';
    return 1;
}

static int append_u32(char *output, u32 capacity, u32 *used, u32 value) {
    char digits[10];
    u32 count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) {
        char digit[2];
        digit[0] = digits[--count];
        digit[1] = '\0';
        if (!append_text(output, capacity, used, digit)) return 0;
    }
    return 1;
}

static int make_model_name(
    char *output,
    u32 capacity,
    const WhisperModelConfig *model,
    const char *prefix,
    const char *suffix
) {
    u32 used = 0U;
    return append_text(output, capacity, &used, prefix) &&
        append_text(output, capacity, &used, model->name) &&
        append_text(output, capacity, &used, suffix);
}

static int make_projection_name(
    char *output,
    u32 capacity,
    const WhisperModelConfig *model,
    u32 projection,
    const char *suffix
) {
    u32 used = 0U;
    return append_text(output, capacity, &used, model->name) &&
        append_text(output, capacity, &used, "_decoder_l") &&
        append_u32(output, capacity, &used, projection / 2U) &&
        append_text(output, capacity, &used,
            (projection & 1U) == 0U ? "_cross_k" : "_cross_v") &&
        append_text(output, capacity, &used, suffix);
}

static int make_weight_path(
    char *output,
    u32 capacity,
    const char *prefix,
    const WhisperModelConfig *model
) {
    u32 used = 0U;
    return append_text(output, capacity, &used, prefix) &&
        append_text(output, capacity, &used, model->name) &&
        append_text(output, capacity, &used, "/decoder-fp16/cross-kv-fp16.bin");
}

static int make_mlp_weight_path(
    char *output,
    u32 capacity,
    const char *prefix,
    const WhisperModelConfig *model
) {
    u32 used = 0U;
    return append_text(output, capacity, &used, prefix) &&
        append_text(output, capacity, &used, model->name) &&
        append_text(output, capacity, &used, "/decoder-fp16/mlp-fp16.bin");
}

static int make_mlp_name(
    char *output,
    u32 capacity,
    const WhisperModelConfig *model,
    u32 layer,
    const char *suffix
) {
    u32 used = 0U;
    return append_text(output, capacity, &used, model->name) &&
        append_text(output, capacity, &used, "_decoder_l") &&
        append_u32(output, capacity, &used, layer) &&
        append_text(output, capacity, &used, "_mlp_") &&
        append_text(output, capacity, &used, suffix);
}

static int make_cross_name(
    char *output,
    u32 capacity,
    const WhisperModelConfig *model,
    u32 layer,
    const char *suffix
) {
    u32 used = 0U;
    return append_text(output, capacity, &used, model->name) &&
        append_text(output, capacity, &used, "_decoder_l") &&
        append_u32(output, capacity, &used, layer) &&
        append_text(output, capacity, &used, "_cross_attention_") &&
        append_text(output, capacity, &used, suffix);
}

static int initialize_names(WhisperDecoderQnn *decoder) {
    u32 index;
    u32 layer;
    const WhisperModelConfig *model = &decoder->model;
    if (!make_model_name(
            decoder->graph_name, sizeof(decoder->graph_name), model,
            "whisper_", "_decoder_cross_kv_fp16"
        ) || !make_model_name(
            decoder->input_name, sizeof(decoder->input_name), model,
            "", "_decoder_cross_input"
        ) || !make_model_name(
            decoder->norm_weight_name, sizeof(decoder->norm_weight_name), model,
            "", "_decoder_cross_norm_weight"
        ) || !make_model_name(
            decoder->norm_bias_name, sizeof(decoder->norm_bias_name), model,
            "", "_decoder_cross_norm_bias"
        ) || !make_model_name(
            decoder->norm_output_name, sizeof(decoder->norm_output_name), model,
            "", "_decoder_cross_norm_output"
        ) || !make_model_name(
            decoder->norm_axes_name, sizeof(decoder->norm_axes_name), model,
            "", "_decoder_cross_norm_axes"
        ) || !make_model_name(
            decoder->norm_node_name, sizeof(decoder->norm_node_name), model,
            "", "_decoder_cross_norm"
        ) || !make_model_name(
            decoder->projection_perm_names[0], DECODER_QNN_NAME_CAPACITY, model,
            "", "_decoder_cross_head_perm"
        ) || !make_model_name(
            decoder->projection_perm_names[1], DECODER_QNN_NAME_CAPACITY, model,
            "", "_decoder_cross_key_perm"
        ) || !make_weight_path(
            decoder->weight_path, sizeof(decoder->weight_path),
            "experimental/snapdragon/models/whisper-", model
        ) || !make_weight_path(
            decoder->weight_fallback, sizeof(decoder->weight_fallback),
            "../models/whisper-", model
        ) || !make_mlp_weight_path(
            decoder->mlp_weight_path, sizeof(decoder->mlp_weight_path),
            "experimental/snapdragon/models/whisper-", model
        ) || !make_mlp_weight_path(
            decoder->mlp_weight_fallback, sizeof(decoder->mlp_weight_fallback),
            "../models/whisper-", model
        )) return 0;
    for (index = 0U; index < decoder->output_count; ++index) {
        if (!make_projection_name(
                decoder->weight_names[index], sizeof(decoder->weight_names[index]),
                model, index, "_weight"
            ) || !make_projection_name(
                decoder->bias_names[index], sizeof(decoder->bias_names[index]),
                model, index, "_bias"
            ) || !make_projection_name(
                decoder->output_names[index], sizeof(decoder->output_names[index]),
                model, index, ""
            ) || !make_projection_name(
                decoder->node_names[index], sizeof(decoder->node_names[index]),
                model, index, "_fc"
            ) || !make_projection_name(
                decoder->projection_output_names[index], DECODER_QNN_NAME_CAPACITY,
                model, index, "_projected"
            ) || !make_projection_name(
                decoder->projection_split_names[index], DECODER_QNN_NAME_CAPACITY,
                model, index, "_split"
            ) || !make_projection_name(
                decoder->projection_reshape_names[index], DECODER_QNN_NAME_CAPACITY,
                model, index, "_reshape"
            ) || !make_projection_name(
                decoder->projection_transpose_names[index], DECODER_QNN_NAME_CAPACITY,
                model, index, "_transpose"
            )) return 0;
    }
    for (layer = 0U; layer < model->decoder_layers; ++layer) {
        static const char *tensor_suffixes[DECODER_QNN_MLP_TENSORS] = {
            "input", "fc1_weight", "fc1_bias", "fc1_output",
            "gelu_output", "fc2_weight", "fc2_bias", "output"
        };
        static const char *node_suffixes[DECODER_QNN_MLP_NODES] = {
            "fc1", "gelu", "fc2"
        };
        static const char *cross_tensor_suffixes[
            DECODER_QNN_CROSS_TENSORS + DECODER_QNN_CROSS_PARAMETER_TENSORS
        ] = {
            "input", "norm_weight", "norm_bias", "norm_output",
            "q_weight", "q_bias", "q_output", "q_split", "q_heads",
            "key", "value", "scores", "probabilities", "attended_heads",
            "attended_split", "attended_flat", "out_weight", "out_bias",
            "output", "norm_axes", "head_perm"
        };
        static const char *cross_node_suffixes[DECODER_QNN_CROSS_NODES] = {
            "norm", "q_fc", "q_reshape", "q_transpose", "scores",
            "softmax", "values", "inverse_transpose", "flatten", "out_fc"
        };
        if (!make_mlp_name(
                decoder->mlp_graph_names[layer], DECODER_QNN_NAME_CAPACITY,
                model, layer, "graph"
            )) return 0;
        for (index = 0U; index < DECODER_QNN_MLP_TENSORS; ++index) {
            if (!make_mlp_name(
                    decoder->mlp_tensor_names[layer][index],
                    DECODER_QNN_NAME_CAPACITY, model, layer,
                    tensor_suffixes[index]
                )) return 0;
        }
        for (index = 0U; index < DECODER_QNN_MLP_NODES; ++index) {
            if (!make_mlp_name(
                    decoder->mlp_node_names[layer][index],
                    DECODER_QNN_NAME_CAPACITY, model, layer,
                    node_suffixes[index]
                )) return 0;
        }
            if (!make_cross_name(
                decoder->cross_graph_names[layer], DECODER_QNN_NAME_CAPACITY,
                model, layer, "graph"
                )) return 0;
            for (index = 0U; index <
                   DECODER_QNN_CROSS_TENSORS + DECODER_QNN_CROSS_PARAMETER_TENSORS;
                   ++index) {
                if (!make_cross_name(
                    decoder->cross_tensor_names[layer][index],
                    DECODER_QNN_NAME_CAPACITY, model, layer,
                    cross_tensor_suffixes[index]
                )) return 0;
            }
            for (index = 0U; index < DECODER_QNN_CROSS_NODES; ++index) {
                if (!make_cross_name(
                    decoder->cross_node_names[layer][index],
                    DECODER_QNN_NAME_CAPACITY, model, layer,
                    cross_node_suffixes[index]
                )) return 0;
            }
    }
    return 1;
}

static int decoder_qnn_read_exact(void *handle, void *buffer, u32 size) {
    u8 *bytes = (u8 *)buffer;
    while (size != 0U) {
        u32 count = 0U;
        if (!ReadFile(handle, bytes, size, &count, 0) || count == 0U) return 0;
        bytes += count;
        size -= count;
    }
    return 1;
}

static int decoder_qnn_load_weights(WhisperDecoderQnn *decoder) {
    void *invalid = (void *)(usize)-1;
    void *handle = CreateFileA(
        decoder->weight_path, 0x80000000U, 1U, 0, 3U, 0x80U, 0
    );
    u8 header_bytes[WHISPER_ARTIFACT_HEADER_SIZE];
    WhisperArtifactHeader header;
    u64 expected_values;
    u64 expected_payload_size;
    u64 builder_size;
    u64 width_squared;
    u16 *cursor;
    u16 *zero_bias;
    u32 layer;
    const WhisperModelConfig *config = &decoder->model;
    expected_values = whisper_model_cross_kv_weight_count(config);
    if (!whisper_model_config_valid(config) ||
        expected_values == 0U ||
        !whisper_model_size_multiply(
            expected_values, sizeof(u16), &expected_payload_size
        ) || !whisper_model_size_multiply(
            config->width, config->width, &width_squared
        ) || !whisper_model_size_add(
            expected_payload_size, (u64)config->width * sizeof(u16), &builder_size
        ) || expected_payload_size > 0xffffffffULL) {
        return -1;
    }
    if (handle == invalid) {
        handle = CreateFileA(
            decoder->weight_fallback, 0x80000000U, 1U, 0, 3U, 0x80U, 0
        );
    }
    if (handle == invalid) return 0;
    if (!decoder_qnn_read_exact(handle, header_bytes, sizeof(header_bytes)) ||
        !whisper_artifact_decode_header(header_bytes, &header) ||
        !whisper_artifact_header_valid(
            &header, config, WHISPER_ARTIFACT_PAYLOAD_CROSS_KV_WEIGHTS,
            WHISPER_ARTIFACT_ELEMENT_F16, expected_values,
            expected_payload_size
        )) {
        CloseHandle(handle);
        return -1;
    }
    decoder->builder_allocation = VirtualAlloc(
        0, (usize)builder_size, 0x3000U, 0x04U
    );
    if (decoder->builder_allocation == 0 || !decoder_qnn_read_exact(
            handle, decoder->builder_allocation, (u32)expected_payload_size
        )) {
        CloseHandle(handle);
        return -1;
    }
    CloseHandle(handle);
    if (!whisper_artifact_payload_valid(
            &header, decoder->builder_allocation, expected_payload_size
        )) return -1;
    cursor = (u16 *)decoder->builder_allocation;
    decoder->norm_weight = cursor;
    cursor += config->width;
    decoder->norm_bias = cursor;
    cursor += config->width;
    zero_bias = (u16 *)((u8 *)decoder->builder_allocation + expected_payload_size);
    for (layer = 0U; layer < config->decoder_layers; ++layer) {
        u32 key_index = layer * 2U;
        u32 value_index = key_index + 1U;
        decoder->projection_weights[key_index] = cursor;
        cursor += width_squared;
        decoder->projection_biases[key_index] = zero_bias;
        decoder->projection_weights[value_index] = cursor;
        cursor += width_squared;
        decoder->projection_biases[value_index] = cursor;
        cursor += config->width;
    }
    for (layer = 0U; layer < config->decoder_layers; ++layer) {
        DecoderQnnCrossWeights *weights = &decoder->cross_weights[layer];
        weights->norm_weight = cursor;
        cursor += config->width;
        weights->norm_bias = cursor;
        cursor += config->width;
        weights->q_weight = cursor;
        cursor += width_squared;
        weights->q_bias = cursor;
        cursor += config->width;
        weights->out_weight = cursor;
        cursor += width_squared;
        weights->out_bias = cursor;
        cursor += config->width;
    }
    return cursor == (u16 *)((u8 *)decoder->builder_allocation + expected_payload_size)
        ? 1 : -1;
}

static int decoder_qnn_load_mlp_weights(WhisperDecoderQnn *decoder) {
    void *invalid = (void *)(usize)-1;
    void *handle = CreateFileA(
        decoder->mlp_weight_path, 0x80000000U, 1U, 0, 3U, 0x80U, 0
    );
    u8 header_bytes[WHISPER_ARTIFACT_HEADER_SIZE];
    WhisperArtifactHeader header;
    u64 matrix_values;
    u64 layer_values;
    u64 expected_values;
    u64 expected_bytes;
    u16 *cursor;
    u32 layer;
    const WhisperModelConfig *model = &decoder->model;
    if (!whisper_model_size_multiply(
            model->width, model->ffn_width, &matrix_values
        ) || !whisper_model_size_multiply(2U, matrix_values, &layer_values) ||
        !whisper_model_size_add(layer_values, model->ffn_width, &layer_values) ||
        !whisper_model_size_add(layer_values, model->width, &layer_values) ||
        !whisper_model_size_multiply(
            layer_values, model->decoder_layers, &expected_values
        ) || !whisper_model_size_multiply(
            expected_values, sizeof(u16), &expected_bytes
        ) || expected_bytes > 0xffffffffULL) return -1;
    if (handle == invalid) {
        handle = CreateFileA(
            decoder->mlp_weight_fallback, 0x80000000U, 1U, 0, 3U, 0x80U, 0
        );
    }
    if (handle == invalid) return 0;
    if (!decoder_qnn_read_exact(handle, header_bytes, sizeof(header_bytes)) ||
        !whisper_artifact_decode_header(header_bytes, &header) ||
        !whisper_artifact_header_valid(
            &header, model, WHISPER_ARTIFACT_PAYLOAD_DECODER_MLP_WEIGHTS,
            WHISPER_ARTIFACT_ELEMENT_F16, expected_values, expected_bytes
        )) {
        CloseHandle(handle);
        return -1;
    }
    decoder->mlp_builder_allocation = VirtualAlloc(
        0, (usize)expected_bytes, 0x3000U, 0x04U
    );
    if (decoder->mlp_builder_allocation == 0 || !decoder_qnn_read_exact(
            handle, decoder->mlp_builder_allocation, (u32)expected_bytes
        )) {
        CloseHandle(handle);
        return -1;
    }
    CloseHandle(handle);
    if (!whisper_artifact_payload_valid(
            &header, decoder->mlp_builder_allocation, expected_bytes
        )) return -1;
    cursor = (u16 *)decoder->mlp_builder_allocation;
    for (layer = 0U; layer < model->decoder_layers; ++layer) {
        decoder->mlp_weights[layer].fc1_weight = cursor;
        cursor += matrix_values;
        decoder->mlp_weights[layer].fc1_bias = cursor;
        cursor += model->ffn_width;
        decoder->mlp_weights[layer].fc2_weight = cursor;
        cursor += matrix_values;
        decoder->mlp_weights[layer].fc2_bias = cursor;
        cursor += model->width;
    }
    return cursor == (u16 *)decoder->mlp_builder_allocation + expected_values
        ? 1 : -1;
}

WhisperDecoderQnn *whisper_decoder_qnn_create(const WhisperModelConfig *model) {
    WhisperDecoderQnn *decoder;
    u64 cache_values;
    u64 cache_bytes;
    u64 runtime_bytes;
    u64 vector_bytes;
    if (!whisper_model_config_valid(model) ||
        model->decoder_layers > WHISPER_DECODER_QNN_MAX_OUTPUTS / 2U ||
        !whisper_model_size_multiply(
            model->decoder_layers, model->encoder_frames, &cache_values
        ) || !whisper_model_size_multiply(
            cache_values, model->width, &cache_values
        ) || !whisper_model_size_multiply(
            cache_values, sizeof(u16), &cache_bytes
        ) || !whisper_model_size_multiply(2U, cache_bytes, &runtime_bytes) ||
        !whisper_model_size_multiply(model->width, sizeof(u16), &vector_bytes) ||
        !whisper_model_size_add(runtime_bytes, 2U * vector_bytes, &runtime_bytes) ||
        model->decoder_layers > WHISPER_DECODER_QNN_MAX_LAYERS) {
        return 0;
    }
    decoder = VirtualAlloc(0, sizeof(*decoder), 0x3000U, 0x04U);
    if (decoder == 0) return 0;
    decoder->model = *model;
    decoder->output_count = model->decoder_layers * 2U;
    decoder->runtime_allocation = VirtualAlloc(
        0, (usize)runtime_bytes, 0x3000U, 0x04U
    );
    if (decoder->runtime_allocation == 0) {
        whisper_decoder_qnn_shutdown(decoder);
        return 0;
    }
    decoder->keys_cache = (u16 *)decoder->runtime_allocation;
    decoder->values_cache = (u16 *)((u8 *)decoder->runtime_allocation + cache_bytes);
    decoder->mlp_input_buffer = (u16 *)((u8 *)decoder->values_cache + cache_bytes);
    decoder->mlp_output_buffer = decoder->mlp_input_buffer + model->width;
    decoder->activation_dimensions[0] = model->encoder_frames;
    decoder->activation_dimensions[1] = model->width;
    decoder->split_dimensions[0] = model->encoder_frames;
    decoder->split_dimensions[1] = model->attention_heads;
    decoder->split_dimensions[2] = model->width / model->attention_heads;
    decoder->head_dimensions[0] = model->attention_heads;
    decoder->head_dimensions[1] = model->encoder_frames;
    decoder->head_dimensions[2] = model->width / model->attention_heads;
    decoder->key_dimensions[0] = model->attention_heads;
    decoder->key_dimensions[1] = model->width / model->attention_heads;
    decoder->key_dimensions[2] = model->encoder_frames;
    decoder->score_dimensions[0] = model->attention_heads;
    decoder->score_dimensions[1] = 1U;
    decoder->score_dimensions[2] = model->encoder_frames;
    decoder->cross_split_dimensions[0] = 1U;
    decoder->cross_split_dimensions[1] = model->attention_heads;
    decoder->cross_split_dimensions[2] = model->width / model->attention_heads;
    decoder->cross_head_dimensions[0] = model->attention_heads;
    decoder->cross_head_dimensions[1] = 1U;
    decoder->cross_head_dimensions[2] = model->width / model->attention_heads;
    decoder->weight_dimensions[0] = model->width;
    decoder->weight_dimensions[1] = model->width;
    decoder->width_dimensions[0] = model->width;
    decoder->vector_dimensions[0] = 1U;
    decoder->axes[0] = 1U;
    decoder->perm_dimensions[0] = 3U;
    decoder->head_perm[0] = 1U;
    decoder->head_perm[1] = 0U;
    decoder->head_perm[2] = 2U;
    decoder->key_perm[0] = 1U;
    decoder->key_perm[1] = 2U;
    decoder->key_perm[2] = 0U;
    decoder->mlp_activation_dimensions[0] = 1U;
    decoder->mlp_activation_dimensions[1] = model->width;
    decoder->mlp_hidden_dimensions[0] = 1U;
    decoder->mlp_hidden_dimensions[1] = model->ffn_width;
    decoder->mlp_fc1_dimensions[0] = model->ffn_width;
    decoder->mlp_fc1_dimensions[1] = model->width;
    decoder->mlp_fc2_dimensions[0] = model->width;
    decoder->mlp_fc2_dimensions[1] = model->ffn_width;
    decoder->mlp_hidden_vector_dimensions[0] = model->ffn_width;
    if (!initialize_names(decoder)) {
        whisper_decoder_qnn_shutdown(decoder);
        return 0;
    }
    return decoder;
}

static QnnTensor decoder_qnn_tensor(
    const char *name,
    u32 type,
    u32 *dimensions,
    u32 rank
) {
    QnnTensor tensor = {0};
    tensor.version = QNN_TENSOR_VERSION_1;
    tensor.data.v1.name = name;
    tensor.data.v1.type = type;
    tensor.data.v1.data_format = QNN_TENSOR_DATA_FORMAT_DENSE;
    tensor.data.v1.data_type = QNN_DATATYPE_FLOAT_16;
    tensor.data.v1.quantize_params.encoding_definition = QNN_DEFINITION_UNDEFINED;
    tensor.data.v1.quantize_params.quantization_encoding =
        QNN_QUANTIZATION_ENCODING_UNDEFINED;
    tensor.data.v1.rank = rank;
    tensor.data.v1.dimensions = dimensions;
    tensor.data.v1.memory_type = QNN_TENSORMEMTYPE_RAW;
    return tensor;
}

static void decoder_qnn_release_shared_cache(WhisperDecoderQnn *decoder) {
    u32 index;
    if (decoder->api != 0) {
        for (index = 0U; index < decoder->output_count; ++index) {
            if (decoder->cache_handles[index] != 0) {
                (void)decoder->api->mem_deregister(
                    &decoder->cache_handles[index], 1U
                );
                decoder->cache_handles[index] = 0;
            }
        }
    }
    if (decoder->shared_cache_allocation != 0 && decoder->rpcmem_free != 0) {
        decoder->rpcmem_free(decoder->shared_cache_allocation);
    }
    if (decoder->rpcmem_module != 0) FreeLibrary(decoder->rpcmem_module);
    decoder->shared_cache_allocation = 0;
    decoder->rpcmem_module = 0;
    decoder->rpcmem_free = 0;
    decoder->shared_cache_ready = 0;
    decoder->keys_cache = (u16 *)decoder->runtime_allocation;
    decoder->values_cache = decoder->keys_cache +
        (u64)decoder->model.decoder_layers * decoder->model.encoder_frames *
        decoder->model.width;
}

static int decoder_qnn_register_shared_cache(WhisperDecoderQnn *decoder) {
    DecoderRpcMemAlloc rpcmem_alloc;
    DecoderRpcMemToFd rpcmem_to_fd;
    QnnMemDescriptor descriptor = {0};
    QnnHtpMemDescriptor htp_descriptor = {0};
    u64 layer_values = (u64)decoder->model.encoder_frames * decoder->model.width;
    u64 layer_bytes = layer_values * sizeof(u16);
    u64 cache_bytes = layer_bytes * decoder->model.decoder_layers;
    u64 total_bytes = cache_bytes * 2U;
    i32 fd;
    u32 index;
    if (decoder->shared_cache_ready) return 1;
    if (decoder->shared_cache_disabled || decoder->api == 0 ||
        decoder->context == 0 || decoder->api->mem_register == 0 ||
        decoder->api->mem_deregister == 0 || total_bytes > 0x7fffffffULL) {
        return 0;
    }
    decoder->rpcmem_module = LoadLibraryA("libcdsprpc.dll");
    if (decoder->rpcmem_module == 0) goto unavailable;
    rpcmem_alloc = (DecoderRpcMemAlloc)GetProcAddress(
        decoder->rpcmem_module, "rpcmem_alloc"
    );
    decoder->rpcmem_free = (DecoderRpcMemFree)GetProcAddress(
        decoder->rpcmem_module, "rpcmem_free"
    );
    rpcmem_to_fd = (DecoderRpcMemToFd)GetProcAddress(
        decoder->rpcmem_module, "rpcmem_to_fd"
    );
    if (rpcmem_alloc == 0 || decoder->rpcmem_free == 0 || rpcmem_to_fd == 0) {
        goto unavailable;
    }
    decoder->shared_cache_allocation = rpcmem_alloc(25, 1U, (i32)total_bytes);
    if (decoder->shared_cache_allocation == 0) goto unavailable;
    fd = rpcmem_to_fd(decoder->shared_cache_allocation);
    if (fd == -1) goto unavailable;
    descriptor.data_type = QNN_DATATYPE_FLOAT_16;
    descriptor.memory_type = QNN_MEM_TYPE_CUSTOM;
    descriptor.memory.custom_info = &htp_descriptor;
    htp_descriptor.type = QNN_HTP_MEM_SHARED_BUFFER;
    htp_descriptor.size = total_bytes;
    htp_descriptor.config.shared_buffer.fd = fd;
    for (index = 0U; index < decoder->output_count; ++index) {
        u32 layer = index / 2U;
        descriptor.shape.rank = decoder->outputs[index].data.v1.rank;
        descriptor.shape.dimensions = decoder->outputs[index].data.v1.dimensions;
        htp_descriptor.config.shared_buffer.offset =
            (index & 1U) == 0U ? layer * layer_bytes : cache_bytes + layer * layer_bytes;
        if (decoder->api->mem_register(
                decoder->context, &descriptor, 1U,
                &decoder->cache_handles[index]
            ) != 0U || decoder->cache_handles[index] == 0) {
            goto unavailable;
        }
    }
    decoder->keys_cache = (u16 *)decoder->shared_cache_allocation;
    decoder->values_cache = decoder->keys_cache + cache_bytes / sizeof(u16);
    decoder->shared_cache_ready = 1;
    return 1;

unavailable:
    decoder_qnn_release_shared_cache(decoder);
    decoder->shared_cache_disabled = 1;
    return 0;
}

static u64 decoder_qnn_add_node(
    WhisperDecoderQnn *decoder,
    const QnnInterfaceV2 *api,
    const char *name,
    const char *type,
    QnnTensor *input0,
    QnnTensor *input1,
    QnnTensor *input2,
    u32 input_count,
    QnnTensor *output,
    QnnParam *parameters,
    u32 parameter_count
) {
    QnnTensor inputs[3];
    QnnTensor outputs[1];
    QnnOpConfig operation = {0};
    u32 index;
    for (index = 0U; index < input_count; ++index) {
        QnnTensor *source = index == 0U ? input0 : (index == 1U ? input1 : input2);
        inputs[index] = *source;
    }
    outputs[0] = *output;
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = name;
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = type;
    operation.data.v1.parameter_count = parameter_count;
    operation.data.v1.parameters = parameters;
    operation.data.v1.input_count = input_count;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = outputs;
    return api->graph_add_node(decoder->graph, operation);
}

static u64 decoder_qnn_add_graph_node(
    const QnnInterfaceV2 *api,
    QnnGraphHandle graph,
    const char *name,
    const char *type,
    QnnTensor *input0,
    QnnTensor *input1,
    QnnTensor *input2,
    u32 input_count,
    QnnTensor *output,
    QnnParam *parameters,
    u32 parameter_count
) {
    QnnTensor inputs[3];
    QnnTensor outputs[1];
    QnnOpConfig operation = {0};
    u32 index;
    for (index = 0U; index < input_count; ++index) {
        QnnTensor *source = index == 0U ? input0 : (index == 1U ? input1 : input2);
        inputs[index] = *source;
    }
    outputs[0] = *output;
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = name;
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = type;
    operation.data.v1.parameter_count = parameter_count;
    operation.data.v1.parameters = parameters;
    operation.data.v1.input_count = input_count;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = outputs;
    return api->graph_add_node(graph, operation);
}

static int __attribute__((noinline)) decoder_qnn_build_cross_attention(
    WhisperDecoderQnn *decoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    WhisperDecoderQnnIds *ids_out
) {
    enum {
        CROSS_INPUT,
        CROSS_NORM_WEIGHT,
        CROSS_NORM_BIAS,
        CROSS_NORM_OUTPUT,
        CROSS_Q_WEIGHT,
        CROSS_Q_BIAS,
        CROSS_Q_OUTPUT,
        CROSS_Q_SPLIT,
        CROSS_Q_HEADS,
        CROSS_KEYS,
        CROSS_VALUES,
        CROSS_SCORES,
        CROSS_PROBABILITIES,
        CROSS_ATTENDED_HEADS,
        CROSS_ATTENDED_SPLIT,
        CROSS_ATTENDED_FLAT,
        CROSS_OUT_WEIGHT,
        CROSS_OUT_BIAS,
        CROSS_OUTPUT
    };
    u64 matrix_bytes = (u64)decoder->model.width *
        decoder->model.width * sizeof(u16);
    u64 width_bytes = (u64)decoder->model.width * sizeof(u16);
    u32 layer;
    if (matrix_bytes > 0xffffffffULL) return -1;
    for (layer = 0U; layer < decoder->model.decoder_layers; ++layer) {
        DecoderQnnCrossWeights *weights = &decoder->cross_weights[layer];
        QnnTensor *tensors = decoder->cross_build_tensors;
        QnnParam *parameters = decoder->cross_build_parameters;
        QnnTensor **registered = decoder->cross_registered;
        u32 registered_count = 0U;
        u32 index;
        if (api->graph_create(
                context, decoder->cross_graph_names[layer], 0,
                &decoder->cross_graphs[layer]
            ) != 0U) return -1;
        tensors[CROSS_INPUT] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_INPUT],
            QNN_TENSOR_TYPE_APP_WRITE, decoder->mlp_activation_dimensions, 2U
        );
        tensors[CROSS_NORM_WEIGHT] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_NORM_WEIGHT],
            QNN_TENSOR_TYPE_STATIC, decoder->width_dimensions, 1U
        );
        tensors[CROSS_NORM_WEIGHT].data.v1.memory.client_buffer.data =
            weights->norm_weight;
        tensors[CROSS_NORM_WEIGHT].data.v1.memory.client_buffer.data_size =
            (u32)width_bytes;
        tensors[CROSS_NORM_BIAS] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_NORM_BIAS],
            QNN_TENSOR_TYPE_STATIC, decoder->width_dimensions, 1U
        );
        tensors[CROSS_NORM_BIAS].data.v1.memory.client_buffer.data =
            weights->norm_bias;
        tensors[CROSS_NORM_BIAS].data.v1.memory.client_buffer.data_size =
            (u32)width_bytes;
        tensors[CROSS_NORM_OUTPUT] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_NORM_OUTPUT],
            QNN_TENSOR_TYPE_NATIVE, decoder->mlp_activation_dimensions, 2U
        );
        tensors[CROSS_Q_WEIGHT] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_Q_WEIGHT],
            QNN_TENSOR_TYPE_STATIC, decoder->weight_dimensions, 2U
        );
        tensors[CROSS_Q_WEIGHT].data.v1.memory.client_buffer.data = weights->q_weight;
        tensors[CROSS_Q_WEIGHT].data.v1.memory.client_buffer.data_size = (u32)matrix_bytes;
        tensors[CROSS_Q_BIAS] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_Q_BIAS],
            QNN_TENSOR_TYPE_STATIC, decoder->width_dimensions, 1U
        );
        tensors[CROSS_Q_BIAS].data.v1.memory.client_buffer.data = weights->q_bias;
        tensors[CROSS_Q_BIAS].data.v1.memory.client_buffer.data_size = (u32)width_bytes;
        tensors[CROSS_Q_OUTPUT] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_Q_OUTPUT],
            QNN_TENSOR_TYPE_NATIVE, decoder->mlp_activation_dimensions, 2U
        );
        tensors[CROSS_Q_SPLIT] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_Q_SPLIT],
            QNN_TENSOR_TYPE_NATIVE, decoder->cross_split_dimensions, 3U
        );
        tensors[CROSS_Q_HEADS] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_Q_HEADS],
            QNN_TENSOR_TYPE_NATIVE, decoder->cross_head_dimensions, 3U
        );
        tensors[CROSS_KEYS] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_KEYS],
            QNN_TENSOR_TYPE_APP_WRITE, decoder->key_dimensions, 3U
        );
        tensors[CROSS_VALUES] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_VALUES],
            QNN_TENSOR_TYPE_APP_WRITE, decoder->head_dimensions, 3U
        );
        tensors[CROSS_SCORES] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_SCORES],
            QNN_TENSOR_TYPE_NATIVE, decoder->score_dimensions, 3U
        );
        tensors[CROSS_PROBABILITIES] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_PROBABILITIES],
            QNN_TENSOR_TYPE_NATIVE, decoder->score_dimensions, 3U
        );
        tensors[CROSS_ATTENDED_HEADS] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_ATTENDED_HEADS],
            QNN_TENSOR_TYPE_NATIVE, decoder->cross_head_dimensions, 3U
        );
        tensors[CROSS_ATTENDED_SPLIT] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_ATTENDED_SPLIT],
            QNN_TENSOR_TYPE_NATIVE, decoder->cross_split_dimensions, 3U
        );
        tensors[CROSS_ATTENDED_FLAT] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_ATTENDED_FLAT],
            QNN_TENSOR_TYPE_NATIVE, decoder->mlp_activation_dimensions, 2U
        );
        tensors[CROSS_OUT_WEIGHT] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_OUT_WEIGHT],
            QNN_TENSOR_TYPE_STATIC, decoder->weight_dimensions, 2U
        );
        tensors[CROSS_OUT_WEIGHT].data.v1.memory.client_buffer.data = weights->out_weight;
        tensors[CROSS_OUT_WEIGHT].data.v1.memory.client_buffer.data_size =
            (u32)matrix_bytes;
        tensors[CROSS_OUT_BIAS] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_OUT_BIAS],
            QNN_TENSOR_TYPE_STATIC, decoder->width_dimensions, 1U
        );
        tensors[CROSS_OUT_BIAS].data.v1.memory.client_buffer.data = weights->out_bias;
        tensors[CROSS_OUT_BIAS].data.v1.memory.client_buffer.data_size =
            (u32)width_bytes;
        tensors[CROSS_OUTPUT] = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][CROSS_OUTPUT],
            QNN_TENSOR_TYPE_APP_READ, decoder->mlp_activation_dimensions, 2U
        );
        for (index = 0U; index < DECODER_QNN_CROSS_TENSORS; ++index) {
            registered[registered_count++] = &tensors[index];
        }
        parameters[0].type = QNN_PARAMTYPE_SCALAR;
        parameters[0].name = "epsilon";
        parameters[0].value.scalar.data_type = QNN_DATATYPE_FLOAT_32;
        parameters[0].value.scalar.value.float_value = 0.00001f;
        parameters[1].type = QNN_PARAMTYPE_TENSOR;
        parameters[1].name = "axes";
        parameters[1].value.tensor = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][DECODER_QNN_CROSS_TENSORS],
            QNN_TENSOR_TYPE_STATIC, decoder->vector_dimensions, 1U
        );
        parameters[1].value.tensor.data.v1.data_type = QNN_DATATYPE_UINT_32;
        parameters[1].value.tensor.data.v1.memory.client_buffer.data = decoder->axes;
        parameters[1].value.tensor.data.v1.memory.client_buffer.data_size =
            sizeof(decoder->axes);
        registered[registered_count++] = &parameters[1].value.tensor;
        parameters[2].type = QNN_PARAMTYPE_TENSOR;
        parameters[2].name = "perm";
        parameters[2].value.tensor = decoder_qnn_tensor(
            decoder->cross_tensor_names[layer][DECODER_QNN_CROSS_TENSORS + 1U],
            QNN_TENSOR_TYPE_STATIC, decoder->perm_dimensions, 1U
        );
        parameters[2].value.tensor.data.v1.data_type = QNN_DATATYPE_UINT_32;
        parameters[2].value.tensor.data.v1.memory.client_buffer.data = decoder->head_perm;
        parameters[2].value.tensor.data.v1.memory.client_buffer.data_size =
            sizeof(decoder->head_perm);
        registered[registered_count++] = &parameters[2].value.tensor;
        for (index = 0U; index < registered_count; ++index) {
            if (api->tensor_create_graph_tensor(
                    decoder->cross_graphs[layer], registered[index]
                ) != 0U) return -1;
        }
#define ADD_CROSS_NODE(node_index, type, input0, input1, input2, count, output, params, param_count) \
        if (decoder_qnn_add_graph_node( \
                api, decoder->cross_graphs[layer], \
                decoder->cross_node_names[layer][node_index], type, \
                input0, input1, input2, count, output, params, param_count \
            ) != 0U) return -1
        ADD_CROSS_NODE(0U, "LayerNorm", &tensors[CROSS_INPUT],
            &tensors[CROSS_NORM_WEIGHT], &tensors[CROSS_NORM_BIAS], 3U,
            &tensors[CROSS_NORM_OUTPUT], parameters, 2U);
        ADD_CROSS_NODE(1U, "FullyConnected", &tensors[CROSS_NORM_OUTPUT],
            &tensors[CROSS_Q_WEIGHT], &tensors[CROSS_Q_BIAS], 3U,
            &tensors[CROSS_Q_OUTPUT], 0, 0U);
        ADD_CROSS_NODE(2U, "Reshape", &tensors[CROSS_Q_OUTPUT], 0, 0, 1U,
            &tensors[CROSS_Q_SPLIT], 0, 0U);
        ADD_CROSS_NODE(3U, "Transpose", &tensors[CROSS_Q_SPLIT], 0, 0, 1U,
            &tensors[CROSS_Q_HEADS], &parameters[2], 1U);
        ADD_CROSS_NODE(4U, "MatMul", &tensors[CROSS_Q_HEADS],
            &tensors[CROSS_KEYS], 0, 2U, &tensors[CROSS_SCORES], 0, 0U);
        ADD_CROSS_NODE(5U, "Softmax", &tensors[CROSS_SCORES], 0, 0, 1U,
            &tensors[CROSS_PROBABILITIES], 0, 0U);
        ADD_CROSS_NODE(6U, "MatMul", &tensors[CROSS_PROBABILITIES],
            &tensors[CROSS_VALUES], 0, 2U, &tensors[CROSS_ATTENDED_HEADS], 0, 0U);
        ADD_CROSS_NODE(7U, "Transpose", &tensors[CROSS_ATTENDED_HEADS],
            0, 0, 1U, &tensors[CROSS_ATTENDED_SPLIT], &parameters[2], 1U);
        ADD_CROSS_NODE(8U, "Reshape", &tensors[CROSS_ATTENDED_SPLIT], 0, 0, 1U,
            &tensors[CROSS_ATTENDED_FLAT], 0, 0U);
        ADD_CROSS_NODE(9U, "FullyConnected", &tensors[CROSS_ATTENDED_FLAT],
            &tensors[CROSS_OUT_WEIGHT], &tensors[CROSS_OUT_BIAS], 3U,
            &tensors[CROSS_OUTPUT], 0, 0U);
#undef ADD_CROSS_NODE
        if (api->graph_finalize(decoder->cross_graphs[layer], 0, 0) != 0U) return -1;
        decoder->cross_inputs[layer] = tensors[CROSS_INPUT];
        decoder->cross_keys[layer] = tensors[CROSS_KEYS];
        decoder->cross_values[layer] = tensors[CROSS_VALUES];
        decoder->cross_outputs[layer] = tensors[CROSS_OUTPUT];
        if (ids_out != 0) {
            ids_out->cross_input_ids[layer] = tensors[CROSS_INPUT].data.v1.id;
            ids_out->cross_key_ids[layer] = tensors[CROSS_KEYS].data.v1.id;
            ids_out->cross_value_ids[layer] = tensors[CROSS_VALUES].data.v1.id;
            ids_out->cross_output_ids[layer] = tensors[CROSS_OUTPUT].data.v1.id;
        }
    }
    if (ids_out != 0) ids_out->cross_layer_count = decoder->model.decoder_layers;
    decoder->cross_ready = 1;
    return 1;
}

static int __attribute__((noinline)) decoder_qnn_build_mlps(
    WhisperDecoderQnn *decoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    WhisperDecoderQnnIds *ids_out
) {
    u64 matrix_bytes = (u64)decoder->model.width *
        decoder->model.ffn_width * sizeof(u16);
    u64 width_bytes = (u64)decoder->model.width * sizeof(u16);
    u64 hidden_bytes = (u64)decoder->model.ffn_width * sizeof(u16);
    u32 layer;
    int loaded = decoder_qnn_load_mlp_weights(decoder);
    if (loaded != 1 || matrix_bytes > 0xffffffffULL) return loaded;
    for (layer = 0U; layer < decoder->model.decoder_layers; ++layer) {
        DecoderQnnMlpWeights *weights = &decoder->mlp_weights[layer];
        QnnTensor tensors[DECODER_QNN_MLP_TENSORS];
        QnnTensor *registered[DECODER_QNN_MLP_TENSORS];
        u32 index;
        if (api->graph_create(
                context, decoder->mlp_graph_names[layer], 0,
                &decoder->mlp_graphs[layer]
            ) != 0U) return -1;
        tensors[0] = decoder_qnn_tensor(
            decoder->mlp_tensor_names[layer][0], QNN_TENSOR_TYPE_APP_WRITE,
            decoder->mlp_activation_dimensions, 2U
        );
        tensors[1] = decoder_qnn_tensor(
            decoder->mlp_tensor_names[layer][1], QNN_TENSOR_TYPE_STATIC,
            decoder->mlp_fc1_dimensions, 2U
        );
        tensors[1].data.v1.memory.client_buffer.data = weights->fc1_weight;
        tensors[1].data.v1.memory.client_buffer.data_size = (u32)matrix_bytes;
        tensors[2] = decoder_qnn_tensor(
            decoder->mlp_tensor_names[layer][2], QNN_TENSOR_TYPE_STATIC,
            decoder->mlp_hidden_vector_dimensions, 1U
        );
        tensors[2].data.v1.memory.client_buffer.data = weights->fc1_bias;
        tensors[2].data.v1.memory.client_buffer.data_size = (u32)hidden_bytes;
        tensors[3] = decoder_qnn_tensor(
            decoder->mlp_tensor_names[layer][3], QNN_TENSOR_TYPE_NATIVE,
            decoder->mlp_hidden_dimensions, 2U
        );
        tensors[4] = decoder_qnn_tensor(
            decoder->mlp_tensor_names[layer][4], QNN_TENSOR_TYPE_NATIVE,
            decoder->mlp_hidden_dimensions, 2U
        );
        tensors[5] = decoder_qnn_tensor(
            decoder->mlp_tensor_names[layer][5], QNN_TENSOR_TYPE_STATIC,
            decoder->mlp_fc2_dimensions, 2U
        );
        tensors[5].data.v1.memory.client_buffer.data = weights->fc2_weight;
        tensors[5].data.v1.memory.client_buffer.data_size = (u32)matrix_bytes;
        tensors[6] = decoder_qnn_tensor(
            decoder->mlp_tensor_names[layer][6], QNN_TENSOR_TYPE_STATIC,
            decoder->width_dimensions, 1U
        );
        tensors[6].data.v1.memory.client_buffer.data = weights->fc2_bias;
        tensors[6].data.v1.memory.client_buffer.data_size = (u32)width_bytes;
        tensors[7] = decoder_qnn_tensor(
            decoder->mlp_tensor_names[layer][7], QNN_TENSOR_TYPE_APP_READ,
            decoder->mlp_activation_dimensions, 2U
        );
        for (index = 0U; index < DECODER_QNN_MLP_TENSORS; ++index) {
            registered[index] = &tensors[index];
            if (api->tensor_create_graph_tensor(
                    decoder->mlp_graphs[layer], registered[index]
                ) != 0U) return -1;
        }
        if (decoder_qnn_add_graph_node(
                api, decoder->mlp_graphs[layer],
                decoder->mlp_node_names[layer][0], "FullyConnected",
                &tensors[0], &tensors[1], &tensors[2], 3U, &tensors[3], 0, 0U
            ) != 0U || decoder_qnn_add_graph_node(
                api, decoder->mlp_graphs[layer],
                decoder->mlp_node_names[layer][1], "Gelu",
                &tensors[3], 0, 0, 1U, &tensors[4], 0, 0U
            ) != 0U || decoder_qnn_add_graph_node(
                api, decoder->mlp_graphs[layer],
                decoder->mlp_node_names[layer][2], "FullyConnected",
                &tensors[4], &tensors[5], &tensors[6], 3U, &tensors[7], 0, 0U
            ) != 0U || api->graph_finalize(
                decoder->mlp_graphs[layer], 0, 0
            ) != 0U) return -1;
        decoder->mlp_inputs[layer] = tensors[0];
        decoder->mlp_outputs[layer] = tensors[7];
        if (ids_out != 0) {
            ids_out->mlp_input_ids[layer] = tensors[0].data.v1.id;
            ids_out->mlp_output_ids[layer] = tensors[7].data.v1.id;
        }
    }
    if (ids_out != 0) ids_out->mlp_layer_count = decoder->model.decoder_layers;
    decoder->api = api;
    decoder->mlp_ready = 1;
    return 1;
}

int whisper_decoder_qnn_build(
    WhisperDecoderQnn *decoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    WhisperDecoderQnnIds *ids_out
) {
    u32 registered_count = 0U;
    u32 index;
    u64 status;
    int loaded;
    u64 matrix_bytes;
    u64 vector_bytes;
    if (decoder == 0 || api == 0 || decoder->builder_allocation != 0 ||
        !whisper_model_size_multiply(
            decoder->model.width, decoder->model.width, &matrix_bytes
        ) || !whisper_model_size_multiply(matrix_bytes, sizeof(u16), &matrix_bytes) ||
        !whisper_model_size_multiply(
            decoder->model.width, sizeof(u16), &vector_bytes
        )) return -1;
    decoder->api = api;
    decoder->context = context;
    loaded = decoder_qnn_load_weights(decoder);
    if (loaded != 1) return loaded;
    status = api->graph_create(
        context, decoder->graph_name, 0, &decoder->graph
    );
    if (status != 0U) return -1;
    decoder->input = decoder_qnn_tensor(
        decoder->input_name, QNN_TENSOR_TYPE_APP_WRITE,
        decoder->activation_dimensions, 2U
    );
    decoder->norm_weight_tensor = decoder_qnn_tensor(
        decoder->norm_weight_name, QNN_TENSOR_TYPE_STATIC,
        decoder->width_dimensions, 1U
    );
    decoder->norm_weight_tensor.data.v1.memory.client_buffer.data = decoder->norm_weight;
    decoder->norm_weight_tensor.data.v1.memory.client_buffer.data_size = (u32)vector_bytes;
    decoder->norm_bias_tensor = decoder_qnn_tensor(
        decoder->norm_bias_name, QNN_TENSOR_TYPE_STATIC,
        decoder->width_dimensions, 1U
    );
    decoder->norm_bias_tensor.data.v1.memory.client_buffer.data = decoder->norm_bias;
    decoder->norm_bias_tensor.data.v1.memory.client_buffer.data_size = (u32)vector_bytes;
    decoder->norm_output_tensor = decoder_qnn_tensor(
        decoder->norm_output_name, QNN_TENSOR_TYPE_NATIVE,
        decoder->activation_dimensions, 2U
    );
    decoder->norm_parameters[0].type = QNN_PARAMTYPE_SCALAR;
    decoder->norm_parameters[0].name = "epsilon";
    decoder->norm_parameters[0].value.scalar.data_type = QNN_DATATYPE_FLOAT_32;
    decoder->norm_parameters[0].value.scalar.value.float_value = 0.00001f;
    decoder->norm_parameters[1].type = QNN_PARAMTYPE_TENSOR;
    decoder->norm_parameters[1].name = "axes";
    decoder->norm_parameters[1].value.tensor = decoder_qnn_tensor(
        decoder->norm_axes_name, QNN_TENSOR_TYPE_STATIC,
        decoder->vector_dimensions, 1U
    );
    decoder->norm_parameters[1].value.tensor.data.v1.data_type = QNN_DATATYPE_UINT_32;
    decoder->norm_parameters[1].value.tensor.data.v1.memory.client_buffer.data =
        decoder->axes;
    decoder->norm_parameters[1].value.tensor.data.v1.memory.client_buffer.data_size =
        sizeof(decoder->axes);
    decoder->registered[registered_count++] = &decoder->input;
    decoder->registered[registered_count++] = &decoder->norm_weight_tensor;
    decoder->registered[registered_count++] = &decoder->norm_bias_tensor;
    decoder->registered[registered_count++] = &decoder->norm_output_tensor;
    decoder->registered[registered_count++] = &decoder->norm_parameters[1].value.tensor;
    for (index = 0U; index < 2U; ++index) {
        decoder->projection_transpose_parameters[index].type = QNN_PARAMTYPE_TENSOR;
        decoder->projection_transpose_parameters[index].name = "perm";
        decoder->projection_transpose_parameters[index].value.tensor = decoder_qnn_tensor(
            decoder->projection_perm_names[index], QNN_TENSOR_TYPE_STATIC,
            decoder->perm_dimensions, 1U
        );
        decoder->projection_transpose_parameters[index].value.tensor.data.v1.data_type =
            QNN_DATATYPE_UINT_32;
        decoder->projection_transpose_parameters[index].value.tensor.data.v1.memory.client_buffer.data =
            index == 0U ? (void *)decoder->head_perm : (void *)decoder->key_perm;
        decoder->projection_transpose_parameters[index].value.tensor.data.v1.memory.client_buffer.data_size =
            sizeof(decoder->head_perm);
        decoder->registered[registered_count++] =
            &decoder->projection_transpose_parameters[index].value.tensor;
    }
    for (index = 0U; index < decoder->output_count; ++index) {
        decoder->projection_weight_tensors[index] = decoder_qnn_tensor(
            decoder->weight_names[index], QNN_TENSOR_TYPE_STATIC,
            decoder->weight_dimensions, 2U
        );
        decoder->projection_weight_tensors[index].data.v1.memory.client_buffer.data =
            decoder->projection_weights[index];
        decoder->projection_weight_tensors[index].data.v1.memory.client_buffer.data_size =
            (u32)matrix_bytes;
        decoder->projection_bias_tensors[index] = decoder_qnn_tensor(
            decoder->bias_names[index], QNN_TENSOR_TYPE_STATIC,
            decoder->width_dimensions, 1U
        );
        decoder->projection_bias_tensors[index].data.v1.memory.client_buffer.data =
            decoder->projection_biases[index];
        decoder->projection_bias_tensors[index].data.v1.memory.client_buffer.data_size =
            (u32)vector_bytes;
        decoder->projection_outputs[index] = decoder_qnn_tensor(
            decoder->projection_output_names[index], QNN_TENSOR_TYPE_NATIVE,
            decoder->activation_dimensions, 2U
        );
        decoder->projection_splits[index] = decoder_qnn_tensor(
            decoder->projection_split_names[index], QNN_TENSOR_TYPE_NATIVE,
            decoder->split_dimensions, 3U
        );
        decoder->outputs[index] = decoder_qnn_tensor(
            decoder->output_names[index], QNN_TENSOR_TYPE_APP_READ,
            (index & 1U) == 0U ? decoder->key_dimensions : decoder->head_dimensions,
            3U
        );
        decoder->registered[registered_count++] = &decoder->projection_weight_tensors[index];
        decoder->registered[registered_count++] = &decoder->projection_bias_tensors[index];
        decoder->registered[registered_count++] = &decoder->projection_outputs[index];
        decoder->registered[registered_count++] = &decoder->projection_splits[index];
        decoder->registered[registered_count++] = &decoder->outputs[index];
    }
    for (index = 0U; index < registered_count; ++index) {
        if (api->tensor_create_graph_tensor(
                decoder->graph, decoder->registered[index]) != 0U) {
            return -1;
        }
    }
    status = decoder_qnn_add_node(
        decoder, api, decoder->norm_node_name, "LayerNorm", &decoder->input,
        &decoder->norm_weight_tensor, &decoder->norm_bias_tensor, 3U,
        &decoder->norm_output_tensor, decoder->norm_parameters, 2U
    );
    if (status != 0U) return -1;
    for (index = 0U; index < decoder->output_count; ++index) {
        status = decoder_qnn_add_node(
            decoder, api, decoder->node_names[index], "FullyConnected",
            &decoder->norm_output_tensor,
            &decoder->projection_weight_tensors[index],
            &decoder->projection_bias_tensors[index], 3U,
            &decoder->projection_outputs[index], 0, 0U
        );
        if (status != 0U || decoder_qnn_add_node(
                decoder, api, decoder->projection_reshape_names[index], "Reshape",
                &decoder->projection_outputs[index], 0, 0, 1U,
                &decoder->projection_splits[index], 0, 0U
            ) != 0U || decoder_qnn_add_node(
                decoder, api, decoder->projection_transpose_names[index], "Transpose",
                &decoder->projection_splits[index], 0, 0, 1U,
                &decoder->outputs[index],
                &decoder->projection_transpose_parameters[
                    (index & 1U) == 0U ? 1U : 0U
                ], 1U
            ) != 0U) return -1;
    }
    if (api->graph_finalize(decoder->graph, 0, 0) != 0U) return -1;
    if (ids_out != 0) {
        ids_out->model_id = decoder->model.model_id;
        ids_out->output_count = decoder->output_count;
        ids_out->input_id = decoder->input.data.v1.id;
        for (index = 0U; index < decoder->output_count; ++index) {
            ids_out->output_ids[index] = decoder->outputs[index].data.v1.id;
        }
    }
    loaded = decoder_qnn_build_cross_attention(decoder, api, context, ids_out);
    if (loaded != 1) return loaded;
    return decoder_qnn_build_mlps(decoder, api, context, ids_out);
}

int whisper_decoder_qnn_restore(
    WhisperDecoderQnn *decoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    const WhisperDecoderQnnIds *ids
) {
    u32 index;
    if (decoder == 0 || ids == 0 || ids->model_id != decoder->model.model_id ||
        ids->output_count != decoder->output_count ||
        ids->mlp_layer_count != decoder->model.decoder_layers ||
        ids->cross_layer_count != decoder->model.decoder_layers ||
        api->graph_retrieve(
            context, decoder->graph_name, &decoder->graph) != 0U) {
        return 0;
    }
    decoder_qnn_release_shared_cache(decoder);
    decoder->shared_cache_disabled = 0;
    decoder->api = api;
    decoder->context = context;
    if (decoder->builder_allocation != 0) {
        VirtualFree(decoder->builder_allocation, 0U, 0x8000U);
        decoder->builder_allocation = 0;
    }
    if (decoder->mlp_builder_allocation != 0) {
        VirtualFree(decoder->mlp_builder_allocation, 0U, 0x8000U);
        decoder->mlp_builder_allocation = 0;
    }
    decoder->input = decoder_qnn_tensor(
        decoder->input_name, QNN_TENSOR_TYPE_APP_WRITE,
        decoder->activation_dimensions, 2U
    );
    decoder->input.data.v1.id = ids->input_id;
    for (index = 0U; index < decoder->output_count; ++index) {
        decoder->outputs[index] = decoder_qnn_tensor(
            decoder->output_names[index], QNN_TENSOR_TYPE_APP_READ,
            (index & 1U) == 0U ? decoder->key_dimensions : decoder->head_dimensions,
            3U
        );
        decoder->outputs[index].data.v1.id = ids->output_ids[index];
    }
    for (index = 0U; index < decoder->model.decoder_layers; ++index) {
        if (api->graph_retrieve(
                context, decoder->cross_graph_names[index],
                &decoder->cross_graphs[index]
            ) != 0U) return 0;
        decoder->cross_inputs[index] = decoder_qnn_tensor(
            decoder->cross_tensor_names[index][0], QNN_TENSOR_TYPE_APP_WRITE,
            decoder->mlp_activation_dimensions, 2U
        );
        decoder->cross_inputs[index].data.v1.id = ids->cross_input_ids[index];
        decoder->cross_keys[index] = decoder_qnn_tensor(
            decoder->cross_tensor_names[index][9], QNN_TENSOR_TYPE_APP_WRITE,
            decoder->key_dimensions, 3U
        );
        decoder->cross_keys[index].data.v1.id = ids->cross_key_ids[index];
        decoder->cross_values[index] = decoder_qnn_tensor(
            decoder->cross_tensor_names[index][10], QNN_TENSOR_TYPE_APP_WRITE,
            decoder->head_dimensions, 3U
        );
        decoder->cross_values[index].data.v1.id = ids->cross_value_ids[index];
        decoder->cross_outputs[index] = decoder_qnn_tensor(
            decoder->cross_tensor_names[index][18], QNN_TENSOR_TYPE_APP_READ,
            decoder->mlp_activation_dimensions, 2U
        );
        decoder->cross_outputs[index].data.v1.id = ids->cross_output_ids[index];
        if (api->graph_retrieve(
                context, decoder->mlp_graph_names[index],
                &decoder->mlp_graphs[index]
            ) != 0U) return 0;
        decoder->mlp_inputs[index] = decoder_qnn_tensor(
            decoder->mlp_tensor_names[index][0], QNN_TENSOR_TYPE_APP_WRITE,
            decoder->mlp_activation_dimensions, 2U
        );
        decoder->mlp_inputs[index].data.v1.id = ids->mlp_input_ids[index];
        decoder->mlp_outputs[index] = decoder_qnn_tensor(
            decoder->mlp_tensor_names[index][7], QNN_TENSOR_TYPE_APP_READ,
            decoder->mlp_activation_dimensions, 2U
        );
        decoder->mlp_outputs[index].data.v1.id = ids->mlp_output_ids[index];
    }
    decoder->api = api;
    decoder->cross_ready = 1;
    decoder->mlp_ready = 1;
    return 1;
}

u64 whisper_decoder_qnn_execute(
    WhisperDecoderQnn *decoder,
    const QnnInterfaceV2 *api,
    const u16 *encoder_output
) {
    QnnTensor input;
    u64 activation_values;
    u64 activation_bytes;
    u32 index;
    if (decoder == 0 || api == 0 || encoder_output == 0 || decoder->graph == 0 ||
        !whisper_model_size_multiply(
            decoder->model.encoder_frames, decoder->model.width, &activation_values
        ) || !whisper_model_size_multiply(
            activation_values, sizeof(u16), &activation_bytes
        ) || activation_bytes > 0xffffffffULL) return ~0ULL;
    input = decoder->input;
    input.data.v1.memory.client_buffer.data = (void *)encoder_output;
    input.data.v1.memory.client_buffer.data_size = (u32)activation_bytes;
    (void)decoder_qnn_register_shared_cache(decoder);
    for (index = 0U; index < decoder->output_count; ++index) {
        u32 layer = index / 2U;
        decoder->execute_outputs[index] = decoder->outputs[index];
        if (decoder->shared_cache_ready) {
            decoder->execute_outputs[index].data.v1.memory_type =
                QNN_TENSORMEMTYPE_MEMHANDLE;
            decoder->execute_outputs[index].data.v1.memory.memory_handle =
                decoder->cache_handles[index];
        } else {
            decoder->execute_outputs[index].data.v1.memory.client_buffer.data =
                (index & 1U) == 0U
                ? (void *)(decoder->keys_cache + layer * activation_values)
                : (void *)(decoder->values_cache + layer * activation_values);
            decoder->execute_outputs[index].data.v1.memory.client_buffer.data_size =
                (u32)activation_bytes;
        }
    }
    return api->graph_execute(
        decoder->graph, &input, 1U, decoder->execute_outputs,
        decoder->output_count, 0, 0
    );
}

const u16 *whisper_decoder_qnn_keys(const WhisperDecoderQnn *decoder) {
    return decoder == 0 ? 0 : decoder->keys_cache;
}

const u16 *whisper_decoder_qnn_values(const WhisperDecoderQnn *decoder) {
    return decoder == 0 ? 0 : decoder->values_cache;
}

int whisper_decoder_qnn_mlp_offload(
    void *context,
    u32 layer,
    const float *normalized,
    float *projected,
    u64 *execute_ticks
) {
    WhisperDecoderQnn *decoder = (WhisperDecoderQnn *)context;
    QnnTensor input;
    QnnTensor output;
    long long execute_start;
    long long execute_end;
    u64 status;
    u32 index;
    u32 vector_bytes;
    if (execute_ticks != 0) *execute_ticks = 0U;
    if (decoder == 0 || normalized == 0 || projected == 0 ||
        !decoder->mlp_ready || decoder->mlp_disabled ||
        layer >= decoder->model.decoder_layers || decoder->api == 0) return 0;
    vector_bytes = decoder->model.width * sizeof(u16);
    for (index = 0U; index < decoder->model.width; ++index) {
        decoder->mlp_input_buffer[index] =
            whisper_frontend_float_to_half(normalized[index]);
    }
    input = decoder->mlp_inputs[layer];
    input.data.v1.memory.client_buffer.data = decoder->mlp_input_buffer;
    input.data.v1.memory.client_buffer.data_size = vector_bytes;
    output = decoder->mlp_outputs[layer];
    output.data.v1.memory.client_buffer.data = decoder->mlp_output_buffer;
    output.data.v1.memory.client_buffer.data_size = vector_bytes;
    QueryPerformanceCounter(&execute_start);
    status = decoder->api->graph_execute(
        decoder->mlp_graphs[layer], &input, 1U, &output, 1U, 0, 0
    );
    QueryPerformanceCounter(&execute_end);
    if (execute_ticks != 0) {
        *execute_ticks = (u64)(execute_end - execute_start);
    }
    if (status != 0U) {
        decoder->mlp_disabled = 1;
        return 0;
    }
    for (index = 0U; index < decoder->model.width; ++index) {
        projected[index] =
            whisper_frontend_half_to_float(decoder->mlp_output_buffer[index]);
    }
    return 1;
}

int whisper_decoder_qnn_cross_attention_offload(
    void *context,
    u32 layer,
    const float *hidden,
    float *projected,
    u64 *execute_ticks
) {
    WhisperDecoderQnn *decoder = (WhisperDecoderQnn *)context;
    QnnTensor inputs[3];
    QnnTensor output;
    long long execute_start;
    long long execute_end;
    u64 status;
    u32 index;
    u32 vector_bytes;
    if (execute_ticks != 0) *execute_ticks = 0U;
    if (decoder == 0 || hidden == 0 || projected == 0 ||
        !decoder->cross_ready || decoder->cross_disabled ||
        !decoder->shared_cache_ready ||
        layer >= decoder->model.decoder_layers || decoder->api == 0) return 0;
    vector_bytes = decoder->model.width * sizeof(u16);
    for (index = 0U; index < decoder->model.width; ++index) {
        decoder->mlp_input_buffer[index] =
            whisper_frontend_float_to_half(hidden[index]);
    }
    inputs[0] = decoder->cross_inputs[layer];
    inputs[0].data.v1.memory.client_buffer.data = decoder->mlp_input_buffer;
    inputs[0].data.v1.memory.client_buffer.data_size = vector_bytes;
    inputs[1] = decoder->cross_keys[layer];
    inputs[1].data.v1.memory_type = QNN_TENSORMEMTYPE_MEMHANDLE;
    inputs[1].data.v1.memory.memory_handle = decoder->cache_handles[layer * 2U];
    inputs[2] = decoder->cross_values[layer];
    inputs[2].data.v1.memory_type = QNN_TENSORMEMTYPE_MEMHANDLE;
    inputs[2].data.v1.memory.memory_handle = decoder->cache_handles[layer * 2U + 1U];
    output = decoder->cross_outputs[layer];
    output.data.v1.memory.client_buffer.data = decoder->mlp_output_buffer;
    output.data.v1.memory.client_buffer.data_size = vector_bytes;
    QueryPerformanceCounter(&execute_start);
    status = decoder->api->graph_execute(
        decoder->cross_graphs[layer], inputs, 3U, &output, 1U, 0, 0
    );
    QueryPerformanceCounter(&execute_end);
    if (execute_ticks != 0) *execute_ticks = (u64)(execute_end - execute_start);
    if (status != 0U) {
        decoder->cross_disabled = 1;
        return 0;
    }
    for (index = 0U; index < decoder->model.width; ++index) {
        projected[index] =
            whisper_frontend_half_to_float(decoder->mlp_output_buffer[index]);
    }
    return 1;
}

void whisper_decoder_qnn_shutdown(WhisperDecoderQnn *decoder) {
    if (decoder == 0) return;
    decoder_qnn_release_shared_cache(decoder);
    if (decoder->builder_allocation != 0) {
        VirtualFree(decoder->builder_allocation, 0U, 0x8000U);
    }
    if (decoder->mlp_builder_allocation != 0) {
        VirtualFree(decoder->mlp_builder_allocation, 0U, 0x8000U);
    }
    if (decoder->runtime_allocation != 0) {
        VirtualFree(decoder->runtime_allocation, 0U, 0x8000U);
    }
    VirtualFree(decoder, 0U, 0x8000U);
}
