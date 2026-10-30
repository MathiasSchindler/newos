#include "nemotron_frontend.h"
#include "math/math.h"
#include "runtime.h"

static int front_error(NmFrontend *state, const char *message) {
    if (state) { state->error = message; state->failed = 1; }
    return 0;
}
static int emit_chunk(NmFrontend *state, uint32_t final) {
    uint32_t flags = (state->chunks ? 0U : NM_AUDIO_FIRST_FLAG) | final;
    size_t used = (size_t)state->valid * NM_AUDIO_MELS;
    memset(state->features + used, 0, ((size_t)state->width * NM_AUDIO_MELS - used) * sizeof(float));
    if (!state->sink(state->context, state->features, state->width, state->valid, flags, state->chunks))
        return front_error(state, "feature consumer failed");
    ++state->chunks; state->valid = 0; state->width = NM_AUDIO_STEADY;
    return 1;
}
static int emit_frame(NmFrontend *state) {
    float power[NM_AUDIO_FFT / 2 + 1];
    uint32_t i, length, bin, mel;
    for (i = 0; i < NM_AUDIO_FFT; ++i) {
        uint32_t reversed = 0, bit, value = i;
        float product = state->ring[(state->head + i) % NM_AUDIO_FFT] * state->window[i];
        for (bit = 0; bit < 9; ++bit) { reversed = (reversed << 1) | (value & 1); value >>= 1; }
        state->real[reversed] = product;
        state->imaginary[reversed] = 0;
    }
    for (length = 2; length <= NM_AUDIO_FFT; length *= 2) {
        uint32_t half = length / 2, stride = NM_AUDIO_FFT / length, base;
        for (base = 0; base < NM_AUDIO_FFT; base += length) {
            for (i = 0; i < half; ++i) {
                uint32_t left = base + i, right = left + half, angle = i * stride;
                double real = state->cosine[angle] * state->real[right] -
                              state->sine[angle] * state->imaginary[right];
                double imaginary = state->cosine[angle] * state->imaginary[right] +
                                   state->sine[angle] * state->real[right];
                state->real[right] = state->real[left] - real;
                state->imaginary[right] = state->imaginary[left] - imaginary;
                state->real[left] += real; state->imaginary[left] += imaginary;
            }
        }
    }
    for (bin = 0; bin <= NM_AUDIO_FFT / 2; ++bin) {
        float real = (float)state->real[bin], imaginary = (float)state->imaginary[bin];
        float magnitude = (float)math_sqrt((double)real * real + (double)imaginary * imaginary);
        power[bin] = magnitude * magnitude;
    }
    for (mel = 0; mel < NM_AUDIO_MELS; ++mel) {
        float sum = 0, feature;
        for (bin = 0; bin <= NM_AUDIO_FFT / 2; ++bin)
            sum += power[bin] * state->mel[mel * (NM_AUDIO_FFT / 2 + 1) + bin];
        sum += 0x1p-24f;
        feature = (float)math_log(sum);
        if (!math_is_finite(feature)) return front_error(state, "nonfinite feature");
        state->features[state->valid * NM_AUDIO_MELS + mel] = feature;
    }
    ++state->frames; ++state->valid;
    state->head = (state->head + NM_AUDIO_HOP) % NM_AUDIO_FFT;
    state->count -= NM_AUDIO_HOP;
    return state->valid != state->width || emit_chunk(state, 0);
}
int nm_frontend_reset(NmFrontend *state) {
    if (!state || !state->window || !state->mel || !state->sink)
        return front_error(state, "uninitialized frontend");
    state->samples = state->frames = state->chunks = 0;
    state->head = state->valid = state->closed = state->failed = 0;
    state->count = NM_AUDIO_FFT / 2; state->width = NM_AUDIO_FIRST;
    state->previous = 0; state->error = 0;
    memset(state->ring, 0, sizeof(state->ring));
    memset(state->features, 0, sizeof(state->features));
    return 1;
}
int nm_frontend_init(NmFrontend *state, const float *window, const float *mel,
                     NmFeatureSink sink, void *context) {
    uint32_t i;
    if (!state) return 0;
    memset(state, 0, sizeof(*state));
    if (!window || !mel || !sink) return front_error(state, "invalid frontend configuration");
    for (i = 0; i < NM_AUDIO_FFT; ++i)
        if (!math_is_finite(window[i])) return front_error(state, "nonfinite window");
    for (i = 0; i < NM_AUDIO_MELS * (NM_AUDIO_FFT / 2 + 1); ++i)
        if (!math_is_finite(mel[i]) || mel[i] < 0) return front_error(state, "invalid mel filter");
    state->window = window; state->mel = mel; state->sink = sink; state->context = context;
    for (i = 0; i < NM_AUDIO_FFT / 2; ++i) {
        double angle = -2 * MATH_PI * i / NM_AUDIO_FFT;
        state->cosine[i] = math_cos(angle); state->sine[i] = math_sin(angle);
    }
    return nm_frontend_reset(state);
}
int nm_frontend_feed(NmFrontend *state, const float *samples, size_t count) {
    size_t i;
    if (!state || !state->sink) return front_error(state, "uninitialized frontend");
    if (state->closed || state->failed) return front_error(state, "frontend closed or failed");
    if ((!samples && count) || count > UINT64_MAX - state->samples)
        return front_error(state, "invalid audio feed size");
    for (i = 0; i < count; ++i)
        if (!math_is_finite(samples[i]) || samples[i] < -1 || samples[i] > 1)
            return front_error(state, "audio must be finite normalized PCM");
    for (i = 0; i < count; ++i) {
        float previous = 0.97f * state->previous;
        state->ring[(state->head + state->count) % NM_AUDIO_FFT] = samples[i] - previous;
        state->previous = samples[i]; ++state->samples; ++state->count;
        if (state->count == NM_AUDIO_FFT && !emit_frame(state)) return 0;
    }
    return 1;
}
int nm_frontend_finish(NmFrontend *state) {
    if (!state || !state->sink) return front_error(state, "uninitialized frontend");
    if (state->closed || state->failed) return front_error(state, "frontend closed or failed");
    state->closed = 1;
    if (state->samples < NM_AUDIO_HOP) return front_error(state, "audio shorter than one feature frame");
    while (state->frames < state->samples / NM_AUDIO_HOP) {
        state->ring[(state->head + state->count) % NM_AUDIO_FFT] = 0;
        ++state->count;
        if (state->count == NM_AUDIO_FFT && !emit_frame(state)) return 0;
    }
    if (state->valid && !emit_chunk(state, NM_AUDIO_FINAL_FLAG)) return 0;
    if (!state->sink(state->context, 0, 0, 0, NM_AUDIO_END_FLAG, state->chunks))
        return front_error(state, "feature end consumer failed");
    return 1;
}
