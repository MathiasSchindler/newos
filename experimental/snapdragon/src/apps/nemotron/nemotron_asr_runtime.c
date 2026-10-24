#include "nemotron_asr_runtime.h"

static QnnTensor *binding(NmAsrGraph *graph, const char *name, int output) {
    QnnTensor *values = output ? graph->outputs : graph->inputs;
    u32 count = output ? graph->no : graph->ni, index;
    for (index = 0; index < count; ++index)
        if (!rt_strcmp(values[index].data.v1.name, name)) return values + index;
    nm_text("Missing model binding: "); nm_text(name); nm_text("\n"); return 0;
}
int nm_asr_binding_copy(QnnTensor *destination, const QnnTensor *source) {
    const QnnTensorV1 *from; QnnTensorV1 *to;
    u32 axis;
    u64 start, end, bytes;
    if (!destination || !source) return 0;
    from = &source->data.v1; to = &destination->data.v1;
    if (to->rank != from->rank || to->data_type != from->data_type ||
        to->memory.client_buffer.data_size != from->memory.client_buffer.data_size ||
        !to->memory.client_buffer.data || !from->memory.client_buffer.data)
        return nm_error("model cache dtype/size/alias mismatch");
    start = (u64)to->memory.client_buffer.data; end = (u64)from->memory.client_buffer.data;
    bytes = to->memory.client_buffer.data_size;
    if (!bytes || start > ~(u64)0 - bytes || end > ~(u64)0 - bytes ||
        (start < end + bytes && end < start + bytes)) return nm_error("model cache buffers overlap");
    for (axis = 0; axis < to->rank; ++axis)
        if (to->dimensions[axis] != from->dimensions[axis]) return nm_error("model cache shape mismatch");
    memcpy(to->memory.client_buffer.data, from->memory.client_buffer.data, to->memory.client_buffer.data_size);
    return 1;
}
static int execute(NmAsrModel *model, u32 index) {
    NmAsrGraph *graph = model->graphs + index;
    u64 invoke = 0, copies = 0;
    if (!nm_rpc_execute(index, graph->inputs, graph->ni, graph->outputs, graph->no, &invoke, &copies, 1)) return 0;
    ++model->graph_calls; model->invoke_ticks += invoke; model->copy_ticks += copies; return 1;
}
static int make_bindings(NmAsrGraph *graph, int shared) {
    u32 index, ni = 0, no = 0;
    for (index = 0; index < graph->artifact.tensors; ++index) {
        u32 type = nm_u32(nm_tensor(&graph->artifact, index) + 64);
        if (type == QNN_TENSOR_TYPE_APP_WRITE) ++graph->ni;
        if (type == QNN_TENSOR_TYPE_APP_READ) ++graph->no;
    }
    if (!graph->ni || !graph->no) return nm_error("model graph requires inputs and outputs");
    graph->inputs = nm_alloc((u64)graph->ni * sizeof(*graph->inputs));
    graph->outputs = nm_alloc((u64)graph->no * sizeof(*graph->outputs));
    if (!graph->inputs || !graph->outputs) return 0;
    for (index = 0; index < graph->artifact.tensors; ++index) {
        const u8 *tensor = nm_tensor(&graph->artifact, index); u32 type = nm_u32(tensor + 64);
        QnnTensor *target; void *buffer;
        if (type != QNN_TENSOR_TYPE_APP_WRITE && type != QNN_TENSOR_TYPE_APP_READ) continue;
        target = type == QNN_TENSOR_TYPE_APP_WRITE ? graph->inputs + ni++ : graph->outputs + no++;
        buffer = shared ? 0 : nm_alloc(nm_tensor_bytes(tensor));
        if (!shared && !buffer) return 0;
        nm_tensor_binding(target, tensor, buffer, graph->artifact.payload);
        if (shared) target->data.v1.memory.client_buffer.data_size = (u32)nm_tensor_bytes(tensor);
    }
    return 1;
}
static void hex_hash(const u8 *data, char output[65]) {
    static const char digits[] = "0123456789abcdef"; u32 index;
    for (index = 0; index < 32; ++index) {
        output[index * 2] = digits[data[index] >> 4]; output[index * 2 + 1] = digits[data[index] & 15];
    }
    output[64] = 0;
}
int nm_asr_model_open(NmAsrModel *model, const NmRuntime *runtime, int diagnostics, int shared_io) {
    u32 role; char module[4096], catalog[4096], driver[4096], module_sha[65], driver_sha[65];
    memset(model, 0, sizeof(*model));
    model->shared_io = shared_io;
    model->fused_decoder = !memcmp(runtime->artifact.payload + 64, "NMRT3204", 8);
    for (role = 1; role <= NM_RUNTIME_FILES; ++role) {
        if (role >= 6 && role <= 10) {
            NmAsrGraph *graph = model->graphs + role - 6;
            graph->data = nm_runtime_read(runtime, role, &graph->bytes);
            if (!graph->data) goto fail;
            if (!nm_validate(&graph->artifact, graph->data, graph->bytes) ||
                !nm_runtime_graph_contract(&graph->artifact, role)) {
                nm_error(graph->artifact.error); goto fail;
            }
            if (!make_bindings(graph, shared_io)) goto fail;
            if (role == 9 && model->fused_decoder &&
                (!binding(graph, "encoder", 0) || !binding(graph, "logits", 1))) goto fail;
        } else if (!nm_runtime_verify(runtime, role)) goto fail;
    }
    if (!nm_runtime_path(runtime, 11, module) || !nm_runtime_path(runtime, 12, catalog) ||
        !nm_runtime_path(runtime, 15, driver) ||
        !nm_module_stage(&model->module_stage, module, catalog,
                         nm_runtime_entry(&runtime->artifact, 10) + 272,
                         nm_runtime_entry(&runtime->artifact, 11) + 272)) goto fail;
    hex_hash(nm_runtime_entry(&runtime->artifact, 10) + 272, module_sha);
    hex_hash(nm_runtime_entry(&runtime->artifact, 14) + 272, driver_sha);
    if (!nm_environment_set("NM_FP32_DSP_PACKAGE", model->module_stage.module) ||
        !nm_environment_set("NM_FP32_DSP_SHA256", module_sha) ||
        !nm_environment_set("NM_FP32_RPC_DRIVER", driver) ||
        !nm_environment_set("NM_FP32_RPC_SHA256", driver_sha) ||
        !nm_environment_set("NM_FP32_DSP_WORKERS", "4") ||
        !nm_environment_set("NM_FP32_DSP_POWER_PROFILE", "baseline") ||
        !nm_environment_set("NM_FP32_RPC_ALIGN_COEFFICIENTS", "1")) goto fail;
    nm_rpc_diagnostics(diagnostics); model->rpc_started = 1;
    if (!nm_rpc_initialize()) goto fail;
    for (role = 0; role < NM_RPC_HOST_GRAPHS; ++role) {
        if (!nm_rpc_stage(role, &model->graphs[role].artifact)) goto fail;
        if (shared_io) {
            NmAsrGraph *graph = model->graphs + role; u32 output, index;
            for (output = 0; output < 2; ++output) {
                QnnTensor *values = output ? graph->outputs : graph->inputs;
                u32 count = output ? graph->no : graph->ni;
                for (index = 0; index < count; ++index) {
                    QnnTensorV1 *value = &values[index].data.v1;
                    value->memory.client_buffer.data = nm_rpc_io_view(role, value->name,
                        output ? QNN_TENSOR_TYPE_APP_READ : QNN_TENSOR_TYPE_APP_WRITE,
                        value->memory.client_buffer.data_size);
                    if (!value->memory.client_buffer.data) goto fail;
                }
            }
        }
    }
    return 1;
fail:
    nm_asr_model_close(model); return 0;
}
int nm_asr_model_close(NmAsrModel *model) {
    u32 graph, index; int ok = 1;
    if (model->rpc_started && !nm_rpc_cleanup()) ok = 0;
    if (!nm_module_stage_close(&model->module_stage)) ok = 0;
    for (graph = 0; graph < NM_RPC_HOST_GRAPHS; ++graph) {
        NmAsrGraph *value = model->graphs + graph;
        for (index = 0; !model->shared_io && value->inputs && index < value->ni; ++index)
            nm_free(value->inputs[index].data.v1.memory.client_buffer.data,
                    value->inputs[index].data.v1.memory.client_buffer.data_size);
        for (index = 0; !model->shared_io && value->outputs && index < value->no; ++index)
            nm_free(value->outputs[index].data.v1.memory.client_buffer.data,
                    value->outputs[index].data.v1.memory.client_buffer.data_size);
        nm_free(value->inputs, (u64)value->ni * sizeof(*value->inputs));
        nm_free(value->outputs, (u64)value->no * sizeof(*value->outputs));
        nm_free(value->data, value->bytes);
    }
    memset(model, 0, sizeof(*model)); return ok;
}
int nm_asr_model_reset(void *context) {
    NmAsrModel *model = context; u32 graph, index;
    for (graph = 0; graph < NM_RPC_HOST_GRAPHS; ++graph) {
        NmAsrGraph *value = model->graphs + graph;
        for (index = 0; index < value->ni; ++index)
            memset(value->inputs[index].data.v1.memory.client_buffer.data, 0,
                   value->inputs[index].data.v1.memory.client_buffer.data_size);
        for (index = 0; index < value->no; ++index)
            memset(value->outputs[index].data.v1.memory.client_buffer.data, 0,
                   value->outputs[index].data.v1.memory.client_buffer.data_size);
    }
    model->chunks = model->graph_calls = model->invoke_ticks = model->copy_ticks = model->fused_joiner_reuses = 0;
    model->have_encoder = model->fused_ready = 0;
    memset(model->current_encoder, 0, sizeof(model->current_encoder));
    nm_rpc_io_stats_reset(); return 1;
}
int nm_asr_model_predict(void *context, uint32_t token) {
    NmAsrModel *model = context; NmAsrGraph *graph = model->graphs + 3;
    QnnTensor *input = binding(graph, "token", 0); u32 layer;
    if (!input || token >= 13088) return nm_error("prediction token outside contract");
    nm_put32(input->data.v1.memory.client_buffer.data, token);
    if (model->fused_decoder) {
        QnnTensor *encoder = binding(graph, "encoder", 0);
        if (!encoder) return 0;
        memcpy(encoder->data.v1.memory.client_buffer.data, model->current_encoder, sizeof(model->current_encoder));
    }
    if (!execute(model, 3)) return 0;
    for (layer = 0; layer < 2; ++layer) {
        char name[32] = "lstm-0-h-in", output[32] = "lstm-0-h-out"; u32 cell;
        name[5] = output[5] = (char)('0' + layer);
        for (cell = 0; cell < 2; ++cell) {
            name[7] = output[7] = cell ? 'c' : 'h';
            if (!nm_asr_binding_copy(binding(graph, name, 0), binding(graph, output, 1))) return 0;
        }
    }
    model->fused_ready = model->fused_decoder && model->have_encoder;
    return 1;
}
int nm_asr_model_join(void *context, const float *encoder, const float **logits, uint32_t *count) {
    NmAsrModel *model = context; NmAsrGraph *graph = model->graphs + 4;
    QnnTensor *enc, *result;
    if (model->fused_ready) {
        if (memcmp(encoder, model->current_encoder, sizeof(model->current_encoder)))
            return nm_error("fused decoder logits belong to a different encoder frame");
        result = binding(model->graphs + 3, "logits", 1);
        if (!result) return 0;
        *logits = result->data.v1.memory.client_buffer.data; *count = 13088;
        model->fused_ready = 0; ++model->fused_joiner_reuses; return 1;
    }
    enc = binding(graph, "encoder", 0);
    memcpy(model->current_encoder, encoder, sizeof(model->current_encoder)); model->have_encoder = 1;
    if (!enc || !nm_asr_binding_copy(binding(graph, "prediction", 0), binding(model->graphs + 3, "prediction", 1))) return 0;
    memcpy(enc->data.v1.memory.client_buffer.data, encoder, 640 * sizeof(float));
    if (!execute(model, 4)) return 0;
    result = binding(graph, "logits", 1);
    if (!result) return 0;
    *logits = result->data.v1.memory.client_buffer.data; *count = 13088; return 1;
}
int nm_asr_model_encode(NmAsrModel *model, const float *features, u32 width, const float **projected) {
    u32 first = model->chunks == 0, graph_index = first ? 0 : 1, cache, layer, index;
    NmAsrGraph *front = model->graphs + graph_index, *core = model->graphs + 2;
    QnnTensor *input = binding(front, "features", 0), *mask = binding(core, "mask", 0), *output;
    u32 seen = model->chunks >= 14 ? 56 : (u32)model->chunks * 4;
    if (width != (first ? 25U : 32U) || !input || !mask) return nm_error("encoder chunk shape mismatch");
    memcpy(input->data.v1.memory.client_buffer.data, features, width * 128 * sizeof(float));
    if (!execute(model, graph_index) || !nm_asr_binding_copy(binding(core, "hidden", 0), binding(front, "hidden", 1))) return 0;
    for (cache = 0; cache < 3; ++cache) {
        char from[16] = "sub-0-out", to[16] = "sub-0-in";
        from[4] = to[4] = (char)('0' + cache);
        if (!nm_asr_binding_copy(binding(model->graphs + 1, to, 0), binding(front, from, 1))) return 0;
    }
    for (index = 0; index < 8 * 4 * 60; ++index)
        ((float *)mask->data.v1.memory.client_buffer.data)[index] = index % 60 < 56 - seen ? -10000.0f : 0.0f;
    if (!execute(model, 2)) return 0;
    for (layer = 0; layer < 24; ++layer) {
        const char *suffixes[3] = {"-key", "-value", "-conv"};
        for (cache = 0; cache < 3; ++cache) {
            char from[64] = "layer-00", to[64];
            size_t length;
            from[6] = (char)('0' + layer / 10); from[7] = (char)('0' + layer % 10);
            rt_copy_string(from + 8, sizeof(from) - 8, suffixes[cache]); length = rt_strlen(from);
            rt_copy_string(to, sizeof(to), from);
            rt_copy_string(from + length, sizeof(from) - length, "-out");
            rt_copy_string(to + length, sizeof(to) - length, "-in");
            if (!nm_asr_binding_copy(binding(core, to, 0), binding(core, from, 1))) return 0;
        }
    }
    output = binding(core, "projected", 1);
    if (!output) return 0;
    *projected = output->data.v1.memory.client_buffer.data; ++model->chunks; return 1;
}
