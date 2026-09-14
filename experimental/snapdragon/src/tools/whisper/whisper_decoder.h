#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_DECODER_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_DECODER_H

#include "whisper_model.h"

#define WHISPER_DECODER_MAX_TOKENS WHISPER_TINY_TEXT_CONTEXT

typedef void (*WhisperDecoderWrite)(const char *data, unsigned int size);
typedef int (*WhisperDecoderCancelled)(void *context);
enum { WHISPER_DECODER_CANCELLED = -2 };
typedef int (*WhisperDecoderMlpOffload)(
    void *context,
    unsigned int layer,
    const float *normalized,
    float *projected,
    unsigned long long *execute_ticks
);
typedef int (*WhisperDecoderCrossAttentionOffload)(
    void *context,
    unsigned int layer,
    const float *hidden,
    float *projected,
    unsigned long long *execute_ticks
);
typedef int (*WhisperDecoderFusedCrossMlpOffload)(
    void *context,
    unsigned int layer,
    const float *hidden,
    float *output,
    unsigned long long *execute_ticks
);
typedef int (*WhisperDecoderLogitsOffload)(
    void *context,
    const float *hidden,
    const unsigned short **logits,
    unsigned long long *execute_ticks
);
typedef int (*WhisperDecoderSelfAttentionOffload)(
    void *context,
    unsigned int layer,
    unsigned int position,
    const float *hidden,
    float *projected,
    unsigned long long *execute_ticks
);
typedef struct WhisperDecoder WhisperDecoder;
void whisper_decoder_set_cancellation(
    WhisperDecoder *decoder, WhisperDecoderCancelled cancelled, void *context
);

typedef struct WhisperDecoderNpuCalls {
    unsigned long long maximum_ticks;
    unsigned int offload_calls;
    unsigned int graph_submissions;
    unsigned int over_10ms;
    unsigned int over_100ms;
    unsigned int over_1000ms;
} WhisperDecoderNpuCalls;

typedef struct WhisperDecoderProfile {
    unsigned long long encoder_normalize_ticks;
    unsigned long long cross_cache_ticks;
    unsigned long long self_attention_ticks;
    unsigned long long npu_self_attention_ticks;
    unsigned long long npu_self_attention_execute_ticks;
    unsigned long long cross_attention_ticks;
    unsigned long long npu_cross_attention_ticks;
    unsigned long long npu_cross_attention_execute_ticks;
    unsigned long long npu_fused_cross_mlp_ticks;
    unsigned long long npu_fused_cross_mlp_execute_ticks;
    unsigned long long feed_forward_ticks;
    unsigned long long npu_feed_forward_ticks;
    unsigned long long npu_mlp_execute_ticks;
    unsigned long long logits_ticks;
    unsigned long long npu_logits_ticks;
    unsigned long long npu_logits_execute_ticks;
    WhisperDecoderNpuCalls npu_self_attention_calls;
    WhisperDecoderNpuCalls npu_cross_attention_calls;
    WhisperDecoderNpuCalls npu_fused_cross_mlp_calls;
    WhisperDecoderNpuCalls npu_mlp_calls;
    WhisperDecoderNpuCalls npu_logits_calls;
    unsigned int decoder_steps;
    unsigned int prefix_reused_steps;
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
void whisper_decoder_set_cross_attention_offload(
    WhisperDecoder *decoder,
    WhisperDecoderCrossAttentionOffload offload,
    void *context
);
void whisper_decoder_set_fused_cross_mlp_offload(
    WhisperDecoder *decoder,
    WhisperDecoderFusedCrossMlpOffload offload,
    void *context
);
void whisper_decoder_set_logits_offload(
    WhisperDecoder *decoder,
    WhisperDecoderLogitsOffload offload,
    void *context
);
void whisper_decoder_set_self_attention_offload(
    WhisperDecoder *decoder,
    WhisperDecoderSelfAttentionOffload offload,
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
