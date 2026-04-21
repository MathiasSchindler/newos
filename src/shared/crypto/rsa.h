#ifndef NEWOS_CRYPTO_RSA_H
#define NEWOS_CRYPTO_RSA_H

#include <stddef.h>

#define CRYPTO_RSA2048_MODULUS_SIZE 256

typedef struct {
    unsigned char p[128];
    unsigned char q[128];
    unsigned char dp[128];
    unsigned char dq[128];
    unsigned char qinv[128];
    size_t p_len;
    size_t q_len;
    size_t dp_len;
    size_t dq_len;
    size_t qinv_len;
    size_t modulus_len;
} CryptoRsaPrivateKey;

int crypto_rsa2048_parse_private_key_der(
    CryptoRsaPrivateKey *key,
    const unsigned char *der,
    size_t der_len
);

int crypto_rsa2048_pss_sha256_sign(
    unsigned char *signature,
    size_t signature_cap,
    size_t *signature_len,
    const unsigned char *message,
    size_t message_len,
    const CryptoRsaPrivateKey *key
);

#endif
