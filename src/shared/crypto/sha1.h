#ifndef NEWOS_CRYPTO_SHA1_H
#define NEWOS_CRYPTO_SHA1_H

#include <stddef.h>

#define CRYPTO_SHA1_DIGEST_SIZE 20U
#define CRYPTO_SHA1_BLOCK_SIZE 64U

typedef struct sha1_ctx {
    unsigned int state[5];
    unsigned long long bit_count;
    unsigned char buffer[CRYPTO_SHA1_BLOCK_SIZE];
    size_t buffer_len;
} CryptoSha1Context;

void crypto_sha1_init(CryptoSha1Context *ctx);
void crypto_sha1_update(CryptoSha1Context *ctx, const unsigned char *data, size_t len);
void crypto_sha1_final(CryptoSha1Context *ctx, unsigned char out[CRYPTO_SHA1_DIGEST_SIZE]);
void crypto_sha1_hash(const unsigned char *data, size_t len, unsigned char out[CRYPTO_SHA1_DIGEST_SIZE]);

#endif