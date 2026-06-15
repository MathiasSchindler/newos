#ifndef NEWOS_CRYPTO_RSA_H
#define NEWOS_CRYPTO_RSA_H

#include <stddef.h>

#define CRYPTO_RSA2048_MODULUS_SIZE 256
#define CRYPTO_RSA_MAX_MODULUS_SIZE 512
#define CRYPTO_RSA_HASH_SHA256 256
#define CRYPTO_RSA_HASH_SHA384 384

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

int crypto_rsa_pkcs1_v15_verify_digest(
    const unsigned char *modulus,
    size_t modulus_len,
    const unsigned char *exponent,
    size_t exponent_len,
    const unsigned char *signature,
    size_t signature_len,
    const unsigned char *digest,
    size_t digest_len,
    int hash_id
);

int crypto_rsa_pss_verify_digest(
    const unsigned char *modulus,
    size_t modulus_len,
    const unsigned char *exponent,
    size_t exponent_len,
    const unsigned char *signature,
    size_t signature_len,
    const unsigned char *digest,
    size_t digest_len,
    int hash_id
);

int crypto_rsa_pkcs1_v15_encrypt(
    unsigned char *ciphertext,
    size_t ciphertext_cap,
    size_t *ciphertext_len,
    const unsigned char *message,
    size_t message_len,
    const unsigned char *modulus,
    size_t modulus_len,
    const unsigned char *exponent,
    size_t exponent_len
);

int crypto_modexp_be(
    unsigned char *out,
    size_t out_len,
    const unsigned char *base,
    size_t base_len,
    const unsigned char *exponent,
    size_t exponent_len,
    const unsigned char *modulus,
    size_t modulus_len
);

int crypto_mul_mod_be(
    unsigned char *out,
    size_t out_len,
    const unsigned char *left,
    size_t left_len,
    const unsigned char *right,
    size_t right_len,
    const unsigned char *modulus,
    size_t modulus_len
);

#endif
