#include "crypto/x509.h"

#include "crypto/rsa.h"
#include "crypto/sha256.h"
#include "crypto/sha512.h"
#include "runtime.h"

#define X509_MAX_CERTS 8U
#define X509_MAX_DER_SIZE 8192U
#define X509_MAX_DNS_NAMES 16U
#define X509_MAX_DNS_NAME_LEN 128U

#define TLS_SIG_RSA_PSS_RSAE_SHA256 0x0804U
#define TLS_SIG_RSA_PSS_RSAE_SHA384 0x0805U

static const unsigned char g_oid_rsa_encryption[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
static const unsigned char g_oid_sha256_rsa[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
static const unsigned char g_oid_sha384_rsa[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c };
static const unsigned char g_oid_subject_alt_name[] = { 0x55, 0x1d, 0x11 };
static const unsigned char g_oid_basic_constraints[] = { 0x55, 0x1d, 0x13 };

struct der_tlv {
    unsigned char tag;
    const unsigned char *tlv;
    size_t tlv_len;
    const unsigned char *value;
    size_t value_len;
};

typedef struct {
    const unsigned char *der;
    size_t der_len;
    const unsigned char *tbs;
    size_t tbs_len;
    const unsigned char *issuer;
    size_t issuer_len;
    const unsigned char *subject;
    size_t subject_len;
    const unsigned char *signature;
    size_t signature_len;
    int signature_hash_id;
    unsigned char rsa_modulus[CRYPTO_RSA_MAX_MODULUS_SIZE];
    size_t rsa_modulus_len;
    unsigned char rsa_exponent[8];
    size_t rsa_exponent_len;
    long long not_before;
    long long not_after;
    int is_ca;
    char dns_names[X509_MAX_DNS_NAMES][X509_MAX_DNS_NAME_LEN];
    size_t dns_name_count;
} X509Parsed;

static void x509_status(char *status, size_t status_size, const char *message) {
    if (status != 0 && status_size != 0U) {
        rt_copy_string(status, status_size, message != 0 ? message : "x509 error");
    }
}

static int bytes_equal(const unsigned char *left, size_t left_len, const unsigned char *right, size_t right_len) {
    return left_len == right_len && (left_len == 0U || memcmp(left, right, left_len) == 0);
}

static int der_read_length(const unsigned char *der, size_t der_len, size_t *pos, size_t *out_len) {
    unsigned char first;
    size_t count;
    size_t length = 0U;
    size_t i;

    if (der == 0 || pos == 0 || out_len == 0 || *pos >= der_len) {
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

static int der_read_tlv(const unsigned char *der, size_t der_len, size_t *pos, struct der_tlv *out) {
    size_t start;
    size_t length;

    if (der == 0 || pos == 0 || out == 0 || *pos >= der_len) {
        return -1;
    }
    start = *pos;
    out->tag = der[*pos];
    *pos += 1U;
    if (der_read_length(der, der_len, pos, &length) != 0 || *pos + length > der_len) {
        return -1;
    }
    out->tlv = der + start;
    out->value = der + *pos;
    out->value_len = length;
    *pos += length;
    out->tlv_len = *pos - start;
    return 0;
}

static int der_expect_tlv(const unsigned char *der, size_t der_len, size_t *pos, unsigned char tag, struct der_tlv *out) {
    if (der_read_tlv(der, der_len, pos, out) != 0 || out->tag != tag) {
        return -1;
    }
    return 0;
}

static int der_read_oid(const unsigned char *der, size_t der_len, size_t *pos, const unsigned char **oid, size_t *oid_len) {
    struct der_tlv tlv;

    if (der_expect_tlv(der, der_len, pos, 0x06U, &tlv) != 0) {
        return -1;
    }
    *oid = tlv.value;
    *oid_len = tlv.value_len;
    return 0;
}

static int oid_is(const unsigned char *oid, size_t oid_len, const unsigned char *expected, size_t expected_len) {
    return oid_len == expected_len && memcmp(oid, expected, expected_len) == 0;
}

static int parse_algorithm_hash(const unsigned char *der, size_t der_len, int *hash_id_out) {
    struct der_tlv seq;
    const unsigned char *oid;
    size_t oid_len;
    size_t pos = 0U;
    size_t inner = 0U;

    if (der_expect_tlv(der, der_len, &pos, 0x30U, &seq) != 0 || pos != der_len ||
        der_read_oid(seq.value, seq.value_len, &inner, &oid, &oid_len) != 0) {
        return -1;
    }
    if (oid_is(oid, oid_len, g_oid_sha256_rsa, sizeof(g_oid_sha256_rsa))) {
        *hash_id_out = CRYPTO_RSA_HASH_SHA256;
        return 0;
    }
    if (oid_is(oid, oid_len, g_oid_sha384_rsa, sizeof(g_oid_sha384_rsa))) {
        *hash_id_out = CRYPTO_RSA_HASH_SHA384;
        return 0;
    }
    return -1;
}

static int copy_der_integer(unsigned char *dst, size_t cap, size_t *out_len, const unsigned char *value, size_t value_len) {
    size_t offset = 0U;
    size_t used;

    while (offset + 1U < value_len && value[offset] == 0U) {
        offset += 1U;
    }
    used = value_len - offset;
    if (used == 0U || used > cap) {
        return -1;
    }
    memcpy(dst, value + offset, used);
    *out_len = used;
    return 0;
}

static int parse_rsa_spki(X509Parsed *cert, const unsigned char *spki_der, size_t spki_len) {
    struct der_tlv spki;
    struct der_tlv alg;
    struct der_tlv bit_string;
    struct der_tlv rsa_seq;
    struct der_tlv integer;
    const unsigned char *oid;
    size_t oid_len;
    size_t pos = 0U;
    size_t inner = 0U;
    size_t alg_pos = 0U;
    size_t rsa_pos = 0U;

    if (der_expect_tlv(spki_der, spki_len, &pos, 0x30U, &spki) != 0 || pos != spki_len ||
        der_expect_tlv(spki.value, spki.value_len, &inner, 0x30U, &alg) != 0 ||
        der_read_oid(alg.value, alg.value_len, &alg_pos, &oid, &oid_len) != 0 ||
        !oid_is(oid, oid_len, g_oid_rsa_encryption, sizeof(g_oid_rsa_encryption)) ||
        der_expect_tlv(spki.value, spki.value_len, &inner, 0x03U, &bit_string) != 0 ||
        bit_string.value_len < 2U || bit_string.value[0] != 0U ||
        der_expect_tlv(bit_string.value + 1U, bit_string.value_len - 1U, &rsa_pos, 0x30U, &rsa_seq) != 0 ||
        rsa_pos != bit_string.value_len - 1U) {
        return -1;
    }
    rsa_pos = 0U;
    if (der_expect_tlv(rsa_seq.value, rsa_seq.value_len, &rsa_pos, 0x02U, &integer) != 0 ||
        copy_der_integer(cert->rsa_modulus, sizeof(cert->rsa_modulus), &cert->rsa_modulus_len, integer.value, integer.value_len) != 0 ||
        der_expect_tlv(rsa_seq.value, rsa_seq.value_len, &rsa_pos, 0x02U, &integer) != 0 ||
        copy_der_integer(cert->rsa_exponent, sizeof(cert->rsa_exponent), &cert->rsa_exponent_len, integer.value, integer.value_len) != 0) {
        return -1;
    }
    return rsa_pos == rsa_seq.value_len ? 0 : -1;
}

static int parse_digits(const unsigned char *p, size_t count, int *out) {
    size_t i;
    int value = 0;

    for (i = 0; i < count; ++i) {
        if (p[i] < '0' || p[i] > '9') {
            return -1;
        }
        value = value * 10 + (int)(p[i] - '0');
    }
    *out = value;
    return 0;
}

static long long days_from_civil(int year, unsigned int month, unsigned int day) {
    int era;
    unsigned int yoe;
    unsigned int doy;
    unsigned int doe;
    unsigned int adjusted_month;

    year -= month <= 2U;
    era = (year >= 0 ? year : year - 399) / 400;
    yoe = (unsigned int)(year - era * 400);
    adjusted_month = month > 2U ? month - 3U : month + 9U;
    doy = (153U * adjusted_month + 2U) / 5U + day - 1U;
    doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (long long)era * 146097LL + (long long)doe - 719468LL;
}

static int parse_time(const unsigned char *value, size_t value_len, unsigned char tag, long long *epoch_out) {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    size_t off = 0U;

    if (tag == 0x17U) {
        int yy;
        if (value_len != 13U || value[12] != 'Z' || parse_digits(value, 2U, &yy) != 0) return -1;
        year = yy >= 50 ? 1900 + yy : 2000 + yy;
        off = 2U;
    } else if (tag == 0x18U) {
        if (value_len != 15U || value[14] != 'Z' || parse_digits(value, 4U, &year) != 0) return -1;
        off = 4U;
    } else {
        return -1;
    }
    if (parse_digits(value + off, 2U, &month) != 0 || parse_digits(value + off + 2U, 2U, &day) != 0 ||
        parse_digits(value + off + 4U, 2U, &hour) != 0 || parse_digits(value + off + 6U, 2U, &minute) != 0 ||
        parse_digits(value + off + 8U, 2U, &second) != 0 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
        return -1;
    }
    *epoch_out = days_from_civil(year, (unsigned int)month, (unsigned int)day) * 86400LL +
                 (long long)hour * 3600LL + (long long)minute * 60LL + (long long)second;
    return 0;
}

static int add_dns_name(X509Parsed *cert, const unsigned char *name, size_t name_len) {
    if (cert->dns_name_count >= X509_MAX_DNS_NAMES || name_len == 0U || name_len >= X509_MAX_DNS_NAME_LEN) {
        return 0;
    }
    memcpy(cert->dns_names[cert->dns_name_count], name, name_len);
    cert->dns_names[cert->dns_name_count][name_len] = '\0';
    cert->dns_name_count += 1U;
    return 0;
}

static void parse_subject_alt_name(X509Parsed *cert, const unsigned char *der, size_t der_len) {
    struct der_tlv seq;
    struct der_tlv name;
    size_t pos = 0U;

    if (der_expect_tlv(der, der_len, &pos, 0x30U, &seq) != 0 || pos != der_len) {
        return;
    }
    pos = 0U;
    while (pos < seq.value_len) {
        if (der_read_tlv(seq.value, seq.value_len, &pos, &name) != 0) {
            return;
        }
        if (name.tag == 0x82U) {
            (void)add_dns_name(cert, name.value, name.value_len);
        }
    }
}

static void parse_basic_constraints(X509Parsed *cert, const unsigned char *der, size_t der_len) {
    struct der_tlv seq;
    struct der_tlv field;
    size_t pos = 0U;

    if (der_expect_tlv(der, der_len, &pos, 0x30U, &seq) != 0 || pos != der_len) {
        return;
    }
    pos = 0U;
    if (pos < seq.value_len && der_read_tlv(seq.value, seq.value_len, &pos, &field) == 0 && field.tag == 0x01U && field.value_len == 1U && field.value[0] != 0U) {
        cert->is_ca = 1;
    }
}

static void parse_extensions(X509Parsed *cert, const unsigned char *der, size_t der_len) {
    struct der_tlv outer;
    struct der_tlv ext;
    size_t pos = 0U;
    size_t ext_pos;

    if (der_expect_tlv(der, der_len, &pos, 0x30U, &outer) != 0 || pos != der_len) {
        return;
    }
    pos = 0U;
    while (pos < outer.value_len) {
        const unsigned char *oid;
        size_t oid_len;
        struct der_tlv maybe;
        struct der_tlv octets;

        if (der_expect_tlv(outer.value, outer.value_len, &pos, 0x30U, &ext) != 0) {
            return;
        }
        ext_pos = 0U;
        if (der_read_oid(ext.value, ext.value_len, &ext_pos, &oid, &oid_len) != 0) {
            return;
        }
        if (ext_pos < ext.value_len && ext.value[ext_pos] == 0x01U) {
            if (der_read_tlv(ext.value, ext.value_len, &ext_pos, &maybe) != 0) return;
        }
        if (der_expect_tlv(ext.value, ext.value_len, &ext_pos, 0x04U, &octets) != 0) {
            return;
        }
        if (oid_is(oid, oid_len, g_oid_subject_alt_name, sizeof(g_oid_subject_alt_name))) {
            parse_subject_alt_name(cert, octets.value, octets.value_len);
        } else if (oid_is(oid, oid_len, g_oid_basic_constraints, sizeof(g_oid_basic_constraints))) {
            parse_basic_constraints(cert, octets.value, octets.value_len);
        }
    }
}

static int parse_certificate(X509Parsed *cert, const unsigned char *der, size_t der_len) {
    struct der_tlv cert_seq;
    struct der_tlv tbs;
    struct der_tlv field;
    struct der_tlv validity;
    struct der_tlv time_field;
    struct der_tlv sig_alg;
    struct der_tlv sig_bits;
    size_t pos = 0U;
    size_t cert_pos = 0U;
    size_t tbs_pos = 0U;
    size_t validity_pos = 0U;

    if (cert == 0 || der == 0 || der_len == 0U || der_len > X509_MAX_DER_SIZE) {
        return -1;
    }
    memset(cert, 0, sizeof(*cert));
    cert->der = der;
    cert->der_len = der_len;
    if (der_expect_tlv(der, der_len, &pos, 0x30U, &cert_seq) != 0 || pos != der_len ||
        der_expect_tlv(cert_seq.value, cert_seq.value_len, &cert_pos, 0x30U, &tbs) != 0) {
        return -1;
    }
    cert->tbs = tbs.tlv;
    cert->tbs_len = tbs.tlv_len;
    if (tbs_pos < tbs.value_len && tbs.value[tbs_pos] == 0xa0U) {
        if (der_read_tlv(tbs.value, tbs.value_len, &tbs_pos, &field) != 0) return -1;
    }
    if (der_expect_tlv(tbs.value, tbs.value_len, &tbs_pos, 0x02U, &field) != 0 ||
        der_read_tlv(tbs.value, tbs.value_len, &tbs_pos, &field) != 0 ||
        der_read_tlv(tbs.value, tbs.value_len, &tbs_pos, &field) != 0 || field.tag != 0x30U) {
        return -1;
    }
    cert->issuer = field.tlv;
    cert->issuer_len = field.tlv_len;
    if (der_expect_tlv(tbs.value, tbs.value_len, &tbs_pos, 0x30U, &validity) != 0 ||
        der_read_tlv(validity.value, validity.value_len, &validity_pos, &time_field) != 0 ||
        parse_time(time_field.value, time_field.value_len, time_field.tag, &cert->not_before) != 0 ||
        der_read_tlv(validity.value, validity.value_len, &validity_pos, &time_field) != 0 ||
        parse_time(time_field.value, time_field.value_len, time_field.tag, &cert->not_after) != 0 ||
        validity_pos != validity.value_len ||
        der_read_tlv(tbs.value, tbs.value_len, &tbs_pos, &field) != 0 || field.tag != 0x30U) {
        return -1;
    }
    cert->subject = field.tlv;
    cert->subject_len = field.tlv_len;
    if (der_read_tlv(tbs.value, tbs.value_len, &tbs_pos, &field) != 0 || parse_rsa_spki(cert, field.tlv, field.tlv_len) != 0) {
        return -1;
    }
    while (tbs_pos < tbs.value_len) {
        if (der_read_tlv(tbs.value, tbs.value_len, &tbs_pos, &field) != 0) return -1;
        if (field.tag == 0xa3U) {
            parse_extensions(cert, field.value, field.value_len);
        }
    }
    if (der_read_tlv(cert_seq.value, cert_seq.value_len, &cert_pos, &sig_alg) != 0 ||
        parse_algorithm_hash(sig_alg.tlv, sig_alg.tlv_len, &cert->signature_hash_id) != 0 ||
        der_expect_tlv(cert_seq.value, cert_seq.value_len, &cert_pos, 0x03U, &sig_bits) != 0 ||
        sig_bits.value_len < 2U || sig_bits.value[0] != 0U || cert_pos != cert_seq.value_len) {
        return -1;
    }
    cert->signature = sig_bits.value + 1U;
    cert->signature_len = sig_bits.value_len - 1U;
    return 0;
}

static int ascii_lower(int ch) {
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

static int dns_equal(const char *pattern, const char *hostname) {
    size_t i = 0U;

    while (pattern[i] != '\0' && hostname[i] != '\0') {
        if (ascii_lower((unsigned char)pattern[i]) != ascii_lower((unsigned char)hostname[i])) {
            return 0;
        }
        i += 1U;
    }
    return pattern[i] == '\0' && hostname[i] == '\0';
}

static int dns_match_wildcard(const char *pattern, const char *hostname) {
    const char *suffix;
    const char *host_suffix;
    size_t suffix_len;
    size_t host_len;

    if (pattern[0] != '*' || pattern[1] != '.') {
        return dns_equal(pattern, hostname);
    }
    suffix = pattern + 1;
    suffix_len = rt_strlen(suffix);
    host_len = rt_strlen(hostname);
    if (host_len <= suffix_len) {
        return 0;
    }
    host_suffix = hostname + host_len - suffix_len;
    if (!dns_equal(suffix, host_suffix)) {
        return 0;
    }
    while (hostname < host_suffix) {
        if (*hostname == '.') {
            return 0;
        }
        hostname += 1;
    }
    return 1;
}

static int verify_hostname(const X509Parsed *leaf, const char *hostname) {
    size_t i;

    if (hostname == 0 || hostname[0] == '\0' || leaf->dns_name_count == 0U) {
        return 0;
    }
    for (i = 0; i < leaf->dns_name_count; ++i) {
        if (dns_match_wildcard(leaf->dns_names[i], hostname)) {
            return 1;
        }
    }
    return 0;
}

static int hash_tbs(const X509Parsed *cert, unsigned char *digest, size_t *digest_len) {
    if (cert->signature_hash_id == CRYPTO_RSA_HASH_SHA384) {
        crypto_sha384_hash(cert->tbs, cert->tbs_len, digest);
        *digest_len = CRYPTO_SHA384_DIGEST_SIZE;
        return 0;
    }
    if (cert->signature_hash_id == CRYPTO_RSA_HASH_SHA256) {
        crypto_sha256_hash(cert->tbs, cert->tbs_len, digest);
        *digest_len = CRYPTO_SHA256_DIGEST_SIZE;
        return 0;
    }
    return -1;
}

static int verify_cert_signature(const X509Parsed *cert, const X509Parsed *issuer) {
    unsigned char digest[CRYPTO_SHA384_DIGEST_SIZE];
    size_t digest_len = 0U;

    if (hash_tbs(cert, digest, &digest_len) != 0) {
        return -1;
    }
    return crypto_rsa_pkcs1_v15_verify_digest(
        issuer->rsa_modulus,
        issuer->rsa_modulus_len,
        issuer->rsa_exponent,
        issuer->rsa_exponent_len,
        cert->signature,
        cert->signature_len,
        digest,
        digest_len,
        cert->signature_hash_id
    );
}

static int base64_value(int ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static int pem_decode_next_cert(const unsigned char *pem, size_t pem_len, size_t *pos, unsigned char *der, size_t der_cap, size_t *der_len) {
    const char *begin = "-----BEGIN CERTIFICATE-----";
    const char *end = "-----END CERTIFICATE-----";
    size_t begin_len = rt_strlen(begin);
    size_t end_len = rt_strlen(end);
    size_t i;
    unsigned int accum = 0U;
    unsigned int bits = 0U;
    size_t out = 0U;
    int found = 0;

    for (i = *pos; i + begin_len <= pem_len; ++i) {
        if (memcmp(pem + i, begin, begin_len) == 0) {
            i += begin_len;
            found = 1;
            break;
        }
    }
    if (!found) return 1;
    while (i < pem_len) {
        int v;
        if (i + end_len <= pem_len && memcmp(pem + i, end, end_len) == 0) {
            *pos = i + end_len;
            *der_len = out;
            return out != 0U ? 0 : -1;
        }
        if (pem[i] == '=') {
            i += 1U;
            continue;
        }
        v = base64_value(pem[i]);
        i += 1U;
        if (v < 0) {
            continue;
        }
        accum = (accum << 6U) | (unsigned int)v;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            if (out >= der_cap) return -1;
            der[out++] = (unsigned char)(accum >> bits);
            accum &= (1U << bits) - 1U;
        }
    }
    return -1;
}

int crypto_x509_verify_chain(
    const CryptoX509DerCert *chain,
    size_t chain_count,
    const char *hostname,
    long long now_epoch_seconds,
    const unsigned char *trust_pem,
    size_t trust_pem_length,
    char *status,
    size_t status_size
) {
    X509Parsed parsed[X509_MAX_CERTS];
    unsigned char anchor_der[X509_MAX_DER_SIZE];
    X509Parsed anchor;
    size_t i;
    size_t pem_pos = 0U;

    if (chain == 0 || chain_count == 0U || chain_count > X509_MAX_CERTS || trust_pem == 0 || trust_pem_length == 0U) {
        x509_status(status, status_size, "invalid x509 verification input");
        return -1;
    }
    for (i = 0; i < chain_count; ++i) {
        if (parse_certificate(&parsed[i], chain[i].data, chain[i].length) != 0) {
            x509_status(status, status_size, "certificate parse failed");
            return -1;
        }
        if (now_epoch_seconds < parsed[i].not_before || now_epoch_seconds > parsed[i].not_after) {
            x509_status(status, status_size, "certificate expired or not yet valid");
            return -1;
        }
    }
    if (!verify_hostname(&parsed[0], hostname)) {
        x509_status(status, status_size, "certificate hostname mismatch");
        return -1;
    }
    for (i = 0; i + 1U < chain_count; ++i) {
        if (!bytes_equal(parsed[i].issuer, parsed[i].issuer_len, parsed[i + 1U].subject, parsed[i + 1U].subject_len) || !parsed[i + 1U].is_ca ||
            verify_cert_signature(&parsed[i], &parsed[i + 1U]) != 0) {
            x509_status(status, status_size, "certificate chain signature failed");
            return -1;
        }
    }
    for (;;) {
        size_t anchor_len = 0U;
        int result = pem_decode_next_cert(trust_pem, trust_pem_length, &pem_pos, anchor_der, sizeof(anchor_der), &anchor_len);
        if (result == 1) break;
        if (result != 0 || parse_certificate(&anchor, anchor_der, anchor_len) != 0) {
            continue;
        }
        if (now_epoch_seconds < anchor.not_before || now_epoch_seconds > anchor.not_after) {
            continue;
        }
        if (bytes_equal(parsed[chain_count - 1U].issuer, parsed[chain_count - 1U].issuer_len, anchor.subject, anchor.subject_len) && anchor.is_ca &&
            verify_cert_signature(&parsed[chain_count - 1U], &anchor) == 0) {
            x509_status(status, status_size, "trusted");
            return 0;
        }
    }
    x509_status(status, status_size, "no trusted root matched certificate chain");
    return -1;
}

int crypto_x509_verify_tls13_certificate_verify(
    const unsigned char *leaf_der,
    size_t leaf_der_length,
    unsigned short signature_scheme,
    const unsigned char *signed_content,
    size_t signed_content_length,
    const unsigned char *signature,
    size_t signature_length
) {
    X509Parsed leaf;
    unsigned char digest[CRYPTO_SHA384_DIGEST_SIZE];
    int hash_id;
    size_t digest_len;

    if (parse_certificate(&leaf, leaf_der, leaf_der_length) != 0 || signed_content == 0 || signature == 0) {
        return -1;
    }
    if (signature_scheme == TLS_SIG_RSA_PSS_RSAE_SHA256) {
        crypto_sha256_hash(signed_content, signed_content_length, digest);
        hash_id = CRYPTO_RSA_HASH_SHA256;
        digest_len = CRYPTO_SHA256_DIGEST_SIZE;
    } else if (signature_scheme == TLS_SIG_RSA_PSS_RSAE_SHA384) {
        crypto_sha384_hash(signed_content, signed_content_length, digest);
        hash_id = CRYPTO_RSA_HASH_SHA384;
        digest_len = CRYPTO_SHA384_DIGEST_SIZE;
    } else {
        return -1;
    }
    return crypto_rsa_pss_verify_digest(
        leaf.rsa_modulus,
        leaf.rsa_modulus_len,
        leaf.rsa_exponent,
        leaf.rsa_exponent_len,
        signature,
        signature_length,
        digest,
        digest_len,
        hash_id
    );
}

int crypto_x509_get_rsa_public_key(
    const unsigned char *leaf_der,
    size_t leaf_der_length,
    unsigned char *modulus,
    size_t modulus_cap,
    size_t *modulus_len_out,
    unsigned char *exponent,
    size_t exponent_cap,
    size_t *exponent_len_out
) {
    X509Parsed leaf;

    if (parse_certificate(&leaf, leaf_der, leaf_der_length) != 0 || modulus == 0 || exponent == 0 || modulus_len_out == 0 || exponent_len_out == 0 ||
        leaf.rsa_modulus_len > modulus_cap || leaf.rsa_exponent_len > exponent_cap) {
        return -1;
    }
    memcpy(modulus, leaf.rsa_modulus, leaf.rsa_modulus_len);
    memcpy(exponent, leaf.rsa_exponent, leaf.rsa_exponent_len);
    *modulus_len_out = leaf.rsa_modulus_len;
    *exponent_len_out = leaf.rsa_exponent_len;
    return 0;
}
