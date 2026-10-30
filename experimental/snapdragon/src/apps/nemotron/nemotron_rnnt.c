#include "nemotron_rnnt.h"
#include "math/math.h"
#include "runtime.h"

static int rnnt_error(NmRnnt *state, const char *error) {
    if (state) { state->failed = 1; state->error = error; }
    return 0;
}
int nm_rnnt_reset(NmRnnt *state) {
    if (!state || !state->ops.reset || !state->ops.predict || !state->ops.join ||
        !state->tokens || !state->capacity) return rnnt_error(state, "uninitialized RNN-T");
    state->count = 0; state->frames = state->prediction_calls = state->joiner_calls = 0;
    state->failed = 0; state->error = 0;
    if (!state->ops.reset(state->context)) return rnnt_error(state, "model reset failed");
    if (!state->ops.predict(state->context, state->blank)) return rnnt_error(state, "initial blank prediction failed");
    state->prediction_calls = 1; return 1;
}
int nm_rnnt_init(NmRnnt *state, NmRnntOps ops, void *context, uint32_t *tokens, size_t capacity) {
    if (!state) return 0;
    memset(state, 0, sizeof(*state));
    state->ops = ops; state->context = context; state->tokens = tokens; state->capacity = capacity;
    state->blank = 13087; state->vocabulary = 13088; state->max_symbols = 10;
    return nm_rnnt_reset(state);
}
int nm_rnnt_frame(NmRnnt *state, const float *encoder) {
    uint32_t symbols = 0;
    if (!state || state->failed || !encoder) return rnnt_error(state, "invalid or failed RNN-T frame");
    for (;;) {
        const float *logits = 0; uint32_t count = 0, index, token = 0;
        if (!state->ops.join(state->context, encoder, &logits, &count))
            return rnnt_error(state, "joiner execution failed");
        ++state->joiner_calls;
        if (!logits || count != state->vocabulary) return rnnt_error(state, "joiner logits shape mismatch");
        for (index = 0; index < count; ++index) {
            if (!math_is_finite(logits[index])) return rnnt_error(state, "nonfinite joiner logits");
            if (logits[index] > logits[token]) token = index;
        }
        if (token == state->blank) { ++state->frames; return 1; }
        if (++symbols >= state->max_symbols) return rnnt_error(state, "RNN-T symbol limit reached");
        if (state->count == state->capacity) return rnnt_error(state, "RNN-T token limit reached");
        if (!state->ops.predict(state->context, token)) return rnnt_error(state, "nonblank prediction failed");
        ++state->prediction_calls;
        state->tokens[state->count++] = token;
    }
}
