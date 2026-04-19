#include "crypto/sha512.h"
#include "runtime.h"

static unsigned long long crypto_rotr64(unsigned long long value, unsigned int count) {
    return (value >> count) | (value << (64U - count));
}

static const unsigned long long g_sha512_k[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static void crypto_sha512_transform(CryptoSha512Context *ctx, const unsigned char block[128]) {
    unsigned long long w[80];
    unsigned long long a;
    unsigned long long b;
    unsigned long long c;
    unsigned long long d;
    unsigned long long e;
    unsigned long long f;
    unsigned long long g;
    unsigned long long h;
    unsigned int i;

    for (i = 0; i < 16U; ++i) {
        size_t offset = (size_t)i * 8U;
        w[i] = ((unsigned long long)block[offset] << 56) |
               ((unsigned long long)block[offset + 1U] << 48) |
               ((unsigned long long)block[offset + 2U] << 40) |
               ((unsigned long long)block[offset + 3U] << 32) |
               ((unsigned long long)block[offset + 4U] << 24) |
               ((unsigned long long)block[offset + 5U] << 16) |
               ((unsigned long long)block[offset + 6U] << 8) |
               (unsigned long long)block[offset + 7U];
    }

    for (i = 16U; i < 80U; ++i) {
        unsigned long long s0 = crypto_rotr64(w[i - 15U], 1U) ^ crypto_rotr64(w[i - 15U], 8U) ^ (w[i - 15U] >> 7);
        unsigned long long s1 = crypto_rotr64(w[i - 2U], 19U) ^ crypto_rotr64(w[i - 2U], 61U) ^ (w[i - 2U] >> 6);
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

    for (i = 0; i < 80U; ++i) {
        unsigned long long s1 = crypto_rotr64(e, 14U) ^ crypto_rotr64(e, 18U) ^ crypto_rotr64(e, 41U);
        unsigned long long ch = (e & f) ^ ((~e) & g);
        unsigned long long temp1 = h + s1 + ch + g_sha512_k[i] + w[i];
        unsigned long long s0 = crypto_rotr64(a, 28U) ^ crypto_rotr64(a, 34U) ^ crypto_rotr64(a, 39U);
        unsigned long long maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned long long temp2 = s0 + maj;

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

static void crypto_sha512_add_bits(CryptoSha512Context *ctx, unsigned long long bits) {
    unsigned long long old_low = ctx->bit_count_low;

    ctx->bit_count_low += bits;
    if (ctx->bit_count_low < old_low) {
        ctx->bit_count_high += 1ULL;
    }
}

void crypto_sha512_init(CryptoSha512Context *ctx) {
    if (ctx == 0) {
        return;
    }

    ctx->state[0] = 0x6a09e667f3bcc908ULL;
    ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL;
    ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL;
    ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL;
    ctx->state[7] = 0x5be0cd19137e2179ULL;
    ctx->bit_count_low = 0ULL;
    ctx->bit_count_high = 0ULL;
    ctx->buffer_len = 0U;
}

void crypto_sha512_update(CryptoSha512Context *ctx, const unsigned char *data, size_t len) {
    size_t i = 0;

    if (ctx == 0 || (data == 0 && len != 0U)) {
        return;
    }

    crypto_sha512_add_bits(ctx, (unsigned long long)len * 8ULL);

    while (i < len) {
        size_t space = CRYPTO_SHA512_BLOCK_SIZE - ctx->buffer_len;
        size_t chunk = (len - i < space) ? (len - i) : space;

        memcpy(ctx->buffer + ctx->buffer_len, data + i, chunk);
        ctx->buffer_len += chunk;
        i += chunk;

        if (ctx->buffer_len == CRYPTO_SHA512_BLOCK_SIZE) {
            crypto_sha512_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0U;
        }
    }
}

void crypto_sha512_final(CryptoSha512Context *ctx, unsigned char out[CRYPTO_SHA512_DIGEST_SIZE]) {
    unsigned char pad = 0x80U;
    unsigned char zero = 0U;
    unsigned char length_bytes[16];
    unsigned int i;

    if (ctx == 0 || out == 0) {
        return;
    }

    for (i = 0; i < 8U; ++i) {
        length_bytes[i] = (unsigned char)((ctx->bit_count_high >> (56U - 8U * i)) & 0xffU);
        length_bytes[8U + i] = (unsigned char)((ctx->bit_count_low >> (56U - 8U * i)) & 0xffU);
    }

    crypto_sha512_update(ctx, &pad, 1U);
    while (ctx->buffer_len != 112U) {
        crypto_sha512_update(ctx, &zero, 1U);
    }
    crypto_sha512_update(ctx, length_bytes, 16U);

    for (i = 0; i < 8U; ++i) {
        out[i * 8U] = (unsigned char)((ctx->state[i] >> 56) & 0xffU);
        out[i * 8U + 1U] = (unsigned char)((ctx->state[i] >> 48) & 0xffU);
        out[i * 8U + 2U] = (unsigned char)((ctx->state[i] >> 40) & 0xffU);
        out[i * 8U + 3U] = (unsigned char)((ctx->state[i] >> 32) & 0xffU);
        out[i * 8U + 4U] = (unsigned char)((ctx->state[i] >> 24) & 0xffU);
        out[i * 8U + 5U] = (unsigned char)((ctx->state[i] >> 16) & 0xffU);
        out[i * 8U + 6U] = (unsigned char)((ctx->state[i] >> 8) & 0xffU);
        out[i * 8U + 7U] = (unsigned char)(ctx->state[i] & 0xffU);
    }
}

void crypto_sha512_hash(const unsigned char *data, size_t len, unsigned char out[CRYPTO_SHA512_DIGEST_SIZE]) {
    CryptoSha512Context ctx;

    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, data, len);
    crypto_sha512_final(&ctx, out);
}
