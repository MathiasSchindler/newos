#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_ARTIFACT_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_ARTIFACT_H

#include "whisper_model.h"

#define WHISPER_ARTIFACT_HEADER_SIZE 96U
#define WHISPER_ARTIFACT_HASH_OFFSET_BASIS 0xcbf29ce484222325ULL

typedef enum WhisperArtifactPayloadType {
    WHISPER_ARTIFACT_PAYLOAD_DECODER_WEIGHTS = 1,
    WHISPER_ARTIFACT_PAYLOAD_TOKEN_BYTES = 2,
    WHISPER_ARTIFACT_PAYLOAD_CROSS_KV_WEIGHTS = 3,
    WHISPER_ARTIFACT_PAYLOAD_QNN_CONTEXT = 4,
    WHISPER_ARTIFACT_PAYLOAD_FRONTEND_WEIGHTS = 5,
    WHISPER_ARTIFACT_PAYLOAD_ENCODER_WEIGHTS = 6
} WhisperArtifactPayloadType;

typedef enum WhisperArtifactElementType {
    WHISPER_ARTIFACT_ELEMENT_F32 = 1,
    WHISPER_ARTIFACT_ELEMENT_F16 = 2,
    WHISPER_ARTIFACT_ELEMENT_U8 = 3,
    WHISPER_ARTIFACT_ELEMENT_BLOB = 4
} WhisperArtifactElementType;

typedef struct WhisperArtifactHeader {
    unsigned int model_id;
    unsigned int payload_type;
    unsigned int element_type;
    unsigned long long element_count;
    unsigned long long payload_size;
    unsigned long long payload_hash;
    unsigned int width;
    unsigned int ffn_width;
    unsigned int attention_heads;
    unsigned int encoder_layers;
    unsigned int decoder_layers;
    unsigned int vocabulary_size;
    unsigned int text_context;
    unsigned int mel_bins;
    unsigned int encoder_frames;
} WhisperArtifactHeader;

void whisper_artifact_encode_header(
    unsigned char output[WHISPER_ARTIFACT_HEADER_SIZE],
    const WhisperArtifactHeader *header
);
int whisper_artifact_decode_header(
    const unsigned char input[WHISPER_ARTIFACT_HEADER_SIZE],
    WhisperArtifactHeader *header
);
int whisper_artifact_header_valid(
    const WhisperArtifactHeader *header,
    const WhisperModelConfig *model,
    unsigned int payload_type,
    unsigned int element_type,
    unsigned long long element_count,
    unsigned long long payload_size
);
int whisper_artifact_payload_valid(
    const WhisperArtifactHeader *header,
    const void *payload,
    unsigned long long payload_size
);
unsigned long long whisper_artifact_hash_update(
    unsigned long long hash,
    const void *data,
    unsigned long long size
);

#endif
