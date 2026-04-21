#include "crypto/rsa.h"
#include "crypto/crypto_util.h"
#include "crypto/sha256.h"

#define RSA_BN_MAX_WORDS 80U

typedef struct {
    unsigned int words[RSA_BN_MAX_WORDS];
    size_t length;
} CryptoRsaBigNum;

static void bn_zero(CryptoRsaBigNum *value) {
    size_t i;

    for (i = 0; i < RSA_BN_MAX_WORDS; ++i) {
        value->words[i] = 0U;
    }
    value->length = 0U;
}

static void bn_normalize(CryptoRsaBigNum *value) {
    while (value->length > 0U && value->words[value->length - 1U] == 0U) {
        value->length -= 1U;
    }
}

static void bn_from_u32(CryptoRsaBigNum *value, unsigned int word) {
    bn_zero(value);
    if (word != 0U) {
        value->words[0] = word;
        value->length = 1U;
    }
}

static void bn_from_u64(CryptoRsaBigNum *value, unsigned long long word) {
    bn_zero(value);
    if (word != 0ULL) {
        value->words[0] = (unsigned int)word;
        value->words[1] = (unsigned int)(word >> 32U);
        value->length = value->words[1] != 0U ? 2U : 1U;
    }
}

static void bn_from_bytes(CryptoRsaBigNum *value, const unsigned char *bytes, size_t len) {
    size_t i;

    bn_zero(value);
    for (i = 0; i < len; ++i) {
        size_t word_index = i >> 2U;
        value->words[word_index] |= (unsigned int)bytes[len - 1U - i] << ((i & 3U) * 8U);
    }
    value->length = (len + 3U) >> 2U;
    bn_normalize(value);
}

static void bn_to_bytes(unsigned char *out, size_t out_len, const CryptoRsaBigNum *value) {
    size_t i;

    for (i = 0; i < out_len; ++i) {
        out[i] = 0U;
    }
    for (i = 0; i < value->length * 4U && i < out_len; ++i) {
        out[out_len - 1U - i] = (unsigned char)(value->words[i >> 2U] >> ((i & 3U) * 8U));
    }
}

static void bn_copy(CryptoRsaBigNum *dst, const CryptoRsaBigNum *src) {
    size_t i;

    dst->length = src->length;
    for (i = 0; i < RSA_BN_MAX_WORDS; ++i) {
        dst->words[i] = src->words[i];
    }
}

static int bn_compare(const CryptoRsaBigNum *left, const CryptoRsaBigNum *right) {
    size_t i;

    if (left->length > right->length) {
        return 1;
    }
    if (left->length < right->length) {
        return -1;
    }

    i = left->length;
    while (i > 0U) {
        i -= 1U;
        if (left->words[i] > right->words[i]) {
            return 1;
        }
        if (left->words[i] < right->words[i]) {
            return -1;
        }
    }

    return 0;
}

static void bn_add(CryptoRsaBigNum *out, const CryptoRsaBigNum *left, const CryptoRsaBigNum *right) {
    CryptoRsaBigNum tmp;
    size_t i;
    size_t max_words = left->length > right->length ? left->length : right->length;
    unsigned long long carry = 0ULL;

    bn_zero(&tmp);
    for (i = 0; i < max_words; ++i) {
        unsigned long long lv = i < left->length ? left->words[i] : 0U;
        unsigned long long rv = i < right->length ? right->words[i] : 0U;
        unsigned long long sum = lv + rv + carry;
        tmp.words[i] = (unsigned int)sum;
        carry = sum >> 32U;
    }

    tmp.length = max_words;
    if (carry != 0ULL && tmp.length < RSA_BN_MAX_WORDS) {
        tmp.words[tmp.length] = (unsigned int)carry;
        tmp.length += 1U;
    }

    bn_normalize(&tmp);
    bn_copy(out, &tmp);
    crypto_secure_bzero(&tmp, sizeof(tmp));
}

static void bn_sub(CryptoRsaBigNum *out, const CryptoRsaBigNum *left, const CryptoRsaBigNum *right) {
    CryptoRsaBigNum tmp;
    size_t i;
    unsigned long long borrow = 0ULL;

    bn_zero(&tmp);
    for (i = 0; i < left->length; ++i) {
        unsigned long long lv = left->words[i];
        unsigned long long rv = i < right->length ? right->words[i] : 0U;
        unsigned long long subtrahend = rv + borrow;

        if (lv >= subtrahend) {
            tmp.words[i] = (unsigned int)(lv - subtrahend);
            borrow = 0ULL;
        } else {
            tmp.words[i] = (unsigned int)(((1ULL << 32U) + lv) - subtrahend);
            borrow = 1ULL;
        }
    }

    tmp.length = left->length;
    bn_normalize(&tmp);
    bn_copy(out, &tmp);
    crypto_secure_bzero(&tmp, sizeof(tmp));
}

static size_t bn_bit_length(const CryptoRsaBigNum *value) {
    unsigned int top;
    size_t bits;

    if (value->length == 0U) {
        return 0U;
    }

    top = value->words[value->length - 1U];
    bits = (value->length - 1U) * 32U;
    while (top != 0U) {
        bits += 1U;
        top >>= 1U;
    }
    return bits;
}

static void bn_shift_left_bits(CryptoRsaBigNum *out, const CryptoRsaBigNum *value, size_t shift) {
    size_t word_shift = shift >> 5U;
    size_t bit_shift = shift & 31U;
    size_t i;
    unsigned int carry = 0U;

    bn_zero(out);
    if (value->length == 0U) {
        return;
    }

    for (i = 0; i < value->length; ++i) {
        unsigned long long current = ((unsigned long long)value->words[i] << bit_shift) | carry;
        out->words[i + word_shift] = (unsigned int)current;
        carry = (unsigned int)(current >> 32U);
    }

    out->length = value->length + word_shift;
    if (carry != 0U) {
        out->words[out->length] = carry;
        out->length += 1U;
    }

    bn_normalize(out);
}

static void bn_mul(CryptoRsaBigNum *out, const CryptoRsaBigNum *left, const CryptoRsaBigNum *right) {
    size_t i;
    size_t j;

    bn_zero(out);
    for (i = 0; i < left->length; ++i) {
        unsigned long long carry = 0ULL;
        for (j = 0; j < right->length; ++j) {
            unsigned long long current = (unsigned long long)out->words[i + j] +
                                         (unsigned long long)left->words[i] * (unsigned long long)right->words[j] +
                                         carry;
            out->words[i + j] = (unsigned int)current;
            carry = current >> 32U;
        }

        j = i + right->length;
        while (carry != 0ULL && j < RSA_BN_MAX_WORDS) {
            unsigned long long current = (unsigned long long)out->words[j] + carry;
            out->words[j] = (unsigned int)current;
            carry = current >> 32U;
            j += 1U;
        }
    }

    out->length = left->length + right->length;
    if (out->length > RSA_BN_MAX_WORDS) {
        out->length = RSA_BN_MAX_WORDS;
    }
    bn_normalize(out);
}

static void bn_mod(CryptoRsaBigNum *value, const CryptoRsaBigNum *modulus) {
    CryptoRsaBigNum shifted;
    size_t modulus_bits;

    if (modulus->length == 0U) {
        return;
    }

    modulus_bits = bn_bit_length(modulus);
    while (bn_compare(value, modulus) >= 0) {
        size_t value_bits = bn_bit_length(value);
        size_t shift = value_bits > modulus_bits ? value_bits - modulus_bits : 0U;

        bn_shift_left_bits(&shifted, modulus, shift);
        if (bn_compare(value, &shifted) < 0 && shift > 0U) {
            shift -= 1U;
            bn_shift_left_bits(&shifted, modulus, shift);
        }
        bn_sub(value, value, &shifted);
    }
}

static void bn_mul_mod(
    CryptoRsaBigNum *out,
    const CryptoRsaBigNum *left,
    const CryptoRsaBigNum *right,
    const CryptoRsaBigNum *modulus
) {
    CryptoRsaBigNum tmp;

    bn_mul(&tmp, left, right);
    bn_mod(&tmp, modulus);
    bn_copy(out, &tmp);
    crypto_secure_bzero(&tmp, sizeof(tmp));
}

static int bn_test_bit(const CryptoRsaBigNum *value, size_t bit_index) {
    size_t word_index = bit_index >> 5U;
    size_t shift = bit_index & 31U;

    if (word_index >= value->length) {
        return 0;
    }
    return (int)((value->words[word_index] >> shift) & 1U);
}

static void bn_modexp(
    CryptoRsaBigNum *out,
    const CryptoRsaBigNum *base,
    const CryptoRsaBigNum *exponent,
    const CryptoRsaBigNum *modulus
) {
    CryptoRsaBigNum result;
    CryptoRsaBigNum base_acc;
    CryptoRsaBigNum tmp;
    size_t bit;
    size_t bits = bn_bit_length(exponent);

    bn_from_u32(&result, 1U);
    bn_copy(&base_acc, base);
    bn_mod(&base_acc, modulus);

    for (bit = bits; bit > 0U; --bit) {
        bn_mul_mod(&tmp, &result, &result, modulus);
        bn_copy(&result, &tmp);
        if (bn_test_bit(exponent, bit - 1U)) {
            bn_mul_mod(&tmp, &result, &base_acc, modulus);
            bn_copy(&result, &tmp);
        }
    }

    bn_copy(out, &result);
    crypto_secure_bzero(&result, sizeof(result));
    crypto_secure_bzero(&base_acc, sizeof(base_acc));
    crypto_secure_bzero(&tmp, sizeof(tmp));
}

static int rsa_blind_exponent(
    CryptoRsaBigNum *out,
    const unsigned char *exponent_bytes,
    size_t exponent_len,
    const CryptoRsaBigNum *prime
) {
    unsigned char blind_bytes[8];
    CryptoRsaBigNum exponent;
    CryptoRsaBigNum blind;
    CryptoRsaBigNum prime_minus_one;
    CryptoRsaBigNum blind_mul;
    CryptoRsaBigNum one;
    int status = -1;

    if (crypto_random_bytes(blind_bytes, sizeof(blind_bytes)) != 0) {
        goto cleanup;
    }
    blind_bytes[sizeof(blind_bytes) - 1U] |= 1U;

    bn_from_bytes(&exponent, exponent_bytes, exponent_len);
    bn_from_u32(&one, 1U);
    bn_sub(&prime_minus_one, prime, &one);
    bn_from_u64(
        &blind,
        ((unsigned long long)blind_bytes[0] << 56U) |
        ((unsigned long long)blind_bytes[1] << 48U) |
        ((unsigned long long)blind_bytes[2] << 40U) |
        ((unsigned long long)blind_bytes[3] << 32U) |
        ((unsigned long long)blind_bytes[4] << 24U) |
        ((unsigned long long)blind_bytes[5] << 16U) |
        ((unsigned long long)blind_bytes[6] << 8U) |
        (unsigned long long)blind_bytes[7]
    );
    bn_mul(&blind_mul, &prime_minus_one, &blind);
    bn_add(out, &exponent, &blind_mul);
    status = 0;

cleanup:
    crypto_secure_bzero(blind_bytes, sizeof(blind_bytes));
    crypto_secure_bzero(&exponent, sizeof(exponent));
    crypto_secure_bzero(&blind, sizeof(blind));
    crypto_secure_bzero(&prime_minus_one, sizeof(prime_minus_one));
    crypto_secure_bzero(&blind_mul, sizeof(blind_mul));
    crypto_secure_bzero(&one, sizeof(one));
    return status;
}

static int der_read_length(
    const unsigned char *der,
    size_t der_len,
    size_t *pos,
    size_t *out_len
) {
    unsigned char first;
    size_t count;
    size_t length = 0U;
    size_t i;

    if (*pos >= der_len) {
        return -1;
    }

    first = der[*pos];
    *pos += 1U;
    if ((first & 0x80U) == 0U) {
        *out_len = (size_t)first;
        return 0;
    }

    count = (size_t)(first & 0x7fU);
    if (count == 0U || count > sizeof(size_t) || *pos + count > der_len) {
        return -1;
    }

    for (i = 0; i < count; ++i) {
        length = (length << 8U) | der[*pos];
        *pos += 1U;
    }

    *out_len = length;
    return 0;
}

static int der_expect_tlv(
    const unsigned char *der,
    size_t der_len,
    size_t *pos,
    unsigned char expected_tag,
    const unsigned char **value,
    size_t *value_len
) {
    size_t length;

    if (*pos >= der_len || der[*pos] != expected_tag) {
        return -1;
    }

    *pos += 1U;
    if (der_read_length(der, der_len, pos, &length) != 0 || *pos + length > der_len) {
        return -1;
    }

    *value = der + *pos;
    *value_len = length;
    *pos += length;
    return 0;
}

static size_t trim_leading_zeroes(const unsigned char *data, size_t len) {
    size_t offset = 0U;

    while (offset + 1U < len && data[offset] == 0U) {
        offset += 1U;
    }
    return offset;
}

static int copy_der_integer(
    unsigned char *dst,
    size_t cap,
    size_t *out_len,
    const unsigned char *value,
    size_t value_len
) {
    size_t offset = trim_leading_zeroes(value, value_len);
    size_t used = value_len - offset;
    size_t i;

    if (used == 0U || used > cap) {
        return -1;
    }

    for (i = 0; i < used; ++i) {
        dst[i] = value[offset + i];
    }
    *out_len = used;
    return 0;
}

int crypto_rsa2048_parse_private_key_der(
    CryptoRsaPrivateKey *key,
    const unsigned char *der,
    size_t der_len
) {
    const unsigned char *sequence;
    const unsigned char *value;
    size_t sequence_len;
    size_t pos = 0U;
    size_t inner = 0U;
    size_t integer_len = 0U;

    if (key == 0 || der == 0 || der_len == 0U) {
        return -1;
    }

    crypto_secure_bzero(key, sizeof(*key));

    if (der_expect_tlv(der, der_len, &pos, 0x30U, &sequence, &sequence_len) != 0 || pos != der_len) {
        return -1;
    }

    if (der_expect_tlv(sequence, sequence_len, &inner, 0x02U, &value, &integer_len) != 0) {
        return -1;
    }
    if (der_expect_tlv(sequence, sequence_len, &inner, 0x02U, &value, &integer_len) != 0) {
        return -1;
    }
    key->modulus_len = integer_len - trim_leading_zeroes(value, integer_len);

    if (der_expect_tlv(sequence, sequence_len, &inner, 0x02U, &value, &integer_len) != 0 ||
        der_expect_tlv(sequence, sequence_len, &inner, 0x02U, &value, &integer_len) != 0) {
        return -1;
    }

    if (der_expect_tlv(sequence, sequence_len, &inner, 0x02U, &value, &integer_len) != 0 ||
        copy_der_integer(key->p, sizeof(key->p), &key->p_len, value, integer_len) != 0 ||
        der_expect_tlv(sequence, sequence_len, &inner, 0x02U, &value, &integer_len) != 0 ||
        copy_der_integer(key->q, sizeof(key->q), &key->q_len, value, integer_len) != 0 ||
        der_expect_tlv(sequence, sequence_len, &inner, 0x02U, &value, &integer_len) != 0 ||
        copy_der_integer(key->dp, sizeof(key->dp), &key->dp_len, value, integer_len) != 0 ||
        der_expect_tlv(sequence, sequence_len, &inner, 0x02U, &value, &integer_len) != 0 ||
        copy_der_integer(key->dq, sizeof(key->dq), &key->dq_len, value, integer_len) != 0 ||
        der_expect_tlv(sequence, sequence_len, &inner, 0x02U, &value, &integer_len) != 0 ||
        copy_der_integer(key->qinv, sizeof(key->qinv), &key->qinv_len, value, integer_len) != 0) {
        return -1;
    }

    return inner == sequence_len && key->modulus_len == CRYPTO_RSA2048_MODULUS_SIZE ? 0 : -1;
}

static void rsa_mgf1_sha256(unsigned char *out, size_t out_len, const unsigned char seed[32]) {
    unsigned char input[36];
    unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned int counter = 0U;
    size_t done = 0U;
    size_t i;

    for (i = 0; i < 32U; ++i) {
        input[i] = seed[i];
    }

    while (done < out_len) {
        input[32] = (unsigned char)(counter >> 24U);
        input[33] = (unsigned char)(counter >> 16U);
        input[34] = (unsigned char)(counter >> 8U);
        input[35] = (unsigned char)counter;
        crypto_sha256_hash(input, sizeof(input), digest);
        for (i = 0; i < sizeof(digest) && done < out_len; ++i) {
            out[done] = digest[i];
            done += 1U;
        }
        counter += 1U;
    }

    crypto_secure_bzero(input, sizeof(input));
    crypto_secure_bzero(digest, sizeof(digest));
}

int crypto_rsa2048_pss_sha256_sign(
    unsigned char *signature,
    size_t signature_cap,
    size_t *signature_len,
    const unsigned char *message,
    size_t message_len,
    const CryptoRsaPrivateKey *key
) {
    unsigned char message_hash[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned char salt[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned char mprime[72];
    unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned char db[223];
    unsigned char db_mask[223];
    unsigned char encoded[CRYPTO_RSA2048_MODULUS_SIZE];
    CryptoRsaBigNum p;
    CryptoRsaBigNum q;
    CryptoRsaBigNum dp;
    CryptoRsaBigNum dq;
    CryptoRsaBigNum qinv;
    CryptoRsaBigNum message_bn;
    CryptoRsaBigNum mp;
    CryptoRsaBigNum mq;
    CryptoRsaBigNum m1;
    CryptoRsaBigNum m2;
    CryptoRsaBigNum diff;
    CryptoRsaBigNum h_bn;
    CryptoRsaBigNum qh;
    CryptoRsaBigNum sig_bn;
    CryptoRsaBigNum tmp;
    CryptoRsaBigNum dp_blinded;
    CryptoRsaBigNum dq_blinded;
    size_t i;
    size_t ps_len;
    int status = -1;

    if (signature == 0 || signature_len == 0 || key == 0 ||
        (message == 0 && message_len != 0U) ||
        key->modulus_len != CRYPTO_RSA2048_MODULUS_SIZE ||
        signature_cap < key->modulus_len) {
        return -1;
    }

    crypto_sha256_hash(message, message_len, message_hash);
    if (crypto_random_bytes(salt, sizeof(salt)) != 0) {
        goto cleanup;
    }

    for (i = 0; i < 8U; ++i) {
        mprime[i] = 0U;
    }
    for (i = 0; i < sizeof(message_hash); ++i) {
        mprime[8U + i] = message_hash[i];
        mprime[40U + i] = salt[i];
    }
    crypto_sha256_hash(mprime, sizeof(mprime), digest);

    ps_len = sizeof(db) - sizeof(salt) - 1U;
    for (i = 0; i < ps_len; ++i) {
        db[i] = 0U;
    }
    db[ps_len] = 0x01U;
    for (i = 0; i < sizeof(salt); ++i) {
        db[ps_len + 1U + i] = salt[i];
    }

    rsa_mgf1_sha256(db_mask, sizeof(db_mask), digest);
    for (i = 0; i < sizeof(db); ++i) {
        encoded[i] = (unsigned char)(db[i] ^ db_mask[i]);
    }
    encoded[0] &= 0x7fU;
    for (i = 0; i < sizeof(digest); ++i) {
        encoded[sizeof(db) + i] = digest[i];
    }
    encoded[sizeof(encoded) - 1U] = 0xbcU;

    bn_from_bytes(&p, key->p, key->p_len);
    bn_from_bytes(&q, key->q, key->q_len);
    bn_from_bytes(&dp, key->dp, key->dp_len);
    bn_from_bytes(&dq, key->dq, key->dq_len);
    bn_from_bytes(&qinv, key->qinv, key->qinv_len);
    bn_from_bytes(&message_bn, encoded, sizeof(encoded));

    if (rsa_blind_exponent(&dp_blinded, key->dp, key->dp_len, &p) != 0 ||
        rsa_blind_exponent(&dq_blinded, key->dq, key->dq_len, &q) != 0) {
        goto cleanup;
    }

    bn_copy(&mp, &message_bn);
    bn_copy(&mq, &message_bn);
    bn_mod(&mp, &p);
    bn_mod(&mq, &q);
    bn_modexp(&m1, &mp, &dp_blinded, &p);
    bn_modexp(&m2, &mq, &dq_blinded, &q);

    while (bn_compare(&m1, &m2) < 0) {
        bn_add(&tmp, &m1, &p);
        bn_copy(&m1, &tmp);
    }
    bn_sub(&diff, &m1, &m2);
    bn_mul_mod(&h_bn, &diff, &qinv, &p);
    bn_mul(&qh, &q, &h_bn);
    bn_add(&sig_bn, &m2, &qh);

    bn_to_bytes(signature, key->modulus_len, &sig_bn);
    *signature_len = key->modulus_len;
    status = 0;

cleanup:
    crypto_secure_bzero(message_hash, sizeof(message_hash));
    crypto_secure_bzero(salt, sizeof(salt));
    crypto_secure_bzero(mprime, sizeof(mprime));
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(db, sizeof(db));
    crypto_secure_bzero(db_mask, sizeof(db_mask));
    crypto_secure_bzero(encoded, sizeof(encoded));
    crypto_secure_bzero(&p, sizeof(p));
    crypto_secure_bzero(&q, sizeof(q));
    crypto_secure_bzero(&dp, sizeof(dp));
    crypto_secure_bzero(&dq, sizeof(dq));
    crypto_secure_bzero(&qinv, sizeof(qinv));
    crypto_secure_bzero(&message_bn, sizeof(message_bn));
    crypto_secure_bzero(&mp, sizeof(mp));
    crypto_secure_bzero(&mq, sizeof(mq));
    crypto_secure_bzero(&m1, sizeof(m1));
    crypto_secure_bzero(&m2, sizeof(m2));
    crypto_secure_bzero(&diff, sizeof(diff));
    crypto_secure_bzero(&h_bn, sizeof(h_bn));
    crypto_secure_bzero(&qh, sizeof(qh));
    crypto_secure_bzero(&sig_bn, sizeof(sig_bn));
    crypto_secure_bzero(&tmp, sizeof(tmp));
    crypto_secure_bzero(&dp_blinded, sizeof(dp_blinded));
    crypto_secure_bzero(&dq_blinded, sizeof(dq_blinded));
    return status;
}
