#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_GEMMA_MODEL_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_GEMMA_MODEL_H

typedef enum GemmaAttentionType {
    GEMMA_ATTENTION_INVALID = 0,
    GEMMA_ATTENTION_SLIDING = 1,
    GEMMA_ATTENTION_FULL = 2
} GemmaAttentionType;

typedef struct GemmaModelConfig {
    const char *name;
    const char *repository;
    const char *revision;
    const char *model_type;
    const char *hidden_activation;
    unsigned int vocabulary_size;
    unsigned int hidden_size;
    unsigned int intermediate_size;
    unsigned int decoder_layers;
    unsigned int attention_heads;
    unsigned int key_value_heads;
    unsigned int head_size;
    unsigned int maximum_positions;
    unsigned int deployment_context;
    unsigned int sliding_window;
    unsigned int sliding_window_pattern;
    unsigned int query_pre_attention_scalar;
    unsigned int pad_token_id;
    unsigned int eos_token_id;
    unsigned int bos_token_id;
    unsigned int source_file_count;
    unsigned int weight_shard_count;
    unsigned int stored_tensor_count;
    unsigned int text_tensor_count;
    unsigned int maximum_safetensors_header_bytes;
    double rms_norm_epsilon;
    double global_rope_theta;
    double local_rope_theta;
    unsigned long long text_parameter_count;
    unsigned long long text_raw_bf16_bytes;
} GemmaModelConfig;

enum {
    GEMMA_TRANSLATEGEMMA_4B_VOCABULARY_SIZE = 262208,
    GEMMA_TRANSLATEGEMMA_4B_HIDDEN_SIZE = 2560,
    GEMMA_TRANSLATEGEMMA_4B_INTERMEDIATE_SIZE = 10240,
    GEMMA_TRANSLATEGEMMA_4B_DECODER_LAYERS = 34,
    GEMMA_TRANSLATEGEMMA_4B_ATTENTION_HEADS = 8,
    GEMMA_TRANSLATEGEMMA_4B_KEY_VALUE_HEADS = 4,
    GEMMA_TRANSLATEGEMMA_4B_HEAD_SIZE = 256,
    GEMMA_TRANSLATEGEMMA_4B_MAXIMUM_POSITIONS = 131072,
    GEMMA_TRANSLATEGEMMA_4B_DEPLOYMENT_CONTEXT = 2048,
    GEMMA_TRANSLATEGEMMA_4B_SLIDING_WINDOW = 1024,
    GEMMA_TRANSLATEGEMMA_4B_SLIDING_WINDOW_PATTERN = 6,
    GEMMA_TRANSLATEGEMMA_4B_QUERY_PRE_ATTENTION_SCALAR = 256,
    GEMMA_TRANSLATEGEMMA_4B_PAD_TOKEN_ID = 0,
    GEMMA_TRANSLATEGEMMA_4B_EOS_TOKEN_ID = 1,
    GEMMA_TRANSLATEGEMMA_4B_END_OF_TURN_TOKEN_ID = 106,
    GEMMA_TRANSLATEGEMMA_4B_BOS_TOKEN_ID = 2,
    GEMMA_TRANSLATEGEMMA_4B_FULL_ATTENTION_LAYERS = 5,
    GEMMA_TRANSLATEGEMMA_4B_SLIDING_ATTENTION_LAYERS = 29,
    GEMMA_TRANSLATEGEMMA_4B_SOURCE_FILES = 15,
    GEMMA_TRANSLATEGEMMA_4B_WEIGHT_SHARDS = 2,
    GEMMA_TRANSLATEGEMMA_4B_STORED_TENSORS = 883,
    GEMMA_TRANSLATEGEMMA_4B_TEXT_TENSORS = 444,
    GEMMA_TRANSLATEGEMMA_4B_MAXIMUM_SAFETENSORS_HEADER_BYTES = 104857600
};

#define GEMMA_TRANSLATEGEMMA_4B_TEXT_PARAMETER_COUNT 3880263168ULL
#define GEMMA_TRANSLATEGEMMA_4B_TEXT_RAW_BF16_BYTES 7760526336ULL

const GemmaModelConfig *gemma_model_translategemma_4b(void);
int gemma_model_is_stop_token(unsigned int token_id);
int gemma_model_config_valid(const GemmaModelConfig *config);
GemmaAttentionType gemma_model_attention_type(
    const GemmaModelConfig *config,
    unsigned int layer
);

#endif