#include "crypto/sha1.h"
#include "runtime.h"

static unsigned int crypto_sha1_rotl32(unsigned int value, unsigned int count) {
    return (value << count) | (value >> (32U - count));
}

static unsigned int crypto_sha1_load_be32(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 24) |
           ((unsigned int)bytes[1] << 16) |
           ((unsigned int)bytes[2] << 8) |
           (unsigned int)bytes[3];
}

static void crypto_sha1_store_be32(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)(value >> 24);
    bytes[1] = (unsigned char)(value >> 16);
    bytes[2] = (unsigned char)(value >> 8);
    bytes[3] = (unsigned char)value;
}

static void crypto_sha1_transform(CryptoSha1Context *ctx, const unsigned char block[CRYPTO_SHA1_BLOCK_SIZE]) {
    unsigned int words[80];
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;
    unsigned int e;
    unsigned int round;

    for (round = 0U; round < 16U; ++round) {
        words[round] = crypto_sha1_load_be32(block + (size_t)round * 4U);
    }
    for (round = 16U; round < 80U; ++round) {
        words[round] = crypto_sha1_rotl32(words[round - 3U] ^ words[round - 8U] ^ words[round - 14U] ^ words[round - 16U], 1U);
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];

    for (round = 0U; round < 80U; ++round) {
        unsigned int f;
        unsigned int k;
        unsigned int next;

        if (round < 20U) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999U;
        } else if (round < 40U) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1U;
        } else if (round < 60U) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcU;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6U;
        }

        next = crypto_sha1_rotl32(a, 5U) + f + e + k + words[round];
        e = d;
        d = c;
        c = crypto_sha1_rotl32(b, 30U);
        b = a;
        a = next;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

void crypto_sha1_init(CryptoSha1Context *ctx) {
    if (ctx == 0) return;
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xefcdab89U;
    ctx->state[2] = 0x98badcfeU;
    ctx->state[3] = 0x10325476U;
    ctx->state[4] = 0xc3d2e1f0U;
    ctx->bit_count = 0ULL;
    ctx->buffer_len = 0U;
}

void crypto_sha1_update(CryptoSha1Context *ctx, const unsigned char *data, size_t len) {
    size_t offset = 0U;

    if (ctx == 0 || (data == 0 && len != 0U)) return;
    ctx->bit_count += (unsigned long long)len * 8ULL;

    if (ctx->buffer_len == 0U) {
        while (len - offset >= CRYPTO_SHA1_BLOCK_SIZE) {
            crypto_sha1_transform(ctx, data + offset);
            offset += CRYPTO_SHA1_BLOCK_SIZE;
        }
    }

    while (offset < len) {
        size_t space = CRYPTO_SHA1_BLOCK_SIZE - ctx->buffer_len;
        size_t chunk = (len - offset < space) ? (len - offset) : space;

        memcpy(ctx->buffer + ctx->buffer_len, data + offset, chunk);
        ctx->buffer_len += chunk;
        offset += chunk;
        if (ctx->buffer_len == CRYPTO_SHA1_BLOCK_SIZE) {
            crypto_sha1_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0U;
        }
    }
}

void crypto_sha1_final(CryptoSha1Context *ctx, unsigned char out[CRYPTO_SHA1_DIGEST_SIZE]) {
    unsigned char pad = 0x80U;
    unsigned char zero = 0U;
    unsigned char length_bytes[8];
    unsigned int index;

    if (ctx == 0 || out == 0) return;
    for (index = 0U; index < 8U; ++index) {
        length_bytes[index] = (unsigned char)(ctx->bit_count >> (56U - 8U * index));
    }

    crypto_sha1_update(ctx, &pad, 1U);
    while (ctx->buffer_len != 56U) {
        crypto_sha1_update(ctx, &zero, 1U);
    }
    crypto_sha1_update(ctx, length_bytes, sizeof(length_bytes));

    for (index = 0U; index < 5U; ++index) {
        crypto_sha1_store_be32(out + (size_t)index * 4U, ctx->state[index]);
    }
}

void crypto_sha1_hash(const unsigned char *data, size_t len, unsigned char out[CRYPTO_SHA1_DIGEST_SIZE]) {
    CryptoSha1Context ctx;
    crypto_sha1_init(&ctx);
    crypto_sha1_update(&ctx, data, len);
    crypto_sha1_final(&ctx, out);
}