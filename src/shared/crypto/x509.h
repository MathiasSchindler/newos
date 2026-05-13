#ifndef NEWOS_CRYPTO_X509_H
#define NEWOS_CRYPTO_X509_H

#include <stddef.h>

typedef struct {
    const unsigned char *data;
    size_t length;
} CryptoX509DerCert;

int crypto_x509_verify_chain(
    const CryptoX509DerCert *chain,
    size_t chain_count,
    const char *hostname,
    long long now_epoch_seconds,
    const unsigned char *trust_pem,
    size_t trust_pem_length,
    char *status,
    size_t status_size
);

int crypto_x509_verify_tls13_certificate_verify(
    const unsigned char *leaf_der,
    size_t leaf_der_length,
    unsigned short signature_scheme,
    const unsigned char *signed_content,
    size_t signed_content_length,
    const unsigned char *signature,
    size_t signature_length
);

int crypto_x509_get_rsa_public_key(
    const unsigned char *leaf_der,
    size_t leaf_der_length,
    unsigned char *modulus,
    size_t modulus_cap,
    size_t *modulus_len_out,
    unsigned char *exponent,
    size_t exponent_cap,
    size_t *exponent_len_out
);

#endif
