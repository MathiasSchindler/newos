#ifndef NEWOS_CRYPTO_MD5_H
#define NEWOS_CRYPTO_MD5_H

#include <stddef.h>

#define CRYPTO_MD5_DIGEST_SIZE 16
#define CRYPTO_MD5_BLOCK_SIZE 64

typedef struct {
    unsigned int state[4];
    unsigned long long bit_count;
    unsigned char buffer[CRYPTO_MD5_BLOCK_SIZE];
    size_t buffer_len;
} CryptoMd5Context;

void crypto_md5_init(CryptoMd5Context *ctx);
void crypto_md5_update(CryptoMd5Context *ctx, const unsigned char *data, size_t len);
void crypto_md5_final(CryptoMd5Context *ctx, unsigned char out[CRYPTO_MD5_DIGEST_SIZE]);
void crypto_md5_hash(const unsigned char *data, size_t len, unsigned char out[CRYPTO_MD5_DIGEST_SIZE]);

#endif
