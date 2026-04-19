#ifndef NEWOS_CRYPTO_ED25519_H
#define NEWOS_CRYPTO_ED25519_H

#include <stddef.h>

int crypto_ed25519_public_key_from_seed(unsigned char public_key[32], const unsigned char seed[32]);
int crypto_ed25519_sign(
    unsigned char signature[64],
    const unsigned char *message,
    size_t message_len,
    const unsigned char seed[32],
    const unsigned char public_key[32]
);
int crypto_ed25519_verify(
    const unsigned char signature[64],
    const unsigned char *message,
    size_t message_len,
    const unsigned char public_key[32]
);

#endif
