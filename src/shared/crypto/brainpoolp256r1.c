#include "crypto/brainpoolp256r1.h"
#include "runtime.h"

#define BP256_WORDS 8U

typedef struct {
    unsigned int word[BP256_WORDS];
} Bp256Int;

typedef struct {
    Bp256Int x;
    Bp256Int y;
    Bp256Int z;
    int infinity;
} Bp256Jacobian;

static const Bp256Int BP256_ZERO = {{0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}};
static const Bp256Int BP256_ONE = {{1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}};
static const Bp256Int BP256_THREE = {{3U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}};
static const Bp256Int BP256_EIGHT = {{8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}};

static const Bp256Int BP256_P = {{
    0x1f6e5377U, 0x2013481dU, 0xd5262028U, 0x6e3bf623U,
    0x9d838d72U, 0x3e660a90U, 0xa1eea9bcU, 0xa9fb57dbU
}};

static const Bp256Int BP256_N = {{
    0x974856a7U, 0x901e0e82U, 0xb561a6f7U, 0x8c397aa3U,
    0x9d838d71U, 0x3e660a90U, 0xa1eea9bcU, 0xa9fb57dbU
}};

static const Bp256Int BP256_R_P = {{
    0xe091ac89U, 0xdfecb7e2U, 0x2ad9dfd7U, 0x91c409dcU,
    0x627c728dU, 0xc199f56fU, 0x5e115643U, 0x5604a824U
}};

static const Bp256Int BP256_R_N = {{
    0x68b7a959U, 0x6fe1f17dU, 0x4a9e5908U, 0x73c6855cU,
    0x627c728eU, 0xc199f56fU, 0x5e115643U, 0x5604a824U
}};

static const Bp256Int BP256_A = {{
    0xf330b5d9U, 0xe94a4b44U, 0x26dc5c6cU, 0xfb8055c1U,
    0x417affe7U, 0xeef67530U, 0xfc2c3057U, 0x7d5a0975U
}};

static const Bp256Int BP256_B = {{
    0xff8c07b6U, 0x6bccdc18U, 0x5cf7e1ceU, 0x95841629U,
    0xbbd77cbfU, 0xf330b5d9U, 0xe94a4b44U, 0x26dc5c6cU
}};

static const Bp256Int BP256_GX = {{
    0x9ace3262U, 0x3a4453bdU, 0xe3bd23c2U, 0xb9de27e1U,
    0xfc81b7afU, 0x2c4b482fU, 0xcb7e57cbU, 0x8bd2aeb9U
}};

static const Bp256Int BP256_GY = {{
    0x2f046997U, 0x5c1d54c7U, 0x2ded8e54U, 0xc2774513U,
    0x14611dc9U, 0x97f8461aU, 0xc3dac4fdU, 0x547ef835U
}};

static void bp256_copy(Bp256Int *out, const Bp256Int *in) {
    size_t index;
    for (index = 0U; index < BP256_WORDS; ++index) out->word[index] = in->word[index];
}

static void bp256_zero(Bp256Int *out) {
    size_t index;
    for (index = 0U; index < BP256_WORDS; ++index) out->word[index] = 0U;
}

static int bp256_is_zero(const Bp256Int *value) {
    size_t index;
    unsigned int any = 0U;
    for (index = 0U; index < BP256_WORDS; ++index) any |= value->word[index];
    return any == 0U;
}

static int bp256_cmp(const Bp256Int *left, const Bp256Int *right) {
    size_t index = BP256_WORDS;
    while (index > 0U) {
        index -= 1U;
        if (left->word[index] > right->word[index]) return 1;
        if (left->word[index] < right->word[index]) return -1;
    }
    return 0;
}

static unsigned int bp256_add_raw(Bp256Int *out, const Bp256Int *left, const Bp256Int *right) {
    unsigned long long carry = 0ULL;
    size_t index;
    for (index = 0U; index < BP256_WORDS; ++index) {
        unsigned long long sum = (unsigned long long)left->word[index] + (unsigned long long)right->word[index] + carry;
        out->word[index] = (unsigned int)sum;
        carry = sum >> 32U;
    }
    return (unsigned int)carry;
}

static unsigned int bp256_sub_raw(Bp256Int *out, const Bp256Int *left, const Bp256Int *right) {
    unsigned long long borrow = 0ULL;
    size_t index;
    for (index = 0U; index < BP256_WORDS; ++index) {
        unsigned long long left_value = (unsigned long long)left->word[index];
        unsigned long long right_value = (unsigned long long)right->word[index] + borrow;
        if (left_value >= right_value) {
            out->word[index] = (unsigned int)(left_value - right_value);
            borrow = 0ULL;
        } else {
            out->word[index] = (unsigned int)((1ULL << 32U) + left_value - right_value);
            borrow = 1ULL;
        }
    }
    return (unsigned int)borrow;
}

static const Bp256Int *bp256_reduction_for_modulus(const Bp256Int *modulus) {
    return modulus == &BP256_N ? &BP256_R_N : &BP256_R_P;
}

static void bp256_mod_reduce_once(Bp256Int *value, const Bp256Int *modulus) {
    Bp256Int tmp;
    if (bp256_cmp(value, modulus) >= 0) {
        (void)bp256_sub_raw(&tmp, value, modulus);
        bp256_copy(value, &tmp);
    }
}

static void bp256_mod_add(Bp256Int *out, const Bp256Int *left, const Bp256Int *right, const Bp256Int *modulus) {
    Bp256Int tmp;
    unsigned int carry = bp256_add_raw(&tmp, left, right);
    while (carry != 0U) {
        carry = bp256_add_raw(&tmp, &tmp, bp256_reduction_for_modulus(modulus));
    }
    while (bp256_cmp(&tmp, modulus) >= 0) {
        (void)bp256_sub_raw(&tmp, &tmp, modulus);
    }
    bp256_copy(out, &tmp);
}

static void bp256_mod_sub(Bp256Int *out, const Bp256Int *left, const Bp256Int *right, const Bp256Int *modulus) {
    Bp256Int tmp;
    if (bp256_sub_raw(&tmp, left, right) != 0U) {
        (void)bp256_add_raw(&tmp, &tmp, modulus);
    }
    bp256_copy(out, &tmp);
}

static int bp256_get_bit(const Bp256Int *value, unsigned int bit) {
    return (int)((value->word[bit >> 5U] >> (bit & 31U)) & 1U);
}

static void bp256_mod_double(Bp256Int *out, const Bp256Int *value, const Bp256Int *modulus) {
    bp256_mod_add(out, value, value, modulus);
}

static void bp256_mod_mul(Bp256Int *out, const Bp256Int *left, const Bp256Int *right, const Bp256Int *modulus) {
    Bp256Int result;
    Bp256Int addend;
    unsigned int bit;

    bp256_zero(&result);
    bp256_copy(&addend, left);
    bp256_mod_reduce_once(&addend, modulus);
    for (bit = 0U; bit < 256U; ++bit) {
        if (bp256_get_bit(right, bit)) bp256_mod_add(&result, &result, &addend, modulus);
        bp256_mod_double(&addend, &addend, modulus);
    }
    bp256_copy(out, &result);
}

static void bp256_mod_square(Bp256Int *out, const Bp256Int *value, const Bp256Int *modulus) {
    bp256_mod_mul(out, value, value, modulus);
}

static void bp256_mod_pow(Bp256Int *out, const Bp256Int *base, const Bp256Int *exponent, const Bp256Int *modulus) {
    Bp256Int result;
    Bp256Int factor;
    int bit;

    bp256_copy(&result, &BP256_ONE);
    bp256_copy(&factor, base);
    bp256_mod_reduce_once(&factor, modulus);
    for (bit = 255; bit >= 0; --bit) {
        bp256_mod_square(&result, &result, modulus);
        if (bp256_get_bit(exponent, (unsigned int)bit)) bp256_mod_mul(&result, &result, &factor, modulus);
    }
    bp256_copy(out, &result);
}

static void bp256_mod_inverse(Bp256Int *out, const Bp256Int *value, const Bp256Int *modulus) {
    Bp256Int exponent;
    bp256_copy(&exponent, modulus);
    exponent.word[0] -= 2U;
    bp256_mod_pow(out, value, &exponent, modulus);
}

static void bp256_from_be(Bp256Int *out, const unsigned char bytes[32]) {
    size_t index;
    bp256_zero(out);
    for (index = 0U; index < 32U; ++index) {
        size_t word_index = index >> 2U;
        out->word[word_index] |= (unsigned int)bytes[31U - index] << ((index & 3U) * 8U);
    }
}

static void bp256_to_be(unsigned char bytes[32], const Bp256Int *value) {
    size_t index;
    for (index = 0U; index < 32U; ++index) {
        size_t word_index = index >> 2U;
        bytes[31U - index] = (unsigned char)(value->word[word_index] >> ((index & 3U) * 8U));
    }
}

static void field_add(Bp256Int *out, const Bp256Int *left, const Bp256Int *right) { bp256_mod_add(out, left, right, &BP256_P); }
static void field_sub(Bp256Int *out, const Bp256Int *left, const Bp256Int *right) { bp256_mod_sub(out, left, right, &BP256_P); }
static void field_mul(Bp256Int *out, const Bp256Int *left, const Bp256Int *right) { bp256_mod_mul(out, left, right, &BP256_P); }
static void field_square(Bp256Int *out, const Bp256Int *value) { bp256_mod_square(out, value, &BP256_P); }
static void field_inv(Bp256Int *out, const Bp256Int *value) { bp256_mod_inverse(out, value, &BP256_P); }

static int scalar_valid(const Bp256Int *scalar) {
    return !bp256_is_zero(scalar) && bp256_cmp(scalar, &BP256_N) < 0;
}

static int bp256_int_equal(const Bp256Int *left, const Bp256Int *right) {
    return bp256_cmp(left, right) == 0;
}

static int bp256_point_is_on_curve(const Bp256Int *x, const Bp256Int *y) {
    Bp256Int lhs;
    Bp256Int rhs;
    Bp256Int tmp;

    if (bp256_cmp(x, &BP256_P) >= 0 || bp256_cmp(y, &BP256_P) >= 0) return 0;
    field_square(&lhs, y);
    field_square(&rhs, x);
    field_mul(&rhs, &rhs, x);
    field_mul(&tmp, &BP256_A, x);
    field_add(&rhs, &rhs, &tmp);
    field_add(&rhs, &rhs, &BP256_B);
    return bp256_int_equal(&lhs, &rhs);
}

static void bp256_jacobian_set_infinity(Bp256Jacobian *point) {
    bp256_copy(&point->x, &BP256_ZERO);
    bp256_copy(&point->y, &BP256_ZERO);
    bp256_copy(&point->z, &BP256_ZERO);
    point->infinity = 1;
}

static void bp256_jacobian_set_affine(Bp256Jacobian *point, const Bp256Int *x, const Bp256Int *y) {
    bp256_copy(&point->x, x);
    bp256_copy(&point->y, y);
    bp256_copy(&point->z, &BP256_ONE);
    point->infinity = 0;
}

static void bp256_point_double(Bp256Jacobian *out, const Bp256Jacobian *point) {
    Bp256Int xx;
    Bp256Int yy;
    Bp256Int yyyy;
    Bp256Int zz;
    Bp256Int zzzz;
    Bp256Int s;
    Bp256Int m;
    Bp256Int t;
    Bp256Int tmp1;
    Bp256Int tmp2;

    if (point->infinity || bp256_is_zero(&point->y)) {
        bp256_jacobian_set_infinity(out);
        return;
    }

    field_square(&xx, &point->x);
    field_square(&yy, &point->y);
    field_square(&yyyy, &yy);
    field_square(&zz, &point->z);
    field_square(&zzzz, &zz);

    field_add(&tmp1, &point->x, &yy);
    field_square(&tmp1, &tmp1);
    field_sub(&tmp1, &tmp1, &xx);
    field_sub(&tmp1, &tmp1, &yyyy);
    field_add(&s, &tmp1, &tmp1);

    field_mul(&m, &BP256_THREE, &xx);
    field_mul(&tmp1, &BP256_A, &zzzz);
    field_add(&m, &m, &tmp1);

    field_square(&t, &m);
    field_sub(&t, &t, &s);
    field_sub(&t, &t, &s);
    bp256_copy(&out->x, &t);

    field_sub(&tmp1, &s, &t);
    field_mul(&tmp1, &m, &tmp1);
    field_mul(&tmp2, &BP256_EIGHT, &yyyy);
    field_sub(&out->y, &tmp1, &tmp2);

    field_add(&tmp1, &point->y, &point->z);
    field_square(&tmp1, &tmp1);
    field_sub(&tmp1, &tmp1, &yy);
    field_sub(&out->z, &tmp1, &zz);
    out->infinity = 0;
}

static void bp256_point_add_mixed(Bp256Jacobian *out, const Bp256Jacobian *point, const Bp256Int *x2, const Bp256Int *y2) {
    Bp256Int z1z1;
    Bp256Int u2;
    Bp256Int s2;
    Bp256Int h;
    Bp256Int hh;
    Bp256Int i;
    Bp256Int j;
    Bp256Int r;
    Bp256Int v;
    Bp256Int tmp1;
    Bp256Int tmp2;

    if (point->infinity) {
        bp256_jacobian_set_affine(out, x2, y2);
        return;
    }

    field_square(&z1z1, &point->z);
    field_mul(&u2, x2, &z1z1);
    field_mul(&s2, &point->z, &z1z1);
    field_mul(&s2, y2, &s2);
    field_sub(&h, &u2, &point->x);
    field_sub(&r, &s2, &point->y);
    if (bp256_is_zero(&h)) {
        if (bp256_is_zero(&r)) bp256_point_double(out, point);
        else bp256_jacobian_set_infinity(out);
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

static int bp256_jacobian_to_affine(Bp256Int *out_x, Bp256Int *out_y, const Bp256Jacobian *point) {
    Bp256Int z_inv;
    Bp256Int z2;
    Bp256Int z3;

    if (point->infinity) return 0;
    field_inv(&z_inv, &point->z);
    field_square(&z2, &z_inv);
    field_mul(&z3, &z2, &z_inv);
    field_mul(out_x, &point->x, &z2);
    field_mul(out_y, &point->y, &z3);
    return 1;
}

static int bp256_scalar_mult_affine(Bp256Int *out_x, Bp256Int *out_y, const Bp256Int *scalar, const Bp256Int *base_x, const Bp256Int *base_y) {
    Bp256Jacobian result;
    Bp256Jacobian next;
    int bit;

    bp256_jacobian_set_infinity(&result);
    for (bit = 255; bit >= 0; --bit) {
        bp256_point_double(&next, &result);
        result = next;
        if (bp256_get_bit(scalar, (unsigned int)bit)) {
            bp256_point_add_mixed(&next, &result, base_x, base_y);
            result = next;
        }
    }
    return bp256_jacobian_to_affine(out_x, out_y, &result);
}

static int bp256_double_scalar_mult_affine(Bp256Int *out_x, Bp256Int *out_y, const Bp256Int *left_scalar, const Bp256Int *left_x, const Bp256Int *left_y, const Bp256Int *right_scalar, const Bp256Int *right_x, const Bp256Int *right_y) {
    Bp256Jacobian result;
    Bp256Jacobian next;
    int bit;

    bp256_jacobian_set_infinity(&result);
    for (bit = 255; bit >= 0; --bit) {
        bp256_point_double(&next, &result);
        result = next;
        if (bp256_get_bit(left_scalar, (unsigned int)bit)) {
            bp256_point_add_mixed(&next, &result, left_x, left_y);
            result = next;
        }
        if (bp256_get_bit(right_scalar, (unsigned int)bit)) {
            bp256_point_add_mixed(&next, &result, right_x, right_y);
            result = next;
        }
    }
    return bp256_jacobian_to_affine(out_x, out_y, &result);
}

static int bp256_decode_public(const unsigned char public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE], Bp256Int *x, Bp256Int *y) {
    if (public_key == 0 || x == 0 || y == 0 || public_key[0] != 0x04U) return 0;
    bp256_from_be(x, public_key + 1U);
    bp256_from_be(y, public_key + 33U);
    return bp256_point_is_on_curve(x, y);
}

static void bp256_encode_public(unsigned char public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE], const Bp256Int *x, const Bp256Int *y) {
    public_key[0] = 0x04U;
    bp256_to_be(public_key + 1U, x);
    bp256_to_be(public_key + 33U, y);
}

int crypto_brainpoolp256r1_public_key_valid(const unsigned char public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE]) {
    Bp256Int x;
    Bp256Int y;
    return bp256_decode_public(public_key, &x, &y);
}

int crypto_brainpoolp256r1_public_from_private(const unsigned char private_key[CRYPTO_BRAINPOOLP256R1_SCALAR_SIZE], unsigned char public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE]) {
    Bp256Int scalar;
    Bp256Int x;
    Bp256Int y;

    if (private_key == 0 || public_key == 0) return 0;
    bp256_from_be(&scalar, private_key);
    if (!scalar_valid(&scalar)) return 0;
    if (!bp256_scalar_mult_affine(&x, &y, &scalar, &BP256_GX, &BP256_GY)) return 0;
    bp256_encode_public(public_key, &x, &y);
    return 1;
}

int crypto_brainpoolp256r1_scalar_mult_public(const unsigned char scalar_bytes[CRYPTO_BRAINPOOLP256R1_SCALAR_SIZE], const unsigned char public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE], unsigned char result_public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE]) {
    Bp256Int scalar;
    Bp256Int base_x;
    Bp256Int base_y;
    Bp256Int out_x;
    Bp256Int out_y;

    if (scalar_bytes == 0 || public_key == 0 || result_public_key == 0) return 0;
    bp256_from_be(&scalar, scalar_bytes);
    if (!scalar_valid(&scalar) || !bp256_decode_public(public_key, &base_x, &base_y)) return 0;
    if (!bp256_scalar_mult_affine(&out_x, &out_y, &scalar, &base_x, &base_y)) return 0;
    bp256_encode_public(result_public_key, &out_x, &out_y);
    return 1;
}

int crypto_brainpoolp256r1_shared_secret(const unsigned char private_key[CRYPTO_BRAINPOOLP256R1_SCALAR_SIZE], const unsigned char peer_public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE], unsigned char shared_x[CRYPTO_BRAINPOOLP256R1_COORD_SIZE]) {
    Bp256Int scalar;
    Bp256Int peer_x;
    Bp256Int peer_y;
    Bp256Int out_x;
    Bp256Int out_y;

    if (private_key == 0 || peer_public_key == 0 || shared_x == 0) return 0;
    bp256_from_be(&scalar, private_key);
    if (!scalar_valid(&scalar) || !bp256_decode_public(peer_public_key, &peer_x, &peer_y)) return 0;
    if (!bp256_scalar_mult_affine(&out_x, &out_y, &scalar, &peer_x, &peer_y)) return 0;
    (void)out_y;
    bp256_to_be(shared_x, &out_x);
    return 1;
}

int crypto_brainpoolp256r1_add_generator_mul(const unsigned char scalar_bytes[CRYPTO_BRAINPOOLP256R1_SCALAR_SIZE], const unsigned char public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE], unsigned char result_public_key[CRYPTO_BRAINPOOLP256R1_PUBLIC_KEY_SIZE]) {
    Bp256Int scalar;
    Bp256Int add_x;
    Bp256Int add_y;
    Bp256Int one;
    Bp256Int out_x;
    Bp256Int out_y;

    if (scalar_bytes == 0 || public_key == 0 || result_public_key == 0) return 0;
    bp256_from_be(&scalar, scalar_bytes);
    if (!scalar_valid(&scalar) || !bp256_decode_public(public_key, &add_x, &add_y)) return 0;
    bp256_copy(&one, &BP256_ONE);
    if (!bp256_double_scalar_mult_affine(&out_x, &out_y, &scalar, &BP256_GX, &BP256_GY, &one, &add_x, &add_y)) return 0;
    bp256_encode_public(result_public_key, &out_x, &out_y);
    return 1;
}