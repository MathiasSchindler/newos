#ifndef NEWOS_CRYPTO_SSH_KDF_H
#define NEWOS_CRYPTO_SSH_KDF_H

#include <stddef.h>

int crypto_ssh_kdf_derive_sha256(
    const unsigned char *shared_secret,
    size_t shared_secret_len,
    const unsigned char *exchange_hash,
    size_t exchange_hash_len,
    char label,
    const unsigned char *session_id,
    size_t session_id_len,
    unsigned char *out,
    size_t out_len
);

#endif
