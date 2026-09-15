#ifndef NEWOS_GEMMA_BLOCK_H
#define NEWOS_GEMMA_BLOCK_H

#include "qnn_abi.h"
#include "gemma_artifact.h"

enum { GEMMA_BLOCK_TENSORS = 160, GEMMA_BLOCK_TOKENS = 3, GEMMA_BLOCK_CACHE = 2048 };

typedef struct GemmaBlockTensor {
    QnnTensor tensor;
    u32 dimensions[4];
    char name[80];
    void *buffer;
    u32 bytes;
} GemmaBlockTensor;

typedef struct GemmaBlockHost {
    void *user;
    void *(*allocate)(void *user, u64 bytes);
    const void *(*weight)(void *user, const char *suffix, GemmaArtifactHeader *header);
    void (*status)(const char *operation, u64 status);
} GemmaBlockHost;

typedef struct GemmaBlock {
    const QnnInterfaceV2 *api;
    QnnGraphHandle graph;
    GemmaBlockHost host;
    GemmaBlockTensor tensors[GEMMA_BLOCK_TENSORS];
    u32 count;
    u32 bits;
    u32 tokens;
    u32 cache;
    u32 internal;
    char prefix[16];
    const QnnTensor *hidden_input;
    u64 error;
} GemmaBlock;

u32 gemma_block_tensor(GemmaBlock *, const char *, u32, u32, const u32 *, u32, void *);
u32 gemma_block_node(GemmaBlock *, const char *, const char *, const u32 *, u32, u32, QnnParam *, u32);
u32 gemma_block_projection(GemmaBlock *, u32, const char *, const char *, u32, u32);
int gemma_block_build(GemmaBlock *, const QnnInterfaceV2 *, QnnContextHandle, GemmaBlockHost, u32);
int gemma_block_build_shape(GemmaBlock *, const QnnInterfaceV2 *, QnnContextHandle, GemmaBlockHost, u32, u32, u32);
int gemma_block_logits(GemmaBlock *);

#endif