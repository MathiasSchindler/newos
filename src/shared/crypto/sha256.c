#include "crypto/sha256.h"
#include "runtime.h"

#define CRYPTO_ROTR32(value, count) (((value) >> (count)) | ((value) << (32U - (count))))

#if defined(NEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES) && NEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES
static unsigned int crypto_load_be32(const unsigned char *p) {
    return ((unsigned int)p[0] << 24) |
           ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) |
           (unsigned int)p[3];
}
#endif

#if defined(NEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES) && NEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES
static const unsigned char *crypto_sha256_initial_ptr(void) {
    return (const unsigned char *)"\x6a\x09\xe6\x67\xbb\x67\xae\x85\x3c\x6e\xf3\x72\xa5\x4f\xf5\x3a\x51\x0e\x52\x7f\x9b\x05\x68\x8c\x1f\x83\xd9\xab\x5b\xe0\xcd\x19";
}

static const unsigned char *crypto_sha256_k_bytes_ptr(void) {
    return (const unsigned char *)
        "\x42\x8a\x2f\x98\x71\x37\x44\x91\xb5\xc0\xfb\xcf\xe9\xb5\xdb\xa5"
        "\x39\x56\xc2\x5b\x59\xf1\x11\xf1\x92\x3f\x82\xa4\xab\x1c\x5e\xd5"
        "\xd8\x07\xaa\x98\x12\x83\x5b\x01\x24\x31\x85\xbe\x55\x0c\x7d\xc3"
        "\x72\xbe\x5d\x74\x80\xde\xb1\xfe\x9b\xdc\x06\xa7\xc1\x9b\xf1\x74"
        "\xe4\x9b\x69\xc1\xef\xbe\x47\x86\x0f\xc1\x9d\xc6\x24\x0c\xa1\xcc"
        "\x2d\xe9\x2c\x6f\x4a\x74\x84\xaa\x5c\xb0\xa9\xdc\x76\xf9\x88\xda"
        "\x98\x3e\x51\x52\xa8\x31\xc6\x6d\xb0\x03\x27\xc8\xbf\x59\x7f\xc7"
        "\xc6\xe0\x0b\xf3\xd5\xa7\x91\x47\x06\xca\x63\x51\x14\x29\x29\x67"
        "\x27\xb7\x0a\x85\x2e\x1b\x21\x38\x4d\x2c\x6d\xfc\x53\x38\x0d\x13"
        "\x65\x0a\x73\x54\x76\x6a\x0a\xbb\x81\xc2\xc9\x2e\x92\x72\x2c\x85"
        "\xa2\xbf\xe8\xa1\xa8\x1a\x66\x4b\xc2\x4b\x8b\x70\xc7\x6c\x51\xa3"
        "\xd1\x92\xe8\x19\xd6\x99\x06\x24\xf4\x0e\x35\x85\x10\x6a\xa0\x70"
        "\x19\xa4\xc1\x16\x1e\x37\x6c\x08\x27\x48\x77\x4c\x34\xb0\xbc\xb5"
        "\x39\x1c\x0c\xb3\x4e\xd8\xaa\x4a\x5b\x9c\xca\x4f\x68\x2e\x6f\xf3"
        "\x74\x8f\x82\xee\x78\xa5\x63\x6f\x84\xc8\x78\x14\x8c\xc7\x02\x08"
        "\x90\xbe\xff\xfa\xa4\x50\x6c\xeb\xbe\xf9\xa3\xf7\xc6\x71\x78\xf2";
}
#endif

#if !(defined(NEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES) && NEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES)
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
#endif

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
#if defined(NEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES) && NEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES
    const unsigned char *k_bytes = crypto_sha256_k_bytes_ptr();
#endif

    for (i = 0; i < 16U; ++i) {
        size_t offset = (size_t)i * 4U;
        w[i] = ((unsigned int)block[offset] << 24) |
               ((unsigned int)block[offset + 1U] << 16) |
               ((unsigned int)block[offset + 2U] << 8) |
               (unsigned int)block[offset + 3U];
    }

    for (i = 16U; i < 64U; ++i) {
        unsigned int s0 = CRYPTO_ROTR32(w[i - 15U], 7U) ^ CRYPTO_ROTR32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3);
        unsigned int s1 = CRYPTO_ROTR32(w[i - 2U], 17U) ^ CRYPTO_ROTR32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10);
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
        unsigned int s1 = CRYPTO_ROTR32(e, 6U) ^ CRYPTO_ROTR32(e, 11U) ^ CRYPTO_ROTR32(e, 25U);
        unsigned int ch = (e & f) ^ ((~e) & g);
        unsigned int k;
    #if defined(NEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES) && NEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES
        k = crypto_load_be32(k_bytes + ((size_t)i * 4U));
    #else
        k = g_sha256_k[i];
    #endif
        unsigned int temp1 = h + s1 + ch + k + w[i];
        unsigned int s0 = CRYPTO_ROTR32(a, 2U) ^ CRYPTO_ROTR32(a, 13U) ^ CRYPTO_ROTR32(a, 22U);
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

    for (unsigned int state_index = 0; state_index < 8U; ++state_index) {
#if defined(NEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES) && NEWOS_CRYPTO_SHA256_AVOID_STATIC_TABLES
        ctx->state[state_index] = crypto_load_be32(crypto_sha256_initial_ptr() + ((size_t)state_index * 4U));
#else
        static const unsigned int initial_state[8] = {
            0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
        };
        ctx->state[state_index] = initial_state[state_index];
#endif
    }
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
