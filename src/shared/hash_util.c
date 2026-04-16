#include "hash_util.h"
#include "platform.h"
#include "runtime.h"

#define HASH_STREAM_BUFFER_SIZE 4096

static unsigned int rotl32(unsigned int value, unsigned int count) {
    return (value << count) | (value >> (32U - count));
}

static unsigned int rotr32(unsigned int value, unsigned int count) {
    return (value >> count) | (value << (32U - count));
}

static unsigned long long rotr64(unsigned long long value, unsigned int count) {
    return (value >> count) | (value << (64U - count));
}

typedef struct {
    unsigned int state[4];
    unsigned long long bit_count;
    unsigned char buffer[64];
    size_t buffer_len;
} Md5Context;

static const unsigned int md5_k[64] = {
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

static const unsigned int md5_s[64] = {
    7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U,
    5U, 9U, 14U, 20U, 5U, 9U, 14U, 20U, 5U, 9U, 14U, 20U, 5U, 9U, 14U, 20U,
    4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U,
    6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U
};

static void md5_transform(Md5Context *ctx, const unsigned char block[64]) {
    unsigned int a = ctx->state[0];
    unsigned int b = ctx->state[1];
    unsigned int c = ctx->state[2];
    unsigned int d = ctx->state[3];
    unsigned int words[16];
    unsigned int i;

    for (i = 0; i < 16; ++i) {
        size_t offset = (size_t)i * 4U;
        words[i] = (unsigned int)block[offset] |
                   ((unsigned int)block[offset + 1] << 8) |
                   ((unsigned int)block[offset + 2] << 16) |
                   ((unsigned int)block[offset + 3] << 24);
    }

    for (i = 0; i < 64; ++i) {
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
        b = b + rotl32(a + f + md5_k[i] + words[g], md5_s[i]);
        a = temp;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

static void md5_init(Md5Context *ctx) {
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xefcdab89U;
    ctx->state[2] = 0x98badcfeU;
    ctx->state[3] = 0x10325476U;
    ctx->bit_count = 0ULL;
    ctx->buffer_len = 0U;
}

static void md5_update(Md5Context *ctx, const unsigned char *data, size_t len) {
    size_t i = 0;

    ctx->bit_count += (unsigned long long)len * 8ULL;

    while (i < len) {
        size_t space = 64U - ctx->buffer_len;
        size_t chunk = (len - i < space) ? (len - i) : space;

        memcpy(ctx->buffer + ctx->buffer_len, data + i, chunk);
        ctx->buffer_len += chunk;
        i += chunk;

        if (ctx->buffer_len == 64U) {
            md5_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0U;
        }
    }
}

static void md5_final(Md5Context *ctx, unsigned char out[16]) {
    unsigned char pad = 0x80U;
    unsigned char zero = 0U;
    unsigned char length_bytes[8];
    unsigned int i;

    for (i = 0; i < 8U; ++i) {
        length_bytes[i] = (unsigned char)((ctx->bit_count >> (8U * i)) & 0xffU);
    }

    md5_update(ctx, &pad, 1U);
    while (ctx->buffer_len != 56U) {
        md5_update(ctx, &zero, 1U);
    }
    md5_update(ctx, length_bytes, 8U);

    for (i = 0; i < 4U; ++i) {
        out[i * 4U] = (unsigned char)(ctx->state[i] & 0xffU);
        out[i * 4U + 1U] = (unsigned char)((ctx->state[i] >> 8) & 0xffU);
        out[i * 4U + 2U] = (unsigned char)((ctx->state[i] >> 16) & 0xffU);
        out[i * 4U + 3U] = (unsigned char)((ctx->state[i] >> 24) & 0xffU);
    }
}

typedef struct {
    unsigned int state[8];
    unsigned long long bit_count;
    unsigned char buffer[64];
    size_t buffer_len;
} Sha256Context;

static const unsigned int sha256_k[64] = {
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

static void sha256_transform(Sha256Context *ctx, const unsigned char block[64]) {
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
               ((unsigned int)block[offset + 1] << 16) |
               ((unsigned int)block[offset + 2] << 8) |
               (unsigned int)block[offset + 3];
    }

    for (i = 16U; i < 64U; ++i) {
        unsigned int s0 = rotr32(w[i - 15U], 7U) ^ rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3);
        unsigned int s1 = rotr32(w[i - 2U], 17U) ^ rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10);
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
        unsigned int s1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
        unsigned int ch = (e & f) ^ ((~e) & g);
        unsigned int temp1 = h + s1 + ch + sha256_k[i] + w[i];
        unsigned int s0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
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

static void sha256_init(Sha256Context *ctx) {
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

static void sha256_update(Sha256Context *ctx, const unsigned char *data, size_t len) {
    size_t i = 0;

    ctx->bit_count += (unsigned long long)len * 8ULL;

    while (i < len) {
        size_t space = 64U - ctx->buffer_len;
        size_t chunk = (len - i < space) ? (len - i) : space;

        memcpy(ctx->buffer + ctx->buffer_len, data + i, chunk);
        ctx->buffer_len += chunk;
        i += chunk;

        if (ctx->buffer_len == 64U) {
            sha256_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0U;
        }
    }
}

static void sha256_final(Sha256Context *ctx, unsigned char out[32]) {
    unsigned char pad = 0x80U;
    unsigned char zero = 0U;
    unsigned char length_bytes[8];
    unsigned int i;

    for (i = 0; i < 8U; ++i) {
        length_bytes[7U - i] = (unsigned char)((ctx->bit_count >> (8U * i)) & 0xffU);
    }

    sha256_update(ctx, &pad, 1U);
    while (ctx->buffer_len != 56U) {
        sha256_update(ctx, &zero, 1U);
    }
    sha256_update(ctx, length_bytes, 8U);

    for (i = 0; i < 8U; ++i) {
        out[i * 4U] = (unsigned char)((ctx->state[i] >> 24) & 0xffU);
        out[i * 4U + 1U] = (unsigned char)((ctx->state[i] >> 16) & 0xffU);
        out[i * 4U + 2U] = (unsigned char)((ctx->state[i] >> 8) & 0xffU);
        out[i * 4U + 3U] = (unsigned char)(ctx->state[i] & 0xffU);
    }
}

typedef struct {
    unsigned long long state[8];
    unsigned long long bit_count_low;
    unsigned long long bit_count_high;
    unsigned char buffer[128];
    size_t buffer_len;
} Sha512Context;

static const unsigned long long sha512_k[80] = {
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

static void sha512_transform(Sha512Context *ctx, const unsigned char block[128]) {
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
               ((unsigned long long)block[offset + 1] << 48) |
               ((unsigned long long)block[offset + 2] << 40) |
               ((unsigned long long)block[offset + 3] << 32) |
               ((unsigned long long)block[offset + 4] << 24) |
               ((unsigned long long)block[offset + 5] << 16) |
               ((unsigned long long)block[offset + 6] << 8) |
               (unsigned long long)block[offset + 7];
    }

    for (i = 16U; i < 80U; ++i) {
        unsigned long long s0 = rotr64(w[i - 15U], 1U) ^ rotr64(w[i - 15U], 8U) ^ (w[i - 15U] >> 7);
        unsigned long long s1 = rotr64(w[i - 2U], 19U) ^ rotr64(w[i - 2U], 61U) ^ (w[i - 2U] >> 6);
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
        unsigned long long s1 = rotr64(e, 14U) ^ rotr64(e, 18U) ^ rotr64(e, 41U);
        unsigned long long ch = (e & f) ^ ((~e) & g);
        unsigned long long temp1 = h + s1 + ch + sha512_k[i] + w[i];
        unsigned long long s0 = rotr64(a, 28U) ^ rotr64(a, 34U) ^ rotr64(a, 39U);
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

static void sha512_add_bits(Sha512Context *ctx, unsigned long long bits) {
    unsigned long long old_low = ctx->bit_count_low;
    ctx->bit_count_low += bits;
    if (ctx->bit_count_low < old_low) {
        ctx->bit_count_high += 1ULL;
    }
}

static void sha512_init(Sha512Context *ctx) {
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

static void sha512_update(Sha512Context *ctx, const unsigned char *data, size_t len) {
    size_t i = 0;

    sha512_add_bits(ctx, (unsigned long long)len * 8ULL);

    while (i < len) {
        size_t space = 128U - ctx->buffer_len;
        size_t chunk = (len - i < space) ? (len - i) : space;

        memcpy(ctx->buffer + ctx->buffer_len, data + i, chunk);
        ctx->buffer_len += chunk;
        i += chunk;

        if (ctx->buffer_len == 128U) {
            sha512_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0U;
        }
    }
}

static void sha512_final(Sha512Context *ctx, unsigned char out[64]) {
    unsigned char pad = 0x80U;
    unsigned char zero = 0U;
    unsigned char length_bytes[16];
    unsigned int i;

    for (i = 0; i < 8U; ++i) {
        length_bytes[i] = (unsigned char)((ctx->bit_count_high >> (56U - 8U * i)) & 0xffU);
        length_bytes[8U + i] = (unsigned char)((ctx->bit_count_low >> (56U - 8U * i)) & 0xffU);
    }

    sha512_update(ctx, &pad, 1U);
    while (ctx->buffer_len != 112U) {
        sha512_update(ctx, &zero, 1U);
    }
    sha512_update(ctx, length_bytes, 16U);

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

void hash_to_hex(const unsigned char *digest, size_t digest_size, char *hex_out) {
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < digest_size; ++i) {
        hex_out[i * 2U] = digits[(digest[i] >> 4) & 0x0fU];
        hex_out[i * 2U + 1U] = digits[digest[i] & 0x0fU];
    }

    hex_out[digest_size * 2U] = '\0';
}

int hash_md5_stream(int fd, unsigned char out[HASH_MD5_SIZE]) {
    Md5Context ctx;
    unsigned char buffer[HASH_STREAM_BUFFER_SIZE];

    md5_init(&ctx);
    for (;;) {
        long bytes = platform_read(fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        md5_update(&ctx, buffer, (size_t)bytes);
    }
    md5_final(&ctx, out);
    return 0;
}

int hash_sha256_stream(int fd, unsigned char out[HASH_SHA256_SIZE]) {
    Sha256Context ctx;
    unsigned char buffer[HASH_STREAM_BUFFER_SIZE];

    sha256_init(&ctx);
    for (;;) {
        long bytes = platform_read(fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        sha256_update(&ctx, buffer, (size_t)bytes);
    }
    sha256_final(&ctx, out);
    return 0;
}

int hash_sha512_stream(int fd, unsigned char out[HASH_SHA512_SIZE]) {
    Sha512Context ctx;
    unsigned char buffer[HASH_STREAM_BUFFER_SIZE];

    sha512_init(&ctx);
    for (;;) {
        long bytes = platform_read(fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        sha512_update(&ctx, buffer, (size_t)bytes);
    }
    sha512_final(&ctx, out);
    return 0;
}
