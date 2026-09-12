#include "whisper_decoder_qnn.h"
#include "whisper_artifact.h"

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
    DECODER_QNN_NAME_CAPACITY = 56,
    DECODER_QNN_PATH_CAPACITY = 192
};

struct WhisperDecoderQnn {
    WhisperModelConfig model;
    u32 output_count;
    void *runtime_allocation;
    void *builder_allocation;
    u16 *keys_cache;
    u16 *values_cache;
    u16 *norm_weight;
    u16 *norm_bias;
    u16 *projection_weights[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    u16 *projection_biases[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnGraphHandle graph;
    QnnTensor input;
    QnnTensor outputs[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnTensor execute_outputs[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnTensor norm_weight_tensor;
    QnnTensor norm_bias_tensor;
    QnnTensor norm_output_tensor;
    QnnTensor projection_weight_tensors[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnTensor projection_bias_tensors[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    QnnParam norm_parameters[2];
    QnnTensor *registered[5U + WHISPER_DECODER_QNN_MAX_OUTPUTS * 3U];
    u32 activation_dimensions[2];
    u32 weight_dimensions[2];
    u32 width_dimensions[1];
    u32 vector_dimensions[1];
    u32 axes[1];
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
    char weight_path[DECODER_QNN_PATH_CAPACITY];
    char weight_fallback[DECODER_QNN_PATH_CAPACITY];
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

static int initialize_names(WhisperDecoderQnn *decoder) {
    u32 index;
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
        ) || !make_weight_path(
            decoder->weight_path, sizeof(decoder->weight_path),
            "experimental/snapdragon/models/whisper-", model
        ) || !make_weight_path(
            decoder->weight_fallback, sizeof(decoder->weight_fallback),
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
            )) return 0;
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
    return cursor == (u16 *)((u8 *)decoder->builder_allocation + expected_payload_size)
        ? 1 : -1;
}

WhisperDecoderQnn *whisper_decoder_qnn_create(const WhisperModelConfig *model) {
    WhisperDecoderQnn *decoder;
    u64 cache_values;
    u64 cache_bytes;
    u64 runtime_bytes;
    if (!whisper_model_config_valid(model) ||
        model->decoder_layers > WHISPER_DECODER_QNN_MAX_OUTPUTS / 2U ||
        !whisper_model_size_multiply(
            model->decoder_layers, model->encoder_frames, &cache_values
        ) || !whisper_model_size_multiply(
            cache_values, model->width, &cache_values
        ) || !whisper_model_size_multiply(
            cache_values, sizeof(u16), &cache_bytes
        ) || !whisper_model_size_multiply(2U, cache_bytes, &runtime_bytes)) {
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
    decoder->activation_dimensions[0] = model->encoder_frames;
    decoder->activation_dimensions[1] = model->width;
    decoder->weight_dimensions[0] = model->width;
    decoder->weight_dimensions[1] = model->width;
    decoder->width_dimensions[0] = model->width;
    decoder->vector_dimensions[0] = 1U;
    decoder->axes[0] = 1U;
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
        decoder->outputs[index] = decoder_qnn_tensor(
            decoder->output_names[index], QNN_TENSOR_TYPE_APP_READ,
            decoder->activation_dimensions, 2U
        );
        decoder->registered[registered_count++] = &decoder->projection_weight_tensors[index];
        decoder->registered[registered_count++] = &decoder->projection_bias_tensors[index];
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
            &decoder->outputs[index], 0, 0U
        );
        if (status != 0U) return -1;
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
    return 1;
}

int whisper_decoder_qnn_restore(
    WhisperDecoderQnn *decoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    const WhisperDecoderQnnIds *ids
) {
    u32 index;
    if (decoder == 0 || ids == 0 || ids->model_id != decoder->model.model_id ||
        ids->output_count != decoder->output_count || api->graph_retrieve(
            context, decoder->graph_name, &decoder->graph) != 0U) {
        return 0;
    }
    if (decoder->builder_allocation != 0) {
        VirtualFree(decoder->builder_allocation, 0U, 0x8000U);
        decoder->builder_allocation = 0;
    }
    decoder->input = decoder_qnn_tensor(
        decoder->input_name, QNN_TENSOR_TYPE_APP_WRITE,
        decoder->activation_dimensions, 2U
    );
    decoder->input.data.v1.id = ids->input_id;
    for (index = 0U; index < decoder->output_count; ++index) {
        decoder->outputs[index] = decoder_qnn_tensor(
            decoder->output_names[index], QNN_TENSOR_TYPE_APP_READ,
            decoder->activation_dimensions, 2U
        );
        decoder->outputs[index].data.v1.id = ids->output_ids[index];
    }
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
    for (index = 0U; index < decoder->output_count; ++index) {
        u32 layer = index / 2U;
        decoder->execute_outputs[index] = decoder->outputs[index];
        decoder->execute_outputs[index].data.v1.memory.client_buffer.data =
            (index & 1U) == 0U
            ? (void *)(decoder->keys_cache + layer * activation_values)
            : (void *)(decoder->values_cache + layer * activation_values);
        decoder->execute_outputs[index].data.v1.memory.client_buffer.data_size =
            (u32)activation_bytes;
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

void whisper_decoder_qnn_shutdown(WhisperDecoderQnn *decoder) {
    if (decoder == 0) return;
    if (decoder->builder_allocation != 0) {
        VirtualFree(decoder->builder_allocation, 0U, 0x8000U);
    }
    if (decoder->runtime_allocation != 0) {
        VirtualFree(decoder->runtime_allocation, 0U, 0x8000U);
    }
    VirtualFree(decoder, 0U, 0x8000U);
}