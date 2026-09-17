#include "../gemma_model.h"

int main(void) {
    const GemmaModelConfig *config = gemma_model_translategemma_4b();
    GemmaModelConfig changed;
    unsigned int layer;
    unsigned int full_layers = 0;
    unsigned int sliding_layers = 0;

    if (!gemma_model_config_valid(config)) return 1;
    if (!gemma_model_is_stop_token(1U) || !gemma_model_is_stop_token(106U) ||
        gemma_model_is_stop_token(0U) || gemma_model_is_stop_token(2U) ||
        gemma_model_is_stop_token(262208U)) return 6;
    for (layer = 0; layer < config->decoder_layers; ++layer) {
        GemmaAttentionType type = gemma_model_attention_type(config, layer);
        if (type == GEMMA_ATTENTION_FULL) ++full_layers;
        else if (type == GEMMA_ATTENTION_SLIDING) ++sliding_layers;
        else return 2;
    }
    if (full_layers != GEMMA_TRANSLATEGEMMA_4B_FULL_ATTENTION_LAYERS ||
        sliding_layers != GEMMA_TRANSLATEGEMMA_4B_SLIDING_ATTENTION_LAYERS ||
        gemma_model_attention_type(config, config->decoder_layers) !=
            GEMMA_ATTENTION_INVALID) {
        return 3;
    }
    changed = *config;
    changed.hidden_size += 1U;
    if (gemma_model_config_valid(&changed)) return 4;
    changed = *config;
    changed.repository = "other/translategemma-4b-it";
    if (gemma_model_config_valid(&changed)) return 5;
    return 0;
}