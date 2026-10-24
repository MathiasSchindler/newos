#include "nemotron_audio.h"
#include "runtime.h"
#include "platform.h"
#include "math.h"

static int resample_error(NmResampler *state, const char *message) { state->error = message; return 0; }
int nm_resampler_init(NmResampler *state, uint32_t rate, uint32_t channels, uint32_t format) {
    uint32_t a = rate, b = 16000, phase, tap;
    memset(state, 0, sizeof(*state));
    if ((rate != 8000 && rate != 16000 && rate != 24000 && rate != 32000 &&
         rate != 44100 && rate != 48000 && rate != 88200 && rate != 96000) ||
        !channels || channels > 8 || (format != 1 && format != 3))
        return resample_error(state, "unsupported audio rate/channel/PCM16-or-float32 format");
    while (b) { uint32_t next = a % b; a = b; b = next; }
    state->rate = rate; state->channels = channels; state->format = format; state->gcd = a;
    state->phases = 16000 / a;
    if (state->phases > NM_RESAMPLE_PHASES) return resample_error(state, "resampler phase bound exceeded");
    for (phase = 0; phase < state->phases; ++phase) {
        double sum = 0, cutoff = 0.90 * (rate > 16000 ? 16000.0 / rate : 1.0);
        for (tap = 0; tap < NM_RESAMPLE_TAPS; ++tap) {
            double x = (double)tap - 31.5 - (double)phase / state->phases;
            double sinc = x == 0 ? cutoff : math_sin(MATH_PI * cutoff * x) / (MATH_PI * x);
            double window = 0.42 - 0.5 * math_cos(2 * MATH_PI * tap / 63) + 0.08 * math_cos(4 * MATH_PI * tap / 63);
            state->weights[phase][tap] = (float)(sinc * window);
            sum += state->weights[phase][tap];
        }
        if (!math_is_finite(sum) || sum <= 0) return resample_error(state, "invalid resampler filter");
        for (tap = 0; tap < NM_RESAMPLE_TAPS; ++tap) state->weights[phase][tap] = (float)(state->weights[phase][tap] / sum);
    }
    return 1;
}
int nm_resampler_feed(NmResampler *state, const void *samples, uint32_t frames, int silent,
                      NmAudioSink sink, void *context) {
    const unsigned char *input = samples;
    float output[256]; uint32_t used = 0, frame, channel;
    if (!state->rate || state->error || !sink || (!samples && frames && !silent))
        return resample_error(state, "invalid or failed resampler input");
    for (frame = 0; frame < frames; ++frame) {
        double sum = 0;
        for (channel = 0; !silent && channel < state->channels; ++channel) {
            size_t index = (size_t)frame * state->channels + channel;
            float value;
            if (state->format == 1) {
                uint32_t bits = input[index * 2] | (uint32_t)input[index * 2 + 1] << 8;
                int signed_value = bits & 32768 ? (int)bits - 65536 : (int)bits;
                value = (float)signed_value * 0x1p-15f;
            } else {
                memcpy(&value, input + index * 4, 4);
                if (!math_is_finite(value)) return resample_error(state, "nonfinite microphone sample");
                if (value > 1) { value = 1; ++state->clipped; }
                if (value < -1) { value = -1; ++state->clipped; }
            }
            sum += value;
        }
        state->history[state->head] = (float)(sum / state->channels);
        ++state->input_frames; state->phase += 16000;
        while (state->phase >= state->rate) {
            float value; uint32_t tap;
            state->phase -= state->rate;
            if (state->rate == 16000) value = state->history[state->head];
            else {
                double filtered = 0; uint32_t phase = state->phase / state->gcd;
                if (phase >= state->phases) return resample_error(state, "resampler phase out of bounds");
                for (tap = 0; tap < NM_RESAMPLE_TAPS; ++tap)
                    filtered += state->history[(state->head + NM_RESAMPLE_TAPS - tap) % NM_RESAMPLE_TAPS] * state->weights[phase][tap];
                value = (float)filtered;
                if (value > 1) { value = 1; ++state->clipped; }
                if (value < -1) { value = -1; ++state->clipped; }
            }
            output[used++] = value; ++state->output_samples;
            if (used == 256) {
                if (!sink(context, output, used)) return resample_error(state, "audio consumer failed");
                used = 0;
            }
        }
        state->head = (state->head + 1) % NM_RESAMPLE_TAPS;
    }
    return !used || sink(context, output, used) || resample_error(state, "audio consumer failed");
}
void nm_audio_queue_init(NmAudioQueue *queue, float *samples, uint32_t capacity) {
    memset(queue, 0, sizeof(*queue)); queue->samples = samples; queue->capacity = capacity;
}
uint32_t nm_audio_queue_count(const NmAudioQueue *queue) {
    return __atomic_load_n(&queue->write, __ATOMIC_ACQUIRE) - __atomic_load_n(&queue->read, __ATOMIC_ACQUIRE);
}
static void queue_failure(NmAudioQueue *queue, uint32_t failure) {
    if (failure) {
        uint32_t expected = 0;
        __atomic_compare_exchange_n(&queue->failure, &expected, failure, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
}
void nm_audio_queue_end(NmAudioQueue *queue, uint32_t failure) {
    queue_failure(queue, failure);
    __atomic_store_n(&queue->ended, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&queue->wake, 1, __ATOMIC_RELEASE); platform_wake_word_all(&queue->wake);
}
void nm_audio_queue_stop(NmAudioQueue *queue) {
    __atomic_store_n(&queue->stop, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&queue->wake, 1, __ATOMIC_RELEASE); platform_wake_word_all(&queue->wake);
}
int nm_audio_queue_push(void *context, const float *samples, uint32_t count) {
    NmAudioQueue *queue = context; uint64_t write, read; uint32_t index, queued;
    if (__atomic_load_n(&queue->stop, __ATOMIC_ACQUIRE)) return 0;
    write = __atomic_load_n(&queue->write, __ATOMIC_RELAXED);
    read = __atomic_load_n(&queue->read, __ATOMIC_ACQUIRE);
    if (!queue->samples || !queue->capacity || write - read > queue->capacity ||
        count > queue->capacity - (write - read)) {
        queue_failure(queue, 1); nm_audio_queue_stop(queue); return 0;
    }
    for (index = 0; index < count; ++index) queue->samples[(write + index) % queue->capacity] = samples[index];
    queue->accepted += count; queued = write - read + count;
    if (queued > __atomic_load_n(&queue->peak, __ATOMIC_RELAXED))
        __atomic_store_n(&queue->peak, queued, __ATOMIC_RELAXED);
    __atomic_store_n(&queue->write, write + count, __ATOMIC_RELEASE);
    __atomic_add_fetch(&queue->wake, 1, __ATOMIC_RELEASE); platform_wake_word_all(&queue->wake);
    return 1;
}
uint32_t nm_audio_queue_pop(NmAudioQueue *queue, float *samples, uint32_t capacity) {
    uint64_t read = __atomic_load_n(&queue->read, __ATOMIC_RELAXED);
    uint32_t count = __atomic_load_n(&queue->write, __ATOMIC_ACQUIRE) - read, index;
    if (count > capacity) count = capacity;
    for (index = 0; index < count; ++index) samples[index] = queue->samples[(read + index) % queue->capacity];
    queue->consumed += count;
    __atomic_store_n(&queue->read, read + count, __ATOMIC_RELEASE);
    return count;
}
static int endpoint_error(NmEndpoint *state, const char *message) { state->error = message; return 0; }
static int endpoint_emit(NmEndpoint *state, const float *samples, uint32_t count) {
    if (!count) return 1;
    if (!state->ops.feed(state->context, samples, count))
        return endpoint_error(state, "endpoint audio consumer failed");
    state->model_samples += count; return 1;
}
int nm_endpoint_init(NmEndpoint *state, uint32_t silence_ms, float threshold, NmEndpointOps ops, void *context) {
    memset(state, 0, sizeof(*state));
    if (silence_ms < 100 || silence_ms > 10000 || !math_is_finite(threshold) || threshold <= 0 || threshold >= 1 ||
        !ops.begin || !ops.feed || !ops.end) return endpoint_error(state, "invalid endpoint settings");
    state->ops = ops; state->context = context; state->silence_samples = silence_ms * 16;
    state->threshold = threshold; return 1;
}
static int endpoint_frame(NmEndpoint *state, uint32_t count) {
    double energy = 0; uint32_t index; int speech;
    for (index = 0; index < count; ++index) energy += (double)state->frame[index] * state->frame[index];
    speech = energy > (double)state->threshold * state->threshold * count;
    state->samples += count;
    if (!state->active) {
        for (index = 0; index < count; ++index) {
            state->preroll[state->head] = state->frame[index];
            state->head = (state->head + 1) % NM_LIVE_PREROLL;
            if (state->retained < NM_LIVE_PREROLL) ++state->retained;
            else ++state->idle_samples;
        }
        state->speech_frames = speech ? state->speech_frames + 1 : 0;
        if (state->speech_frames >= 3) {
            uint32_t start = (state->head + NM_LIVE_PREROLL - state->retained) % NM_LIVE_PREROLL;
            uint32_t first = NM_LIVE_PREROLL - start;
            if (first > state->retained) first = state->retained;
            if (!state->ops.begin(state->context, state->samples - state->retained) ||
                !endpoint_emit(state, state->preroll + start, first) ||
                (first < state->retained && !endpoint_emit(state, state->preroll, state->retained - first)))
                return endpoint_error(state, "endpoint start consumer failed");
            state->active = 1; state->retained = 0; state->quiet_samples = 0; state->last_speech = state->samples;
        }
    } else {
        if (speech) {
            if (!endpoint_emit(state, state->quiet, state->quiet_samples) ||
                !endpoint_emit(state, state->frame, count)) return 0;
            state->quiet_samples = 0; state->last_speech = state->samples;
        } else {
            if (count > NM_LIVE_QUIET_LIMIT - state->quiet_samples)
                return endpoint_error(state, "endpoint tentative silence exceeded bound");
            memcpy(state->quiet + state->quiet_samples, state->frame, count * sizeof(float));
            state->quiet_samples += count;
        }
        if (state->quiet_samples >= state->silence_samples) {
            uint32_t kept = state->quiet_samples < NM_LIVE_TAIL ? state->quiet_samples : NM_LIVE_TAIL;
            uint32_t trimmed = state->quiet_samples - kept;
            if (!endpoint_emit(state, state->quiet, kept)) return 0;
            state->trimmed_samples += trimmed;
            if (!state->ops.end(state->context, state->samples - trimmed, state->last_speech))
                return endpoint_error(state, "endpoint final consumer failed");
            ++state->utterances; state->active = state->speech_frames = state->quiet_samples = state->retained = 0;
        }
    }
    return 1;
}
int nm_endpoint_feed(NmEndpoint *state, const float *samples, uint32_t count) {
    uint32_t index;
    if (state->error || (!samples && count)) return endpoint_error(state, "invalid or failed endpoint input");
    for (index = 0; index < count; ++index) {
        if (!math_is_finite(samples[index]) || samples[index] < -1 || samples[index] > 1)
            return endpoint_error(state, "endpoint audio must be finite normalized PCM");
        state->frame[state->pending++] = samples[index];
        if (state->pending == NM_LIVE_FRAME) {
            if (!endpoint_frame(state, state->pending)) return 0;
            state->pending = 0;
        }
    }
    return 1;
}
int nm_endpoint_finish(NmEndpoint *state) {
    if (state->error) return 0;
    if (state->pending && !endpoint_frame(state, state->pending)) return 0;
    state->pending = 0;
    if (state->active) {
        if (!endpoint_emit(state, state->quiet, state->quiet_samples)) return 0;
        if (!state->ops.end(state->context, state->samples, state->last_speech))
            return endpoint_error(state, "endpoint EOF consumer failed");
        ++state->utterances; state->active = state->quiet_samples = 0;
    }
    state->idle_samples += state->retained; state->retained = 0;
    return 1;
}
