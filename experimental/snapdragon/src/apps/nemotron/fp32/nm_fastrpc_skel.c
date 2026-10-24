#include "protocol.h"
#include "nm_fp32.h"
#include <qurt_hvx.h>
#include <qurt_sclk.h>
#include <qurt_cycles.h>
#include <qurt_memory.h>
#include <qurt_error.h>
#include <HAP_power.h>
#include <qurt_thread.h>
#include <qurt_sem.h>
#pragma weak qurt_hvx_lock
#pragma weak qurt_hvx_unlock
#pragma weak qurt_sysclock_get_hw_ticks
#pragma weak qurt_get_core_pcycles
#pragma weak qurt_mem_region_query
#pragma weak qurt_mem_region_attr_get
#pragma weak HAP_power_set
#pragma weak HAP_power_destroy_client
#pragma weak qurt_thread_create
#pragma weak qurt_thread_join
#pragma weak qurt_thread_exit
#pragma weak qurt_sem_init_val
#pragma weak qurt_sem_destroy
#pragma weak qurt_sem_down
#pragma weak qurt_sem_add
#ifndef NM_RPC_HVX_COUNTER_ENABLED
#define NM_RPC_HVX_COUNTER_ENABLED 0
#endif
volatile unsigned long long nm_rpc_hvx_output_vectors[64];
static volatile uint32_t execution_busy;
typedef struct NmRpcJob {
    const uint32_t *metadata;
    size_t words, ninputs, output_count;
    const void *inputs[4];
    size_t counts[4];
    float *output;
    int stop;
} NmRpcJob;
typedef struct NmRpcWorker {
    qurt_sem_t start;
    qurt_thread_t thread;
    unsigned int slice;
    int code, acquired;
    uint64_t elapsed;
} NmRpcWorker;
static NmRpcJob job;
static NmRpcWorker workers[3];
static qurt_sem_t finished;
static unsigned char worker_stacks[3][32768] __attribute__((aligned(128)));
static uint64_t vector_count(void) {
    return nm_rpc_hvx_output_vectors[0] + nm_rpc_hvx_output_vectors[16] +
           nm_rpc_hvx_output_vectors[32] + nm_rpc_hvx_output_vectors[48];
}
static void worker_entry(void *arg) {
    NmRpcWorker *w = arg;
    w->code = qurt_hvx_lock(QURT_HVX_MODE_128B);
    w->acquired = !w->code;
    qurt_sem_add(&finished, 1);
    if (w->acquired) for (;;) {
        qurt_sem_down(&w->start);
        if (job.stop) break;
        {
            uint64_t before = qurt_sysclock_get_hw_ticks();
            w->code = nm_fp32_execute(job.metadata, job.words, job.inputs, job.counts, job.ninputs,
                                     job.output, job.output_count, w->slice, 4);
            w->elapsed = qurt_sysclock_get_hw_ticks() - before;
        }
        qurt_sem_add(&finished, 1);
    }
    qurt_thread_exit(w->acquired ? qurt_hvx_unlock() : w->code);
}
static int start_workers(unsigned int *created, NmRpcProfile *profile) {
    unsigned int i;
    *created = 0; job.stop = 0;
    profile->worker_stage = 1;
    profile->worker_api_mask = (qurt_thread_create ? 1U : 0) | (qurt_thread_join ? 2U : 0) |
        (qurt_thread_exit ? 4U : 0) | (qurt_sem_init_val ? 8U : 0) | (qurt_sem_destroy ? 16U : 0) |
        (qurt_sem_down ? 32U : 0) | (qurt_sem_add ? 64U : 0);
    if (profile->worker_api_mask != 127) { profile->worker_status = -1; return 1; }
    qurt_sem_init_val(&finished, 0);
    for (i = 0; i < 3; ++i) {
        qurt_thread_attr_t attr;
        qurt_sem_init_val(&workers[i].start, 0);
        workers[i].slice = i + 1; workers[i].code = 0; workers[i].acquired = 0;
        qurt_thread_attr_init(&attr);
        qurt_thread_attr_set_stack_addr(&attr, worker_stacks[i]);
        qurt_thread_attr_set_stack_size(&attr, sizeof(worker_stacks[i]));
        qurt_thread_attr_set_priority(&attr, 128);
        qurt_thread_attr_set_detachstate(&attr, QURT_THREAD_ATTR_CREATE_JOINABLE);
        profile->worker_stage = 2;
        profile->worker_status = qurt_thread_create(&workers[i].thread, &attr, worker_entry, workers + i);
        if (profile->worker_status) {
            qurt_sem_destroy(&workers[i].start);
            if (!*created) qurt_sem_destroy(&finished);
            return 1;
        }
        ++*created;
        qurt_sem_down(&finished);
        profile->worker_stage = 3;
        if (!workers[i].acquired) { profile->worker_status = workers[i].code; return 1; }
    }
    profile->worker_stage = 0;
    return 0;
}
static int stop_workers(unsigned int created, NmRpcProfile *profile) {
    unsigned int i;
    int failed = 0;
    job.stop = 1;
    for (i = 0; i < created; ++i) if (workers[i].acquired) qurt_sem_add(&workers[i].start, 1);
    for (i = 0; i < created; ++i) {
        int status = 0;
        int joined = qurt_thread_join(workers[i].thread, &status);
        if (joined || status) {
            profile->worker_stage = 4;
            profile->worker_status = joined ? joined : status;
            failed = 1;
        }
        qurt_sem_destroy(&workers[i].start);
    }
    if (created) qurt_sem_destroy(&finished);
    return failed;
}
static int cache_mode(void *address, uint32_t *mode) {
    qurt_mem_region_t region;
    qurt_mem_region_attr_t attr;
    qurt_mem_cache_mode_t value;
    int code;
    *mode = UINT32_MAX;
    if (!qurt_mem_region_query || !qurt_mem_region_attr_get) return -1;
    code = qurt_mem_region_query(&region, (qurt_addr_t)(uintptr_t)address, (qurt_paddr_t)QURT_EINVALID);
    if (code) return code;
    code = qurt_mem_region_attr_get(region, &attr);
    if (!code) { qurt_mem_region_attr_get_cache_mode(&attr, &value); *mode = (uint32_t)value; }
    return code;
}

_Static_assert(sizeof(void *) == 4 && sizeof(NmRpcArg) == 8, "Hexagon RPC ABI");
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const unsigned char *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }
static int name_is(const unsigned char *p, const char *s) {
    unsigned int i;
    for (i = 0; i < 64; ++i) {
        if (p[i] != (unsigned char)s[i]) return 0;
        if (!s[i]) return 1;
    }
    return 0;
}
static int metadata_kind(const uint32_t *meta, size_t words, uint32_t values) {
    size_t cursor = NM_FP32_META_BASE;
    uint32_t i, packed = 0;
    if (!meta || words < NM_FP32_META_BASE || meta[0] != NM_FP32_META_MAGIC ||
        (meta[1] != NM_FP32_META_VERSION && meta[1] != NM_FP32_PACKED_VERSION) ||
        meta[3] != values || meta[33] > NM_FP32_MAX_PARAMS) return 0;
    for (i = 0; i < meta[33]; ++i) {
        uint32_t code, count;
        if (words - cursor < 2) return 0;
        code = meta[cursor]; count = meta[cursor + 1]; cursor += 2;
        if (!count || count > words - cursor) return 0;
        if (code == NM_FP32_WEIGHT_TILES) {
            if (packed || meta[1] != NM_FP32_PACKED_VERSION || meta[2] != NM_FP32_MATMUL ||
                count != 1 || meta[cursor] != 32) return 0;
            packed = 1;
        }
        cursor += count;
    }
    if (cursor != words || (meta[1] == NM_FP32_PACKED_VERSION && !packed)) return 0;
    return packed ? 2 : 1;
}
static uint32_t tensor_bytes(const unsigned char *t) {
    uint32_t rank = get32(t + 72), i;
    uint64_t n = 4;
    if (rank < 1 || rank > 4 ||
        (get32(t + 68) != NM_FP32_DTYPE_FLOAT && get32(t + 68) != NM_FP32_DTYPE_INT &&
         !(get32(t + 64) == 4 && get32(t + 68) == 0x0132U))) return 0;
    for (i = 0; i < rank; ++i) {
        uint32_t d = get32(t + 80 + i * 4);
        if (!d || n > NM_RPC_MAX_BUFFER / d) return 0;
        n *= d;
    }
    return (uint32_t)n;
}
static void *pointer(const NmRpcControl *c, const uint64_t *arena, NmRpcArg *args,
                     unsigned char *workspace, const unsigned char *t, uint32_t index, uint32_t bytes) {
    uint32_t p;
    if (get32(t + 64) == 4) {
        uint64_t offset = get64(t + 96);
        if (get64(t + 104) != bytes) return 0;
        for (p = 0; p < c->parts; ++p) {
            if (offset >= c->part[p].offset && offset - c->part[p].offset <= c->part[p].bytes &&
                bytes <= c->part[p].bytes - (offset - c->part[p].offset))
                return (unsigned char *)args[p + 1].buffer.data + (size_t)(offset - c->part[p].offset);
        }
        return 0;
    }
    if ((arena[index] & 127) || arena[index] < ((NM_RPC_WORKSPACE_PREFIX + c->tensors + 127U) & ~127U) ||
        arena[index] > c->workspace_bytes || bytes > c->workspace_bytes - arena[index]) return 0;
    return workspace + (size_t)arena[index];
}
__attribute__((visibility("default")))
int nm_fastrpc_skel_invoke(uint64_t handle, uint32_t scalars, NmRpcArg *args) {
    NmRpcControl *c;
    NmRpcReply *reply;
    NmRpcProfile *profile;
    unsigned char *workspace, *available;
    const unsigned char *table;
    const uint64_t *arena;
    uint32_t ni = (scalars >> 16) & 255U, no = (scalars >> 8) & 255U, i, j;
    uint64_t expected_offset = 0;
    uint64_t arena_end = 0;
    uint64_t entered, copy_started, started;
    uint64_t core_entered = 0;
    int locked = 0, power_active = 0, power_client = 0, guarded = 0;
    unsigned int created = 0;
    (void)handle;
    if (((scalars >> 24) & 31U) < 2) return 0;
    if (!qurt_sysclock_get_hw_ticks) return 8;
    entered = qurt_sysclock_get_hw_ticks();
    if (qurt_get_core_pcycles) core_entered = qurt_get_core_pcycles();
    if (!args || (scalars >> 24) != NM_RPC_METHOD || no != 1 || ni < 3 ||
        ni > NM_RPC_MAX_PARTS + 2 || (scalars & 255U) || !args[0].buffer.data ||
        args[0].buffer.bytes < sizeof(NmRpcControl) || ((uintptr_t)args[0].buffer.data & 7)) return 14;
    c = args[0].buffer.data;
    if (c->magic != NM_RPC_MAGIC || c->version != NM_RPC_VERSION ||
        !c->parts || c->parts > NM_RPC_MAX_PARTS || ni != c->parts + 2 ||
        !c->tensors || c->tensors > NM_RPC_MAX_TENSORS ||
        !c->nodes || c->nodes > NM_RPC_MAX_NODES ||
        (c->flags & ~(NM_RPC_TURBO | NM_RPC_ALIGN_COEFFICIENTS)) ||
        (c->workers != 1 && c->workers != 4) || c->reserved[0] || c->reserved[1] ||
        c->source_payload_bytes < c->metadata_bytes ||
        c->source_payload_bytes > (uint64_t)NM_RPC_MAX_PARTS * NM_RPC_MAX_BUFFER ||
        (!(c->flags & NM_RPC_ALIGN_COEFFICIENTS) && c->source_payload_bytes != c->payload_bytes) ||
        args[0].buffer.bytes != sizeof(*c) + (size_t)c->tensors * 8 ||
        c->workspace_bytes > NM_RPC_MAX_BUFFER || c->workspace_bytes < NM_RPC_WORKSPACE_PREFIX + c->tensors ||
        args[ni].buffer.bytes != c->workspace_bytes || args[ni - 1].buffer.bytes != c->workspace_bytes ||
        !args[ni].buffer.data || !args[ni - 1].buffer.data) return 14;
    workspace = args[ni].buffer.data;
    copy_started = qurt_sysclock_get_hw_ticks();
    if (workspace != args[ni - 1].buffer.data) {
        const unsigned char *source = args[ni - 1].buffer.data;
        for (i = 0; i < (uint32_t)c->workspace_bytes; ++i) workspace[i] = source[i];
    }
    reply = (NmRpcReply *)workspace;
    if (reply->magic != NM_RPC_MAGIC || reply->version != NM_RPC_VERSION ||
        reply->invocation != c->invocation) return 14;
    reply->status = NM_RPC_BAD_CONTROL; reply->failed_node = UINT32_MAX;
    reply->completed_nodes = 0; reply->hvx_locked = 0; reply->hvx_unlock_status = 0; reply->cycles = 0;
    reply->matmul_scalar_ticks = 0; reply->matmul_hvx_ticks = 0;
    profile = (NmRpcProfile *)(workspace + NM_RPC_REPLY_BYTES);
    {
        volatile unsigned char *bytes = (volatile unsigned char *)profile;
        for (i = 0; i < sizeof(*profile); ++i) bytes[i] = 0;
    }
    profile->frequency = QURT_SYSCLOCK_TIMETICK_FROM_US(1000000ULL);
    profile->entry_ticks = entered;
    profile->workers = c->workers;
    profile->workspace_copy_ticks = qurt_sysclock_get_hw_ticks() - copy_started;
    profile->weight_cache_status = cache_mode(args[1].buffer.data, &profile->weight_cache_mode);
    profile->workspace_cache_status = cache_mode(workspace, &profile->workspace_cache_mode);
    for (i = 0; i < c->parts; ++i) {
        if (!c->part[i].bytes || c->part[i].bytes > NM_RPC_MAX_BUFFER ||
            c->part[i].offset != expected_offset || c->part[i].reserved ||
            args[i + 1].buffer.bytes != c->part[i].bytes || !args[i + 1].buffer.data) return 0;
        profile->part_base_mod128[i] = (uint32_t)((uintptr_t)args[i + 1].buffer.data & 127U);
        if ((c->flags & NM_RPC_ALIGN_COEFFICIENTS) &&
            ((c->part[i].offset & 127U) || profile->part_base_mod128[i])) return 0;
        expected_offset += c->part[i].bytes;
    }
    if (expected_offset != c->payload_bytes ||
        c->metadata_bytes != 64U + c->tensors * NM_RPC_TENSOR_BYTES + c->nodes * NM_RPC_NODE_BYTES ||
        c->metadata_bytes > c->part[0].bytes) return 0;
    arena = (const uint64_t *)(c + 1);
    table = args[1].buffer.data;
    available = workspace + NM_RPC_WORKSPACE_PREFIX;
    reply->status = NM_RPC_BAD_TENSOR;
    for (i = 0; i < c->tensors; ++i) {
        const unsigned char *t = table + 64 + i * NM_RPC_TENSOR_BYTES;
        uint32_t bytes = tensor_bytes(t), type = get32(t + 64);
        void *data;
        if (!bytes || (type != 0 && type != 1 && type != 3 && type != 4) ||
            !(data = pointer(c, arena, args, workspace, t, i, bytes))) return 0;
        if (type == 4) {
            uint32_t residue = (uint32_t)((uintptr_t)data & 127U);
            if ((c->flags & NM_RPC_ALIGN_COEFFICIENTS) && residue) return 0;
            if (get32(t + 68) == NM_FP32_DTYPE_FLOAT) {
                ++profile->coefficient_alignment[residue];
                profile->coefficient_alignment_bytes[residue] += bytes;
            }
        }
        available[i] = type == 0 || type == 4;
        if (type != 4) {
            if (arena[i] < arena_end) return 0;
            arena_end = arena[i] + bytes;
        }
    }
    profile->validation_ticks = qurt_sysclock_get_hw_ticks() - entered;
    if (!__sync_bool_compare_and_swap(&execution_busy, 0, 1)) {
        reply->status = NM_RPC_WORKER_ERROR; return 0;
    }
    guarded = 1;
    if (c->flags & NM_RPC_TURBO) {
        HAP_power_request_t power;
        volatile unsigned char *bytes = (volatile unsigned char *)&power;
        for (i = 0; i < sizeof(power); ++i) bytes[i] = 0;
        reply->status = NM_RPC_POWER_ERROR;
        if (!HAP_power_set || !HAP_power_destroy_client) {
            profile->power_status = -1; goto done;
        }
        power.type = HAP_power_set_DCVS_v3;
        power.dcvs_v3.set_dcvs_enable = TRUE;
        power.dcvs_v3.dcvs_enable = TRUE;
        power.dcvs_v3.dcvs_option = HAP_DCVS_V2_PERFORMANCE_MODE;
        power.dcvs_v3.set_core_params = TRUE;
        power.dcvs_v3.core_params.min_corner = HAP_DCVS_VCORNER_TURBO;
        power.dcvs_v3.core_params.max_corner = HAP_DCVS_VCORNER_TURBO;
        power.dcvs_v3.core_params.target_corner = HAP_DCVS_VCORNER_TURBO;
        power.dcvs_v3.set_bus_params = TRUE;
        power.dcvs_v3.bus_params.min_corner = HAP_DCVS_VCORNER_TURBO;
        power.dcvs_v3.bus_params.max_corner = HAP_DCVS_VCORNER_TURBO;
        power.dcvs_v3.bus_params.target_corner = HAP_DCVS_VCORNER_TURBO;
        power.dcvs_v3.set_latency = TRUE; power.dcvs_v3.latency = 40;
        power_active = 1;
        profile->power_status = HAP_power_set(&power_client, &power);
        if (profile->power_status) goto done;
    }
    reply->status = NM_RPC_HVX_UNAVAILABLE;
    started = qurt_sysclock_get_hw_ticks();
    if (!qurt_hvx_lock || !qurt_hvx_unlock || qurt_hvx_lock(QURT_HVX_MODE_128B)) goto done;
    profile->hvx_lock_ticks = qurt_sysclock_get_hw_ticks() - started;
    locked = 1; reply->hvx_locked = 1;
    if (c->workers == 4 && start_workers(&created, profile)) { reply->status = NM_RPC_WORKER_ERROR; goto done; }
    for (i = 0; i < 4; ++i) nm_rpc_hvx_output_vectors[i * 16] = 0;
    for (i = 0; i < c->nodes; ++i) {
        const unsigned char *node = table + 64 + c->tensors * NM_RPC_TENSOR_BYTES + i * NM_RPC_NODE_BYTES;
        const uint32_t *meta = 0;
        const void *inputs[4];
        size_t counts[4], words = 0;
        float *output = 0;
        size_t output_count = 0;
        uint32_t inputs_count = get32(node + 128), output_index = get32(node + 208);
        int gather = name_is(node + 64, NM_FP32_GATHER);
        reply->failed_node = i; reply->status = NM_RPC_BAD_NODE;
        if ((!gather && !name_is(node + 64, NM_FP32_OPERATOR)) || inputs_count < 2 || inputs_count > 5 ||
            (gather && inputs_count != 3) || get32(node + 132) != 1 || get32(node + 136) ||
            output_index >= c->tensors) goto done;
        for (j = 0; j < inputs_count; ++j) {
            uint32_t index = get32(node + 144 + j * 4);
            const unsigned char *t;
            uint32_t bytes, dtype;
            void *data;
            if (index >= c->tensors || !available[index] || index == output_index) goto done;
            t = table + 64 + index * NM_RPC_TENSOR_BYTES;
            dtype = !j || (gather && j == 2) ? NM_FP32_DTYPE_INT : NM_FP32_DTYPE_FLOAT;
            if (get32(t + 68) != dtype || (!j && get32(t + 64) != 4)) goto done;
            bytes = tensor_bytes(t); data = pointer(c, arena, args, workspace, t, index, bytes);
            if (!j) { meta = data; words = bytes / 4; }
            else { inputs[j - 1] = data; counts[j - 1] = bytes / 4; }
        }
        {
            int kind = metadata_kind(meta, words, inputs_count - 1);
            if (!kind || (kind == 2 && (inputs_count != 3 ||
                get32(table + 64 + get32(node + 152) * NM_RPC_TENSOR_BYTES + 64) != 4))) goto done;
            if (kind == 2) {
                uint32_t residue = (uint32_t)((uintptr_t)inputs[1] & 127U);
                ++profile->packed_alignment[residue];
                profile->packed_alignment_bytes[residue] += (uint64_t)counts[1] * 4;
            }
        }
        {
            const unsigned char *t = table + 64 + output_index * NM_RPC_TENSOR_BYTES;
            uint32_t bytes = tensor_bytes(t);
            if (get32(t + 68) != NM_FP32_DTYPE_FLOAT || get32(t + 64) == 4 || get32(t + 64) == 0) goto done;
            output = pointer(c, arena, args, workspace, t, output_index, bytes); output_count = bytes / 4;
        }
        reply->status = NM_RPC_KERNEL_ERROR;
        {
            uint64_t before = qurt_sysclock_get_hw_ticks(), vectors = vector_count();
            uint32_t opcode = words >= 3 ? meta[2] : 0;
            uint64_t call_started, pure;
            int code;
            if (c->workers == 4) {
                job.metadata = meta; job.words = words; job.ninputs = inputs_count - 1;
                job.output = output; job.output_count = output_count;
                for (j = 0; j < inputs_count - 1; ++j) { job.inputs[j] = inputs[j]; job.counts[j] = counts[j]; }
                for (j = 0; j < 3; ++j) {
                    workers[j].elapsed = 0;
                    qurt_sem_add(&workers[j].start, 1);
                }
            }
            call_started = qurt_sysclock_get_hw_ticks();
            code = nm_fp32_execute(meta, words, inputs, counts, inputs_count - 1, output, output_count, 0, c->workers);
            pure = qurt_sysclock_get_hw_ticks() - call_started;
            if (c->workers == 4) {
                for (j = 0; j < 3; ++j) qurt_sem_down(&finished);
                for (j = 0; j < 3; ++j) if (workers[j].code) code = workers[j].code;
                for (j = 0; j < 3; ++j) if (workers[j].elapsed > pure) pure = workers[j].elapsed;
            }
            uint64_t elapsed = qurt_sysclock_get_hw_ticks() - before;
            if (opcode == NM_FP32_MATMUL && NM_RPC_HVX_COUNTER_ENABLED) {
                if (vector_count() != vectors) reply->matmul_hvx_ticks += elapsed;
                else reply->matmul_scalar_ticks += elapsed;
            }
            profile->kernel_ticks += elapsed;
            profile->pure_kernel_ticks += pure;
            if (elapsed > pure) profile->worker_admin_ticks += elapsed - pure;
            if (opcode < NM_RPC_PROFILE_OPS) {
                profile->op[opcode].ticks += elapsed;
                profile->op[opcode].pure_ticks += pure;
                profile->op[opcode].elements += output_count;
                profile->op[opcode].hvx_vectors += vector_count() - vectors;
                ++profile->op[opcode].calls;
            }
            if (code) goto done;
        }
        available[output_index] = 1; ++reply->completed_nodes;
    }
    for (i = 0; i < c->tensors; ++i)
        if (get32(table + 64 + i * NM_RPC_TENSOR_BYTES + 64) == 1 && !available[i]) {
            reply->status = NM_RPC_BAD_GRAPH; goto done;
        }
    reply->status = NM_RPC_OK; reply->failed_node = UINT32_MAX;
done:
    if (created && stop_workers(created, profile) && reply->status == NM_RPC_OK) reply->status = NM_RPC_WORKER_ERROR;
    if (locked) {
        started = qurt_sysclock_get_hw_ticks();
        int status = qurt_hvx_unlock();
        profile->hvx_unlock_ticks = qurt_sysclock_get_hw_ticks() - started;
        reply->hvx_unlock_status = (uint32_t)status;
        if (status && reply->status == NM_RPC_OK) reply->status = NM_RPC_HVX_UNLOCK_ERROR;
    }
    if (power_active) {
        profile->power_release_status = HAP_power_destroy_client(&power_client);
        if (profile->power_release_status && reply->status == NM_RPC_OK) reply->status = NM_RPC_POWER_ERROR;
    }
    profile->hvx_vectors = vector_count();
    profile->elapsed_ticks = qurt_sysclock_get_hw_ticks() - entered;
    reply->reserved = 1U | (NM_RPC_HVX_COUNTER_ENABLED ? 2U : 0U);
    if (qurt_get_core_pcycles) reply->cycles = qurt_get_core_pcycles() - core_entered;
    if (guarded) __sync_lock_release(&execution_busy);
    return 0;
}
