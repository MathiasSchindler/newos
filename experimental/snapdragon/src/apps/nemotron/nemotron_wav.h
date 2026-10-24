#ifndef NEWOS_NEMOTRON_WAV_H
#define NEWOS_NEMOTRON_WAV_H
#include <stdint.h>
typedef struct NmInput {
    void *context;
    uint64_t bytes;
    int (*read_at)(void *context, uint64_t offset, void *data, uint32_t bytes);
} NmInput;
typedef struct NmWav {
    NmInput input;
    uint64_t offset, samples, cursor;
    const char *error;
} NmWav;
int nm_wav_open(NmWav *wav, const NmInput *input);
int nm_wav_read(NmWav *wav, float *samples, uint32_t capacity, uint32_t *count);
int nm_wav_reset(NmWav *wav);
#endif
