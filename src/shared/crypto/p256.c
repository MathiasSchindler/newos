#include "crypto/p256.h"
#include "crypto/crypto_util.h"

#include <stddef.h>

#define P256_WORDS 8U

typedef struct {
    unsigned int word[P256_WORDS];
} P256Int;

typedef struct {
    P256Int x;
    P256Int y;
    P256Int z;
    int infinity;
} P256Jacobian;

static const P256Int P256_ONE = {{1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}};
static const P256Int P256_THREE = {{3U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}};

static const P256Int P256_P = {{
    0xffffffffU, 0xffffffffU, 0xffffffffU, 0x00000000U,
    0x00000000U, 0x00000000U, 0x00000001U, 0xffffffffU
}};

static const P256Int P256_N = {{
    0xfc632551U, 0xf3b9cac2U, 0xa7179e84U, 0xbce6faadU,
    0xffffffffU, 0xffffffffU, 0x00000000U, 0xffffffffU
}};

static const P256Int P256_R_P = {{
    0x00000001U, 0x00000000U, 0x00000000U, 0xffffffffU,
    0xffffffffU, 0xffffffffU, 0xfffffffeU, 0x00000000U
}};

static const P256Int P256_R_N = {{
    0x039cdaafU, 0x0c46353dU, 0x58e8617bU, 0x43190552U,
    0x00000000U, 0x00000000U, 0xffffffffU, 0x00000000U
}};

static const P256Int P256_B = {{
    0x27d2604bU, 0x3bce3c3eU, 0xcc53b0f6U, 0x651d06b0U,
    0x769886bcU, 0xb3ebbd55U, 0xaa3a93e7U, 0x5ac635d8U
}};

static const P256Int P256_GX = {{
    0xd898c296U, 0xf4a13945U, 0x2deb33a0U, 0x77037d81U,
    0x63a440f2U, 0xf8bce6e5U, 0xe12c4247U, 0x6b17d1f2U
}};

static const P256Int P256_GY = {{
    0x37bf51f5U, 0xcbb64068U, 0x6b315eceU, 0x2bce3357U,
    0x7c0f9e16U, 0x8ee7eb4aU, 0xfe1a7f9bU, 0x4fe342e2U
}};

static void p256_copy(P256Int *out, const P256Int *in) {
    size_t i;
    for (i = 0U; i < P256_WORDS; ++i) out->word[i] = in->word[i];
}

static void p256_zero(P256Int *out) {
    size_t i;
    for (i = 0U; i < P256_WORDS; ++i) out->word[i] = 0U;
}

static int p256_is_zero(const P256Int *value) {
    size_t i;
    unsigned int any = 0U;
    for (i = 0U; i < P256_WORDS; ++i) any |= value->word[i];
    return any == 0U;
}

static int p256_cmp(const P256Int *left, const P256Int *right) {
    size_t i = P256_WORDS;
    while (i > 0U) {
        i -= 1U;
        if (left->word[i] > right->word[i]) return 1;
        if (left->word[i] < right->word[i]) return -1;
    }
    return 0;
}

static unsigned int p256_add_raw(P256Int *out, const P256Int *left, const P256Int *right) {
    unsigned long long carry = 0ULL;
    size_t i;
    for (i = 0U; i < P256_WORDS; ++i) {
        unsigned long long sum = (unsigned long long)left->word[i] + (unsigned long long)right->word[i] + carry;
        out->word[i] = (unsigned int)sum;
        carry = sum >> 32U;
    }
    return (unsigned int)carry;
}

static unsigned int p256_sub_raw(P256Int *out, const P256Int *left, const P256Int *right) {
    unsigned long long borrow = 0ULL;
    size_t i;
    for (i = 0U; i < P256_WORDS; ++i) {
        unsigned long long lv = (unsigned long long)left->word[i];
        unsigned long long rv = (unsigned long long)right->word[i] + borrow;
        if (lv >= rv) {
            out->word[i] = (unsigned int)(lv - rv);
            borrow = 0ULL;
        } else {
            out->word[i] = (unsigned int)((1ULL << 32U) + lv - rv);
            borrow = 1ULL;
        }
    }
    return (unsigned int)borrow;
}

static void p256_mod_reduce_once(P256Int *value, const P256Int *modulus) {
    P256Int tmp;
    if (p256_cmp(value, modulus) >= 0) {
        (void)p256_sub_raw(&tmp, value, modulus);
        p256_copy(value, &tmp);
    }
}

static void p256_mod_add(P256Int *out, const P256Int *left, const P256Int *right, const P256Int *modulus) {
    P256Int tmp;
    unsigned int carry = p256_add_raw(&tmp, left, right);
    while (carry != 0U) {
        const P256Int *reduction = modulus == &P256_P ? &P256_R_P : &P256_R_N;
        carry = p256_add_raw(&tmp, &tmp, reduction);
    }
    while (p256_cmp(&tmp, modulus) >= 0) {
        (void)p256_sub_raw(&tmp, &tmp, modulus);
    }
    p256_copy(out, &tmp);
}

static void p256_mod_sub(P256Int *out, const P256Int *left, const P256Int *right, const P256Int *modulus) {
    P256Int tmp;
    if (p256_sub_raw(&tmp, left, right) != 0U) {
        (void)p256_add_raw(&tmp, &tmp, modulus);
    }
    p256_copy(out, &tmp);
}

static void p256_mod_double(P256Int *out, const P256Int *value, const P256Int *modulus) {
    p256_mod_add(out, value, value, modulus);
}

static int p256_get_bit(const P256Int *value, unsigned int bit) {
    return (int)((value->word[bit >> 5U] >> (bit & 31U)) & 1U);
}

static void p256_mod_mul(P256Int *out, const P256Int *left, const P256Int *right, const P256Int *modulus) {
    P256Int result;
    P256Int addend;
    unsigned int bit;

    p256_zero(&result);
    p256_copy(&addend, left);
    p256_mod_reduce_once(&addend, modulus);
    for (bit = 0U; bit < 256U; ++bit) {
        if (p256_get_bit(right, bit)) {
            p256_mod_add(&result, &result, &addend, modulus);
        }
        p256_mod_double(&addend, &addend, modulus);
    }
    p256_copy(out, &result);
}

static void p256_mod_square(P256Int *out, const P256Int *value, const P256Int *modulus) {
    p256_mod_mul(out, value, value, modulus);
}

static void p256_mod_pow(P256Int *out, const P256Int *base, const P256Int *exponent, const P256Int *modulus) {
    P256Int result;
    P256Int factor;
    int bit;

    p256_copy(&result, &P256_ONE);
    p256_copy(&factor, base);
    p256_mod_reduce_once(&factor, modulus);
    for (bit = 255; bit >= 0; --bit) {
        p256_mod_square(&result, &result, modulus);
        if (p256_get_bit(exponent, (unsigned int)bit)) {
            p256_mod_mul(&result, &result, &factor, modulus);
        }
    }
    p256_copy(out, &result);
}

static void p256_mod_inverse(P256Int *out, const P256Int *value, const P256Int *modulus) {
    P256Int exponent;
    p256_copy(&exponent, modulus);
    exponent.word[0] -= 2U;
    p256_mod_pow(out, value, &exponent, modulus);
}

static void p256_from_be(P256Int *out, const unsigned char bytes[32]) {
    size_t i;
    p256_zero(out);
    for (i = 0U; i < 32U; ++i) {
        size_t word_index = i >> 2U;
        out->word[word_index] |= (unsigned int)bytes[31U - i] << ((i & 3U) * 8U);
    }
}

static void field_add(P256Int *out, const P256Int *left, const P256Int *right) { p256_mod_add(out, left, right, &P256_P); }
static void field_sub(P256Int *out, const P256Int *left, const P256Int *right) { p256_mod_sub(out, left, right, &P256_P); }
static void field_mul(P256Int *out, const P256Int *left, const P256Int *right) { p256_mod_mul(out, left, right, &P256_P); }
static void field_square(P256Int *out, const P256Int *value) { p256_mod_square(out, value, &P256_P); }
static void field_inv(P256Int *out, const P256Int *value) { p256_mod_inverse(out, value, &P256_P); }
static void scalar_mul(P256Int *out, const P256Int *left, const P256Int *right) { p256_mod_mul(out, left, right, &P256_N); }
static void scalar_inv(P256Int *out, const P256Int *value) { p256_mod_inverse(out, value, &P256_N); }

static int p256_int_equal(const P256Int *left, const P256Int *right) {
    return p256_cmp(left, right) == 0;
}

static int p256_point_is_on_curve(const P256Int *x, const P256Int *y) {
    P256Int lhs;
    P256Int rhs;
    P256Int tmp;

    field_square(&lhs, y);
    field_square(&rhs, x);
    field_mul(&rhs, &rhs, x);
    field_mul(&tmp, &P256_THREE, x);
    field_sub(&rhs, &rhs, &tmp);
    field_add(&rhs, &rhs, &P256_B);
    return p256_int_equal(&lhs, &rhs);
}

static void p256_jacobian_set_infinity(P256Jacobian *point) {
    p256_zero(&point->x);
    p256_zero(&point->y);
    p256_zero(&point->z);
    point->infinity = 1;
}

static void p256_jacobian_set_affine(P256Jacobian *point, const P256Int *x, const P256Int *y) {
    p256_copy(&point->x, x);
    p256_copy(&point->y, y);
    p256_copy(&point->z, &P256_ONE);
    point->infinity = 0;
}

static void p256_point_double(P256Jacobian *out, const P256Jacobian *point) {
    P256Int xx;
    P256Int yy;
    P256Int yyyy;
    P256Int zz;
    P256Int s;
    P256Int m;
    P256Int t;
    P256Int tmp1;
    P256Int tmp2;

    if (point->infinity || p256_is_zero(&point->y)) {
        p256_jacobian_set_infinity(out);
        return;
    }

    field_square(&xx, &point->x);
    field_square(&yy, &point->y);
    field_square(&yyyy, &yy);
    field_square(&zz, &point->z);

    field_add(&tmp1, &point->x, &yy);
    field_square(&tmp1, &tmp1);
    field_sub(&tmp1, &tmp1, &xx);
    field_sub(&tmp1, &tmp1, &yyyy);
    field_add(&s, &tmp1, &tmp1);

    field_sub(&tmp2, &point->x, &zz);
    field_add(&tmp1, &point->x, &zz);
    field_mul(&m, &tmp1, &tmp2);
    field_mul(&m, &m, &P256_THREE);

    field_square(&t, &m);
    field_sub(&t, &t, &s);
    field_sub(&t, &t, &s);
    p256_copy(&out->x, &t);

    field_sub(&tmp1, &s, &t);
    field_mul(&tmp1, &m, &tmp1);
    field_add(&tmp2, &yyyy, &yyyy);
    field_add(&tmp2, &tmp2, &tmp2);
    field_add(&tmp2, &tmp2, &tmp2);
    field_sub(&out->y, &tmp1, &tmp2);

    field_add(&tmp1, &point->y, &point->z);
    field_square(&tmp1, &tmp1);
    field_sub(&tmp1, &tmp1, &yy);
    field_sub(&out->z, &tmp1, &zz);
    out->infinity = 0;
}

static void p256_point_add_mixed(P256Jacobian *out, const P256Jacobian *point, const P256Int *x2, const P256Int *y2) {
    P256Int z1z1;
    P256Int u2;
    P256Int s2;
    P256Int h;
    P256Int hh;
    P256Int i;
    P256Int j;
    P256Int r;
    P256Int v;
    P256Int tmp1;
    P256Int tmp2;

    if (point->infinity) {
        p256_jacobian_set_affine(out, x2, y2);
        return;
    }

    field_square(&z1z1, &point->z);
    field_mul(&u2, x2, &z1z1);
    field_mul(&s2, &point->z, &z1z1);
    field_mul(&s2, y2, &s2);
    field_sub(&h, &u2, &point->x);
    field_sub(&r, &s2, &point->y);
    if (p256_is_zero(&h)) {
        if (p256_is_zero(&r)) {
            p256_point_double(out, point);
        } else {
            p256_jacobian_set_infinity(out);
        }
        return;
    }

    field_add(&r, &r, &r);
    field_square(&hh, &h);
    field_add(&i, &hh, &hh);
    field_add(&i, &i, &i);
    field_mul(&j, &h, &i);
    field_mul(&v, &point->x, &i);

    field_square(&out->x, &r);
    field_sub(&out->x, &out->x, &j);
    field_sub(&out->x, &out->x, &v);
    field_sub(&out->x, &out->x, &v);

    field_sub(&tmp1, &v, &out->x);
    field_mul(&tmp1, &r, &tmp1);
    field_mul(&tmp2, &point->y, &j);
    field_add(&tmp2, &tmp2, &tmp2);
    field_sub(&out->y, &tmp1, &tmp2);

    field_add(&tmp1, &point->z, &h);
    field_square(&tmp1, &tmp1);
    field_sub(&tmp1, &tmp1, &z1z1);
    field_sub(&out->z, &tmp1, &hh);
    out->infinity = 0;
}

static int p256_double_scalar_mult_affine(P256Int *out_x,
                                          P256Int *out_y,
                                          const P256Int *left_scalar,
                                          const P256Int *left_x,
                                          const P256Int *left_y,
                                          const P256Int *right_scalar,
                                          const P256Int *right_x,
                                          const P256Int *right_y) {
    P256Jacobian result;
    P256Jacobian next;
    P256Int z_inv;
    P256Int z2;
    P256Int z3;
    int bit;

    p256_jacobian_set_infinity(&result);
    for (bit = 255; bit >= 0; --bit) {
        p256_point_double(&next, &result);
        result = next;
        if (p256_get_bit(left_scalar, (unsigned int)bit)) {
            p256_point_add_mixed(&next, &result, left_x, left_y);
            result = next;
        }
        if (p256_get_bit(right_scalar, (unsigned int)bit)) {
            p256_point_add_mixed(&next, &result, right_x, right_y);
            result = next;
        }
    }
    if (result.infinity) {
        return 0;
    }
    field_inv(&z_inv, &result.z);
    field_square(&z2, &z_inv);
    field_mul(&z3, &z2, &z_inv);
    field_mul(out_x, &result.x, &z2);
    field_mul(out_y, &result.y, &z3);
    return 1;
}

int crypto_p256_ecdsa_sha256_verify(
    const unsigned char public_key[CRYPTO_P256_UNCOMPRESSED_PUBLIC_KEY_SIZE],
    const unsigned char digest[32],
    const unsigned char signature[CRYPTO_P256_ECDSA_SIGNATURE_SIZE]
) {
    P256Int qx;
    P256Int qy;
    P256Int r;
    P256Int s;
    P256Int z;
    P256Int w;
    P256Int u1;
    P256Int u2;
    P256Int rx;
    P256Int ry;

    if (public_key == 0 || digest == 0 || signature == 0 || public_key[0] != 0x04U) {
        return 0;
    }
    p256_from_be(&qx, public_key + 1U);
    p256_from_be(&qy, public_key + 33U);
    p256_from_be(&r, signature);
    p256_from_be(&s, signature + 32U);
    p256_from_be(&z, digest);

    if (p256_cmp(&qx, &P256_P) >= 0 || p256_cmp(&qy, &P256_P) >= 0 || !p256_point_is_on_curve(&qx, &qy)) {
        return 0;
    }
    if (p256_is_zero(&r) || p256_is_zero(&s) || p256_cmp(&r, &P256_N) >= 0 || p256_cmp(&s, &P256_N) >= 0) {
        return 0;
    }

    scalar_inv(&w, &s);
    scalar_mul(&u1, &z, &w);
    scalar_mul(&u2, &r, &w);

    if (!p256_double_scalar_mult_affine(&rx, &ry, &u1, &P256_GX, &P256_GY, &u2, &qx, &qy)) {
        return 0;
    }
    (void)ry;
    if (p256_cmp(&rx, &P256_N) >= 0) {
        P256Int reduced;
        (void)p256_sub_raw(&reduced, &rx, &P256_N);
        p256_copy(&rx, &reduced);
    }
    return p256_int_equal(&rx, &r);
}
