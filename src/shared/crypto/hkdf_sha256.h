#ifndef NEWOS_CRYPTO_HKDF_SHA256_H
#define NEWOS_CRYPTO_HKDF_SHA256_H

#include <stddef.h>

#include "crypto/sha256.h"

int crypto_hkdf_sha256_extract(
    unsigned char prk[CRYPTO_SHA256_DIGEST_SIZE],
    const unsigned char *salt,
    size_t salt_len,
    const unsigned char *ikm,
    size_t ikm_len
);

int crypto_hkdf_sha256_expand(
    unsigned char *out,
    size_t out_len,
    const unsigned char prk[CRYPTO_SHA256_DIGEST_SIZE],
    const unsigned char *info,
    size_t info_len
);

#endif
