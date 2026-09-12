#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_ENCODER_QNN_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_ENCODER_QNN_H

#include "qnn_abi.h"
#include "whisper_model.h"

typedef struct WhisperEncoderQnn WhisperEncoderQnn;

typedef struct WhisperEncoderQnnIds {
    u32 model_id;
    u32 frontend_input_ids[2];
    u32 frontend_output_ids[2];
    u32 encoder_input_id;
    u32 encoder_output_id;
} WhisperEncoderQnnIds;

WhisperEncoderQnn *whisper_encoder_qnn_create(const WhisperModelConfig *model);
int whisper_encoder_qnn_build(
    WhisperEncoderQnn *encoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    WhisperEncoderQnnIds *ids_out
);
int whisper_encoder_qnn_restore(
    WhisperEncoderQnn *encoder,
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    const WhisperEncoderQnnIds *ids
);
u64 whisper_encoder_qnn_execute_frontend(
    WhisperEncoderQnn *encoder,
    const QnnInterfaceV2 *api,
    const float *log_mel
);
u64 whisper_encoder_qnn_execute_encoder(
    WhisperEncoderQnn *encoder,
    const QnnInterfaceV2 *api
);
const u16 *whisper_encoder_qnn_frontend_output(const WhisperEncoderQnn *encoder);
const u16 *whisper_encoder_qnn_output(const WhisperEncoderQnn *encoder);
u64 whisper_encoder_qnn_activation_bytes(const WhisperEncoderQnn *encoder);
void whisper_encoder_qnn_release_builder(WhisperEncoderQnn *encoder);
void whisper_encoder_qnn_shutdown(WhisperEncoderQnn *encoder);

#endif