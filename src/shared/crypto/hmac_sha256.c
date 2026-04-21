#include "crypto/hmac_sha256.h"
#include "crypto/crypto_util.h"

static void hmac_sha256_prepare_key_block(
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

void crypto_hmac_sha256(
    unsigned char out[CRYPTO_SHA256_DIGEST_SIZE],
    const unsigned char *key,
    size_t key_len,
    const unsigned char *data,
    size_t data_len
) {
    CryptoSha256Context ctx;
    unsigned char key_block[CRYPTO_SHA256_BLOCK_SIZE];
    unsigned char inner[CRYPTO_SHA256_DIGEST_SIZE];
    size_t i;

    if (out == 0 || (key == 0 && key_len != 0U) || (data == 0 && data_len != 0U)) {
        return;
    }

    hmac_sha256_prepare_key_block(key_block, key, key_len);

    for (i = 0; i < CRYPTO_SHA256_BLOCK_SIZE; ++i) {
        key_block[i] ^= 0x36U;
    }

    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, key_block, sizeof(key_block));
    if (data_len != 0U) {
        crypto_sha256_update(&ctx, data, data_len);
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
}
