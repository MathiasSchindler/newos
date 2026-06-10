#include "crypto/sha1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAYLOAD_SIZE 32U
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
#define EMPTY_NONCE UINT32_MAX

typedef struct {
    unsigned char *data;
    size_t size;
} Buffer;

typedef struct {
    uint64_t *keys;
    uint32_t *nonces;
    size_t capacity;
} HashTable;

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
        fprintf(stderr, "toy_sha1_backend: out of memory reading %s\n", path);
        fclose(file);
        return 1;
    }
    if (buffer->size != 0U && fread(buffer->data, 1U, buffer->size, file) != buffer->size) {
        fprintf(stderr, "toy_sha1_backend: cannot read %s\n", path);
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
        fprintf(stderr, "toy_sha1_backend: cannot write %s\n", path);
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
        fprintf(stderr, "toy_sha1_backend: missing %s\n", name);
        return 0;
    }
    return value;
}

static void write_u32_le(unsigned char *data, uint32_t value) {
    data[0] = (unsigned char)(value & 0xffU);
    data[1] = (unsigned char)((value >> 8U) & 0xffU);
    data[2] = (unsigned char)((value >> 16U) & 0xffU);
    data[3] = (unsigned char)((value >> 24U) & 0xffU);
}

static void build_payload(unsigned char payload[PAYLOAD_SIZE], char side, uint32_t nonce) {
    memset(payload, 0, PAYLOAD_SIZE);
    memcpy(payload, "newos-sha1-24-", 14U);
    payload[14] = (unsigned char)side;
    write_u32_le(payload + 24U, nonce);
    write_u32_le(payload + 28U, ~nonce);
}

static uint64_t sha1_truncated_key(const CryptoSha1Context *prefix_context,
                                   const unsigned char payload[PAYLOAD_SIZE],
                                   const Buffer *suffix,
                                   unsigned int bits) {
    CryptoSha1Context context = *prefix_context;
    unsigned char digest[CRYPTO_SHA1_DIGEST_SIZE];
    uint64_t key;
    unsigned int index;

    crypto_sha1_update(&context, payload, PAYLOAD_SIZE);
    crypto_sha1_update(&context, suffix->data, suffix->size);
    crypto_sha1_final(&context, digest);
    key = 0U;
    for (index = 0U; index < 8U; ++index) {
        key = (key << 8U) | digest[index];
    }
    if (bits < 64U) {
        key &= ~((1ULL << (64U - bits)) - 1ULL);
    }
    return key;
}

static size_t table_slot(uint64_t key, size_t table_capacity) {
    key ^= key >> 33U;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33U;
    return (size_t)key & (table_capacity - 1U);
}

static int table_init(HashTable *table, size_t capacity) {
    size_t index;

    table->capacity = capacity;
    table->keys = (uint64_t *)malloc(capacity * sizeof(table->keys[0]));
    table->nonces = (uint32_t *)malloc(capacity * sizeof(table->nonces[0]));
    if (!table->keys || !table->nonces) {
        free(table->keys);
        free(table->nonces);
        table->keys = 0;
        table->nonces = 0;
        table->capacity = 0U;
        return 1;
    }
    for (index = 0U; index < capacity; ++index) {
        table->nonces[index] = EMPTY_NONCE;
    }
    return 0;
}

static void table_free(HashTable *table) {
    free(table->keys);
    free(table->nonces);
    table->keys = 0;
    table->nonces = 0;
    table->capacity = 0U;
}

static void table_insert(HashTable *table, uint64_t key, uint32_t nonce) {
    size_t table_capacity = table->capacity;
    size_t slot = table_slot(key, table_capacity);

    while (table->nonces[slot] != EMPTY_NONCE) {
        if (table->keys[slot] == key) {
            return;
        }
        slot = (slot + 1U) & (table_capacity - 1U);
    }
    table->keys[slot] = key;
    table->nonces[slot] = nonce;
}

static int table_find(const HashTable *table, uint64_t key, uint32_t *nonce_out) {
    size_t table_capacity = table->capacity;
    size_t slot = table_slot(key, table_capacity);

    while (table->nonces[slot] != EMPTY_NONCE) {
        if (table->keys[slot] == key) {
            *nonce_out = table->nonces[slot];
            return 1;
        }
        slot = (slot + 1U) & (table_capacity - 1U);
    }
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
    fprintf(stderr, "toy_sha1_backend: expected sha1-24/32/40/48/56/64, got %s\n", hash_name);
    return 1;
}

static int parse_u32_env(const char *name, uint32_t *value) {
    const char *text = getenv(name);
    char *end = 0;
    unsigned long parsed;

    if (!text || text[0] == '\0') {
        return 0;
    }
    parsed = strtoul(text, &end, 10);
    if (!end || *end != '\0' || parsed > UINT32_MAX) {
        fprintf(stderr, "toy_sha1_backend: invalid %s=%s\n", name, text);
        return 1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int parse_size_env(const char *name, size_t *value) {
    const char *text = getenv(name);
    char *end = 0;
    unsigned long long parsed;

    if (!text || text[0] == '\0') {
        return 0;
    }
    parsed = strtoull(text, &end, 10);
    if (!end || *end != '\0' || parsed > (unsigned long long)(size_t)-1) {
        fprintf(stderr, "toy_sha1_backend: invalid %s=%s\n", name, text);
        return 1;
    }
    *value = (size_t)parsed;
    return 0;
}

static int is_power_of_two_size(size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
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
    CryptoSha1Context prefix_a_context;
    CryptoSha1Context prefix_b_context;
    HashTable table = {0};
    unsigned char payload_a[PAYLOAD_SIZE];
    unsigned char payload_b[PAYLOAD_SIZE];
    uint32_t nonce;
    uint32_t a_candidates;
    uint32_t b_limit;
    size_t table_capacity;
    unsigned int bits;
    int status = 1;

    if (!hash_name || !prefix_a_path || !prefix_b_path || !suffix_path || !block_a_path || !block_b_path) {
        return 1;
    }
    if (configure_profile(hash_name, &bits, &a_candidates, &b_limit, &table_capacity) != 0) {
        return 1;
    }
    if (parse_u32_env("NEWOS_TOY_A_CANDIDATES", &a_candidates) != 0 ||
        parse_u32_env("NEWOS_TOY_B_LIMIT", &b_limit) != 0 ||
        parse_size_env("NEWOS_TOY_TABLE_CAPACITY", &table_capacity) != 0) {
        return 1;
    }
    if (!is_power_of_two_size(table_capacity) || table_capacity <= (size_t)a_candidates * 2U) {
        fprintf(stderr, "toy_sha1_backend: table capacity must be a power of two and more than twice A candidates\n");
        return 1;
    }
    if (read_file(prefix_a_path, &prefix_a) != 0 ||
        read_file(prefix_b_path, &prefix_b) != 0 ||
        read_file(suffix_path, &suffix) != 0) {
        free_buffer(&prefix_a);
        free_buffer(&prefix_b);
        free_buffer(&suffix);
        return 1;
    }
    fprintf(stderr, "toy_sha1_backend: bits=%u A=%u B=%u table=%llu\n",
            bits, a_candidates, b_limit, (unsigned long long)table_capacity);
    if (table_init(&table, table_capacity) != 0) {
        fprintf(stderr, "toy_sha1_backend: out of memory allocating table with %llu slots\n",
                (unsigned long long)table_capacity);
        goto cleanup;
    }

    crypto_sha1_init(&prefix_a_context);
    crypto_sha1_update(&prefix_a_context, prefix_a.data, prefix_a.size);
    crypto_sha1_init(&prefix_b_context);
    crypto_sha1_update(&prefix_b_context, prefix_b.data, prefix_b.size);

    for (nonce = 0U; nonce < a_candidates; ++nonce) {
        uint64_t key;

        build_payload(payload_a, 'A', nonce);
        key = sha1_truncated_key(&prefix_a_context, payload_a, &suffix, bits);
        table_insert(&table, key, nonce);
    }
    for (nonce = 0U; nonce < b_limit; ++nonce) {
        uint64_t key;
        uint32_t match_nonce;

        build_payload(payload_b, 'B', nonce);
        key = sha1_truncated_key(&prefix_b_context, payload_b, &suffix, bits);
        if (table_find(&table, key, &match_nonce)) {
            build_payload(payload_a, 'A', match_nonce);
            if (write_file(block_a_path, payload_a, PAYLOAD_SIZE) == 0 &&
                write_file(block_b_path, payload_b, PAYLOAD_SIZE) == 0) {
                fprintf(stderr, "toy_sha1_backend: matched %u-bit SHA-1 prefix after A=%u B=%u\n",
                        bits, match_nonce, nonce);
                status = 0;
            }
            break;
        }
    }
    if (status != 0) {
        fprintf(stderr, "toy_sha1_backend: no collision found within search limit\n");
    }

cleanup:
    table_free(&table);
    free_buffer(&prefix_a);
    free_buffer(&prefix_b);
    free_buffer(&suffix);
    return status;
}
