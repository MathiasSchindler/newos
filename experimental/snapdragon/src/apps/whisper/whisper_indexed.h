#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_INDEXED_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_INDEXED_H

#include "whisper_model.h"
#include "whisper_tensor_index.h"

typedef struct WhisperIndexed {
    int fd;
    unsigned int count;
    unsigned long long data_offset;
    WhisperTensorIndex tensors[WHISPER_TENSOR_MAX_COUNT];
    unsigned long long hashes[WHISPER_TENSOR_MAX_COUNT];
} WhisperIndexed;

int whisper_indexed_open(WhisperIndexed *model, const char *path, const WhisperModelConfig *config);
void whisper_indexed_close(WhisperIndexed *model);
const WhisperTensorIndex *whisper_indexed_find(const WhisperIndexed *model, const char *name);
int whisper_indexed_read(
    const WhisperIndexed *model, const WhisperTensorIndex *tensor,
    void *buffer, unsigned long long capacity
);

#endif