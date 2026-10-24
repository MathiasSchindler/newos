#include "nm_fastrpc.h"
#include "protocol.h"
#include "nm_fp32.h"

__declspec(dllimport) void *LoadLibraryExW(const u16 *, void *, u32);
__declspec(dllimport) int MultiByteToWideChar(u32, u32, const char *, int, u16 *, int);
__declspec(dllimport) u32 GetLastError(void);
__declspec(dllimport) u32 GetEnvironmentVariableW(const u16 *, u16 *, u32);
typedef int (*RemoteControl)(u32, void *, u32);
typedef int (*RemoteOpen)(const char *, u64 *);
typedef int (*RemoteInvoke)(u64, u32, NmRpcArg *);
typedef int (*RemoteClose)(u64);
typedef void *(*RpcAlloc)(int, u32, int);
typedef void (*RpcFree)(void *);
typedef struct NmRpcGraph {
    const NmArtifact *artifact;
    NmRpcControl *control;
    void *parts[NM_RPC_MAX_PARTS];
    u8 *workspace;
} NmRpcGraph;
static NmRpcGraph graphs[NM_RPC_HOST_GRAPHS];
static int diagnostics = 1;
static void *driver;
static u64 handle, invocation;
static int opened;
static RemoteInvoke invoke;
static RemoteClose close_rpc;
static RpcAlloc allocate;
static RpcFree release;
static u32 power_flags;
static u32 requested_workers = 1;
static int align_coefficients = 1;
static u64 io_copied, io_shared;
_Static_assert(sizeof(NmRpcArg) == 16, "Windows RPC ABI");
void nm_rpc_diagnostics(int enabled) { diagnostics = enabled != 0; }
void nm_rpc_io_stats(u64 *copied, u64 *shared) { *copied = io_copied; *shared = io_shared; }
void nm_rpc_io_stats_reset(void) { io_copied = io_shared = 0; }
static int status(const char *operation, int code) {
    nm_text("OEM FastRPC "); nm_text(operation); nm_text(" status="); nm_number((u32)code); nm_text("\n");
    return !code;
}
static void *rpc_allocate(u64 bytes, const char *kind) {
    void *p;
    if (!bytes || bytes > NM_RPC_MAX_BUFFER) { nm_error("RPC buffer exceeds 512 MiB"); return 0; }
    p = allocate(25, 1, (int)bytes);
    nm_text("RPC allocation "); nm_text(kind); nm_text(" bytes="); nm_number(bytes); nm_text(" success=");
    nm_number(p != 0); nm_text("\n");
    return p;
}
int nm_rpc_initialize(void) {
    static char module[4096], path[4096], uri[8192];
    static u16 wide[32768];
    RemoteOpen open_rpc;
    RemoteControl session;
    struct { int domain, enable; } request = {3, 1};
    static const char suffix[] = "?nm_fastrpc_skel_invoke&_modver=1.0&_dom=cdsp";
    u32 i, cursor = 0;
    if (opened || driver) return nm_error("RPC runtime already initialized");
    power_flags = 0; requested_workers = 1; align_coefficients = 1;
    nm_rpc_io_stats_reset();
    {
        u16 key[64], value[32];
        u32 n;
        MultiByteToWideChar(65001, 8, "NM_FP32_DSP_POWER_PROFILE", -1, key, 64);
        n = GetEnvironmentVariableW(key, value, 32);
        if (n) {
            static const char turbo[] = "turbo", baseline[] = "baseline";
            int matches_turbo = n == 5, matches_baseline = n == 8;
            if (n >= 32) return nm_error("invalid DSP power profile");
            for (i = 0; i < n && matches_turbo; ++i) if (value[i] != (u16)turbo[i]) matches_turbo = 0;
            for (i = 0; i < n && matches_baseline; ++i) if (value[i] != (u16)baseline[i]) matches_baseline = 0;
            if (!matches_turbo && !matches_baseline) return nm_error("DSP power profile must be baseline or turbo");
            power_flags = matches_turbo ? NM_RPC_TURBO : 0;
        }
        nm_text("DSP power request="); nm_text(power_flags ? "scoped HAP DCVS_V3 core/bus TURBO latency40us" : "baseline (no vote)");
        nm_text("\n");
        MultiByteToWideChar(65001, 8, "NM_FP32_DSP_WORKERS", -1, key, 64);
        n = GetEnvironmentVariableW(key, value, 32);
        if (n) {
            if (n != 1 || (value[0] != '1' && value[0] != '4')) return nm_error("DSP workers must be 1 or 4");
            requested_workers = value[0] == '4' ? 4 : 1;
        }
        nm_text("DSP self-slice workers="); nm_number(requested_workers); nm_text("\n");
        MultiByteToWideChar(65001, 8, "NM_FP32_RPC_ALIGN_COEFFICIENTS", -1, key, 64);
        n = GetEnvironmentVariableW(key, value, 32);
        if (n) {
            if (n != 1 || (value[0] != '0' && value[0] != '1')) return nm_error("coefficient alignment must be 0 or 1");
            align_coefficients = value[0] == '1';
        }
        nm_text("RPC coefficient staging="); nm_text(align_coefficients ? "128-byte aligned immutable tensors" : "original payload slices");
        nm_text("\n");
    }
    if (nm_package_path("NM_FP32_DSP_PACKAGE", "NM_FP32_DSP_SHA256", module) != 1 ||
        nm_package_path("NM_FP32_RPC_DRIVER", "NM_FP32_RPC_SHA256", path) != 1)
        return nm_error("verified OEM driver and pure FastRPC DSP package are required");
    if (!MultiByteToWideChar(65001, 8, path, -1, wide, 32768)) return 0;
    driver = LoadLibraryExW(wide, 0, 0x1100);
    if (!driver) { nm_text("OEM driver LoadLibrary error="); nm_number(GetLastError()); nm_text("\n"); return 0; }
    session = (RemoteControl)nm_symbol(driver, "remote_session_control");
    open_rpc = (RemoteOpen)nm_symbol(driver, "remote_handle64_open");
    invoke = (RemoteInvoke)nm_symbol(driver, "remote_handle64_invoke");
    close_rpc = (RemoteClose)nm_symbol(driver, "remote_handle64_close");
    allocate = (RpcAlloc)nm_symbol(driver, "rpcmem_alloc");
    release = (RpcFree)nm_symbol(driver, "rpcmem_free");
    if (!session || !open_rpc || !invoke || !close_rpc || !allocate || !release)
        return nm_error("required OEM RPC API export missing");
    if (!status("unsigned-session (existing signed-catalog policy unchanged)", session(2, &request, sizeof(request)))) return 0;
    memcpy(uri, "file:///", 8); cursor = 8;
    for (i = 0; module[i]; ++i) {
        static const char hex[] = "0123456789ABCDEF";
        u8 c = (u8)module[i];
        if (cursor + 4 + sizeof(suffix) > sizeof(uri)) return nm_error("RPC module URI exceeds bound");
        if (c == ' ' || c == '%' || c == '?' || c == '#' || c >= 128) {
            uri[cursor++] = '%'; uri[cursor++] = hex[c >> 4]; uri[cursor++] = hex[c & 15];
        } else uri[cursor++] = (char)c;
    }
    {
        memcpy(uri + cursor, suffix, sizeof(suffix));
    }
    nm_text("backend=OEMFastRPC-HVX-FP32 (not QNN; no host model kernels)\nmoduleURI="); nm_text(uri); nm_text("\n");
    if (!status("open signed FP32 module", open_rpc(uri, &handle))) return 0;
    opened = 1; return nm_modules();
}
static u8 *staged_pointer(NmRpcGraph *g, u64 offset, u64 bytes) {
    u32 i;
    for (i = 0; i < g->control->parts; ++i) {
        const NmRpcPart *p = g->control->part + i;
        if (offset >= p->offset && offset - p->offset <= p->bytes &&
            bytes <= p->bytes - (offset - p->offset))
            return (u8 *)g->parts[i] + (size_t)(offset - p->offset);
    }
    return 0;
}
int nm_rpc_stage(u32 graph, const NmArtifact *a) {
    NmRpcGraph *g;
    NmRpcControl header;
    u64 offset = 0, arena_bytes, *arena, live_bytes;
    u32 i, parts = 0;
    if (graph >= NM_RPC_HOST_GRAPHS || a->kind != NM_GRAPH || a->precision != 32 ||
        a->metadata_bytes > NM_RPC_MAX_BUFFER || !a->nodes) return nm_error("RPC requires a validated FP32 graph");
    if (graphs[graph].control) return nm_error("RPC graph already staged");
    g = graphs + graph; g->artifact = a;
    memset(&header, 0, sizeof(header));
    header.magic = NM_RPC_MAGIC; header.version = NM_RPC_VERSION;
    header.flags = power_flags | (align_coefficients ? NM_RPC_ALIGN_COEFFICIENTS : 0);
    header.workers = requested_workers; header.tensors = a->tensors; header.nodes = a->nodes;
    header.metadata_bytes = (u32)a->metadata_bytes; header.source_payload_bytes = a->payload_bytes;
    memcpy(header.source_payload_sha256, a->header + 136, 32);
    g->control = rpc_allocate(sizeof(header) + (u64)a->tensors * 8, "control");
    if (!g->control) return 0;
    memset(g->control, 0, sizeof(header) + (size_t)a->tensors * 8);
    arena = (u64 *)(g->control + 1);
    live_bytes = a->metadata_bytes;
    if (align_coefficients) {
        u64 cursor = (a->metadata_bytes + 127) & ~(u64)127, part_start = 0;
        for (i = 0; i < a->tensors; ++i) {
            const u8 *t = nm_tensor(a, i);
            u64 bytes;
            if (nm_u32(t + 64) != QNN_TENSOR_TYPE_STATIC) continue;
            bytes = nm_u64(t + 104);
            if (!bytes || bytes > NM_RPC_MAX_BUFFER || cursor > NM_MAX_PAYLOAD - 127 ||
                live_bytes > NM_MAX_PAYLOAD - bytes || live_bytes > a->payload_bytes ||
                bytes > a->payload_bytes - live_bytes) return nm_error("aligned coefficient layout exceeds bounds");
            cursor = (cursor + 127) & ~(u64)127;
            if (cursor - part_start > NM_RPC_MAX_BUFFER - bytes) {
                if (!cursor || cursor - part_start > NM_RPC_MAX_BUFFER || parts + 1 >= NM_RPC_MAX_PARTS)
                    return nm_error("aligned coefficients exceed 16 bounded RPC parts");
                header.part[parts].offset = part_start; header.part[parts++].bytes = (u32)(cursor - part_start);
                part_start = cursor;
            }
            if (cursor > NM_MAX_PAYLOAD - bytes) return nm_error("aligned coefficient offset overflow");
            arena[i] = cursor; cursor += bytes; live_bytes += bytes;
        }
        if (cursor > NM_MAX_PAYLOAD - 127) return nm_error("aligned coefficient final padding overflow");
        cursor = (cursor + 127) & ~(u64)127;
        if (cursor <= part_start || cursor - part_start > NM_RPC_MAX_BUFFER) return nm_error("invalid aligned final RPC part");
        header.part[parts].offset = part_start; header.part[parts++].bytes = (u32)(cursor - part_start);
        header.payload_bytes = cursor;
    } else {
        header.payload_bytes = a->payload_bytes;
        while (offset < a->payload_bytes) {
            u64 end = a->payload_bytes - offset > NM_RPC_MAX_BUFFER ? offset + NM_RPC_MAX_BUFFER : a->payload_bytes;
            if (parts == NM_RPC_MAX_PARTS) return nm_error("graph exceeds 16 RPC weight parts");
            for (i = 0; i < a->tensors; ++i) {
                const u8 *t = nm_tensor(a, i);
                u64 begin = nm_u64(t + 96), bytes = nm_u64(t + 104);
                if (nm_u32(t + 64) == QNN_TENSOR_TYPE_STATIC && begin < end && bytes > end - begin) end = begin;
            }
            if (end <= offset || (!parts && end < a->metadata_bytes)) return nm_error("static tensor crosses maximum RPC part boundary");
            header.part[parts].offset = offset; header.part[parts++].bytes = (u32)(end - offset); offset = end;
        }
        for (i = 0; i < a->tensors; ++i) if (nm_u32(nm_tensor(a, i) + 64) == QNN_TENSOR_TYPE_STATIC) {
            u64 bytes = nm_u64(nm_tensor(a, i) + 104);
            if (live_bytes > a->payload_bytes || bytes > a->payload_bytes - live_bytes)
                return nm_error("coefficient byte accounting exceeds original graph");
            live_bytes += bytes;
        }
    }
    header.parts = parts; memcpy(g->control, &header, sizeof(header));
    arena_bytes = (NM_RPC_WORKSPACE_PREFIX + (u64)a->tensors + 127) & ~(u64)127;
    for (i = 0; i < a->tensors; ++i) {
        const u8 *t = nm_tensor(a, i);
        if (nm_u32(t + 64) != QNN_TENSOR_TYPE_STATIC) {
            u64 bytes = nm_tensor_bytes(t);
            if (!bytes || bytes > NM_RPC_MAX_BUFFER || arena_bytes > NM_RPC_MAX_BUFFER - bytes ||
                ((arena_bytes + bytes + 127) & ~(u64)127) > NM_RPC_MAX_BUFFER)
                return nm_error("mutable tensor workspace exceeds 512 MiB");
            arena[i] = arena_bytes; arena_bytes = (arena_bytes + bytes + 127) & ~(u64)127;
        }
    }
    g->control->workspace_bytes = arena_bytes;
    g->workspace = rpc_allocate(arena_bytes, "IN/OUT workspace");
    if (!g->workspace) return 0;
    memset(g->workspace, 0, (size_t)arena_bytes);
    for (i = 0; i < parts; ++i) {
        g->parts[i] = rpc_allocate(header.part[i].bytes, "read-only graph/weights");
        if (!g->parts[i]) return 0;
        if (align_coefficients) memset(g->parts[i], 0, header.part[i].bytes);
        else memcpy(g->parts[i], a->payload + header.part[i].offset, header.part[i].bytes);
    }
    if (align_coefficients) {
        u8 *table = g->parts[0];
        memcpy(table, a->payload, (size_t)a->metadata_bytes);
        for (i = 0; i < a->tensors; ++i) {
            const u8 *t = nm_tensor(a, i);
            u64 bytes, source;
            u8 *destination;
            if (nm_u32(t + 64) != QNN_TENSOR_TYPE_STATIC) continue;
            bytes = nm_u64(t + 104); source = nm_u64(t + 96);
            if (source > a->payload_bytes || bytes > a->payload_bytes - source)
                return nm_error("original coefficient source bounds changed");
            destination = staged_pointer(g, arena[i], bytes);
            if (!destination || ((u64)destination & 127)) return nm_error("aligned coefficient destination mapping failed");
            memcpy(destination, a->payload + source, (size_t)bytes);
            if (memcmp(destination, a->payload + source, (size_t)bytes)) return nm_error("coefficient byte-copy verification failed");
            nm_put64(table + 64 + (u64)i * NM_TENSOR_BYTES + 96, arena[i]);
        }
    }
    nm_text("RPC graph staged graph="); nm_number(graph); nm_text(" parts="); nm_number(parts);
    nm_text(" workspaceBytes="); nm_number(arena_bytes); nm_text(" nodes="); nm_number(a->nodes);
    nm_text(" sourceBytes="); nm_number(a->payload_bytes); nm_text(" stagedBytes="); nm_number(header.payload_bytes);
    nm_text(" paddingBytes="); nm_number(header.payload_bytes - live_bytes);
    nm_text(" sourcePaddingBytes="); nm_number(a->payload_bytes - live_bytes); nm_text("\n");
    {
        static const char hex[] = "0123456789abcdef";
        char digest[65];
        for (i = 0; i < 32; ++i) {
            digest[i * 2] = hex[header.source_payload_sha256[i] >> 4];
            digest[i * 2 + 1] = hex[header.source_payload_sha256[i] & 15];
        }
        digest[64] = 0;
        nm_text("RPC original source payload SHA256="); nm_text(digest); nm_text("\n");
    }
    return 1;
}
void *nm_rpc_io_view(u32 graph, const char *name, u32 role, u32 bytes) {
    NmRpcGraph *g; u32 index; u64 *arena, offset;
    if (graph >= NM_RPC_HOST_GRAPHS || !graphs[graph].control || !graphs[graph].workspace ||
        (role != QNN_TENSOR_TYPE_APP_WRITE && role != QNN_TENSOR_TYPE_APP_READ)) {
        nm_error("invalid RPC workspace IO view"); return 0;
    }
    g = graphs + graph; arena = (u64 *)(g->control + 1);
    for (index = 0; index < g->artifact->tensors; ++index) {
        const u8 *tensor = nm_tensor(g->artifact, index);
        if (nm_u32(tensor + 64) == role && !rt_strcmp((const char *)tensor, name)) break;
    }
    if (index == g->artifact->tensors || nm_tensor_bytes(nm_tensor(g->artifact, index)) != bytes) {
        nm_error("RPC workspace view name/size mismatch"); return 0;
    }
    offset = arena[index];
    if (offset < NM_RPC_WORKSPACE_PREFIX || offset > g->control->workspace_bytes ||
        bytes > g->control->workspace_bytes - offset) {
        nm_error("RPC workspace view outside mutable arena"); return 0;
    }
    return g->workspace + offset;
}
static int transfer(NmRpcGraph *g, QnnTensor *bindings, u32 count, u32 role) {
    u64 *arena = (u64 *)(g->control + 1);
    u32 i, j;
    for (i = 0; i < count; ++i) {
        QnnTensorV1 *binding = &bindings[i].data.v1;
        const u8 *tensor;
        j = binding->id ? binding->id - 1 : g->artifact->tensors;
        if (j >= g->artifact->tensors || nm_u32(nm_tensor(g->artifact, j) + 76) != binding->id ||
            nm_u32(nm_tensor(g->artifact, j) + 64) != role ||
            rt_strcmp((const char *)nm_tensor(g->artifact, j), binding->name)) {
            for (j = 0; j < g->artifact->tensors; ++j) {
                const u8 *t = nm_tensor(g->artifact, j);
                if (nm_u32(t + 64) == role && !rt_strcmp((const char *)t, binding->name)) break;
            }
        }
        if (j == g->artifact->tensors || !binding->memory.client_buffer.data ||
            binding->memory.client_buffer.data_size != nm_tensor_bytes(nm_tensor(g->artifact, j)))
            return nm_error("RPC IO binding mismatch");
        tensor = nm_tensor(g->artifact, j);
        if (!binding->dimensions || binding->data_type != nm_u32(tensor + 68) || binding->rank != nm_u32(tensor + 72) ||
            memcmp(binding->dimensions, tensor + 80, binding->rank * 4))
            return nm_error("RPC IO dtype/shape mismatch");
        if (binding->memory.client_buffer.data == g->workspace + arena[j]) {
            io_shared += binding->memory.client_buffer.data_size; continue;
        }
        {
            u64 source = (u64)binding->memory.client_buffer.data, destination = (u64)(g->workspace + arena[j]);
            u64 bytes = binding->memory.client_buffer.data_size;
            if (source > ~(u64)0 - bytes || destination > ~(u64)0 - bytes ||
                (source < destination + bytes && destination < source + bytes))
                return nm_error("RPC client/workspace buffers partially overlap");
        }
        if (role == QNN_TENSOR_TYPE_APP_WRITE)
            memcpy(g->workspace + arena[j], binding->memory.client_buffer.data, binding->memory.client_buffer.data_size);
        else memcpy(binding->memory.client_buffer.data, g->workspace + arena[j], binding->memory.client_buffer.data_size);
        io_copied += binding->memory.client_buffer.data_size;
    }
    return 1;
}
int nm_rpc_execute(u32 graph, QnnTensor *inputs, u32 ni, QnnTensor *outputs, u32 no,
                   u64 *invoke_ticks, u64 *copy_ticks, int benchmark) {
    NmRpcGraph *g;
    NmRpcArg arguments[NM_RPC_MAX_PARTS + 3];
    NmRpcReply *reply;
    u32 i, nin;
    u64 started = nm_clock(), before, after, input_copy_ticks;
    int code;
    if (graph >= NM_RPC_HOST_GRAPHS || !graphs[graph].control || !graphs[graph].workspace ||
        !opened || !invoke_ticks || !copy_ticks || (!inputs && ni) || (!outputs && no))
        return nm_error("invalid or unstaged RPC execution");
    g = graphs + graph; reply = (NmRpcReply *)g->workspace; nin = g->control->parts + 2;
    if (!transfer(g, inputs, ni, QNN_TENSOR_TYPE_APP_WRITE)) return 0;
    input_copy_ticks = nm_clock() - started;
    memset(reply, 0, sizeof(*reply));
    reply->magic = NM_RPC_MAGIC; reply->version = NM_RPC_VERSION;
    reply->invocation = ++invocation; g->control->invocation = invocation;
    arguments[0].buffer.data = g->control;
    arguments[0].buffer.bytes = sizeof(*g->control) + (size_t)g->control->tensors * 8;
    for (i = 0; i < g->control->parts; ++i) {
        arguments[i + 1].buffer.data = g->parts[i]; arguments[i + 1].buffer.bytes = g->control->part[i].bytes;
    }
    arguments[nin - 1].buffer.data = g->workspace; arguments[nin - 1].buffer.bytes = (size_t)g->control->workspace_bytes;
    arguments[nin] = arguments[nin - 1];
    if (diagnostics && (!benchmark || invocation == 1)) {
        nm_text("RPC dispatch graph="); nm_number(graph); nm_text(" invocation="); nm_number(invocation);
        nm_text(" readonlyBytes="); nm_number(g->control->payload_bytes); nm_text("\n");
    }
    before = nm_clock();
    code = invoke(handle, (NM_RPC_METHOD << 24) | (nin << 16) | (1U << 8), arguments);
    after = nm_clock(); *invoke_ticks = after - before;
    if (code) return status("whole graph invoke", code);
    if (reply->magic != NM_RPC_MAGIC || reply->version != NM_RPC_VERSION || reply->invocation != invocation ||
        reply->status || reply->completed_nodes != g->artifact->nodes || reply->hvx_locked != 1 ||
        reply->hvx_unlock_status) {
        nm_text("DSP FP32 failure code="); nm_number(reply->status); nm_text(" completed=");
        nm_number(reply->completed_nodes); nm_text(" failedNode="); nm_number(reply->failed_node);
        nm_text(" hvx128="); nm_number(reply->hvx_locked); nm_text("\n");
        if (reply->status == NM_RPC_WORKER_ERROR) {
            const NmRpcProfile *p = (const NmRpcProfile *)(g->workspace + NM_RPC_REPLY_BYTES);
            nm_text("DSP worker failure stage="); nm_number(p->worker_stage);
            nm_text(" status="); nm_number((u32)p->worker_status); nm_text(" availableApiMask=");
            nm_number(p->worker_api_mask); nm_text(" (expected127)\n");
        }
        return nm_error("DSP graph execution or HVX resource gate failed");
    }
    started = nm_clock();
    if (!transfer(g, outputs, no, QNN_TENSOR_TYPE_APP_READ)) return 0;
    *copy_ticks = input_copy_ticks + (nm_clock() - started);
    if (diagnostics && (!benchmark || invocation == 1)) {
        nm_text("DSP FP32 reply invocation="); nm_number(invocation); nm_text(" code="); nm_number(reply->status);
        nm_text(" completed="); nm_number(reply->completed_nodes); nm_text(" failedNode="); nm_number(reply->failed_node);
        nm_text(" hvx128="); nm_number(reply->hvx_locked); nm_text("\n");
    }
    if (diagnostics) {
        const NmRpcProfile *p = (const NmRpcProfile *)(g->workspace + NM_RPC_REPLY_BYTES);
        nm_text("DSP_PROFILE graph="); nm_number(graph); nm_text(" invocation="); nm_number(invocation);
        nm_text(" workers="); nm_number(p->workers);
        nm_text(" powerRequested="); nm_number(g->control->flags & NM_RPC_TURBO);
        nm_text(" hvxCounterEnabled="); nm_number((reply->reserved >> 1) & 1U);
        nm_text(" alignedCoefficients="); nm_number((g->control->flags & NM_RPC_ALIGN_COEFFICIENTS) != 0);
        nm_text(" frequency="); nm_number(p->frequency); nm_text(" entry="); nm_number(p->entry_ticks);
        nm_text(" elapsed="); nm_number(p->elapsed_ticks); nm_text(" validation="); nm_number(p->validation_ticks);
        nm_text(" coreCycles="); nm_number(reply->cycles);
        nm_text(" matmulScalarTicks="); nm_number(reply->matmul_scalar_ticks);
        nm_text(" matmulHvxTicks="); nm_number(reply->matmul_hvx_ticks);
        nm_text(" matmulUnknownTicks="); nm_number((reply->reserved & 2U) ? 0 : p->op[NM_FP32_MATMUL].ticks);
        nm_text(" lock="); nm_number(p->hvx_lock_ticks); nm_text(" kernels="); nm_number(p->kernel_ticks);
        nm_text(" pureKernels="); nm_number(p->pure_kernel_ticks); nm_text(" workerAdmin=");
        nm_number(p->worker_admin_ticks);
        nm_text(" unlock="); nm_number(p->hvx_unlock_ticks); nm_text(" workspaceCopy="); nm_number(p->workspace_copy_ticks);
        nm_text(" weightCache="); nm_number(p->weight_cache_mode); nm_text(" workspaceCache="); nm_number(p->workspace_cache_mode);
        nm_text(" weightCacheStatus="); nm_number((u32)p->weight_cache_status);
        nm_text(" workspaceCacheStatus="); nm_number((u32)p->workspace_cache_status);
        nm_text(" powerStatus="); nm_number((u32)p->power_status); nm_text(" powerReleaseStatus=");
        nm_number((u32)p->power_release_status);
        nm_text(" hvxVectors32="); nm_number(p->hvx_vectors); nm_text(" hostInvokeTicks="); nm_number(*invoke_ticks);
        nm_text(" hostCopyTicks="); nm_number(*copy_ticks); nm_text(" hostFrequency="); nm_number(nm_frequency()); nm_text("\n");
        for (i = 0; i < NM_RPC_PROFILE_OPS; ++i) if (p->op[i].calls) {
            nm_text("DSP_OP graph="); nm_number(graph); nm_text(" opcode="); nm_number(i);
            nm_text(" calls="); nm_number(p->op[i].calls); nm_text(" ticks="); nm_number(p->op[i].ticks);
            nm_text(" pureTicks="); nm_number(p->op[i].pure_ticks);
            nm_text(" adminTicks="); nm_number(p->op[i].ticks > p->op[i].pure_ticks ?
                p->op[i].ticks - p->op[i].pure_ticks : 0);
            nm_text(" elements="); nm_number(p->op[i].elements); nm_text(" hvxVectors32=");
            nm_number(p->op[i].hvx_vectors); nm_text("\n");
        }
        for (i = 0; i < g->control->parts; ++i) {
            nm_text("DSP_PART_ALIGNMENT graph="); nm_number(graph); nm_text(" part="); nm_number(i);
            nm_text(" actualBaseMod128="); nm_number(p->part_base_mod128[i]); nm_text("\n");
        }
        for (i = 0; i < 128; ++i) if (p->coefficient_alignment[i] || p->packed_alignment[i]) {
            nm_text("DSP_COEFFICIENT_ALIGNMENT graph="); nm_number(graph); nm_text(" actualPointerMod128="); nm_number(i);
            nm_text(" tensors="); nm_number(p->coefficient_alignment[i]);
            nm_text(" bytes="); nm_number(p->coefficient_alignment_bytes[i]);
            nm_text(" packedReferences="); nm_number(p->packed_alignment[i]);
            nm_text(" packedBytes="); nm_number(p->packed_alignment_bytes[i]); nm_text("\n");
        }
    }
    return 1;
}
int nm_rpc_cleanup(void) {
    u32 g, i;
    int ok = 1;
    if (opened) { ok = status("close module", close_rpc(handle)); opened = 0; }
    if (release) for (g = 0; g < NM_RPC_HOST_GRAPHS; ++g) {
        if (graphs[g].control) release(graphs[g].control);
        if (graphs[g].workspace) release(graphs[g].workspace);
        for (i = 0; i < NM_RPC_MAX_PARTS; ++i) if (graphs[g].parts[i]) release(graphs[g].parts[i]);
        memset(graphs + g, 0, sizeof(*graphs));
    }
    nm_unload(driver); driver = 0; handle = invocation = 0;
    invoke = 0; close_rpc = 0; allocate = 0; release = 0; return ok;
}
