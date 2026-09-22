#include "whisper_model.h"

enum {
    WHISPER_MODEL_MAX_WIDTH = 1280,
    WHISPER_MODEL_MAX_FFN_WIDTH = 5120,
    WHISPER_MODEL_MAX_LAYERS = 32,
    WHISPER_MODEL_MAX_VOCABULARY_SIZE = 65536,
    WHISPER_MODEL_MAX_TEXT_CONTEXT = 448,
    WHISPER_MODEL_MAX_MEL_BINS = 128,
    WHISPER_MODEL_MAX_ENCODER_FRAMES = 1500
};

static WhisperModelConfig tiny_config = {
    WHISPER_MODEL_ID_TINY,
    0,
    WHISPER_TINY_WIDTH,
    WHISPER_TINY_FFN_WIDTH,
    WHISPER_TINY_ATTENTION_HEADS,
    WHISPER_TINY_ENCODER_LAYERS,
    WHISPER_TINY_DECODER_LAYERS,
    WHISPER_TINY_VOCABULARY_SIZE,
    WHISPER_TINY_TEXT_CONTEXT,
    WHISPER_TINY_MEL_BINS,
    WHISPER_TINY_ENCODER_FRAMES
};

static WhisperModelConfig base_config = {
    WHISPER_MODEL_ID_BASE,
    0,
    WHISPER_BASE_WIDTH,
    WHISPER_BASE_FFN_WIDTH,
    WHISPER_BASE_ATTENTION_HEADS,
    WHISPER_BASE_ENCODER_LAYERS,
    WHISPER_BASE_DECODER_LAYERS,
    WHISPER_BASE_VOCABULARY_SIZE,
    WHISPER_BASE_TEXT_CONTEXT,
    WHISPER_BASE_MEL_BINS,
    WHISPER_BASE_ENCODER_FRAMES
};

static WhisperModelConfig medium_config = {
    WHISPER_MODEL_ID_MEDIUM,
    0,
    1024, 4096, 16, 24, 24, 51865, 448, 80, 1500
};

static WhisperModelConfig small_config = {
    WHISPER_MODEL_ID_SMALL,
    0,
    WHISPER_SMALL_WIDTH,
    WHISPER_SMALL_FFN_WIDTH,
    WHISPER_SMALL_ATTENTION_HEADS,
    WHISPER_SMALL_ENCODER_LAYERS,
    WHISPER_SMALL_DECODER_LAYERS,
    WHISPER_SMALL_VOCABULARY_SIZE,
    WHISPER_SMALL_TEXT_CONTEXT,
    WHISPER_SMALL_MEL_BINS,
    WHISPER_SMALL_ENCODER_FRAMES
};

_Static_assert(WHISPER_TINY_DECODER_FLOAT_COUNT == 29553024,
    "Whisper Tiny decoder weight count changed");
_Static_assert(WHISPER_TINY_CROSS_KV_WEIGHT_VALUES == 1181952,
    "Whisper Tiny cross-K/V weight count changed");

const WhisperModelConfig *whisper_model_tiny(void) {
    tiny_config.name = "tiny";
    return &tiny_config;
}

const WhisperModelConfig *whisper_model_base(void) {
    base_config.name = "base";
    return &base_config;
}

const WhisperModelConfig *whisper_model_small(void) {
    small_config.name = "small";
    return &small_config;
}

const WhisperModelConfig *whisper_model_medium(void) {
    medium_config.name = "medium";
    return &medium_config;
}

int whisper_model_config_valid(const WhisperModelConfig *config) {
    if (config == 0 || config->model_id == 0U || config->name == 0) return 0;
    if (config->width == 0U || config->width > WHISPER_MODEL_MAX_WIDTH ||
        config->ffn_width == 0U || config->ffn_width > WHISPER_MODEL_MAX_FFN_WIDTH ||
        config->attention_heads == 0U || config->attention_heads > config->width ||
        config->encoder_layers == 0U ||
        config->encoder_layers > WHISPER_MODEL_MAX_LAYERS ||
        config->decoder_layers == 0U ||
        config->decoder_layers > WHISPER_MODEL_MAX_LAYERS ||
        config->vocabulary_size == 0U ||
        config->vocabulary_size > WHISPER_MODEL_MAX_VOCABULARY_SIZE ||
        config->text_context == 0U ||
        config->text_context > WHISPER_MODEL_MAX_TEXT_CONTEXT ||
        config->mel_bins == 0U || config->mel_bins > WHISPER_MODEL_MAX_MEL_BINS ||
        config->encoder_frames == 0U ||
        config->encoder_frames > WHISPER_MODEL_MAX_ENCODER_FRAMES) {
        return 0;
    }
    if (config->width % config->attention_heads != 0U ||
        config->width / config->attention_heads != 64U ||
        config->ffn_width != config->width * 4U) {
        return 0;
    }
    return 1;
}

int whisper_model_size_add(
    unsigned long long left,
    unsigned long long right,
    unsigned long long *result
) {
    if (result == 0 || left > ~0ULL - right) return 0;
    *result = left + right;
    return 1;
}

int whisper_model_size_multiply(
    unsigned long long left,
    unsigned long long right,
    unsigned long long *result
) {
    if (result == 0 || (right != 0U && left > ~0ULL / right)) return 0;
    *result = left * right;
    return 1;
}

unsigned long long whisper_model_decoder_float_count(const WhisperModelConfig *config) {
    unsigned long long width;
    unsigned long long width_squared;
    unsigned long long global_factor;
    unsigned long long global_values;
    unsigned long long quadratic_values;
    unsigned long long linear_values;
    unsigned long long layer_values;
    unsigned long long all_layer_values;
    unsigned long long total;
    if (!whisper_model_config_valid(config)) return 0U;
    width = config->width;
    if (!whisper_model_size_add(
            config->vocabulary_size, config->text_context, &global_factor
        ) || !whisper_model_size_add(global_factor, 4U, &global_factor) ||
        !whisper_model_size_multiply(global_factor, width, &global_values) ||
        !whisper_model_size_multiply(width, width, &width_squared) ||
        !whisper_model_size_multiply(16U, width_squared, &quadratic_values) ||
        !whisper_model_size_multiply(17U, width, &linear_values) ||
        !whisper_model_size_add(quadratic_values, linear_values, &layer_values) ||
        !whisper_model_size_multiply(
            config->decoder_layers, layer_values, &all_layer_values
        ) || !whisper_model_size_add(global_values, all_layer_values, &total)) {
        return 0U;
    }
    return total;
}

unsigned long long whisper_model_cross_kv_weight_count(const WhisperModelConfig *config) {
    unsigned long long width;
    unsigned long long width_squared;
    unsigned long long projected_values;
    unsigned long long layer_values;
    unsigned long long all_layer_values;
    unsigned long long norm_values;
    unsigned long long total;
    if (!whisper_model_config_valid(config)) return 0U;
    width = config->width;
    if (!whisper_model_size_multiply(width, width, &width_squared) ||
        !whisper_model_size_multiply(4U, width_squared, &projected_values) ||
        !whisper_model_size_add(projected_values, 5U * width, &layer_values) ||
        !whisper_model_size_multiply(
            config->decoder_layers, layer_values, &all_layer_values
        ) || !whisper_model_size_multiply(2U, width, &norm_values) ||
        !whisper_model_size_add(norm_values, all_layer_values, &total)) {
        return 0U;
    }
    return total;
}

unsigned long long whisper_model_frontend_weight_count(const WhisperModelConfig *config) {
    unsigned long long conv1;
    unsigned long long conv2;
    unsigned long long positions;
    unsigned long long total;
    if (!whisper_model_config_valid(config) || !whisper_model_size_multiply(
            config->width, config->mel_bins * 3U, &conv1
        ) || !whisper_model_size_add(conv1, config->width, &conv1) ||
        !whisper_model_size_multiply(config->width, config->width * 3U, &conv2) ||
        !whisper_model_size_add(conv2, config->width, &conv2) ||
        !whisper_model_size_multiply(
            config->encoder_frames, config->width, &positions
        ) || !whisper_model_size_add(conv1, conv2, &total) ||
        !whisper_model_size_add(total, positions, &total)) return 0U;
    return total;
}

unsigned long long whisper_model_encoder_weight_count(const WhisperModelConfig *config) {
    unsigned long long width_squared;
    unsigned long long ffn_values;
    unsigned long long layer_values;
    unsigned long long total;
    if (!whisper_model_config_valid(config) || !whisper_model_size_multiply(
            config->width, config->width, &width_squared
        ) || !whisper_model_size_multiply(
            config->width, config->ffn_width, &ffn_values
        ) || !whisper_model_size_multiply(4U, width_squared, &layer_values) ||
        !whisper_model_size_add(layer_values, 2U * ffn_values, &layer_values) ||
        !whisper_model_size_add(
            layer_values, 9U * config->width + config->ffn_width, &layer_values
        ) || !whisper_model_size_multiply(
            config->encoder_layers, layer_values, &total
        )) return 0U;
    return total;
}
