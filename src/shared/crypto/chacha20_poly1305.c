#include "crypto/chacha20_poly1305.h"
#include "crypto/crypto_util.h"

#include <stddef.h>

typedef size_t usize;
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define SSHLAB_ENABLE_CRYPTO_TUNING 0
#define SSHLAB_FORCE_INLINE inline
#define SSHLAB_CRYPTO_VECTOR
#define SSHLAB_CRYPTO_SPEED
#define ssh_chachapoly_decrypt_length crypto_ssh_chachapoly_decrypt_length
#define ssh_chachapoly_encrypt_packet crypto_ssh_chachapoly_encrypt_packet
#define ssh_chachapoly_decrypt_packet crypto_ssh_chachapoly_decrypt_packet

#if SSHLAB_ENABLE_CRYPTO_TUNING && defined(__x86_64__)
#include <immintrin.h>
#endif

static SSHLAB_FORCE_INLINE u32 load32_le(const u8 *p) {
    return (u32)p[0]
        | ((u32)p[1] << 8)
        | ((u32)p[2] << 16)
        | ((u32)p[3] << 24);
}

static void secure_bzero(void *ptr, usize len) {
    crypto_secure_bzero(ptr, len);
}

static SSHLAB_FORCE_INLINE void store32_le(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static SSHLAB_FORCE_INLINE u32 rotl32(u32 x, u32 n) {
    return (x << n) | (x >> (32u - n));
}

static SSHLAB_FORCE_INLINE void chacha20_quarter_round(u32 *a, u32 *b, u32 *c, u32 *d) {
    *a += *b; *d ^= *a; *d = rotl32(*d, 16);
    *c += *d; *b ^= *c; *b = rotl32(*b, 12);
    *a += *b; *d ^= *a; *d = rotl32(*d, 8);
    *c += *d; *b ^= *c; *b = rotl32(*b, 7);
}

#if SSHLAB_ENABLE_CRYPTO_TUNING && defined(__x86_64__)
static SSHLAB_CRYPTO_VECTOR __m256i rotl32_256(__m256i x, int n) {
    return _mm256_or_si256(_mm256_slli_epi32(x, n), _mm256_srli_epi32(x, 32 - n));
}

static SSHLAB_CRYPTO_VECTOR void chacha20_quarter_round_256(__m256i *a, __m256i *b, __m256i *c, __m256i *d) {
    *a = _mm256_add_epi32(*a, *b);
    *d = _mm256_xor_si256(*d, *a);
    *d = rotl32_256(*d, 16);

    *c = _mm256_add_epi32(*c, *d);
    *b = _mm256_xor_si256(*b, *c);
    *b = rotl32_256(*b, 12);

    *a = _mm256_add_epi32(*a, *b);
    *d = _mm256_xor_si256(*d, *a);
    *d = rotl32_256(*d, 8);

    *c = _mm256_add_epi32(*c, *d);
    *b = _mm256_xor_si256(*b, *c);
    *b = rotl32_256(*b, 7);
}

static SSHLAB_CRYPTO_VECTOR void chacha20_blocks8(u8 out[512], const u8 key[32], u64 counter, u32 seqnr) {
    u32 ctr_lo[8];
    u32 ctr_hi[8];
    u32 words[16][8];
    u8 iv[8];
    __m256i x0;
    __m256i x1;
    __m256i x2;
    __m256i x3;
    __m256i x4;
    __m256i x5;
    __m256i x6;
    __m256i x7;
    __m256i x8;
    __m256i x9;
    __m256i x10;
    __m256i x11;
    __m256i x12;
    __m256i x13;
    __m256i x14;
    __m256i x15;
    __m256i y0;
    __m256i y1;
    __m256i y2;
    __m256i y3;
    __m256i y4;
    __m256i y5;
    __m256i y6;
    __m256i y7;
    __m256i y8;
    __m256i y9;
    __m256i y10;
    __m256i y11;
    __m256i y12;
    __m256i y13;
    __m256i y14;
    __m256i y15;
    u32 i;
    u32 lane;

    iv[0] = 0u;
    iv[1] = 0u;
    iv[2] = 0u;
    iv[3] = 0u;
    iv[4] = (u8)(seqnr >> 24);
    iv[5] = (u8)(seqnr >> 16);
    iv[6] = (u8)(seqnr >> 8);
    iv[7] = (u8)seqnr;

    for (lane = 0; lane < 8u; lane++) {
        u64 ctr = counter + (u64)lane;
        ctr_lo[lane] = (u32)ctr;
        ctr_hi[lane] = (u32)(ctr >> 32);
    }

    x0 = _mm256_set1_epi32(0x61707865u);
    x1 = _mm256_set1_epi32(0x3320646eu);
    x2 = _mm256_set1_epi32(0x79622d32u);
    x3 = _mm256_set1_epi32(0x6b206574u);
    x4 = _mm256_set1_epi32((int)load32_le(key + 0u));
    x5 = _mm256_set1_epi32((int)load32_le(key + 4u));
    x6 = _mm256_set1_epi32((int)load32_le(key + 8u));
    x7 = _mm256_set1_epi32((int)load32_le(key + 12u));
    x8 = _mm256_set1_epi32((int)load32_le(key + 16u));
    x9 = _mm256_set1_epi32((int)load32_le(key + 20u));
    x10 = _mm256_set1_epi32((int)load32_le(key + 24u));
    x11 = _mm256_set1_epi32((int)load32_le(key + 28u));
    x12 = _mm256_loadu_si256((const __m256i *)ctr_lo);
    x13 = _mm256_loadu_si256((const __m256i *)ctr_hi);
    x14 = _mm256_set1_epi32((int)load32_le(iv + 0u));
    x15 = _mm256_set1_epi32((int)load32_le(iv + 4u));

    y0 = x0; y1 = x1; y2 = x2; y3 = x3;
    y4 = x4; y5 = x5; y6 = x6; y7 = x7;
    y8 = x8; y9 = x9; y10 = x10; y11 = x11;
    y12 = x12; y13 = x13; y14 = x14; y15 = x15;

    for (i = 0; i < 10u; i++) {
        chacha20_quarter_round_256(&x0, &x4, &x8, &x12);
        chacha20_quarter_round_256(&x1, &x5, &x9, &x13);
        chacha20_quarter_round_256(&x2, &x6, &x10, &x14);
        chacha20_quarter_round_256(&x3, &x7, &x11, &x15);
        chacha20_quarter_round_256(&x0, &x5, &x10, &x15);
        chacha20_quarter_round_256(&x1, &x6, &x11, &x12);
        chacha20_quarter_round_256(&x2, &x7, &x8, &x13);
        chacha20_quarter_round_256(&x3, &x4, &x9, &x14);
    }

    x0 = _mm256_add_epi32(x0, y0);
    x1 = _mm256_add_epi32(x1, y1);
    x2 = _mm256_add_epi32(x2, y2);
    x3 = _mm256_add_epi32(x3, y3);
    x4 = _mm256_add_epi32(x4, y4);
    x5 = _mm256_add_epi32(x5, y5);
    x6 = _mm256_add_epi32(x6, y6);
    x7 = _mm256_add_epi32(x7, y7);
    x8 = _mm256_add_epi32(x8, y8);
    x9 = _mm256_add_epi32(x9, y9);
    x10 = _mm256_add_epi32(x10, y10);
    x11 = _mm256_add_epi32(x11, y11);
    x12 = _mm256_add_epi32(x12, y12);
    x13 = _mm256_add_epi32(x13, y13);
    x14 = _mm256_add_epi32(x14, y14);
    x15 = _mm256_add_epi32(x15, y15);

    _mm256_storeu_si256((__m256i *)words[0], x0);
    _mm256_storeu_si256((__m256i *)words[1], x1);
    _mm256_storeu_si256((__m256i *)words[2], x2);
    _mm256_storeu_si256((__m256i *)words[3], x3);
    _mm256_storeu_si256((__m256i *)words[4], x4);
    _mm256_storeu_si256((__m256i *)words[5], x5);
    _mm256_storeu_si256((__m256i *)words[6], x6);
    _mm256_storeu_si256((__m256i *)words[7], x7);
    _mm256_storeu_si256((__m256i *)words[8], x8);
    _mm256_storeu_si256((__m256i *)words[9], x9);
    _mm256_storeu_si256((__m256i *)words[10], x10);
    _mm256_storeu_si256((__m256i *)words[11], x11);
    _mm256_storeu_si256((__m256i *)words[12], x12);
    _mm256_storeu_si256((__m256i *)words[13], x13);
    _mm256_storeu_si256((__m256i *)words[14], x14);
    _mm256_storeu_si256((__m256i *)words[15], x15);

    for (lane = 0; lane < 8u; lane++) {
        for (i = 0; i < 16u; i++) {
            store32_le(out + (usize)lane * 64u + (usize)i * 4u, words[i][lane]);
        }
    }

    secure_bzero(iv, sizeof(iv));
    secure_bzero(ctr_lo, sizeof(ctr_lo));
    secure_bzero(ctr_hi, sizeof(ctr_hi));
    secure_bzero(words, sizeof(words));
}
#endif

static SSHLAB_CRYPTO_VECTOR void chacha20_block(u8 out[64], const u8 key[32], u64 counter, u32 seqnr) {
    u32 state[16];
    u32 working[16];
    u8 iv[8];
    u32 i;

    state[0] = 0x61707865u;
    state[1] = 0x3320646eu;
    state[2] = 0x79622d32u;
    state[3] = 0x6b206574u;

    for (i = 0; i < 8u; i++) {
        state[4u + i] = load32_le(key + (usize)i * 4u);
    }

    state[12] = (u32)counter;
    state[13] = (u32)(counter >> 32);

    iv[0] = 0u;
    iv[1] = 0u;
    iv[2] = 0u;
    iv[3] = 0u;
    iv[4] = (u8)(seqnr >> 24);
    iv[5] = (u8)(seqnr >> 16);
    iv[6] = (u8)(seqnr >> 8);
    iv[7] = (u8)seqnr;
    state[14] = load32_le(iv + 0u);
    state[15] = load32_le(iv + 4u);

    for (i = 0; i < 16u; i++) {
        working[i] = state[i];
    }

    for (i = 0; i < 10u; i++) {
        chacha20_quarter_round(&working[0], &working[4], &working[8], &working[12]);
        chacha20_quarter_round(&working[1], &working[5], &working[9], &working[13]);
        chacha20_quarter_round(&working[2], &working[6], &working[10], &working[14]);
        chacha20_quarter_round(&working[3], &working[7], &working[11], &working[15]);
        chacha20_quarter_round(&working[0], &working[5], &working[10], &working[15]);
        chacha20_quarter_round(&working[1], &working[6], &working[11], &working[12]);
        chacha20_quarter_round(&working[2], &working[7], &working[8], &working[13]);
        chacha20_quarter_round(&working[3], &working[4], &working[9], &working[14]);
    }

    for (i = 0; i < 16u; i++) {
        working[i] += state[i];
        store32_le(out + (usize)i * 4u, working[i]);
    }

    secure_bzero(iv, sizeof(iv));
    secure_bzero(state, sizeof(state));
    secure_bzero(working, sizeof(working));
}

static SSHLAB_CRYPTO_VECTOR void chacha20_xor(u8 *out, const u8 *in, usize len, const u8 key[32], u64 counter, u32 seqnr) {
#if SSHLAB_ENABLE_CRYPTO_TUNING && defined(__x86_64__)
    u8 blocks8[512];
#endif
    u8 block[64];
    usize done = 0;
    usize i;

    while (done < len) {
#if SSHLAB_ENABLE_CRYPTO_TUNING && defined(__x86_64__)
        if (len - done >= sizeof(blocks8)) {
            chacha20_blocks8(blocks8, key, counter, seqnr);
            for (i = 0; i < sizeof(blocks8); i++) {
                u8 src = in != (const u8 *)0 ? in[done + i] : 0u;
                out[done + i] = src ^ blocks8[i];
            }
            done += sizeof(blocks8);
            counter += 8u;
            continue;
        }
#endif
        {
            usize take = len - done;
            if (take > sizeof(block)) {
                take = sizeof(block);
            }

            chacha20_block(block, key, counter, seqnr);
            for (i = 0; i < take; i++) {
                u8 src = in != (const u8 *)0 ? in[done + i] : 0u;
                out[done + i] = src ^ block[i];
            }

            done += take;
            counter++;
        }
    }

#if SSHLAB_ENABLE_CRYPTO_TUNING && defined(__x86_64__)
    secure_bzero(blocks8, sizeof(blocks8));
#endif
    secure_bzero(block, sizeof(block));
}

static SSHLAB_CRYPTO_SPEED void poly1305_auth(u8 tag[16], const u8 *m, usize inlen, const u8 key[32]) {
    u32 t0, t1, t2, t3;
    u32 h0, h1, h2, h3, h4;
    u32 r0, r1, r2, r3, r4;
    u32 s1, s2, s3, s4;
    u32 b, nb;
    usize j;
    u64 t[5];
    u64 f0, f1, f2, f3;
    u32 g0, g1, g2, g3, g4;
    u8 mp[16];

    t0 = load32_le(key + 0);
    t1 = load32_le(key + 4);
    t2 = load32_le(key + 8);
    t3 = load32_le(key + 12);

    r0 = t0 & 0x3ffffffu; t0 >>= 26; t0 |= t1 << 6;
    r1 = t0 & 0x3ffff03u; t1 >>= 20; t1 |= t2 << 12;
    r2 = t1 & 0x3ffc0ffu; t2 >>= 14; t2 |= t3 << 18;
    r3 = t2 & 0x3f03fffu; t3 >>= 8;
    r4 = t3 & 0x00fffffu;

    s1 = r1 * 5u;
    s2 = r2 * 5u;
    s3 = r3 * 5u;
    s4 = r4 * 5u;

    h0 = 0u;
    h1 = 0u;
    h2 = 0u;
    h3 = 0u;
    h4 = 0u;

    if (inlen < 16u) {
        goto poly1305_donna_atmost15bytes;
    }

poly1305_donna_16bytes:
    m += 16u;
    inlen -= 16u;

    t0 = load32_le(m - 16u);
    t1 = load32_le(m - 12u);
    t2 = load32_le(m - 8u);
    t3 = load32_le(m - 4u);

    h0 += t0 & 0x3ffffffu;
    h1 += ((((u64)t1 << 32) | t0) >> 26) & 0x3ffffffu;
    h2 += ((((u64)t2 << 32) | t1) >> 20) & 0x3ffffffu;
    h3 += ((((u64)t3 << 32) | t2) >> 14) & 0x3ffffffu;
    h4 += (t3 >> 8) | (1u << 24);

poly1305_donna_mul:
    t[0] = (u64)h0 * r0 + (u64)h1 * s4 + (u64)h2 * s3 + (u64)h3 * s2 + (u64)h4 * s1;
    t[1] = (u64)h0 * r1 + (u64)h1 * r0 + (u64)h2 * s4 + (u64)h3 * s3 + (u64)h4 * s2;
    t[2] = (u64)h0 * r2 + (u64)h1 * r1 + (u64)h2 * r0 + (u64)h3 * s4 + (u64)h4 * s3;
    t[3] = (u64)h0 * r3 + (u64)h1 * r2 + (u64)h2 * r1 + (u64)h3 * r0 + (u64)h4 * s4;
    t[4] = (u64)h0 * r4 + (u64)h1 * r3 + (u64)h2 * r2 + (u64)h3 * r1 + (u64)h4 * r0;

    h0 = (u32)t[0] & 0x3ffffffu; b = (u32)(t[0] >> 26);
    t[1] += b; h1 = (u32)t[1] & 0x3ffffffu; b = (u32)(t[1] >> 26);
    t[2] += b; h2 = (u32)t[2] & 0x3ffffffu; b = (u32)(t[2] >> 26);
    t[3] += b; h3 = (u32)t[3] & 0x3ffffffu; b = (u32)(t[3] >> 26);
    t[4] += b; h4 = (u32)t[4] & 0x3ffffffu; b = (u32)(t[4] >> 26);
    h0 += b * 5u;

    if (inlen >= 16u) {
        goto poly1305_donna_16bytes;
    }

poly1305_donna_atmost15bytes:
    if (inlen == 0u) {
        goto poly1305_donna_finish;
    }

    for (j = 0; j < inlen; j++) {
        mp[j] = m[j];
    }
    mp[j++] = 1u;
    while (j < 16u) {
        mp[j++] = 0u;
    }
    inlen = 0u;

    t0 = load32_le(mp + 0);
    t1 = load32_le(mp + 4);
    t2 = load32_le(mp + 8);
    t3 = load32_le(mp + 12);

    h0 += t0 & 0x3ffffffu;
    h1 += ((((u64)t1 << 32) | t0) >> 26) & 0x3ffffffu;
    h2 += ((((u64)t2 << 32) | t1) >> 20) & 0x3ffffffu;
    h3 += ((((u64)t3 << 32) | t2) >> 14) & 0x3ffffffu;
    h4 += (t3 >> 8);

    goto poly1305_donna_mul;

poly1305_donna_finish:
    b = h0 >> 26; h0 &= 0x3ffffffu;
    h1 += b; b = h1 >> 26; h1 &= 0x3ffffffu;
    h2 += b; b = h2 >> 26; h2 &= 0x3ffffffu;
    h3 += b; b = h3 >> 26; h3 &= 0x3ffffffu;
    h4 += b; b = h4 >> 26; h4 &= 0x3ffffffu;
    h0 += b * 5u; b = h0 >> 26; h0 &= 0x3ffffffu;
    h1 += b;

    g0 = h0 + 5u; b = g0 >> 26; g0 &= 0x3ffffffu;
    g1 = h1 + b; b = g1 >> 26; g1 &= 0x3ffffffu;
    g2 = h2 + b; b = g2 >> 26; g2 &= 0x3ffffffu;
    g3 = h3 + b; b = g3 >> 26; g3 &= 0x3ffffffu;
    g4 = h4 + b - (1u << 26);

    b = (g4 >> 31) - 1u;
    nb = ~b;
    h0 = (h0 & nb) | (g0 & b);
    h1 = (h1 & nb) | (g1 & b);
    h2 = (h2 & nb) | (g2 & b);
    h3 = (h3 & nb) | (g3 & b);
    h4 = (h4 & nb) | (g4 & b);

    f0 = ((h0      ) | (h1 << 26)) + (u64)load32_le(key + 16);
    f1 = ((h1 >>  6) | (h2 << 20)) + (u64)load32_le(key + 20);
    f2 = ((h2 >> 12) | (h3 << 14)) + (u64)load32_le(key + 24);
    f3 = ((h3 >> 18) | (h4 <<  8)) + (u64)load32_le(key + 28);

    store32_le(tag + 0, (u32)f0);
    f1 += (f0 >> 32);
    store32_le(tag + 4, (u32)f1);
    f2 += (f1 >> 32);
    store32_le(tag + 8, (u32)f2);
    f3 += (f2 >> 32);
    store32_le(tag + 12, (u32)f3);

    secure_bzero(&t0, sizeof(t0));
    secure_bzero(&t1, sizeof(t1));
    secure_bzero(&t2, sizeof(t2));
    secure_bzero(&t3, sizeof(t3));
    secure_bzero(&h0, sizeof(h0));
    secure_bzero(&h1, sizeof(h1));
    secure_bzero(&h2, sizeof(h2));
    secure_bzero(&h3, sizeof(h3));
    secure_bzero(&h4, sizeof(h4));
    secure_bzero(&r0, sizeof(r0));
    secure_bzero(&r1, sizeof(r1));
    secure_bzero(&r2, sizeof(r2));
    secure_bzero(&r3, sizeof(r3));
    secure_bzero(&r4, sizeof(r4));
    secure_bzero(&s1, sizeof(s1));
    secure_bzero(&s2, sizeof(s2));
    secure_bzero(&s3, sizeof(s3));
    secure_bzero(&s4, sizeof(s4));
    secure_bzero(&b, sizeof(b));
    secure_bzero(&nb, sizeof(nb));
    secure_bzero(&f0, sizeof(f0));
    secure_bzero(&f1, sizeof(f1));
    secure_bzero(&f2, sizeof(f2));
    secure_bzero(&f3, sizeof(f3));
    secure_bzero(&g0, sizeof(g0));
    secure_bzero(&g1, sizeof(g1));
    secure_bzero(&g2, sizeof(g2));
    secure_bzero(&g3, sizeof(g3));
    secure_bzero(&g4, sizeof(g4));
    secure_bzero(mp, sizeof(mp));
    secure_bzero(t, sizeof(t));
}

static void compute_poly_key(u8 poly_key[32], const u8 main_key[32], u32 seqnr) {
    u8 block[64];
    size_t i;

    chacha20_block(block, main_key, 0u, seqnr);
    for (i = 0; i < 32u; i++) {
        poly_key[i] = block[i];
    }
    secure_bzero(block, sizeof(block));
}

static int tag_matches(const u8 *a, const u8 *b, usize len) {
    u8 diff = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        diff |= (u8)(a[i] ^ b[i]);
    }
    return diff == 0 ? 0 : -1;
}

void ssh_chachapoly_decrypt_length(const u8 key[64], u32 seqnr, const u8 encrypted_len[4], u8 plain_len[4]) {
    chacha20_xor(plain_len, encrypted_len, 4u, key + 32, 0u, seqnr);
}

void ssh_chachapoly_encrypt_packet(const u8 key[64], u32 seqnr, u8 *packet, usize packet_len, u8 tag[16]) {
    u8 poly_key[32];

    chacha20_xor(packet, packet, 4u, key + 32, 0u, seqnr);
    if (packet_len > 4u) {
        chacha20_xor(packet + 4u, packet + 4u, packet_len - 4u, key, 1u, seqnr);
    }
    compute_poly_key(poly_key, key, seqnr);
    poly1305_auth(tag, packet, packet_len, poly_key);
    secure_bzero(poly_key, sizeof(poly_key));
}

int ssh_chachapoly_decrypt_packet(const u8 key[64], u32 seqnr, u8 *packet, usize packet_len, const u8 tag[16]) {
    u8 expected[16];
    u8 poly_key[32];
    int status = 0;

    compute_poly_key(poly_key, key, seqnr);
    poly1305_auth(expected, packet, packet_len, poly_key);
    if (tag_matches(expected, tag, 16u) != 0) {
        status = -1;
        goto cleanup;
    }

    ssh_chachapoly_decrypt_length(key, seqnr, packet, packet);
    if (packet_len > 4u) {
        chacha20_xor(packet + 4u, packet + 4u, packet_len - 4u, key, 1u, seqnr);
    }

cleanup:
    secure_bzero(expected, sizeof(expected));
    secure_bzero(poly_key, sizeof(poly_key));
    return status;
}
