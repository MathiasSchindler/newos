#define WHISPER_RUNTIME_ONLY
#define mainCRTStartup whisper_probe_entry
#include "../main.c"
#undef mainCRTStartup

static u32 retrieved_graphs;

static u64 test_graph_retrieve(
    QnnContextHandle context, const char *name, QnnGraphHandle *graph
) {
    (void)context;
    (void)name;
    ++retrieved_graphs;
    *graph = (void *)(usize)retrieved_graphs;
    return 0U;
}

static int test_medium_restore(u32 mask, u32 expected_graphs) {
    QnnInterfaceV2 api = {0};
    WhisperDecoderQnnIds ids = {0};
    WhisperDecoderQnn *decoder = whisper_decoder_qnn_create(whisper_model_medium(), mask);
    int restored;
    if (decoder == 0) return 0;
    api.graph_retrieve = test_graph_retrieve;
    ids.model_id = WHISPER_MODEL_ID_MEDIUM;
    ids.output_count = 48U;
    ids.cross_layer_count = (mask & DECODER_OFFLOAD_CROSS) ? 24U : 0U;
    ids.mlp_layer_count = (mask & DECODER_OFFLOAD_MLP) ? 24U : 0U;
    ids.fused_layer_count = (mask & DECODER_OFFLOAD_FUSED) ? 24U : 0U;
    ids.self_layer_count = (mask & DECODER_OFFLOAD_SELF) ? 24U : 0U;
    retrieved_graphs = 0U;
    restored = whisper_decoder_qnn_restore(decoder, &api, 0, &ids);
    if (!restored || retrieved_graphs != expected_graphs) {
        whisper_decoder_qnn_shutdown(decoder);
        return 0;
    }
    ids.cross_layer_count = ids.cross_layer_count == 0U ? 24U : 0U;
    restored = whisper_decoder_qnn_restore(decoder, &api, 0, &ids);
    whisper_decoder_qnn_shutdown(decoder);
    return !restored;
}

void mainCRTStartup(void) {
    static ModelContextCacheMetadata original;
    static ModelContextCacheMetadata decoded;
    static u8 encoded[MODEL_CONTEXT_CACHE_METADATA_SIZE];
    u8 *original_bytes = (u8 *)&original;
    const u8 *decoded_bytes = (const u8 *)&decoded;
    u32 index;
    _Static_assert(MODEL_CONTEXT_CACHE_METADATA_SIZE == 2152U,
        "24-layer cache layout changed");
    _Static_assert(sizeof(ModelContextCacheMetadata) == MODEL_CONTEXT_CACHE_METADATA_SIZE,
        "Metadata test requires a padding-free layout");
    for (index = 0U; index < sizeof(original); ++index) {
        original_bytes[index] = (u8)(index * 37U + index / 251U);
    }
    encode_model_context_metadata(encoded, &original);
    decode_model_context_metadata(encoded, &decoded);
    for (index = 0U; index < sizeof(original); ++index) {
        if (original_bytes[index] != decoded_bytes[index]) ExitProcess(1U);
    }
    if (!test_medium_restore(DECODER_OFFLOAD_CROSS | DECODER_OFFLOAD_MLP, 49U)) {
        ExitProcess(2U);
    }
    if (!test_medium_restore(
            DECODER_OFFLOAD_FUSED | DECODER_OFFLOAD_SELF | DECODER_OFFLOAD_LOGITS,
            74U
        )) ExitProcess(3U);
    ExitProcess(0U);
}