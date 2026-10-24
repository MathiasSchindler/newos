#ifndef NEWOS_NEMOTRON_ASR_RUNTIME_H
#define NEWOS_NEMOTRON_ASR_RUNTIME_H
#include "nemotron_runtime.h"
#include "fp32/nm_fastrpc.h"
typedef struct NmAsrGraph {
    NmArtifact artifact;
    u8 *data;
    u64 bytes;
    QnnTensor *inputs, *outputs;
    u32 ni, no;
} NmAsrGraph;
typedef struct NmAsrModel {
    NmAsrGraph graphs[NM_RPC_HOST_GRAPHS];
    NmModuleStage module_stage;
    u64 chunks, graph_calls, invoke_ticks, copy_ticks;
    u64 fused_joiner_reuses;
    float current_encoder[640];
    int rpc_started, shared_io, fused_decoder, have_encoder, fused_ready;
} NmAsrModel;
int nm_asr_model_open(NmAsrModel *model, const NmRuntime *runtime, int diagnostics, int shared_io);
int nm_asr_model_close(NmAsrModel *model);
int nm_asr_model_reset(void *context);
int nm_asr_model_predict(void *context, uint32_t token);
int nm_asr_model_join(void *context, const float *encoder, const float **logits, uint32_t *count);
int nm_asr_model_encode(NmAsrModel *model, const float *features, u32 width, const float **projected);
int nm_asr_binding_copy(QnnTensor *destination, const QnnTensor *source);
#endif
