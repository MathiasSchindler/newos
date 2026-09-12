#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_DECODER_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_DECODER_H

#include "whisper_model.h"

#define WHISPER_DECODER_MAX_TOKENS WHISPER_TINY_TEXT_CONTEXT

typedef void (*WhisperDecoderWrite)(const char *data, unsigned int size);
typedef int (*WhisperDecoderMlpOffload)(
    void *context,
    unsigned int layer,
    const float *normalized,
    float *projected,
    unsigned long long *execute_ticks
);
typedef struct WhisperDecoder WhisperDecoder;

typedef struct WhisperDecoderProfile {
    unsigned long long encoder_normalize_ticks;
    unsigned long long cross_cache_ticks;
    unsigned long long self_attention_ticks;
    unsigned long long cross_attention_ticks;
    unsigned long long feed_forward_ticks;
    unsigned long long npu_feed_forward_ticks;
    unsigned long long npu_mlp_execute_ticks;
    unsigned long long logits_ticks;
    unsigned int decoder_steps;
    unsigned int worker_count;
} WhisperDecoderProfile;

WhisperDecoder *whisper_decoder_load(const WhisperModelConfig *model);
WhisperDecoder *whisper_decoder_load_with_workers(
    const WhisperModelConfig *model,
    unsigned int worker_count
);
void whisper_decoder_set_mlp_offload(
    WhisperDecoder *decoder,
    WhisperDecoderMlpOffload offload,
    void *context
);
int whisper_decoder_transcribe(
    WhisperDecoder *decoder,
    const unsigned short *encoder_output,
    unsigned int maximum_tokens,
    WhisperDecoderWrite write_output
);
int whisper_decoder_transcribe_with_cross_cache(
    WhisperDecoder *decoder,
    const unsigned short *cross_keys,
    const unsigned short *cross_values,
    unsigned int maximum_tokens,
    WhisperDecoderWrite write_output
);
const WhisperDecoderProfile *whisper_decoder_get_profile(const WhisperDecoder *decoder);
void whisper_decoder_shutdown(WhisperDecoder *decoder);

#endif
