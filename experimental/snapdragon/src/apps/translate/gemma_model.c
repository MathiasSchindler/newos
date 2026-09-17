#include "gemma_model.h"

static const GemmaModelConfig translategemma_4b_config = {
    "translategemma-4b-it",
    "google/translategemma-4b-it",
    "10042cb0e6e7fdce748996a71dc3dc432a4e0c89",
    "gemma3",
    "gelu_pytorch_tanh",
    GEMMA_TRANSLATEGEMMA_4B_VOCABULARY_SIZE,
    GEMMA_TRANSLATEGEMMA_4B_HIDDEN_SIZE,
    GEMMA_TRANSLATEGEMMA_4B_INTERMEDIATE_SIZE,
    GEMMA_TRANSLATEGEMMA_4B_DECODER_LAYERS,
    GEMMA_TRANSLATEGEMMA_4B_ATTENTION_HEADS,
    GEMMA_TRANSLATEGEMMA_4B_KEY_VALUE_HEADS,
    GEMMA_TRANSLATEGEMMA_4B_HEAD_SIZE,
    GEMMA_TRANSLATEGEMMA_4B_MAXIMUM_POSITIONS,
    GEMMA_TRANSLATEGEMMA_4B_DEPLOYMENT_CONTEXT,
    GEMMA_TRANSLATEGEMMA_4B_SLIDING_WINDOW,
    GEMMA_TRANSLATEGEMMA_4B_SLIDING_WINDOW_PATTERN,
    GEMMA_TRANSLATEGEMMA_4B_QUERY_PRE_ATTENTION_SCALAR,
    GEMMA_TRANSLATEGEMMA_4B_PAD_TOKEN_ID,
    GEMMA_TRANSLATEGEMMA_4B_EOS_TOKEN_ID,
    GEMMA_TRANSLATEGEMMA_4B_BOS_TOKEN_ID,
    GEMMA_TRANSLATEGEMMA_4B_SOURCE_FILES,
    GEMMA_TRANSLATEGEMMA_4B_WEIGHT_SHARDS,
    GEMMA_TRANSLATEGEMMA_4B_STORED_TENSORS,
    GEMMA_TRANSLATEGEMMA_4B_TEXT_TENSORS,
    GEMMA_TRANSLATEGEMMA_4B_MAXIMUM_SAFETENSORS_HEADER_BYTES,
    0.000001,
    1000000.0,
    10000.0,
    GEMMA_TRANSLATEGEMMA_4B_TEXT_PARAMETER_COUNT,
    GEMMA_TRANSLATEGEMMA_4B_TEXT_RAW_BF16_BYTES
};

_Static_assert(
    GEMMA_TRANSLATEGEMMA_4B_ATTENTION_HEADS %
        GEMMA_TRANSLATEGEMMA_4B_KEY_VALUE_HEADS == 0,
    "TranslateGemma grouped-query geometry changed"
);
_Static_assert(
    GEMMA_TRANSLATEGEMMA_4B_INTERMEDIATE_SIZE ==
        GEMMA_TRANSLATEGEMMA_4B_HIDDEN_SIZE * 4,
    "TranslateGemma MLP geometry changed"
);
_Static_assert(
    GEMMA_TRANSLATEGEMMA_4B_FULL_ATTENTION_LAYERS +
        GEMMA_TRANSLATEGEMMA_4B_SLIDING_ATTENTION_LAYERS ==
            GEMMA_TRANSLATEGEMMA_4B_DECODER_LAYERS,
    "TranslateGemma attention schedule changed"
);
_Static_assert(
    GEMMA_TRANSLATEGEMMA_4B_TEXT_PARAMETER_COUNT * 2ULL ==
        GEMMA_TRANSLATEGEMMA_4B_TEXT_RAW_BF16_BYTES,
    "TranslateGemma BF16 byte count changed"
);

const GemmaModelConfig *gemma_model_translategemma_4b(void) {
    return &translategemma_4b_config;
}

int gemma_model_is_stop_token(unsigned int token_id) {
    return token_id == GEMMA_TRANSLATEGEMMA_4B_EOS_TOKEN_ID ||
        token_id == GEMMA_TRANSLATEGEMMA_4B_END_OF_TURN_TOKEN_ID;
}

static int string_equal(const char *left, const char *right) {
    if (left == 0 || right == 0) return 0;
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

int gemma_model_config_valid(const GemmaModelConfig *config) {
    if (config == 0 || config->name == 0 || config->repository == 0 ||
        config->revision == 0 || config->model_type == 0 ||
        config->hidden_activation == 0) {
        return 0;
    }
    return string_equal(config->name, "translategemma-4b-it") &&
        string_equal(config->repository, "google/translategemma-4b-it") &&
        string_equal(config->revision, "10042cb0e6e7fdce748996a71dc3dc432a4e0c89") &&
        string_equal(config->model_type, "gemma3") &&
        string_equal(config->hidden_activation, "gelu_pytorch_tanh") &&
        config->vocabulary_size == GEMMA_TRANSLATEGEMMA_4B_VOCABULARY_SIZE &&
        config->hidden_size == GEMMA_TRANSLATEGEMMA_4B_HIDDEN_SIZE &&
        config->intermediate_size == GEMMA_TRANSLATEGEMMA_4B_INTERMEDIATE_SIZE &&
        config->decoder_layers == GEMMA_TRANSLATEGEMMA_4B_DECODER_LAYERS &&
        config->attention_heads == GEMMA_TRANSLATEGEMMA_4B_ATTENTION_HEADS &&
        config->key_value_heads == GEMMA_TRANSLATEGEMMA_4B_KEY_VALUE_HEADS &&
        config->head_size == GEMMA_TRANSLATEGEMMA_4B_HEAD_SIZE &&
        config->maximum_positions == GEMMA_TRANSLATEGEMMA_4B_MAXIMUM_POSITIONS &&
        config->deployment_context == GEMMA_TRANSLATEGEMMA_4B_DEPLOYMENT_CONTEXT &&
        config->sliding_window == GEMMA_TRANSLATEGEMMA_4B_SLIDING_WINDOW &&
        config->sliding_window_pattern ==
            GEMMA_TRANSLATEGEMMA_4B_SLIDING_WINDOW_PATTERN &&
        config->query_pre_attention_scalar ==
            GEMMA_TRANSLATEGEMMA_4B_QUERY_PRE_ATTENTION_SCALAR &&
        config->pad_token_id == GEMMA_TRANSLATEGEMMA_4B_PAD_TOKEN_ID &&
        config->eos_token_id == GEMMA_TRANSLATEGEMMA_4B_EOS_TOKEN_ID &&
        config->bos_token_id == GEMMA_TRANSLATEGEMMA_4B_BOS_TOKEN_ID &&
        config->source_file_count == GEMMA_TRANSLATEGEMMA_4B_SOURCE_FILES &&
        config->weight_shard_count == GEMMA_TRANSLATEGEMMA_4B_WEIGHT_SHARDS &&
        config->stored_tensor_count == GEMMA_TRANSLATEGEMMA_4B_STORED_TENSORS &&
        config->text_tensor_count == GEMMA_TRANSLATEGEMMA_4B_TEXT_TENSORS &&
        config->maximum_safetensors_header_bytes ==
            GEMMA_TRANSLATEGEMMA_4B_MAXIMUM_SAFETENSORS_HEADER_BYTES &&
        config->rms_norm_epsilon == 0.000001 &&
        config->global_rope_theta == 1000000.0 &&
        config->local_rope_theta == 10000.0 &&
        config->text_parameter_count ==
            GEMMA_TRANSLATEGEMMA_4B_TEXT_PARAMETER_COUNT &&
        config->text_raw_bf16_bytes ==
            GEMMA_TRANSLATEGEMMA_4B_TEXT_RAW_BF16_BYTES;
}

GemmaAttentionType gemma_model_attention_type(
    const GemmaModelConfig *config,
    unsigned int layer
) {
    if (!gemma_model_config_valid(config) || layer >= config->decoder_layers) {
        return GEMMA_ATTENTION_INVALID;
    }
    if ((layer + 1U) % config->sliding_window_pattern == 0U) {
        return GEMMA_ATTENTION_FULL;
    }
    return GEMMA_ATTENTION_SLIDING;
}