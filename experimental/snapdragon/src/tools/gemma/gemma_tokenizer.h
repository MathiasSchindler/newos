#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_GEMMA_TOKENIZER_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_GEMMA_TOKENIZER_H

#include "gemma_artifact.h"

#define GEMMA_TOKENIZER_MAX_BYTES 196608U
#define GEMMA_TOKENIZER_MAX_ARTIFACT_BYTES 33554432U
#define GEMMA_TOKENIZER_TABLE_NAME "tokenizer/bpe-tables-v1"

typedef struct GemmaTokenizer {
    const unsigned char *pieces;
    const unsigned char *lexical;
    const unsigned char *added;
    const unsigned char *merges;
    const unsigned char *languages;
    const unsigned char *strings;
    unsigned int piece_count;
    unsigned int lexical_count;
    unsigned int added_count;
    unsigned int merge_count;
    unsigned int language_count;
    unsigned int string_bytes;
    unsigned int byte_ids[256];
} GemmaTokenizer;

typedef struct GemmaBpeNode {
    unsigned int token;
    unsigned int previous;
    unsigned int next;
    unsigned int rank;
    unsigned int merged;
    unsigned int heap_position;
} GemmaBpeNode;

typedef struct GemmaTokenizerWork {
    GemmaBpeNode nodes[GEMMA_TOKENIZER_MAX_BYTES];
    unsigned int heap[GEMMA_TOKENIZER_MAX_BYTES];
    unsigned int heap_size;
    unsigned char prompt[GEMMA_TOKENIZER_MAX_BYTES];
} GemmaTokenizerWork;

int gemma_utf8_valid(const unsigned char *text, unsigned int size);
int gemma_tokenizer_open(GemmaTokenizer *tokenizer, const unsigned char *artifact,
                         unsigned long long size);
int gemma_tokenizer_encode(const GemmaTokenizer *tokenizer, GemmaTokenizerWork *work,
    const unsigned char *text, unsigned int size, int add_bos,
    unsigned int *tokens, unsigned int capacity, unsigned int *count);
int gemma_tokenizer_decode(const GemmaTokenizer *tokenizer,
    const unsigned int *tokens, unsigned int count, int skip_special,
    unsigned char *text, unsigned int capacity, unsigned int *size);
int gemma_tokenizer_prompt(const GemmaTokenizer *tokenizer, GemmaTokenizerWork *work,
    const char *source_language, const char *target_language,
    const unsigned char *text, unsigned int size, unsigned int maximum_new_tokens,
    unsigned int *tokens, unsigned int capacity, unsigned int *count);

#endif