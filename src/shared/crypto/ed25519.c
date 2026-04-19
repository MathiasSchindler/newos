#include "crypto/ed25519.h"
#include "crypto/crypto_util.h"
#include "crypto/sha512.h"

#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

typedef size_t usize;
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define SSHLAB_FORCE_INLINE inline
#define SSHLAB_CRYPTO_SPEED
#define ed25519_public_key_from_seed crypto_ed25519_public_key_from_seed
#define ed25519_sign crypto_ed25519_sign
#define ed25519_verify crypto_ed25519_verify
#define sha512_init crypto_sha512_init
#define sha512_update crypto_sha512_update
#define sha512_final crypto_sha512_final

#define MASK51 ((u64)((1ULL << 51) - 1ULL))

struct fe {
    u64 v[5];
};

struct ge_p3 {
    struct fe X;
    struct fe Y;
    struct fe Z;
    struct fe T;
};

static const struct fe g_ed25519_d = {
    {929955233495203ULL, 466365720129213ULL, 1662059464998953ULL, 2033849074728123ULL, 1442794654840575ULL}
};

static const struct fe g_ed25519_sqrtm1 = {
    {1718705420411056ULL, 234908883556509ULL, 2233514472574048ULL, 2117202627021982ULL, 765476049583133ULL}
};

static const u8 g_group_order[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

static const u8 g_field_prime[32] = {
    0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f
};

static const u8 g_basepoint_bytes[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66
};

static SSHLAB_FORCE_INLINE u64 load64_le(const u8 *p) {
    return (u64)p[0]
        | ((u64)p[1] << 8)
        | ((u64)p[2] << 16)
        | ((u64)p[3] << 24)
        | ((u64)p[4] << 32)
        | ((u64)p[5] << 40)
        | ((u64)p[6] << 48)
        | ((u64)p[7] << 56);
}

static void secure_bzero(void *ptr, usize len) {
    crypto_secure_bzero(ptr, len);
}

static void fe_0(struct fe *f) {
    f->v[0] = 0; f->v[1] = 0; f->v[2] = 0; f->v[3] = 0; f->v[4] = 0;
}

static void fe_1(struct fe *f) {
    fe_0(f);
    f->v[0] = 1;
}

static void fe_copy(struct fe *dst, const struct fe *src) {
    dst->v[0] = src->v[0];
    dst->v[1] = src->v[1];
    dst->v[2] = src->v[2];
    dst->v[3] = src->v[3];
    dst->v[4] = src->v[4];
}

static void fe_carry(struct fe *f) {
    u64 c;

    c = f->v[0] >> 51;
    f->v[0] &= MASK51;
    f->v[1] += c;
    c = f->v[1] >> 51;
    f->v[1] &= MASK51;
    f->v[2] += c;
    c = f->v[2] >> 51;
    f->v[2] &= MASK51;
    f->v[3] += c;
    c = f->v[3] >> 51;
    f->v[3] &= MASK51;
    f->v[4] += c;
    c = f->v[4] >> 51;
    f->v[4] &= MASK51;
    f->v[0] += c * 19u;
    c = f->v[0] >> 51;
    f->v[0] &= MASK51;
    f->v[1] += c;
    c = f->v[1] >> 51;
    f->v[1] &= MASK51;
    f->v[2] += c;
    c = f->v[2] >> 51;
    f->v[2] &= MASK51;
    f->v[3] += c;
    c = f->v[3] >> 51;
    f->v[3] &= MASK51;
    f->v[4] += c;
    c = f->v[4] >> 51;
    f->v[4] &= MASK51;
    f->v[0] += c * 19u;
    c = f->v[0] >> 51;
    f->v[0] &= MASK51;
    f->v[1] += c;
}

static void fe_frombytes(struct fe *f, const u8 s[32]) {
    u64 t0 = load64_le(s + 0);
    u64 t1 = load64_le(s + 6);
    u64 t2 = load64_le(s + 12);
    u64 t3 = load64_le(s + 19);
    u64 t4 = load64_le(s + 24);

    f->v[0] = t0 & MASK51;
    f->v[1] = (t1 >> 3) & MASK51;
    f->v[2] = (t2 >> 6) & MASK51;
    f->v[3] = (t3 >> 1) & MASK51;
    f->v[4] = (t4 >> 12) & MASK51;
}

static void fe_normalize(struct fe *f) {
    const u64 p0 = MASK51 - 18u;
    u64 t[5];
    u64 sub = 0;
    u64 borrow = 0;
    u64 mask = 0;
    usize i = 0;

    fe_carry(f);
    fe_carry(f);

    sub = p0;
    t[0] = f->v[0] - sub;
    borrow = (t[0] >> 63) & 1u;

    for (i = 1; i < 5u; i++) {
        sub = MASK51 + borrow;
        t[i] = f->v[i] - sub;
        borrow = (t[i] >> 63) & 1u;
    }

    mask = (u64)0 - (u64)(borrow ^ 1u);
    for (i = 0; i < 5u; i++) {
        f->v[i] = (f->v[i] & ~mask) | (t[i] & mask);
    }
}

static void fe_tobytes(u8 out[32], const struct fe *f) {
    struct fe t;
    u64 h0;
    u64 h1;
    u64 h2;
    u64 h3;
    u64 h4;

    fe_copy(&t, f);
    fe_normalize(&t);

    h0 = t.v[0];
    h1 = t.v[1];
    h2 = t.v[2];
    h3 = t.v[3];
    h4 = t.v[4];

    out[0] = (u8)(h0 >> 0);
    out[1] = (u8)(h0 >> 8);
    out[2] = (u8)(h0 >> 16);
    out[3] = (u8)(h0 >> 24);
    out[4] = (u8)(h0 >> 32);
    out[5] = (u8)(h0 >> 40);
    out[6] = (u8)((h0 >> 48) | (h1 << 3));
    out[7] = (u8)(h1 >> 5);
    out[8] = (u8)(h1 >> 13);
    out[9] = (u8)(h1 >> 21);
    out[10] = (u8)(h1 >> 29);
    out[11] = (u8)(h1 >> 37);
    out[12] = (u8)((h1 >> 45) | (h2 << 6));
    out[13] = (u8)(h2 >> 2);
    out[14] = (u8)(h2 >> 10);
    out[15] = (u8)(h2 >> 18);
    out[16] = (u8)(h2 >> 26);
    out[17] = (u8)(h2 >> 34);
    out[18] = (u8)(h2 >> 42);
    out[19] = (u8)((h2 >> 50) | (h3 << 1));
    out[20] = (u8)(h3 >> 7);
    out[21] = (u8)(h3 >> 15);
    out[22] = (u8)(h3 >> 23);
    out[23] = (u8)(h3 >> 31);
    out[24] = (u8)(h3 >> 39);
    out[25] = (u8)((h3 >> 47) | (h4 << 4));
    out[26] = (u8)(h4 >> 4);
    out[27] = (u8)(h4 >> 12);
    out[28] = (u8)(h4 >> 20);
    out[29] = (u8)(h4 >> 28);
    out[30] = (u8)(h4 >> 36);
    out[31] = (u8)(h4 >> 44);
}

static void fe_add(struct fe *h, const struct fe *f, const struct fe *g) {
    h->v[0] = f->v[0] + g->v[0];
    h->v[1] = f->v[1] + g->v[1];
    h->v[2] = f->v[2] + g->v[2];
    h->v[3] = f->v[3] + g->v[3];
    h->v[4] = f->v[4] + g->v[4];
}

static void fe_sub(struct fe *h, const struct fe *f, const struct fe *g) {
    h->v[0] = f->v[0] + (MASK51 - 18u) * 2u - g->v[0];
    h->v[1] = f->v[1] + MASK51 * 2u - g->v[1];
    h->v[2] = f->v[2] + MASK51 * 2u - g->v[2];
    h->v[3] = f->v[3] + MASK51 * 2u - g->v[3];
    h->v[4] = f->v[4] + MASK51 * 2u - g->v[4];
}

static void fe_neg(struct fe *h, const struct fe *f) {
    struct fe zero;
    fe_0(&zero);
    fe_sub(h, &zero, f);
}

static SSHLAB_CRYPTO_SPEED void fe_mul(struct fe *h, const struct fe *f, const struct fe *g) {
    unsigned __int128 f0 = f->v[0];
    unsigned __int128 f1 = f->v[1];
    unsigned __int128 f2 = f->v[2];
    unsigned __int128 f3 = f->v[3];
    unsigned __int128 f4 = f->v[4];
    unsigned __int128 g0 = g->v[0];
    unsigned __int128 g1 = g->v[1];
    unsigned __int128 g2 = g->v[2];
    unsigned __int128 g3 = g->v[3];
    unsigned __int128 g4 = g->v[4];
    unsigned __int128 h0 = f0 * g0 + 19u * (f1 * g4 + f2 * g3 + f3 * g2 + f4 * g1);
    unsigned __int128 h1 = f0 * g1 + f1 * g0 + 19u * (f2 * g4 + f3 * g3 + f4 * g2);
    unsigned __int128 h2 = f0 * g2 + f1 * g1 + f2 * g0 + 19u * (f3 * g4 + f4 * g3);
    unsigned __int128 h3 = f0 * g3 + f1 * g2 + f2 * g1 + f3 * g0 + 19u * (f4 * g4);
    unsigned __int128 h4 = f0 * g4 + f1 * g3 + f2 * g2 + f3 * g1 + f4 * g0;
    u64 carry;

    carry = (u64)(h0 >> 51); h0 &= MASK51; h1 += carry;
    carry = (u64)(h1 >> 51); h1 &= MASK51; h2 += carry;
    carry = (u64)(h2 >> 51); h2 &= MASK51; h3 += carry;
    carry = (u64)(h3 >> 51); h3 &= MASK51; h4 += carry;
    carry = (u64)(h4 >> 51); h4 &= MASK51; h0 += (unsigned __int128)carry * 19u;
    carry = (u64)(h0 >> 51); h0 &= MASK51; h1 += carry;

    h->v[0] = (u64)h0;
    h->v[1] = (u64)h1;
    h->v[2] = (u64)h2;
    h->v[3] = (u64)h3;
    h->v[4] = (u64)h4;
}

static SSHLAB_CRYPTO_SPEED void fe_sq(struct fe *h, const struct fe *f) {
    unsigned __int128 f0 = f->v[0];
    unsigned __int128 f1 = f->v[1];
    unsigned __int128 f2 = f->v[2];
    unsigned __int128 f3 = f->v[3];
    unsigned __int128 f4 = f->v[4];
    unsigned __int128 f0_2 = f0 + f0;
    unsigned __int128 f1_2 = f1 + f1;
    unsigned __int128 h0 = f0 * f0 + 38u * (f1 * f4 + f2 * f3);
    unsigned __int128 h1 = f0_2 * f1 + 38u * f2 * f4 + 19u * f3 * f3;
    unsigned __int128 h2 = f0_2 * f2 + f1 * f1 + 38u * f3 * f4;
    unsigned __int128 h3 = f0_2 * f3 + f1_2 * f2 + 19u * f4 * f4;
    unsigned __int128 h4 = f0_2 * f4 + f1_2 * f3 + f2 * f2;
    u64 carry;

    carry = (u64)(h0 >> 51); h0 &= MASK51; h1 += carry;
    carry = (u64)(h1 >> 51); h1 &= MASK51; h2 += carry;
    carry = (u64)(h2 >> 51); h2 &= MASK51; h3 += carry;
    carry = (u64)(h3 >> 51); h3 &= MASK51; h4 += carry;
    carry = (u64)(h4 >> 51); h4 &= MASK51; h0 += (unsigned __int128)carry * 19u;
    carry = (u64)(h0 >> 51); h0 &= MASK51; h1 += carry;

    h->v[0] = (u64)h0;
    h->v[1] = (u64)h1;
    h->v[2] = (u64)h2;
    h->v[3] = (u64)h3;
    h->v[4] = (u64)h4;
}

static SSHLAB_CRYPTO_SPEED void fe_mul_small(struct fe *h, const struct fe *f, u64 k) {
    unsigned __int128 h0 = (unsigned __int128)f->v[0] * k;
    unsigned __int128 h1 = (unsigned __int128)f->v[1] * k;
    unsigned __int128 h2 = (unsigned __int128)f->v[2] * k;
    unsigned __int128 h3 = (unsigned __int128)f->v[3] * k;
    unsigned __int128 h4 = (unsigned __int128)f->v[4] * k;
    u64 carry;

    carry = (u64)(h0 >> 51); h0 &= MASK51; h1 += carry;
    carry = (u64)(h1 >> 51); h1 &= MASK51; h2 += carry;
    carry = (u64)(h2 >> 51); h2 &= MASK51; h3 += carry;
    carry = (u64)(h3 >> 51); h3 &= MASK51; h4 += carry;
    carry = (u64)(h4 >> 51); h4 &= MASK51; h0 += (unsigned __int128)carry * 19u;
    carry = (u64)(h0 >> 51); h0 &= MASK51; h1 += carry;

    h->v[0] = (u64)h0;
    h->v[1] = (u64)h1;
    h->v[2] = (u64)h2;
    h->v[3] = (u64)h3;
    h->v[4] = (u64)h4;
}

static SSHLAB_CRYPTO_SPEED void fe_pow22523(struct fe *out, const struct fe *z) {
    struct fe z2;
    struct fe z9;
    struct fe z11;
    struct fe z2_5_0;
    struct fe z2_10_0;
    struct fe z2_20_0;
    struct fe z2_50_0;
    struct fe z2_100_0;
    struct fe t;
    int i;

    fe_sq(&z2, z);
    fe_sq(&t, &z2);
    fe_sq(&t, &t);
    fe_mul(&z9, &t, z);
    fe_mul(&z11, &z9, &z2);
    fe_sq(&t, &z11);
    fe_mul(&z2_5_0, &t, &z9);

    fe_sq(&t, &z2_5_0);
    for (i = 1; i < 5; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&z2_10_0, &t, &z2_5_0);

    fe_sq(&t, &z2_10_0);
    for (i = 1; i < 10; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&z2_20_0, &t, &z2_10_0);

    fe_sq(&t, &z2_20_0);
    for (i = 1; i < 20; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&t, &t, &z2_20_0);

    fe_sq(&t, &t);
    for (i = 1; i < 10; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&z2_50_0, &t, &z2_10_0);

    fe_sq(&t, &z2_50_0);
    for (i = 1; i < 50; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&z2_100_0, &t, &z2_50_0);

    fe_sq(&t, &z2_100_0);
    for (i = 1; i < 100; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&t, &t, &z2_100_0);

    fe_sq(&t, &t);
    for (i = 1; i < 50; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&t, &t, &z2_50_0);

    fe_sq(&t, &t);
    fe_sq(&t, &t);
    fe_mul(out, &t, z);

    secure_bzero(&z2, sizeof(z2));
    secure_bzero(&z9, sizeof(z9));
    secure_bzero(&z11, sizeof(z11));
    secure_bzero(&z2_5_0, sizeof(z2_5_0));
    secure_bzero(&z2_10_0, sizeof(z2_10_0));
    secure_bzero(&z2_20_0, sizeof(z2_20_0));
    secure_bzero(&z2_50_0, sizeof(z2_50_0));
    secure_bzero(&z2_100_0, sizeof(z2_100_0));
    secure_bzero(&t, sizeof(t));
}

static SSHLAB_CRYPTO_SPEED void fe_inv(struct fe *out, const struct fe *z) {
    struct fe z2;
    struct fe z9;
    struct fe z11;
    struct fe z2_5_0;
    struct fe z2_10_0;
    struct fe z2_20_0;
    struct fe z2_50_0;
    struct fe z2_100_0;
    struct fe t;
    int i;

    fe_sq(&z2, z);
    fe_sq(&t, &z2);
    fe_sq(&t, &t);
    fe_mul(&z9, &t, z);
    fe_mul(&z11, &z9, &z2);
    fe_sq(&t, &z11);
    fe_mul(&z2_5_0, &t, &z9);

    fe_sq(&t, &z2_5_0);
    for (i = 1; i < 5; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&z2_10_0, &t, &z2_5_0);

    fe_sq(&t, &z2_10_0);
    for (i = 1; i < 10; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&z2_20_0, &t, &z2_10_0);

    fe_sq(&t, &z2_20_0);
    for (i = 1; i < 20; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&t, &t, &z2_20_0);

    fe_sq(&t, &t);
    for (i = 1; i < 10; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&z2_50_0, &t, &z2_10_0);

    fe_sq(&t, &z2_50_0);
    for (i = 1; i < 50; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&z2_100_0, &t, &z2_50_0);

    fe_sq(&t, &z2_100_0);
    for (i = 1; i < 100; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&t, &t, &z2_100_0);

    fe_sq(&t, &t);
    for (i = 1; i < 50; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(&t, &t, &z2_50_0);

    fe_sq(&t, &t);
    for (i = 1; i < 5; i++) {
        fe_sq(&t, &t);
    }
    fe_mul(out, &t, &z11);

    secure_bzero(&z2, sizeof(z2));
    secure_bzero(&z9, sizeof(z9));
    secure_bzero(&z11, sizeof(z11));
    secure_bzero(&z2_5_0, sizeof(z2_5_0));
    secure_bzero(&z2_10_0, sizeof(z2_10_0));
    secure_bzero(&z2_20_0, sizeof(z2_20_0));
    secure_bzero(&z2_50_0, sizeof(z2_50_0));
    secure_bzero(&z2_100_0, sizeof(z2_100_0));
    secure_bzero(&t, sizeof(t));
}

static int fe_equal(const struct fe *a, const struct fe *b) {
    u8 sa[32];
    u8 sb[32];
    u8 diff = 0;
    usize i;

    fe_tobytes(sa, a);
    fe_tobytes(sb, b);
    for (i = 0; i < 32u; i++) {
        diff |= (u8)(sa[i] ^ sb[i]);
    }
    secure_bzero(sa, sizeof(sa));
    secure_bzero(sb, sizeof(sb));
    return diff == 0 ? 1 : 0;
}

static int fe_iszero(const struct fe *f) {
    u8 s[32];
    u8 diff = 0;
    usize i;

    fe_tobytes(s, f);
    for (i = 0; i < 32u; i++) {
        diff |= s[i];
    }
    secure_bzero(s, sizeof(s));
    return diff == 0 ? 1 : 0;
}

static int fe_isnegative(const struct fe *f) {
    u8 s[32];
    fe_tobytes(s, f);
    return s[0] & 1u;
}

static void fe_cmov(struct fe *dst, const struct fe *src, u32 move) {
    u64 mask = (u64)0 - (u64)(move & 1u);
    usize i = 0;

    for (i = 0; i < 5u; i++) {
        dst->v[i] = (dst->v[i] & ~mask) | (src->v[i] & mask);
    }
}

static void ge_identity(struct ge_p3 *p) {
    fe_0(&p->X);
    fe_1(&p->Y);
    fe_1(&p->Z);
    fe_0(&p->T);
}

static void ge_cmov(struct ge_p3 *dst, const struct ge_p3 *src, u32 move) {
    fe_cmov(&dst->X, &src->X, move);
    fe_cmov(&dst->Y, &src->Y, move);
    fe_cmov(&dst->Z, &src->Z, move);
    fe_cmov(&dst->T, &src->T, move);
}

static void ge_add(struct ge_p3 *r, const struct ge_p3 *p, const struct ge_p3 *q) {
    struct fe y1_minus_x1;
    struct fe y1_plus_x1;
    struct fe y2_minus_x2;
    struct fe y2_plus_x2;
    struct fe a;
    struct fe b;
    struct fe c;
    struct fe d;
    struct fe e;
    struct fe f;
    struct fe g;
    struct fe h;

    fe_sub(&y1_minus_x1, &p->Y, &p->X);
    fe_add(&y1_plus_x1, &p->Y, &p->X);
    fe_sub(&y2_minus_x2, &q->Y, &q->X);
    fe_add(&y2_plus_x2, &q->Y, &q->X);
    fe_mul(&a, &y1_minus_x1, &y2_minus_x2);
    fe_mul(&b, &y1_plus_x1, &y2_plus_x2);
    fe_mul(&c, &p->T, &q->T);
    fe_mul(&c, &c, &g_ed25519_d);
    fe_mul_small(&c, &c, 2u);
    fe_mul(&d, &p->Z, &q->Z);
    fe_mul_small(&d, &d, 2u);
    fe_sub(&e, &b, &a);
    fe_sub(&f, &d, &c);
    fe_add(&g, &d, &c);
    fe_add(&h, &b, &a);
    fe_mul(&r->X, &e, &f);
    fe_mul(&r->Y, &g, &h);
    fe_mul(&r->T, &e, &h);
    fe_mul(&r->Z, &f, &g);
}

static void ge_double(struct ge_p3 *r, const struct ge_p3 *p) {
    struct fe a;
    struct fe b;
    struct fe c;
    struct fe d;
    struct fe e;
    struct fe f;
    struct fe g;
    struct fe h;
    struct fe x_plus_y;

    fe_sq(&a, &p->X);
    fe_sq(&b, &p->Y);
    fe_sq(&c, &p->Z);
    fe_mul_small(&c, &c, 2u);
    fe_neg(&d, &a);
    fe_add(&x_plus_y, &p->X, &p->Y);
    fe_sq(&e, &x_plus_y);
    fe_sub(&e, &e, &a);
    fe_sub(&e, &e, &b);
    fe_add(&g, &d, &b);
    fe_sub(&f, &g, &c);
    fe_sub(&h, &d, &b);
    fe_mul(&r->X, &e, &f);
    fe_mul(&r->Y, &g, &h);
    fe_mul(&r->T, &e, &h);
    fe_mul(&r->Z, &f, &g);
}

static int ge_has_small_order(const struct ge_p3 *p);

static int ge_frombytes(struct ge_p3 *h, const u8 s[32]) {
    u8 y_bytes[32];
    struct fe y2;
    struct fe u;
    struct fe v;
    struct fe v3;
    struct fe x;
    struct fe check;
    struct fe neg_u;
    struct fe one;
    usize i;
    u8 sign = s[31] >> 7;

    for (i = 0; i < 32u; i++) {
        y_bytes[i] = s[i];
    }
    y_bytes[31] &= 0x7fu;

    for (i = 32u; i > 0u; i--) {
        u8 yb = y_bytes[i - 1u];
        u8 pb = g_field_prime[i - 1u];
        if (yb < pb) {
            break;
        }
        if (yb > pb) {
            return -1;
        }
        if (i == 1u) {
            return -1;
        }
    }

    fe_frombytes(&h->Y, y_bytes);
    fe_1(&h->Z);
    fe_1(&one);

    fe_sq(&y2, &h->Y);
    fe_sub(&u, &y2, &one);
    fe_mul(&v, &y2, &g_ed25519_d);
    fe_add(&v, &v, &one);

    fe_sq(&v3, &v);
    fe_mul(&v3, &v3, &v);
    fe_sq(&x, &v3);
    fe_mul(&x, &x, &v);
    fe_mul(&x, &x, &u);
    fe_pow22523(&x, &x);
    fe_mul(&x, &x, &v3);
    fe_mul(&x, &x, &u);

    fe_sq(&check, &x);
    fe_mul(&check, &check, &v);
    if (!fe_equal(&check, &u)) {
        fe_neg(&neg_u, &u);
        if (!fe_equal(&check, &neg_u)) {
            fe_mul(&x, &x, &g_ed25519_sqrtm1);
            fe_sq(&check, &x);
            fe_mul(&check, &check, &v);
            if (!fe_equal(&check, &u)) {
                return -1;
            }
        }
    }

    if (fe_iszero(&x) && sign != 0) {
        return -1;
    }
    if (fe_isnegative(&x) != sign) {
        fe_neg(&x, &x);
    }

    fe_copy(&h->X, &x);
    fe_mul(&h->T, &h->X, &h->Y);
    if (ge_has_small_order(h)) {
        return -1;
    }
    return 0;
}

static void ge_tobytes(u8 out[32], const struct ge_p3 *p) {
    struct fe inv_z;
    struct fe x;
    struct fe y;

    fe_inv(&inv_z, &p->Z);
    fe_mul(&x, &p->X, &inv_z);
    fe_mul(&y, &p->Y, &inv_z);
    fe_tobytes(out, &y);
    out[31] ^= (u8)(fe_isnegative(&x) << 7);
}

static int ge_is_identity(const struct ge_p3 *p) {
    return fe_iszero(&p->X) && fe_equal(&p->Y, &p->Z);
}

static int ge_has_small_order(const struct ge_p3 *p) {
    struct ge_p3 q;
    int i;
    int is_small_order;

    q = *p;
    for (i = 0; i < 3; i++) {
        ge_double(&q, &q);
    }
    is_small_order = ge_is_identity(&q);
    secure_bzero(&q, sizeof(q));
    return is_small_order;
}

static void ge_scalarmult(struct ge_p3 *r, const struct ge_p3 *p, const u8 scalar[32]) {
    struct ge_p3 result;
    struct ge_p3 tmp;
    int bit;

    ge_identity(&result);
    for (bit = 255; bit >= 0; bit--) {
        u32 do_add = (u32)((scalar[bit / 8] >> (bit & 7)) & 1u);
        ge_double(&result, &result);
        ge_add(&tmp, &result, p);
        ge_cmov(&result, &tmp, do_add);
    }
    *r = result;
    secure_bzero(&tmp, sizeof(tmp));
}

static int ge_scalarmult_base(struct ge_p3 *r, const u8 scalar[32]) {
    struct ge_p3 base;
    if (ge_frombytes(&base, g_basepoint_bytes) != 0) {
        return -1;
    }
    ge_scalarmult(r, &base, scalar);
    return 0;
}

static int scalar_lt_l(const u8 scalar[32]) {
    int i;
    for (i = 31; i >= 0; i--) {
        if (scalar[i] < g_group_order[i]) return 1;
        if (scalar[i] > g_group_order[i]) return 0;
    }
    return 0;
}

static int scalar_ge_l(const u8 scalar[32]) {
    return !scalar_lt_l(scalar);
}

static void scalar_sub_l(u8 scalar[32]) {
    unsigned borrow = 0;
    usize i;

    for (i = 0; i < 32u; i++) {
        unsigned value = (unsigned)scalar[i];
        unsigned sub = (unsigned)g_group_order[i] + borrow;
        if (value < sub) {
            scalar[i] = (u8)(value + 256u - sub);
            borrow = 1;
        } else {
            scalar[i] = (u8)(value - sub);
            borrow = 0;
        }
    }
}

static void scalar_shl1(u8 scalar[32]) {
    unsigned carry = 0;
    usize i;

    for (i = 0; i < 32u; i++) {
        unsigned next = scalar[i] >> 7;
        scalar[i] = (u8)((scalar[i] << 1) | carry);
        carry = next;
    }
}

static void scalar_reduce512(u8 out[32], const u8 in[64]) {
    int bit;
    usize i;

    for (i = 0; i < 32u; i++) {
        out[i] = 0;
    }

    for (bit = 511; bit >= 0; bit--) {
        scalar_shl1(out);
        out[0] |= (u8)((in[bit / 8] >> (bit & 7)) & 1u);
        if (scalar_ge_l(out)) {
            scalar_sub_l(out);
        }
    }
}

static void scalar_clamp(u8 scalar[32]) {
    scalar[0] &= 248u;
    scalar[31] &= 63u;
    scalar[31] |= 64u;
}

static void scalar_muladd(u8 out[32], const u8 a[32], const u8 b[32], const u8 c[32]) {
    u32 acc[65];
    u8 wide[64];
    u32 carry = 0;
    usize i;
    usize j;

    for (i = 0; i < 65u; i++) {
        acc[i] = 0u;
    }

    for (i = 0; i < 32u; i++) {
        for (j = 0; j < 32u; j++) {
            acc[i + j] += (u32)a[i] * (u32)b[j];
        }
    }
    for (i = 0; i < 32u; i++) {
        acc[i] += (u32)c[i];
    }

    for (i = 0; i < 64u; i++) {
        u32 v = acc[i] + carry;
        wide[i] = (u8)(v & 0xffu);
        carry = v >> 8;
    }

    scalar_reduce512(out, wide);
    secure_bzero(acc, sizeof(acc));
    secure_bzero(wide, sizeof(wide));
}

int ed25519_public_key_from_seed(u8 public_key[32], const u8 seed[32]) {
    struct sha512_ctx ctx;
    struct ge_p3 point;
    u8 expanded[64];
    int ok = -1;

    sha512_init(&ctx);
    sha512_update(&ctx, seed, 32u);
    sha512_final(&ctx, expanded);
    scalar_clamp(expanded);

    if (ge_scalarmult_base(&point, expanded) != 0) {
        goto cleanup;
    }
    ge_tobytes(public_key, &point);
    ok = 0;

cleanup:
    secure_bzero(&ctx, sizeof(ctx));
    secure_bzero(&point, sizeof(point));
    secure_bzero(expanded, sizeof(expanded));
    return ok;
}

int ed25519_sign(u8 signature[64], const u8 *message, usize message_len, const u8 seed[32], const u8 public_key[32]) {
    struct sha512_ctx ctx;
    struct ge_p3 r_point;
    u8 expanded[64];
    u8 derived_public_key[32];
    u8 nonce_digest[64];
    u8 nonce_scalar[32];
    u8 hram_digest[64];
    u8 hram_scalar[32];
    u8 secret_scalar[32];
    int ok = -1;
    usize i;

    sha512_init(&ctx);
    sha512_update(&ctx, seed, 32u);
    sha512_final(&ctx, expanded);
    for (i = 0; i < 32u; i++) {
        secret_scalar[i] = expanded[i];
    }
    scalar_clamp(secret_scalar);

    if (public_key == (const u8 *)0) {
        if (ed25519_public_key_from_seed(derived_public_key, seed) != 0) {
            goto cleanup;
        }
        public_key = derived_public_key;
    }

    sha512_init(&ctx);
    sha512_update(&ctx, expanded + 32u, 32u);
    sha512_update(&ctx, message, message_len);
    sha512_final(&ctx, nonce_digest);
    scalar_reduce512(nonce_scalar, nonce_digest);

    if (ge_scalarmult_base(&r_point, nonce_scalar) != 0) {
        goto cleanup;
    }
    ge_tobytes(signature, &r_point);

    sha512_init(&ctx);
    sha512_update(&ctx, signature, 32u);
    sha512_update(&ctx, public_key, 32u);
    sha512_update(&ctx, message, message_len);
    sha512_final(&ctx, hram_digest);
    scalar_reduce512(hram_scalar, hram_digest);

    scalar_muladd(signature + 32u, hram_scalar, secret_scalar, nonce_scalar);
    ok = 0;

cleanup:
    secure_bzero(&ctx, sizeof(ctx));
    secure_bzero(&r_point, sizeof(r_point));
    secure_bzero(expanded, sizeof(expanded));
    secure_bzero(derived_public_key, sizeof(derived_public_key));
    secure_bzero(nonce_digest, sizeof(nonce_digest));
    secure_bzero(nonce_scalar, sizeof(nonce_scalar));
    secure_bzero(hram_digest, sizeof(hram_digest));
    secure_bzero(hram_scalar, sizeof(hram_scalar));
    secure_bzero(secret_scalar, sizeof(secret_scalar));
    return ok;
}

SSHLAB_CRYPTO_SPEED int ed25519_verify(const u8 signature[64], const u8 *message, usize message_len, const u8 public_key[32]) {
    struct ge_p3 pub;
    struct ge_p3 r_point;
    struct ge_p3 sb;
    struct ge_p3 hpub;
    struct ge_p3 rhs;
    struct sha512_ctx ctx;
    u8 digest[64];
    u8 h_scalar[32];
    u8 lhs_bytes[32];
    u8 rhs_bytes[32];
    u8 diff = 0;
    int ok = -1;
    usize i;

    if (!scalar_lt_l(signature + 32)) {
        goto cleanup;
    }
    if (ge_frombytes(&pub, public_key) != 0) {
        goto cleanup;
    }
    if (ge_frombytes(&r_point, signature) != 0) {
        goto cleanup;
    }

    sha512_init(&ctx);
    sha512_update(&ctx, signature, 32u);
    sha512_update(&ctx, public_key, 32u);
    sha512_update(&ctx, message, message_len);
    sha512_final(&ctx, digest);
    scalar_reduce512(h_scalar, digest);

    if (ge_scalarmult_base(&sb, signature + 32) != 0) {
        goto cleanup;
    }
    ge_scalarmult(&hpub, &pub, h_scalar);
    ge_add(&rhs, &r_point, &hpub);

    ge_tobytes(lhs_bytes, &sb);
    ge_tobytes(rhs_bytes, &rhs);
    for (i = 0; i < 32u; i++) {
        diff |= (u8)(lhs_bytes[i] ^ rhs_bytes[i]);
    }
    ok = diff == 0 ? 0 : -1;

cleanup:
    secure_bzero(&pub, sizeof(pub));
    secure_bzero(&r_point, sizeof(r_point));
    secure_bzero(&sb, sizeof(sb));
    secure_bzero(&hpub, sizeof(hpub));
    secure_bzero(&rhs, sizeof(rhs));
    secure_bzero(&ctx, sizeof(ctx));
    secure_bzero(digest, sizeof(digest));
    secure_bzero(h_scalar, sizeof(h_scalar));
    secure_bzero(lhs_bytes, sizeof(lhs_bytes));
    secure_bzero(rhs_bytes, sizeof(rhs_bytes));
    return ok;
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
