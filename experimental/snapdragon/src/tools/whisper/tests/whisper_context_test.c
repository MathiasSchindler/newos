#define WHISPER_RUNTIME_ONLY
#define mainCRTStartup whisper_probe_entry
#include "../main.c"
#undef mainCRTStartup

static u32 retrieved_graphs;
static u32 create_calls;
static u32 fail_create_call;
static const char *const *expected_decoder_names;
static const char *const *expected_encoder_names;
static const void *expected_binary;

static void test_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    qnn_log_callback(format, QNN_LOG_LEVEL_ERROR, 0U, args);
    va_end(args);
}

static int test_diagnostics(u64 status, int quiet, int unrelated) {
    static const char path[] = "tests/tmp/whisper-context-log.txt";
    char output[1024] = {0};
    u32 size;
    original_stderr_handle = CreateFileA(path, 0x40000000U, 1U, 0, 2U, 0x80U, 0);
    if (original_stderr_handle == (void *)(usize)-1) return 0;
    quiet_output = quiet;
    qnn_log_begin_restore();
    test_log("%s Context %u failed on pd %u", " <E>", 2U, 0U);
    test_log("contextFromBin (submit) Failed code: %llu", 5005ULL);
    if (unrelated) test_log("Inference failed: %s %08x", "tensor", 42U);
    qnn_log_end_restore(status);
    CloseHandle(original_stderr_handle);
    void *input = CreateFileA(path, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    if (input == (void *)(usize)-1) return 0;
    int read_ok = ReadFile(input, output, sizeof(output) - 1U, &size, 0);
    CloseHandle(input);
    quiet_output = 0;
    if (!read_ok) return 0;
    if (status != 0U) return qnn_log_contains(output, "<E> Context 2 failed on pd 0") &&
        !qnn_log_contains(output, "<E> <E>") &&
        qnn_log_contains(output, "Failed code: 5005");
    if (unrelated) return qnn_log_contains(output, "<E> Inference failed: tensor 0000002a") &&
        !qnn_log_contains(output, "failed on pd");
    return quiet ? size == 0U : qnn_log_contains(output, "<W> QNN recovered") &&
        !qnn_log_contains(output, "<E>");
}

static u64 test_context_create_from_binary(
    QnnBackendHandle backend, QnnDeviceHandle device,
    const QnnContextConfig **configs, const void *binary, u64 size,
    QnnContextHandle *context, QnnProfileHandle profile
) {
    const char *const *expected_names = create_calls == 0U ?
        expected_decoder_names : expected_encoder_names;
    (void)backend;
    (void)device;
    (void)profile;
    ++create_calls;
    if (binary != expected_binary || size != 17U) return 90U;
    if (expected_names == 0) {
        if (configs != 0) return 91U;
    } else if (configs == 0 || configs[0] == 0 || configs[1] != 0 ||
        configs[0]->option != QNN_CONTEXT_CONFIG_ENABLE_GRAPHS ||
        configs[0]->value.enable_graphs != expected_names) return 92U;
    if (create_calls == fail_create_call) return 93U;
    *context = (void *)(usize)create_calls;
    return 0U;
}

static int test_context_partition(int split, u32 failure) {
    QnnInterfaceV2 api = {0};
    QnnContextHandle context = 0;
    QnnContextHandle encoder_context = 0;
    const char *decoder_names[] = {"decoder", 0};
    const char *encoder_names[] = {"encoder", 0};
    u8 binary[17] = {0};
    u64 status;
    api.context_create_from_binary = test_context_create_from_binary;
    expected_binary = binary;
    expected_decoder_names = split ? decoder_names : 0;
    expected_encoder_names = split ? encoder_names : 0;
    create_calls = 0U;
    fail_create_call = failure;
    status = create_model_contexts_from_binary(
        &api, 0, 0, 0, binary, sizeof(binary),
        expected_decoder_names, expected_encoder_names, &context, &encoder_context
    );
    if (failure == 1U) return status == 93U && create_calls == 1U &&
        context == 0 && encoder_context == 0;
    if (failure == 2U) return status == 93U && create_calls == 2U &&
        context == (void *)(usize)1U && encoder_context == 0;
    return status == 0U && create_calls == (split ? 2U : 1U) &&
        context == (void *)(usize)1U &&
        encoder_context == (split ? (void *)(usize)2U : 0);
}

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
    const char *names[WHISPER_DECODER_QNN_MAX_GRAPHS + 1U];
    int restored;
    if (decoder == 0) return 0;
    if (whisper_decoder_qnn_graph_names(decoder, names, expected_graphs) != 0U ||
        whisper_decoder_qnn_graph_names(decoder, names, expected_graphs + 1U) != expected_graphs ||
        names[expected_graphs] != 0) {
        whisper_decoder_qnn_shutdown(decoder);
        return 0;
    }
    for (u32 index = 0U; index < expected_graphs; ++index) {
        if (names[index] == 0 || names[index][0] == 0) {
            whisper_decoder_qnn_shutdown(decoder);
            return 0;
        }
    }
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
    if (console_control_handler(2U) != 0 || transcription_cancelled(0)) ExitProcess(7U);
    if (!console_control_handler(0U) || !transcription_cancelled(0)) ExitProcess(8U);
    __atomic_store_n(&stop_requested, 0U, __ATOMIC_RELAXED);
    if (!console_control_handler(1U) || !transcription_cancelled(0)) ExitProcess(9U);
    __atomic_store_n(&stop_requested, 0U, __ATOMIC_RELAXED);
    if (!test_diagnostics(0U, 1, 0) || !test_diagnostics(0U, 0, 0) ||
        !test_diagnostics(1002U, 1, 0) || !test_diagnostics(0U, 1, 1)) ExitProcess(10U);
    _Static_assert(sizeof(QnnContextConfig) == 16U, "QNN context config ABI size");
    _Static_assert(__builtin_offsetof(QnnContextConfig, value) == 8U,
        "QNN context config ABI value offset");
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
    if (!test_context_partition(0, 0U) || !test_context_partition(1, 0U) ||
        !test_context_partition(1, 1U) || !test_context_partition(1, 2U)) ExitProcess(4U);
    WhisperEncoderQnn *encoder = whisper_encoder_qnn_create(whisper_model_medium());
    const char *encoder_names[4];
    if (encoder == 0) ExitProcess(5U);
    if (whisper_encoder_qnn_graph_names(encoder, encoder_names, 3U) != 0U ||
        whisper_encoder_qnn_graph_names(encoder, encoder_names, 4U) != 3U ||
        encoder_names[3] != 0) ExitProcess(6U);
    whisper_encoder_qnn_shutdown(encoder);
    ExitProcess(0U);
}