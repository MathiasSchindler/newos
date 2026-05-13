#ifndef NEWOS_CRYPTO_P256_H
#define NEWOS_CRYPTO_P256_H

#include <stddef.h>

#define CRYPTO_P256_SCALAR_SIZE 32
#define CRYPTO_P256_COORD_SIZE 32
#define CRYPTO_P256_UNCOMPRESSED_PUBLIC_KEY_SIZE 65
#define CRYPTO_P256_ECDSA_SIGNATURE_SIZE 64

int crypto_p256_ecdsa_sha256_verify(
    const unsigned char public_key[CRYPTO_P256_UNCOMPRESSED_PUBLIC_KEY_SIZE],
    const unsigned char digest[32],
    const unsigned char signature[CRYPTO_P256_ECDSA_SIGNATURE_SIZE]
);

#endif
