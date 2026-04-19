#include "crypto/sha256.h"
#include "runtime.h"

static unsigned int crypto_rotr32(unsigned int value, unsigned int count) {
    return (value >> count) | (value << (32U - count));
}

static const unsigned int g_sha256_k[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void crypto_sha256_transform(CryptoSha256Context *ctx, const unsigned char block[64]) {
    unsigned int w[64];
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;
    unsigned int e;
    unsigned int f;
    unsigned int g;
    unsigned int h;
    unsigned int i;

    for (i = 0; i < 16U; ++i) {
        size_t offset = (size_t)i * 4U;
        w[i] = ((unsigned int)block[offset] << 24) |
               ((unsigned int)block[offset + 1U] << 16) |
               ((unsigned int)block[offset + 2U] << 8) |
               (unsigned int)block[offset + 3U];
    }

    for (i = 16U; i < 64U; ++i) {
        unsigned int s0 = crypto_rotr32(w[i - 15U], 7U) ^ crypto_rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3);
        unsigned int s1 = crypto_rotr32(w[i - 2U], 17U) ^ crypto_rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10);
        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64U; ++i) {
        unsigned int s1 = crypto_rotr32(e, 6U) ^ crypto_rotr32(e, 11U) ^ crypto_rotr32(e, 25U);
        unsigned int ch = (e & f) ^ ((~e) & g);
        unsigned int temp1 = h + s1 + ch + g_sha256_k[i] + w[i];
        unsigned int s0 = crypto_rotr32(a, 2U) ^ crypto_rotr32(a, 13U) ^ crypto_rotr32(a, 22U);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void crypto_sha256_init(CryptoSha256Context *ctx) {
    if (ctx == 0) {
        return;
    }

    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
    ctx->bit_count = 0ULL;
    ctx->buffer_len = 0U;
}

void crypto_sha256_update(CryptoSha256Context *ctx, const unsigned char *data, size_t len) {
    size_t i = 0;

    if (ctx == 0 || (data == 0 && len != 0U)) {
        return;
    }

    ctx->bit_count += (unsigned long long)len * 8ULL;

    while (i < len) {
        size_t space = CRYPTO_SHA256_BLOCK_SIZE - ctx->buffer_len;
        size_t chunk = (len - i < space) ? (len - i) : space;

        memcpy(ctx->buffer + ctx->buffer_len, data + i, chunk);
        ctx->buffer_len += chunk;
        i += chunk;

        if (ctx->buffer_len == CRYPTO_SHA256_BLOCK_SIZE) {
            crypto_sha256_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0U;
        }
    }
}

void crypto_sha256_final(CryptoSha256Context *ctx, unsigned char out[CRYPTO_SHA256_DIGEST_SIZE]) {
    unsigned char pad = 0x80U;
    unsigned char zero = 0U;
    unsigned char length_bytes[8];
    unsigned int i;

    if (ctx == 0 || out == 0) {
        return;
    }

    for (i = 0; i < 8U; ++i) {
        length_bytes[7U - i] = (unsigned char)((ctx->bit_count >> (8U * i)) & 0xffU);
    }

    crypto_sha256_update(ctx, &pad, 1U);
    while (ctx->buffer_len != 56U) {
        crypto_sha256_update(ctx, &zero, 1U);
    }
    crypto_sha256_update(ctx, length_bytes, 8U);

    for (i = 0; i < 8U; ++i) {
        out[i * 4U] = (unsigned char)((ctx->state[i] >> 24) & 0xffU);
        out[i * 4U + 1U] = (unsigned char)((ctx->state[i] >> 16) & 0xffU);
        out[i * 4U + 2U] = (unsigned char)((ctx->state[i] >> 8) & 0xffU);
        out[i * 4U + 3U] = (unsigned char)(ctx->state[i] & 0xffU);
    }
}

void crypto_sha256_hash(const unsigned char *data, size_t len, unsigned char out[CRYPTO_SHA256_DIGEST_SIZE]) {
    CryptoSha256Context ctx;

    crypto_sha256_init(&ctx);
    crypto_sha256_update(&ctx, data, len);
    crypto_sha256_final(&ctx, out);
}
