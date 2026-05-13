#include "tls/tls12_client.h"

#include "crypto/aes128_gcm.h"
#include "crypto/crypto_util.h"
#include "crypto/hmac_sha256.h"
#include "crypto/rsa.h"
#include "crypto/sha512.h"
#include "platform.h"
#include "runtime.h"

#define TLS12_CONTENT_CHANGE_CIPHER_SPEC 20U
#define TLS12_CONTENT_ALERT 21U
#define TLS12_CONTENT_HANDSHAKE 22U
#define TLS12_CONTENT_APPLICATION_DATA 23U

#define TLS12_HS_CLIENT_HELLO 1U
#define TLS12_HS_SERVER_HELLO 2U
#define TLS12_HS_CERTIFICATE 11U
#define TLS12_HS_SERVER_HELLO_DONE 14U
#define TLS12_HS_CLIENT_KEY_EXCHANGE 16U
#define TLS12_HS_FINISHED 20U

#define TLS12_RSA_WITH_AES_256_GCM_SHA384 0x009dU
#define TLS12_MASTER_SECRET_SIZE 48U
#define TLS12_PREMASTER_SECRET_SIZE 48U
#define TLS12_VERIFY_DATA_SIZE 12U
#define TLS12_GCM_TAG_SIZE 16U
#define TLS12_GCM_EXPLICIT_NONCE_SIZE 8U
#define TLS12_MAX_HANDSHAKE_TRANSCRIPT 65536U

static int read_exact_timeout(int fd, void *buf, size_t len, unsigned int timeout_ms) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0U;
    while (got < len) {
        size_t ready_index = 0U;
        if (platform_poll_fds(&fd, 1U, &ready_index, (int)timeout_ms) <= 0) return 0;
        long result = platform_read(fd, p + got, len - got);
        if (result <= 0) return 0;
        got += (size_t)result;
    }
    return 1;
}

static int write_all_timeout(int fd, const void *buf, size_t len, unsigned int timeout_ms) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t off = 0U;
    (void)timeout_ms;
    while (off < len) {
        long result = platform_write(fd, p + off, len - off);
        if (result <= 0) return 0;
        off += (size_t)result;
    }
    return 1;
}

static void store_u16(unsigned char *p, unsigned int value) {
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static void store_u24(unsigned char *p, unsigned int value) {
    p[0] = (unsigned char)(value >> 16);
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)value;
}

static void store_u64(unsigned char *p, unsigned long long value) {
    p[0] = (unsigned char)(value >> 56);
    p[1] = (unsigned char)(value >> 48);
    p[2] = (unsigned char)(value >> 40);
    p[3] = (unsigned char)(value >> 32);
    p[4] = (unsigned char)(value >> 24);
    p[5] = (unsigned char)(value >> 16);
    p[6] = (unsigned char)(value >> 8);
    p[7] = (unsigned char)value;
}

static unsigned int load_u16(const unsigned char *p) {
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

static unsigned int load_u24(const unsigned char *p) {
    return ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | (unsigned int)p[2];
}

static int tls12_fail(Tls12Client *c, const char *message) {
    if (c != 0) c->last_error = message;
    return -1;
}

void tls12_client_init(Tls12Client *c, int fd, unsigned int timeout_ms) {
    if (c == 0) return;
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->timeout_ms = timeout_ms;
    c->last_error = "none";
}

const char *tls12_client_last_error(const Tls12Client *c) {
    if (c == 0 || c->last_error == 0) return "unknown tls12 error";
    return c->last_error;
}

size_t tls12_client_peer_certificates(const Tls12Client *c, CryptoX509DerCert *certs, size_t cert_capacity) {
    size_t i;
    size_t count;

    if (c == 0 || certs == 0 || cert_capacity == 0U) return 0U;
    count = c->peer_cert_count < cert_capacity ? c->peer_cert_count : cert_capacity;
    for (i = 0U; i < count; ++i) {
        certs[i].data = c->peer_cert_der[i];
        certs[i].length = c->peer_cert_len[i];
    }
    return count;
}

static void tls12_hmac_sha384_vec(unsigned char out[CRYPTO_SHA384_DIGEST_SIZE], const unsigned char *key, size_t key_len, const unsigned char *a, size_t a_len, const unsigned char *b, size_t b_len) {
    unsigned char buffer[256];

    if (a_len + b_len <= sizeof(buffer)) {
        if (a_len != 0U) memcpy(buffer, a, a_len);
        if (b_len != 0U) memcpy(buffer + a_len, b, b_len);
        crypto_hmac_sha384(out, key, key_len, buffer, a_len + b_len);
    } else {
        out[0] = 0U;
    }
    crypto_secure_bzero(buffer, sizeof(buffer));
}

static int tls12_prf_sha384(const unsigned char *secret, size_t secret_len, const char *label, const unsigned char *seed, size_t seed_len, unsigned char *out, size_t out_len) {
    unsigned char a[CRYPTO_SHA384_DIGEST_SIZE];
    unsigned char h[CRYPTO_SHA384_DIGEST_SIZE];
    unsigned char label_seed[160];
    size_t label_len = rt_strlen(label);
    size_t done = 0U;

    if (label_len + seed_len > sizeof(label_seed)) return -1;
    memcpy(label_seed, label, label_len);
    if (seed_len != 0U) memcpy(label_seed + label_len, seed, seed_len);
    crypto_hmac_sha384(a, secret, secret_len, label_seed, label_len + seed_len);
    while (done < out_len) {
        tls12_hmac_sha384_vec(h, secret, secret_len, a, sizeof(a), label_seed, label_len + seed_len);
        size_t n = out_len - done < sizeof(h) ? out_len - done : sizeof(h);
        memcpy(out + done, h, n);
        done += n;
        crypto_hmac_sha384(a, secret, secret_len, a, sizeof(a));
    }
    crypto_secure_bzero(a, sizeof(a));
    crypto_secure_bzero(h, sizeof(h));
    crypto_secure_bzero(label_seed, sizeof(label_seed));
    return 0;
}

static int tls12_append_transcript(Tls12Client *c, unsigned char *transcript, size_t *transcript_len, const unsigned char *msg, size_t msg_len) {
    if (*transcript_len + msg_len > TLS12_MAX_HANDSHAKE_TRANSCRIPT) return tls12_fail(c, "tls12 handshake transcript too large");
    memcpy(transcript + *transcript_len, msg, msg_len);
    *transcript_len += msg_len;
    return 0;
}

static int tls12_write_record(Tls12Client *c, unsigned char type, const unsigned char *payload, size_t payload_len) {
    unsigned char header[5];

    if (payload_len > 0xffffU) return tls12_fail(c, "tls12 plaintext record too large");
    header[0] = type;
    header[1] = 0x03U;
    header[2] = 0x03U;
    store_u16(header + 3, (unsigned int)payload_len);
    if (!write_all_timeout(c->fd, header, sizeof(header), c->timeout_ms) || (payload_len != 0U && !write_all_timeout(c->fd, payload, payload_len, c->timeout_ms))) {
        return tls12_fail(c, "tls12 record write failed");
    }
    return 0;
}

static int tls12_read_record(Tls12Client *c, unsigned char *type_out, unsigned char *payload, size_t payload_cap, size_t *payload_len_out) {
    unsigned char header[5];
    unsigned int length;

    if (!read_exact_timeout(c->fd, header, sizeof(header), c->timeout_ms)) return tls12_fail(c, "tls12 record header read failed");
    if (header[1] != 0x03U || (header[2] != 0x01U && header[2] != 0x03U)) return tls12_fail(c, "tls12 record version rejected");
    length = load_u16(header + 3);
    if (length > payload_cap) return tls12_fail(c, "tls12 record too large");
    if (!read_exact_timeout(c->fd, payload, length, c->timeout_ms)) return tls12_fail(c, "tls12 record payload read failed");
    *type_out = header[0];
    *payload_len_out = length;
    return 0;
}

static int tls12_read_handshake_message(Tls12Client *c, unsigned char *buffer, size_t buffer_cap, size_t *buffer_len, unsigned char *msg, size_t msg_cap, size_t *msg_len_out) {
    while (*buffer_len < 4U || *buffer_len < 4U + (size_t)load_u24(buffer + 1U)) {
        unsigned char type;
        unsigned char payload[32768];
        size_t payload_len = 0U;
        if (tls12_read_record(c, &type, payload, sizeof(payload), &payload_len) != 0) return -1;
        if (type == TLS12_CONTENT_ALERT) return tls12_fail(c, "tls12 plaintext alert");
        if (type != TLS12_CONTENT_HANDSHAKE) return tls12_fail(c, "tls12 expected handshake record");
        if (*buffer_len + payload_len > buffer_cap) return tls12_fail(c, "tls12 handshake buffer too large");
        memcpy(buffer + *buffer_len, payload, payload_len);
        *buffer_len += payload_len;
    }
    size_t msg_len = 4U + (size_t)load_u24(buffer + 1U);
    if (msg_len > msg_cap) return tls12_fail(c, "tls12 handshake message too large");
    memcpy(msg, buffer, msg_len);
    memmove(buffer, buffer + msg_len, *buffer_len - msg_len);
    *buffer_len -= msg_len;
    *msg_len_out = msg_len;
    return 0;
}

static int tls12_build_client_hello(Tls12Client *c, const char *sni, size_t sni_len, unsigned char random_out[32], unsigned char *out, size_t out_cap, size_t *out_len) {
    unsigned char body[512];
    size_t p = 0U;
    size_t ext_len_pos;
    size_t ext_start;

    if (out_cap < sizeof(body) + 4U || sni_len > 255U) return tls12_fail(c, "tls12 client hello too large");
    if (crypto_random_bytes(random_out, 32U) != 0) return tls12_fail(c, "tls12 random failed");
    body[p++] = 0x03U;
    body[p++] = 0x03U;
    memcpy(body + p, random_out, 32U);
    p += 32U;
    body[p++] = 0U;
    store_u16(body + p, 4U);
    p += 2U;
    store_u16(body + p, TLS12_RSA_WITH_AES_256_GCM_SHA384);
    p += 2U;
    store_u16(body + p, 0x00ffU);
    p += 2U;
    body[p++] = 1U;
    body[p++] = 0U;
    ext_len_pos = p;
    p += 2U;
    ext_start = p;
    if (sni != 0 && sni_len != 0U) {
        store_u16(body + p, 0U);
        p += 2U;
        store_u16(body + p, (unsigned int)(5U + sni_len));
        p += 2U;
        store_u16(body + p, (unsigned int)(3U + sni_len));
        p += 2U;
        body[p++] = 0U;
        store_u16(body + p, (unsigned int)sni_len);
        p += 2U;
        memcpy(body + p, sni, sni_len);
        p += sni_len;
    }
    store_u16(body + p, 0xff01U);
    p += 2U;
    store_u16(body + p, 1U);
    p += 2U;
    body[p++] = 0U;
    store_u16(body + ext_len_pos, (unsigned int)(p - ext_start));

    out[0] = TLS12_HS_CLIENT_HELLO;
    store_u24(out + 1U, (unsigned int)p);
    memcpy(out + 4U, body, p);
    *out_len = 4U + p;
    return 0;
}

static int tls12_parse_server_hello(Tls12Client *c, const unsigned char *msg, size_t msg_len, unsigned char server_random[32]) {
    size_t p = 4U;
    unsigned int session_len;
    unsigned int cipher;

    if (msg_len < 42U || msg[0] != TLS12_HS_SERVER_HELLO || msg[1] != 0U || load_u24(msg + 1U) + 4U != msg_len) return tls12_fail(c, "tls12 server hello parse failed");
    if (msg[p] != 0x03U || msg[p + 1U] != 0x03U) return tls12_fail(c, "tls12 server did not select TLS 1.2");
    p += 2U;
    memcpy(server_random, msg + p, 32U);
    p += 32U;
    session_len = msg[p++];
    if (p + session_len + 3U > msg_len) return tls12_fail(c, "tls12 server hello session parse failed");
    p += session_len;
    cipher = load_u16(msg + p);
    p += 2U;
    if (cipher != TLS12_RSA_WITH_AES_256_GCM_SHA384) return tls12_fail(c, "tls12 server selected unsupported cipher");
    if (msg[p++] != 0U) return tls12_fail(c, "tls12 server selected compression");
    return 0;
}

static int tls12_parse_certificate(Tls12Client *c, const unsigned char *msg, size_t msg_len) {
    size_t p = 4U;
    size_t end;
    unsigned int list_len;

    if (msg_len < 7U || msg[0] != TLS12_HS_CERTIFICATE || load_u24(msg + 1U) + 4U != msg_len) return tls12_fail(c, "tls12 certificate parse failed");
    list_len = load_u24(msg + p);
    p += 3U;
    if (p + list_len != msg_len) return tls12_fail(c, "tls12 certificate list length failed");
    end = p + list_len;
    c->peer_cert_count = 0U;
    while (p < end) {
        unsigned int cert_len;
        if (p + 3U > end) return tls12_fail(c, "tls12 certificate length failed");
        cert_len = load_u24(msg + p);
        p += 3U;
        if (cert_len == 0U || p + cert_len > end || cert_len > TLS12_MAX_PEER_CERT_DER_SIZE) return tls12_fail(c, "tls12 certificate too large");
        if (c->peer_cert_count < TLS12_MAX_PEER_CERTS) {
            memcpy(c->peer_cert_der[c->peer_cert_count], msg + p, cert_len);
            c->peer_cert_len[c->peer_cert_count] = cert_len;
            c->peer_cert_count += 1U;
        }
        p += cert_len;
    }
    return c->peer_cert_count != 0U ? 0 : tls12_fail(c, "tls12 no peer certificate");
}

static int tls12_write_handshake_record(Tls12Client *c, unsigned char *transcript, size_t *transcript_len, const unsigned char *msg, size_t msg_len) {
    if (tls12_append_transcript(c, transcript, transcript_len, msg, msg_len) != 0) return -1;
    return tls12_write_record(c, TLS12_CONTENT_HANDSHAKE, msg, msg_len);
}

static int tls12_make_key_block(Tls12Client *c, const unsigned char premaster[48], const unsigned char client_random[32], const unsigned char server_random[32]) {
    unsigned char seed[64];
    unsigned char master[TLS12_MASTER_SECRET_SIZE];
    unsigned char key_block[72];

    memcpy(seed, client_random, 32U);
    memcpy(seed + 32U, server_random, 32U);
    if (tls12_prf_sha384(premaster, 48U, "master secret", seed, sizeof(seed), master, sizeof(master)) != 0) return tls12_fail(c, "tls12 master secret failed");
    memcpy(seed, server_random, 32U);
    memcpy(seed + 32U, client_random, 32U);
    if (tls12_prf_sha384(master, sizeof(master), "key expansion", seed, sizeof(seed), key_block, sizeof(key_block)) != 0) return tls12_fail(c, "tls12 key expansion failed");
    memcpy(c->client_write_key, key_block, 32U);
    memcpy(c->server_write_key, key_block + 32U, 32U);
    memcpy(c->client_write_iv, key_block + 64U, 4U);
    memcpy(c->server_write_iv, key_block + 68U, 4U);
    crypto_secure_bzero(master, sizeof(master));
    crypto_secure_bzero(key_block, sizeof(key_block));
    return 0;
}

static void tls12_make_aad(unsigned char aad[13], unsigned long long seq, unsigned char type, size_t plaintext_len) {
    store_u64(aad, seq);
    aad[8] = type;
    aad[9] = 0x03U;
    aad[10] = 0x03U;
    store_u16(aad + 11U, (unsigned int)plaintext_len);
}

static int tls12_write_encrypted_record(Tls12Client *c, unsigned char type, const unsigned char *plaintext, size_t plaintext_len) {
    unsigned char record[5 + TLS12_GCM_EXPLICIT_NONCE_SIZE + 16384 + TLS12_GCM_TAG_SIZE];
    unsigned char aad[13];
    unsigned char nonce[12];
    unsigned char tag[16];
    size_t record_len;

    if (plaintext_len > 16384U) return tls12_fail(c, "tls12 encrypted record too large");
    record[0] = type;
    record[1] = 0x03U;
    record[2] = 0x03U;
    store_u16(record + 3U, (unsigned int)(TLS12_GCM_EXPLICIT_NONCE_SIZE + plaintext_len + TLS12_GCM_TAG_SIZE));
    memcpy(nonce, c->client_write_iv, 4U);
    store_u64(nonce + 4U, c->client_seq);
    memcpy(record + 5U, nonce + 4U, TLS12_GCM_EXPLICIT_NONCE_SIZE);
    tls12_make_aad(aad, c->client_seq, type, plaintext_len);
    if (crypto_aes256_gcm_encrypt(c->client_write_key, nonce, aad, sizeof(aad), plaintext, plaintext_len, record + 5U + TLS12_GCM_EXPLICIT_NONCE_SIZE, tag) != 0) {
        return tls12_fail(c, "tls12 encrypt failed");
    }
    memcpy(record + 5U + TLS12_GCM_EXPLICIT_NONCE_SIZE + plaintext_len, tag, sizeof(tag));
    record_len = 5U + TLS12_GCM_EXPLICIT_NONCE_SIZE + plaintext_len + TLS12_GCM_TAG_SIZE;
    c->client_seq += 1U;
    return write_all_timeout(c->fd, record, record_len, c->timeout_ms) ? 0 : tls12_fail(c, "tls12 encrypted write failed");
}

static int tls12_read_encrypted_record(Tls12Client *c, unsigned char *type_out, unsigned char *plaintext, size_t plaintext_cap, size_t *plaintext_len_out) {
    unsigned char type;
    unsigned char payload[32768];
    size_t payload_len = 0U;
    unsigned char aad[13];
    unsigned char nonce[12];
    const unsigned char *ciphertext;
    const unsigned char *tag;
    size_t ciphertext_len;

    if (tls12_read_record(c, &type, payload, sizeof(payload), &payload_len) != 0) return -1;
    if (payload_len < TLS12_GCM_EXPLICIT_NONCE_SIZE + TLS12_GCM_TAG_SIZE) return tls12_fail(c, "tls12 encrypted record too short");
    ciphertext_len = payload_len - TLS12_GCM_EXPLICIT_NONCE_SIZE - TLS12_GCM_TAG_SIZE;
    if (ciphertext_len > plaintext_cap) return tls12_fail(c, "tls12 decrypted record too large");
    memcpy(nonce, c->server_write_iv, 4U);
    memcpy(nonce + 4U, payload, TLS12_GCM_EXPLICIT_NONCE_SIZE);
    ciphertext = payload + TLS12_GCM_EXPLICIT_NONCE_SIZE;
    tag = ciphertext + ciphertext_len;
    tls12_make_aad(aad, c->server_seq, type, ciphertext_len);
    if (crypto_aes256_gcm_decrypt(c->server_write_key, nonce, aad, sizeof(aad), ciphertext, ciphertext_len, tag, plaintext) != 0) {
        return tls12_fail(c, "tls12 decrypt failed");
    }
    c->server_seq += 1U;
    *type_out = type;
    *plaintext_len_out = ciphertext_len;
    return 0;
}

static int tls12_finished_verify_data(Tls12Client *c, const unsigned char master[48], const char *label, const unsigned char *transcript, size_t transcript_len, unsigned char out[12]) {
    unsigned char hash[CRYPTO_SHA384_DIGEST_SIZE];
    crypto_sha384_hash(transcript, transcript_len, hash);
    if (tls12_prf_sha384(master, 48U, label, hash, sizeof(hash), out, 12U) != 0) return tls12_fail(c, "tls12 finished prf failed");
    return 0;
}

int tls12_client_handshake(Tls12Client *c, const char *sni, size_t sni_len) {
    unsigned char transcript[TLS12_MAX_HANDSHAKE_TRANSCRIPT];
    size_t transcript_len = 0U;
    unsigned char hs_buffer[32768];
    size_t hs_buffer_len = 0U;
    unsigned char msg[32768];
    size_t msg_len = 0U;
    unsigned char client_random[32];
    unsigned char server_random[32];
    unsigned char premaster[TLS12_PREMASTER_SECRET_SIZE];
    unsigned char master[TLS12_MASTER_SECRET_SIZE];
    unsigned char seed[64];
    unsigned char modulus[CRYPTO_RSA_MAX_MODULUS_SIZE];
    unsigned char exponent[8];
    size_t modulus_len = 0U;
    size_t exponent_len = 0U;
    unsigned char encrypted[CRYPTO_RSA_MAX_MODULUS_SIZE];
    size_t encrypted_len = 0U;
    unsigned char type;
    unsigned char verify_data[TLS12_VERIFY_DATA_SIZE];
    int got_cert = 0;

    if (c == 0) return -1;
    c->last_error = "none";
    if (c->fd < 0) return tls12_fail(c, "tls12 invalid socket");
    if (sni != 0 && (sni_len == 0U || sni_len > 255U)) return tls12_fail(c, "tls12 invalid sni");

    if (tls12_build_client_hello(c, sni, sni_len, client_random, msg, sizeof(msg), &msg_len) != 0) return -1;
    if (tls12_write_handshake_record(c, transcript, &transcript_len, msg, msg_len) != 0) return -1;

    if (tls12_read_handshake_message(c, hs_buffer, sizeof(hs_buffer), &hs_buffer_len, msg, sizeof(msg), &msg_len) != 0) return -1;
    if (tls12_append_transcript(c, transcript, &transcript_len, msg, msg_len) != 0 || tls12_parse_server_hello(c, msg, msg_len, server_random) != 0) return -1;

    for (;;) {
        if (tls12_read_handshake_message(c, hs_buffer, sizeof(hs_buffer), &hs_buffer_len, msg, sizeof(msg), &msg_len) != 0) return -1;
        if (tls12_append_transcript(c, transcript, &transcript_len, msg, msg_len) != 0) return -1;
        if (msg[0] == TLS12_HS_CERTIFICATE) {
            if (tls12_parse_certificate(c, msg, msg_len) != 0) return -1;
            got_cert = 1;
        } else if (msg[0] == TLS12_HS_SERVER_HELLO_DONE) {
            break;
        } else {
            return tls12_fail(c, "tls12 unexpected server handshake message");
        }
    }
    if (!got_cert) return tls12_fail(c, "tls12 server sent no certificate");

    premaster[0] = 0x03U;
    premaster[1] = 0x03U;
    if (crypto_random_bytes(premaster + 2U, sizeof(premaster) - 2U) != 0) return tls12_fail(c, "tls12 premaster random failed");
    if (crypto_x509_get_rsa_public_key(c->peer_cert_der[0], c->peer_cert_len[0], modulus, sizeof(modulus), &modulus_len, exponent, sizeof(exponent), &exponent_len) != 0) {
        return tls12_fail(c, "tls12 leaf rsa key parse failed");
    }
    if (crypto_rsa_pkcs1_v15_encrypt(encrypted, sizeof(encrypted), &encrypted_len, premaster, sizeof(premaster), modulus, modulus_len, exponent, exponent_len) != 0) {
        return tls12_fail(c, "tls12 rsa premaster encrypt failed");
    }
    msg[0] = TLS12_HS_CLIENT_KEY_EXCHANGE;
    store_u24(msg + 1U, (unsigned int)(2U + encrypted_len));
    store_u16(msg + 4U, (unsigned int)encrypted_len);
    memcpy(msg + 6U, encrypted, encrypted_len);
    msg_len = 6U + encrypted_len;
    if (tls12_write_handshake_record(c, transcript, &transcript_len, msg, msg_len) != 0) return -1;

    memcpy(seed, client_random, 32U);
    memcpy(seed + 32U, server_random, 32U);
    if (tls12_prf_sha384(premaster, sizeof(premaster), "master secret", seed, sizeof(seed), master, sizeof(master)) != 0) return tls12_fail(c, "tls12 master secret failed");
    if (tls12_make_key_block(c, premaster, client_random, server_random) != 0) return -1;

    { unsigned char ccs = 1U; if (tls12_write_record(c, TLS12_CONTENT_CHANGE_CIPHER_SPEC, &ccs, 1U) != 0) return -1; }
    if (tls12_finished_verify_data(c, master, "client finished", transcript, transcript_len, verify_data) != 0) return -1;
    msg[0] = TLS12_HS_FINISHED;
    store_u24(msg + 1U, TLS12_VERIFY_DATA_SIZE);
    memcpy(msg + 4U, verify_data, TLS12_VERIFY_DATA_SIZE);
    msg_len = 4U + TLS12_VERIFY_DATA_SIZE;
    if (tls12_write_encrypted_record(c, TLS12_CONTENT_HANDSHAKE, msg, msg_len) != 0) return -1;
    if (tls12_append_transcript(c, transcript, &transcript_len, msg, msg_len) != 0) return -1;

    {
        unsigned char payload[64];
        size_t payload_len = 0U;
        if (tls12_read_record(c, &type, payload, sizeof(payload), &payload_len) != 0) return -1;
        if (type != TLS12_CONTENT_CHANGE_CIPHER_SPEC || payload_len != 1U || payload[0] != 1U) return tls12_fail(c, "tls12 server ccs failed");
    }
    if (tls12_read_encrypted_record(c, &type, msg, sizeof(msg), &msg_len) != 0) return -1;
    if (type != TLS12_CONTENT_HANDSHAKE || msg_len != 4U + TLS12_VERIFY_DATA_SIZE || msg[0] != TLS12_HS_FINISHED) return tls12_fail(c, "tls12 server finished message failed");
    if (tls12_finished_verify_data(c, master, "server finished", transcript, transcript_len, verify_data) != 0) return -1;
    if (!crypto_constant_time_equal(msg + 4U, verify_data, TLS12_VERIFY_DATA_SIZE)) return tls12_fail(c, "tls12 server finished verify failed");
    if (tls12_append_transcript(c, transcript, &transcript_len, msg, msg_len) != 0) return -1;

    c->handshake_done = 1;
    crypto_secure_bzero(transcript, sizeof(transcript));
    crypto_secure_bzero(premaster, sizeof(premaster));
    crypto_secure_bzero(master, sizeof(master));
    crypto_secure_bzero(encrypted, sizeof(encrypted));
    return 0;
}

long tls12_client_write_app(Tls12Client *c, const unsigned char *buf, size_t len) {
    size_t written = 0U;

    if (c == 0 || (buf == 0 && len != 0U) || !c->handshake_done) return -1;
    while (written < len) {
        size_t n = len - written < 16384U ? len - written : 16384U;
        if (tls12_write_encrypted_record(c, TLS12_CONTENT_APPLICATION_DATA, buf + written, n) != 0) return -1;
        written += n;
    }
    return (long)written;
}

static long tls12_deliver_pending(Tls12Client *c, unsigned char *buf, size_t cap) {
    size_t available = c->pending_app_len - c->pending_app_offset;
    size_t n = available < cap ? available : cap;
    if (n != 0U) memcpy(buf, c->pending_app + c->pending_app_offset, n);
    c->pending_app_offset += n;
    if (c->pending_app_offset >= c->pending_app_len) {
        c->pending_app_len = 0U;
        c->pending_app_offset = 0U;
    }
    return (long)n;
}

long tls12_client_read_app(Tls12Client *c, unsigned char *buf, size_t cap) {
    unsigned char type;
    unsigned char plaintext[32768];
    size_t plaintext_len = 0U;

    if (c == 0 || buf == 0 || cap == 0U || !c->handshake_done) return -1;
    if (c->pending_app_len > c->pending_app_offset) return tls12_deliver_pending(c, buf, cap);
    for (;;) {
        if (tls12_read_encrypted_record(c, &type, plaintext, sizeof(plaintext), &plaintext_len) != 0) return -1;
        if (type == TLS12_CONTENT_ALERT) {
            if (plaintext_len >= 2U && plaintext[1] == 0U) return 0;
            return tls12_fail(c, "tls12 encrypted alert");
        }
        if (type != TLS12_CONTENT_APPLICATION_DATA) continue;
        if (plaintext_len > sizeof(c->pending_app)) return tls12_fail(c, "tls12 app data too large");
        memcpy(c->pending_app, plaintext, plaintext_len);
        c->pending_app_len = plaintext_len;
        c->pending_app_offset = 0U;
        return tls12_deliver_pending(c, buf, cap);
    }
}

int tls12_client_close_notify(Tls12Client *c) {
    unsigned char alert[2];
    if (c == 0 || !c->handshake_done) return 0;
    alert[0] = 1U;
    alert[1] = 0U;
    return tls12_write_encrypted_record(c, TLS12_CONTENT_ALERT, alert, sizeof(alert));
}
