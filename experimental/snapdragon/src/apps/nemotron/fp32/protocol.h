#ifndef NM_FASTRPC_PROTOCOL_H
#define NM_FASTRPC_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>

#define NM_RPC_MAGIC 0x4e4d5250U
#define NM_RPC_VERSION 8U
#define NM_RPC_TURBO 1U
#define NM_RPC_ALIGN_COEFFICIENTS 2U
#define NM_RPC_MAX_PARTS 16U
#define NM_RPC_MAX_BUFFER 0x20000000U
#define NM_RPC_MAX_TENSORS 8192U
#define NM_RPC_MAX_NODES 4096U
#define NM_RPC_TENSOR_BYTES 160U
#define NM_RPC_NODE_BYTES 672U
#define NM_RPC_REPLY_BYTES 64U
#define NM_RPC_PROFILE_OPS 22U
#define NM_RPC_METHOD 2U
typedef struct NmRpcPart {
    uint64_t offset;
    uint32_t bytes, reserved;
} NmRpcPart;
typedef struct NmRpcControl {
    uint32_t magic, version, parts, tensors, nodes, metadata_bytes;
    uint64_t payload_bytes, workspace_bytes, invocation;
    uint32_t flags, workers, reserved[2];
    uint64_t source_payload_bytes;
    unsigned char source_payload_sha256[32];
    NmRpcPart part[NM_RPC_MAX_PARTS];
} NmRpcControl;
typedef struct NmRpcReply {
    uint32_t magic, version, status, failed_node, completed_nodes, hvx_locked, hvx_unlock_status, reserved;
    uint64_t invocation, cycles, matmul_scalar_ticks, matmul_hvx_ticks;
} NmRpcReply;
typedef struct NmRpcOpProfile {
    uint64_t ticks, pure_ticks, elements, hvx_vectors;
    uint32_t calls, reserved;
} NmRpcOpProfile;
typedef struct NmRpcProfile {
    uint64_t frequency, entry_ticks, elapsed_ticks, validation_ticks, hvx_lock_ticks;
    uint64_t kernel_ticks, hvx_unlock_ticks, workspace_copy_ticks, hvx_vectors;
    uint64_t pure_kernel_ticks, worker_admin_ticks;
    uint32_t weight_cache_mode, workspace_cache_mode;
    int32_t weight_cache_status, workspace_cache_status, power_status, power_release_status;
    uint32_t workers, worker_stage;
    int32_t worker_status;
    uint32_t worker_api_mask;
    uint32_t part_base_mod128[NM_RPC_MAX_PARTS];
    uint32_t coefficient_alignment[128], packed_alignment[128];
    uint64_t coefficient_alignment_bytes[128], packed_alignment_bytes[128];
    NmRpcOpProfile op[NM_RPC_PROFILE_OPS];
} NmRpcProfile;
#define NM_RPC_WORKSPACE_PREFIX (NM_RPC_REPLY_BYTES + sizeof(NmRpcProfile))
typedef union NmRpcArg {
    struct { void *data; size_t bytes; } buffer;
    uint32_t handle;
    uint64_t handle64;
    struct { int fd; uint32_t offset; } dma;
} NmRpcArg;
enum {
    NM_RPC_OK = 0, NM_RPC_BAD_CONTROL = 1, NM_RPC_BAD_GRAPH = 2,
    NM_RPC_BAD_TENSOR = 3, NM_RPC_BAD_NODE = 4, NM_RPC_HVX_UNAVAILABLE = 5,
    NM_RPC_KERNEL_ERROR = 6, NM_RPC_HVX_UNLOCK_ERROR = 7, NM_RPC_POWER_ERROR = 8, NM_RPC_WORKER_ERROR = 9
};
_Static_assert(sizeof(NmRpcPart) == 16 && sizeof(NmRpcControl) == 360, "RPC control ABI");
_Static_assert(sizeof(NmRpcReply) == NM_RPC_REPLY_BYTES, "RPC reply ABI");
#endif
