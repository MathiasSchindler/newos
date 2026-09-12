#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_DECODER_QNN_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_DECODER_QNN_H

#include "qnn_abi.h"
#include "whisper_model.h"

enum {
    WHISPER_DECODER_QNN_MAX_OUTPUTS = 64,
    WHISPER_DECODER_QNN_MAX_LAYERS = 16
};

typedef struct WhisperDecoderQnn WhisperDecoderQnn;

typedef struct WhisperDecoderQnnIds {
    u32 model_id;
    u32 output_count;
    u32 input_id;
    u32 output_ids[WHISPER_DECODER_QNN_MAX_OUTPUTS];
    u32 mlp_layer_count;
    u32 mlp_input_ids[WHISPER_DECODER_QNN_MAX_LAYERS];
    u32 mlp_output_ids[WHISPER_DECODER_QNN_MAX_LAYERS];
} WhisperDecoderQnnIds;

WhisperDecoderQnn *whisper_decoder_qnn_create(const WhisperModelConfig *model);
int whisper_decoder_qnn_build(
    WhisperDecoderQnn *decoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    WhisperDecoderQnnIds *ids_out
);
int whisper_decoder_qnn_restore(
    WhisperDecoderQnn *decoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    const WhisperDecoderQnnIds *ids
);
u64 whisper_decoder_qnn_execute(
    WhisperDecoderQnn *decoder,
    const QnnInterfaceV2 *api,
    const u16 *encoder_output
);
const u16 *whisper_decoder_qnn_keys(const WhisperDecoderQnn *decoder);
const u16 *whisper_decoder_qnn_values(const WhisperDecoderQnn *decoder);
int whisper_decoder_qnn_mlp_offload(
    void *context,
    u32 layer,
    const float *normalized,
    float *projected,
    u64 *execute_ticks
);
void whisper_decoder_qnn_shutdown(WhisperDecoderQnn *decoder);

#endif