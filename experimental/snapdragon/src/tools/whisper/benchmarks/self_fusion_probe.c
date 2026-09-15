#include "qnn_abi.h"
#include "../whisper_decoder_qnn.c"

__declspec(dllimport) void *LoadLibraryA(const char *name);
__declspec(dllimport) void *GetProcAddress(void *module, const char *name);
__declspec(dllimport) int FreeLibrary(void *module);
__declspec(dllimport) void *GetStdHandle(u32 handle_id);
__declspec(dllimport) int WriteFile(void *handle, const void *data, u32 size, u32 *written, void *overlapped);
__declspec(dllimport) void ExitProcess(u32 status);
__declspec(dllimport) int QueryPerformanceFrequency(long long *value);

enum { WIDTH = 1024, HEADS = 16, HEAD_WIDTH = 64, CONTEXT = 448, CACHE = WIDTH * CONTEXT };
enum { PROJECTION_QUERY = 8, PROJECTION_KEY = 11, PROJECTION_VALUE = 14 };
enum { ATTENTION_QUERY = 0, ATTENTION_KEYS = 1, ATTENTION_VALUES = 2, ATTENTION_MASK = 3 };
_Static_assert(DECODER_QNN_SELF_PROJECTION_TENSORS == 15, "Update probe tensor mapping when the builder changes");
_Static_assert(DECODER_QNN_SELF_ATTENTION_TENSORS == 13, "Update probe tensor mapping when the builder changes");
static u16 cache_input[CACHE];
static u16 cache_output[CACHE];
static u16 expected[CACHE];
static u16 updates[WIDTH];
static i32 indices[WIDTH];
static u16 *reference_keys;
static u16 *reference_values;
static u16 *fused_keys[2];
static u16 *fused_values[2];
static void *shared_allocation;
static void *rpc_module;
static DecoderRpcMemFree release_rpc;
static QnnMemHandle cache_handles[6];
static u32 registered_count;
static u64 sample_ticks[2][3][CONTEXT];
static u16 hidden[WIDTH];
static u16 mask[HEADS * CONTEXT];
static u16 reference_output[WIDTH];
static u16 fused_output[WIDTH];
static u16 query[WIDTH];
static float hidden_float[WIDTH];
static float output_float[WIDTH];
static u16 key[WIDTH];
static u16 value[WIDTH];
static const QnnInterfaceV2 *real_api;
static WhisperDecoderQnn *building;
static int fusion_enabled;
static u32 creation_count;
static u32 finalization_count;
static QnnGraphHandle fused_graph;
static QnnTensor fused_old_keys;
static QnnTensor fused_old_values;
static QnnTensor fused_key_indices;
static QnnTensor fused_value_indices;
static u32 key_shape[3] = {HEADS, HEAD_WIDTH, CONTEXT};
static u32 value_shape[3] = {HEADS, CONTEXT, HEAD_WIDTH};
static u32 key_update_shape[3] = {HEADS, HEAD_WIDTH, 1U};
static u32 value_update_shape[3] = {HEADS, 1U, HEAD_WIDTH};

static void text(const char *value) {
    u32 length = 0U;
    u32 written;
    while (value[length]) ++length;
    WriteFile(GetStdHandle(0xfffffff5U), value, length, &written, 0);
}

static int checked(const char *name, u64 status) {
    static const char digits[] = "0123456789abcdef";
    char encoded[17];
    u32 index;
    for (index = 0U; index < 16U; ++index) encoded[15U - index] = digits[(status >> (index * 4U)) & 15U];
    encoded[16] = 0;
    text(name);
    text(" status=0x");
    text(encoded);
    text("\n");
    return status == 0U;
}

static void number(u64 value) {
    char digits[21];
    u32 used = 20U;
    digits[used] = 0;
    do { digits[--used] = (char)('0' + value % 10U); value /= 10U; } while (value);
    text(digits + used);
}

static QnnTensor tensor(const char *name, u32 type, u32 datatype, u32 *shape, void *buffer, u32 bytes) {
    QnnTensor result = {0};
    result.version = QNN_TENSOR_VERSION_1;
    result.data.v1.name = name;
    result.data.v1.type = type;
    result.data.v1.data_type = datatype;
    result.data.v1.rank = 3U;
    result.data.v1.dimensions = shape;
    result.data.v1.quantize_params.encoding_definition = QNN_DEFINITION_UNDEFINED;
    result.data.v1.quantize_params.quantization_encoding = QNN_QUANTIZATION_ENCODING_UNDEFINED;
    result.data.v1.memory_type = QNN_TENSORMEMTYPE_RAW;
    if (type == QNN_TENSOR_TYPE_STATIC) {
        result.data.v1.memory.client_buffer.data = buffer;
        result.data.v1.memory.client_buffer.data_size = bytes;
    }
    return result;
}

static int __attribute__((noinline)) scatter(const QnnInterfaceV2 *api, QnnContextHandle context, int keys) {
    static const u32 positions[] = {0U, 1U, 127U, 447U, 0U};
    u32 cache_shape[3] = {HEADS, keys ? HEAD_WIDTH : CONTEXT, keys ? CONTEXT : HEAD_WIDTH};
    u32 update_shape[3] = {HEADS, keys ? HEAD_WIDTH : 1U, keys ? 1U : HEAD_WIDTH};
    QnnGraphHandle graph = 0;
    QnnTensor inputs[3];
    QnnTensor output;
    QnnParam parameters[2] = {0};
    QnnOpConfig operation = {0};
    u32 index;
    u32 step;
    text(keys ? "KEY cache ScatterElements\n" : "VALUE cache ScatterElements\n");
    if (!checked("graph_create", api->graph_create(context, keys ? "key_scatter" : "value_scatter", 0, &graph))) return 0;
    inputs[0] = tensor("cache", QNN_TENSOR_TYPE_APP_WRITE, QNN_DATATYPE_FLOAT_16, cache_shape, cache_input, sizeof(cache_input));
    inputs[1] = tensor("indices", QNN_TENSOR_TYPE_APP_WRITE, QNN_DATATYPE_INT_32, update_shape, indices, sizeof(indices));
    inputs[2] = tensor("updates", QNN_TENSOR_TYPE_APP_WRITE, QNN_DATATYPE_FLOAT_16, update_shape, updates, sizeof(updates));
    output = tensor("updated", QNN_TENSOR_TYPE_APP_READ, QNN_DATATYPE_FLOAT_16, cache_shape, cache_output, sizeof(cache_output));
    for (index = 0U; index < 3U; ++index) {
        if (!checked("input_tensor", api->tensor_create_graph_tensor(graph, &inputs[index]))) return 0;
    }
    if (!checked("output_tensor", api->tensor_create_graph_tensor(graph, &output))) return 0;
    parameters[0].type = QNN_PARAMTYPE_SCALAR;
    parameters[0].name = "axis";
    parameters[0].value.scalar.data_type = QNN_DATATYPE_UINT_32;
    parameters[0].value.scalar.value.uint32_value = keys ? 2U : 1U;
    parameters[1].type = QNN_PARAMTYPE_SCALAR;
    parameters[1].name = "reduction";
    parameters[1].value.scalar.data_type = QNN_DATATYPE_UINT_32;
    parameters[1].value.scalar.value.uint32_value = 0U;
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "insert";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "ScatterElements";
    operation.data.v1.parameter_count = 2U;
    operation.data.v1.parameters = parameters;
    operation.data.v1.input_count = 3U;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = &output;
    if (!checked("graph_add_node", api->graph_add_node(graph, operation))) return 0;
    if (!checked("graph_finalize", api->graph_finalize(graph, 0, 0))) return 0;
    inputs[0].data.v1.memory.client_buffer.data = cache_input;
    inputs[0].data.v1.memory.client_buffer.data_size = sizeof(cache_input);
    inputs[1].data.v1.memory.client_buffer.data = indices;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(indices);
    inputs[2].data.v1.memory.client_buffer.data = updates;
    inputs[2].data.v1.memory.client_buffer.data_size = sizeof(updates);
    output.data.v1.memory.client_buffer.data = cache_output;
    output.data.v1.memory.client_buffer.data_size = sizeof(cache_output);
    for (index = 0U; index < CACHE; ++index) cache_input[index] = expected[index] = (u16)(0x3000U + index % 1024U);
    for (step = 0U; step < sizeof(positions) / sizeof(positions[0]); ++step) {
        for (index = 0U; index < WIDTH; ++index) {
            u32 offset = keys ? index * CONTEXT + positions[step] :
                (index / HEAD_WIDTH * CONTEXT + positions[step]) * HEAD_WIDTH + index % HEAD_WIDTH;
            indices[index] = (i32)positions[step];
            updates[index] = (u16)(0x3800U + (index + step * 37U) % 1024U);
            expected[offset] = updates[index];
        }
        if (!checked("graph_execute", api->graph_execute(graph, inputs, 3U, &output, 1U, 0, 0))) return 0;
        for (index = 0U; index < CACHE; ++index) {
            if (cache_output[index] != expected[index]) {
                text("FAIL cache element mismatch\n");
                return 0;
            }
            cache_input[index] = cache_output[index];
        }
    }
    text("PASS dynamic cache insertion, boundaries, repeat and untouched values\n");
    return 1;
}

static int same_name(const char *left, const char *right) {
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

static u64 probe_node(QnnGraphHandle graph, const char *name, const char *type,
                     QnnTensor *inputs, u32 count, QnnTensor *output, QnnParam *parameter) {
    QnnOpConfig operation = {0};
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = name;
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = type;
    operation.data.v1.inputs = inputs;
    operation.data.v1.input_count = count;
    operation.data.v1.outputs = output;
    operation.data.v1.output_count = 1U;
    operation.data.v1.parameters = parameter;
    operation.data.v1.parameter_count = parameter ? 1U : 0U;
    return real_api->graph_add_node(graph, operation);
}

static u64 insert_cache_nodes(QnnGraphHandle graph) {
    QnnTensor key_update = tensor("key_update", QNN_TENSOR_TYPE_NATIVE, QNN_DATATYPE_FLOAT_16, key_update_shape, 0, 0);
    QnnTensor value_update = tensor("value_update", QNN_TENSOR_TYPE_NATIVE, QNN_DATATYPE_FLOAT_16, value_update_shape, 0, 0);
    QnnTensor inputs[3];
    QnnParam axis = {0};
    fused_old_keys = tensor("old_keys", QNN_TENSOR_TYPE_APP_WRITE, QNN_DATATYPE_FLOAT_16, key_shape, 0, 0);
    fused_old_values = tensor("old_values", QNN_TENSOR_TYPE_APP_WRITE, QNN_DATATYPE_FLOAT_16, value_shape, 0, 0);
    fused_key_indices = tensor("key_positions", QNN_TENSOR_TYPE_APP_WRITE, QNN_DATATYPE_INT_32, key_update_shape, 0, 0);
    fused_value_indices = tensor("value_positions", QNN_TENSOR_TYPE_APP_WRITE, QNN_DATATYPE_INT_32, value_update_shape, 0, 0);
    QnnTensor *tensors[] = {&key_update, &value_update, &fused_old_keys, &fused_old_values, &fused_key_indices, &fused_value_indices};
    for (u32 index = 0U; index < sizeof(tensors) / sizeof(tensors[0]); ++index) {
        u64 status = real_api->tensor_create_graph_tensor(graph, tensors[index]);
        if (status) return status;
    }
    u64 status = probe_node(graph, "key_reshape", "Reshape", &building->self_projection_build_tensors[PROJECTION_KEY], 1U, &key_update, 0);
    if (status) return status;
    status = probe_node(graph, "value_reshape", "Reshape", &building->self_projection_build_tensors[PROJECTION_VALUE], 1U, &value_update, 0);
    if (status) return status;
    axis.type = QNN_PARAMTYPE_SCALAR;
    axis.name = "axis";
    axis.value.scalar.data_type = QNN_DATATYPE_UINT_32;
    axis.value.scalar.value.uint32_value = 2U;
    inputs[0] = fused_old_keys; inputs[1] = fused_key_indices; inputs[2] = key_update;
    status = probe_node(graph, "key_insert", "ScatterElements", inputs, 3U, &building->self_attention_build_tensors[ATTENTION_KEYS], &axis);
    if (status) return status;
    axis.value.scalar.value.uint32_value = 1U;
    inputs[0] = fused_old_values; inputs[1] = fused_value_indices; inputs[2] = value_update;
    return probe_node(graph, "value_insert", "ScatterElements", inputs, 3U, &building->self_attention_build_tensors[ATTENTION_VALUES], &axis);
}

static u64 create_layer_graph(QnnContextHandle context, const char *name,
                             const QnnGraphConfig **config, QnnGraphHandle *graph) {
    building->model.decoder_layers = 1U;
    ++creation_count;
    if (fusion_enabled && creation_count == 2U) { *graph = fused_graph; return 0U; }
    u64 status = real_api->graph_create(context, (fusion_enabled || building->self_fusion) ? "fused_medium_layer_zero" : name, config, graph);
    if (fusion_enabled) fused_graph = *graph;
    return status;
}

static u64 create_layer_tensor(QnnGraphHandle graph, QnnTensor *item) {
    if (!fusion_enabled) return real_api->tensor_create_graph_tensor(graph, item);
    if (creation_count == 1U) {
        for (u32 index = 0U; index < 3U; ++index) {
            static const u32 outputs[] = {PROJECTION_QUERY, PROJECTION_KEY, PROJECTION_VALUE};
            if (same_name(item->data.v1.name, building->self_projection_tensor_names[0][outputs[index]]))
                item->data.v1.type = QNN_TENSOR_TYPE_NATIVE;
        }
    } else {
        if (same_name(item->data.v1.name, building->self_attention_tensor_names[0][ATTENTION_QUERY])) {
            *item = building->self_projection_build_tensors[PROJECTION_QUERY];
            return 0U;
        }
        if (same_name(item->data.v1.name, building->self_attention_tensor_names[0][ATTENTION_KEYS]) ||
            same_name(item->data.v1.name, building->self_attention_tensor_names[0][ATTENTION_VALUES])) item->data.v1.type = QNN_TENSOR_TYPE_APP_READ;
    }
    u64 status = real_api->tensor_create_graph_tensor(graph, item);
    if (!status && creation_count == 2U && same_name(item->data.v1.name, building->self_attention_tensor_names[0][ATTENTION_MASK]))
        status = insert_cache_nodes(graph);
    return status;
}

static u64 finalize_layer(QnnGraphHandle graph, QnnProfileHandle profile, void *signal) {
    ++finalization_count;
    if (fusion_enabled && finalization_count == 1U) return 0U;
    return real_api->graph_finalize(graph, profile, signal);
}

static void bind_raw(QnnTensor *item, void *buffer, u32 bytes) {
    item->data.v1.memory_type = QNN_TENSORMEMTYPE_RAW;
    item->data.v1.memory.client_buffer.data = buffer;
    item->data.v1.memory.client_buffer.data_size = bytes;
}

static void bind_shared(QnnTensor *item, u32 handle_index) {
    item->data.v1.memory_type = QNN_TENSORMEMTYPE_MEMHANDLE;
    item->data.v1.memory.memory_handle = cache_handles[handle_index];
}

static int allocate_shared(const QnnInterfaceV2 *api, QnnContextHandle context) {
    DecoderRpcMemAlloc allocate;
    DecoderRpcMemToFd to_fd;
    QnnMemDescriptor descriptor = {0};
    QnnHtpMemDescriptor htp = {0};
    rpc_module = LoadLibraryA("libcdsprpc.dll");
    if (!rpc_module) return 0;
    allocate = (DecoderRpcMemAlloc)GetProcAddress(rpc_module, "rpcmem_alloc");
    release_rpc = (DecoderRpcMemFree)GetProcAddress(rpc_module, "rpcmem_free");
    to_fd = (DecoderRpcMemToFd)GetProcAddress(rpc_module, "rpcmem_to_fd");
    if (!allocate || !release_rpc || !to_fd) return 0;
    shared_allocation = allocate(25, 1U, 6U * CACHE * sizeof(u16));
    if (!shared_allocation) return 0;
    htp.type = QNN_HTP_MEM_SHARED_BUFFER;
    htp.size = 6U * CACHE * sizeof(u16);
    htp.config.shared_buffer.fd = to_fd(shared_allocation);
    if (htp.config.shared_buffer.fd == -1) return 0;
    descriptor.shape.rank = 3U;
    descriptor.data_type = QNN_DATATYPE_FLOAT_16;
    descriptor.memory_type = QNN_MEM_TYPE_CUSTOM;
    descriptor.memory.custom_info = &htp;
    for (u32 index = 0U; index < 6U; ++index) {
        descriptor.shape.dimensions = (index & 1U) ? value_shape : key_shape;
        htp.config.shared_buffer.offset = (u64)index * CACHE * sizeof(u16);
        if (!checked("mem_register", api->mem_register(context, &descriptor, 1U, &cache_handles[index]))) return 0;
        ++registered_count;
    }
    reference_keys = shared_allocation;
    reference_values = reference_keys + CACHE;
    fused_keys[0] = reference_values + CACHE;
    fused_values[0] = fused_keys[0] + CACHE;
    fused_keys[1] = fused_values[0] + CACHE;
    fused_values[1] = fused_keys[1] + CACHE;
    for (u32 index = 0U; index < 6U * CACHE; ++index) ((u16 *)shared_allocation)[index] = 0U;
    return 1;
}

static void timing_report(u64 frequency) {
    text("variant,round,samples,mean_us,median_us,p95_us,max_us\n");
    for (u32 variant = 0U; variant < 2U; ++variant) {
        for (u32 round = 0U; round < 3U; ++round) {
            u64 *samples = sample_ticks[variant][round] + 16U;
            u64 total = 0U;
            const u32 count = CONTEXT - 16U;
            for (u32 index = 0U; index < count; ++index) {
                u64 sample = samples[index];
                u32 insertion = index;
                total += sample;
                while (insertion && samples[insertion - 1U] > sample) {
                    samples[insertion] = samples[insertion - 1U]; --insertion;
                }
                samples[insertion] = sample;
            }
            text(variant ? "fused," : "split,"); number(round + 1U); text(","); number(count);
            text(","); number(total * 1000000U / frequency / count);
            text(","); number((samples[count / 2U - 1U] + samples[count / 2U]) * 500000U / frequency);
            text(","); number(samples[(count * 95U + 99U) / 100U - 1U] * 1000000U / frequency);
            text(","); number(samples[count - 1U] * 1000000U / frequency); text("\n");
        }
    }
}

static u64 retrieve_layer_graph(QnnContextHandle context, const char *name, QnnGraphHandle *graph) {
    (void)context;
    if (!same_name(name, building->graph_name) && !same_name(name, building->self_attention_graph_names[0])) return 1U;
    *graph = fused_graph;
    return 0U;
}

static int __attribute__((noinline)) compare_layer(const QnnInterfaceV2 *api, QnnContextHandle context) {
    static const u32 positions[] = {0U, 1U, 2U, 7U, 127U, 447U, 0U, 1U};
    WhisperDecoderQnn *baseline = whisper_decoder_qnn_create(whisper_model_medium(), DECODER_OFFLOAD_SELF);
    WhisperDecoderQnn *candidate = whisper_decoder_qnn_create(whisper_model_medium(), DECODER_OFFLOAD_SELF);
    static QnnInterfaceV2 builder_api;
    static QnnInterfaceV2 execution_api;
    static WhisperDecoderQnnIds ids;
    builder_api = *api;
    execution_api = *api;
    long long frequency;
    int result = 0;
    if (!baseline || !candidate || !QueryPerformanceFrequency(&frequency)) goto cleanup;
    real_api = api;
    builder_api.graph_create = create_layer_graph;
    builder_api.tensor_create_graph_tensor = create_layer_tensor;
    builder_api.graph_finalize = finalize_layer;
    building = baseline; fusion_enabled = 0; creation_count = finalization_count = 0U;
    if (decoder_qnn_build_self_attention(baseline, &builder_api, context, 0) != 1) { text("FAIL split layer build\n"); goto cleanup; }
    candidate->self_fusion = 1;
    building = candidate; fusion_enabled = 0; creation_count = finalization_count = 0U;
    if (decoder_qnn_build_self_attention(candidate, &builder_api, context, &ids) != 1) { text("FAIL fused layer build\n"); goto cleanup; }
    fused_graph = candidate->self_attention_graphs[0];
    candidate->output_count = 2U;
    ids.model_id = candidate->model.model_id;
    ids.output_count = candidate->output_count;
    execution_api.graph_retrieve = retrieve_layer_graph;
    if (!whisper_decoder_qnn_restore(candidate, &execution_api, context, &ids)) { text("FAIL fused tensor restore\n"); goto cleanup; }
    fused_old_keys = candidate->self_cache_inputs[0][0];
    fused_old_values = candidate->self_cache_inputs[0][1];
    fused_key_indices = candidate->self_cache_inputs[0][2];
    fused_value_indices = candidate->self_cache_inputs[0][3];
    text("PASS split and fused Medium layer-zero graph builds\n");
    if (!allocate_shared(api, context)) { text("FAIL shared cache registration\n"); goto cleanup; }
    candidate->shared_cache_ready = 1;
    for (u32 index = 2U; index < 6U; ++index) candidate->cache_handles[index] = cache_handles[index];
    for (u32 step = 0U; step < 3U * CONTEXT + sizeof(positions) / sizeof(positions[0]); ++step) {
        u32 position = step < 3U * CONTEXT ? step % CONTEXT : positions[step - 3U * CONTEXT];
        u32 source = step & 1U;
        u32 destination = source ^ 1U;
        QnnTensor projection_input = baseline->self_projection_inputs[0];
        QnnTensor projection_outputs[] = {baseline->self_query_outputs[0], baseline->self_key_outputs[0], baseline->self_value_outputs[0]};
        QnnTensor attention_inputs[] = {baseline->self_query_inputs[0], baseline->self_key_inputs[0], baseline->self_value_inputs[0], baseline->self_mask_inputs[0]};
        QnnTensor attention_output = baseline->self_outputs[0];
        QnnTensor fused_inputs[] = {candidate->self_projection_inputs[0], fused_old_keys, fused_old_values, fused_key_indices, fused_value_indices, candidate->self_mask_inputs[0]};
        QnnTensor fused_outputs[] = {candidate->self_outputs[0], candidate->self_key_inputs[0], candidate->self_value_inputs[0]};
        for (u32 index = 0U; index < WIDTH; ++index) {
            hidden[index] = whisper_frontend_float_to_half((float)((i32)((index * 17U + step * 23U) % 257U) - 128) / 128.0f);
            hidden_float[index] = whisper_frontend_half_to_float(hidden[index]);
            indices[index] = (i32)position;
        }
        for (u32 index = 0U; index < HEADS * CONTEXT; ++index) mask[index] = index % CONTEXT <= position ? 0U : 0xfbffU;
        bind_raw(&projection_input, hidden, sizeof(hidden));
        bind_raw(&projection_outputs[0], query, sizeof(query));
        bind_raw(&projection_outputs[1], key, sizeof(key));
        bind_raw(&projection_outputs[2], value, sizeof(value));
        bind_raw(&attention_inputs[0], query, sizeof(query));
        bind_shared(&attention_inputs[1], 0U);
        bind_shared(&attention_inputs[2], 1U);
        bind_raw(&attention_inputs[3], mask, sizeof(mask));
        bind_raw(&attention_output, reference_output, sizeof(reference_output));
        bind_raw(&fused_inputs[0], hidden, sizeof(hidden));
        bind_shared(&fused_inputs[1], 2U + source * 2U);
        bind_shared(&fused_inputs[2], 3U + source * 2U);
        bind_raw(&fused_inputs[3], indices, sizeof(indices));
        bind_raw(&fused_inputs[4], indices, sizeof(indices));
        bind_raw(&fused_inputs[5], mask, sizeof(mask));
        bind_raw(&fused_outputs[0], fused_output, sizeof(fused_output));
        bind_shared(&fused_outputs[1], 2U + destination * 2U);
        bind_shared(&fused_outputs[2], 3U + destination * 2U);
        for (u32 order = 0U; order < 2U; ++order) {
            u32 variant = order ^ (step & 1U);
            long long start;
            long long end;
            u64 status;
            QueryPerformanceCounter(&start);
            if (variant) {
                status = whisper_decoder_qnn_self_attention_offload(candidate, 0U, position,
                    hidden_float, output_float, 0) ? 0U : 1U;
            } else {
                status = api->graph_execute(baseline->self_projection_graphs[0], &projection_input, 1U, projection_outputs, 3U, 0, 0);
                if (!status) {
                    for (u32 index = 0U; index < WIDTH; ++index) {
                        reference_keys[index * CONTEXT + position] = key[index];
                        reference_values[(index / HEAD_WIDTH * CONTEXT + position) * HEAD_WIDTH + index % HEAD_WIDTH] = value[index];
                    }
                    status = api->graph_execute(baseline->self_attention_graphs[0], attention_inputs, 4U, &attention_output, 1U, 0, 0);
                }
            }
            QueryPerformanceCounter(&end);
            if (status) { checked(variant ? "fused_execute" : "split_execute", status); goto cleanup; }
            if (step < 3U * CONTEXT) sample_ticks[variant][step / CONTEXT][position] = (u64)(end - start);
        }
        for (u32 index = 0U; index < CACHE; ++index) {
            if (reference_keys[index] != fused_keys[destination][index] || reference_values[index] != fused_values[destination][index]) {
                text("FAIL split/fused KV mismatch\n"); goto cleanup;
            }
        }
        for (u32 index = 0U; index < WIDTH; ++index) {
            fused_output[index] = whisper_frontend_float_to_half(output_float[index]);
            if (reference_output[index] != fused_output[index]) { text("FAIL split/fused attention output mismatch\n"); goto cleanup; }
        }
        if (candidate->self_cache_bank[0] != destination) { text("FAIL cache bank advance\n"); goto cleanup; }
    }
    text("PASS 1352 split/fused shared-cache cases: bit-exact KV and attention output\n");
    text("PASS restored tensor IDs and production callback bank transitions\n");
    timing_report((u64)frequency);
    result = 1;
cleanup:
    if (candidate) {
        for (u32 index = 2U; index < 6U; ++index) candidate->cache_handles[index] = 0;
    }
    if (registered_count) {
        if (!checked("mem_deregister", api->mem_deregister(cache_handles, registered_count))) result = 0;
        registered_count = 0U;
    }
    whisper_decoder_qnn_shutdown(candidate);
    whisper_decoder_qnn_shutdown(baseline);
    return result;
}

void mainCRTStartup(void) {
    void *module = LoadLibraryA("QnnHtp.dll");
    QnnInterfaceGetProviders get_providers;
    const QnnInterfacePrefix **providers = 0;
    const QnnInterfaceV2 *api = 0;
    QnnBackendHandle backend = 0;
    QnnDeviceHandle device = 0;
    QnnContextHandle context = 0;
    u32 count = 0U;
    u32 index;
    u32 result = 1U;
    if (!module) { text("FAIL load QnnHtp.dll\n"); ExitProcess(1U); }
    get_providers = (QnnInterfaceGetProviders)GetProcAddress(module, "QnnInterface_getProviders");
    if (!get_providers || !checked("providers", get_providers(&providers, &count)) || !providers || !count) goto cleanup;
    for (index = 0U; index < count; ++index) {
        if (providers[index]->api_version.core_api_version.major == 2U &&
            providers[index]->api_version.core_api_version.minor >= 39U) {
            api = &((const QnnInterfaceProviderV2 *)providers[index])->api;
            break;
        }
    }
    if (!api || !api->backend_create || !api->backend_free || !api->device_create || !api->device_free ||
        !api->context_create || !api->context_free || !api->graph_create || !api->graph_add_node ||
        !api->tensor_create_graph_tensor || !api->graph_finalize || !api->graph_execute ||
        !api->mem_register || !api->mem_deregister) goto cleanup;
    if (!checked("backend_create", api->backend_create(0, 0, &backend))) goto cleanup;
    if (!checked("device_create", api->device_create(0, 0, &device))) goto cleanup;
    if (!checked("context_create", api->context_create(backend, device, 0, &context))) goto cleanup;
    if (!scatter(api, context, 1) || !scatter(api, context, 0)) goto cleanup;
    if (!compare_layer(api, context)) goto cleanup;
    result = 0U;
cleanup:
    if (context && !checked("context_free", api->context_free(context, 0))) result = 1U;
    if (shared_allocation) { release_rpc(shared_allocation); shared_allocation = 0; }
    if (rpc_module) { FreeLibrary(rpc_module); rpc_module = 0; }
    if (device && !checked("device_free", api->device_free(device))) result = 1U;
    if (backend && !checked("backend_free", api->backend_free(backend))) result = 1U;
    FreeLibrary(module);
    ExitProcess(result);
}