#include "crypto/ssh_kdf.h"
#include "crypto/crypto_util.h"
#include "crypto/sha256.h"

#define sha256_init crypto_sha256_init
#define sha256_update crypto_sha256_update
#define sha256_final crypto_sha256_final

static void store_be32(unsigned char out[4], unsigned int value) {
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static void sha256_update_u32be(struct sha256_ctx *ctx, unsigned int value) {
    unsigned char tmp[4];
    store_be32(tmp, value);
    sha256_update(ctx, tmp, sizeof(tmp));
}

static void encode_ssh_mpint_from_bytes(
    const unsigned char *bytes,
    size_t len,
    unsigned char *out,
    unsigned int *out_len
) {
    size_t start = 0;
    size_t used = 0;
    size_t i = 0;

    while (start < len && bytes[start] == 0U) {
        start += 1U;
    }
    used = len - start;

    if (used == 0U) {
        *out_len = 0U;
        return;
    }

    if ((bytes[start] & 0x80U) != 0U) {
        out[0] = 0U;
        while (i < used) {
            out[1U + i] = bytes[start + i];
            i += 1U;
        }
        *out_len = (unsigned int)(used + 1U);
    } else {
        while (i < used) {
            out[i] = bytes[start + i];
            i += 1U;
        }
        *out_len = (unsigned int)used;
    }
}

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
) {
    struct sha256_ctx ctx;
    unsigned char digest[32];
    unsigned char mpint[128];
    unsigned int mpint_len = 0U;
    size_t generated = 0U;
    size_t i = 0U;
    int status = -1;

    if ((shared_secret == 0 && shared_secret_len != 0U) ||
        exchange_hash == 0 || exchange_hash_len == 0U ||
        session_id == 0 || session_id_len == 0U ||
        out == 0 || out_len == 0U ||
        shared_secret_len > 127U) {
        goto cleanup;
    }

    encode_ssh_mpint_from_bytes(shared_secret, shared_secret_len, mpint, &mpint_len);

    while (generated < out_len) {
        size_t take = out_len - generated;

        sha256_init(&ctx);
        sha256_update_u32be(&ctx, mpint_len);
        if (mpint_len != 0U) {
            sha256_update(&ctx, mpint, (size_t)mpint_len);
        }
        sha256_update(&ctx, exchange_hash, exchange_hash_len);

        if (generated == 0U) {
            sha256_update(&ctx, (const unsigned char *)&label, 1U);
            sha256_update(&ctx, session_id, session_id_len);
        } else {
            sha256_update(&ctx, out, generated);
        }

        sha256_final(&ctx, digest);

        if (take > sizeof(digest)) {
            take = sizeof(digest);
        }
        for (i = 0; i < take; ++i) {
            out[generated + i] = digest[i];
        }
        generated += take;
    }

    status = 0;

cleanup:
    crypto_secure_bzero(&ctx, sizeof(ctx));
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(mpint, sizeof(mpint));
    return status;
}
