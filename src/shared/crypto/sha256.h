#ifndef NEWOS_CRYPTO_SHA256_H
#define NEWOS_CRYPTO_SHA256_H

#include <stddef.h>

#define CRYPTO_SHA256_DIGEST_SIZE 32
#define CRYPTO_SHA256_BLOCK_SIZE 64

typedef struct sha256_ctx {
    unsigned int state[8];
    unsigned long long bit_count;
    unsigned char buffer[CRYPTO_SHA256_BLOCK_SIZE];
    size_t buffer_len;
} CryptoSha256Context;

void crypto_sha256_init(CryptoSha256Context *ctx);
void crypto_sha256_update(CryptoSha256Context *ctx, const unsigned char *data, size_t len);
void crypto_sha256_final(CryptoSha256Context *ctx, unsigned char out[CRYPTO_SHA256_DIGEST_SIZE]);
void crypto_sha256_hash(const unsigned char *data, size_t len, unsigned char out[CRYPTO_SHA256_DIGEST_SIZE]);

#endif
