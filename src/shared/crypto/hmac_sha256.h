#ifndef NEWOS_CRYPTO_HMAC_SHA256_H
#define NEWOS_CRYPTO_HMAC_SHA256_H

#include <stddef.h>

#include "crypto/sha256.h"

void crypto_hmac_sha256(
    unsigned char out[CRYPTO_SHA256_DIGEST_SIZE],
    const unsigned char *key,
    size_t key_len,
    const unsigned char *data,
    size_t data_len
);

#endif
