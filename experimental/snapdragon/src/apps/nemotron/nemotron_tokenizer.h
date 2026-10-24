#ifndef NEWOS_NEMOTRON_TOKENIZER_H
#define NEWOS_NEMOTRON_TOKENIZER_H
#include <stddef.h>
#include <stdint.h>
typedef struct NmTokenizer {
    const unsigned char *entries, *strings;
    uint32_t count, string_bytes;
    const char *error;
} NmTokenizer;
int nm_tokenizer_open(NmTokenizer *tokenizer, const void *data, uint64_t bytes);
int nm_tokenizer_decode(NmTokenizer *tokenizer, const uint32_t *tokens, size_t count,
                        int skip_special, char *output, size_t capacity, size_t *bytes);
#endif
