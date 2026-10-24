#ifndef NEWOS_NEMOTRON_RNNT_H
#define NEWOS_NEMOTRON_RNNT_H
#include <stddef.h>
#include <stdint.h>
typedef struct NmRnntOps {
    int (*reset)(void *context);
    int (*predict)(void *context, uint32_t token);
    int (*join)(void *context, const float *encoder, const float **logits, uint32_t *count);
} NmRnntOps;
typedef struct NmRnnt {
    NmRnntOps ops;
    void *context;
    uint32_t *tokens, blank, vocabulary, max_symbols;
    size_t count, capacity;
    uint64_t frames, prediction_calls, joiner_calls;
    int failed;
    const char *error;
} NmRnnt;
int nm_rnnt_init(NmRnnt *state, NmRnntOps ops, void *context, uint32_t *tokens, size_t capacity);
int nm_rnnt_reset(NmRnnt *state);
int nm_rnnt_frame(NmRnnt *state, const float *encoder);
#endif
