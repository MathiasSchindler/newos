#ifndef NEWOS_CRYPTO_BRAINPOOLP256R1_H
#define NEWOS_CRYPTO_BRAINPOOLP256R1_H

#include <stddef.h>

#define CRYPTO_BRAINPOOLP256R1_SCALAR_SIZE 32U
#define CRYPTO_BRAINPOOLP256R1_COORD_SIZE 32U
#define CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE 65U

int crypto_brainpoolp256r1_public_from_private(
    const unsigned char private_key[CRYPTO_BRAINPOOLP256R1_SCALAR_SIZE],
    unsigned char public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE]
);

int crypto_brainpoolp256r1_shared_secret(
    const unsigned char private_key[CRYPTO_BRAINPOOLP256R1_SCALAR_SIZE],
    const unsigned char peer_public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE],
    unsigned char shared_x[CRYPTO_BRAINPOOLP256R1_COORD_SIZE]
);

int crypto_brainpoolp256r1_public_key_valid(
    const unsigned char public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE]
);

int crypto_brainpoolp256r1_add_generator_mul(
    const unsigned char scalar[CRYPTO_BRAINPOOLP256R1_SCALAR_SIZE],
    const unsigned char public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE],
    unsigned char result_public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE]
);

int crypto_brainpoolp256r1_scalar_mult_public(
    const unsigned char scalar[CRYPTO_BRAINPOOLP256R1_SCALAR_SIZE],
    const unsigned char public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE],
    unsigned char result_public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE]
);

#endif