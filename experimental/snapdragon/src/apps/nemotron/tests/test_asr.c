#include "../nemotron_asr_runtime.h"
#include "../nemotron_rnnt.h"
#include "math/math.h"

static float logits[13088], encoder[640];
static u32 tokens[32], predictions[32], choices[32], position, predicted, choice_count;
static int nonfinite, wrong_shape;
static u32 passed, failed;
static void check(int condition, const char *label) {
    if (condition) ++passed;
    else { ++failed; nm_text("FAIL "); nm_text(label); nm_text("\n"); }
}
static int reset(void *context) {
    (void)context; position = predicted = 0; return 1;
}
static int predict(void *context, uint32_t token) {
    (void)context;
    if (predicted >= 32) return 0;
    predictions[predicted++] = token; return 1;
}
static int join(void *context, const float *features, const float **values, uint32_t *count) {
    u32 token = position < choice_count ? choices[position] : 0;
    (void)context; (void)features; ++position;
    memset(logits, 0, sizeof(logits));
    if (token == 0xffffffffU) { logits[0] = logits[1] = 1; }
    else if (token < 13088) logits[token] = 1;
    if (nonfinite) logits[7] = (float)math_nan();
    *values = logits; *count = wrong_shape ? 13087 : 13088; return 1;
}
void mainCRTStartup(void) {
    NmRnnt state; NmRnntOps ops = {reset, predict, join};
    QnnTensor source = {0}, destination = {0};
    u32 dimensions[2] = {1, 4}; float values[12] = {1, 2, 3, 4};
    choices[0] = 0; choices[1] = choices[2] = 13087; choice_count = 3;
    check(nm_rnnt_init(&state, ops, 0, tokens, 32), "RNNT initial blank prediction");
    check(predicted == 1 && predictions[0] == 13087, "prediction starts from model blank");
    check(nm_rnnt_frame(&state, encoder) && nm_rnnt_frame(&state, encoder), "nonblank and blank frames");
    check(state.count == 1 && tokens[0] == 0 && state.frames == 2, "token/frame advancement");
    check(predicted == 2 && predictions[1] == 0 && position == 3, "blank reuse, nonblank-only prediction commit");
    check(nm_rnnt_reset(&state) && !state.count && !state.frames && predicted == 1, "fresh reset state");
    check(nm_rnnt_frame(&state, encoder) && state.count == 1 && tokens[0] == 0, "reset replay matches fresh");
    choices[0] = 0xffffffffU; choices[1] = 13087; choice_count = 2;
    check(nm_rnnt_reset(&state) && nm_rnnt_frame(&state, encoder) && tokens[0] == 0,
          "argmax equal-score tie chooses lowest id");
    nonfinite = 1;
    check(nm_rnnt_reset(&state) && !nm_rnnt_frame(&state, encoder) && state.failed &&
          predicted == 1 && !state.count && state.error, "nonfinite logits are an error before commit");
    nonfinite = 0; wrong_shape = 1;
    check(nm_rnnt_reset(&state) && !nm_rnnt_frame(&state, encoder) && state.error, "invalid logits shape");
    wrong_shape = 0; choice_count = 0;
    check(nm_rnnt_reset(&state) && !nm_rnnt_frame(&state, encoder) &&
          position == 10 && state.count == 9 && !state.frames, "symbol limit is fatal, not truncation");
    check(!nm_rnnt_frame(&state, encoder), "failed controller cannot continue");
    choices[0] = 0; choices[1] = 1; choices[2] = 13087; choice_count = 3;
    check(nm_rnnt_init(&state, ops, 0, tokens, 1) && !nm_rnnt_frame(&state, encoder) &&
          state.count == 1 && predicted == 2, "token buffer bound");
    source.data.v1.data_type = destination.data.v1.data_type = QNN_DATATYPE_FLOAT_32;
    source.data.v1.rank = destination.data.v1.rank = 2;
    source.data.v1.dimensions = destination.data.v1.dimensions = dimensions;
    source.data.v1.memory.client_buffer.data = values;
    destination.data.v1.memory.client_buffer.data = values + 4;
    source.data.v1.memory.client_buffer.data_size = destination.data.v1.memory.client_buffer.data_size = 16;
    check(nm_asr_binding_copy(&destination, &source) && !memcmp(values, values + 4, 16), "cache copy with distinct buffers");
    destination.data.v1.memory.client_buffer.data = values;
    check(!nm_asr_binding_copy(&destination, &source), "exact input/output alias rejected");
    destination.data.v1.memory.client_buffer.data = values + 1;
    check(!nm_asr_binding_copy(&destination, &source), "partial buffer alias rejected");
    destination.data.v1.memory.client_buffer.data = values + 4;
    destination.data.v1.data_type = QNN_DATATYPE_INT_32;
    check(!nm_asr_binding_copy(&destination, &source), "cache dtype mismatch rejected");
    check(!nm_rpc_execute(NM_RPC_HOST_GRAPHS, 0, 0, 0, 0, 0, 0, 1), "RPC graph bound checked before dereference");
    check(!nm_rpc_io_view(NM_RPC_HOST_GRAPHS, "hidden", 0, 4), "RPC shared view graph bound");
    check(!nm_rpc_io_view(0, "hidden", 0, 4), "RPC shared view requires staged graph");
    nm_text("ASR controller checks passed "); nm_number(passed); nm_text(" failed "); nm_number(failed); nm_text("\n");
    ExitProcess(failed ? 1 : 0);
}
