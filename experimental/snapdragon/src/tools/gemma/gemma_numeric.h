#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_GEMMA_NUMERIC_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_GEMMA_NUMERIC_H

#include "gemma_model.h"

float gemma_numeric_f16(unsigned short bits);
int gemma_numeric_dequantize(const unsigned char *weights, unsigned int bits,
    unsigned int count, unsigned short scale, float *output);
int gemma_numeric_rope(const float *input, const float *cosine, const float *sine,
    unsigned int width, float *output);
int gemma_numeric_visible(unsigned int layer, unsigned int query, unsigned int key);
int gemma_numeric_mask(unsigned int layer, unsigned int query_start, unsigned int query_count,
    unsigned int key_start, unsigned int key_count, float *output, unsigned int capacity);
int gemma_numeric_kv_offset(unsigned int layer, unsigned int head,
    unsigned int position, unsigned int channel, unsigned long long *offset);
int gemma_numeric_argmax(const float *values, unsigned int count, unsigned int *token);

#endif