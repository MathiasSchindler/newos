#include "nemotron_windows.h"
#include "fp32/nm_fp32.h"
#ifdef NM_FASTRPC
#include "fp32/nm_fastrpc.h"
#endif
#define NM_RUN "run"
#define NM_RUN_BENCHMARK "run-benchmark"
#define NM_PIPELINE "pipeline"
#define NM_PIPELINE_BENCHMARK "pipeline-benchmark"

_Static_assert(sizeof(QnnScaleOffset) == 8 && __builtin_offsetof(QnnScaleOffset, offset) == 4,
               "per-axis artifact table must match QNN scale-offset layout");

typedef struct NmQnn {
    void *module;
    QnnInterfaceV2 api;
    QnnBackendHandle backend;
    QnnDeviceHandle device;
    QnnProfileHandle profile;
    u64 cycles;
    int fp32_package;
} NmQnn;
typedef struct NmContext {
    NmArtifact artifact;
    u8 *file;
    u64 file_bytes;
    QnnContextHandle context;
    QnnGraphHandle graph;
    QnnTensor *inputs, *outputs;
    u32 ni, no;
} NmContext;
static NmQnn qnn;
static NmContext contexts[3];
static char args[8][4096];

static int qstatus(const char *operation, u64 code) {
    if (!code) return 1;
    nm_text("QNN "); nm_text(operation); nm_text(" failed: "); nm_number(code); nm_text("\n"); return 0;
}
static int artifact(NmArtifact *a, u8 **file, u64 *bytes, const char *path, u32 kind) {
    *file = nm_read_file(path, bytes);
    if (!*file) return 0;
    if (!nm_validate(a, *file, *bytes)) {
        nm_error(a->error);
        if (a->error_name) { nm_text("artifact node: "); nm_text(a->error_name); nm_text("\n"); }
        return 0;
    }
    if (kind && a->kind != kind) return nm_error("artifact kind does not match command");
    return 1;
}
static int initialize(int profiling) {
#ifdef NM_FASTRPC
    (void)profiling;
    return nm_rpc_initialize();
#else
    QnnInterfaceGetProviders get; const QnnInterfacePrefix **providers = 0; u32 count = 0, i;
    qnn.module = nm_load_qnn(); if (!qnn.module) return 0;
    get = (QnnInterfaceGetProviders)nm_symbol(qnn.module, "QnnInterface_getProviders");
    if (!get || !qstatus("getProviders", get(&providers, &count)) || !providers || count > 64) return nm_error("QNN provider list invalid");
    for (i = 0; i < count; ++i) {
        const QnnInterfacePrefix *p = providers[i];
        if (p) {
            nm_text("QNN provider core "); nm_number(p->api_version.core_api_version.major); nm_text(".");
            nm_number(p->api_version.core_api_version.minor); nm_text(".");
            nm_number(p->api_version.core_api_version.patch); nm_text(" backend ");
            nm_number(p->api_version.backend_api_version.major); nm_text(".");
            nm_number(p->api_version.backend_api_version.minor); nm_text(".");
            nm_number(p->api_version.backend_api_version.patch); nm_text("\n");
        }
        /* Header 2.18 is the pinned SDK metadata contract, not the HTP provider's 5.50 version. */
        if (p && p->api_version.core_api_version.major == 2 && p->api_version.core_api_version.minor == 39) {
            qnn.api = ((const QnnInterfaceProviderV2 *)p)->api;
            nm_text("QNN core 2.39."); nm_number(p->api_version.core_api_version.patch);
            nm_text("; frozen SDK backend metadata 2.18\n"); break;
        }
    }
    if (i == count) return nm_error("required QNN core2.39 provider unavailable");
    if (!qnn.api.backend_create || !qnn.api.backend_free || !qnn.api.device_create || !qnn.api.device_free ||
        !qnn.api.context_create_from_binary || !qnn.api.context_free || !qnn.api.graph_retrieve || !qnn.api.graph_execute)
        return nm_error("QNN required function slots missing");
#ifdef NM_PREPARE
    if (!qnn.api.context_create || !qnn.api.graph_create || !qnn.api.tensor_create_graph_tensor ||
        !qnn.api.graph_add_node || !qnn.api.graph_finalize || !qnn.api.context_get_binary_size || !qnn.api.context_get_binary)
        return nm_error("QNN preparation function slots missing");
#endif
    if (!qstatus("backendCreate", qnn.api.backend_create(0, 0, &qnn.backend))) return 0;
    if (!qstatus("deviceCreate", qnn.api.device_create(0, 0, &qnn.device))) return 0;
    {
        static char dsp[4096];
        int present = nm_package_path("NM_FP32_DSP_PACKAGE", "NM_FP32_DSP_SHA256", dsp);
        if (present < 0) return 0;
        if (present) {
            if (!qnn.api.backend_register_op_package) return nm_error("QNN package registration slot unavailable");
            nm_boundary("backendRegisterOpPackage", dsp, "HTP V73 DSP");
            if (!qstatus("register FP32 DSP HTP", qnn.api.backend_register_op_package(
                qnn.backend, dsp, "NmFp32InterfaceProvider", "HTP"))) return 0;
#ifdef NM_PREPARE
            {
                static char host[4096];
                int host_present = nm_package_path("NM_FP32_HOST_PACKAGE", "NM_FP32_HOST_SHA256", host);
                if (host_present < 0) return 0;
                if (host_present) {
                    nm_boundary("backendRegisterOpPackage", host, "CPU prepare-only");
                    if (!qstatus("register FP32 host CPU", qnn.api.backend_register_op_package(
                        qnn.backend, host, "NmFp32InterfaceProvider", "CPU"))) return 0;
                } else nm_text("FP32 DSP-only registration; no CPU package loaded\n");
            }
#endif
            qnn.fp32_package = 1;
            nm_text("FP32 HTP package registration requested; context creation verifies DSP acceptance (not execution proof)\n");
        }
    }
    if (profiling && qnn.api.profile_create && qnn.api.profile_free && qnn.api.profile_get_events &&
        qnn.api.profile_get_sub_events && qnn.api.profile_get_event_data) {
        if (!qstatus("profileCreate (optional)", qnn.api.profile_create(qnn.backend, 1, &qnn.profile))) qnn.profile = 0;
    }
    if (!profiling) nm_text("QNN profiling disabled for benchmark; profileCycles=0\n");
    else if (!qnn.profile) nm_text("QNN profiling unavailable; profileCycles=0\n");
    return nm_modules();
#endif
}
#ifndef NM_PREPARE
static void event_cycles(u64 id, u32 depth, u32 *budget) {
    QnnProfileEventData data; const u64 *children = 0; u32 count = 0, i;
    if (!*budget || depth > 16) return;
    --*budget; memset(&data, 0, sizeof(data));
    if (!qnn.api.profile_get_event_data(id, &data) && data.unit == 3 && data.value <= ~(u64)0 - qnn.cycles) {
        qnn.cycles += data.value;
        return;
    }
    if (!qnn.api.profile_get_sub_events(id, &children, &count) && children && count <= *budget)
        for (i = 0; i < count; ++i) event_cycles(children[i], depth + 1, budget);
}
static void profile_cycles(void) {
    /* BASIC profiles may expose only time events; zero cycles means unavailable, not zero work. */
    const u64 *events = 0; u32 count = 0, i, budget = 65536;
    if (qnn.profile && !qnn.api.profile_get_events(qnn.profile, &events, &count) && events && count <= budget)
        for (i = 0; i < count; ++i) event_cycles(events[i], 0, &budget);
}
static void profile_reset(void) {
    if (!qnn.profile) return;
    qstatus("profileFree", qnn.api.profile_free(qnn.profile)); qnn.profile = 0;
    if (!qstatus("profileCreate (optional)", qnn.api.profile_create(qnn.backend, 1, &qnn.profile))) qnn.profile = 0;
}
#endif
static int cleanup(void) {
    u32 i; int ok = 1;
#ifdef NM_FASTRPC
    ok = nm_rpc_cleanup();
#endif
    for (i = 0; i < 3; ++i) {
        NmContext *c = contexts + i;
        if (c->context && !qstatus("contextFree", qnn.api.context_free(c->context, 0))) ok = 0;
        nm_free(c->inputs, (u64)c->ni * sizeof(QnnTensor)); nm_free(c->outputs, (u64)c->no * sizeof(QnnTensor));
        nm_free(c->file, c->file_bytes);
    }
    if (qnn.profile && !qstatus("profileFree", qnn.api.profile_free(qnn.profile))) ok = 0;
    if (qnn.device && !qstatus("deviceFree", qnn.api.device_free(qnn.device))) ok = 0;
    if (qnn.backend && !qstatus("backendFree", qnn.api.backend_free(qnn.backend))) ok = 0;
    nm_unload(qnn.module); return ok;
}
static void tensor(QnnTensor *q, const u8 *t, void *buffer, const u8 *payload) {
    nm_tensor_binding(q, t, buffer, payload);
}
#ifdef NM_PREPARE
struct QnnGraphConfig {
    u32 option;
    void *custom;
};
typedef struct NmHtpGraphConfig {
    u32 option;
    union {
        u8 enabled;
        u64 reserved[6];
    } value;
} NmHtpGraphConfig;

static int prepare(const char *graph_path, const char *context_path, int accurate) {
    NmContext *c = contexts; QnnTensor *tensors = 0; u8 *out = 0;
    u64 binary = 0, used = 0, total = 0, start; u32 i, j, count = 0; int ok = 0;
    NmHtpGraphConfig custom[2] = {{0}};
    QnnGraphConfig config[2] = {{0}};
    const QnnGraphConfig *configurations[3] = {0};
    if (accurate) {
        custom[0].option = 18; custom[0].value.enabled = 1;
        custom[1].option = 16; custom[1].value.enabled = 1;
        for (i = 0; i < 2; ++i) {
            config[i].custom = custom + i;
            configurations[i] = config + i;
        }
        nm_text("HTP precision compensation and high-precision sigmoid enabled\n");
    }
    if (!artifact(&c->artifact, &c->file, &c->file_bytes, graph_path, NM_GRAPH) || !initialize(1)) goto done;
    if (!qstatus("contextCreate", qnn.api.context_create(qnn.backend, qnn.device, 0, &c->context)) ||
        !qstatus("graphCreate", qnn.api.graph_create(c->context, (const char *)c->artifact.payload,
                                                  accurate ? configurations : 0, &c->graph))) goto done;
    tensors = nm_alloc((u64)c->artifact.tensors * sizeof(QnnTensor)); if (!tensors) goto done;
    for (i = 0; i < c->artifact.tensors; ++i) {
        const u8 *t = nm_tensor(&c->artifact, i);
        tensor(tensors + i, t, nm_u32(t + 64) == QNN_TENSOR_TYPE_STATIC ?
               (void *)(c->artifact.payload + nm_u64(t + 96)) : 0, c->artifact.payload);
        nm_text("tensor index="); nm_number(i); nm_text(" logicalId="); nm_number(nm_u32(t + 76));
        nm_text(" qnnType="); nm_number(nm_u32(t + 64)); nm_text(" dtype="); nm_number(nm_u32(t + 68));
        nm_text(" rank="); nm_number(nm_u32(t + 72)); nm_text(" bytes="); nm_number(nm_tensor_bytes(t)); nm_text("\n");
        nm_boundary("tensorCreateGraphTensor", (const char *)t, "tensor");
        if (!qstatus("tensorCreateGraphTensor", qnn.api.tensor_create_graph_tensor(c->graph, tensors + i))) {
            nm_text("tensor: "); nm_text((const char *)t); nm_text("\n"); goto done;
        }
        if (nm_u32(t + 64) <= QNN_TENSOR_TYPE_APP_READ) ++count;
    }
    if (!count || count > NM_MAX_IO) { nm_error("context IO count exceeds limits"); goto done; }
    for (i = 0; i < c->artifact.nodes; ++i) {
        const u8 *n = c->artifact.payload + 64 + (u64)c->artifact.tensors * NM_TENSOR_BYTES + (u64)i * NM_NODE_BYTES;
        QnnOpConfig op; QnnTensor inputs[16], outputs[16]; QnnParam params[6];
        memset(&op, 0, sizeof(op)); memset(params, 0, sizeof(params));
        op.version = QNN_OPCONFIG_VERSION_1; op.data.v1.name = (const char *)n;
        op.data.v1.package_name = "qti.aisw"; op.data.v1.type_name = (const char *)n + 64;
        if (!rt_strcmp(op.data.v1.type_name, NM_FP32_OPERATOR) || !rt_strcmp(op.data.v1.type_name, NM_FP32_GATHER)) {
            if (!qnn.fp32_package) { nm_error("custom FP32 graph requires verified registered packages"); goto done; }
            op.data.v1.package_name = NM_FP32_PACKAGE;
        }
        op.data.v1.input_count = nm_u32(n + 128); op.data.v1.output_count = nm_u32(n + 132);
        op.data.v1.parameter_count = nm_u32(n + 136);
        op.data.v1.inputs = inputs; op.data.v1.outputs = outputs; op.data.v1.parameters = params;
        for (j = 0; j < op.data.v1.input_count; ++j) inputs[j] = tensors[nm_u32(n + 144 + j * 4)];
        for (j = 0; j < op.data.v1.output_count; ++j) outputs[j] = tensors[nm_u32(n + 208 + j * 4)];
        for (j = 0; j < op.data.v1.parameter_count; ++j) {
            const u8 *p = n + 272 + j * 64;
            params[j].name = (const char *)p; params[j].type = nm_u32(p + 32);
            if (params[j].type) params[j].value.tensor = tensors[nm_u32(p + 40)];
            else { params[j].value.scalar.data_type = nm_u32(p + 36); params[j].value.scalar.value.uint32_value = nm_u32(p + 40); }
        }
        nm_boundary("graphAddNode", (const char *)n, (const char *)n + 64);
        if (!qstatus("graphAddNode", qnn.api.graph_add_node(c->graph, op))) {
            nm_text("node: "); nm_text((const char *)n); nm_text(" operator: "); nm_text((const char *)n + 64); nm_text("\n"); goto done;
        }
    }
    nm_boundary("graphFinalize", (const char *)c->artifact.payload, "graph");
    if (!qstatus("graphFinalize", qnn.api.graph_finalize(c->graph, qnn.profile, 0)) ||
        !qstatus("contextGetBinarySize", qnn.api.context_get_binary_size(c->context, &binary))) goto done;
    start = nm_align16(64 + (u64)count * NM_TENSOR_BYTES);
    if (!binary || binary > NM_MAX_PAYLOAD - start - NM_HEADER_BYTES) { nm_error("context binary size exceeds bound"); goto done; }
    total = NM_HEADER_BYTES + start + binary; out = nm_alloc(total); if (!out) goto done;
    memcpy(out + NM_HEADER_BYTES, c->artifact.payload, 64); j = 0;
    for (i = 0; i < c->artifact.tensors; ++i) {
        const u8 *t = nm_tensor(&c->artifact, i);
        if (nm_u32(t + 64) <= QNN_TENSOR_TYPE_APP_READ) {
            u8 *d = out + NM_HEADER_BYTES + 64 + (u64)j++ * NM_TENSOR_BYTES;
            memcpy(d, t, NM_TENSOR_BYTES); nm_put32(d + 76, tensors[i].data.v1.id);
        }
    }
    if (!qstatus("contextGetBinary", qnn.api.context_get_binary(c->context, out + NM_HEADER_BYTES + start, binary, &used)) || used != binary) {
        nm_error("serialized context length mismatch"); goto done;
    }
    nm_header(out, NM_CONTEXT, c->artifact.precision, count, 0, 0, out + NM_HEADER_BYTES, total - NM_HEADER_BYTES);
    { NmArtifact check; if (!nm_validate(&check, out, total)) { nm_error(check.error); goto done; } }
    ok = nm_write_file(context_path, out, total);
    if (ok) { nm_text("Prepared context bytes "); nm_number(binary); nm_text("\n"); }
    if (!nm_modules()) ok = 0;
done:
    nm_free(out, total); nm_free(tensors, (u64)c->artifact.tensors * sizeof(QnnTensor)); return ok;
}
#else
typedef struct NmBuffer { u8 *current, *previous; u64 bytes; QnnTensor *binding; } NmBuffer;
static int restore(NmContext *c) {
    u32 i, ni = 0, no = 0;
#ifdef NM_FASTRPC
    if (!nm_rpc_stage((u32)(c - contexts), &c->artifact)) return 0;
#else
    if (!qstatus("contextCreateFromBinary", qnn.api.context_create_from_binary(qnn.backend, qnn.device, 0,
            c->artifact.payload + c->artifact.binary_offset, c->artifact.payload_bytes - c->artifact.binary_offset,
            &c->context, qnn.profile)) ||
        !qstatus("graphRetrieve", qnn.api.graph_retrieve(c->context, (const char *)c->artifact.payload, &c->graph))) return 0;
#endif
    for (i = 0; i < c->artifact.tensors; ++i) {
        u32 type = nm_u32(nm_tensor(&c->artifact, i) + 64);
        if (type == QNN_TENSOR_TYPE_APP_WRITE) ++c->ni;
        else if (type == QNN_TENSOR_TYPE_APP_READ) ++c->no;
    }
    if (!c->ni || !c->no) return nm_error("context requires app inputs and outputs");
    c->inputs = nm_alloc((u64)c->ni * sizeof(QnnTensor)); c->outputs = nm_alloc((u64)c->no * sizeof(QnnTensor));
    if (!c->inputs || !c->outputs) return 0;
    for (i = 0; i < c->artifact.tensors; ++i) {
        const u8 *t = nm_tensor(&c->artifact, i);
        if (nm_u32(t + 64) != QNN_TENSOR_TYPE_APP_WRITE && nm_u32(t + 64) != QNN_TENSOR_TYPE_APP_READ) continue;
        tensor(nm_u32(t + 64) == QNN_TENSOR_TYPE_APP_WRITE ? c->inputs + ni++ : c->outputs + no++,
               t, 0, c->artifact.payload);
    }
    return 1;
}
static int bind(NmArtifact *f, NmBuffer *buffers, u32 graphs) {
    u32 i, g, j;
    for (g = 0; g < graphs; ++g) {
        u32 found = 0;
        for (i = 0; i < f->tensors; ++i) {
            const u8 *t = nm_tensor(f, i); const u8 *ct = 0; NmContext *c = contexts + g;
            u32 index = 0, type = nm_u32(t + 64);
            if (nm_u32(t + 112) != g) continue;
            for (j = 0; j < c->artifact.tensors; ++j) {
                const u8 *candidate = nm_tensor(&c->artifact, j);
                if (!rt_strcmp((const char *)candidate, (const char *)t)) { ct = candidate; break; }
                if (nm_u32(candidate + 64) == type) ++index;
            }
            if (!ct || nm_u32(ct + 64) != type || !nm_same_shape(t, ct)) return nm_error("fixture/context IO name/type/dtype/shape mismatch");
            buffers[i].bytes = nm_tensor_bytes(t); buffers[i].current = nm_alloc(buffers[i].bytes);
            if (!buffers[i].current) return 0;
            if (type == QNN_TENSOR_TYPE_APP_READ) {
                buffers[i].previous = nm_alloc(buffers[i].bytes); if (!buffers[i].previous) return 0;
            }
            buffers[i].binding = type == QNN_TENSOR_TYPE_APP_WRITE ? c->inputs + index : c->outputs + index;
            buffers[i].binding->data.v1.memory.client_buffer.data = buffers[i].current;
            buffers[i].binding->data.v1.memory.client_buffer.data_size = (u32)buffers[i].bytes;
            ++found;
        }
        if (found != contexts[g].ni + contexts[g].no) return nm_error("fixture does not bind all context IO");
    }
    for (i = 0; i < f->tensors; ++i) if (nm_u32(nm_tensor(f, i) + 112) >= graphs) return nm_error("fixture graph index outside command");
    return 1;
}
static u8 *result(NmArtifact *f, u64 *bytes, int benchmark) {
    u64 metadata = 64 + (u64)f->tensors * NM_TENSOR_BYTES + (u64)f->steps * (NM_RESULT_STEP_BYTES + (u64)f->tensors * NM_CELL_BYTES);
    u64 size = nm_align16(metadata); u32 s, i; u8 *out;
    if (!benchmark) for (s = 0; s < f->steps; ++s) for (i = 0; i < f->tensors; ++i) {
        const u8 *t = nm_tensor(f, i);
        if (nm_u32(t + 64) == QNN_TENSOR_TYPE_APP_READ && nm_active(f, s, nm_u32(t + 112))) {
            u64 n = nm_tensor_bytes(t);
            if (size > NM_MAX_PAYLOAD - n - 15) { nm_error("result payload exceeds bound"); return 0; }
            size = nm_align16(size + n);
        }
    }
    if (size > NM_MAX_PAYLOAD - NM_HEADER_BYTES) return 0;
    *bytes = NM_HEADER_BYTES + size; out = nm_alloc(*bytes);
    if (out) {
        memcpy(out + NM_HEADER_BYTES, f->payload, (size_t)(64 + (u64)f->tensors * NM_TENSOR_BYTES));
        for (s = 0; s < f->steps; ++s) for (i = 0; i < f->tensors; ++i)
            nm_put32(out + NM_HEADER_BYTES + 64 + (u64)f->tensors * NM_TENSOR_BYTES +
                     (u64)s * (NM_RESULT_STEP_BYTES + (u64)f->tensors * NM_CELL_BYTES) +
                     NM_RESULT_STEP_BYTES + (u64)i * NM_CELL_BYTES, NM_ZERO);
    }
    return out;
}
static void compare(const u8 *t, const u8 *actual, const u8 *expected, u8 *metric, u32 step) {
    u32 violations = nm_compare(t, actual, expected, metric);
    if (violations) {
        nm_text("comparison step "); nm_number(step); nm_text(" tensor "); nm_text((const char *)t);
        nm_text(" violations "); nm_number(violations); nm_text("\n");
    }
}
static int run(u32 graphs, const char *fixture_path, const char *result_path, int benchmark) {
    NmArtifact f; NmBuffer *buffers = 0; u8 *file = 0, *out = 0;
    u64 file_bytes = 0, out_bytes = 0, cursor = 0, restore_ticks, restore_started, started;
    u64 violations = 0, total_compared = 0;
    u32 i, s, g, pass; int ok = 0;
    memset(&f, 0, sizeof(f));
    if (!artifact(&f, &file, &file_bytes, fixture_path, NM_FIXTURE)) goto done;
    if ((graphs == 3) != f.pipeline) { nm_error("fixture single/pipeline graph layout mismatch"); goto done; }
    /* Cold restore includes context read/hash and DLL/QNN initialization, excluding fixture load. */
    restore_started = nm_clock();
    for (g = 0; g < graphs; ++g) {
        if (!artifact(&contexts[g].artifact, &contexts[g].file, &contexts[g].file_bytes, args[2 + g],
#ifdef NM_FASTRPC
                      NM_GRAPH
#else
                      NM_CONTEXT
#endif
                      )) goto done;
        if (contexts[g].artifact.precision != f.precision) { nm_error("context/fixture precision mismatch"); goto done; }
    }
    if (!initialize(!benchmark)) goto done;
    for (g = 0; g < graphs; ++g) if (!restore(contexts + g)) goto done;
    restore_ticks = nm_clock() - restore_started;
    if (!nm_modules()) goto done;
    buffers = nm_alloc((u64)f.tensors * sizeof(NmBuffer)); if (!buffers || !bind(&f, buffers, graphs)) goto done;
    out = result(&f, &out_bytes, benchmark); if (!out) goto done;
    cursor = nm_align16(64 + (u64)f.tensors * NM_TENSOR_BYTES + (u64)f.steps * (NM_RESULT_STEP_BYTES + (u64)f.tensors * NM_CELL_BYTES));
    if (benchmark) nm_text("Benchmark timing only: no correctness comparison or output snapshots; 3 full-fixture warmups\n");
    for (pass = 0; pass < (benchmark ? 4U : 1U); ++pass) {
#ifdef NM_FASTRPC
        u64 cpu_user_begin = 0, cpu_kernel_begin = 0;
        int cpu_started = benchmark && pass == 3 && nm_process_cpu(&cpu_user_begin, &cpu_kernel_begin);
#endif
        for (s = 0; s < f.steps; ++s) {
        u8 *metric = out + NM_HEADER_BYTES + 64 + (u64)f.tensors * NM_TENSOR_BYTES +
            (u64)s * (NM_RESULT_STEP_BYTES + (u64)f.tensors * NM_CELL_BYTES);
        u64 wall = nm_clock(), copy_ticks = 0;
        nm_put32(metric, s); nm_put32(metric + 4, nm_u32(nm_step(&f, s)));
        for (g = 0; g < graphs; ++g) {
            NmContext *c = contexts + g; u64 execute_ticks;
            if (!nm_active(&f, s, g)) continue;
            started = nm_clock();
            for (i = 0; i < f.tensors; ++i) {
                const u8 *t = nm_tensor(&f, i), *cell = nm_cell(&f, s, i);
                if (nm_u32(t + 112) != g || nm_u32(t + 64) != QNN_TENSOR_TYPE_APP_WRITE) continue;
                if (nm_u32(cell) == NM_DATA) memcpy(buffers[i].current, f.payload + nm_u64(cell + 8), (size_t)buffers[i].bytes);
                else if (nm_u32(cell) == NM_ZERO) nm_zero(t, buffers[i].current);
                else {
                    u32 source = nm_u32(cell + 4);
                    const u8 *from = nm_u32(cell + 24) ? buffers[source].previous : buffers[source].current;
                    memcpy(buffers[i].current, from, (size_t)buffers[i].bytes);
                }
                if (!nm_finite(buffers[i].current, buffers[i].bytes, nm_u32(t + 68))) {
                    nm_text("nonfinite/invalid native input: "); nm_text((const char *)t); nm_text("\n"); goto done;
                }
            }
            copy_ticks += nm_clock() - started;
            if (!benchmark) profile_reset();
            started = nm_clock();
#ifdef NM_FASTRPC
            {
                u64 rpc_copy_ticks = 0;
                if (!nm_rpc_execute(g, c->inputs, c->ni, c->outputs, c->no, &execute_ticks, &rpc_copy_ticks, benchmark)) goto done;
                copy_ticks += rpc_copy_ticks;
            }
#else
            if (!qstatus("graphExecute", qnn.api.graph_execute(c->graph, c->inputs, c->ni, c->outputs, c->no, qnn.profile, 0))) goto done;
            execute_ticks = nm_clock() - started;
#endif
            nm_put64(metric + (graphs == 3 && g < 2 ? 8 : 16), execute_ticks);
            if (!benchmark) profile_cycles();
            if (!benchmark) for (i = 0; i < f.tensors; ++i) {
                const u8 *t = nm_tensor(&f, i), *cell = nm_cell(&f, s, i);
                u8 *rc = metric + NM_RESULT_STEP_BYTES + (u64)i * NM_CELL_BYTES;
                if (nm_u32(t + 112) != g || nm_u32(t + 64) != QNN_TENSOR_TYPE_APP_READ) continue;
                compare(t, buffers[i].current, f.payload + nm_u64(cell + 8), metric, s);
                nm_put32(rc, NM_DATA); nm_put64(rc + 8, cursor); nm_put64(rc + 16, buffers[i].bytes);
                memcpy(out + NM_HEADER_BYTES + cursor, buffers[i].current, (size_t)buffers[i].bytes);
                cursor = nm_align16(cursor + buffers[i].bytes);
            }
        }
        started = nm_clock();
        for (i = 0; i < f.tensors; ++i) {
            const u8 *t = nm_tensor(&f, i);
            if (buffers[i].previous && nm_active(&f, s, nm_u32(t + 112)))
                memcpy(buffers[i].previous, buffers[i].current, (size_t)buffers[i].bytes);
        }
        copy_ticks += nm_clock() - started;
        /* Cache snapshots remain necessary for autoregressive age-one CARRY bindings. */
        nm_put64(metric + 24, copy_ticks); nm_put64(metric + 32, nm_clock() - wall);
        violations += nm_u32(metric + 44); total_compared += nm_u64(metric + 48);
        }
#ifdef NM_FASTRPC
        if (benchmark && pass == 3) {
            u64 user, kernel;
            if (cpu_started && nm_process_cpu(&user, &kernel) && user >= cpu_user_begin && kernel >= cpu_kernel_begin) {
                nm_text("HOST_PROCESS_CPU measuredSteps="); nm_number(f.steps);
                nm_text(" warmupPasses=3 user100ns="); nm_number(user - cpu_user_begin);
                nm_text(" kernel100ns="); nm_number(kernel - cpu_kernel_begin);
                nm_text(" total100ns="); nm_number(user - cpu_user_begin + kernel - cpu_kernel_begin); nm_text("\n");
            } else nm_text("HOST_PROCESS_CPU unavailable or nonmonotonic; no CPU utilization inferred\n");
        }
#endif
    }
    nm_header(out, NM_RESULT, f.precision, f.tensors, 0, f.steps, out + NM_HEADER_BYTES, out_bytes - NM_HEADER_BYTES);
    nm_put64(out + 200, nm_frequency()); nm_put64(out + 208, restore_ticks);
    nm_put64(out + 216, nm_peak_rss()); nm_put64(out + 224, qnn.cycles);
    ok = nm_write_file(result_path, out, out_bytes);
    if (!nm_modules()) ok = 0;
    nm_text("Compared elements "); nm_number(total_compared); nm_text(" violations "); nm_number(violations);
    nm_text(" restoreTicks "); nm_number(restore_ticks); nm_text(" profileCycles "); nm_number(qnn.cycles); nm_text("\n");
    if (violations) ok = 0;
done:
    if (buffers) for (i = 0; i < f.tensors; ++i) {
        nm_free(buffers[i].current, buffers[i].bytes); nm_free(buffers[i].previous, buffers[i].bytes);
    }
    nm_free(buffers, (u64)f.tensors * sizeof(NmBuffer)); nm_free(out, out_bytes); nm_free(file, file_bytes); return ok;
}
#endif
void mainCRTStartup(void) {
    nm_debug_init();
    int argc = nm_arguments(args), ok = 0;
    if (argc == 3 && !rt_strcmp(args[1], "validate")) {
        NmArtifact a; u8 *file = 0; u64 bytes = 0;
        ok = artifact(&a, &file, &bytes, args[2], 0);
        if (ok) {
            nm_text("Validated kind "); nm_number(a.kind); nm_text(" tensors "); nm_number(a.tensors); nm_text("\n");
            if (a.benchmark) nm_text("Benchmark timing-only result; correctness NOT assessed\n");
        }
        nm_free(file, bytes);
#ifdef NM_PREPARE
    } else if ((argc == 4 || (argc == 5 && !rt_strcmp(args[4], "--accurate"))) &&
               !rt_strcmp(args[1], "prepare")) ok = prepare(args[2], args[3], argc == 5);
#else
    } else if (argc == 5 && (!rt_strcmp(args[1], NM_RUN) || !rt_strcmp(args[1], NM_RUN_BENCHMARK)))
        ok = run(1, args[3], args[4], !rt_strcmp(args[1], NM_RUN_BENCHMARK));
    else if (argc == 7 && (!rt_strcmp(args[1], NM_PIPELINE) || !rt_strcmp(args[1], NM_PIPELINE_BENCHMARK)))
        ok = run(3, args[5], args[6], !rt_strcmp(args[1], NM_PIPELINE_BENCHMARK));
#endif
    else nm_error(
#ifdef NM_PREPARE
        "usage: nemotron-prepare.exe prepare graph.nmg new-context.nmc [--accurate] | validate artifact"
#elif defined(NM_FASTRPC)
        "usage: nemotron-fastrpc-fp32.exe run[-benchmark] graph.nmg cases.nmf new-result.nmr | pipeline[-benchmark] first.nmg steady.nmg encoder.nmg cases.nmf new-result.nmr"
#else
        "usage: nemotron-probe.exe run[-benchmark] context.nmc cases.nmf new-result.nmr | pipeline[-benchmark] first.nmc steady.nmc encoder.nmc cases.nmf new-result.nmr | validate artifact"
#endif
    );
    if (!cleanup()) ok = 0;
    ExitProcess(ok ? 0 : 1);
}
