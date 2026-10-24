#ifndef NEWOS_NEMOTRON_AUDIO_H
#define NEWOS_NEMOTRON_AUDIO_H
#include <stdint.h>
#include <stddef.h>
#define NM_RESAMPLE_TAPS 64U
#define NM_RESAMPLE_PHASES 160U
#define NM_LIVE_FRAME 160U
#define NM_LIVE_PREROLL 4800U
#define NM_LIVE_QUIET_LIMIT 160000U
#define NM_LIVE_TAIL 3200U
typedef int (*NmAudioSink)(void *context, const float *samples, uint32_t count);
typedef struct NmResampler {
    uint32_t rate, channels, format, phases, gcd, phase, head;
    uint64_t input_frames, output_samples, clipped;
    float history[NM_RESAMPLE_TAPS], weights[NM_RESAMPLE_PHASES][NM_RESAMPLE_TAPS];
    const char *error;
} NmResampler;
int nm_resampler_init(NmResampler *state, uint32_t rate, uint32_t channels, uint32_t format);
int nm_resampler_feed(NmResampler *state, const void *samples, uint32_t frames, int silent,
                      NmAudioSink sink, void *context);

typedef struct NmAudioQueue {
    float *samples;
    uint32_t capacity, wake, stop, ended, failure, peak;
    uint64_t read, write;
    uint64_t accepted, consumed;
} NmAudioQueue;
void nm_audio_queue_init(NmAudioQueue *queue, float *samples, uint32_t capacity);
int nm_audio_queue_push(void *context, const float *samples, uint32_t count);
uint32_t nm_audio_queue_pop(NmAudioQueue *queue, float *samples, uint32_t capacity);
void nm_audio_queue_end(NmAudioQueue *queue, uint32_t failure);
void nm_audio_queue_stop(NmAudioQueue *queue);
uint32_t nm_audio_queue_count(const NmAudioQueue *queue);

typedef struct NmEndpointOps {
    int (*begin)(void *context, uint64_t sample);
    int (*feed)(void *context, const float *samples, uint32_t count);
    int (*end)(void *context, uint64_t sample, uint64_t last_speech);
} NmEndpointOps;
typedef struct NmEndpoint {
    NmEndpointOps ops;
    void *context;
    float preroll[NM_LIVE_PREROLL], frame[NM_LIVE_FRAME], quiet[NM_LIVE_QUIET_LIMIT], threshold;
    uint32_t head, retained, pending, speech_frames, quiet_samples, silence_samples, active;
    uint64_t samples, last_speech, utterances, idle_samples, model_samples, trimmed_samples;
    const char *error;
} NmEndpoint;
int nm_endpoint_init(NmEndpoint *state, uint32_t silence_ms, float threshold, NmEndpointOps ops, void *context);
int nm_endpoint_feed(NmEndpoint *state, const float *samples, uint32_t count);
int nm_endpoint_finish(NmEndpoint *state);
#endif
