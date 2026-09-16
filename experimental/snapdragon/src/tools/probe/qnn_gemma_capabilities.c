#include "qnn_gemma_capabilities.h"

typedef unsigned long long usize;

__declspec(dllimport) void *GetStdHandle(u32 handle_id);
__declspec(dllimport) int WriteFile(
    void *handle,
    const void *buffer,
    u32 size,
    u32 *written,
    void *overlapped
);
__declspec(dllimport) void *VirtualAlloc(
    void *address,
    usize size,
    u32 allocation_type,
    u32 protection
);
__declspec(dllimport) int VirtualFree(void *address, usize size, u32 free_type);
__declspec(dllimport) void *LoadLibraryA(const char *name);
__declspec(dllimport) void *GetProcAddress(void *module, const char *name);
__declspec(dllimport) int FreeLibrary(void *module);

typedef void *(*GemmaRpcMemAlloc)(i32 heap_id, u32 flags, i32 size);
typedef void (*GemmaRpcMemFree)(void *pointer);
typedef i32 (*GemmaRpcMemToFd)(void *pointer);

enum {
    GEMMA_HIDDEN_WIDTH = 2560U,
    GEMMA_W4_WEIGHT_ELEMENTS = GEMMA_HIDDEN_WIDTH * GEMMA_HIDDEN_WIDTH,
    GEMMA_W4_BUILD_WEIGHT_BYTES = GEMMA_W4_WEIGHT_ELEMENTS
};

static usize text_length(const char *text) {
    usize length = 0U;
    while (text[length] != '\0') ++length;
    return length;
}

static void write_bytes(const void *data, usize size) {
    const u8 *cursor = (const u8 *)data;
    void *output = GetStdHandle(0xfffffff5U);
    while (size != 0U) {
        u32 chunk = size > 0xffffffffULL ? 0xffffffffU : (u32)size;
        u32 written = 0U;
        if (!WriteFile(output, cursor, chunk, &written, 0) || written == 0U) return;
        cursor += written;
        size -= written;
    }
}

static void write_text(const char *text) {
    write_bytes(text, text_length(text));
}

static void write_u32(u32 value) {
    char digits[10];
    u32 used = 0U;
    do {
        digits[used++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (used != 0U) write_bytes(&digits[--used], 1U);
}

static void write_hex64(u64 value) {
    static const char hex[] = "0123456789abcdef";
    char digits[16];
    u32 index;
    for (index = 0U; index < 16U; ++index) {
        digits[15U - index] = hex[value & 15U];
        value >>= 4U;
    }
    write_text("0x");
    write_bytes(digits, sizeof(digits));
}

static void write_status(const char *name, u64 status) {
    write_text(name);
    write_text(": ");
    write_hex64(status);
    write_text("\n");
}

static QnnTensor make_plain_tensor(
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

static QnnTensor make_bw_axis_tensor(
    const char *name,
    u32 type,
    u32 data_type,
    u32 *dimensions,
    u32 rank,
    i32 axis,
    u32 scale_count,
    float *scales
) {
    QnnTensor tensor = make_plain_tensor(name, type, data_type, dimensions, rank);
    tensor.data.v1.quantize_params.encoding_definition = QNN_DEFINITION_DEFINED;
    tensor.data.v1.quantize_params.quantization_encoding =
        QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET;
    tensor.data.v1.quantize_params.encoding.bw_axis_scale_offset.bitwidth = 4U;
    tensor.data.v1.quantize_params.encoding.bw_axis_scale_offset.axis = axis;
    tensor.data.v1.quantize_params.encoding.bw_axis_scale_offset.element_count = scale_count;
    tensor.data.v1.quantize_params.encoding.bw_axis_scale_offset.scales = scales;
    tensor.data.v1.quantize_params.encoding.bw_axis_scale_offset.offsets = 0;
    return tensor;
}

static float half_to_float(u16 value) {
    union {
        u32 bits;
        float value;
    } result;
    u32 sign = ((u32)value & 0x8000U) << 16U;
    u32 exponent = ((u32)value >> 10U) & 31U;
    u32 fraction = (u32)value & 1023U;
    if (exponent == 0U) {
        if (fraction == 0U) {
            result.bits = sign;
        } else {
            exponent = 113U;
            while ((fraction & 1024U) == 0U) {
                fraction <<= 1U;
                --exponent;
            }
            result.bits = sign | (exponent << 23U) | ((fraction & 1023U) << 13U);
        }
    } else if (exponent == 31U) {
        result.bits = sign | 0x7f800000U | (fraction << 13U);
    } else {
        result.bits = sign | ((exponent + 112U) << 23U) | (fraction << 13U);
    }
    return result.value;
}

static float absolute_float(float value) {
    return value < 0.0f ? -value : value;
}

static float square_root(float value) {
    float estimate = value > 1.0f ? value : 1.0f;
    u32 iteration;
    for (iteration = 0U; iteration < 12U; ++iteration) {
        estimate = 0.5f * (estimate + value / estimate);
    }
    return estimate;
}

static u64 add_node(
    const QnnInterfaceV2 *api,
    QnnGraphHandle graph,
    const char *name,
    const char *type,
    QnnTensor *inputs,
    u32 input_count,
    QnnTensor *outputs,
    u32 output_count,
    QnnParam *parameters,
    u32 parameter_count
) {
    QnnOpConfig operation = {0};
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = name;
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = type;
    operation.data.v1.parameter_count = parameter_count;
    operation.data.v1.parameters = parameters;
    operation.data.v1.input_count = input_count;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = output_count;
    operation.data.v1.outputs = outputs;
    return api->graph_add_node(graph, operation);
}

static u32 run_w4a16_projection(
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    u32 grouped
) {
    typedef struct GemmaBlockMappedEncoding {
        u32 bitwidth;
        u32 mapping;
        u32 *block_size;
        QnnScaleOffset *scale_offset;
    } GemmaBlockMappedEncoding;
    _Static_assert(sizeof(GemmaBlockMappedEncoding) == 24U, "QNN block-mapped ABI size");
    _Static_assert(sizeof(GemmaBlockMappedEncoding) <= sizeof(QnnQuantizeEncoding), "QNN encoding storage");
    static const u16 input_pattern[] = {
        0xbc00U, 0xb800U, 0U, 0x3800U, 0x3c00U, 0x4000U, 0xc000U
    };
    u32 input_dimensions[2] = {1U, GEMMA_HIDDEN_WIDTH};
    u32 weight_dimensions[2] = {GEMMA_HIDDEN_WIDTH, GEMMA_HIDDEN_WIDTH};
    u32 output_dimensions[2] = {1U, GEMMA_HIDDEN_WIDTH};
    usize input_bytes = GEMMA_HIDDEN_WIDTH * sizeof(u16);
    usize output_bytes = GEMMA_HIDDEN_WIDTH * sizeof(u16);
    usize scales_bytes = GEMMA_HIDDEN_WIDTH * sizeof(float);
    usize references_bytes = GEMMA_HIDDEN_WIDTH * sizeof(float);
    u16 *input = VirtualAlloc(0, input_bytes, 0x3000U, 0x04U);
    i8 *weights = VirtualAlloc(0, GEMMA_W4_BUILD_WEIGHT_BYTES, 0x3000U, 0x04U);
    u16 *output = VirtualAlloc(0, output_bytes, 0x3000U, 0x04U);
    float *scales = VirtualAlloc(0, scales_bytes, 0x3000U, 0x04U);
    float *references = VirtualAlloc(0, references_bytes, 0x3000U, 0x04U);
    QnnScaleOffset *block_scales = grouped ? VirtualAlloc(0,
        (GEMMA_W4_WEIGHT_ELEMENTS / 32U) * sizeof(QnnScaleOffset), 0x3000U, 0x04U) : 0;
    u32 block_dimensions[2] = {32U, 1U};
    GemmaBlockMappedEncoding block_encoding = {4U, 0U, block_dimensions, block_scales};
    QnnTensor inputs[2];
    QnnTensor outputs[1];
    QnnOpConfig operation = {0};
    QnnGraphHandle graph = 0;
    u64 status = 0U;
    float maximum_delta = 0.0f;
    u32 input_index;
    u32 output_index;
    u32 result = 0U;

    write_text(grouped ? "TranslateGemma diagnostic group-32 W4A16 projection\n" : "TranslateGemma Stage 1 W4A16 projection\n");
    write_text(grouped ? "  shape: [1,2560] x [2560,2560], block-mapped [32,1] scales\n" :
        "  shape: [1,2560] x [2560,2560], per-output scales\n");
    if (input == 0 || weights == 0 || output == 0 || scales == 0 || references == 0 || (grouped && !block_scales)) {
        result = 120U;
        goto cleanup;
    }
    for (input_index = 0U; input_index < GEMMA_HIDDEN_WIDTH; ++input_index) {
        input[input_index] = input_pattern[input_index %
            (sizeof(input_pattern) / sizeof(input_pattern[0]))];
    }
    for (output_index = 0U; output_index < GEMMA_HIDDEN_WIDTH; ++output_index) {
        scales[output_index] = 0.015625f * (float)(1U + output_index % 4U);
        references[output_index] = 0.0f;
    }
    for (input_index = 0U; input_index < GEMMA_HIDDEN_WIDTH; ++input_index) {
        float input_value = half_to_float(input[input_index]);
        for (output_index = 0U; output_index < GEMMA_HIDDEN_WIDTH; ++output_index) {
            u32 index = input_index * GEMMA_HIDDEN_WIDTH + output_index;
            i32 weight = (i32)((input_index * 13U + output_index * 7U) % 15U) - 7;
            float weight_scale = scales[output_index];
            if (grouped) {
                u32 block_index = (input_index / 32U) * GEMMA_HIDDEN_WIDTH + output_index;
                weight_scale *= 0.25f * (float)(1U + (input_index / 32U) % 4U);
                block_scales[block_index].scale = weight_scale;
                block_scales[block_index].offset = 0;
            }
            weights[index] = (i8)weight;
            references[output_index] += input_value * (float)weight *
                weight_scale;
        }
    }

    status = api->graph_create(context, grouped ? "gemma_w4a16_group32_projection" : "gemma_w4a16_projection", 0, &graph);
    write_status("  graphCreate", status);
    if (status != 0U) {
        result = 121U;
        goto cleanup;
    }
    inputs[0] = make_plain_tensor(
        "gemma_projection_input", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, input_dimensions, 2U
    );
    inputs[1] = make_bw_axis_tensor(
        "gemma_projection_weight", QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_SFIXED_POINT_8, weight_dimensions, 2U, 1,
        GEMMA_HIDDEN_WIDTH, scales
    );
    if (grouped) {
        inputs[1].data.v1.quantize_params.quantization_encoding = 9U;
        inputs[1].data.v1.quantize_params.encoding = (QnnQuantizeEncoding){0};
        inputs[1].data.v1.quantize_params.encoding.reserved[0] = (usize)&block_encoding;
    }
    inputs[1].data.v1.memory.client_buffer.data = weights;
    inputs[1].data.v1.memory.client_buffer.data_size = GEMMA_W4_BUILD_WEIGHT_BYTES;
    outputs[0] = make_plain_tensor(
        "gemma_projection_output", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_FLOAT_16, output_dimensions, 2U
    );
    status = api->tensor_create_graph_tensor(graph, &inputs[0]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &inputs[1]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[0]);
    write_status("  tensorCreate", status);
    if (status != 0U) {
        result = 122U;
        goto cleanup;
    }
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "gemma_w4a16_matmul";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "MatMul";
    operation.data.v1.input_count = 2U;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = outputs;
    status = api->graph_add_node(graph, operation);
    write_status("  graphAddNode MatMul", status);
    if (status != 0U) {
        result = 123U;
        goto cleanup;
    }
    status = api->graph_finalize(graph, 0, 0);
    write_status("  graphFinalize", status);
    if (status != 0U) {
        result = 124U;
        goto cleanup;
    }
    inputs[0].data.v1.memory.client_buffer.data = input;
    inputs[0].data.v1.memory.client_buffer.data_size = (u32)input_bytes;
    outputs[0].data.v1.memory.client_buffer.data = output;
    outputs[0].data.v1.memory.client_buffer.data_size = (u32)output_bytes;
    status = api->graph_execute(graph, inputs, 1U, outputs, 1U, 0, 0);
    write_status("  graphExecute", status);
    if (status != 0U) {
        result = 125U;
        goto cleanup;
    }
    for (output_index = 0U; output_index < GEMMA_HIDDEN_WIDTH; ++output_index) {
        float delta = absolute_float(
            half_to_float(output[output_index]) - references[output_index]
        );
        if (delta > maximum_delta) maximum_delta = delta;
        if (delta > 0.25f) {
            write_text("  numerical mismatch at output ");
            write_u32(output_index);
            write_text("; absolute error (micro-units): ");
            write_u32((u32)(delta * 1000000.0f));
            write_text("\n");
            result = 126U;
            goto cleanup;
        }
    }
    write_text("  maximum absolute error (micro-units): ");
    write_u32((u32)(maximum_delta * 1000000.0f));
    write_text("\n  result: supported and numerically accepted\n");

cleanup:
    if (block_scales != 0) VirtualFree(block_scales, 0U, 0x8000U);
    if (references != 0) VirtualFree(references, 0U, 0x8000U);
    if (scales != 0) VirtualFree(scales, 0U, 0x8000U);
    if (output != 0) VirtualFree(output, 0U, 0x8000U);
    if (weights != 0) VirtualFree(weights, 0U, 0x8000U);
    if (input != 0) VirtualFree(input, 0U, 0x8000U);
    return result;
}

static u32 run_rms_norm(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    u32 activation_dimensions[2] = {2U, 8U};
    u32 parameter_dimensions[1] = {8U};
    u32 axes_dimensions[1] = {1U};
    u32 axes_data[1] = {1U};
    u16 input_data[16] = {
        0x3c00U, 0xbc00U, 0x4000U, 0xc000U,
        0x3800U, 0xb800U, 0x3400U, 0xb400U,
        0x3c00U, 0x3c00U, 0x3c00U, 0x3c00U,
        0x3c00U, 0x3c00U, 0x3c00U, 0x3c00U
    };
    u16 scale_data[8] = {
        0x3c00U, 0x4000U, 0x3800U, 0x3c00U,
        0x4000U, 0x3800U, 0x3c00U, 0x4000U
    };
    u16 output_data[16] = {0U};
    QnnTensor inputs[2];
    QnnTensor outputs[1];
    QnnParam parameters[2] = {0};
    QnnOpConfig operation = {0};
    QnnGraphHandle graph = 0;
    u64 status;
    u32 row;
    u32 column;

    write_text("TranslateGemma Stage 1 FP16 RMSNorm\n");
    status = api->graph_create(context, "gemma_rms_norm", 0, &graph);
    write_status("  graphCreate", status);
    if (status != 0U) return 130U;
    inputs[0] = make_plain_tensor(
        "gemma_rms_input", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, activation_dimensions, 2U
    );
    inputs[1] = make_plain_tensor(
        "gemma_rms_scale", QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, parameter_dimensions, 1U
    );
    inputs[1].data.v1.memory.client_buffer.data = scale_data;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(scale_data);
    outputs[0] = make_plain_tensor(
        "gemma_rms_output", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_FLOAT_16, activation_dimensions, 2U
    );
    status = api->tensor_create_graph_tensor(graph, &inputs[0]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &inputs[1]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[0]);
    parameters[0].type = QNN_PARAMTYPE_SCALAR;
    parameters[0].name = "epsilon";
    parameters[0].value.scalar.data_type = QNN_DATATYPE_FLOAT_32;
    parameters[0].value.scalar.value.float_value = 0.000001f;
    parameters[1].type = QNN_PARAMTYPE_TENSOR;
    parameters[1].name = "axes";
    parameters[1].value.tensor = make_plain_tensor(
        "gemma_rms_axes", QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_UINT_32, axes_dimensions, 1U
    );
    parameters[1].value.tensor.data.v1.memory.client_buffer.data = axes_data;
    parameters[1].value.tensor.data.v1.memory.client_buffer.data_size = sizeof(axes_data);
    if (status == 0U) {
        status = api->tensor_create_graph_tensor(graph, &parameters[1].value.tensor);
    }
    write_status("  tensorCreate", status);
    if (status != 0U) return 131U;
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "gemma_rms_norm_node";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "RmsNorm";
    operation.data.v1.parameter_count = 2U;
    operation.data.v1.parameters = parameters;
    operation.data.v1.input_count = 2U;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = outputs;
    status = api->graph_add_node(graph, operation);
    write_status("  graphAddNode RmsNorm", status);
    if (status != 0U) return 132U;
    status = api->graph_finalize(graph, 0, 0);
    write_status("  graphFinalize", status);
    if (status != 0U) return 133U;
    inputs[0].data.v1.memory.client_buffer.data = input_data;
    inputs[0].data.v1.memory.client_buffer.data_size = sizeof(input_data);
    outputs[0].data.v1.memory.client_buffer.data = output_data;
    outputs[0].data.v1.memory.client_buffer.data_size = sizeof(output_data);
    status = api->graph_execute(graph, inputs, 1U, outputs, 1U, 0, 0);
    write_status("  graphExecute", status);
    if (status != 0U) return 134U;
    for (row = 0U; row < 2U; ++row) {
        float mean_square = 0.0f;
        for (column = 0U; column < 8U; ++column) {
            float value = half_to_float(input_data[row * 8U + column]);
            mean_square += value * value;
        }
        mean_square *= 0.125f;
        for (column = 0U; column < 8U; ++column) {
            float expected = half_to_float(input_data[row * 8U + column]) *
                half_to_float(scale_data[column]) /
                square_root(mean_square + 0.000001f);
            float actual = half_to_float(output_data[row * 8U + column]);
            if (absolute_float(actual - expected) > 0.01f) {
                write_text("  numerical mismatch at element ");
                write_u32(row * 8U + column);
                write_text("\n");
                return 135U;
            }
        }
    }
    write_text("  result: supported and numerically accepted\n");
    return 0U;
}

static u32 run_gated_gelu(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    static const float expected[8] = {
        0.0f, 1.68269f, -0.47597f, 7.81800f,
        -0.02275f, -0.34573f, -0.30854f, 0.74899f
    };
    u32 dimensions[2] = {1U, 8U};
    u16 activation_data[8] = {
        0U, 0x3c00U, 0xbc00U, 0x4000U,
        0xc000U, 0x3800U, 0xb800U, 0x4200U
    };
    u16 gate_data[8] = {
        0x3c00U, 0x4000U, 0x4200U, 0x4400U,
        0x3800U, 0xbc00U, 0x4000U, 0x3400U
    };
    u16 output_data[8] = {0U};
    QnnTensor inputs[2];
    QnnTensor gelu;
    QnnTensor outputs[1];
    QnnTensor multiply_inputs[2];
    QnnGraphHandle graph = 0;
    u64 status;
    u32 index;

    write_text("TranslateGemma Stage 1 gated GELU composition\n");
    status = api->graph_create(context, "gemma_gated_gelu", 0, &graph);
    if (status != 0U) return 136U;
    inputs[0] = make_plain_tensor(
        "gemma_gelu_input", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, dimensions, 2U
    );
    inputs[1] = make_plain_tensor(
        "gemma_gate_input", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, dimensions, 2U
    );
    gelu = make_plain_tensor(
        "gemma_gelu_output", QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, dimensions, 2U
    );
    outputs[0] = make_plain_tensor(
        "gemma_gated_output", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_FLOAT_16, dimensions, 2U
    );
    status = api->tensor_create_graph_tensor(graph, &inputs[0]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &inputs[1]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &gelu);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[0]);
    if (status != 0U) return 137U;
    status = add_node(api, graph, "gemma_gelu", "Gelu", &inputs[0], 1U, &gelu, 1U, 0, 0U);
    multiply_inputs[0] = gelu;
    multiply_inputs[1] = inputs[1];
    if (status == 0U) {
        status = add_node(
            api, graph, "gemma_gelu_gate", "ElementWiseMultiply",
            multiply_inputs, 2U, outputs, 1U, 0, 0U
        );
    }
    write_status("  graphAddNode Gelu/Multiply", status);
    if (status != 0U) return 138U;
    status = api->graph_finalize(graph, 0, 0);
    write_status("  graphFinalize", status);
    if (status != 0U) return 139U;
    inputs[0].data.v1.memory.client_buffer.data = activation_data;
    inputs[0].data.v1.memory.client_buffer.data_size = sizeof(activation_data);
    inputs[1].data.v1.memory.client_buffer.data = gate_data;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(gate_data);
    outputs[0].data.v1.memory.client_buffer.data = output_data;
    outputs[0].data.v1.memory.client_buffer.data_size = sizeof(output_data);
    status = api->graph_execute(graph, inputs, 2U, outputs, 1U, 0, 0);
    write_status("  graphExecute", status);
    if (status != 0U) return 140U;
    for (index = 0U; index < 8U; ++index) {
        if (absolute_float(half_to_float(output_data[index]) - expected[index]) > 0.03f) {
            write_text("  numerical mismatch at element ");
            write_u32(index);
            write_text("\n");
            return 141U;
        }
    }
    write_text("  result: supported and numerically accepted\n");
    return 0U;
}

static u32 run_gather(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    u32 table_dimensions[2] = {4U, 4U};
    u32 index_dimensions[1] = {2U};
    u32 output_dimensions[2] = {2U, 4U};
    u16 table_data[16];
    u32 index_data[2] = {3U, 1U};
    u16 output_data[8] = {0U};
    QnnTensor inputs[2];
    QnnTensor outputs[1];
    QnnParam parameter = {0};
    QnnGraphHandle graph = 0;
    u64 status;
    u32 index;

    write_text("TranslateGemma Stage 1 Gather\n");
    for (index = 0U; index < 16U; ++index) table_data[index] = (u16)(0x3c00U + index);
    status = api->graph_create(context, "gemma_gather", 0, &graph);
    if (status != 0U) return 142U;
    inputs[0] = make_plain_tensor(
        "gemma_gather_table", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, table_dimensions, 2U
    );
    inputs[1] = make_plain_tensor(
        "gemma_gather_indices", QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_UINT_32, index_dimensions, 1U
    );
    inputs[1].data.v1.memory.client_buffer.data = index_data;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(index_data);
    outputs[0] = make_plain_tensor(
        "gemma_gather_output", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_FLOAT_16, output_dimensions, 2U
    );
    status = api->tensor_create_graph_tensor(graph, &inputs[0]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &inputs[1]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[0]);
    parameter.type = QNN_PARAMTYPE_SCALAR;
    parameter.name = "axis";
    parameter.value.scalar.data_type = QNN_DATATYPE_INT_32;
    parameter.value.scalar.value.int32_value = 0;
    if (status == 0U) {
        status = add_node(
            api, graph, "gemma_gather_node", "Gather", inputs, 2U,
            outputs, 1U, &parameter, 1U
        );
    }
    write_status("  graphAddNode Gather", status);
    if (status != 0U) return 143U;
    status = api->graph_finalize(graph, 0, 0);
    write_status("  graphFinalize", status);
    if (status != 0U) return 144U;
    inputs[0].data.v1.memory.client_buffer.data = table_data;
    inputs[0].data.v1.memory.client_buffer.data_size = sizeof(table_data);
    outputs[0].data.v1.memory.client_buffer.data = output_data;
    outputs[0].data.v1.memory.client_buffer.data_size = sizeof(output_data);
    status = api->graph_execute(graph, inputs, 1U, outputs, 1U, 0, 0);
    write_status("  graphExecute", status);
    if (status != 0U) return 145U;
    for (index = 0U; index < 4U; ++index) {
        if (output_data[index] != table_data[12U + index] ||
            output_data[4U + index] != table_data[4U + index]) return 146U;
    }
    write_text("  result: supported and exact\n");
    return 0U;
}

static u32 run_argmax_topk(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    u32 input_dimensions[2] = {1U, 8U};
    u32 argmax_dimensions[1] = {1U};
    u32 topk_dimensions[2] = {1U, 3U};
    u16 input_data[8] = {
        0x3c00U, 0xc000U, 0x4200U, 0x3800U,
        0x4400U, 0xbc00U, 0x4000U, 0U
    };
    u32 argmax_data[1] = {0U};
    u16 topk_values[3] = {0U};
    u32 topk_indices[3] = {0U};
    QnnTensor input;
    QnnTensor outputs[3];
    QnnTensor topk_outputs[2];
    QnnParam argmax_parameters[2] = {0};
    QnnParam topk_parameters[2] = {0};
    QnnGraphHandle graph = 0;
    u64 status;

    write_text("TranslateGemma Stage 1 Argmax and TopK\n");
    status = api->graph_create(context, "gemma_selection", 0, &graph);
    if (status != 0U) return 147U;
    input = make_plain_tensor(
        "gemma_logits", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, input_dimensions, 2U
    );
    outputs[0] = make_plain_tensor(
        "gemma_argmax", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_UINT_32, argmax_dimensions, 1U
    );
    outputs[1] = make_plain_tensor(
        "gemma_topk_values", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_FLOAT_16, topk_dimensions, 2U
    );
    outputs[2] = make_plain_tensor(
        "gemma_topk_indices", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_UINT_32, topk_dimensions, 2U
    );
    status = api->tensor_create_graph_tensor(graph, &input);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[0]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[1]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[2]);
    argmax_parameters[0].type = QNN_PARAMTYPE_SCALAR;
    argmax_parameters[0].name = "axis";
    argmax_parameters[0].value.scalar.data_type = QNN_DATATYPE_UINT_32;
    argmax_parameters[0].value.scalar.value.uint32_value = 1U;
    argmax_parameters[1].type = QNN_PARAMTYPE_SCALAR;
    argmax_parameters[1].name = "keep_dims";
    argmax_parameters[1].value.scalar.data_type = QNN_DATATYPE_BOOL_8;
    argmax_parameters[1].value.scalar.value.bool8_value = 0U;
    if (status == 0U) {
        status = add_node(
            api, graph, "gemma_argmax_node", "Argmax", &input, 1U,
            &outputs[0], 1U, argmax_parameters, 2U
        );
    }
    topk_parameters[0].type = QNN_PARAMTYPE_SCALAR;
    topk_parameters[0].name = "k";
    topk_parameters[0].value.scalar.data_type = QNN_DATATYPE_UINT_32;
    topk_parameters[0].value.scalar.value.uint32_value = 3U;
    topk_parameters[1].type = QNN_PARAMTYPE_SCALAR;
    topk_parameters[1].name = "largest";
    topk_parameters[1].value.scalar.data_type = QNN_DATATYPE_BOOL_8;
    topk_parameters[1].value.scalar.value.bool8_value = 1U;
    topk_outputs[0] = outputs[1];
    topk_outputs[1] = outputs[2];
    if (status == 0U) {
        status = add_node(
            api, graph, "gemma_topk_node", "TopK", &input, 1U,
            topk_outputs, 2U, topk_parameters, 2U
        );
    }
    write_status("  graphAddNode Argmax/TopK", status);
    if (status != 0U) return 148U;
    status = api->graph_finalize(graph, 0, 0);
    write_status("  graphFinalize", status);
    if (status != 0U) return 149U;
    input.data.v1.memory.client_buffer.data = input_data;
    input.data.v1.memory.client_buffer.data_size = sizeof(input_data);
    outputs[0].data.v1.memory.client_buffer.data = argmax_data;
    outputs[0].data.v1.memory.client_buffer.data_size = sizeof(argmax_data);
    outputs[1].data.v1.memory.client_buffer.data = topk_values;
    outputs[1].data.v1.memory.client_buffer.data_size = sizeof(topk_values);
    outputs[2].data.v1.memory.client_buffer.data = topk_indices;
    outputs[2].data.v1.memory.client_buffer.data_size = sizeof(topk_indices);
    status = api->graph_execute(graph, &input, 1U, outputs, 3U, 0, 0);
    write_status("  graphExecute", status);
    if (status != 0U) return 150U;
    if (argmax_data[0] != 4U || topk_indices[0] != 4U ||
        topk_indices[1] != 2U || topk_indices[2] != 6U ||
        topk_values[0] != input_data[4] || topk_values[1] != input_data[2] ||
        topk_values[2] != input_data[6]) return 151U;
    write_text("  result: supported and exact\n");
    return 0U;
}

static u32 run_rotary_embedding(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    enum { VALUES = 8U * 256U, CACHE_VALUES = 128U };
    u32 input_dimensions[4] = {1U, 8U, 1U, 256U};
    u32 cache_dimensions[2] = {1U, 128U};
    u32 position_dimensions[2] = {1U, 1U};
    static u16 input_data[VALUES];
    static u16 cosine_data[CACHE_VALUES];
    static u16 sine_data[CACHE_VALUES];
    static i32 position_data[1] = {0};
    static u16 output_data[VALUES];
    QnnTensor inputs[4];
    QnnTensor outputs[1];
    QnnParam parameters[2] = {0};
    QnnGraphHandle graph = 0;
    u64 status;
    u32 index;

    write_text("TranslateGemma Stage 1 RotaryEmbedding [1,8,1,256]\n");
    for (index = 0U; index < VALUES; ++index) {
        input_data[index] = (index % 256U) < 128U ? 0x3c00U : 0x4000U;
    }
    for (index = 0U; index < CACHE_VALUES; ++index) {
        cosine_data[index] = 0U;
        sine_data[index] = 0x3c00U;
    }
    status = api->graph_create(context, "gemma_rotary_embedding", 0, &graph);
    if (status != 0U) return 152U;
    inputs[0] = make_plain_tensor("gemma_rope_input", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, input_dimensions, 4U);
    inputs[1] = make_plain_tensor("gemma_rope_cos", QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, cache_dimensions, 2U);
    inputs[2] = make_plain_tensor("gemma_rope_sin", QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, cache_dimensions, 2U);
    inputs[3] = make_plain_tensor("gemma_rope_position", QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_INT_32, position_dimensions, 2U);
    inputs[1].data.v1.memory.client_buffer.data = cosine_data;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(cosine_data);
    inputs[2].data.v1.memory.client_buffer.data = sine_data;
    inputs[2].data.v1.memory.client_buffer.data_size = sizeof(sine_data);
    inputs[3].data.v1.memory.client_buffer.data = position_data;
    inputs[3].data.v1.memory.client_buffer.data_size = sizeof(position_data);
    outputs[0] = make_plain_tensor("gemma_rope_output", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_FLOAT_16, input_dimensions, 4U);
    for (index = 0U; index < 4U && status == 0U; ++index) {
        status = api->tensor_create_graph_tensor(graph, &inputs[index]);
    }
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[0]);
    parameters[0].type = QNN_PARAMTYPE_SCALAR;
    parameters[0].name = "interleaved";
    parameters[0].value.scalar.data_type = QNN_DATATYPE_BOOL_8;
    parameters[0].value.scalar.value.bool8_value = 0U;
    parameters[1].type = QNN_PARAMTYPE_SCALAR;
    parameters[1].name = "rotary_embedding_dim";
    parameters[1].value.scalar.data_type = QNN_DATATYPE_UINT_32;
    parameters[1].value.scalar.value.uint32_value = 256U;
    if (status == 0U) status = add_node(api, graph, "gemma_rope_node", "RotaryEmbedding",
        inputs, 4U, outputs, 1U, parameters, 2U);
    write_status("  graphAddNode RotaryEmbedding", status);
    if (status != 0U) return 153U;
    status = api->graph_finalize(graph, 0, 0);
    write_status("  graphFinalize", status);
    if (status != 0U) return 154U;
    inputs[0].data.v1.memory.client_buffer.data = input_data;
    inputs[0].data.v1.memory.client_buffer.data_size = sizeof(input_data);
    outputs[0].data.v1.memory.client_buffer.data = output_data;
    outputs[0].data.v1.memory.client_buffer.data_size = sizeof(output_data);
    status = api->graph_execute(graph, inputs, 1U, outputs, 1U, 0, 0);
    write_status("  graphExecute", status);
    if (status != 0U) return 155U;
    for (index = 0U; index < VALUES; ++index) {
        u16 expected = (index % 256U) < 128U ? 0xc000U : 0x3c00U;
        if (output_data[index] != expected) return 156U;
    }
    write_text("  result: supported and exact\n");
    return 0U;
}

static u32 run_group_query_attention(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    enum {
        QUERY_HEADS = 8U,
        KV_HEADS = 4U,
        HEAD_WIDTH = 256U,
        SEQUENCE_LENGTH = 2U,
        QUERY_VALUES = SEQUENCE_LENGTH * QUERY_HEADS * HEAD_WIDTH,
        KV_VALUES = SEQUENCE_LENGTH * KV_HEADS * HEAD_WIDTH
    };
    static u16 query_data[QUERY_VALUES];
    static u16 key_data[KV_VALUES];
    static u16 value_data[KV_VALUES];
    static u16 output_data[QUERY_VALUES];
    static i32 sequence_lengths[1] = {1};
    static i32 maximum_sequence_length = 2;
    u32 query_dimensions[3] = {1U, SEQUENCE_LENGTH, QUERY_HEADS * HEAD_WIDTH};
    u32 key_value_dimensions[3] = {1U, SEQUENCE_LENGTH, KV_HEADS * HEAD_WIDTH};
    u32 sequence_dimensions[1] = {1U};
    QnnTensor inputs[5];
    QnnTensor outputs[1];
    QnnParam parameters[2] = {0};
    QnnGraphHandle graph = 0;
    u64 status;
    u32 index;

    write_text("TranslateGemma Stage 1 GroupQueryAttention [1,2,8x256;4x256]\n");
    for (index = 0U; index < KV_VALUES; ++index) {
        value_data[index] = index < KV_HEADS * HEAD_WIDTH ? 0x3c00U : 0x4200U;
    }
    status = api->graph_create(context, "gemma_group_query_attention", 0, &graph);
    if (status != 0U) return 157U;
    inputs[0] = make_plain_tensor(
        "gemma_gqa_query", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, query_dimensions, 3U
    );
    inputs[1] = make_plain_tensor(
        "gemma_gqa_sequence_lengths", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_INT_32, sequence_dimensions, 1U
    );
    inputs[2] = make_plain_tensor(
        "gemma_gqa_maximum_sequence_length", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_INT_32, 0, 0U
    );
    inputs[3] = make_plain_tensor(
        "gemma_gqa_key", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, key_value_dimensions, 3U
    );
    inputs[4] = make_plain_tensor(
        "gemma_gqa_value", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, key_value_dimensions, 3U
    );
    outputs[0] = make_plain_tensor(
        "gemma_gqa_output", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_FLOAT_16, query_dimensions, 3U
    );
    for (index = 0U; index < 5U && status == 0U; ++index) {
        status = api->tensor_create_graph_tensor(graph, &inputs[index]);
    }
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[0]);
    parameters[0].type = QNN_PARAMTYPE_SCALAR;
    parameters[0].name = "num_heads";
    parameters[0].value.scalar.data_type = QNN_DATATYPE_UINT_32;
    parameters[0].value.scalar.value.uint32_value = QUERY_HEADS;
    parameters[1].type = QNN_PARAMTYPE_SCALAR;
    parameters[1].name = "kv_num_heads";
    parameters[1].value.scalar.data_type = QNN_DATATYPE_UINT_32;
    parameters[1].value.scalar.value.uint32_value = KV_HEADS;
    if (status == 0U) {
        status = add_node(
            api, graph, "gemma_gqa_node", "GroupQueryAttention",
            inputs, 5U, outputs, 1U, parameters, 2U
        );
    }
    write_status("  graphAddNode GroupQueryAttention", status);
    if (status != 0U) return 158U;
    status = api->graph_finalize(graph, 0, 0);
    write_status("  graphFinalize", status);
    if (status == 1002U) {
        write_text("  result: direct op unavailable; primitive composition required\n");
        return 0U;
    }
    if (status != 0U) return 159U;
    inputs[0].data.v1.memory.client_buffer.data = query_data;
    inputs[0].data.v1.memory.client_buffer.data_size = sizeof(query_data);
    inputs[1].data.v1.memory.client_buffer.data = sequence_lengths;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(sequence_lengths);
    inputs[2].data.v1.memory.client_buffer.data = &maximum_sequence_length;
    inputs[2].data.v1.memory.client_buffer.data_size = sizeof(maximum_sequence_length);
    inputs[3].data.v1.memory.client_buffer.data = key_data;
    inputs[3].data.v1.memory.client_buffer.data_size = sizeof(key_data);
    inputs[4].data.v1.memory.client_buffer.data = value_data;
    inputs[4].data.v1.memory.client_buffer.data_size = sizeof(value_data);
    outputs[0].data.v1.memory.client_buffer.data = output_data;
    outputs[0].data.v1.memory.client_buffer.data_size = sizeof(output_data);
    status = api->graph_execute(graph, inputs, 5U, outputs, 1U, 0, 0);
    write_status("  graphExecute", status);
    if (status != 0U) return 160U;
    for (index = 0U; index < QUERY_VALUES; ++index) {
        u16 expected = index < QUERY_HEADS * HEAD_WIDTH ? 0x3c00U : 0x4000U;
        if (output_data[index] != expected) {
            write_text("  causal numerical mismatch at element ");
            write_u32(index);
            write_text("\n");
            return 161U;
        }
    }
    write_text("  result: grouped heads and causal masking supported and exact\n");
    return 0U;
}

static u32 run_group_query_attention_composition(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    enum {
        QUERY_HEADS = 8U,
        KV_HEADS = 4U,
        HEAD_WIDTH = 256U,
        SEQUENCE_LENGTH = 2U,
        HEAD_VALUES = QUERY_HEADS * SEQUENCE_LENGTH * HEAD_WIDTH,
        SCORE_VALUES = QUERY_HEADS * SEQUENCE_LENGTH * SEQUENCE_LENGTH,
        OUTPUT_VALUES = SEQUENCE_LENGTH * QUERY_HEADS * HEAD_WIDTH
    };
    static u16 query_data[HEAD_VALUES];
    static u16 key_data[HEAD_VALUES];
    static u16 value_data[HEAD_VALUES];
    static u16 mask_data[SCORE_VALUES];
    static u16 output_data[OUTPUT_VALUES];
    static u32 permutation[3] = {1U, 0U, 2U};
    static const u16 integer_halves[6] = {
        0x3c00U, 0x4000U, 0x4200U, 0x4400U, 0x4500U, 0x4600U
    };
    u32 query_dimensions[3] = {QUERY_HEADS, SEQUENCE_LENGTH, HEAD_WIDTH};
    u32 key_dimensions[3] = {QUERY_HEADS, HEAD_WIDTH, SEQUENCE_LENGTH};
    u32 score_dimensions[3] = {QUERY_HEADS, SEQUENCE_LENGTH, SEQUENCE_LENGTH};
    u32 transposed_dimensions[3] = {SEQUENCE_LENGTH, QUERY_HEADS, HEAD_WIDTH};
    u32 output_dimensions[3] = {1U, SEQUENCE_LENGTH, QUERY_HEADS * HEAD_WIDTH};
    u32 permutation_dimensions[1] = {3U};
    static QnnTensor tensors[10];
    static QnnTensor runtime_inputs[4];
    static QnnTensor runtime_output;
    QnnParam permutation_parameter = {0};
    QnnGraphHandle graph = 0;
    u64 status;
    u32 head;
    u32 index;

    write_text("TranslateGemma Stage 1 grouped attention composition [8Q/4KV,256]\n");
    for (head = 0U; head < QUERY_HEADS; ++head) {
        u32 kv_head = head / (QUERY_HEADS / KV_HEADS);
        u32 token;
        for (token = 0U; token < SEQUENCE_LENGTH; ++token) {
            u32 base = (head * SEQUENCE_LENGTH + token) * HEAD_WIDTH;
            u16 value = integer_halves[kv_head + token * 2U];
            for (index = 0U; index < HEAD_WIDTH; ++index) value_data[base + index] = value;
        }
        mask_data[head * 4U] = 0U;
        mask_data[head * 4U + 1U] = 0xfbffU;
        mask_data[head * 4U + 2U] = 0U;
        mask_data[head * 4U + 3U] = 0U;
    }
    status = api->graph_create(context, "gemma_grouped_attention_composition", 0, &graph);
    if (status != 0U) return 162U;
    tensors[0] = make_plain_tensor("gemma_attention_query", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, query_dimensions, 3U);
    tensors[1] = make_plain_tensor("gemma_attention_key", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, key_dimensions, 3U);
    tensors[2] = make_plain_tensor("gemma_attention_scores", QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, score_dimensions, 3U);
    tensors[3] = make_plain_tensor("gemma_attention_mask", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, score_dimensions, 3U);
    tensors[4] = make_plain_tensor("gemma_attention_masked", QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, score_dimensions, 3U);
    tensors[5] = make_plain_tensor("gemma_attention_probabilities", QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, score_dimensions, 3U);
    tensors[6] = make_plain_tensor("gemma_attention_value", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, query_dimensions, 3U);
    tensors[7] = make_plain_tensor("gemma_attention_heads", QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, query_dimensions, 3U);
    tensors[8] = make_plain_tensor("gemma_attention_transposed", QNN_TENSOR_TYPE_NATIVE,
        QNN_DATATYPE_FLOAT_16, transposed_dimensions, 3U);
    tensors[9] = make_plain_tensor("gemma_attention_output", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_FLOAT_16, output_dimensions, 3U);
    for (index = 0U; index < 10U && status == 0U; ++index) {
        status = api->tensor_create_graph_tensor(graph, &tensors[index]);
    }
    permutation_parameter.type = QNN_PARAMTYPE_TENSOR;
    permutation_parameter.name = "perm";
    permutation_parameter.value.tensor = make_plain_tensor(
        "gemma_attention_permutation", QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_UINT_32, permutation_dimensions, 1U
    );
    permutation_parameter.value.tensor.data.v1.memory.client_buffer.data = permutation;
    permutation_parameter.value.tensor.data.v1.memory.client_buffer.data_size =
        sizeof(permutation);
    if (status == 0U) {
        status = api->tensor_create_graph_tensor(
            graph, &permutation_parameter.value.tensor
        );
    }
    if (status == 0U) status = add_node(api, graph, "gemma_attention_scores_node",
        "MatMul", &tensors[0], 2U, &tensors[2], 1U, 0, 0U);
    if (status == 0U) status = add_node(api, graph, "gemma_attention_mask_node",
        "ElementWiseAdd", &tensors[2], 2U, &tensors[4], 1U, 0, 0U);
    if (status == 0U) status = add_node(api, graph, "gemma_attention_softmax_node",
        "Softmax", &tensors[4], 1U, &tensors[5], 1U, 0, 0U);
    if (status == 0U) status = add_node(api, graph, "gemma_attention_values_node",
        "MatMul", &tensors[5], 2U, &tensors[7], 1U, 0, 0U);
    if (status == 0U) status = add_node(api, graph, "gemma_attention_transpose_node",
        "Transpose", &tensors[7], 1U, &tensors[8], 1U,
        &permutation_parameter, 1U);
    if (status == 0U) status = add_node(api, graph, "gemma_attention_reshape_node",
        "Reshape", &tensors[8], 1U, &tensors[9], 1U, 0, 0U);
    write_status("  graphAddNode attention composition", status);
    if (status != 0U) return 163U;
    status = api->graph_finalize(graph, 0, 0);
    write_status("  graphFinalize", status);
    if (status != 0U) return 164U;
    runtime_inputs[0] = tensors[0];
    runtime_inputs[0].data.v1.memory.client_buffer.data = query_data;
    runtime_inputs[0].data.v1.memory.client_buffer.data_size = sizeof(query_data);
    runtime_inputs[1] = tensors[1];
    runtime_inputs[1].data.v1.memory.client_buffer.data = key_data;
    runtime_inputs[1].data.v1.memory.client_buffer.data_size = sizeof(key_data);
    runtime_inputs[2] = tensors[3];
    runtime_inputs[2].data.v1.memory.client_buffer.data = mask_data;
    runtime_inputs[2].data.v1.memory.client_buffer.data_size = sizeof(mask_data);
    runtime_inputs[3] = tensors[6];
    runtime_inputs[3].data.v1.memory.client_buffer.data = value_data;
    runtime_inputs[3].data.v1.memory.client_buffer.data_size = sizeof(value_data);
    runtime_output = tensors[9];
    runtime_output.data.v1.memory.client_buffer.data = output_data;
    runtime_output.data.v1.memory.client_buffer.data_size = sizeof(output_data);
    status = api->graph_execute(graph, runtime_inputs, 4U, &runtime_output, 1U, 0, 0);
    write_status("  graphExecute", status);
    if (status != 0U) return 165U;
    for (index = 0U; index < OUTPUT_VALUES; ++index) {
        u32 token = index / (QUERY_HEADS * HEAD_WIDTH);
        u32 head_in_token = (index / HEAD_WIDTH) % QUERY_HEADS;
        u32 kv_head = head_in_token / (QUERY_HEADS / KV_HEADS);
        u16 expected = integer_halves[kv_head + token];
        if (absolute_float(
                half_to_float(output_data[index]) - half_to_float(expected)
            ) > 0.01f) {
            write_text("  causal numerical mismatch at element ");
            write_u32(index);
            write_text("\n");
            return 166U;
        }
    }
    write_text("  result: graph fallback grouped heads and causal masking accepted\n");
    return 0U;
}

static u32 run_shared_kv(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    enum {
        CACHE_VALUES = 4U * 2048U * 256U,
        CACHE_BYTES = CACHE_VALUES * sizeof(u16),
        TOTAL_BYTES = CACHE_BYTES * 2U
    };
    u32 cache_dimensions[4] = {1U, 4U, 2048U, 256U};
    u32 scalar_dimensions[1] = {1U};
    u16 zero = 0U;
    void *rpcmem_module = 0;
    void *allocation = 0;
    GemmaRpcMemAlloc rpcmem_alloc = 0;
    GemmaRpcMemFree rpcmem_free = 0;
    GemmaRpcMemToFd rpcmem_to_fd = 0;
    QnnMemDescriptor descriptor = {0};
    QnnHtpMemDescriptor htp_descriptor = {0};
    QnnMemHandle handles[2] = {0, 0};
    QnnTensor inputs[2];
    QnnTensor output;
    QnnGraphHandle graph = 0;
    u16 *key_cache;
    u16 *value_cache;
    i32 fd;
    u64 status = 0U;
    u32 index;
    u32 result = 167U;

    write_text("TranslateGemma Stage 1 shared KV [1,4,2048,256] x2\n");
    if (api->mem_register == 0 || api->mem_deregister == 0) return result;
    rpcmem_module = LoadLibraryA("libcdsprpc.dll");
    if (rpcmem_module == 0) return result;
    rpcmem_alloc = (GemmaRpcMemAlloc)GetProcAddress(rpcmem_module, "rpcmem_alloc");
    rpcmem_free = (GemmaRpcMemFree)GetProcAddress(rpcmem_module, "rpcmem_free");
    rpcmem_to_fd = (GemmaRpcMemToFd)GetProcAddress(rpcmem_module, "rpcmem_to_fd");
    if (rpcmem_alloc == 0 || rpcmem_free == 0 || rpcmem_to_fd == 0) goto cleanup;
    allocation = rpcmem_alloc(25, 1U, TOTAL_BYTES);
    if (allocation == 0) goto cleanup;
    fd = rpcmem_to_fd(allocation);
    if (fd == -1) goto cleanup;
    key_cache = (u16 *)allocation;
    value_cache = key_cache + CACHE_VALUES;
    for (index = 0U; index < CACHE_VALUES; ++index) {
        key_cache[index] = (u16)(0x3000U + index % 0x0800U);
        value_cache[index] = 0U;
    }
    descriptor.shape.rank = 4U;
    descriptor.shape.dimensions = cache_dimensions;
    descriptor.data_type = QNN_DATATYPE_FLOAT_16;
    descriptor.memory_type = QNN_MEM_TYPE_CUSTOM;
    descriptor.memory.custom_info = &htp_descriptor;
    htp_descriptor.type = QNN_HTP_MEM_SHARED_BUFFER;
    htp_descriptor.size = TOTAL_BYTES;
    htp_descriptor.config.shared_buffer.fd = fd;
    htp_descriptor.config.shared_buffer.offset = 0U;
    status = api->mem_register(context, &descriptor, 1U, &handles[0]);
    write_status("  memRegister key", status);
    if (status != 0U || handles[0] == 0) goto cleanup;
    htp_descriptor.config.shared_buffer.offset = CACHE_BYTES;
    status = api->mem_register(context, &descriptor, 1U, &handles[1]);
    write_status("  memRegister value", status);
    if (status != 0U || handles[1] == 0) goto cleanup;
    status = api->graph_create(context, "gemma_shared_kv", 0, &graph);
    if (status != 0U) goto cleanup;
    inputs[0] = make_plain_tensor(
        "gemma_shared_key", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, cache_dimensions, 4U
    );
    inputs[1] = make_plain_tensor(
        "gemma_shared_zero", QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, scalar_dimensions, 1U
    );
    inputs[1].data.v1.memory.client_buffer.data = &zero;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(zero);
    output = make_plain_tensor(
        "gemma_shared_value", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_FLOAT_16, cache_dimensions, 4U
    );
    status = api->tensor_create_graph_tensor(graph, &inputs[0]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &inputs[1]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &output);
    if (status == 0U) {
        status = add_node(
            api, graph, "gemma_shared_kv_use", "ElementWiseAdd",
            inputs, 2U, &output, 1U, 0, 0U
        );
    }
    write_status("  graphAddNode ElementWiseAdd", status);
    if (status != 0U) goto cleanup;
    status = api->graph_finalize(graph, 0, 0);
    write_status("  graphFinalize", status);
    if (status != 0U) goto cleanup;
    inputs[0].data.v1.memory_type = QNN_TENSORMEMTYPE_MEMHANDLE;
    inputs[0].data.v1.memory.memory_handle = handles[0];
    output.data.v1.memory_type = QNN_TENSORMEMTYPE_MEMHANDLE;
    output.data.v1.memory.memory_handle = handles[1];
    status = api->graph_execute(graph, inputs, 1U, &output, 1U, 0, 0);
    write_status("  graphExecute shared KV", status);
    if (status != 0U) goto cleanup;
    for (index = 0U; index < CACHE_VALUES; ++index) {
        if (value_cache[index] != key_cache[index]) {
            write_text("  shared KV mismatch at element ");
            write_u32(index);
            write_text("\n");
            goto cleanup;
        }
    }
    write_text("  result: model-shaped shared KV registered, used, and exact\n");
    result = 0U;

cleanup:
    if (handles[1] != 0) (void)api->mem_deregister(&handles[1], 1U);
    if (handles[0] != 0) (void)api->mem_deregister(&handles[0], 1U);
    if (allocation != 0 && rpcmem_free != 0) rpcmem_free(allocation);
    if (rpcmem_module != 0) FreeLibrary(rpcmem_module);
    return result;
}

static u32 run_scaled_residual(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    u32 dimensions[2] = {3U, GEMMA_HIDDEN_WIDTH};
    u32 weight_dimensions[1] = {GEMMA_HIDDEN_WIDTH};
    u32 axes_dimensions[1] = {1U};
    u32 axes_data[1] = {1U};
    u16 left[3U * GEMMA_HIDDEN_WIDTH] = {0U};
    u16 right[3U * GEMMA_HIDDEN_WIDTH] = {0U};
    u16 sum[3U * GEMMA_HIDDEN_WIDTH] = {0U};
    u16 normalized[3U * GEMMA_HIDDEN_WIDTH] = {0U};
    u16 gain[GEMMA_HIDDEN_WIDTH];
    QnnTensor inputs[2];
    QnnTensor outputs[2];
    QnnTensor norm_inputs[2];
    QnnParam parameters[2] = {0};
    QnnGraphHandle graph = 0;
    u64 status;
    u32 index;

    write_text("TranslateGemma scaled FP16 residual and RMSNorm [3,2560], divisor 32\n");
    for (index = 0U; index < GEMMA_HIDDEN_WIDTH; ++index) {
        u16 sign = index % 2U ? 0x8000U : 0U;
        left[index] = (u16)(0x6400U | sign);
        right[index] = (u16)(0x6500U | sign);
        left[GEMMA_HIDDEN_WIDTH + index] = (u16)(0x0400U | sign);
        gain[index] = 0x3c00U;
    }
    status = api->graph_create(context, "gemma_scaled_residual", 0, &graph);
    if (status != 0U) return 220U;
    inputs[0] = make_plain_tensor("scaled_left", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, dimensions, 2U);
    inputs[1] = make_plain_tensor("scaled_right", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_16, dimensions, 2U);
    outputs[0] = make_plain_tensor("scaled_sum", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_FLOAT_16, dimensions, 2U);
    outputs[1] = make_plain_tensor("scaled_normalized", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_FLOAT_16, dimensions, 2U);
    norm_inputs[1] = make_plain_tensor("scaled_gain", QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_FLOAT_16, weight_dimensions, 1U);
    norm_inputs[1].data.v1.memory.client_buffer.data = gain;
    norm_inputs[1].data.v1.memory.client_buffer.data_size = sizeof(gain);
    parameters[0].type = QNN_PARAMTYPE_SCALAR;
    parameters[0].name = "epsilon";
    parameters[0].value.scalar.data_type = QNN_DATATYPE_FLOAT_32;
    parameters[0].value.scalar.value.float_value = 9.765625e-10f;
    parameters[1].type = QNN_PARAMTYPE_TENSOR;
    parameters[1].name = "axes";
    parameters[1].value.tensor = make_plain_tensor("scaled_axes", QNN_TENSOR_TYPE_STATIC,
        QNN_DATATYPE_UINT_32, axes_dimensions, 1U);
    parameters[1].value.tensor.data.v1.memory.client_buffer.data = axes_data;
    parameters[1].value.tensor.data.v1.memory.client_buffer.data_size = sizeof(axes_data);
    for (index = 0U; index < 2U && status == 0U; ++index) {
        status = api->tensor_create_graph_tensor(graph, &inputs[index]);
        if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[index]);
    }
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &norm_inputs[1]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &parameters[1].value.tensor);
    if (status == 0U) status = add_node(api, graph, "scaled_add", "ElementWiseAdd",
        inputs, 2U, &outputs[0], 1U, 0, 0U);
    norm_inputs[0] = outputs[0];
    if (status == 0U) status = add_node(api, graph, "scaled_norm", "RmsNorm",
        norm_inputs, 2U, &outputs[1], 1U, parameters, 2U);
    if (status == 0U) status = api->graph_finalize(graph, 0, 0);
    write_status("  graphFinalize", status);
    if (status != 0U) return 221U;
    inputs[0].data.v1.memory.client_buffer.data = left;
    inputs[0].data.v1.memory.client_buffer.data_size = sizeof(left);
    inputs[1].data.v1.memory.client_buffer.data = right;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(right);
    outputs[0].data.v1.memory.client_buffer.data = sum;
    outputs[0].data.v1.memory.client_buffer.data_size = sizeof(sum);
    outputs[1].data.v1.memory.client_buffer.data = normalized;
    outputs[1].data.v1.memory.client_buffer.data_size = sizeof(normalized);
    status = api->graph_execute(graph, inputs, 2U, outputs, 2U, 0, 0);
    write_status("  graphExecute", status);
    if (status != 0U) return 222U;
    for (index = 0U; index < 3U * GEMMA_HIDDEN_WIDTH; ++index) {
        float expected = half_to_float(left[index]) + half_to_float(right[index]);
        float denominator = index < GEMMA_HIDDEN_WIDTH ? 2304.0f :
            (index < 2U * GEMMA_HIDDEN_WIDTH ? 0.000068570111f : 0.00003125f);
        if (half_to_float(sum[index]) != expected ||
            !(absolute_float(half_to_float(normalized[index]) - expected / denominator) <= 0.003f)) {
            write_text("  scaled residual/epsilon mismatch at element ");
            write_u32(index);
            write_text("\n");
            return 223U;
        }
    }
    write_text("  result: scaled residual range and compensated RMSNorm epsilon accepted\n");
    return 0U;
}

static u32 run_residual_precision(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    u32 dimensions[2] = {1U, GEMMA_HIDDEN_WIDTH};
    float left[GEMMA_HIDDEN_WIDTH];
    float right[GEMMA_HIDDEN_WIDTH];
    float output[GEMMA_HIDDEN_WIDTH];
    QnnTensor inputs[2];
    QnnTensor outputs[1];
    QnnGraphHandle graph = 0;
    u64 status;
    u32 index;

    write_text("TranslateGemma FP32 residual range probe\n");
    status = api->graph_create(context, "gemma_residual_fp32", 0, &graph);
    if (status != 0U) return 210U;
    inputs[0] = make_plain_tensor("residual_left", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_32, dimensions, 2U);
    inputs[1] = make_plain_tensor("residual_right", QNN_TENSOR_TYPE_APP_WRITE,
        QNN_DATATYPE_FLOAT_32, dimensions, 2U);
    outputs[0] = make_plain_tensor("residual_sum", QNN_TENSOR_TYPE_APP_READ,
        QNN_DATATYPE_FLOAT_32, dimensions, 2U);
    status = api->tensor_create_graph_tensor(graph, &inputs[0]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &inputs[1]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[0]);
    if (status == 0U) status = add_node(api, graph, "residual_add", "ElementWiseAdd",
        inputs, 2U, outputs, 1U, 0, 0U);
    write_status("  graphAddNode FP32 add", status);
    if (status == 0U) status = api->graph_finalize(graph, 0, 0);
    write_status("  graphFinalize", status);
    if (status != 0U) {
        write_text("  result: FP32 residual unavailable; explicit scaling required\n");
        return 0U;
    }
    for (index = 0U; index < GEMMA_HIDDEN_WIDTH; ++index) {
        left[index] = index % 2U ? -32768.0f : 32768.0f;
        right[index] = index % 2U ? -40960.0f : 40960.0f;
        output[index] = 0.0f;
    }
    inputs[0].data.v1.memory.client_buffer.data = left;
    inputs[0].data.v1.memory.client_buffer.data_size = sizeof(left);
    inputs[1].data.v1.memory.client_buffer.data = right;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(right);
    outputs[0].data.v1.memory.client_buffer.data = output;
    outputs[0].data.v1.memory.client_buffer.data_size = sizeof(output);
    status = api->graph_execute(graph, inputs, 2U, outputs, 1U, 0, 0);
    write_status("  graphExecute", status);
    if (status != 0U) return 211U;
    for (index = 0U; index < GEMMA_HIDDEN_WIDTH; ++index) {
        if (output[index] != left[index] + right[index]) {
            write_text("  result: FP32 IO does not preserve residual range; explicit scaling required\n");
            return 0U;
        }
    }
    write_text("  result: FP32 add preserves +/-73728; normalization chain still requires validation\n");
    return 0U;
}

static void report_quantization_properties(const QnnInterfaceV2 *api) {
    typedef u64 (*GemmaHasCapability)(u32 key);
    static const u32 keys[] = {512U, 513U, 514U, 528U, 530U, 531U, 532U};
    static const char *const names[] = {
        "quantization property: bw-axis",
        "quantization property: block",
        "quantization property: blockwise-expansion",
        "quantization property: float-block",
        "quantization property: bw-block-mapped",
        "quantization property: bw-blockwise-expansion-mapped",
        "quantization property: bw-float-block"
    };
    GemmaHasCapability has_capability = (GemmaHasCapability)api->property_has_capability;
    u32 index;
    if (!has_capability) {
        write_text("quantization properties: capability API unavailable\n");
        return;
    }
    write_text("Quantization properties (zero means supported, not MatMul acceptance):\n");
    for (index = 0U; index < sizeof(keys) / sizeof(keys[0]); ++index) {
        write_status(names[index], has_capability(keys[index]));
    }
}

u32 qnn_gemma_stage1_probe(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    u32 result;
    report_quantization_properties(api);
    result = run_w4a16_projection(api, context, 0U);
#ifdef GEMMA_GROUP32_DIAGNOSTIC
    if (result == 0U) {
        u32 grouped_result = run_w4a16_projection(api, context, 1U);
        if (grouped_result >= 122U && grouped_result <= 124U) {
            write_text("  result: group-32 block-mapped graph unavailable; no deployment acceptance\n");
        } else {
            result = grouped_result;
        }
    }
#endif
    if (result == 0U) result = run_rms_norm(api, context);
    if (result == 0U) result = run_gated_gelu(api, context);
    if (result == 0U) result = run_gather(api, context);
    if (result == 0U) result = run_argmax_topk(api, context);
    if (result == 0U) result = run_rotary_embedding(api, context);
    if (result == 0U) result = run_group_query_attention(api, context);
    if (result == 0U) result = run_group_query_attention_composition(api, context);
    if (result == 0U) result = run_shared_kv(api, context);
    if (result == 0U) result = run_residual_precision(api, context);
    if (result == 0U) result = run_scaled_residual(api, context);
    return result;
}