#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_MODEL_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_MODEL_H

typedef struct WhisperModelConfig {
    unsigned int model_id;
    const char *name;
    unsigned int width;
    unsigned int ffn_width;
    unsigned int attention_heads;
    unsigned int encoder_layers;
    unsigned int decoder_layers;
    unsigned int vocabulary_size;
    unsigned int text_context;
    unsigned int mel_bins;
    unsigned int encoder_frames;
} WhisperModelConfig;

enum {
    WHISPER_MODEL_ID_TINY = 1,
    WHISPER_MODEL_ID_BASE = 2,
    WHISPER_TINY_WIDTH = 384,
    WHISPER_TINY_FFN_WIDTH = 1536,
    WHISPER_TINY_ATTENTION_HEADS = 6,
    WHISPER_TINY_ENCODER_LAYERS = 4,
    WHISPER_TINY_DECODER_LAYERS = 4,
    WHISPER_TINY_VOCABULARY_SIZE = 51865,
    WHISPER_TINY_TEXT_CONTEXT = 448,
    WHISPER_TINY_MEL_BINS = 80,
    WHISPER_TINY_ENCODER_FRAMES = 1500,
    WHISPER_TINY_DECODER_FLOAT_COUNT =
        (WHISPER_TINY_VOCABULARY_SIZE + WHISPER_TINY_TEXT_CONTEXT + 4) *
            WHISPER_TINY_WIDTH +
        WHISPER_TINY_DECODER_LAYERS *
            (16 * WHISPER_TINY_WIDTH * WHISPER_TINY_WIDTH +
             17 * WHISPER_TINY_WIDTH),
    WHISPER_TINY_CROSS_KV_OUTPUT_COUNT = WHISPER_TINY_DECODER_LAYERS * 2,
    WHISPER_TINY_CROSS_KV_WEIGHT_VALUES =
        2 * WHISPER_TINY_WIDTH +
        WHISPER_TINY_DECODER_LAYERS *
            (2 * WHISPER_TINY_WIDTH * WHISPER_TINY_WIDTH +
             WHISPER_TINY_WIDTH),
    WHISPER_BASE_WIDTH = 512,
    WHISPER_BASE_FFN_WIDTH = 2048,
    WHISPER_BASE_ATTENTION_HEADS = 8,
    WHISPER_BASE_ENCODER_LAYERS = 6,
    WHISPER_BASE_DECODER_LAYERS = 6,
    WHISPER_BASE_VOCABULARY_SIZE = 51865,
    WHISPER_BASE_TEXT_CONTEXT = 448,
    WHISPER_BASE_MEL_BINS = 80,
    WHISPER_BASE_ENCODER_FRAMES = 1500
};

const WhisperModelConfig *whisper_model_tiny(void);
const WhisperModelConfig *whisper_model_base(void);
int whisper_model_config_valid(const WhisperModelConfig *config);
int whisper_model_size_add(
    unsigned long long left,
    unsigned long long right,
    unsigned long long *result
);
int whisper_model_size_multiply(
    unsigned long long left,
    unsigned long long right,
    unsigned long long *result
);
unsigned long long whisper_model_decoder_float_count(const WhisperModelConfig *config);
unsigned long long whisper_model_cross_kv_weight_count(const WhisperModelConfig *config);
unsigned long long whisper_model_frontend_weight_count(const WhisperModelConfig *config);
unsigned long long whisper_model_encoder_weight_count(const WhisperModelConfig *config);

#endif
