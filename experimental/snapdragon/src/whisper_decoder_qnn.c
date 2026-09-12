#include "whisper_decoder_qnn.h"

typedef unsigned long long usize;

enum {
    DECODER_QNN_LAYERS = 4,
    DECODER_QNN_WIDTH = 384,
    DECODER_QNN_FRAMES = 1500,
    DECODER_QNN_PROJECTIONS = 8,
    DECODER_QNN_WEIGHT_VALUES = 1181952
};

#define DECODER_QNN_WEIGHT_MAGIC 0x31564b5143454457ULL

__declspec(dllimport) int CloseHandle(void *handle);
__declspec(dllimport) void *CreateFileA(
    const char *name, u32 access, u32 sharing, void *security,
    u32 creation, u32 attributes, void *template_file
);
__declspec(dllimport) int ReadFile(
    void *handle, void *buffer, u32 size, u32 *read, void *overlapped
);

static QnnGraphHandle decoder_qnn_graph;
static QnnTensor decoder_qnn_input;
static QnnTensor decoder_qnn_outputs[DECODER_QNN_PROJECTIONS];
static u32 decoder_qnn_activation_dimensions[2] = {
    DECODER_QNN_FRAMES, DECODER_QNN_WIDTH
};
static u32 decoder_qnn_weight_dimensions[2] = {
    DECODER_QNN_WIDTH, DECODER_QNN_WIDTH
};
static u32 decoder_qnn_width_dimensions[1] = {DECODER_QNN_WIDTH};
static u32 decoder_qnn_vector_dimensions[1] = {1U};
static u32 decoder_qnn_axes[1] = {1U};
static u16 decoder_qnn_norm_weight[DECODER_QNN_WIDTH];
static u16 decoder_qnn_norm_bias[DECODER_QNN_WIDTH];
static u16 decoder_qnn_weights[DECODER_QNN_PROJECTIONS]
    [DECODER_QNN_WIDTH * DECODER_QNN_WIDTH];
static u16 decoder_qnn_biases[DECODER_QNN_PROJECTIONS][DECODER_QNN_WIDTH];
static u16 decoder_qnn_keys_cache[DECODER_QNN_LAYERS]
    [DECODER_QNN_FRAMES * DECODER_QNN_WIDTH];
static u16 decoder_qnn_values_cache[DECODER_QNN_LAYERS]
    [DECODER_QNN_FRAMES * DECODER_QNN_WIDTH];
static QnnTensor decoder_qnn_norm_weight_tensor;
static QnnTensor decoder_qnn_norm_bias_tensor;
static QnnTensor decoder_qnn_norm_output_tensor;
static QnnTensor decoder_qnn_projection_weights[DECODER_QNN_PROJECTIONS];
static QnnTensor decoder_qnn_projection_biases[DECODER_QNN_PROJECTIONS];
static QnnParam decoder_qnn_norm_parameters[2];
static QnnTensor *decoder_qnn_registered[5U + DECODER_QNN_PROJECTIONS * 3U];
static QnnTensor decoder_qnn_execute_outputs[DECODER_QNN_PROJECTIONS];

static u32 decoder_qnn_read_u32(const u8 *bytes) {
    return (u32)bytes[0] | ((u32)bytes[1] << 8U) |
        ((u32)bytes[2] << 16U) | ((u32)bytes[3] << 24U);
}

static u64 decoder_qnn_read_u64(const u8 *bytes) {
    return (u64)decoder_qnn_read_u32(bytes) |
        ((u64)decoder_qnn_read_u32(bytes + 4U) << 32U);
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

static int decoder_qnn_load_weights(void) {
    static const char primary[] =
        "experimental/snapdragon/models/whisper-tiny/decoder-f32/cross-kv-fp16.bin";
    static const char fallback[] =
        "../models/whisper-tiny/decoder-f32/cross-kv-fp16.bin";
    void *invalid = (void *)(usize)-1;
    void *handle = CreateFileA(primary, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    u8 header[16];
    u32 layer;
    if (handle == invalid) {
        handle = CreateFileA(fallback, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    }
    if (handle == invalid) return 0;
    if (!decoder_qnn_read_exact(handle, header, sizeof(header)) ||
        decoder_qnn_read_u64(header) != DECODER_QNN_WEIGHT_MAGIC ||
        decoder_qnn_read_u32(header + 8U) != 1U ||
        decoder_qnn_read_u32(header + 12U) != DECODER_QNN_WEIGHT_VALUES ||
        !decoder_qnn_read_exact(
            handle, decoder_qnn_norm_weight, sizeof(decoder_qnn_norm_weight)
        ) || !decoder_qnn_read_exact(
            handle, decoder_qnn_norm_bias, sizeof(decoder_qnn_norm_bias)
        )) {
        CloseHandle(handle);
        return -1;
    }
    for (layer = 0U; layer < DECODER_QNN_LAYERS; ++layer) {
        u32 key_index = layer * 2U;
        u32 value_index = key_index + 1U;
        if (!decoder_qnn_read_exact(
                handle, decoder_qnn_weights[key_index],
                sizeof(decoder_qnn_weights[key_index])
            ) || !decoder_qnn_read_exact(
                handle, decoder_qnn_weights[value_index],
                sizeof(decoder_qnn_weights[value_index])
            ) || !decoder_qnn_read_exact(
                handle, decoder_qnn_biases[value_index],
                sizeof(decoder_qnn_biases[value_index])
            )) {
            CloseHandle(handle);
            return -1;
        }
    }
    CloseHandle(handle);
    return 1;
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

static u64 decoder_qnn_add_node(
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
    return api->graph_add_node(decoder_qnn_graph, operation);
}

int whisper_decoder_qnn_build(
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    WhisperDecoderQnnIds *ids_out
) {
    static const char *weight_names[DECODER_QNN_PROJECTIONS] = {
        "decoder_l0_cross_k_weight", "decoder_l0_cross_v_weight",
        "decoder_l1_cross_k_weight", "decoder_l1_cross_v_weight",
        "decoder_l2_cross_k_weight", "decoder_l2_cross_v_weight",
        "decoder_l3_cross_k_weight", "decoder_l3_cross_v_weight"
    };
    static const char *bias_names[DECODER_QNN_PROJECTIONS] = {
        "decoder_l0_cross_k_bias", "decoder_l0_cross_v_bias",
        "decoder_l1_cross_k_bias", "decoder_l1_cross_v_bias",
        "decoder_l2_cross_k_bias", "decoder_l2_cross_v_bias",
        "decoder_l3_cross_k_bias", "decoder_l3_cross_v_bias"
    };
    static const char *output_names[DECODER_QNN_PROJECTIONS] = {
        "decoder_l0_cross_k", "decoder_l0_cross_v",
        "decoder_l1_cross_k", "decoder_l1_cross_v",
        "decoder_l2_cross_k", "decoder_l2_cross_v",
        "decoder_l3_cross_k", "decoder_l3_cross_v"
    };
    static const char *node_names[DECODER_QNN_PROJECTIONS] = {
        "decoder_l0_cross_k_fc", "decoder_l0_cross_v_fc",
        "decoder_l1_cross_k_fc", "decoder_l1_cross_v_fc",
        "decoder_l2_cross_k_fc", "decoder_l2_cross_v_fc",
        "decoder_l3_cross_k_fc", "decoder_l3_cross_v_fc"
    };
    u32 registered_count = 0U;
    u32 index;
    u64 status;
    int loaded = decoder_qnn_load_weights();
    if (loaded != 1) return loaded;
    status = api->graph_create(
        context, "whisper_decoder_cross_kv_fp16", 0, &decoder_qnn_graph
    );
    if (status != 0U) return -1;
    decoder_qnn_input = decoder_qnn_tensor(
        "decoder_cross_input", QNN_TENSOR_TYPE_APP_WRITE,
        decoder_qnn_activation_dimensions, 2U
    );
    decoder_qnn_norm_weight_tensor = decoder_qnn_tensor(
        "decoder_cross_norm_weight", QNN_TENSOR_TYPE_STATIC,
        decoder_qnn_width_dimensions, 1U
    );
    decoder_qnn_norm_weight_tensor.data.v1.memory.client_buffer.data = decoder_qnn_norm_weight;
    decoder_qnn_norm_weight_tensor.data.v1.memory.client_buffer.data_size =
        sizeof(decoder_qnn_norm_weight);
    decoder_qnn_norm_bias_tensor = decoder_qnn_tensor(
        "decoder_cross_norm_bias", QNN_TENSOR_TYPE_STATIC,
        decoder_qnn_width_dimensions, 1U
    );
    decoder_qnn_norm_bias_tensor.data.v1.memory.client_buffer.data = decoder_qnn_norm_bias;
    decoder_qnn_norm_bias_tensor.data.v1.memory.client_buffer.data_size =
        sizeof(decoder_qnn_norm_bias);
    decoder_qnn_norm_output_tensor = decoder_qnn_tensor(
        "decoder_cross_norm_output", QNN_TENSOR_TYPE_NATIVE,
        decoder_qnn_activation_dimensions, 2U
    );
    decoder_qnn_norm_parameters[0].type = QNN_PARAMTYPE_SCALAR;
    decoder_qnn_norm_parameters[0].name = "epsilon";
    decoder_qnn_norm_parameters[0].value.scalar.data_type = QNN_DATATYPE_FLOAT_32;
    decoder_qnn_norm_parameters[0].value.scalar.value.float_value = 0.00001f;
    decoder_qnn_norm_parameters[1].type = QNN_PARAMTYPE_TENSOR;
    decoder_qnn_norm_parameters[1].name = "axes";
    decoder_qnn_norm_parameters[1].value.tensor = decoder_qnn_tensor(
        "decoder_cross_norm_axes", QNN_TENSOR_TYPE_STATIC,
        decoder_qnn_vector_dimensions, 1U
    );
    decoder_qnn_norm_parameters[1].value.tensor.data.v1.data_type = QNN_DATATYPE_UINT_32;
    decoder_qnn_norm_parameters[1].value.tensor.data.v1.memory.client_buffer.data =
        decoder_qnn_axes;
    decoder_qnn_norm_parameters[1].value.tensor.data.v1.memory.client_buffer.data_size =
        sizeof(decoder_qnn_axes);
    decoder_qnn_registered[registered_count++] = &decoder_qnn_input;
    decoder_qnn_registered[registered_count++] = &decoder_qnn_norm_weight_tensor;
    decoder_qnn_registered[registered_count++] = &decoder_qnn_norm_bias_tensor;
    decoder_qnn_registered[registered_count++] = &decoder_qnn_norm_output_tensor;
    decoder_qnn_registered[registered_count++] =
        &decoder_qnn_norm_parameters[1].value.tensor;
    for (index = 0U; index < DECODER_QNN_PROJECTIONS; ++index) {
        decoder_qnn_projection_weights[index] = decoder_qnn_tensor(
            weight_names[index], QNN_TENSOR_TYPE_STATIC,
            decoder_qnn_weight_dimensions, 2U
        );
        decoder_qnn_projection_weights[index].data.v1.memory.client_buffer.data =
            decoder_qnn_weights[index];
        decoder_qnn_projection_weights[index].data.v1.memory.client_buffer.data_size =
            sizeof(decoder_qnn_weights[index]);
        decoder_qnn_projection_biases[index] = decoder_qnn_tensor(
            bias_names[index], QNN_TENSOR_TYPE_STATIC,
            decoder_qnn_width_dimensions, 1U
        );
        decoder_qnn_projection_biases[index].data.v1.memory.client_buffer.data =
            decoder_qnn_biases[index];
        decoder_qnn_projection_biases[index].data.v1.memory.client_buffer.data_size =
            sizeof(decoder_qnn_biases[index]);
        decoder_qnn_outputs[index] = decoder_qnn_tensor(
            output_names[index], QNN_TENSOR_TYPE_APP_READ,
            decoder_qnn_activation_dimensions, 2U
        );
        decoder_qnn_registered[registered_count++] = &decoder_qnn_projection_weights[index];
        decoder_qnn_registered[registered_count++] = &decoder_qnn_projection_biases[index];
        decoder_qnn_registered[registered_count++] = &decoder_qnn_outputs[index];
    }
    for (index = 0U; index < registered_count; ++index) {
        if (api->tensor_create_graph_tensor(
                decoder_qnn_graph, decoder_qnn_registered[index]) != 0U) {
            return -1;
        }
    }
    status = decoder_qnn_add_node(
        api, "decoder_cross_norm", "LayerNorm", &decoder_qnn_input,
        &decoder_qnn_norm_weight_tensor, &decoder_qnn_norm_bias_tensor, 3U,
        &decoder_qnn_norm_output_tensor, decoder_qnn_norm_parameters, 2U
    );
    if (status != 0U) return -1;
    for (index = 0U; index < DECODER_QNN_PROJECTIONS; ++index) {
        status = decoder_qnn_add_node(
            api, node_names[index], "FullyConnected", &decoder_qnn_norm_output_tensor,
            &decoder_qnn_projection_weights[index],
            &decoder_qnn_projection_biases[index], 3U,
            &decoder_qnn_outputs[index], 0, 0U
        );
        if (status != 0U) return -1;
    }
    if (api->graph_finalize(decoder_qnn_graph, 0, 0) != 0U) return -1;
    if (ids_out != 0) {
        ids_out->input_id = decoder_qnn_input.data.v1.id;
        for (index = 0U; index < DECODER_QNN_PROJECTIONS; ++index) {
            ids_out->output_ids[index] = decoder_qnn_outputs[index].data.v1.id;
        }
    }
    return 1;
}

int whisper_decoder_qnn_restore(
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    const WhisperDecoderQnnIds *ids
) {
    static const char *output_names[DECODER_QNN_PROJECTIONS] = {
        "decoder_l0_cross_k", "decoder_l0_cross_v",
        "decoder_l1_cross_k", "decoder_l1_cross_v",
        "decoder_l2_cross_k", "decoder_l2_cross_v",
        "decoder_l3_cross_k", "decoder_l3_cross_v"
    };
    u32 index;
    if (ids == 0 || api->graph_retrieve(
            context, "whisper_decoder_cross_kv_fp16", &decoder_qnn_graph) != 0U) {
        return 0;
    }
    decoder_qnn_input = decoder_qnn_tensor(
        "decoder_cross_input", QNN_TENSOR_TYPE_APP_WRITE,
        decoder_qnn_activation_dimensions, 2U
    );
    decoder_qnn_input.data.v1.id = ids->input_id;
    for (index = 0U; index < DECODER_QNN_PROJECTIONS; ++index) {
        decoder_qnn_outputs[index] = decoder_qnn_tensor(
            output_names[index], QNN_TENSOR_TYPE_APP_READ,
            decoder_qnn_activation_dimensions, 2U
        );
        decoder_qnn_outputs[index].data.v1.id = ids->output_ids[index];
    }
    return 1;
}

u64 whisper_decoder_qnn_execute(const QnnInterfaceV2 *api, const u16 *encoder_output) {
    QnnTensor input = decoder_qnn_input;
    u32 index;
    input.data.v1.memory.client_buffer.data = (void *)encoder_output;
    input.data.v1.memory.client_buffer.data_size =
        DECODER_QNN_FRAMES * DECODER_QNN_WIDTH * sizeof(u16);
    for (index = 0U; index < DECODER_QNN_PROJECTIONS; ++index) {
        u32 layer = index / 2U;
        decoder_qnn_execute_outputs[index] = decoder_qnn_outputs[index];
        decoder_qnn_execute_outputs[index].data.v1.memory.client_buffer.data =
            (index & 1U) == 0U
            ? (void *)decoder_qnn_keys_cache[layer]
            : (void *)decoder_qnn_values_cache[layer];
        decoder_qnn_execute_outputs[index].data.v1.memory.client_buffer.data_size =
            DECODER_QNN_FRAMES * DECODER_QNN_WIDTH * sizeof(u16);
    }
    return api->graph_execute(
        decoder_qnn_graph, &input, 1U, decoder_qnn_execute_outputs,
        DECODER_QNN_PROJECTIONS, 0, 0
    );
}

const u16 *whisper_decoder_qnn_keys(void) {
    return &decoder_qnn_keys_cache[0][0];
}

const u16 *whisper_decoder_qnn_values(void) {
    return &decoder_qnn_values_cache[0][0];
}