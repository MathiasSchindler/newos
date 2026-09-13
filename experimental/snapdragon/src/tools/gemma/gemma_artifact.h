#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_GEMMA_ARTIFACT_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_GEMMA_ARTIFACT_H

#include "gemma_model.h"

#define GEMMA_ARTIFACT_HEADER_SIZE 256U
#define GEMMA_ARTIFACT_MAX_RANK 8U
#define GEMMA_ARTIFACT_SHA256_SIZE 32U
#define GEMMA_ARTIFACT_NO_AXIS 0xffffffffU

typedef enum GemmaArtifactKind {
    GEMMA_ARTIFACT_KIND_TENSOR = 1,
    GEMMA_ARTIFACT_KIND_TOKENIZER = 2,
    GEMMA_ARTIFACT_KIND_LAYER_TABLE = 3,
    GEMMA_ARTIFACT_KIND_ROPE_TABLE = 4,
    GEMMA_ARTIFACT_KIND_FIXTURE = 5,
    GEMMA_ARTIFACT_KIND_QNN_CONTEXT = 6
} GemmaArtifactKind;

typedef enum GemmaArtifactElementType {
    GEMMA_ARTIFACT_ELEMENT_F16 = 1,
    GEMMA_ARTIFACT_ELEMENT_S8 = 2,
    GEMMA_ARTIFACT_ELEMENT_S4 = 3,
    GEMMA_ARTIFACT_ELEMENT_U8 = 4
} GemmaArtifactElementType;

typedef enum GemmaArtifactQuantization {
    GEMMA_ARTIFACT_QUANTIZATION_NONE = 0,
    GEMMA_ARTIFACT_QUANTIZATION_SYMMETRIC_GROUP = 1
} GemmaArtifactQuantization;

typedef enum GemmaArtifactLayout {
    GEMMA_ARTIFACT_LAYOUT_ROW_MAJOR = 1,
    GEMMA_ARTIFACT_LAYOUT_OPAQUE = 2
} GemmaArtifactLayout;

typedef struct GemmaArtifactHeader {
    unsigned int kind;
    unsigned int element_type;
    unsigned int quantization;
    unsigned int layout;
    unsigned int rank;
    unsigned int group_size;
    unsigned int quantization_axis;
    unsigned long long element_count;
    unsigned long long payload_size;
    unsigned long long data_size;
    unsigned long long scale_count;
    unsigned long long scale_offset;
    unsigned long long tensor_id;
    unsigned long long dimensions[GEMMA_ARTIFACT_MAX_RANK];
    unsigned char model_sha256[GEMMA_ARTIFACT_SHA256_SIZE];
    unsigned char name_sha256[GEMMA_ARTIFACT_SHA256_SIZE];
    unsigned char payload_sha256[GEMMA_ARTIFACT_SHA256_SIZE];
} GemmaArtifactHeader;

void gemma_artifact_encode_header(
    unsigned char output[GEMMA_ARTIFACT_HEADER_SIZE],
    const GemmaArtifactHeader *header
);
int gemma_artifact_decode_header(
    const unsigned char input[GEMMA_ARTIFACT_HEADER_SIZE],
    GemmaArtifactHeader *header
);
int gemma_artifact_header_valid(
    const GemmaArtifactHeader *header,
    const GemmaModelConfig *model
);
int gemma_artifact_header_matches_name(
    const GemmaArtifactHeader *header,
    const char *name
);
int gemma_artifact_payload_valid(
    const GemmaArtifactHeader *header,
    const void *payload,
    unsigned long long payload_size
);
unsigned long long gemma_artifact_tensor_id(const char *name);
void gemma_artifact_name_sha256(
    const char *name,
    unsigned char output[GEMMA_ARTIFACT_SHA256_SIZE]
);
void gemma_artifact_model_sha256(
    const GemmaModelConfig *model,
    unsigned char output[GEMMA_ARTIFACT_SHA256_SIZE]
);
int gemma_artifact_unpack_s4(
    const unsigned char *packed,
    unsigned long long element_count,
    signed char *output,
    unsigned long long output_capacity
);

#endif