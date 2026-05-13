#ifndef NEWOS_CRYPTO_P384_H
#define NEWOS_CRYPTO_P384_H

#include <stddef.h>

#define CRYPTO_P384_SCALAR_SIZE 48
#define CRYPTO_P384_COORD_SIZE 48
#define CRYPTO_P384_UNCOMPRESSED_PUBLIC_KEY_SIZE 97
#define CRYPTO_P384_ECDSA_SIGNATURE_SIZE 96

int crypto_p384_ecdsa_sha384_verify(
    const unsigned char public_key[CRYPTO_P384_UNCOMPRESSED_PUBLIC_KEY_SIZE],
    const unsigned char digest[48],
    const unsigned char signature[CRYPTO_P384_ECDSA_SIGNATURE_SIZE]
);

#endif
