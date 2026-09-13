#include "whisper_encoder_qnn.h"
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
__declspec(dllimport) void *VirtualAlloc(
    void *address, usize size, u32 allocation_type, u32 protect
);
__declspec(dllimport) int VirtualFree(void *address, usize size, u32 free_type);

enum {
    ENCODER_QNN_MAX_LAYERS = 16,
    ENCODER_QNN_NAME_CAPACITY = 64,
    ENCODER_QNN_PATH_CAPACITY = 192,
    ENCODER_QNN_PROJECTIONS = 3,
    ENCODER_QNN_BLOCK_TENSORS = 39,
    ENCODER_QNN_REGISTERED_TENSORS = 43,
    ENCODER_QNN_NODES_PER_LAYER = 22
};

enum {
    BLOCK_INPUT = 0,
    BLOCK_LN1_SCALE = 1,
    BLOCK_LN1_BIAS = 2,
    BLOCK_LN1_OUTPUT = 3,
    BLOCK_PROJECTION_BASE = 4,
    BLOCK_SCORES = 19,
    BLOCK_PROBABILITIES = 20,
    BLOCK_ATTENDED_HEADS = 21,
    BLOCK_ATTENDED_SPLIT = 22,
    BLOCK_ATTENDED_FLAT = 23,
    BLOCK_OUT_WEIGHT = 24,
    BLOCK_OUT_BIAS = 25,
    BLOCK_ATTENTION_OUTPUT = 26,
    BLOCK_ATTENTION_RESIDUAL = 27,
    BLOCK_LN2_SCALE = 28,
    BLOCK_LN2_BIAS = 29,
    BLOCK_LN2_OUTPUT = 30,
    BLOCK_FC1_WEIGHT = 31,
    BLOCK_FC1_BIAS = 32,
    BLOCK_FC1_OUTPUT = 33,
    BLOCK_GELU_OUTPUT = 34,
    BLOCK_FC2_WEIGHT = 35,
    BLOCK_FC2_BIAS = 36,
    BLOCK_FC2_OUTPUT = 37,
    BLOCK_OUTPUT = 38
};

#define BLOCK_WEIGHT(tensors, projection) \
    (tensors)[BLOCK_PROJECTION_BASE + (projection) * 5U]
#define BLOCK_BIAS(tensors, projection) \
    (tensors)[BLOCK_PROJECTION_BASE + (projection) * 5U + 1U]
#define BLOCK_PROJECTION(tensors, projection) \
    (tensors)[BLOCK_PROJECTION_BASE + (projection) * 5U + 2U]
#define BLOCK_SPLIT(tensors, projection) \
    (tensors)[BLOCK_PROJECTION_BASE + (projection) * 5U + 3U]
#define BLOCK_HEADS(tensors, projection) \
    (tensors)[BLOCK_PROJECTION_BASE + (projection) * 5U + 4U]

typedef struct WhisperEncoderLayerWeights {
    u16 *ln1_scale;
    u16 *ln1_bias;
    u16 *projection_weight[ENCODER_QNN_PROJECTIONS];
    u16 *projection_bias[ENCODER_QNN_PROJECTIONS];
    u16 *out_weight;
    u16 *out_bias;
    u16 *ln2_scale;
    u16 *ln2_bias;
    u16 *fc1_weight;
    u16 *fc1_bias;
    u16 *fc2_weight;
    u16 *fc2_bias;
} WhisperEncoderLayerWeights;

struct WhisperEncoderQnn {
    WhisperModelConfig model;
    void *runtime_allocation;
    void *frontend_builder_allocation;
    void *encoder_builder_allocation;
    u16 *conv1_input;
    u16 *conv1_output;
    u16 *conv2_input;
    u16 *frontend_output;
    u16 *encoder_output;
    u16 *conv1_weight;
    u16 *conv1_bias;
    u16 *conv2_weight;
    u16 *conv2_bias;
    u16 *positions;
    WhisperEncoderLayerWeights layers[ENCODER_QNN_MAX_LAYERS];
    QnnGraphHandle frontend_graphs[2];
    QnnTensor frontend_inputs[2];
    QnnTensor frontend_outputs[2];
    QnnGraphHandle encoder_graph;
    QnnTensor encoder_input;
    QnnTensor encoder_output_tensor;
    QnnTensor block_tensors[ENCODER_QNN_BLOCK_TENSORS];
    QnnTensor *registered_tensors[ENCODER_QNN_REGISTERED_TENSORS];
    QnnParam norm_parameters[2][2];
    QnnParam transpose_parameters[2];
    u64 activation_bytes;
    u32 frontend_input_dimensions[2][2];
    u32 frontend_weight_dimensions[2][2];
    u32 frontend_output_dimensions[2][2];
    u32 activation_dimensions[2];
    u32 hidden_dimensions[2];
    u32 matrix_dimensions[2];
    u32 fc1_dimensions[2];
    u32 fc2_dimensions[2];
    u32 width_dimensions[1];
    u32 hidden_bias_dimensions[1];
    u32 split_dimensions[3];
    u32 head_dimensions[3];
    u32 key_dimensions[3];
    u32 score_dimensions[3];
    u32 vector_dimensions[1];
    u32 perm_dimensions[1];
    u32 axes[1];
    u32 head_perm[3];
    u32 key_perm[3];
    char frontend_path[ENCODER_QNN_PATH_CAPACITY];
    char frontend_fallback[ENCODER_QNN_PATH_CAPACITY];
    char encoder_path[ENCODER_QNN_PATH_CAPACITY];
    char encoder_fallback[ENCODER_QNN_PATH_CAPACITY];
    char frontend_graph_names[2][ENCODER_QNN_NAME_CAPACITY];
    char encoder_graph_name[ENCODER_QNN_NAME_CAPACITY];
    char frontend_names[2][8][ENCODER_QNN_NAME_CAPACITY];
    char tensor_names[ENCODER_QNN_MAX_LAYERS]
        [ENCODER_QNN_REGISTERED_TENSORS][ENCODER_QNN_NAME_CAPACITY];
    char node_names[ENCODER_QNN_MAX_LAYERS]
        [ENCODER_QNN_NODES_PER_LAYER][ENCODER_QNN_NAME_CAPACITY];
};

static int encoder_qnn_append(char *output, u32 capacity, u32 *used, const char *text) {
    while (*text != '\0') {
        if (*used + 1U >= capacity) return 0;
        output[(*used)++] = *text++;
    }
    output[*used] = '\0';
    return 1;
}

static int encoder_qnn_append_u32(char *output, u32 capacity, u32 *used, u32 value) {
    char digits[10];
    u32 count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) {
        char digit[2] = {digits[--count], '\0'};
        if (!encoder_qnn_append(output, capacity, used, digit)) return 0;
    }
    return 1;
}

static int encoder_qnn_model_name(
    char *output,
    u32 capacity,
    const WhisperModelConfig *model,
    const char *suffix
) {
    u32 used = 0U;
    return encoder_qnn_append(output, capacity, &used, "whisper_") &&
        encoder_qnn_append(output, capacity, &used, model->name) &&
        encoder_qnn_append(output, capacity, &used, suffix);
}

static int encoder_qnn_layer_name(
    char *output,
    u32 capacity,
    const WhisperModelConfig *model,
    u32 layer,
    const char *suffix
) {
    u32 used = 0U;
    return encoder_qnn_append(output, capacity, &used, model->name) &&
        encoder_qnn_append(output, capacity, &used, "_encoder_l") &&
        encoder_qnn_append_u32(output, capacity, &used, layer) &&
        encoder_qnn_append(output, capacity, &used, "_") &&
        encoder_qnn_append(output, capacity, &used, suffix);
}

static int encoder_qnn_path(
    char *output,
    u32 capacity,
    const char *prefix,
    const WhisperModelConfig *model,
    const char *file
) {
    u32 used = 0U;
    return encoder_qnn_append(output, capacity, &used, prefix) &&
        encoder_qnn_append(output, capacity, &used, model->name) &&
        encoder_qnn_append(output, capacity, &used, "/encoder-fp16/") &&
        encoder_qnn_append(output, capacity, &used, file);
}

static int encoder_qnn_read_exact(void *handle, void *buffer, u64 size) {
    u8 *bytes = buffer;
    while (size != 0U) {
        u32 chunk = size > 0x40000000ULL ? 0x40000000U : (u32)size;
        u32 count = 0U;
        if (!ReadFile(handle, bytes, chunk, &count, 0) || count != chunk) return 0;
        bytes += count;
        size -= count;
    }
    return 1;
}

static int encoder_qnn_load_artifact(
    WhisperEncoderQnn *encoder,
    const char *path,
    const char *fallback,
    u32 payload_type,
    u64 expected_values,
    void **allocation
) {
    void *invalid = (void *)(usize)-1;
    void *handle;
    WhisperArtifactHeader header;
    u8 header_bytes[WHISPER_ARTIFACT_HEADER_SIZE];
    u64 payload_size;
    if (!whisper_model_size_multiply(expected_values, sizeof(u16), &payload_size) ||
        payload_size > 0xffffffffULL) return -1;
    handle = CreateFileA(path, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    if (handle == invalid) {
        handle = CreateFileA(fallback, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    }
    if (handle == invalid) return 0;
    if (!encoder_qnn_read_exact(handle, header_bytes, sizeof(header_bytes)) ||
        !whisper_artifact_decode_header(header_bytes, &header) ||
        !whisper_artifact_header_valid(
            &header, &encoder->model, payload_type,
            WHISPER_ARTIFACT_ELEMENT_F16, expected_values, payload_size
        )) {
        CloseHandle(handle);
        return -1;
    }
    *allocation = VirtualAlloc(0, (usize)payload_size, 0x3000U, 0x04U);
    if (*allocation == 0 || !encoder_qnn_read_exact(handle, *allocation, payload_size)) {
        CloseHandle(handle);
        return -1;
    }
    CloseHandle(handle);
    return whisper_artifact_payload_valid(&header, *allocation, payload_size) ? 1 : -1;
}

static int encoder_qnn_initialize_names(WhisperEncoderQnn *encoder) {
    static const char *frontend_graph_suffixes[2] = {
        "_frontend_conv1_fp16", "_frontend_conv2_fp16"
    };
    static const char *frontend_suffixes[2][8] = {
        {"conv1_input", "conv1_weight", "conv1_bias", "conv1_fc", "conv1_output", "", "", ""},
        {"conv2_input", "conv2_weight", "conv2_bias", "conv2_fc", "conv2_gelu", "positions", "output", ""}
    };
    u32 graph;
    u32 layer;
    u32 index;
    if (!encoder_qnn_model_name(
            encoder->encoder_graph_name, sizeof(encoder->encoder_graph_name),
            &encoder->model, "_encoder_fp16"
        ) || !encoder_qnn_path(
            encoder->frontend_path, sizeof(encoder->frontend_path),
            "experimental/snapdragon/models/whisper-", &encoder->model,
            "frontend-fp16.bin"
        ) || !encoder_qnn_path(
            encoder->frontend_fallback, sizeof(encoder->frontend_fallback),
            "../models/whisper-", &encoder->model, "frontend-fp16.bin"
        ) || !encoder_qnn_path(
            encoder->encoder_path, sizeof(encoder->encoder_path),
            "experimental/snapdragon/models/whisper-", &encoder->model,
            "encoder-fp16.bin"
        ) || !encoder_qnn_path(
            encoder->encoder_fallback, sizeof(encoder->encoder_fallback),
            "../models/whisper-", &encoder->model, "encoder-fp16.bin"
        )) return 0;
    for (graph = 0U; graph < 2U; ++graph) {
        if (!encoder_qnn_model_name(
                encoder->frontend_graph_names[graph], ENCODER_QNN_NAME_CAPACITY,
                &encoder->model, frontend_graph_suffixes[graph]
            )) return 0;
        for (index = 0U; index < 7U; ++index) {
            if (frontend_suffixes[graph][index][0] != '\0' &&
                !encoder_qnn_layer_name(
                    encoder->frontend_names[graph][index], ENCODER_QNN_NAME_CAPACITY,
                    &encoder->model, graph, frontend_suffixes[graph][index]
                )) return 0;
        }
    }
    for (layer = 0U; layer < encoder->model.encoder_layers; ++layer) {
        for (index = 0U; index < ENCODER_QNN_REGISTERED_TENSORS; ++index) {
            char suffix[24];
            u32 used = 0U;
            if (!encoder_qnn_append(suffix, sizeof(suffix), &used, "tensor_") ||
                !encoder_qnn_append_u32(suffix, sizeof(suffix), &used, index) ||
                !encoder_qnn_layer_name(
                    encoder->tensor_names[layer][index], ENCODER_QNN_NAME_CAPACITY,
                    &encoder->model, layer, suffix
                )) return 0;
        }
        for (index = 0U; index < ENCODER_QNN_NODES_PER_LAYER; ++index) {
            char suffix[24];
            u32 used = 0U;
            if (!encoder_qnn_append(suffix, sizeof(suffix), &used, "node_") ||
                !encoder_qnn_append_u32(suffix, sizeof(suffix), &used, index) ||
                !encoder_qnn_layer_name(
                    encoder->node_names[layer][index], ENCODER_QNN_NAME_CAPACITY,
                    &encoder->model, layer, suffix
                )) return 0;
        }
    }
    return 1;
}

static QnnTensor encoder_qnn_tensor(
    const char *name,
    u32 type,
    u32 data_type,
    u32 *dimensions,
    u32 rank
) {
    QnnTensor tensor = {0};
    tensor.version = QNN_TENSOR_VERSION_1;
    tensor.data.v1.name = name;
    tensor.data.v1.type = type;
    tensor.data.v1.data_format = QNN_TENSOR_DATA_FORMAT_DENSE;
    tensor.data.v1.data_type = data_type;
    tensor.data.v1.quantize_params.encoding_definition = QNN_DEFINITION_UNDEFINED;
    tensor.data.v1.quantize_params.quantization_encoding =
        QNN_QUANTIZATION_ENCODING_UNDEFINED;
    tensor.data.v1.rank = rank;
    tensor.data.v1.dimensions = dimensions;
    tensor.data.v1.memory_type = QNN_TENSORMEMTYPE_RAW;
    return tensor;
}

static u64 encoder_qnn_add_node(
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
        inputs[index] = *(index == 0U ? input0 : (index == 1U ? input1 : input2));
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

WhisperEncoderQnn *whisper_encoder_qnn_create(const WhisperModelConfig *model) {
    WhisperEncoderQnn *encoder;
    u64 conv1_input_values;
    u64 conv1_output_values;
    u64 conv2_input_values;
    u64 activation_values;
    u64 runtime_values;
    u64 runtime_bytes;
    u16 *cursor;
    if (!whisper_model_config_valid(model) ||
        model->encoder_layers > ENCODER_QNN_MAX_LAYERS ||
        !whisper_model_size_multiply(
            WHISPER_FRAME_COUNT, model->mel_bins * 3U, &conv1_input_values
        ) || !whisper_model_size_multiply(
            WHISPER_FRAME_COUNT, model->width, &conv1_output_values
        ) || !whisper_model_size_multiply(
            model->encoder_frames, model->width * 3U, &conv2_input_values
        ) || !whisper_model_size_multiply(
            model->encoder_frames, model->width, &activation_values
        ) || !whisper_model_size_add(
            conv1_input_values, conv1_output_values, &runtime_values
        ) || !whisper_model_size_add(runtime_values, conv2_input_values, &runtime_values) ||
        !whisper_model_size_add(runtime_values, activation_values * 2U, &runtime_values) ||
        !whisper_model_size_multiply(runtime_values, sizeof(u16), &runtime_bytes)) {
        return 0;
    }
    encoder = VirtualAlloc(0, sizeof(*encoder), 0x3000U, 0x04U);
    if (encoder == 0) return 0;
    encoder->model = *model;
    encoder->runtime_allocation = VirtualAlloc(
        0, (usize)runtime_bytes, 0x3000U, 0x04U
    );
    if (encoder->runtime_allocation == 0) {
        whisper_encoder_qnn_shutdown(encoder);
        return 0;
    }
    cursor = encoder->runtime_allocation;
    encoder->conv1_input = cursor;
    cursor += conv1_input_values;
    encoder->conv1_output = cursor;
    cursor += conv1_output_values;
    encoder->conv2_input = cursor;
    cursor += conv2_input_values;
    encoder->frontend_output = cursor;
    cursor += activation_values;
    encoder->encoder_output = cursor;
    encoder->activation_bytes = activation_values * sizeof(u16);
    encoder->frontend_input_dimensions[0][0] = WHISPER_FRAME_COUNT;
    encoder->frontend_input_dimensions[0][1] = model->mel_bins * 3U;
    encoder->frontend_input_dimensions[1][0] = model->encoder_frames;
    encoder->frontend_input_dimensions[1][1] = model->width * 3U;
    encoder->frontend_weight_dimensions[0][0] = model->width;
    encoder->frontend_weight_dimensions[0][1] = model->mel_bins * 3U;
    encoder->frontend_weight_dimensions[1][0] = model->width;
    encoder->frontend_weight_dimensions[1][1] = model->width * 3U;
    encoder->frontend_output_dimensions[0][0] = WHISPER_FRAME_COUNT;
    encoder->frontend_output_dimensions[0][1] = model->width;
    encoder->frontend_output_dimensions[1][0] = model->encoder_frames;
    encoder->frontend_output_dimensions[1][1] = model->width;
    encoder->activation_dimensions[0] = model->encoder_frames;
    encoder->activation_dimensions[1] = model->width;
    encoder->hidden_dimensions[0] = model->encoder_frames;
    encoder->hidden_dimensions[1] = model->ffn_width;
    encoder->matrix_dimensions[0] = model->width;
    encoder->matrix_dimensions[1] = model->width;
    encoder->fc1_dimensions[0] = model->ffn_width;
    encoder->fc1_dimensions[1] = model->width;
    encoder->fc2_dimensions[0] = model->width;
    encoder->fc2_dimensions[1] = model->ffn_width;
    encoder->width_dimensions[0] = model->width;
    encoder->hidden_bias_dimensions[0] = model->ffn_width;
    encoder->split_dimensions[0] = model->encoder_frames;
    encoder->split_dimensions[1] = model->attention_heads;
    encoder->split_dimensions[2] = model->width / model->attention_heads;
    encoder->head_dimensions[0] = model->attention_heads;
    encoder->head_dimensions[1] = model->encoder_frames;
    encoder->head_dimensions[2] = model->width / model->attention_heads;
    encoder->key_dimensions[0] = model->attention_heads;
    encoder->key_dimensions[1] = model->width / model->attention_heads;
    encoder->key_dimensions[2] = model->encoder_frames;
    encoder->score_dimensions[0] = model->attention_heads;
    encoder->score_dimensions[1] = model->encoder_frames;
    encoder->score_dimensions[2] = model->encoder_frames;
    encoder->vector_dimensions[0] = 1U;
    encoder->perm_dimensions[0] = 3U;
    encoder->axes[0] = 1U;
    encoder->head_perm[0] = 1U;
    encoder->head_perm[1] = 0U;
    encoder->head_perm[2] = 2U;
    encoder->key_perm[0] = 1U;
    encoder->key_perm[1] = 2U;
    encoder->key_perm[2] = 0U;
    if (!encoder_qnn_initialize_names(encoder)) {
        whisper_encoder_qnn_shutdown(encoder);
        return 0;
    }
    return encoder;
}

static int encoder_qnn_load_weights(WhisperEncoderQnn *encoder) {
    u64 frontend_values = whisper_model_frontend_weight_count(&encoder->model);
    u64 encoder_values = whisper_model_encoder_weight_count(&encoder->model);
    u64 width_squared;
    u16 *cursor;
    u32 layer;
    int loaded = encoder_qnn_load_artifact(
        encoder, encoder->frontend_path, encoder->frontend_fallback,
        WHISPER_ARTIFACT_PAYLOAD_FRONTEND_WEIGHTS, frontend_values,
        &encoder->frontend_builder_allocation
    );
    if (loaded != 1) return loaded;
    loaded = encoder_qnn_load_artifact(
        encoder, encoder->encoder_path, encoder->encoder_fallback,
        WHISPER_ARTIFACT_PAYLOAD_ENCODER_WEIGHTS, encoder_values,
        &encoder->encoder_builder_allocation
    );
    if (loaded != 1) return loaded;
    if (!whisper_model_size_multiply(
            encoder->model.width, encoder->model.width, &width_squared
        )) return -1;
    cursor = encoder->frontend_builder_allocation;
    encoder->conv1_weight = cursor;
    cursor += encoder->model.width * encoder->model.mel_bins * 3U;
    encoder->conv1_bias = cursor;
    cursor += encoder->model.width;
    encoder->conv2_weight = cursor;
    cursor += encoder->model.width * encoder->model.width * 3U;
    encoder->conv2_bias = cursor;
    cursor += encoder->model.width;
    encoder->positions = cursor;
    cursor = encoder->encoder_builder_allocation;
    for (layer = 0U; layer < encoder->model.encoder_layers; ++layer) {
        WhisperEncoderLayerWeights *weights = &encoder->layers[layer];
        weights->ln1_scale = cursor;
        cursor += encoder->model.width;
        weights->ln1_bias = cursor;
        cursor += encoder->model.width;
        weights->projection_weight[0] = cursor;
        cursor += width_squared;
        weights->projection_bias[0] = cursor;
        cursor += encoder->model.width;
        weights->projection_weight[1] = cursor;
        cursor += width_squared;
        weights->projection_bias[1] = cursor;
        cursor += encoder->model.width;
        weights->projection_weight[2] = cursor;
        cursor += width_squared;
        weights->projection_bias[2] = cursor;
        cursor += encoder->model.width;
        weights->out_weight = cursor;
        cursor += width_squared;
        weights->out_bias = cursor;
        cursor += encoder->model.width;
        weights->ln2_scale = cursor;
        cursor += encoder->model.width;
        weights->ln2_bias = cursor;
        cursor += encoder->model.width;
        weights->fc1_weight = cursor;
        cursor += (u64)encoder->model.ffn_width * encoder->model.width;
        weights->fc1_bias = cursor;
        cursor += encoder->model.ffn_width;
        weights->fc2_weight = cursor;
        cursor += (u64)encoder->model.width * encoder->model.ffn_width;
        weights->fc2_bias = cursor;
        cursor += encoder->model.width;
    }
    return cursor == (u16 *)encoder->encoder_builder_allocation + encoder_values ? 1 : -1;
}

static int encoder_qnn_build_frontend(
    WhisperEncoderQnn *encoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    u64 matrix_bytes;
    u64 width_bytes;
    u64 position_bytes;
    u32 graph_index;
    if (!whisper_model_size_multiply(
            encoder->model.width, sizeof(u16), &width_bytes
        ) || !whisper_model_size_multiply(
            encoder->model.encoder_frames, encoder->model.width, &position_bytes
        ) || !whisper_model_size_multiply(position_bytes, sizeof(u16), &position_bytes)) {
        return 0;
    }
    for (graph_index = 0U; graph_index < 2U; ++graph_index) {
        QnnTensor weight;
        QnnTensor bias;
        QnnTensor fc_output;
        QnnTensor gelu_output;
        QnnTensor position;
        QnnTensor output;
        QnnTensor *registered[7];
        u32 registered_count = 0U;
        u32 index;
        u64 status = api->graph_create(
            context, encoder->frontend_graph_names[graph_index], 0,
            &encoder->frontend_graphs[graph_index]
        );
        if (status != 0U) return 0;
        encoder->frontend_inputs[graph_index] = encoder_qnn_tensor(
            encoder->frontend_names[graph_index][0], QNN_TENSOR_TYPE_APP_WRITE,
            QNN_DATATYPE_FLOAT_16, encoder->frontend_input_dimensions[graph_index], 2U
        );
        weight = encoder_qnn_tensor(
            encoder->frontend_names[graph_index][1], QNN_TENSOR_TYPE_STATIC,
            QNN_DATATYPE_FLOAT_16, encoder->frontend_weight_dimensions[graph_index], 2U
        );
        matrix_bytes = (u64)encoder->frontend_weight_dimensions[graph_index][0] *
            encoder->frontend_weight_dimensions[graph_index][1] * sizeof(u16);
        weight.data.v1.memory.client_buffer.data = graph_index == 0U
            ? (void *)encoder->conv1_weight : (void *)encoder->conv2_weight;
        weight.data.v1.memory.client_buffer.data_size = (u32)matrix_bytes;
        bias = encoder_qnn_tensor(
            encoder->frontend_names[graph_index][2], QNN_TENSOR_TYPE_STATIC,
            QNN_DATATYPE_FLOAT_16, encoder->width_dimensions, 1U
        );
        bias.data.v1.memory.client_buffer.data = graph_index == 0U
            ? (void *)encoder->conv1_bias : (void *)encoder->conv2_bias;
        bias.data.v1.memory.client_buffer.data_size = (u32)width_bytes;
        fc_output = encoder_qnn_tensor(
            encoder->frontend_names[graph_index][3], QNN_TENSOR_TYPE_NATIVE,
            QNN_DATATYPE_FLOAT_16, encoder->frontend_output_dimensions[graph_index], 2U
        );
        gelu_output = encoder_qnn_tensor(
            encoder->frontend_names[graph_index][4],
            graph_index == 0U ? QNN_TENSOR_TYPE_APP_READ : QNN_TENSOR_TYPE_NATIVE,
            QNN_DATATYPE_FLOAT_16, encoder->frontend_output_dimensions[graph_index], 2U
        );
        registered[registered_count++] = &encoder->frontend_inputs[graph_index];
        registered[registered_count++] = &weight;
        registered[registered_count++] = &bias;
        registered[registered_count++] = &fc_output;
        registered[registered_count++] = &gelu_output;
        if (graph_index == 1U) {
            position = encoder_qnn_tensor(
                encoder->frontend_names[graph_index][5], QNN_TENSOR_TYPE_STATIC,
                QNN_DATATYPE_FLOAT_16, encoder->frontend_output_dimensions[graph_index], 2U
            );
            position.data.v1.memory.client_buffer.data = encoder->positions;
            position.data.v1.memory.client_buffer.data_size = (u32)position_bytes;
            output = encoder_qnn_tensor(
                encoder->frontend_names[graph_index][6], QNN_TENSOR_TYPE_APP_READ,
                QNN_DATATYPE_FLOAT_16, encoder->frontend_output_dimensions[graph_index], 2U
            );
            registered[registered_count++] = &position;
            registered[registered_count++] = &output;
        }
        for (index = 0U; index < registered_count; ++index) {
            if (api->tensor_create_graph_tensor(
                    encoder->frontend_graphs[graph_index], registered[index]) != 0U) return 0;
        }
        if (encoder_qnn_add_node(
                api, encoder->frontend_graphs[graph_index],
                encoder->frontend_names[graph_index][3], "FullyConnected",
                &encoder->frontend_inputs[graph_index], &weight, &bias, 3U,
                &fc_output, 0, 0U
            ) != 0U || encoder_qnn_add_node(
                api, encoder->frontend_graphs[graph_index],
                encoder->frontend_names[graph_index][4], "Gelu",
                &fc_output, 0, 0, 1U, &gelu_output, 0, 0U
            ) != 0U) return 0;
        if (graph_index == 1U) {
            if (encoder_qnn_add_node(
                    api, encoder->frontend_graphs[graph_index],
                    encoder->frontend_names[graph_index][6], "ElementWiseAdd",
                    &gelu_output, &position, 0, 2U, &output, 0, 0U
                ) != 0U) return 0;
            encoder->frontend_outputs[graph_index] = output;
        } else {
            encoder->frontend_outputs[graph_index] = gelu_output;
        }
        if (api->graph_finalize(encoder->frontend_graphs[graph_index], 0, 0) != 0U) return 0;
    }
    return 1;
}

static int encoder_qnn_build_layer(
    WhisperEncoderQnn *encoder,
    const QnnInterfaceV2 *api,
    u32 layer,
    QnnTensor *previous_output
) {
    QnnTensor *tensors = encoder->block_tensors;
    QnnTensor **registered = encoder->registered_tensors;
    QnnParam (*norm_parameters)[2] = encoder->norm_parameters;
    QnnParam *transpose_parameters = encoder->transpose_parameters;
    WhisperEncoderLayerWeights *weights = &encoder->layers[layer];
    u64 matrix_bytes = (u64)encoder->model.width * encoder->model.width * sizeof(u16);
    u64 width_bytes = (u64)encoder->model.width * sizeof(u16);
    u64 fc_bytes = (u64)encoder->model.width * encoder->model.ffn_width * sizeof(u16);
    u64 hidden_bytes = (u64)encoder->model.ffn_width * sizeof(u16);
    u32 registered_count = 0U;
    u32 projection;
    u32 index;
    u32 node = 0U;
    u64 status;
    tensors[BLOCK_INPUT] = layer == 0U
        ? encoder_qnn_tensor(
            encoder->tensor_names[layer][BLOCK_INPUT], QNN_TENSOR_TYPE_APP_WRITE,
            QNN_DATATYPE_FLOAT_16, encoder->activation_dimensions, 2U)
        : *previous_output;
    tensors[BLOCK_LN1_SCALE] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_LN1_SCALE], QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, encoder->width_dimensions, 1U);
    tensors[BLOCK_LN1_SCALE].data.v1.memory.client_buffer.data = weights->ln1_scale;
    tensors[BLOCK_LN1_SCALE].data.v1.memory.client_buffer.data_size = (u32)width_bytes;
    tensors[BLOCK_LN1_BIAS] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_LN1_BIAS], QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, encoder->width_dimensions, 1U);
    tensors[BLOCK_LN1_BIAS].data.v1.memory.client_buffer.data = weights->ln1_bias;
    tensors[BLOCK_LN1_BIAS].data.v1.memory.client_buffer.data_size = (u32)width_bytes;
    tensors[BLOCK_LN1_OUTPUT] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_LN1_OUTPUT], QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->activation_dimensions, 2U);
    for (projection = 0U; projection < ENCODER_QNN_PROJECTIONS; ++projection) {
        u32 base = BLOCK_PROJECTION_BASE + projection * 5U;
        BLOCK_WEIGHT(tensors, projection) = encoder_qnn_tensor(
            encoder->tensor_names[layer][base], QNN_TENSOR_TYPE_STATIC,
            QNN_DATATYPE_FLOAT_16, encoder->matrix_dimensions, 2U);
        BLOCK_WEIGHT(tensors, projection).data.v1.memory.client_buffer.data =
            weights->projection_weight[projection];
        BLOCK_WEIGHT(tensors, projection).data.v1.memory.client_buffer.data_size =
            (u32)matrix_bytes;
        BLOCK_BIAS(tensors, projection) = encoder_qnn_tensor(
            encoder->tensor_names[layer][base + 1U], QNN_TENSOR_TYPE_STATIC,
            QNN_DATATYPE_FLOAT_16, encoder->width_dimensions, 1U);
        BLOCK_BIAS(tensors, projection).data.v1.memory.client_buffer.data =
            weights->projection_bias[projection];
        BLOCK_BIAS(tensors, projection).data.v1.memory.client_buffer.data_size =
            (u32)width_bytes;
        BLOCK_PROJECTION(tensors, projection) = encoder_qnn_tensor(
            encoder->tensor_names[layer][base + 2U], QNN_TENSOR_TYPE_NATIVE,
            QNN_DATATYPE_FLOAT_16, encoder->activation_dimensions, 2U);
        BLOCK_SPLIT(tensors, projection) = encoder_qnn_tensor(
            encoder->tensor_names[layer][base + 3U], QNN_TENSOR_TYPE_NATIVE,
            QNN_DATATYPE_FLOAT_16, encoder->split_dimensions, 3U);
        BLOCK_HEADS(tensors, projection) = encoder_qnn_tensor(
            encoder->tensor_names[layer][base + 4U], QNN_TENSOR_TYPE_NATIVE,
            QNN_DATATYPE_FLOAT_16,
            projection == 1U ? encoder->key_dimensions : encoder->head_dimensions, 3U);
    }
    tensors[BLOCK_SCORES] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_SCORES], QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->score_dimensions, 3U);
    tensors[BLOCK_PROBABILITIES] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_PROBABILITIES], QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->score_dimensions, 3U);
    tensors[BLOCK_ATTENDED_HEADS] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_ATTENDED_HEADS], QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->head_dimensions, 3U);
    tensors[BLOCK_ATTENDED_SPLIT] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_ATTENDED_SPLIT], QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->split_dimensions, 3U);
    tensors[BLOCK_ATTENDED_FLAT] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_ATTENDED_FLAT], QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->activation_dimensions, 2U);
    tensors[BLOCK_OUT_WEIGHT] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_OUT_WEIGHT], QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, encoder->matrix_dimensions, 2U);
    tensors[BLOCK_OUT_WEIGHT].data.v1.memory.client_buffer.data = weights->out_weight;
    tensors[BLOCK_OUT_WEIGHT].data.v1.memory.client_buffer.data_size = (u32)matrix_bytes;
    tensors[BLOCK_OUT_BIAS] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_OUT_BIAS], QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, encoder->width_dimensions, 1U);
    tensors[BLOCK_OUT_BIAS].data.v1.memory.client_buffer.data = weights->out_bias;
    tensors[BLOCK_OUT_BIAS].data.v1.memory.client_buffer.data_size = (u32)width_bytes;
    tensors[BLOCK_ATTENTION_OUTPUT] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_ATTENTION_OUTPUT], QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->activation_dimensions, 2U);
    tensors[BLOCK_ATTENTION_RESIDUAL] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_ATTENTION_RESIDUAL], QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->activation_dimensions, 2U);
    tensors[BLOCK_LN2_SCALE] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_LN2_SCALE], QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, encoder->width_dimensions, 1U);
    tensors[BLOCK_LN2_SCALE].data.v1.memory.client_buffer.data = weights->ln2_scale;
    tensors[BLOCK_LN2_SCALE].data.v1.memory.client_buffer.data_size = (u32)width_bytes;
    tensors[BLOCK_LN2_BIAS] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_LN2_BIAS], QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, encoder->width_dimensions, 1U);
    tensors[BLOCK_LN2_BIAS].data.v1.memory.client_buffer.data = weights->ln2_bias;
    tensors[BLOCK_LN2_BIAS].data.v1.memory.client_buffer.data_size = (u32)width_bytes;
    tensors[BLOCK_LN2_OUTPUT] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_LN2_OUTPUT], QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->activation_dimensions, 2U);
    tensors[BLOCK_FC1_WEIGHT] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_FC1_WEIGHT], QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, encoder->fc1_dimensions, 2U);
    tensors[BLOCK_FC1_WEIGHT].data.v1.memory.client_buffer.data = weights->fc1_weight;
    tensors[BLOCK_FC1_WEIGHT].data.v1.memory.client_buffer.data_size = (u32)fc_bytes;
    tensors[BLOCK_FC1_BIAS] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_FC1_BIAS], QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, encoder->hidden_bias_dimensions, 1U);
    tensors[BLOCK_FC1_BIAS].data.v1.memory.client_buffer.data = weights->fc1_bias;
    tensors[BLOCK_FC1_BIAS].data.v1.memory.client_buffer.data_size = (u32)hidden_bytes;
    tensors[BLOCK_FC1_OUTPUT] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_FC1_OUTPUT], QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->hidden_dimensions, 2U);
    tensors[BLOCK_GELU_OUTPUT] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_GELU_OUTPUT], QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->hidden_dimensions, 2U);
    tensors[BLOCK_FC2_WEIGHT] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_FC2_WEIGHT], QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, encoder->fc2_dimensions, 2U);
    tensors[BLOCK_FC2_WEIGHT].data.v1.memory.client_buffer.data = weights->fc2_weight;
    tensors[BLOCK_FC2_WEIGHT].data.v1.memory.client_buffer.data_size = (u32)fc_bytes;
    tensors[BLOCK_FC2_BIAS] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_FC2_BIAS], QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, encoder->width_dimensions, 1U);
    tensors[BLOCK_FC2_BIAS].data.v1.memory.client_buffer.data = weights->fc2_bias;
    tensors[BLOCK_FC2_BIAS].data.v1.memory.client_buffer.data_size = (u32)width_bytes;
    tensors[BLOCK_FC2_OUTPUT] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_FC2_OUTPUT], QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->activation_dimensions, 2U);
    tensors[BLOCK_OUTPUT] = encoder_qnn_tensor(
        encoder->tensor_names[layer][BLOCK_OUTPUT],
        layer + 1U == encoder->model.encoder_layers
            ? QNN_TENSOR_TYPE_APP_READ : QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, encoder->activation_dimensions, 2U);
    for (index = layer == 0U ? 0U : 1U;
         index < ENCODER_QNN_BLOCK_TENSORS; ++index) {
        registered[registered_count++] = &tensors[index];
    }
    for (index = 0U; index < 2U; ++index) {
        norm_parameters[index][0].type = QNN_PARAMTYPE_SCALAR;
        norm_parameters[index][0].name = "epsilon";
        norm_parameters[index][0].value.scalar.data_type = QNN_DATATYPE_FLOAT_32;
        norm_parameters[index][0].value.scalar.value.float_value = 0.00001f;
        norm_parameters[index][1].type = QNN_PARAMTYPE_TENSOR;
        norm_parameters[index][1].name = "axes";
        norm_parameters[index][1].value.tensor = encoder_qnn_tensor(
            encoder->tensor_names[layer][ENCODER_QNN_BLOCK_TENSORS + index],
            QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_UINT_32,
            encoder->vector_dimensions, 1U);
        norm_parameters[index][1].value.tensor.data.v1.memory.client_buffer.data =
            encoder->axes;
        norm_parameters[index][1].value.tensor.data.v1.memory.client_buffer.data_size =
            sizeof(encoder->axes);
        registered[registered_count++] = &norm_parameters[index][1].value.tensor;
    }
    for (index = 0U; index < 2U; ++index) {
        transpose_parameters[index].type = QNN_PARAMTYPE_TENSOR;
        transpose_parameters[index].name = "perm";
        transpose_parameters[index].value.tensor = encoder_qnn_tensor(
            encoder->tensor_names[layer][ENCODER_QNN_BLOCK_TENSORS + 2U + index],
            QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_UINT_32,
            encoder->perm_dimensions, 1U);
        transpose_parameters[index].value.tensor.data.v1.memory.client_buffer.data =
            index == 0U ? (void *)encoder->head_perm : (void *)encoder->key_perm;
        transpose_parameters[index].value.tensor.data.v1.memory.client_buffer.data_size =
            sizeof(encoder->head_perm);
        registered[registered_count++] = &transpose_parameters[index].value.tensor;
    }
    for (index = 0U; index < registered_count; ++index) {
        if (api->tensor_create_graph_tensor(encoder->encoder_graph, registered[index]) != 0U) {
            return 0;
        }
    }
#define ADD_NODE(type, input0, input1, input2, count, output, params, param_count) \
    status = encoder_qnn_add_node( \
        api, encoder->encoder_graph, encoder->node_names[layer][node++], type, \
        input0, input1, input2, count, output, params, param_count); \
    if (status != 0U) return 0
    ADD_NODE("LayerNorm", &tensors[BLOCK_INPUT], &tensors[BLOCK_LN1_SCALE],
        &tensors[BLOCK_LN1_BIAS], 3U, &tensors[BLOCK_LN1_OUTPUT], norm_parameters[0], 2U);
    for (projection = 0U; projection < ENCODER_QNN_PROJECTIONS; ++projection) {
        ADD_NODE("FullyConnected", &tensors[BLOCK_LN1_OUTPUT],
            &BLOCK_WEIGHT(tensors, projection), &BLOCK_BIAS(tensors, projection), 3U,
            &BLOCK_PROJECTION(tensors, projection), 0, 0U);
        ADD_NODE("Reshape", &BLOCK_PROJECTION(tensors, projection), 0, 0, 1U,
            &BLOCK_SPLIT(tensors, projection), 0, 0U);
        ADD_NODE("Transpose", &BLOCK_SPLIT(tensors, projection), 0, 0, 1U,
            &BLOCK_HEADS(tensors, projection),
            &transpose_parameters[projection == 1U ? 1U : 0U], 1U);
    }
    ADD_NODE("MatMul", &BLOCK_HEADS(tensors, 0), &BLOCK_HEADS(tensors, 1), 0, 2U,
        &tensors[BLOCK_SCORES], 0, 0U);
    ADD_NODE("Softmax", &tensors[BLOCK_SCORES], 0, 0, 1U,
        &tensors[BLOCK_PROBABILITIES], 0, 0U);
    ADD_NODE("MatMul", &tensors[BLOCK_PROBABILITIES], &BLOCK_HEADS(tensors, 2), 0, 2U,
        &tensors[BLOCK_ATTENDED_HEADS], 0, 0U);
    ADD_NODE("Transpose", &tensors[BLOCK_ATTENDED_HEADS], 0, 0, 1U,
        &tensors[BLOCK_ATTENDED_SPLIT], &transpose_parameters[0], 1U);
    ADD_NODE("Reshape", &tensors[BLOCK_ATTENDED_SPLIT], 0, 0, 1U,
        &tensors[BLOCK_ATTENDED_FLAT], 0, 0U);
    ADD_NODE("FullyConnected", &tensors[BLOCK_ATTENDED_FLAT],
        &tensors[BLOCK_OUT_WEIGHT], &tensors[BLOCK_OUT_BIAS], 3U,
        &tensors[BLOCK_ATTENTION_OUTPUT], 0, 0U);
    ADD_NODE("ElementWiseAdd", &tensors[BLOCK_INPUT],
        &tensors[BLOCK_ATTENTION_OUTPUT], 0, 2U,
        &tensors[BLOCK_ATTENTION_RESIDUAL], 0, 0U);
    ADD_NODE("LayerNorm", &tensors[BLOCK_ATTENTION_RESIDUAL],
        &tensors[BLOCK_LN2_SCALE], &tensors[BLOCK_LN2_BIAS], 3U,
        &tensors[BLOCK_LN2_OUTPUT], norm_parameters[1], 2U);
    ADD_NODE("FullyConnected", &tensors[BLOCK_LN2_OUTPUT],
        &tensors[BLOCK_FC1_WEIGHT], &tensors[BLOCK_FC1_BIAS], 3U,
        &tensors[BLOCK_FC1_OUTPUT], 0, 0U);
    ADD_NODE("Gelu", &tensors[BLOCK_FC1_OUTPUT], 0, 0, 1U,
        &tensors[BLOCK_GELU_OUTPUT], 0, 0U);
    ADD_NODE("FullyConnected", &tensors[BLOCK_GELU_OUTPUT],
        &tensors[BLOCK_FC2_WEIGHT], &tensors[BLOCK_FC2_BIAS], 3U,
        &tensors[BLOCK_FC2_OUTPUT], 0, 0U);
    ADD_NODE("ElementWiseAdd", &tensors[BLOCK_ATTENTION_RESIDUAL],
        &tensors[BLOCK_FC2_OUTPUT], 0, 2U, &tensors[BLOCK_OUTPUT], 0, 0U);
#undef ADD_NODE
    if (node != ENCODER_QNN_NODES_PER_LAYER) return 0;
    if (layer == 0U) encoder->encoder_input = tensors[BLOCK_INPUT];
    *previous_output = tensors[BLOCK_OUTPUT];
    if (layer + 1U == encoder->model.encoder_layers) {
        encoder->encoder_output_tensor = tensors[BLOCK_OUTPUT];
    }
    return 1;
}

static int encoder_qnn_build_encoder(
    WhisperEncoderQnn *encoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    QnnTensor previous_output;
    u32 layer;
    if (api->graph_create(
            context, encoder->encoder_graph_name, 0, &encoder->encoder_graph
        ) != 0U) return 0;
    for (layer = 0U; layer < encoder->model.encoder_layers; ++layer) {
        if (!encoder_qnn_build_layer(encoder, api, layer, &previous_output)) return 0;
    }
    return api->graph_finalize(encoder->encoder_graph, 0, 0) == 0U;
}

int whisper_encoder_qnn_build(
    WhisperEncoderQnn *encoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    WhisperEncoderQnnIds *ids_out
) {
    int loaded;
    if (encoder == 0 || api == 0 || encoder->frontend_builder_allocation != 0 ||
        encoder->encoder_builder_allocation != 0) return -1;
    loaded = encoder_qnn_load_weights(encoder);
    if (loaded != 1) return loaded;
    if (!encoder_qnn_build_frontend(encoder, api, context) ||
        !encoder_qnn_build_encoder(encoder, api, context)) return -1;
    if (ids_out != 0) {
        ids_out->model_id = encoder->model.model_id;
        ids_out->frontend_input_ids[0] = encoder->frontend_inputs[0].data.v1.id;
        ids_out->frontend_input_ids[1] = encoder->frontend_inputs[1].data.v1.id;
        ids_out->frontend_output_ids[0] = encoder->frontend_outputs[0].data.v1.id;
        ids_out->frontend_output_ids[1] = encoder->frontend_outputs[1].data.v1.id;
        ids_out->encoder_input_id = encoder->encoder_input.data.v1.id;
        ids_out->encoder_output_id = encoder->encoder_output_tensor.data.v1.id;
    }
    return 1;
}

int whisper_encoder_qnn_restore(
    WhisperEncoderQnn *encoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    const WhisperEncoderQnnIds *ids
) {
    u32 index;
    if (encoder == 0 || api == 0 || ids == 0 ||
        ids->model_id != encoder->model.model_id ||
        api->graph_retrieve(
            context, encoder->encoder_graph_name, &encoder->encoder_graph
        ) != 0U) return 0;
    for (index = 0U; index < 2U; ++index) {
        if (api->graph_retrieve(
                context, encoder->frontend_graph_names[index],
                &encoder->frontend_graphs[index]
            ) != 0U) return 0;
        encoder->frontend_inputs[index] = encoder_qnn_tensor(
            encoder->frontend_names[index][0], QNN_TENSOR_TYPE_APP_WRITE,
            QNN_DATATYPE_FLOAT_16, encoder->frontend_input_dimensions[index], 2U
        );
        encoder->frontend_outputs[index] = encoder_qnn_tensor(
            encoder->frontend_names[index][index == 0U ? 4U : 6U],
            QNN_TENSOR_TYPE_APP_READ, QNN_DATATYPE_FLOAT_16,
            encoder->frontend_output_dimensions[index], 2U
        );
        encoder->frontend_inputs[index].data.v1.id = ids->frontend_input_ids[index];
        encoder->frontend_outputs[index].data.v1.id = ids->frontend_output_ids[index];
    }
    encoder->encoder_input = encoder_qnn_tensor(
        encoder->tensor_names[0][BLOCK_INPUT], QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, encoder->activation_dimensions, 2U
    );
    encoder->encoder_output_tensor = encoder_qnn_tensor(
        encoder->tensor_names[encoder->model.encoder_layers - 1U][BLOCK_OUTPUT],
        QNN_TENSOR_TYPE_APP_READ, QNN_DATATYPE_FLOAT_16,
        encoder->activation_dimensions, 2U
    );
    encoder->encoder_input.data.v1.id = ids->encoder_input_id;
    encoder->encoder_output_tensor.data.v1.id = ids->encoder_output_id;
    whisper_encoder_qnn_release_builder(encoder);
    return 1;
}

u64 whisper_encoder_qnn_execute_frontend(
    WhisperEncoderQnn *encoder,
    const QnnInterfaceV2 *api,
    const float *log_mel
) {
    QnnTensor input;
    QnnTensor output;
    u64 status;
    u64 conv1_input_bytes;
    u64 conv1_output_bytes;
    u64 conv2_input_bytes;
    if (encoder == 0 || api == 0 || log_mel == 0) return ~0ULL;
    whisper_frontend_pack_conv1(log_mel, encoder->conv1_input);
    conv1_input_bytes = (u64)WHISPER_FRAME_COUNT * encoder->model.mel_bins * 3U * sizeof(u16);
    conv1_output_bytes = (u64)WHISPER_FRAME_COUNT * encoder->model.width * sizeof(u16);
    input = encoder->frontend_inputs[0];
    output = encoder->frontend_outputs[0];
    input.data.v1.memory.client_buffer.data = encoder->conv1_input;
    input.data.v1.memory.client_buffer.data_size = (u32)conv1_input_bytes;
    output.data.v1.memory.client_buffer.data = encoder->conv1_output;
    output.data.v1.memory.client_buffer.data_size = (u32)conv1_output_bytes;
    status = api->graph_execute(
        encoder->frontend_graphs[0], &input, 1U, &output, 1U, 0, 0
    );
    if (status != 0U) return status;
    whisper_frontend_pack_conv2_width(
        encoder->conv1_output, encoder->conv2_input, encoder->model.width
    );
    conv2_input_bytes = encoder->activation_bytes * 3U;
    input = encoder->frontend_inputs[1];
    output = encoder->frontend_outputs[1];
    input.data.v1.memory.client_buffer.data = encoder->conv2_input;
    input.data.v1.memory.client_buffer.data_size = (u32)conv2_input_bytes;
    output.data.v1.memory.client_buffer.data = encoder->frontend_output;
    output.data.v1.memory.client_buffer.data_size = (u32)encoder->activation_bytes;
    return api->graph_execute(
        encoder->frontend_graphs[1], &input, 1U, &output, 1U, 0, 0
    );
}

u64 whisper_encoder_qnn_execute_encoder(
    WhisperEncoderQnn *encoder,
    const QnnInterfaceV2 *api
) {
    QnnTensor input;
    QnnTensor output;
    if (encoder == 0 || api == 0 || encoder->encoder_graph == 0) return ~0ULL;
    input = encoder->encoder_input;
    output = encoder->encoder_output_tensor;
    input.data.v1.memory.client_buffer.data = encoder->frontend_output;
    input.data.v1.memory.client_buffer.data_size = (u32)encoder->activation_bytes;
    output.data.v1.memory.client_buffer.data = encoder->encoder_output;
    output.data.v1.memory.client_buffer.data_size = (u32)encoder->activation_bytes;
    return api->graph_execute(encoder->encoder_graph, &input, 1U, &output, 1U, 0, 0);
}

const u16 *whisper_encoder_qnn_frontend_output(const WhisperEncoderQnn *encoder) {
    return encoder == 0 ? 0 : encoder->frontend_output;
}

const u16 *whisper_encoder_qnn_output(const WhisperEncoderQnn *encoder) {
    return encoder == 0 ? 0 : encoder->encoder_output;
}

u64 whisper_encoder_qnn_activation_bytes(const WhisperEncoderQnn *encoder) {
    return encoder == 0 ? 0U : encoder->activation_bytes;
}

void whisper_encoder_qnn_release_builder(WhisperEncoderQnn *encoder) {
    if (encoder == 0) return;
    if (encoder->frontend_builder_allocation != 0) {
        VirtualFree(encoder->frontend_builder_allocation, 0U, 0x8000U);
        encoder->frontend_builder_allocation = 0;
    }
    if (encoder->encoder_builder_allocation != 0) {
        VirtualFree(encoder->encoder_builder_allocation, 0U, 0x8000U);
        encoder->encoder_builder_allocation = 0;
    }
}

void whisper_encoder_qnn_shutdown(WhisperEncoderQnn *encoder) {
    if (encoder == 0) return;
    whisper_encoder_qnn_release_builder(encoder);
    if (encoder->runtime_allocation != 0) {
        VirtualFree(encoder->runtime_allocation, 0U, 0x8000U);
    }
    VirtualFree(encoder, 0U, 0x8000U);
}

#undef BLOCK_WEIGHT
#undef BLOCK_BIAS
#undef BLOCK_PROJECTION
#undef BLOCK_SPLIT
#undef BLOCK_HEADS