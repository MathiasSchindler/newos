#ifndef NEWOS_NEMOTRON_FRONTEND_H
#define NEWOS_NEMOTRON_FRONTEND_H
#include <stddef.h>
#include <stdint.h>

#define NM_AUDIO_RATE 16000U
#define NM_AUDIO_FFT 512U
#define NM_AUDIO_HOP 160U
#define NM_AUDIO_MELS 128U
#define NM_AUDIO_FIRST 25U
#define NM_AUDIO_STEADY 32U
#define NM_AUDIO_FIRST_FLAG 1U
#define NM_AUDIO_FINAL_FLAG 2U
#define NM_AUDIO_END_FLAG 4U

typedef int (*NmFeatureSink)(void *context, const float *features, uint32_t width,
                             uint32_t valid, uint32_t flags, uint64_t index);

typedef struct NmFrontend {
    const float *window, *mel;
    NmFeatureSink sink;
    void *context;
    const char *error;
    uint64_t samples, frames, chunks;
    uint32_t head, count, valid, width, closed, failed;
    float previous, ring[NM_AUDIO_FFT], features[NM_AUDIO_STEADY * NM_AUDIO_MELS];
    double real[NM_AUDIO_FFT], imaginary[NM_AUDIO_FFT];
    double cosine[NM_AUDIO_FFT / 2], sine[NM_AUDIO_FFT / 2];
} NmFrontend;

int nm_frontend_init(NmFrontend *state, const float *window, const float *mel,
                     NmFeatureSink sink, void *context);
int nm_frontend_feed(NmFrontend *state, const float *samples, size_t count);
int nm_frontend_finish(NmFrontend *state);
int nm_frontend_reset(NmFrontend *state);
#endif
