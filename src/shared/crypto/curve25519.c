#include "crypto/curve25519.h"
#include "crypto/crypto_util.h"

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
#define x25519_scalarmult crypto_x25519_scalarmult
#define x25519_scalarmult_base crypto_x25519_scalarmult_base

#define MASK51 ((u64)((1ULL << 51) - 1ULL))

struct fe {
    u64 v[5];
};

static const u8 g_x25519_basepoint[32] = { 9 };

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

static SSHLAB_FORCE_INLINE void fe_0(struct fe *f) {
    f->v[0] = 0;
    f->v[1] = 0;
    f->v[2] = 0;
    f->v[3] = 0;
    f->v[4] = 0;
}

static SSHLAB_FORCE_INLINE void fe_1(struct fe *f) {
    fe_0(f);
    f->v[0] = 1;
}

static SSHLAB_FORCE_INLINE void fe_copy(struct fe *dst, const struct fe *src) {
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

static SSHLAB_FORCE_INLINE void fe_add(struct fe *h, const struct fe *f, const struct fe *g) {
    h->v[0] = f->v[0] + g->v[0];
    h->v[1] = f->v[1] + g->v[1];
    h->v[2] = f->v[2] + g->v[2];
    h->v[3] = f->v[3] + g->v[3];
    h->v[4] = f->v[4] + g->v[4];
}

static SSHLAB_FORCE_INLINE void fe_sub(struct fe *h, const struct fe *f, const struct fe *g) {
    h->v[0] = f->v[0] + (MASK51 - 18u) * 2u - g->v[0];
    h->v[1] = f->v[1] + MASK51 * 2u - g->v[1];
    h->v[2] = f->v[2] + MASK51 * 2u - g->v[2];
    h->v[3] = f->v[3] + MASK51 * 2u - g->v[3];
    h->v[4] = f->v[4] + MASK51 * 2u - g->v[4];
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

    carry = (u64)(h0 >> 51);
    h0 &= MASK51;
    h1 += carry;

    carry = (u64)(h1 >> 51);
    h1 &= MASK51;
    h2 += carry;

    carry = (u64)(h2 >> 51);
    h2 &= MASK51;
    h3 += carry;

    carry = (u64)(h3 >> 51);
    h3 &= MASK51;
    h4 += carry;

    carry = (u64)(h4 >> 51);
    h4 &= MASK51;
    h0 += (unsigned __int128)carry * 19u;

    carry = (u64)(h0 >> 51);
    h0 &= MASK51;
    h1 += carry;

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

    carry = (u64)(h0 >> 51);
    h0 &= MASK51;
    h1 += carry;

    carry = (u64)(h1 >> 51);
    h1 &= MASK51;
    h2 += carry;

    carry = (u64)(h2 >> 51);
    h2 &= MASK51;
    h3 += carry;

    carry = (u64)(h3 >> 51);
    h3 &= MASK51;
    h4 += carry;

    carry = (u64)(h4 >> 51);
    h4 &= MASK51;
    h0 += (unsigned __int128)carry * 19u;

    carry = (u64)(h0 >> 51);
    h0 &= MASK51;
    h1 += carry;

    h->v[0] = (u64)h0;
    h->v[1] = (u64)h1;
    h->v[2] = (u64)h2;
    h->v[3] = (u64)h3;
    h->v[4] = (u64)h4;
}

static SSHLAB_FORCE_INLINE void fe_cswap(struct fe *a, struct fe *b, u32 swap) {
    u64 mask = (u64)(-(long)swap);
    u64 i;

    for (i = 0; i < 5; i++) {
        u64 t = mask & (a->v[i] ^ b->v[i]);
        a->v[i] ^= t;
        b->v[i] ^= t;
    }
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

static int bytes_are_all_zero(const u8 *buf, usize len) {
    u8 diff = 0;
    usize i = 0;

    while (i < len) {
        diff |= buf[i++];
    }
    return diff == 0 ? 1 : 0;
}

int x25519_scalarmult(u8 out[32], const u8 scalar[32], const u8 point[32]) {
    u8 e[32];
    struct fe x1;
    struct fe x2;
    struct fe z2;
    struct fe x3;
    struct fe z3;
    struct fe a;
    struct fe aa;
    struct fe b;
    struct fe bb;
    struct fe e_diff;
    struct fe c;
    struct fe d;
    struct fe da;
    struct fe cb;
    struct fe t0;
    struct fe t1;
    u32 swap = 0;
    int status = 0;
    int pos;

    for (pos = 0; pos < 32; pos++) {
        e[pos] = scalar[pos];
    }
    e[0] &= 248u;
    e[31] &= 127u;
    e[31] |= 64u;

    fe_frombytes(&x1, point);
    fe_1(&x2);
    fe_0(&z2);
    fe_copy(&x3, &x1);
    fe_1(&z3);

    for (pos = 254; pos >= 0; pos--) {
        u32 bit = (u32)((e[pos / 8] >> (pos & 7)) & 1u);
        swap ^= bit;
        fe_cswap(&x2, &x3, swap);
        fe_cswap(&z2, &z3, swap);
        swap = bit;

        fe_add(&a, &x2, &z2);
        fe_sub(&b, &x2, &z2);
        fe_sq(&aa, &a);
        fe_sq(&bb, &b);
        fe_sub(&e_diff, &aa, &bb);
        fe_add(&c, &x3, &z3);
        fe_sub(&d, &x3, &z3);
        fe_mul(&da, &d, &a);
        fe_mul(&cb, &c, &b);

        fe_add(&t0, &da, &cb);
        fe_sq(&x3, &t0);
        fe_sub(&t1, &da, &cb);
        fe_sq(&t1, &t1);
        fe_mul(&z3, &t1, &x1);
        fe_mul(&x2, &aa, &bb);
        fe_mul_small(&t0, &e_diff, 121665u);
        fe_add(&t0, &t0, &aa);
        fe_mul(&z2, &e_diff, &t0);
    }

    fe_cswap(&x2, &x3, swap);
    fe_cswap(&z2, &z3, swap);
    fe_inv(&z2, &z2);
    fe_mul(&x2, &x2, &z2);
    fe_tobytes(out, &x2);
    status = bytes_are_all_zero(out, 32u) ? -1 : 0;

    secure_bzero(e, sizeof(e));
    secure_bzero(&x1, sizeof(x1));
    secure_bzero(&x2, sizeof(x2));
    secure_bzero(&z2, sizeof(z2));
    secure_bzero(&x3, sizeof(x3));
    secure_bzero(&z3, sizeof(z3));
    secure_bzero(&a, sizeof(a));
    secure_bzero(&aa, sizeof(aa));
    secure_bzero(&b, sizeof(b));
    secure_bzero(&bb, sizeof(bb));
    secure_bzero(&e_diff, sizeof(e_diff));
    secure_bzero(&c, sizeof(c));
    secure_bzero(&d, sizeof(d));
    secure_bzero(&da, sizeof(da));
    secure_bzero(&cb, sizeof(cb));
    secure_bzero(&t0, sizeof(t0));
    secure_bzero(&t1, sizeof(t1));
    return status;
}

int x25519_scalarmult_base(u8 out[32], const u8 scalar[32]) {
    return x25519_scalarmult(out, scalar, g_x25519_basepoint);
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
