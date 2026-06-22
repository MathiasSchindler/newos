#include "tls/tls13_server.h"

#include "crypto/crypto_util.h"
#include "crypto/curve25519.h"
#include "crypto/hkdf_sha256.h"
#include "crypto/rsa.h"
#include "crypto/sha256.h"
#include "platform.h"
#include "runtime.h"
#include "tls/tls13.h"
#include "tls/tls13_record.h"
#include "tls/tls13_transcript.h"

#define TLS13_HS_CLIENT_HELLO 1U
#define TLS13_HS_SERVER_HELLO 2U
#define TLS13_HS_ENCRYPTED_EXTENSIONS 8U
#define TLS13_HS_CERTIFICATE 11U
#define TLS13_HS_CERTIFICATE_VERIFY 15U
#define TLS13_HS_FINISHED 20U

#define TLS13_EXT_SUPPORTED_VERSIONS 0x002bU
#define TLS13_EXT_KEY_SHARE 0x0033U
#define TLS13_EXT_SIGNATURE_ALGORITHMS 0x000dU
#define TLS13_GROUP_X25519 0x001dU
#define TLS13_AES_128_GCM_SHA256 0x1301U
#define TLS13_SIG_RSA_PSS_RSAE_SHA256 0x0804U

static const unsigned char *tls13_server_empty_hash(void) {
    return (const unsigned char *)
        "\xe3\xb0\xc4\x42\x98\xfc\x1c\x14\x9a\xfb\xf4\xc8\x99\x6f\xb9\x24"
        "\x27\xae\x41\xe4\x64\x9b\x93\x4c\xa4\x95\x99\x1b\x78\x52\xb8\x55";
}

typedef struct {
    unsigned char random[32];
    unsigned char session_id[32];
    size_t session_id_len;
    unsigned char key_share[32];
    int has_tls13;
    int has_x25519;
    int has_cipher;
    int has_rsa_pss_sha256;
} Tls13ClientHelloInfo;

typedef struct {
    unsigned char *data;
    size_t capacity;
    size_t size;
} Tls13Buffer;

static unsigned short tls13_load_u16(const unsigned char *data) {
    return (unsigned short)(((unsigned short)data[0] << 8U) | (unsigned short)data[1]);
}

static unsigned int tls13_load_u24(const unsigned char *data) {
    return ((unsigned int)data[0] << 16U) | ((unsigned int)data[1] << 8U) | (unsigned int)data[2];
}

static void tls13_store_u16(unsigned char *data, unsigned int value) {
    data[0] = (unsigned char)(value >> 8U);
    data[1] = (unsigned char)value;
}

static void tls13_store_u24(unsigned char *data, unsigned int value) {
    data[0] = (unsigned char)(value >> 16U);
    data[1] = (unsigned char)(value >> 8U);
    data[2] = (unsigned char)value;
}

static int tls13_server_fail(Tls13Server *server, const char *message) {
    if (server != 0) server->last_error = message;
    return -1;
}

static int tls13_read_exact_timeout(int fd, void *buffer, size_t length, unsigned int timeout_ms) {
    unsigned char *bytes = (unsigned char *)buffer;
    size_t done = 0U;

    while (done < length) {
        size_t ready_index = 0U;
        long result;

        if (platform_poll_fds(&fd, 1U, &ready_index, (int)timeout_ms) <= 0) return 0;
        result = platform_read(fd, bytes + done, length - done);
        if (result <= 0) return 0;
        done += (size_t)result;
    }
    return 1;
}

static int tls13_write_all_timeout(int fd, const void *buffer, size_t length, unsigned int timeout_ms) {
    const unsigned char *bytes = (const unsigned char *)buffer;
    size_t done = 0U;

    (void)timeout_ms;
    while (done < length) {
        long result = platform_write(fd, bytes + done, length - done);
        if (result <= 0) return 0;
        done += (size_t)result;
    }
    return 1;
}

static int tls13_read_record(Tls13Server *server, unsigned char header[5], unsigned char *payload, size_t payload_capacity, size_t *payload_length_out) {
    unsigned short length;

    if (!tls13_read_exact_timeout(server->fd, header, 5U, server->timeout_ms)) return tls13_server_fail(server, "tls13 server record header read failed");
    length = tls13_load_u16(header + 3U);
    if ((size_t)length > payload_capacity) return tls13_server_fail(server, "tls13 server record too large");
    if (!tls13_read_exact_timeout(server->fd, payload, (size_t)length, server->timeout_ms)) return tls13_server_fail(server, "tls13 server record payload read failed");
    *payload_length_out = (size_t)length;
    return 0;
}

static int tls13_write_plain_record(Tls13Server *server, unsigned char type, const unsigned char *payload, size_t payload_length) {
    unsigned char header[5];

    if (payload_length > 0xffffU) return tls13_server_fail(server, "tls13 server plaintext record too large");
    header[0] = type;
    header[1] = 0x03U;
    header[2] = 0x03U;
    tls13_store_u16(header + 3U, (unsigned int)payload_length);
    if (!tls13_write_all_timeout(server->fd, header, sizeof(header), server->timeout_ms)) return tls13_server_fail(server, "tls13 server record header write failed");
    if (payload_length > 0U && !tls13_write_all_timeout(server->fd, payload, payload_length, server->timeout_ms)) return tls13_server_fail(server, "tls13 server record payload write failed");
    return 0;
}

static int tls13_buffer_append(Tls13Buffer *buffer, const void *data, size_t length) {
    if (buffer == 0 || (data == 0 && length != 0U) || buffer->size > buffer->capacity || length > buffer->capacity - buffer->size) return -1;
    if (length != 0U) memcpy(buffer->data + buffer->size, data, length);
    buffer->size += length;
    return 0;
}

static int tls13_buffer_u8(Tls13Buffer *buffer, unsigned int value) {
    unsigned char byte = (unsigned char)value;
    return tls13_buffer_append(buffer, &byte, 1U);
}

static int tls13_buffer_u16(Tls13Buffer *buffer, unsigned int value) {
    unsigned char bytes[2];
    tls13_store_u16(bytes, value);
    return tls13_buffer_append(buffer, bytes, sizeof(bytes));
}

static int tls13_buffer_u24(Tls13Buffer *buffer, unsigned int value) {
    unsigned char bytes[3];
    tls13_store_u24(bytes, value);
    return tls13_buffer_append(buffer, bytes, sizeof(bytes));
}

static int tls13_begin_handshake(Tls13Buffer *buffer, unsigned int type, size_t *length_offset_out) {
    if (tls13_buffer_u8(buffer, type) != 0) return -1;
    *length_offset_out = buffer->size;
    return tls13_buffer_u24(buffer, 0U);
}

static int tls13_finish_handshake(Tls13Buffer *buffer, size_t length_offset) {
    size_t body_length;

    if (length_offset + 3U > buffer->size) return -1;
    body_length = buffer->size - length_offset - 3U;
    if (body_length > 0xffffffU) return -1;
    tls13_store_u24(buffer->data + length_offset, (unsigned int)body_length);
    return 0;
}

static int tls13_parse_vector_bounds(size_t pos, size_t length_size, size_t total, const unsigned char *data, size_t *payload_pos_out, size_t *payload_end_out) {
    size_t vector_length = 0U;
    size_t index;

    if (length_size == 0U || length_size > 3U || pos + length_size > total) return -1;
    for (index = 0U; index < length_size; ++index) vector_length = (vector_length << 8U) | (size_t)data[pos + index];
    if (pos + length_size + vector_length > total) return -1;
    *payload_pos_out = pos + length_size;
    *payload_end_out = pos + length_size + vector_length;
    return 0;
}

static int tls13_parse_client_hello(const unsigned char *message, size_t message_length, Tls13ClientHelloInfo *info) {
    const unsigned char *body;
    size_t body_length;
    size_t pos;
    size_t end;
    size_t extension_pos;
    size_t extension_end;

    if (message == 0 || info == 0 || message_length < 4U || message[0] != TLS13_HS_CLIENT_HELLO || tls13_load_u24(message + 1U) + 4U != message_length) return -1;
    rt_memset(info, 0, sizeof(*info));
    body = message + 4U;
    body_length = message_length - 4U;
    if (body_length < 38U || tls13_load_u16(body) != 0x0303U) return -1;
    memcpy(info->random, body + 2U, sizeof(info->random));
    pos = 34U;
    if (pos >= body_length || body[pos] > sizeof(info->session_id) || pos + 1U + body[pos] > body_length) return -1;
    info->session_id_len = body[pos];
    if (info->session_id_len != 0U) memcpy(info->session_id, body + pos + 1U, info->session_id_len);
    pos += 1U + info->session_id_len;
    if (tls13_parse_vector_bounds(pos, 2U, body_length, body, &pos, &end) != 0 || ((end - pos) & 1U) != 0) return -1;
    while (pos + 2U <= end) {
        if (tls13_load_u16(body + pos) == TLS13_AES_128_GCM_SHA256) info->has_cipher = 1;
        pos += 2U;
    }
    if (end >= body_length || body[end] == 0U || end + 1U + body[end] > body_length) return -1;
    pos = end + 1U + body[end];
    if (pos == body_length) return -1;
    if (tls13_parse_vector_bounds(pos, 2U, body_length, body, &extension_pos, &extension_end) != 0) return -1;
    while (extension_pos + 4U <= extension_end) {
        unsigned int extension_type = tls13_load_u16(body + extension_pos);
        size_t extension_length = tls13_load_u16(body + extension_pos + 2U);
        const unsigned char *extension_data;
        size_t extension_data_end;
        size_t cursor;

        extension_pos += 4U;
        if (extension_pos + extension_length > extension_end) return -1;
        extension_data = body + extension_pos;
        extension_data_end = extension_length;
        if (extension_type == TLS13_EXT_SUPPORTED_VERSIONS) {
            if (extension_length < 1U || 1U + extension_data[0] != extension_length || (extension_data[0] & 1U) != 0U) return -1;
            for (cursor = 1U; cursor + 2U <= extension_length; cursor += 2U) {
                if (tls13_load_u16(extension_data + cursor) == 0x0304U) info->has_tls13 = 1;
            }
        } else if (extension_type == TLS13_EXT_KEY_SHARE) {
            size_t share_pos;
            size_t share_end;
            if (tls13_parse_vector_bounds(0U, 2U, extension_length, extension_data, &share_pos, &share_end) != 0) return -1;
            while (share_pos + 4U <= share_end) {
                unsigned int group = tls13_load_u16(extension_data + share_pos);
                size_t key_length = tls13_load_u16(extension_data + share_pos + 2U);
                share_pos += 4U;
                if (share_pos + key_length > share_end) return -1;
                if (group == TLS13_GROUP_X25519 && key_length == 32U) {
                    memcpy(info->key_share, extension_data + share_pos, 32U);
                    info->has_x25519 = 1;
                }
                share_pos += key_length;
            }
        } else if (extension_type == TLS13_EXT_SIGNATURE_ALGORITHMS) {
            size_t algorithm_pos;
            size_t algorithm_end;
            if (tls13_parse_vector_bounds(0U, 2U, extension_length, extension_data, &algorithm_pos, &algorithm_end) != 0 || ((algorithm_end - algorithm_pos) & 1U) != 0) return -1;
            while (algorithm_pos + 2U <= algorithm_end) {
                if (tls13_load_u16(extension_data + algorithm_pos) == TLS13_SIG_RSA_PSS_RSAE_SHA256) info->has_rsa_pss_sha256 = 1;
                algorithm_pos += 2U;
            }
        }
        extension_pos += extension_data_end;
    }
    return info->has_tls13 && info->has_x25519 && info->has_cipher && info->has_rsa_pss_sha256 ? 0 : -1;
}

static int tls13_read_client_hello(Tls13Server *server, unsigned char *message, size_t message_capacity, size_t *message_length_out) {
    unsigned char header[5];
    unsigned char payload[65536];
    size_t payload_length = 0U;

    if (tls13_read_record(server, header, payload, sizeof(payload), &payload_length) != 0) return -1;
    if (header[0] != TLS_CONTENT_HANDSHAKE || payload_length < 4U) return tls13_server_fail(server, "tls13 server expected client hello");
    if (payload[0] != TLS13_HS_CLIENT_HELLO) return tls13_server_fail(server, "tls13 server expected client hello message");
    *message_length_out = tls13_load_u24(payload + 1U) + 4U;
    if (*message_length_out > payload_length || *message_length_out > message_capacity) return tls13_server_fail(server, "tls13 server client hello too large");
    memcpy(message, payload, *message_length_out);
    return 0;
}

static int tls13_build_server_hello(const Tls13ClientHelloInfo *client, const unsigned char server_random[32], const unsigned char server_public_key[32], unsigned char *out, size_t out_capacity, size_t *out_length) {
    Tls13Buffer buffer;
    size_t handshake_length_offset;
    size_t extensions_length_offset;
    size_t extensions_start;
    size_t extensions_length;

    buffer.data = out;
    buffer.capacity = out_capacity;
    buffer.size = 0U;
    if (tls13_begin_handshake(&buffer, TLS13_HS_SERVER_HELLO, &handshake_length_offset) != 0 ||
        tls13_buffer_u16(&buffer, 0x0303U) != 0 ||
        tls13_buffer_append(&buffer, server_random, 32U) != 0 ||
        tls13_buffer_u8(&buffer, (unsigned int)client->session_id_len) != 0 ||
        tls13_buffer_append(&buffer, client->session_id, client->session_id_len) != 0 ||
        tls13_buffer_u16(&buffer, TLS13_AES_128_GCM_SHA256) != 0 ||
        tls13_buffer_u8(&buffer, 0U) != 0) return -1;
    extensions_length_offset = buffer.size;
    if (tls13_buffer_u16(&buffer, 0U) != 0) return -1;
    extensions_start = buffer.size;
    if (tls13_buffer_u16(&buffer, TLS13_EXT_SUPPORTED_VERSIONS) != 0 || tls13_buffer_u16(&buffer, 2U) != 0 || tls13_buffer_u16(&buffer, 0x0304U) != 0) return -1;
    if (tls13_buffer_u16(&buffer, TLS13_EXT_KEY_SHARE) != 0 || tls13_buffer_u16(&buffer, 36U) != 0 || tls13_buffer_u16(&buffer, TLS13_GROUP_X25519) != 0 || tls13_buffer_u16(&buffer, 32U) != 0 || tls13_buffer_append(&buffer, server_public_key, 32U) != 0) return -1;
    extensions_length = buffer.size - extensions_start;
    if (extensions_length > 0xffffU) return -1;
    tls13_store_u16(buffer.data + extensions_length_offset, (unsigned int)extensions_length);
    if (tls13_finish_handshake(&buffer, handshake_length_offset) != 0) return -1;
    *out_length = buffer.size;
    return 0;
}

static int tls13_build_encrypted_extensions(unsigned char *out, size_t out_capacity, size_t *out_length) {
    Tls13Buffer buffer;
    size_t handshake_length_offset;

    buffer.data = out;
    buffer.capacity = out_capacity;
    buffer.size = 0U;
    if (tls13_begin_handshake(&buffer, TLS13_HS_ENCRYPTED_EXTENSIONS, &handshake_length_offset) != 0 || tls13_buffer_u16(&buffer, 0U) != 0 || tls13_finish_handshake(&buffer, handshake_length_offset) != 0) return -1;
    *out_length = buffer.size;
    return 0;
}

static int tls13_build_certificate(Tls13Server *server, unsigned char *out, size_t out_capacity, size_t *out_length) {
    Tls13Buffer buffer;
    size_t handshake_length_offset;
    size_t list_length_offset;
    size_t list_start;
    size_t list_length;

    if (server->credentials.cert_der == 0 || server->credentials.cert_der_len == 0U || server->credentials.cert_der_len > TLS13_SERVER_MAX_CERT_DER_SIZE) return -1;
    buffer.data = out;
    buffer.capacity = out_capacity;
    buffer.size = 0U;
    if (tls13_begin_handshake(&buffer, TLS13_HS_CERTIFICATE, &handshake_length_offset) != 0 || tls13_buffer_u8(&buffer, 0U) != 0) return -1;
    list_length_offset = buffer.size;
    if (tls13_buffer_u24(&buffer, 0U) != 0) return -1;
    list_start = buffer.size;
    if (tls13_buffer_u24(&buffer, (unsigned int)server->credentials.cert_der_len) != 0 || tls13_buffer_append(&buffer, server->credentials.cert_der, server->credentials.cert_der_len) != 0 || tls13_buffer_u16(&buffer, 0U) != 0) return -1;
    list_length = buffer.size - list_start;
    if (list_length > 0xffffffU) return -1;
    tls13_store_u24(buffer.data + list_length_offset, (unsigned int)list_length);
    if (tls13_finish_handshake(&buffer, handshake_length_offset) != 0) return -1;
    *out_length = buffer.size;
    return 0;
}

static int tls13_build_certificate_verify(Tls13Server *server, const unsigned char transcript_hash[32], unsigned char *out, size_t out_capacity, size_t *out_length) {
    static const char context[] = "TLS 1.3, server CertificateVerify";
    unsigned char signed_content[64U + sizeof(context) + 32U];
    unsigned char signature[CRYPTO_RSA2048_MODULUS_SIZE];
    size_t signature_length = 0U;
    Tls13Buffer buffer;
    size_t handshake_length_offset;
    size_t index;

    if (server->credentials.rsa_key == 0) return -1;
    for (index = 0U; index < 64U; ++index) signed_content[index] = 0x20U;
    memcpy(signed_content + 64U, context, sizeof(context) - 1U);
    signed_content[64U + sizeof(context) - 1U] = 0U;
    memcpy(signed_content + 64U + sizeof(context), transcript_hash, 32U);
    if (crypto_rsa2048_pss_sha256_sign(signature, sizeof(signature), &signature_length, signed_content, sizeof(signed_content), server->credentials.rsa_key) != 0) {
        crypto_secure_bzero(signed_content, sizeof(signed_content));
        return -1;
    }
    buffer.data = out;
    buffer.capacity = out_capacity;
    buffer.size = 0U;
    if (tls13_begin_handshake(&buffer, TLS13_HS_CERTIFICATE_VERIFY, &handshake_length_offset) != 0 ||
        tls13_buffer_u16(&buffer, TLS13_SIG_RSA_PSS_RSAE_SHA256) != 0 ||
        tls13_buffer_u16(&buffer, (unsigned int)signature_length) != 0 ||
        tls13_buffer_append(&buffer, signature, signature_length) != 0 ||
        tls13_finish_handshake(&buffer, handshake_length_offset) != 0) {
        crypto_secure_bzero(signed_content, sizeof(signed_content));
        crypto_secure_bzero(signature, sizeof(signature));
        return -1;
    }
    *out_length = buffer.size;
    crypto_secure_bzero(signed_content, sizeof(signed_content));
    crypto_secure_bzero(signature, sizeof(signature));
    return 0;
}

static int tls13_build_finished(const unsigned char traffic_secret[32], const unsigned char transcript_hash[32], unsigned char *out, size_t out_capacity, size_t *out_length) {
    unsigned char finished_key[32];
    unsigned char verify_data[32];
    Tls13Buffer buffer;
    size_t handshake_length_offset;

    if (tls13_finished_key(traffic_secret, finished_key) != 0) return -1;
    tls13_finished_verify_data(finished_key, transcript_hash, verify_data);
    buffer.data = out;
    buffer.capacity = out_capacity;
    buffer.size = 0U;
    if (tls13_begin_handshake(&buffer, TLS13_HS_FINISHED, &handshake_length_offset) != 0 || tls13_buffer_append(&buffer, verify_data, sizeof(verify_data)) != 0 || tls13_finish_handshake(&buffer, handshake_length_offset) != 0) {
        crypto_secure_bzero(finished_key, sizeof(finished_key));
        crypto_secure_bzero(verify_data, sizeof(verify_data));
        return -1;
    }
    *out_length = buffer.size;
    crypto_secure_bzero(finished_key, sizeof(finished_key));
    crypto_secure_bzero(verify_data, sizeof(verify_data));
    return 0;
}

static int tls13_derive_key_iv(const unsigned char secret[32], unsigned char key[16], unsigned char iv[12]) {
    return tls13_hkdf_expand_label(secret, "key", 0, 0, key, 16U) != 0 || tls13_hkdf_expand_label(secret, "iv", 0, 0, iv, 12U) != 0 ? -1 : 0;
}

static int tls13_decrypt_handshake_record(Tls13Server *server, const unsigned char key[16], const unsigned char iv[12], unsigned long long seq, const unsigned char header[5], const unsigned char *payload, size_t payload_length, unsigned char *plaintext, size_t *plaintext_length_out) {
    unsigned char record[5U + 65536U];
    unsigned char inner_type = 0U;

    if (5U + payload_length > sizeof(record)) return tls13_server_fail(server, "tls13 server encrypted record too large");
    memcpy(record, header, 5U);
    memcpy(record + 5U, payload, payload_length);
    if (tls13_record_decrypt(key, iv, seq, record, 5U + payload_length, &inner_type, plaintext, plaintext_length_out) != 0) return tls13_server_fail(server, "tls13 server handshake record decrypt failed");
    if (inner_type != TLS_CONTENT_HANDSHAKE) return tls13_server_fail(server, "tls13 server expected encrypted handshake");
    return 0;
}

static int tls13_read_client_finished(Tls13Server *server, const unsigned char c_hs[32], const unsigned char c_key[16], const unsigned char c_iv[12], unsigned long long *c_hs_seq_io, struct Tls13Transcript *transcript) {
    unsigned char header[5];
    unsigned char payload[65536];
    unsigned char plaintext[65536];
    size_t payload_length = 0U;
    size_t plaintext_length = 0U;
    unsigned char expected[32];
    unsigned char finished_key[32];
    unsigned char transcript_hash[32];

    if (tls13_read_record(server, header, payload, sizeof(payload), &payload_length) != 0) return -1;
    if (header[0] == TLS_CONTENT_CHANGE_CIPHER_SPEC) {
        if (tls13_read_record(server, header, payload, sizeof(payload), &payload_length) != 0) return -1;
    }
    if (header[0] != TLS_CONTENT_APPLICATION_DATA) return tls13_server_fail(server, "tls13 server expected client finished record");
    if (tls13_decrypt_handshake_record(server, c_key, c_iv, *c_hs_seq_io, header, payload, payload_length, plaintext, &plaintext_length) != 0) return -1;
    *c_hs_seq_io += 1ULL;
    if (plaintext_length != 36U || plaintext[0] != TLS13_HS_FINISHED || tls13_load_u24(plaintext + 1U) != 32U) return tls13_server_fail(server, "tls13 server client finished malformed");
    tls13_transcript_final(transcript, transcript_hash);
    if (tls13_finished_key(c_hs, finished_key) != 0) return tls13_server_fail(server, "tls13 server client finished key failed");
    tls13_finished_verify_data(finished_key, transcript_hash, expected);
    crypto_secure_bzero(finished_key, sizeof(finished_key));
    if (!crypto_constant_time_equal(expected, plaintext + 4U, sizeof(expected))) return tls13_server_fail(server, "tls13 server client finished verification failed");
    tls13_transcript_update(transcript, plaintext, plaintext_length);
    return 0;
}

static long tls13_server_deliver_app(Tls13Server *server, unsigned char *buffer, size_t capacity, const unsigned char *plaintext, size_t plaintext_length) {
    size_t deliver = plaintext_length;

    if (deliver > capacity) deliver = capacity;
    if (deliver != 0U) memcpy(buffer, plaintext, deliver);
    if (deliver < plaintext_length) {
        size_t remainder = plaintext_length - deliver;
        if (remainder > sizeof(server->pending_app)) {
            (void)tls13_server_fail(server, "tls13 server application plaintext too large");
            return -1;
        }
        memcpy(server->pending_app, plaintext + deliver, remainder);
        server->pending_app_len = remainder;
        server->pending_app_offset = 0U;
    }
    return (long)deliver;
}

void tls13_server_init(Tls13Server *server, int fd, const Tls13ServerCredentials *credentials, unsigned int timeout_ms) {
    if (server == 0) return;
    rt_memset(server, 0, sizeof(*server));
    server->fd = fd;
    server->timeout_ms = timeout_ms;
    if (credentials != 0) server->credentials = *credentials;
    server->last_error = "none";
}

const char *tls13_server_last_error(const Tls13Server *server) {
    return server != 0 && server->last_error != 0 ? server->last_error : "unknown tls13 server error";
}

int tls13_server_handshake(Tls13Server *server) {
    unsigned char client_hello[4096];
    size_t client_hello_length = 0U;
    Tls13ClientHelloInfo client;
    unsigned char server_random[32];
    unsigned char server_private_key[32];
    unsigned char server_public_key[32];
    unsigned char shared_secret[32];
    unsigned char server_hello[512];
    size_t server_hello_length = 0U;
    struct Tls13Transcript transcript;
    unsigned char transcript_hash[32];
    unsigned char zeros[32];
    unsigned char early_secret[32];
    unsigned char derived_secret[32];
    unsigned char handshake_secret[32];
    unsigned char c_hs[32];
    unsigned char s_hs[32];
    unsigned char c_hs_key[16];
    unsigned char c_hs_iv[12];
    unsigned char s_hs_key[16];
    unsigned char s_hs_iv[12];
    unsigned long long c_hs_seq = 0ULL;
    unsigned long long s_hs_seq = 0ULL;
    unsigned char handshake_plaintext[12000];
    Tls13Buffer handshake_buffer;
    unsigned char encrypted_record[5U + 12000U + 64U];
    size_t encrypted_record_length = 0U;
    unsigned char derived_master[32];
    unsigned char master_secret[32];
    unsigned char th_post_server_finished[32];
    unsigned char c_ap[32];
    unsigned char s_ap[32];
    int result = -1;

    if (server == 0) return -1;
    server->last_error = "none";
    if (server->fd < 0) return tls13_server_fail(server, "tls13 server invalid socket");
    if (server->credentials.cert_der == 0 || server->credentials.cert_der_len == 0U || server->credentials.rsa_key == 0) return tls13_server_fail(server, "tls13 server credentials missing");
    if (tls13_read_client_hello(server, client_hello, sizeof(client_hello), &client_hello_length) != 0) goto done;
    if (tls13_parse_client_hello(client_hello, client_hello_length, &client) != 0) {
        (void)tls13_write_plain_record(server, TLS_CONTENT_ALERT, (const unsigned char *)"\x02\x28", 2U);
        (void)tls13_server_fail(server, "tls13 server unsupported client hello");
        goto done;
    }
    if (crypto_random_bytes(server_random, sizeof(server_random)) != 0 || crypto_random_bytes(server_private_key, sizeof(server_private_key)) != 0 || crypto_x25519_scalarmult_base(server_public_key, server_private_key) != 0 || crypto_x25519_scalarmult(shared_secret, server_private_key, client.key_share) != 0) {
        (void)tls13_server_fail(server, "tls13 server key share failed");
        goto done;
    }
    if (tls13_build_server_hello(&client, server_random, server_public_key, server_hello, sizeof(server_hello), &server_hello_length) != 0) {
        (void)tls13_server_fail(server, "tls13 server hello build failed");
        goto done;
    }
    tls13_transcript_init(&transcript);
    tls13_transcript_update(&transcript, client_hello, client_hello_length);
    tls13_transcript_update(&transcript, server_hello, server_hello_length);
    if (tls13_write_plain_record(server, TLS_CONTENT_HANDSHAKE, server_hello, server_hello_length) != 0) goto done;

    rt_memset(zeros, 0, sizeof(zeros));
    crypto_hkdf_sha256_extract(early_secret, zeros, sizeof(zeros), zeros, sizeof(zeros));
    if (tls13_derive_secret(early_secret, "derived", tls13_server_empty_hash(), derived_secret) != 0) {
        (void)tls13_server_fail(server, "tls13 server early derived secret failed");
        goto done;
    }
    crypto_hkdf_sha256_extract(handshake_secret, derived_secret, sizeof(derived_secret), shared_secret, sizeof(shared_secret));
    tls13_transcript_final(&transcript, transcript_hash);
    if (tls13_derive_secret(handshake_secret, "c hs traffic", transcript_hash, c_hs) != 0 || tls13_derive_secret(handshake_secret, "s hs traffic", transcript_hash, s_hs) != 0 || tls13_derive_key_iv(c_hs, c_hs_key, c_hs_iv) != 0 || tls13_derive_key_iv(s_hs, s_hs_key, s_hs_iv) != 0) {
        (void)tls13_server_fail(server, "tls13 server handshake key derivation failed");
        goto done;
    }

    handshake_buffer.data = handshake_plaintext;
    handshake_buffer.capacity = sizeof(handshake_plaintext);
    handshake_buffer.size = 0U;
    {
        unsigned char message[9000];
        size_t message_length = 0U;
        if (tls13_build_encrypted_extensions(message, sizeof(message), &message_length) != 0 || tls13_buffer_append(&handshake_buffer, message, message_length) != 0) {
            (void)tls13_server_fail(server, "tls13 server encrypted extensions failed");
            goto done;
        }
        tls13_transcript_update(&transcript, message, message_length);
        if (tls13_build_certificate(server, message, sizeof(message), &message_length) != 0 || tls13_buffer_append(&handshake_buffer, message, message_length) != 0) {
            (void)tls13_server_fail(server, "tls13 server certificate message failed");
            goto done;
        }
        tls13_transcript_update(&transcript, message, message_length);
        tls13_transcript_final(&transcript, transcript_hash);
        if (tls13_build_certificate_verify(server, transcript_hash, message, sizeof(message), &message_length) != 0 || tls13_buffer_append(&handshake_buffer, message, message_length) != 0) {
            (void)tls13_server_fail(server, "tls13 server certificate verify failed");
            goto done;
        }
        tls13_transcript_update(&transcript, message, message_length);
        tls13_transcript_final(&transcript, transcript_hash);
        if (tls13_build_finished(s_hs, transcript_hash, message, sizeof(message), &message_length) != 0 || tls13_buffer_append(&handshake_buffer, message, message_length) != 0) {
            (void)tls13_server_fail(server, "tls13 server finished failed");
            goto done;
        }
        tls13_transcript_update(&transcript, message, message_length);
        tls13_transcript_final(&transcript, th_post_server_finished);
    }
    if (tls13_record_encrypt(s_hs_key, s_hs_iv, s_hs_seq, TLS_CONTENT_HANDSHAKE, handshake_buffer.data, handshake_buffer.size, encrypted_record, sizeof(encrypted_record), &encrypted_record_length) != 0) {
        (void)tls13_server_fail(server, "tls13 server encrypted handshake record failed");
        goto done;
    }
    s_hs_seq += 1ULL;
    if (!tls13_write_all_timeout(server->fd, encrypted_record, encrypted_record_length, server->timeout_ms)) {
        (void)tls13_server_fail(server, "tls13 server encrypted handshake write failed");
        goto done;
    }
    if (tls13_read_client_finished(server, c_hs, c_hs_key, c_hs_iv, &c_hs_seq, &transcript) != 0) goto done;

    if (tls13_derive_secret(handshake_secret, "derived", tls13_server_empty_hash(), derived_master) != 0) {
        (void)tls13_server_fail(server, "tls13 server master derived secret failed");
        goto done;
    }
    crypto_hkdf_sha256_extract(master_secret, derived_master, sizeof(derived_master), zeros, sizeof(zeros));
    if (tls13_derive_secret(master_secret, "c ap traffic", th_post_server_finished, c_ap) != 0 || tls13_derive_secret(master_secret, "s ap traffic", th_post_server_finished, s_ap) != 0 || tls13_derive_key_iv(c_ap, server->c_ap_key, server->c_ap_iv) != 0 || tls13_derive_key_iv(s_ap, server->s_ap_key, server->s_ap_iv) != 0) {
        (void)tls13_server_fail(server, "tls13 server application key derivation failed");
        goto done;
    }
    server->c_ap_seq = 0ULL;
    server->s_ap_seq = 0ULL;
    server->pending_app_len = 0U;
    server->pending_app_offset = 0U;
    server->handshake_done = 1;
    server->last_error = "none";
    result = 0;
done:
    crypto_secure_bzero(server_private_key, sizeof(server_private_key));
    crypto_secure_bzero(shared_secret, sizeof(shared_secret));
    crypto_secure_bzero(early_secret, sizeof(early_secret));
    crypto_secure_bzero(derived_secret, sizeof(derived_secret));
    crypto_secure_bzero(handshake_secret, sizeof(handshake_secret));
    crypto_secure_bzero(c_hs, sizeof(c_hs));
    crypto_secure_bzero(s_hs, sizeof(s_hs));
    crypto_secure_bzero(c_hs_key, sizeof(c_hs_key));
    crypto_secure_bzero(c_hs_iv, sizeof(c_hs_iv));
    crypto_secure_bzero(s_hs_key, sizeof(s_hs_key));
    crypto_secure_bzero(s_hs_iv, sizeof(s_hs_iv));
    crypto_secure_bzero(derived_master, sizeof(derived_master));
    crypto_secure_bzero(master_secret, sizeof(master_secret));
    crypto_secure_bzero(th_post_server_finished, sizeof(th_post_server_finished));
    crypto_secure_bzero(c_ap, sizeof(c_ap));
    crypto_secure_bzero(s_ap, sizeof(s_ap));
    return result;
}

long tls13_server_write_app(Tls13Server *server, const unsigned char *buffer, size_t length) {
    size_t offset = 0U;

    if (server == 0 || !server->handshake_done || (buffer == 0 && length != 0U)) return -1;
    while (offset < length) {
        size_t chunk = length - offset;
        unsigned char record[5U + 16384U + 64U];
        size_t record_length = 0U;

        if (chunk > 16384U) chunk = 16384U;
        if (tls13_record_encrypt(server->s_ap_key, server->s_ap_iv, server->s_ap_seq, TLS_CONTENT_APPLICATION_DATA, buffer + offset, chunk, record, sizeof(record), &record_length) != 0) return -1;
        server->s_ap_seq += 1ULL;
        if (!tls13_write_all_timeout(server->fd, record, record_length, server->timeout_ms)) return -1;
        offset += chunk;
    }
    return (long)length;
}

long tls13_server_read_app(Tls13Server *server, unsigned char *buffer, size_t capacity) {
    if (server == 0 || !server->handshake_done || buffer == 0 || capacity == 0U) return -1;
    if (server->pending_app_offset < server->pending_app_len) {
        size_t available = server->pending_app_len - server->pending_app_offset;
        if (available > capacity) available = capacity;
        memcpy(buffer, server->pending_app + server->pending_app_offset, available);
        server->pending_app_offset += available;
        if (server->pending_app_offset >= server->pending_app_len) {
            server->pending_app_len = 0U;
            server->pending_app_offset = 0U;
        }
        return (long)available;
    }
    for (;;) {
        unsigned char header[5];
        unsigned char payload[65536];
        unsigned char record[5U + 65536U];
        unsigned char plaintext[65536];
        size_t payload_length = 0U;
        size_t plaintext_length = 0U;
        unsigned char inner_type = 0U;

        if (tls13_read_record(server, header, payload, sizeof(payload), &payload_length) != 0) return -1;
        if (header[0] == TLS_CONTENT_CHANGE_CIPHER_SPEC) continue;
        if (header[0] == TLS_CONTENT_ALERT) return 0;
        if (header[0] != TLS_CONTENT_APPLICATION_DATA || 5U + payload_length > sizeof(record)) return tls13_server_fail(server, "tls13 server application record rejected");
        memcpy(record, header, 5U);
        memcpy(record + 5U, payload, payload_length);
        if (tls13_record_decrypt(server->c_ap_key, server->c_ap_iv, server->c_ap_seq, record, 5U + payload_length, &inner_type, plaintext, &plaintext_length) != 0) return tls13_server_fail(server, "tls13 server application record decrypt failed");
        server->c_ap_seq += 1ULL;
        if (inner_type == TLS_CONTENT_APPLICATION_DATA) return tls13_server_deliver_app(server, buffer, capacity, plaintext, plaintext_length);
        if (inner_type == TLS_CONTENT_ALERT) {
            if (plaintext_length >= 2U && plaintext[1] == 0U) return 0;
            return tls13_server_fail(server, "tls13 server decrypted alert");
        }
    }
}

int tls13_server_close_notify(Tls13Server *server) {
    unsigned char alert[2];
    unsigned char record[5U + 64U];
    size_t record_length = 0U;

    if (server == 0 || !server->handshake_done) return -1;
    alert[0] = 1U;
    alert[1] = 0U;
    if (tls13_record_encrypt(server->s_ap_key, server->s_ap_iv, server->s_ap_seq, TLS_CONTENT_ALERT, alert, sizeof(alert), record, sizeof(record), &record_length) != 0) return -1;
    server->s_ap_seq += 1ULL;
    if (!tls13_write_all_timeout(server->fd, record, record_length, server->timeout_ms)) return -1;
    return 0;
}
