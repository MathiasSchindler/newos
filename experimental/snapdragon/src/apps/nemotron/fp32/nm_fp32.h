#ifndef NEWOS_NEMOTRON_FP32_H
#define NEWOS_NEMOTRON_FP32_H

#include <stddef.h>
#include <stdint.h>

#define NM_FP32_PACKAGE "newos.nemotron.fp32"
#define NM_FP32_OPERATOR "NmExecute"
#define NM_FP32_GATHER "NmGather"
#define NM_FP32_META_MAGIC 0x4e4d3332U
#define NM_FP32_META_VERSION 1U
#define NM_FP32_PACKED_VERSION 2U
#define NM_FP32_META_BASE 34U
#define NM_FP32_MAX_INPUTS 4U
#define NM_FP32_MAX_PARAMS 6U
#define NM_FP32_DTYPE_FLOAT 0x0232U
#define NM_FP32_DTYPE_INT 0x0032U

enum NmFp32Op {
    NM_FP32_MATMUL = 1, NM_FP32_ADD, NM_FP32_MULTIPLY, NM_FP32_RESHAPE,
    NM_FP32_TRANSPOSE, NM_FP32_SLICE, NM_FP32_CONCAT, NM_FP32_NORM,
    NM_FP32_SIGMOID, NM_FP32_TANH, NM_FP32_RELU, NM_FP32_SOFTMAX,
    NM_FP32_PAD, NM_FP32_CONV, NM_FP32_DEPTHWISE, NM_FP32_GATHER_OP,
    NM_FP32_FC, NM_FP32_SUBTRACT, NM_FP32_RSQRT, NM_FP32_MEAN,
    NM_FP32_SELECT
};

enum NmFp32Param {
    NM_FP32_AXIS = 1, NM_FP32_PERM, NM_FP32_RANGES, NM_FP32_EPSILON,
    NM_FP32_STRIDE, NM_FP32_DILATION, NM_FP32_PAD_AMOUNT, NM_FP32_SCHEME,
    NM_FP32_PAD_VALUE, NM_FP32_TRANSPOSE_A, NM_FP32_TRANSPOSE_B,
    NM_FP32_BETA, NM_FP32_KEEP_DIMS, NM_FP32_AXES, NM_FP32_WEIGHT_TILES
};

int nm_fp32_execute(
    const uint32_t *metadata, size_t metadata_words,
    const void *const *inputs, const size_t *input_counts, size_t num_inputs,
    float *output, size_t output_count, unsigned int slice, unsigned int slices
);

#endif
