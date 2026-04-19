#ifndef NEWOS_CRYPTO_CURVE25519_H
#define NEWOS_CRYPTO_CURVE25519_H

int crypto_x25519_scalarmult(
    unsigned char out[32],
    const unsigned char scalar[32],
    const unsigned char point[32]
);
int crypto_x25519_scalarmult_base(unsigned char out[32], const unsigned char scalar[32]);

#endif
