#ifndef NEWOS_CRYPTO_SHA512_H
#define NEWOS_CRYPTO_SHA512_H

#include <stddef.h>

#define CRYPTO_SHA512_DIGEST_SIZE 64
#define CRYPTO_SHA512_BLOCK_SIZE 128

typedef struct {
    unsigned long long state[8];
    unsigned long long bit_count_low;
    unsigned long long bit_count_high;
    unsigned char buffer[CRYPTO_SHA512_BLOCK_SIZE];
    size_t buffer_len;
} CryptoSha512Context;

void crypto_sha512_init(CryptoSha512Context *ctx);
void crypto_sha512_update(CryptoSha512Context *ctx, const unsigned char *data, size_t len);
void crypto_sha512_final(CryptoSha512Context *ctx, unsigned char out[CRYPTO_SHA512_DIGEST_SIZE]);
void crypto_sha512_hash(const unsigned char *data, size_t len, unsigned char out[CRYPTO_SHA512_DIGEST_SIZE]);

#endif
