#ifndef NEWOS_NEMOTRON_READER_H
#define NEWOS_NEMOTRON_READER_H
#include "nemotron_format.h"
#include "qnn_abi.h"
#include "runtime.h"

typedef struct NmArtifact {
    const u8 *header, *payload;
    u64 payload_bytes, metadata_bytes, binary_offset;
    u32 kind, precision, tensors, nodes, steps;
    int pipeline;
    int benchmark;
    const char *error;
    const char *error_name;
} NmArtifact;

u32 nm_u32(const u8 *p);
u64 nm_u64(const u8 *p);
void nm_put32(u8 *p, u32 value);
void nm_put64(u8 *p, u64 value);
u64 nm_align16(u64 n);
const u8 *nm_tensor(const NmArtifact *a, u32 index);
const u8 *nm_step(const NmArtifact *a, u32 step);
const u8 *nm_cell(const NmArtifact *a, u32 step, u32 index);
u64 nm_tensor_bytes(const u8 *tensor);
u64 nm_expected_bytes(const u8 *tensor);
int nm_quantized(const u8 *tensor);
void nm_zero(const u8 *tensor, u8 *data);
u32 nm_compare(const u8 *tensor, const u8 *actual, const u8 *expected, u8 *metric);
int nm_same_shape(const u8 *a, const u8 *b);
int nm_active(const NmArtifact *a, u32 step, u32 graph);
int nm_finite(const u8 *data, u64 bytes, u32 dtype);
float nm_float(const u8 *data, u32 dtype);
/* Validates the complete immutable file before QNN calls. RESULT may contain diagnostic NaNs. */
int nm_validate(NmArtifact *a, const void *file, u64 bytes);
const u8 *nm_runtime_entry(const NmArtifact *a, u32 index);
const float *nm_runtime_window(const NmArtifact *a);
const float *nm_runtime_mel(const NmArtifact *a);
int nm_runtime_graph_contract(NmArtifact *a, u32 role);
const u8 *nm_runtime_tokenizer(const NmArtifact *a, u64 *bytes);
void nm_tensor_binding(QnnTensor *binding, const u8 *tensor, void *buffer, const u8 *payload);
void nm_header(u8 *header, u32 kind, u32 precision, u32 tensors, u32 nodes,
               u32 steps, const u8 *payload, u64 bytes);
#endif
