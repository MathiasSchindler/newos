#ifndef NM_FASTRPC_HOST_H
#define NM_FASTRPC_HOST_H
#include "../nemotron_windows.h"
#define NM_RPC_HOST_GRAPHS 5U
void nm_rpc_diagnostics(int enabled);
int nm_rpc_initialize(void);
int nm_rpc_stage(u32 graph, const NmArtifact *artifact);
int nm_rpc_execute(u32 graph, QnnTensor *inputs, u32 ni, QnnTensor *outputs, u32 no,
                   u64 *invoke_ticks, u64 *copy_ticks, int benchmark);
int nm_rpc_cleanup(void);
void *nm_rpc_io_view(u32 graph, const char *name, u32 role, u32 bytes);
void nm_rpc_io_stats(u64 *copied, u64 *shared);
void nm_rpc_io_stats_reset(void);
#endif
