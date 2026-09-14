#include "qnn_abi.h"

static char log_storage __attribute__((used));
static char backend_storage __attribute__((used));
static char device_storage __attribute__((used));
static char profile_storage __attribute__((used));
static char context_storage __attribute__((used));
static char graph_storage __attribute__((used));
static u32 next_tensor_id __attribute__((used));
static u8 *static_input_data __attribute__((used));
static u32 static_input_data_type __attribute__((used));

static i32 __attribute__((used)) half_integer(u16 value) {
    u32 magnitude = value & 0x7fffU;
    i32 mantissa = (i32)(1024U + (magnitude & 1023U));
    i32 shift = (i32)((magnitude >> 10U) & 31U) - 25;
    i32 result = shift >= 0 ? mantissa << shift : mantissa >> -shift;
    return (value & 0x8000U) != 0U ? -result : result;
}

static u16 __attribute__((used)) integer_half(u32 value) {
    u32 exponent = 0U;
    u32 normalized = value;
    if (value == 0U) return 0U;
    while (normalized >= 2U) {
        normalized >>= 1U;
        ++exponent;
    }
    return (u16)(((exponent + 15U) << 10U) + ((value - (1U << exponent)) << (10U - exponent)));
}

static u64 __attribute__((used)) log_create(QnnLogCallback callback, u32 level, QnnHandle *handle) {
    (void)callback;
    if (level < QNN_LOG_LEVEL_ERROR || level > QNN_LOG_LEVEL_DEBUG) return 0x1003U;
    *handle = &log_storage;
    return 0U;
}

static u64 __attribute__((used)) log_free(QnnHandle handle) {
    (void)handle;
    return 0U;
}

static u64 __attribute__((used)) backend_create(QnnLogHandle log, const QnnBackendConfig **config, QnnBackendHandle *handle) {
    (void)log;
    (void)config;
    *handle = &backend_storage;
    return 0U;
}

static u64 __attribute__((used)) backend_free(QnnBackendHandle handle) {
    (void)handle;
    return 0U;
}

static u64 __attribute__((used)) device_create(QnnLogHandle log, const QnnDeviceConfig **config, QnnDeviceHandle *handle) {
    (void)log;
    (void)config;
    *handle = &device_storage;
    return 0U;
}

static u64 __attribute__((used)) device_free(QnnDeviceHandle handle) {
    (void)handle;
    return 0U;
}

static u64 __attribute__((used)) profile_create(QnnBackendHandle backend, u32 level, QnnProfileHandle *handle) {
    (void)backend;
    (void)level;
    *handle = &profile_storage;
    return 0U;
}

static u64 __attribute__((used)) profile_free(QnnProfileHandle handle) {
    (void)handle;
    return 0U;
}

static u64 __attribute__((used)) context_create(QnnBackendHandle backend, QnnDeviceHandle device, const QnnContextConfig **config, QnnContextHandle *handle) {
    (void)backend;
    (void)device;
    (void)config;
#if defined(QNN_MOCK_CONTEXT_FAILURE)
    (void)handle;
    return 0x1234U;
#else
    *handle = &context_storage;
    return 0U;
#endif
}

static u64 __attribute__((used)) context_free(QnnContextHandle context, QnnProfileHandle profile) {
    (void)context;
    (void)profile;
    return 0U;
}

static u64 __attribute__((used)) graph_create(
    QnnContextHandle context,
    const char *name,
    const QnnGraphConfig **config,
    QnnGraphHandle *handle
) {
    (void)context;
    (void)name;
    (void)config;
    static_input_data = 0;
    static_input_data_type = 0U;
#if defined(QNN_MOCK_GRAPH_CREATE_FAILURE)
    (void)handle;
    return 0x2001U;
#else
    *handle = &graph_storage;
    return 0U;
#endif
}

static u64 __attribute__((used)) tensor_create_graph_tensor(QnnGraphHandle graph, QnnTensor *tensor) {
    (void)graph;
#if defined(QNN_MOCK_TENSOR_FAILURE)
    (void)tensor;
    return 0x2002U;
#else
    tensor->data.v1.id = ++next_tensor_id;
    return 0U;
#endif
}

static u64 __attribute__((used)) graph_add_node(QnnGraphHandle graph, QnnOpConfig operation) {
    (void)graph;
#if defined(QNN_MOCK_ADD_NODE_FAILURE)
    (void)operation;
    return 0x2003U;
#else
    if (operation.data.v1.input_count == 1U &&
        operation.data.v1.inputs[0].data.v1.type == QNN_TENSOR_TYPE_STATIC &&
        operation.data.v1.outputs[0].data.v1.data_type == QNN_DATATYPE_SFIXED_POINT_16) {
        static_input_data = operation.data.v1.inputs[0].data.v1.memory.client_buffer.data;
        static_input_data_type = QNN_DATATYPE_UFIXED_POINT_16;
    }
    if (operation.data.v1.input_count >= 2U &&
        operation.data.v1.inputs[1].data.v1.type == QNN_TENSOR_TYPE_STATIC) {
        static_input_data = operation.data.v1.inputs[1].data.v1.memory.client_buffer.data;
        static_input_data_type = operation.data.v1.inputs[1].data.v1.data_type;
    }
    return 0U;
#endif
}

static u64 __attribute__((used)) graph_finalize(QnnGraphHandle graph, QnnProfileHandle profile, QnnHandle signal) {
    (void)graph;
    (void)profile;
    (void)signal;
#if defined(QNN_MOCK_FINALIZE_FAILURE)
    return 0x2004U;
#else
    return 0U;
#endif
}

static u64 __attribute__((used)) graph_execute(
    QnnGraphHandle graph,
    const QnnTensor *inputs,
    u32 input_count,
    QnnTensor *outputs,
    u32 output_count,
    QnnProfileHandle profile,
    QnnHandle signal
) {
    (void)graph;
    (void)profile;
    (void)signal;
#if defined(QNN_MOCK_EXECUTE_FAILURE)
    (void)inputs;
    (void)input_count;
    (void)outputs;
    (void)output_count;
    return 0x2005U;
#else
    u8 *input_a;
    u8 *output;
    u32 index;
    if (output_count != 1U) return 0x2006U;
    input_a = (u8 *)inputs[0].data.v1.memory.client_buffer.data;
    output = (u8 *)outputs[0].data.v1.memory.client_buffer.data;
    if (input_count == 2U) {
        if (outputs[0].data.v1.data_type == QNN_DATATYPE_UFIXED_POINT_16) {
            u16 *input_a_16 = (u16 *)input_a;
            u16 *input_b_16 = (u16 *)inputs[1].data.v1.memory.client_buffer.data;
            u16 *output_16 = (u16 *)output;
            for (index = 0U;
                 index < outputs[0].data.v1.memory.client_buffer.data_size / sizeof(u16);
                 ++index) {
                output_16[index] = (u16)(input_a_16[index] + input_b_16[index]);
            }
        } else {
            u8 *input_b = (u8 *)inputs[1].data.v1.memory.client_buffer.data;
            for (index = 0U; index < outputs[0].data.v1.memory.client_buffer.data_size; ++index) {
                output[index] = (u8)(input_a[index] + input_b[index]);
            }
        }
    } else if (input_count == 1U && inputs[0].data.v1.rank == 2U &&
               outputs[0].data.v1.rank == 3U) {
        u32 head_count = outputs[0].data.v1.dimensions[0];
        u32 sequence_length = outputs[0].data.v1.dimensions[1];
        u32 head_width = outputs[0].data.v1.dimensions[2];
        u32 head;
        u32 sequence;
        u32 lane;
        for (head = 0U; head < head_count; ++head) {
            for (sequence = 0U; sequence < sequence_length; ++sequence) {
                for (lane = 0U; lane < head_width; ++lane) {
                    output[(head * sequence_length + sequence) * head_width + lane] =
                        input_a[(sequence * head_count + head) * head_width + lane];
                }
            }
        }
    } else if (input_count == 1U && inputs[0].data.v1.data_type == QNN_DATATYPE_UFIXED_POINT_16 &&
               (static_input_data_type == QNN_DATATYPE_SFIXED_POINT_16 ||
                static_input_data_type == QNN_DATATYPE_UFIXED_POINT_16)) {
        u16 *input_16 = (u16 *)input_a;
        u16 *weights = (u16 *)static_input_data;
        u16 *output_16 = (u16 *)output;
        u32 batch_size = inputs[0].data.v1.dimensions[0];
        u32 inner_size = inputs[0].data.v1.dimensions[1];
        u32 output_size = outputs[0].data.v1.dimensions[1];
        u32 batch;
        for (batch = 0U; batch < batch_size; ++batch) {
            u32 column;
            for (column = 0U; column < output_size; ++column) {
                i32 sum = 0;
                u32 row;
                for (row = 0U; row < inner_size; ++row) {
                    i32 weight = static_input_data_type == QNN_DATATYPE_UFIXED_POINT_16
                        ? (i32)weights[column * inner_size + row] - 32768
                        : ((i16 *)weights)[column * inner_size + row];
                    sum += (i32)input_16[batch * inner_size + row] * weight;
                }
                output_16[batch * output_size + column] = (u16)sum;
            }
        }
    } else if (input_count == 1U && inputs[0].data.v1.data_type == QNN_DATATYPE_FLOAT_16 &&
               static_input_data_type == QNN_DATATYPE_FLOAT_16) {
        u16 *input_16 = (u16 *)input_a;
        u16 *weights = (u16 *)static_input_data;
        u16 *output_16 = (u16 *)output;
        u32 inner_size = inputs[0].data.v1.dimensions[1];
        u32 output_size = outputs[0].data.v1.dimensions[1];
        u32 column;
        for (column = 0U; column < output_size; ++column) {
            i32 sum = 0;
            u32 row;
            for (row = 0U; row < inner_size; ++row) {
                sum += half_integer(input_16[row]) * half_integer(weights[column * inner_size + row]);
            }
            output_16[column] = integer_half((u32)sum);
        }
    } else if (input_count == 1U && static_input_data != 0) {
        u8 *weights = static_input_data;
        u32 batch_size = inputs[0].data.v1.dimensions[0];
        u32 inner_size = inputs[0].data.v1.dimensions[1];
        u32 output_size = outputs[0].data.v1.dimensions[1];
        u32 fixture_period = inner_size > 384U ? 8U : 4U;
        u32 pattern_count = batch_size < fixture_period ? batch_size : fixture_period;
        u32 batch;
        for (batch = 0U; batch < pattern_count; ++batch) {
            u32 column;
            for (column = 0U; column < output_size; ++column) {
                u32 row;
                u32 sum = 0U;
                for (row = 0U; row < inner_size; ++row) {
                    sum += (u32)input_a[batch * inner_size + row] *
                           (u32)weights[row * output_size + column];
                }
                output[batch * output_size + column] = (u8)sum;
            }
        }
        for (batch = pattern_count; batch < batch_size; ++batch) {
            u32 column;
            for (column = 0U; column < output_size; ++column) {
                output[batch * output_size + column] =
                    output[(batch % fixture_period) * output_size + column];
            }
        }
    } else {
        return 0x2006U;
    }
#if defined(QNN_MOCK_BAD_OUTPUT)
    output[0] += 1U;
#endif
    return 0U;
#endif
}

#if defined(QNN_MOCK_API_MISMATCH)
static const QnnInterfaceProviderV2 provider = {
    {6U, "MOCK_QNN", {{3U, 0U, 0U}, {1U, 0U, 0U}}}, {0}
};
#elif defined(QNN_MOCK_MISSING_LIFECYCLE)
static const QnnInterfaceProviderV2 provider = {
    {6U, "MOCK_QNN", {{QNN_CORE_API_VERSION_MAJOR, QNN_CORE_API_VERSION_MINOR,
        QNN_CORE_API_VERSION_PATCH}, {5U, 41U, 0U}}}, {0}
};
#else
static const QnnInterfaceProviderV2 provider = {
    {6U, "MOCK_QNN", {{QNN_CORE_API_VERSION_MAJOR, QNN_CORE_API_VERSION_MINOR,
        QNN_CORE_API_VERSION_PATCH}, {5U, 41U, 0U}}},
    {
        .backend_create = backend_create,
        .backend_free = backend_free,
        .context_create = context_create,
        .context_free = context_free,
        .graph_create = graph_create,
        .graph_add_node = graph_add_node,
        .graph_finalize = graph_finalize,
        .graph_execute = graph_execute,
        .tensor_create_graph_tensor = tensor_create_graph_tensor,
        .log_create = log_create,
        .log_free = log_free,
        .profile_create = profile_create,
        .profile_free = profile_free,
        .device_create = device_create,
        .device_free = device_free
    }
};
#endif

static const QnnInterfaceProviderV2 *providers[] __attribute__((used)) = {&provider};

int DllMainCRTStartup(void *module, u32 reason, void *reserved) {
    (void)module;
    (void)reason;
    (void)reserved;
    return 1;
}

#if !defined(QNN_MOCK_NO_PROVIDER_EXPORT)
__declspec(dllexport) u64 QnnInterface_getProviders(
    const QnnInterfaceProviderV2 ***provider_list,
    u32 *provider_count
) {
    *provider_list = providers;
    *provider_count = 1U;
    return 0U;
}
#endif

__declspec(dllexport) u32 QnnMockUnrelatedExport(void) {
    return 0U;
}
