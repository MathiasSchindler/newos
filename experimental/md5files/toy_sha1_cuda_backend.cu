#include <cuda_runtime.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#define PAYLOAD_SIZE 32U
#define SHA1_BLOCK_SIZE 64U
#define SHA1_DIGEST_SIZE 20U
#define EMPTY_NONCE 0xffffffffU
#define CUDA_THREADS 256U
#define CUDA_BLOCKS 512U

#define A_CANDIDATES_24 65536U
#define A_CANDIDATES_32 1048576U
#define A_CANDIDATES_40 4194304U
#define A_CANDIDATES_48 33554432U
#define A_CANDIDATES_56 33554432U
#define A_CANDIDATES_64 33554432U
#define B_LIMIT_24 16777216U
#define B_LIMIT_32 16777216U
#define B_LIMIT_40 16777216U
#define B_LIMIT_48 33554432U
#define B_LIMIT_56 33554432U
#define B_LIMIT_64 33554432U
#define TABLE_CAPACITY_24 262144U
#define TABLE_CAPACITY_32 4194304U
#define TABLE_CAPACITY_40 16777216U
#define TABLE_CAPACITY_48 134217728U
#define TABLE_CAPACITY_56 134217728U
#define TABLE_CAPACITY_64 134217728U

typedef struct {
    unsigned char *data;
    size_t size;
} Buffer;

typedef struct {
    uint32_t state[5];
    unsigned long long bit_count;
    unsigned char buffer[SHA1_BLOCK_SIZE];
    size_t buffer_len;
} Sha1Context;

static void free_buffer(Buffer *buffer) {
    free(buffer->data);
    buffer->data = 0;
    buffer->size = 0U;
}

static int read_file(const char *path, Buffer *buffer) {
    FILE *file;
    long size;

    memset(buffer, 0, sizeof(*buffer));
    file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        perror(path);
        fclose(file);
        return 1;
    }
    size = ftell(file);
    if (size < 0) {
        perror(path);
        fclose(file);
        return 1;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        perror(path);
        fclose(file);
        return 1;
    }
    buffer->size = (size_t)size;
    buffer->data = (unsigned char *)malloc(buffer->size == 0U ? 1U : buffer->size);
    if (!buffer->data) {
        fprintf(stderr, "toy_sha1_cuda_backend: out of memory reading %s\n", path);
        fclose(file);
        return 1;
    }
    if (buffer->size != 0U && fread(buffer->data, 1U, buffer->size, file) != buffer->size) {
        fprintf(stderr, "toy_sha1_cuda_backend: cannot read %s\n", path);
        fclose(file);
        free_buffer(buffer);
        return 1;
    }
    if (fclose(file) != 0) {
        perror(path);
        free_buffer(buffer);
        return 1;
    }
    return 0;
}

static int write_file(const char *path, const unsigned char *data, size_t size) {
    FILE *file = fopen(path, "wb");

    if (!file) {
        perror(path);
        return 1;
    }
    if (fwrite(data, 1U, size, file) != size) {
        fprintf(stderr, "toy_sha1_cuda_backend: cannot write %s\n", path);
        fclose(file);
        return 1;
    }
    if (fclose(file) != 0) {
        perror(path);
        return 1;
    }
    return 0;
}

static const char *required_env(const char *name) {
    const char *value = getenv(name);

    if (!value || value[0] == '\0') {
        fprintf(stderr, "toy_sha1_cuda_backend: missing %s\n", name);
        return 0;
    }
    return value;
}

static int parse_u32_env(const char *name, uint32_t *value) {
    const char *text = getenv(name);
    char *end = 0;
    unsigned long parsed;

    if (!text || text[0] == '\0') return 0;
    parsed = strtoul(text, &end, 10);
    if (!end || *end != '\0' || parsed > UINT32_MAX) {
        fprintf(stderr, "toy_sha1_cuda_backend: invalid %s=%s\n", name, text);
        return 1;
    }

    *value = (uint32_t)parsed;
    return 0;
}

static int parse_uint_env(const char *name, unsigned int *value) {
    const char *text = getenv(name);
    char *end = 0;
    unsigned long parsed;

    if (!text || text[0] == '\0') return 0;
    parsed = strtoul(text, &end, 10);
    if (!end || *end != '\0' || parsed > UINT_MAX) {
        fprintf(stderr, "toy_sha1_cuda_backend: invalid %s=%s\n", name, text);
        return 1;
    }
    *value = (unsigned int)parsed;
    return 0;
}

static int parse_size_env(const char *name, size_t *value) {
    const char *text = getenv(name);
    char *end = 0;
    unsigned long long parsed;

    if (!text || text[0] == '\0') return 0;
    parsed = strtoull(text, &end, 10);
    if (!end || *end != '\0' || parsed > (unsigned long long)(size_t)-1) {
        fprintf(stderr, "toy_sha1_cuda_backend: invalid %s=%s\n", name, text);
        return 1;
    }
    *value = (size_t)parsed;
    return 0;
}

static int configure_profile(const char *hash_name,
                             unsigned int *bits_out,
                             uint32_t *a_candidates_out,
                             uint32_t *b_limit_out,
                             size_t *table_capacity_out) {
    if (strcmp(hash_name, "sha1-24") == 0) {
        *bits_out = 24U;
        *a_candidates_out = A_CANDIDATES_24;
        *b_limit_out = B_LIMIT_24;
        *table_capacity_out = TABLE_CAPACITY_24;
        return 0;
    }
    if (strcmp(hash_name, "sha1-32") == 0) {
        *bits_out = 32U;
        *a_candidates_out = A_CANDIDATES_32;
        *b_limit_out = B_LIMIT_32;
        *table_capacity_out = TABLE_CAPACITY_32;
        return 0;
    }
    if (strcmp(hash_name, "sha1-40") == 0) {
        *bits_out = 40U;
        *a_candidates_out = A_CANDIDATES_40;
        *b_limit_out = B_LIMIT_40;
        *table_capacity_out = TABLE_CAPACITY_40;
        return 0;
    }
    if (strcmp(hash_name, "sha1-48") == 0) {
        *bits_out = 48U;
        *a_candidates_out = A_CANDIDATES_48;
        *b_limit_out = B_LIMIT_48;
        *table_capacity_out = TABLE_CAPACITY_48;
        return 0;
    }
    if (strcmp(hash_name, "sha1-56") == 0) {
        *bits_out = 56U;
        *a_candidates_out = A_CANDIDATES_56;
        *b_limit_out = B_LIMIT_56;
        *table_capacity_out = TABLE_CAPACITY_56;
        return 0;
    }
    if (strcmp(hash_name, "sha1-64") == 0) {
        *bits_out = 64U;
        *a_candidates_out = A_CANDIDATES_64;
        *b_limit_out = B_LIMIT_64;
        *table_capacity_out = TABLE_CAPACITY_64;
        return 0;
    }
    fprintf(stderr, "toy_sha1_cuda_backend: expected sha1-24/32/40/48/56/64, got %s\n", hash_name);
    return 1;
}

static int is_power_of_two_size(size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

static __host__ __device__ uint32_t sha1_rotl32(uint32_t value, unsigned int count) {
    return (value << count) | (value >> (32U - count));
}

static __host__ __device__ uint32_t sha1_load_be32(const unsigned char *bytes) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static __host__ __device__ void sha1_store_be32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value >> 24);
    bytes[1] = (unsigned char)(value >> 16);
    bytes[2] = (unsigned char)(value >> 8);
    bytes[3] = (unsigned char)value;
}

static __host__ __device__ void sha1_transform(Sha1Context *ctx, const unsigned char block[SHA1_BLOCK_SIZE]) {
    uint32_t words[80];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    unsigned int round;

    for (round = 0U; round < 16U; ++round) {
        words[round] = sha1_load_be32(block + (size_t)round * 4U);
    }
    for (round = 16U; round < 80U; ++round) {
        words[round] = sha1_rotl32(words[round - 3U] ^ words[round - 8U] ^ words[round - 14U] ^ words[round - 16U], 1U);
    }
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    for (round = 0U; round < 80U; ++round) {
        uint32_t f;
        uint32_t k;
        uint32_t next;

        if (round < 20U) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999U;
        } else if (round < 40U) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1U;
        } else if (round < 60U) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcU;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6U;
        }
        next = sha1_rotl32(a, 5U) + f + e + k + words[round];
        e = d;
        d = c;
        c = sha1_rotl32(b, 30U);
        b = a;
        a = next;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

static __host__ __device__ void sha1_init(Sha1Context *ctx) {
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xefcdab89U;
    ctx->state[2] = 0x98badcfeU;
    ctx->state[3] = 0x10325476U;
    ctx->state[4] = 0xc3d2e1f0U;
    ctx->bit_count = 0ULL;
    ctx->buffer_len = 0U;
}

static __host__ __device__ void sha1_update(Sha1Context *ctx, const unsigned char *data, size_t len) {
    size_t offset = 0U;

    ctx->bit_count += (unsigned long long)len * 8ULL;
    if (ctx->buffer_len == 0U) {
        while (len - offset >= SHA1_BLOCK_SIZE) {
            sha1_transform(ctx, data + offset);
            offset += SHA1_BLOCK_SIZE;
        }
    }
    while (offset < len) {
        size_t space = SHA1_BLOCK_SIZE - ctx->buffer_len;
        size_t chunk = (len - offset < space) ? (len - offset) : space;
        size_t index;

        for (index = 0U; index < chunk; ++index) {
            ctx->buffer[ctx->buffer_len + index] = data[offset + index];
        }
        ctx->buffer_len += chunk;
        offset += chunk;
        if (ctx->buffer_len == SHA1_BLOCK_SIZE) {
            sha1_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0U;
        }
    }
}

static __host__ __device__ void sha1_final(Sha1Context *ctx, unsigned char out[SHA1_DIGEST_SIZE]) {
    unsigned char pad = 0x80U;
    unsigned char zero = 0U;
    unsigned char length_bytes[8];
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        length_bytes[index] = (unsigned char)(ctx->bit_count >> (56U - 8U * index));
    }
    sha1_update(ctx, &pad, 1U);
    while (ctx->buffer_len != 56U) {
        sha1_update(ctx, &zero, 1U);
    }
    sha1_update(ctx, length_bytes, sizeof(length_bytes));
    for (index = 0U; index < 5U; ++index) {
        sha1_store_be32(out + (size_t)index * 4U, ctx->state[index]);
    }
}

static __host__ __device__ void build_payload(unsigned char payload[PAYLOAD_SIZE], char side, uint32_t nonce) {
    const char label[] = "newos-sha1-cu-";
    unsigned int index;

    for (index = 0U; index < PAYLOAD_SIZE; ++index) payload[index] = 0U;
    for (index = 0U; index < sizeof(label) - 1U; ++index) payload[index] = (unsigned char)label[index];
    payload[14] = (unsigned char)side;
    payload[24] = (unsigned char)(nonce & 0xffU);
    payload[25] = (unsigned char)((nonce >> 8U) & 0xffU);
    payload[26] = (unsigned char)((nonce >> 16U) & 0xffU);
    payload[27] = (unsigned char)((nonce >> 24U) & 0xffU);
    nonce = ~nonce;
    payload[28] = (unsigned char)(nonce & 0xffU);
    payload[29] = (unsigned char)((nonce >> 8U) & 0xffU);
    payload[30] = (unsigned char)((nonce >> 16U) & 0xffU);
    payload[31] = (unsigned char)((nonce >> 24U) & 0xffU);
}

static __host__ __device__ uint64_t table_slot_key(uint64_t key) {
    key ^= key >> 33U;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33U;
    return key;
}

static __host__ __device__ uint64_t sha1_truncated_key(const Sha1Context *prefix_context,
                                                        const unsigned char payload[PAYLOAD_SIZE],
                                                        const unsigned char *suffix,
                                                        size_t suffix_size,
                                                        unsigned int bits) {
    Sha1Context context = *prefix_context;
    unsigned char digest[SHA1_DIGEST_SIZE];
    uint64_t key = 0U;
    unsigned int index;

    sha1_update(&context, payload, PAYLOAD_SIZE);
    sha1_update(&context, suffix, suffix_size);
    sha1_final(&context, digest);
    for (index = 0U; index < 8U; ++index) {
        key = (key << 8U) | digest[index];
    }
    if (bits < 64U) {
        key &= ~((1ULL << (64U - bits)) - 1ULL);
    }
    return key;
}

__global__ void build_table_kernel(Sha1Context prefix_context,
                                   const unsigned char *suffix,
                                   size_t suffix_size,
                                   unsigned int bits,
                                   uint32_t nonce_start,
                                   uint32_t a_candidates,
                                   uint64_t *keys,
                                   uint32_t *nonces,
                                   size_t table_capacity) {
    uint32_t stride = blockDim.x * gridDim.x;
    uint32_t nonce = blockIdx.x * blockDim.x + threadIdx.x;

    while (nonce < a_candidates) {
        unsigned char payload[PAYLOAD_SIZE];
        uint32_t actual_nonce = nonce_start + nonce;
        uint64_t key;
        size_t slot;

        build_payload(payload, 'A', actual_nonce);
        key = sha1_truncated_key(&prefix_context, payload, suffix, suffix_size, bits);
        slot = (size_t)table_slot_key(key) & (table_capacity - 1U);
        for (;;) {
            uint32_t old = atomicCAS(nonces + slot, EMPTY_NONCE, actual_nonce);
            if (old == EMPTY_NONCE) {
                keys[slot] = key;
                break;
            }
            if (keys[slot] == key) {
                break;
            }
            slot = (slot + 1U) & (table_capacity - 1U);
        }
        nonce += stride;
    }
}

__global__ void probe_table_kernel(Sha1Context prefix_context,
                                   const unsigned char *suffix,
                                   size_t suffix_size,
                                   unsigned int bits,
                                   uint32_t nonce_start,
                                   uint32_t b_limit,
                                   const uint64_t *keys,
                                   const uint32_t *nonces,
                                   size_t table_capacity,
                                   unsigned int *found,
                                   uint32_t *found_a,
                                   uint32_t *found_b) {
    uint32_t stride = blockDim.x * gridDim.x;
    uint32_t nonce = blockIdx.x * blockDim.x + threadIdx.x;

    while (nonce < b_limit && atomicAdd(found, 0U) == 0U) {
        unsigned char payload[PAYLOAD_SIZE];
        uint32_t actual_nonce = nonce_start + nonce;
        uint64_t key;
        size_t slot;

        build_payload(payload, 'B', actual_nonce);
        key = sha1_truncated_key(&prefix_context, payload, suffix, suffix_size, bits);
        slot = (size_t)table_slot_key(key) & (table_capacity - 1U);
        while (nonces[slot] != EMPTY_NONCE) {
            if (keys[slot] == key) {
                if (atomicCAS(found, 0U, 1U) == 0U) {
                    *found_a = nonces[slot];
                    *found_b = actual_nonce;
                }
                return;
            }
            slot = (slot + 1U) & (table_capacity - 1U);
        }
        nonce += stride;
    }
}

static int cuda_ok(cudaError_t error, const char *operation) {
    if (error != cudaSuccess) {
        fprintf(stderr, "toy_sha1_cuda_backend: %s failed: %s\n", operation, cudaGetErrorString(error));
        return 0;
    }
    return 1;
}

int main(void) {
    const char *hash_name = required_env("NEWOS_HASH_NAME");
    const char *prefix_a_path = required_env("NEWOS_HASH_PREFIX_A");
    const char *prefix_b_path = required_env("NEWOS_HASH_PREFIX_B");
    const char *suffix_path = required_env("NEWOS_HASH_SUFFIX");
    const char *block_a_path = required_env("NEWOS_HASH_BLOCK_A");
    const char *block_b_path = required_env("NEWOS_HASH_BLOCK_B");
    Buffer prefix_a = {0};
    Buffer prefix_b = {0};
    Buffer suffix = {0};
    Sha1Context prefix_a_context;
    Sha1Context prefix_b_context;
    uint64_t *device_keys = 0;
    uint32_t *device_nonces = 0;
    unsigned char *device_suffix = 0;
    unsigned int *device_found = 0;
    uint32_t *device_found_a = 0;
    uint32_t *device_found_b = 0;
    unsigned int found = 0U;
    uint32_t found_a = 0U;
    uint32_t found_b = 0U;
    uint32_t a_candidates;
    uint32_t b_limit;
    uint32_t a_start = 0U;
    uint32_t b_start = 0U;
    size_t table_capacity;
    unsigned int bits;
    unsigned int batches = 1U;
    unsigned char payload_a[PAYLOAD_SIZE];
    unsigned char payload_b[PAYLOAD_SIZE];
    int status = 1;

    if (!hash_name || !prefix_a_path || !prefix_b_path || !suffix_path || !block_a_path || !block_b_path) {
        return 1;
    }
    if (configure_profile(hash_name, &bits, &a_candidates, &b_limit, &table_capacity) != 0 ||
        parse_u32_env("NEWOS_TOY_A_CANDIDATES", &a_candidates) != 0 ||
        parse_u32_env("NEWOS_TOY_B_LIMIT", &b_limit) != 0 ||
        parse_u32_env("NEWOS_TOY_A_START", &a_start) != 0 ||
        parse_u32_env("NEWOS_TOY_B_START", &b_start) != 0 ||
        parse_uint_env("NEWOS_TOY_BATCHES", &batches) != 0 ||
        parse_size_env("NEWOS_TOY_TABLE_CAPACITY", &table_capacity) != 0) {
        return 1;
    }
    if (batches == 0U) {
        fprintf(stderr, "toy_sha1_cuda_backend: NEWOS_TOY_BATCHES must be non-zero\n");
        return 1;
    }
    if (!is_power_of_two_size(table_capacity) || table_capacity <= (size_t)a_candidates * 2U) {
        fprintf(stderr, "toy_sha1_cuda_backend: table capacity must be a power of two and more than twice A candidates\n");
        return 1;
    }
    if (read_file(prefix_a_path, &prefix_a) != 0 ||
        read_file(prefix_b_path, &prefix_b) != 0 ||
        read_file(suffix_path, &suffix) != 0) {
        goto cleanup;
    }
    sha1_init(&prefix_a_context);
    sha1_update(&prefix_a_context, prefix_a.data, prefix_a.size);
    sha1_init(&prefix_b_context);
    sha1_update(&prefix_b_context, prefix_b.data, prefix_b.size);

    fprintf(stderr, "toy_sha1_cuda_backend: bits=%u A=%u B=%u table=%llu batches=%u\n",
            bits, a_candidates, b_limit, (unsigned long long)table_capacity, batches);
    if (!cuda_ok(cudaMalloc((void **)&device_keys, table_capacity * sizeof(device_keys[0])), "cudaMalloc(keys)") ||
        !cuda_ok(cudaMalloc((void **)&device_nonces, table_capacity * sizeof(device_nonces[0])), "cudaMalloc(nonces)") ||
        !cuda_ok(cudaMalloc((void **)&device_suffix, suffix.size == 0U ? 1U : suffix.size), "cudaMalloc(suffix)") ||
        !cuda_ok(cudaMalloc((void **)&device_found, sizeof(*device_found)), "cudaMalloc(found)") ||
        !cuda_ok(cudaMalloc((void **)&device_found_a, sizeof(*device_found_a)), "cudaMalloc(found_a)") ||
        !cuda_ok(cudaMalloc((void **)&device_found_b, sizeof(*device_found_b)), "cudaMalloc(found_b)")) {
        goto cleanup;
    }
    if (!cuda_ok(cudaMemcpy(device_suffix, suffix.data, suffix.size, cudaMemcpyHostToDevice), "cudaMemcpy(suffix)")) {
        goto cleanup;
    }

    for (unsigned int batch = 0U; batch < batches; ++batch) {
        uint32_t batch_a_start = a_start + (uint32_t)((uint64_t)(batch & 15U) * a_candidates);
        uint32_t batch_b_start = b_start + (uint32_t)((uint64_t)(batch >> 4U) * b_limit);
        double attempts = (double)(batch + 1U) * (double)a_candidates * (double)b_limit;
        double probability = 1.0 - exp(-attempts / ldexp(1.0, (int)bits));

        fprintf(stderr, "toy_sha1_cuda_backend: batch %u/%u A_start=%u B_start=%u cumulative_p=%.6f\n",
                batch + 1U, batches, batch_a_start, batch_b_start, probability);
        if (!cuda_ok(cudaMemset(device_nonces, 0xff, table_capacity * sizeof(device_nonces[0])), "cudaMemset(nonces)") ||
            !cuda_ok(cudaMemset(device_found, 0, sizeof(*device_found)), "cudaMemset(found)")) {
            goto cleanup;
        }
        build_table_kernel<<<CUDA_BLOCKS, CUDA_THREADS>>>(prefix_a_context, device_suffix, suffix.size, bits,
                                                          batch_a_start, a_candidates,
                                                          device_keys, device_nonces, table_capacity);
        if (!cuda_ok(cudaGetLastError(), "build_table_kernel launch") ||
            !cuda_ok(cudaDeviceSynchronize(), "build_table_kernel")) {
            goto cleanup;
        }
        probe_table_kernel<<<CUDA_BLOCKS, CUDA_THREADS>>>(prefix_b_context, device_suffix, suffix.size, bits,
                                                          batch_b_start, b_limit,
                                                          device_keys, device_nonces, table_capacity,
                                                          device_found, device_found_a, device_found_b);
        if (!cuda_ok(cudaGetLastError(), "probe_table_kernel launch") ||
            !cuda_ok(cudaDeviceSynchronize(), "probe_table_kernel") ||
            !cuda_ok(cudaMemcpy(&found, device_found, sizeof(found), cudaMemcpyDeviceToHost), "cudaMemcpy(found)")) {
            goto cleanup;
        }
        if (found != 0U) {
            break;
        }
    }
    if (found == 0U) {
        fprintf(stderr, "toy_sha1_cuda_backend: no collision found within batched search limit\n");
        goto cleanup;
    }
    if (!cuda_ok(cudaMemcpy(&found_a, device_found_a, sizeof(found_a), cudaMemcpyDeviceToHost), "cudaMemcpy(found_a)") ||
        !cuda_ok(cudaMemcpy(&found_b, device_found_b, sizeof(found_b), cudaMemcpyDeviceToHost), "cudaMemcpy(found_b)")) {
        goto cleanup;
    }
    build_payload(payload_a, 'A', found_a);
    build_payload(payload_b, 'B', found_b);
    if (write_file(block_a_path, payload_a, PAYLOAD_SIZE) != 0 ||
        write_file(block_b_path, payload_b, PAYLOAD_SIZE) != 0) {
        goto cleanup;
    }
    fprintf(stderr, "toy_sha1_cuda_backend: matched %u-bit SHA-1 prefix after A=%u B=%u\n",
            bits, found_a, found_b);
    status = 0;

cleanup:
    cudaFree(device_keys);
    cudaFree(device_nonces);
    cudaFree(device_suffix);
    cudaFree(device_found);
    cudaFree(device_found_a);
    cudaFree(device_found_b);
    free_buffer(&prefix_a);
    free_buffer(&prefix_b);
    free_buffer(&suffix);
    return status;
}
