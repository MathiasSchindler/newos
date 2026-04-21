#include "crypto/hkdf_sha256.h"
#include "crypto/crypto_util.h"
#include "crypto/hmac_sha256.h"

static void hkdf_sha256_prepare_key_block(
    unsigned char key_block[CRYPTO_SHA256_BLOCK_SIZE],
    const unsigned char *key,
    size_t key_len
) {
    size_t i;

    for (i = 0; i < CRYPTO_SHA256_BLOCK_SIZE; ++i) {
        key_block[i] = 0U;
    }

    if (key == 0 || key_len == 0U) {
        return;
    }

    if (key_len > CRYPTO_SHA256_BLOCK_SIZE) {
        crypto_sha256_hash(key, key_len, key_block);
        return;
    }

    for (i = 0; i < key_len; ++i) {
        key_block[i] = key[i];
    }
}

static int hmac_sha256_parts(
    unsigned char out[CRYPTO_SHA256_DIGEST_SIZE],
    const unsigned char *key,
    size_t key_len,
    const unsigned char *first,
    size_t first_len,
    const unsigned char *second,
    size_t second_len,
    const unsigned char *third,
    size_t third_len
) {
    CryptoSha256Context ctx;
    unsigned char key_block[CRYPTO_SHA256_BLOCK_SIZE];
    unsigned char inner[CRYPTO_SHA256_DIGEST_SIZE];
    size_t i;

    if (out == 0 ||
        (key == 0 && key_len != 0U) ||
        (first == 0 && first_len != 0U) ||
        (second == 0 && second_len != 0U) ||
        (third == 0 && third_len != 0U)) {
        return -1;
    }

    hkdf_sha256_prepare_key_block(key_block, key, key_len);

    for (i = 0; i < CRYPTO_SHA256_BLOCK_SIZE; ++i) {
        key_block[i] ^= 0x36U;
    }

    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, key_block, sizeof(key_block));
    if (first_len != 0U) {
        crypto_sha256_update(&ctx, first, first_len);
    }
    if (second_len != 0U) {
        crypto_sha256_update(&ctx, second, second_len);
    }
    if (third_len != 0U) {
        crypto_sha256_update(&ctx, third, third_len);
    }
    crypto_sha256_final(&ctx, inner);

    for (i = 0; i < CRYPTO_SHA256_BLOCK_SIZE; ++i) {
        key_block[i] ^= (unsigned char)(0x36U ^ 0x5cU);
    }

    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, key_block, sizeof(key_block));
    crypto_sha256_update(&ctx, inner, sizeof(inner));
    crypto_sha256_final(&ctx, out);

    crypto_secure_bzero(&ctx, sizeof(ctx));
    crypto_secure_bzero(key_block, sizeof(key_block));
    crypto_secure_bzero(inner, sizeof(inner));
    return 0;
}

int crypto_hkdf_sha256_extract(
    unsigned char prk[CRYPTO_SHA256_DIGEST_SIZE],
    const unsigned char *salt,
    size_t salt_len,
    const unsigned char *ikm,
    size_t ikm_len
) {
    unsigned char zero_salt[CRYPTO_SHA256_DIGEST_SIZE];
    size_t i;

    if (prk == 0 || (salt == 0 && salt_len != 0U) || (ikm == 0 && ikm_len != 0U)) {
        return -1;
    }

    for (i = 0; i < sizeof(zero_salt); ++i) {
        zero_salt[i] = 0U;
    }

    if (salt == 0 || salt_len == 0U) {
        crypto_hmac_sha256(prk, zero_salt, sizeof(zero_salt), ikm, ikm_len);
    } else {
        crypto_hmac_sha256(prk, salt, salt_len, ikm, ikm_len);
    }

    crypto_secure_bzero(zero_salt, sizeof(zero_salt));
    return 0;
}

int crypto_hkdf_sha256_expand(
    unsigned char *out,
    size_t out_len,
    const unsigned char prk[CRYPTO_SHA256_DIGEST_SIZE],
    const unsigned char *info,
    size_t info_len
) {
    unsigned char block[CRYPTO_SHA256_DIGEST_SIZE];
    size_t generated = 0U;
    size_t previous_len = 0U;
    unsigned int block_index = 1U;

    if (out == 0 || out_len == 0U || prk == 0 || (info == 0 && info_len != 0U)) {
        return -1;
    }
    if (out_len > 255U * CRYPTO_SHA256_DIGEST_SIZE) {
        return -1;
    }

    while (generated < out_len) {
        size_t take = out_len - generated;
        unsigned char counter_byte = (unsigned char)block_index;

        if (counter_byte == 0U ||
            hmac_sha256_parts(block,
                              prk,
                              CRYPTO_SHA256_DIGEST_SIZE,
                              block,
                              previous_len,
                              info,
                              info_len,
                              &counter_byte,
                              1U) != 0) {
            crypto_secure_bzero(block, sizeof(block));
            return -1;
        }

        if (take > sizeof(block)) {
            take = sizeof(block);
        }

        for (previous_len = 0U; previous_len < take; ++previous_len) {
            out[generated + previous_len] = block[previous_len];
        }

        generated += take;
        previous_len = sizeof(block);
        block_index += 1U;
    }

    crypto_secure_bzero(block, sizeof(block));
    return 0;
}
