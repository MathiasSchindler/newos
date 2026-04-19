#include "crypto/md5.h"
#include "runtime.h"

static unsigned int crypto_rotl32(unsigned int value, unsigned int count) {
    return (value << count) | (value >> (32U - count));
}

static const unsigned int g_md5_k[64] = {
    0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU,
    0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
    0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
    0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
    0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU,
    0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
    0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
    0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
    0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
    0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
    0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U,
    0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
    0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U,
    0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
    0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
    0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U
};

static const unsigned int g_md5_s[64] = {
    7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U,
    5U, 9U, 14U, 20U, 5U, 9U, 14U, 20U, 5U, 9U, 14U, 20U, 5U, 9U, 14U, 20U,
    4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U,
    6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U
};

static void crypto_md5_transform(CryptoMd5Context *ctx, const unsigned char block[64]) {
    unsigned int a = ctx->state[0];
    unsigned int b = ctx->state[1];
    unsigned int c = ctx->state[2];
    unsigned int d = ctx->state[3];
    unsigned int words[16];
    unsigned int i;

    for (i = 0; i < 16U; ++i) {
        size_t offset = (size_t)i * 4U;
        words[i] = (unsigned int)block[offset] |
                   ((unsigned int)block[offset + 1U] << 8) |
                   ((unsigned int)block[offset + 2U] << 16) |
                   ((unsigned int)block[offset + 3U] << 24);
    }

    for (i = 0; i < 64U; ++i) {
        unsigned int f;
        unsigned int g;
        unsigned int temp;

        if (i < 16U) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32U) {
            f = (d & b) | ((~d) & c);
            g = (5U * i + 1U) & 15U;
        } else if (i < 48U) {
            f = b ^ c ^ d;
            g = (3U * i + 5U) & 15U;
        } else {
            f = c ^ (b | (~d));
            g = (7U * i) & 15U;
        }

        temp = d;
        d = c;
        c = b;
        b = b + crypto_rotl32(a + f + g_md5_k[i] + words[g], g_md5_s[i]);
        a = temp;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

void crypto_md5_init(CryptoMd5Context *ctx) {
    if (ctx == 0) {
        return;
    }

    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xefcdab89U;
    ctx->state[2] = 0x98badcfeU;
    ctx->state[3] = 0x10325476U;
    ctx->bit_count = 0ULL;
    ctx->buffer_len = 0U;
}

void crypto_md5_update(CryptoMd5Context *ctx, const unsigned char *data, size_t len) {
    size_t i = 0;

    if (ctx == 0 || (data == 0 && len != 0U)) {
        return;
    }

    ctx->bit_count += (unsigned long long)len * 8ULL;

    while (i < len) {
        size_t space = CRYPTO_MD5_BLOCK_SIZE - ctx->buffer_len;
        size_t chunk = (len - i < space) ? (len - i) : space;

        memcpy(ctx->buffer + ctx->buffer_len, data + i, chunk);
        ctx->buffer_len += chunk;
        i += chunk;

        if (ctx->buffer_len == CRYPTO_MD5_BLOCK_SIZE) {
            crypto_md5_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0U;
        }
    }
}

void crypto_md5_final(CryptoMd5Context *ctx, unsigned char out[CRYPTO_MD5_DIGEST_SIZE]) {
    unsigned char pad = 0x80U;
    unsigned char zero = 0U;
    unsigned char length_bytes[8];
    unsigned int i;

    if (ctx == 0 || out == 0) {
        return;
    }

    for (i = 0; i < 8U; ++i) {
        length_bytes[i] = (unsigned char)((ctx->bit_count >> (8U * i)) & 0xffU);
    }

    crypto_md5_update(ctx, &pad, 1U);
    while (ctx->buffer_len != 56U) {
        crypto_md5_update(ctx, &zero, 1U);
    }
    crypto_md5_update(ctx, length_bytes, 8U);

    for (i = 0; i < 4U; ++i) {
        out[i * 4U] = (unsigned char)(ctx->state[i] & 0xffU);
        out[i * 4U + 1U] = (unsigned char)((ctx->state[i] >> 8) & 0xffU);
        out[i * 4U + 2U] = (unsigned char)((ctx->state[i] >> 16) & 0xffU);
        out[i * 4U + 3U] = (unsigned char)((ctx->state[i] >> 24) & 0xffU);
    }
}

void crypto_md5_hash(const unsigned char *data, size_t len, unsigned char out[CRYPTO_MD5_DIGEST_SIZE]) {
    CryptoMd5Context ctx;

    crypto_md5_init(&ctx);
    crypto_md5_update(&ctx, data, len);
    crypto_md5_final(&ctx, out);
}
