#include "crypto/p384.h"
#include "runtime.h"

#include <stddef.h>

#define P384_WORDS 12U
#define P384_BITS 384U

typedef struct {
    unsigned int word[P384_WORDS];
} P384Int;

typedef struct {
    P384Int x;
    P384Int y;
    P384Int z;
    int infinity;
} P384Jacobian;

static const P384Int P384_ONE = {{1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}};
static const P384Int P384_THREE = {{3U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}};

static const P384Int P384_P = {{
    0xffffffffU, 0x00000000U, 0x00000000U, 0xffffffffU,
    0xfffffffeU, 0xffffffffU, 0xffffffffU, 0xffffffffU,
    0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU
}};

static const P384Int P384_N = {{
    0xccc52973U, 0xecec196aU, 0x48b0a77aU, 0x581a0db2U,
    0xf4372ddfU, 0xc7634d81U, 0xffffffffU, 0xffffffffU,
    0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU
}};

static const P384Int P384_R_P = {{
    0x00000001U, 0xffffffffU, 0xffffffffU, 0x00000000U,
    0x00000001U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
}};

static const P384Int P384_R_N = {{
    0x333ad68dU, 0x1313e695U, 0xb74f5885U, 0xa7e5f24dU,
    0x0bc8d220U, 0x389cb27eU, 0x00000000U, 0x00000000U,
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
}};

static const P384Int P384_B = {{
    0xd3ec2aefU, 0x2a85c8edU, 0x8a2ed19dU, 0xc656398dU,
    0x5013875aU, 0x0314088fU, 0xfe814112U, 0x181d9c6eU,
    0xe3f82d19U, 0x988e056bU, 0xe23ee7e4U, 0xb3312fa7U
}};

static const P384Int P384_GX = {{
    0x72760ab7U, 0x3a545e38U, 0xbf55296cU, 0x5502f25dU,
    0x82542a38U, 0x59f741e0U, 0x8ba79b98U, 0x6e1d3b62U,
    0xf320ad74U, 0x8eb1c71eU, 0xbe8b0537U, 0xaa87ca22U
}};

static const P384Int P384_GY = {{
    0x90ea0e5fU, 0x7a431d7cU, 0x1d7e819dU, 0x0a60b1ceU,
    0xb5f0b8c0U, 0xe9da3113U, 0x289a147cU, 0xf8f41dbdU,
    0x9292dc29U, 0x5d9e98bfU, 0x96262c6fU, 0x3617de4aU
}};

static void p384_copy(P384Int *out, const P384Int *in) {
    size_t i;
    for (i = 0U; i < P384_WORDS; ++i) out->word[i] = in->word[i];
}

static void p384_zero(P384Int *out) {
    size_t i;
    for (i = 0U; i < P384_WORDS; ++i) out->word[i] = 0U;
}

static int p384_is_zero(const P384Int *value) {
    size_t i;
    unsigned int any = 0U;
    for (i = 0U; i < P384_WORDS; ++i) any |= value->word[i];
    return any == 0U;
}

static int p384_cmp(const P384Int *left, const P384Int *right) {
    size_t i = P384_WORDS;
    while (i > 0U) {
        i -= 1U;
        if (left->word[i] > right->word[i]) return 1;
        if (left->word[i] < right->word[i]) return -1;
    }
    return 0;
}

static unsigned int p384_add_raw(P384Int *out, const P384Int *left, const P384Int *right) {
    unsigned long long carry = 0ULL;
    size_t i;
    for (i = 0U; i < P384_WORDS; ++i) {
        unsigned long long sum = (unsigned long long)left->word[i] + (unsigned long long)right->word[i] + carry;
        out->word[i] = (unsigned int)sum;
        carry = sum >> 32U;
    }
    return (unsigned int)carry;
}

static unsigned int p384_sub_raw(P384Int *out, const P384Int *left, const P384Int *right) {
    unsigned long long borrow = 0ULL;
    size_t i;
    for (i = 0U; i < P384_WORDS; ++i) {
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

static void p384_mod_reduce_once(P384Int *value, const P384Int *modulus) {
    P384Int tmp;
    if (p384_cmp(value, modulus) >= 0) {
        (void)p384_sub_raw(&tmp, value, modulus);
        p384_copy(value, &tmp);
    }
}

static void p384_mod_add(P384Int *out, const P384Int *left, const P384Int *right, const P384Int *modulus) {
    P384Int tmp;
    unsigned int carry = p384_add_raw(&tmp, left, right);
    while (carry != 0U) {
        const P384Int *reduction = modulus == &P384_P ? &P384_R_P : &P384_R_N;
        carry = p384_add_raw(&tmp, &tmp, reduction);
    }
    while (p384_cmp(&tmp, modulus) >= 0) {
        (void)p384_sub_raw(&tmp, &tmp, modulus);
    }
    p384_copy(out, &tmp);
}

static void p384_mod_sub(P384Int *out, const P384Int *left, const P384Int *right, const P384Int *modulus) {
    P384Int tmp;
    if (p384_sub_raw(&tmp, left, right) != 0U) {
        (void)p384_add_raw(&tmp, &tmp, modulus);
    }
    p384_copy(out, &tmp);
}

static void p384_mod_double(P384Int *out, const P384Int *value, const P384Int *modulus) {
    p384_mod_add(out, value, value, modulus);
}

static int p384_get_bit(const P384Int *value, unsigned int bit) {
    return (int)((value->word[bit >> 5U] >> (bit & 31U)) & 1U);
}

static void p384_mod_mul(P384Int *out, const P384Int *left, const P384Int *right, const P384Int *modulus) {
    P384Int result;
    P384Int addend;
    unsigned int bit;

    p384_zero(&result);
    p384_copy(&addend, left);
    p384_mod_reduce_once(&addend, modulus);
    for (bit = 0U; bit < P384_BITS; ++bit) {
        if (p384_get_bit(right, bit)) {
            p384_mod_add(&result, &result, &addend, modulus);
        }
        p384_mod_double(&addend, &addend, modulus);
    }
    p384_copy(out, &result);
}

static void p384_mod_square(P384Int *out, const P384Int *value, const P384Int *modulus) {
    p384_mod_mul(out, value, value, modulus);
}

static void p384_mod_pow(P384Int *out, const P384Int *base, const P384Int *exponent, const P384Int *modulus) {
    P384Int result;
    P384Int factor;
    int bit;

    p384_copy(&result, &P384_ONE);
    p384_copy(&factor, base);
    p384_mod_reduce_once(&factor, modulus);
    for (bit = (int)P384_BITS - 1; bit >= 0; --bit) {
        p384_mod_square(&result, &result, modulus);
        if (p384_get_bit(exponent, (unsigned int)bit)) {
            p384_mod_mul(&result, &result, &factor, modulus);
        }
    }
    p384_copy(out, &result);
}

static void p384_mod_inverse(P384Int *out, const P384Int *value, const P384Int *modulus) {
    P384Int exponent;
    p384_copy(&exponent, modulus);
    exponent.word[0] -= 2U;
    p384_mod_pow(out, value, &exponent, modulus);
}

static void p384_from_be(P384Int *out, const unsigned char bytes[48]) {
    size_t i;
    p384_zero(out);
    for (i = 0U; i < 48U; ++i) {
        size_t word_index = i >> 2U;
        out->word[word_index] |= (unsigned int)bytes[47U - i] << ((i & 3U) * 8U);
    }
}

static void field_add(P384Int *out, const P384Int *left, const P384Int *right) { p384_mod_add(out, left, right, &P384_P); }
static void field_sub(P384Int *out, const P384Int *left, const P384Int *right) { p384_mod_sub(out, left, right, &P384_P); }
static void field_mul(P384Int *out, const P384Int *left, const P384Int *right) { p384_mod_mul(out, left, right, &P384_P); }
static void field_square(P384Int *out, const P384Int *value) { p384_mod_square(out, value, &P384_P); }
static void field_inv(P384Int *out, const P384Int *value) { p384_mod_inverse(out, value, &P384_P); }
static void scalar_mul(P384Int *out, const P384Int *left, const P384Int *right) { p384_mod_mul(out, left, right, &P384_N); }
static void scalar_inv(P384Int *out, const P384Int *value) { p384_mod_inverse(out, value, &P384_N); }

static int p384_int_equal(const P384Int *left, const P384Int *right) {
    return p384_cmp(left, right) == 0;
}

static int p384_point_is_on_curve(const P384Int *x, const P384Int *y) {
    P384Int lhs;
    P384Int rhs;
    P384Int tmp;

    field_square(&lhs, y);
    field_square(&rhs, x);
    field_mul(&rhs, &rhs, x);
    field_mul(&tmp, &P384_THREE, x);
    field_sub(&rhs, &rhs, &tmp);
    field_add(&rhs, &rhs, &P384_B);
    return p384_int_equal(&lhs, &rhs);
}

static void p384_jacobian_set_infinity(P384Jacobian *point) {
    p384_zero(&point->x);
    p384_zero(&point->y);
    p384_zero(&point->z);
    point->infinity = 1;
}

static void p384_jacobian_set_affine(P384Jacobian *point, const P384Int *x, const P384Int *y) {
    p384_copy(&point->x, x);
    p384_copy(&point->y, y);
    p384_copy(&point->z, &P384_ONE);
    point->infinity = 0;
}

static void p384_point_double(P384Jacobian *out, const P384Jacobian *point) {
    P384Int xx;
    P384Int yy;
    P384Int yyyy;
    P384Int zz;
    P384Int s;
    P384Int m;
    P384Int t;
    P384Int tmp1;
    P384Int tmp2;

    if (point->infinity || p384_is_zero(&point->y)) {
        p384_jacobian_set_infinity(out);
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
    field_mul(&m, &m, &P384_THREE);

    field_square(&t, &m);
    field_sub(&t, &t, &s);
    field_sub(&t, &t, &s);
    p384_copy(&out->x, &t);

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

static void p384_point_add_mixed(P384Jacobian *out, const P384Jacobian *point, const P384Int *x2, const P384Int *y2) {
    P384Int z1z1;
    P384Int u2;
    P384Int s2;
    P384Int h;
    P384Int hh;
    P384Int i;
    P384Int j;
    P384Int r;
    P384Int v;
    P384Int tmp1;
    P384Int tmp2;

    if (point->infinity) {
        p384_jacobian_set_affine(out, x2, y2);
        return;
    }

    field_square(&z1z1, &point->z);
    field_mul(&u2, x2, &z1z1);
    field_mul(&s2, &point->z, &z1z1);
    field_mul(&s2, y2, &s2);
    field_sub(&h, &u2, &point->x);
    field_sub(&r, &s2, &point->y);
    if (p384_is_zero(&h)) {
        if (p384_is_zero(&r)) {
            p384_point_double(out, point);
        } else {
            p384_jacobian_set_infinity(out);
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

static int p384_double_scalar_mult_affine(P384Int *out_x,
                                          P384Int *out_y,
                                          const P384Int *left_scalar,
                                          const P384Int *left_x,
                                          const P384Int *left_y,
                                          const P384Int *right_scalar,
                                          const P384Int *right_x,
                                          const P384Int *right_y) {
    P384Jacobian result;
    P384Jacobian next;
    P384Int z_inv;
    P384Int z2;
    P384Int z3;
    int bit;

    p384_jacobian_set_infinity(&result);
    for (bit = (int)P384_BITS - 1; bit >= 0; --bit) {
        p384_point_double(&next, &result);
        result = next;
        if (p384_get_bit(left_scalar, (unsigned int)bit)) {
            p384_point_add_mixed(&next, &result, left_x, left_y);
            result = next;
        }
        if (p384_get_bit(right_scalar, (unsigned int)bit)) {
            p384_point_add_mixed(&next, &result, right_x, right_y);
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

int crypto_p384_ecdsa_sha384_verify(
    const unsigned char public_key[CRYPTO_P384_UNCOMPRESSED_PUBLIC_KEY_SIZE],
    const unsigned char digest[48],
    const unsigned char signature[CRYPTO_P384_ECDSA_SIGNATURE_SIZE]
) {
    P384Int qx;
    P384Int qy;
    P384Int r;
    P384Int s;
    P384Int z;
    P384Int w;
    P384Int u1;
    P384Int u2;
    P384Int rx;
    P384Int ry;

    if (public_key == 0 || digest == 0 || signature == 0 || public_key[0] != 0x04U) {
        return 0;
    }
    p384_from_be(&qx, public_key + 1U);
    p384_from_be(&qy, public_key + 49U);
    p384_from_be(&r, signature);
    p384_from_be(&s, signature + 48U);
    p384_from_be(&z, digest);

    if (p384_cmp(&qx, &P384_P) >= 0 || p384_cmp(&qy, &P384_P) >= 0 || !p384_point_is_on_curve(&qx, &qy)) {
        return 0;
    }
    if (p384_is_zero(&r) || p384_is_zero(&s) || p384_cmp(&r, &P384_N) >= 0 || p384_cmp(&s, &P384_N) >= 0) {
        return 0;
    }

    scalar_inv(&w, &s);
    scalar_mul(&u1, &z, &w);
    scalar_mul(&u2, &r, &w);

    if (!p384_double_scalar_mult_affine(&rx, &ry, &u1, &P384_GX, &P384_GY, &u2, &qx, &qy)) {
        return 0;
    }
    (void)ry;
    while (p384_cmp(&rx, &P384_N) >= 0) {
        P384Int reduced;
        (void)p384_sub_raw(&reduced, &rx, &P384_N);
        p384_copy(&rx, &reduced);
    }
    return p384_int_equal(&rx, &r);
}
