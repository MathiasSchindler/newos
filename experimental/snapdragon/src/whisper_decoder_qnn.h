#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_DECODER_QNN_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_DECODER_QNN_H

#include "qnn_abi.h"

#define WHISPER_DECODER_QNN_OUTPUT_COUNT 8U

typedef struct WhisperDecoderQnnIds {
    u32 input_id;
    u32 output_ids[WHISPER_DECODER_QNN_OUTPUT_COUNT];
} WhisperDecoderQnnIds;

int whisper_decoder_qnn_build(
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    WhisperDecoderQnnIds *ids_out
);
int whisper_decoder_qnn_restore(
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    const WhisperDecoderQnnIds *ids
);
u64 whisper_decoder_qnn_execute(const QnnInterfaceV2 *api, const u16 *encoder_output);
const u16 *whisper_decoder_qnn_keys(void);
const u16 *whisper_decoder_qnn_values(void);

#endif