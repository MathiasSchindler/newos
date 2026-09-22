#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_TENSOR_INDEX_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_TENSOR_INDEX_H

enum {
    WHISPER_TENSOR_NAME_SIZE = 96,
    WHISPER_TENSOR_MAX_RANK = 8,
    WHISPER_TENSOR_MAX_COUNT = 2048,
    WHISPER_TENSOR_F32 = 1,
    WHISPER_TENSOR_F16 = 2
};

typedef struct WhisperTensorIndex {
    char name[WHISPER_TENSOR_NAME_SIZE];
    unsigned int type;
    unsigned int rank;
    unsigned long long shape[WHISPER_TENSOR_MAX_RANK];
    unsigned long long start;
    unsigned long long end;
} WhisperTensorIndex;

int whisper_tensor_index_parse(
    const unsigned char *json, unsigned int length,
    WhisperTensorIndex *entries, unsigned int capacity, unsigned int *count_out
);
int whisper_tensor_index_validate(
    const WhisperTensorIndex *entries, unsigned int count, unsigned long long data_size
);

#endif