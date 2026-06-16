#include "crypto/md5.h"
#include "runtime.h"

#define MD5_ROTL32(value, count) (((value) << (count)) | ((value) >> (32U - (count))))
#define MD5_F(x, y, z) (((x) & (y)) | ((~(x)) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & (~(z))))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | (~(z))))
#define MD5_STEP(func, a, b, c, d, word, constant, shift) \
    do { \
        (a) += MD5_##func((b), (c), (d)) + (word) + (constant); \
        (a) = (b) + MD5_ROTL32((a), (shift)); \
    } while (0)

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

    MD5_STEP(F, a, b, c, d, words[0], 0xd76aa478U, 7U);
    MD5_STEP(F, d, a, b, c, words[1], 0xe8c7b756U, 12U);
    MD5_STEP(F, c, d, a, b, words[2], 0x242070dbU, 17U);
    MD5_STEP(F, b, c, d, a, words[3], 0xc1bdceeeU, 22U);
    MD5_STEP(F, a, b, c, d, words[4], 0xf57c0fafU, 7U);
    MD5_STEP(F, d, a, b, c, words[5], 0x4787c62aU, 12U);
    MD5_STEP(F, c, d, a, b, words[6], 0xa8304613U, 17U);
    MD5_STEP(F, b, c, d, a, words[7], 0xfd469501U, 22U);
    MD5_STEP(F, a, b, c, d, words[8], 0x698098d8U, 7U);
    MD5_STEP(F, d, a, b, c, words[9], 0x8b44f7afU, 12U);
    MD5_STEP(F, c, d, a, b, words[10], 0xffff5bb1U, 17U);
    MD5_STEP(F, b, c, d, a, words[11], 0x895cd7beU, 22U);
    MD5_STEP(F, a, b, c, d, words[12], 0x6b901122U, 7U);
    MD5_STEP(F, d, a, b, c, words[13], 0xfd987193U, 12U);
    MD5_STEP(F, c, d, a, b, words[14], 0xa679438eU, 17U);
    MD5_STEP(F, b, c, d, a, words[15], 0x49b40821U, 22U);

    MD5_STEP(G, a, b, c, d, words[1], 0xf61e2562U, 5U);
    MD5_STEP(G, d, a, b, c, words[6], 0xc040b340U, 9U);
    MD5_STEP(G, c, d, a, b, words[11], 0x265e5a51U, 14U);
    MD5_STEP(G, b, c, d, a, words[0], 0xe9b6c7aaU, 20U);
    MD5_STEP(G, a, b, c, d, words[5], 0xd62f105dU, 5U);
    MD5_STEP(G, d, a, b, c, words[10], 0x02441453U, 9U);
    MD5_STEP(G, c, d, a, b, words[15], 0xd8a1e681U, 14U);
    MD5_STEP(G, b, c, d, a, words[4], 0xe7d3fbc8U, 20U);
    MD5_STEP(G, a, b, c, d, words[9], 0x21e1cde6U, 5U);
    MD5_STEP(G, d, a, b, c, words[14], 0xc33707d6U, 9U);
    MD5_STEP(G, c, d, a, b, words[3], 0xf4d50d87U, 14U);
    MD5_STEP(G, b, c, d, a, words[8], 0x455a14edU, 20U);
    MD5_STEP(G, a, b, c, d, words[13], 0xa9e3e905U, 5U);
    MD5_STEP(G, d, a, b, c, words[2], 0xfcefa3f8U, 9U);
    MD5_STEP(G, c, d, a, b, words[7], 0x676f02d9U, 14U);
    MD5_STEP(G, b, c, d, a, words[12], 0x8d2a4c8aU, 20U);

    MD5_STEP(H, a, b, c, d, words[5], 0xfffa3942U, 4U);
    MD5_STEP(H, d, a, b, c, words[8], 0x8771f681U, 11U);
    MD5_STEP(H, c, d, a, b, words[11], 0x6d9d6122U, 16U);
    MD5_STEP(H, b, c, d, a, words[14], 0xfde5380cU, 23U);
    MD5_STEP(H, a, b, c, d, words[1], 0xa4beea44U, 4U);
    MD5_STEP(H, d, a, b, c, words[4], 0x4bdecfa9U, 11U);
    MD5_STEP(H, c, d, a, b, words[7], 0xf6bb4b60U, 16U);
    MD5_STEP(H, b, c, d, a, words[10], 0xbebfbc70U, 23U);
    MD5_STEP(H, a, b, c, d, words[13], 0x289b7ec6U, 4U);
    MD5_STEP(H, d, a, b, c, words[0], 0xeaa127faU, 11U);
    MD5_STEP(H, c, d, a, b, words[3], 0xd4ef3085U, 16U);
    MD5_STEP(H, b, c, d, a, words[6], 0x04881d05U, 23U);
    MD5_STEP(H, a, b, c, d, words[9], 0xd9d4d039U, 4U);
    MD5_STEP(H, d, a, b, c, words[12], 0xe6db99e5U, 11U);
    MD5_STEP(H, c, d, a, b, words[15], 0x1fa27cf8U, 16U);
    MD5_STEP(H, b, c, d, a, words[2], 0xc4ac5665U, 23U);

    MD5_STEP(I, a, b, c, d, words[0], 0xf4292244U, 6U);
    MD5_STEP(I, d, a, b, c, words[7], 0x432aff97U, 10U);
    MD5_STEP(I, c, d, a, b, words[14], 0xab9423a7U, 15U);
    MD5_STEP(I, b, c, d, a, words[5], 0xfc93a039U, 21U);
    MD5_STEP(I, a, b, c, d, words[12], 0x655b59c3U, 6U);
    MD5_STEP(I, d, a, b, c, words[3], 0x8f0ccc92U, 10U);
    MD5_STEP(I, c, d, a, b, words[10], 0xffeff47dU, 15U);
    MD5_STEP(I, b, c, d, a, words[1], 0x85845dd1U, 21U);
    MD5_STEP(I, a, b, c, d, words[8], 0x6fa87e4fU, 6U);
    MD5_STEP(I, d, a, b, c, words[15], 0xfe2ce6e0U, 10U);
    MD5_STEP(I, c, d, a, b, words[6], 0xa3014314U, 15U);
    MD5_STEP(I, b, c, d, a, words[13], 0x4e0811a1U, 21U);
    MD5_STEP(I, a, b, c, d, words[4], 0xf7537e82U, 6U);
    MD5_STEP(I, d, a, b, c, words[11], 0xbd3af235U, 10U);
    MD5_STEP(I, c, d, a, b, words[2], 0x2ad7d2bbU, 15U);
    MD5_STEP(I, b, c, d, a, words[9], 0xeb86d391U, 21U);

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

    if (ctx->buffer_len != 0U) {
        size_t space = CRYPTO_MD5_BLOCK_SIZE - ctx->buffer_len;
        size_t chunk = len < space ? len : space;

        memcpy(ctx->buffer + ctx->buffer_len, data, chunk);
        ctx->buffer_len += chunk;
        i += chunk;
        if (ctx->buffer_len != CRYPTO_MD5_BLOCK_SIZE) {
            return;
        }
        crypto_md5_transform(ctx, ctx->buffer);
        ctx->buffer_len = 0U;
    }

    while (i + CRYPTO_MD5_BLOCK_SIZE <= len) {
        crypto_md5_transform(ctx, data + i);
        i += CRYPTO_MD5_BLOCK_SIZE;
    }

    while (i < len) {
        size_t chunk = len - i;

        if (chunk > CRYPTO_MD5_BLOCK_SIZE) chunk = CRYPTO_MD5_BLOCK_SIZE;
        memcpy(ctx->buffer + ctx->buffer_len, data + i, chunk);
        ctx->buffer_len += chunk;
        i += chunk;
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
