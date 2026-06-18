#include "crypto/aes128.h"
#include "crypto/aes128_gcm.h"
#include "crypto/crypto_util.h"
#include "crypto/curve25519.h"
#include "crypto/ed25519.h"
#include "crypto/hkdf_sha256.h"
#include "crypto/rsa.h"
#include "crypto/sha1.h"
#include "crypto/sha256.h"
#include "crypto/sha512.h"
#include "compression/zlib.h"
#include "pgp.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PGPMSG_USAGE "[--json] COMMAND [ARGS...]\n       pgpmsg inspect [-v] [FILE]\n       pgpmsg verify [-k PUBRING] SIGNATURE [FILE]\n       pgpmsg encrypt -r RECIPIENT [-r RECIPIENT ...] [-k PUBRING] [-o OUT] [--armor] [--compress=ALG] [--sign -s SECRING [-u SIGNER]] [--stream] [--DANGER-anyway] [FILE]\n       pgpmsg decrypt [-s SECRING] [-o OUT] [FILE]\n       pgpmsg sign -u SIGNER [-s SECRING] [-o OUT] [--armor] [--detach|--cleartext] [--DANGER-anyway] [FILE]"
#define PGPMSG_ERROR_CAPACITY 160U
#define PGPMSG_KEY_ID_SIZE 8U
#define PGPMSG_ED25519_KEY_SIZE 32U
#define PGPMSG_ED25519_SIGNATURE_SIZE 64U
#define PGPMSG_X25519_KEY_SIZE 32U
#define PGPMSG_AES256_KEY_SIZE 32U
#define PGPMSG_AES_BLOCK_SIZE 16U
#define PGPMSG_AES_GCM_TAG_SIZE 16U
#define PGPMSG_V2_SEIPD_SALT_SIZE 32U
#define PGPMSG_V2_SEIPD_CHUNK_OCTET 16U
#define PGPMSG_V2_SEIPD_CHUNK_SIZE (1ULL << (PGPMSG_V2_SEIPD_CHUNK_OCTET + 6U))
#define PGPMSG_MDC_SIZE 20U
#define PGPMSG_MAX_RECIPIENTS 16U
#define PGPMSG_PGP_LITERAL_BINARY 11U
#define PGPMSG_PGP_COMPRESSED 8U
#define PGPMSG_COMPRESSION_NONE 0U
#define PGPMSG_COMPRESSION_ZIP 1U
#define PGPMSG_COMPRESSION_ZLIB 2U
#define PGPMSG_COMPRESSION_BZIP2 3U
#define PGPMSG_MAX_INFLATED_SIZE (64U * 1024U * 1024U)
#define PGPMSG_STREAM_CHUNK_SIZE 65536U

typedef struct {
    int json;
    const char *pubring;
    const char *secring;
    const char *output_path;
    const char *recipients[PGPMSG_MAX_RECIPIENTS];
    size_t recipient_count;
    const char *signer;
    int armor;
    int detach;
    int cleartext;
    int danger_anyway;
    int stream;
    int sign;
    int verbose;
    unsigned int compression;
} PgpMsgOptions;

typedef struct {
    size_t raw_size;
    size_t decoded_size;
    int armored;
    char armor_kind[40];
    unsigned long long armor_body_lines;
    unsigned long long armor_base64_chars;
    unsigned char armor_crc24[3];
    int has_armor_crc24;
} PgpMsgInspectInputInfo;

typedef struct {
    CryptoAes256Context aes;
    unsigned char feedback[PGPMSG_AES_BLOCK_SIZE];
    unsigned char pending[PGPMSG_AES_BLOCK_SIZE];
    size_t pending_size;
} PgpMsgCfbStream;

typedef struct {
    int fd;
    unsigned char buffer[PGPMSG_STREAM_CHUNK_SIZE];
    size_t used;
} PgpMsgPartialPacketWriter;

typedef struct {
    PgpMsgCfbStream cfb;
    PgpMsgPartialPacketWriter writer;
    CryptoSha1Context sha1;
} PgpMsgSeipdStream;

typedef struct {
    unsigned int version;
    unsigned int signature_type;
    unsigned int public_key_algorithm;
    unsigned int hash_algorithm;
    unsigned long long created;
    unsigned char issuer_key_id[PGPMSG_KEY_ID_SIZE];
    int has_issuer_key_id;
    unsigned char issuer_fingerprint[PGP_FINGERPRINT_MAX_SIZE];
    size_t issuer_fingerprint_size;
    int present;
} PgpMsgSignatureSummary;

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} PgpMsgBuffer;

typedef struct {
    PgpPublicKeyInfo info;
    unsigned char seed[PGPMSG_ED25519_KEY_SIZE];
    int found;
} PgpMsgSecretKey;

typedef struct {
    PgpPublicKeyInfo info;
    unsigned char seed[PGPMSG_X25519_KEY_SIZE];
    int found;
} PgpMsgX25519SecretKey;

typedef struct {
    PgpMsgSignatureSummary summary;
    size_t hash_part_size;
    size_t salt_offset;
    unsigned char salt[32];
    size_t salt_size;
    unsigned char digest_prefix[2];
    unsigned char signature[PGPMSG_ED25519_SIGNATURE_SIZE];
    int has_signature;
} PgpMsgSignaturePacket;

typedef struct {
    const PgpMsgSignatureSummary *signature;
    PgpPublicKeyInfo key;
    int found;
} PgpMsgPublicKeyFindContext;

typedef struct {
    const char *selector;
    PgpPublicKeyInfo key;
    int danger_anyway;
    int found;
    int allowed;
} PgpMsgSignerUsageContext;

typedef struct {
    const char *selector;
    PgpPublicKeyInfo key;
    int danger_anyway;
    int found;
} PgpMsgRecipientFindContext;

typedef struct {
    unsigned int version;
    unsigned int key_version;
    unsigned char key_id[PGPMSG_KEY_ID_SIZE];
    unsigned char fingerprint[PGP_FINGERPRINT_MAX_SIZE];
    size_t fingerprint_size;
    unsigned int public_key_algorithm;
    unsigned char ephemeral_public[PGPMSG_X25519_KEY_SIZE];
    unsigned char wrapped_session_key[64];
    size_t wrapped_session_key_size;
    int found;
} PgpMsgPkesk;

static void print_usage(void) {
    tool_write_usage("pgpmsg", PGPMSG_USAGE);
}

static int add_recipient_option(PgpMsgOptions *options, const char *recipient) {
    if (recipient == 0 || recipient[0] == '\0') {
        tool_write_error("pgpmsg", "empty recipient selector", 0);
        return -1;
    }
    if (options->recipient_count >= PGPMSG_MAX_RECIPIENTS) {
        tool_write_error("pgpmsg", "too many recipients", 0);
        return -1;
    }
    options->recipients[options->recipient_count++] = recipient;
    return 0;
}

static unsigned int read_u16_be(const unsigned char *data) {
    return ((unsigned int)data[0] << 8U) | (unsigned int)data[1];
}

static void msg_buffer_free(PgpMsgBuffer *buffer) {
    if (buffer->data != 0) rt_free(buffer->data);
    buffer->data = 0;
    buffer->size = 0U;
    buffer->capacity = 0U;
}

static int msg_buffer_reserve(PgpMsgBuffer *buffer, size_t extra) {
    size_t needed;
    size_t capacity;
    unsigned char *grown;

    if (extra > ((size_t)-1) - buffer->size) return -1;
    needed = buffer->size + extra;
    if (needed <= buffer->capacity) return 0;
    capacity = buffer->capacity != 0U ? buffer->capacity : 128U;
    while (capacity < needed) {
        if (capacity > ((size_t)-1) / 2U) {
            capacity = needed;
            break;
        }
        capacity *= 2U;
    }
    grown = (unsigned char *)rt_realloc(buffer->data, capacity);
    if (grown == 0) return -1;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 0;
}

static int msg_buffer_append_byte(PgpMsgBuffer *buffer, unsigned int value) {
    if (msg_buffer_reserve(buffer, 1U) != 0) return -1;
    buffer->data[buffer->size++] = (unsigned char)(value & 0xffU);
    return 0;
}

static int msg_buffer_append_data(PgpMsgBuffer *buffer, const unsigned char *data, size_t size) {
    if (size == 0U) return 0;
    if (msg_buffer_reserve(buffer, size) != 0) return -1;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 0;
}

static int msg_buffer_append_u16_be(PgpMsgBuffer *buffer, unsigned int value) {
    return msg_buffer_append_byte(buffer, (value >> 8U) & 0xffU) != 0 || msg_buffer_append_byte(buffer, value & 0xffU) != 0 ? -1 : 0;
}

static int msg_buffer_append_u32_be(PgpMsgBuffer *buffer, unsigned long long value) {
    return msg_buffer_append_byte(buffer, (unsigned int)((value >> 24U) & 0xffU)) != 0 ||
           msg_buffer_append_byte(buffer, (unsigned int)((value >> 16U) & 0xffU)) != 0 ||
           msg_buffer_append_byte(buffer, (unsigned int)((value >> 8U) & 0xffU)) != 0 ||
           msg_buffer_append_byte(buffer, (unsigned int)(value & 0xffU)) != 0 ? -1 : 0;
}

static int msg_buffer_append_packet_length(PgpMsgBuffer *buffer, size_t length) {
    if (length < 192U) return msg_buffer_append_byte(buffer, (unsigned int)length);
    if (length <= 8383U) {
        size_t encoded = length - 192U;
        return msg_buffer_append_byte(buffer, (unsigned int)((encoded >> 8U) + 192U)) != 0 || msg_buffer_append_byte(buffer, (unsigned int)(encoded & 0xffU)) != 0 ? -1 : 0;
    }
    if (length > 0xffffffffULL) return -1;
    return msg_buffer_append_byte(buffer, 255U) != 0 || msg_buffer_append_u32_be(buffer, (unsigned long long)length) != 0 ? -1 : 0;
}

static int msg_buffer_append_packet(PgpMsgBuffer *buffer, unsigned int tag, const PgpMsgBuffer *body) {
    return msg_buffer_append_byte(buffer, 0xc0U | (tag & 0x3fU)) != 0 ||
           msg_buffer_append_packet_length(buffer, body->size) != 0 ||
           msg_buffer_append_data(buffer, body->data, body->size) != 0 ? -1 : 0;
}

static int msg_buffer_append_signature_subpacket(PgpMsgBuffer *buffer, unsigned int type, const unsigned char *body, size_t body_size) {
    if (msg_buffer_append_packet_length(buffer, body_size + 1U) != 0) return -1;
    if (msg_buffer_append_byte(buffer, type) != 0) return -1;
    return msg_buffer_append_data(buffer, body, body_size);
}

static int msg_buffer_append_signature_subpacket_u32(PgpMsgBuffer *buffer, unsigned int type, unsigned long long value) {
    unsigned char body[4];

    body[0] = (unsigned char)((value >> 24U) & 0xffU);
    body[1] = (unsigned char)((value >> 16U) & 0xffU);
    body[2] = (unsigned char)((value >> 8U) & 0xffU);
    body[3] = (unsigned char)(value & 0xffU);
    return msg_buffer_append_signature_subpacket(buffer, type, body, sizeof(body));
}

static int msg_buffer_append_opaque_mpi(PgpMsgBuffer *buffer, const unsigned char *data, size_t size, unsigned int bits) {
    return msg_buffer_append_u16_be(buffer, bits) != 0 || msg_buffer_append_data(buffer, data, size) != 0 ? -1 : 0;
}

static int write_output_data(const char *path, const unsigned char *data, size_t size, int armor_signature) {
    int fd = path != 0 ? platform_open_write(path, 0644U) : 1;
    int result;

    if (fd < 0) return -1;
    result = armor_signature ? pgp_write_signature_armor(fd, data, size) : (rt_write_all(fd, data, size) == 0 ? 0 : -1);
    if (fd > 2 && platform_close(fd) != 0) result = -1;
    return result;
}

static int write_message_output_data(const char *path, const unsigned char *data, size_t size, int armor_message) {
    int fd = path != 0 ? platform_open_write(path, 0644U) : 1;
    int result;

    if (fd < 0) return -1;
    result = armor_message ? pgp_write_message_armor(fd, data, size) : (rt_write_all(fd, data, size) == 0 ? 0 : -1);
    if (fd > 2 && platform_close(fd) != 0) result = -1;
    return result;
}

static int write_cleartext_body_line(int fd, const unsigned char *line, size_t size) {
    if (size != 0U && line[0] == '-') {
        if (rt_write_cstr(fd, "- ") != 0) return -1;
    }
    return rt_write_all(fd, line, size) == 0 && rt_write_cstr(fd, "\r\n") == 0 ? 0 : -1;
}

static int write_cleartext_signed_output(const char *path, const unsigned char *input, size_t input_size, const unsigned char *signature, size_t signature_size) {
    int fd = path != 0 ? platform_open_write(path, 0644U) : 1;
    size_t offset = 0U;
    int result = -1;

    if (fd < 0) return -1;
    if (rt_write_cstr(fd, "-----BEGIN PGP SIGNED MESSAGE-----\r\nHash: SHA512\r\n\r\n") != 0) goto cleanup;
    while (offset < input_size) {
        size_t line_start = offset;
        size_t line_end;

        while (offset < input_size && input[offset] != '\r' && input[offset] != '\n') offset += 1U;
        line_end = offset;
        if (offset < input_size && input[offset] == '\r') offset += 1U;
        if (offset < input_size && input[offset] == '\n') offset += 1U;
        if (write_cleartext_body_line(fd, input + line_start, line_end - line_start) != 0) goto cleanup;
    }
    if (input_size == 0U && write_cleartext_body_line(fd, (const unsigned char *)"", 0U) != 0) goto cleanup;
    if (pgp_write_signature_armor(fd, signature, signature_size) != 0) goto cleanup;
    result = 0;

cleanup:
    if (fd > 2 && platform_close(fd) != 0) result = -1;
    return result;
}

static int selector_matches_user_id(const PgpCertificateInfo *certificate, const char *selector) {
    size_t index;

    if (selector == 0 || selector[0] == '\0') return 1;
    if (pgp_fingerprint_matches_text(&certificate->primary, selector)) return 1;
    for (index = 0U; index < certificate->user_id_count; ++index) {
        if (tool_contains_case_insensitive(certificate->user_ids[index], selector)) return 1;
    }
    return 0;
}

static int key_id_matches_bytes(const PgpPublicKeyInfo *key, const unsigned char key_id[PGPMSG_KEY_ID_SIZE]) {
    return memcmp(key->key_id, key_id, PGPMSG_KEY_ID_SIZE) == 0;
}

static int pgpmsg_key_id_matches(const unsigned char left[PGPMSG_KEY_ID_SIZE], const unsigned char right[PGPMSG_KEY_ID_SIZE]) {
    return memcmp(left, right, PGPMSG_KEY_ID_SIZE) == 0;
}

static int pgpmsg_signature_issuer_matches_key(const PgpSignatureInfo *signature, const PgpPublicKeyInfo *key) {
    if (signature->issuer_fingerprint_size != 0U && signature->issuer_fingerprint_size == key->fingerprint_size && memcmp(signature->issuer_fingerprint, key->fingerprint, key->fingerprint_size) == 0) return 1;
    if (signature->has_issuer_key_id && pgpmsg_key_id_matches(signature->issuer_key_id, key->key_id)) return 1;
    return 0;
}

static int pgpmsg_signature_has_any_flag(const PgpSignatureInfo *signature, unsigned int flags) {
    size_t index;

    for (index = 0U; index < signature->key_flags_size; ++index) {
        if (((unsigned int)signature->key_flags[index] & flags) != 0U) return 1;
    }
    return 0;
}

static int pgpmsg_certificate_primary_allows_sign(const PgpCertificateInfo *certificate) {
    size_t index;

    for (index = 0U; index < certificate->signature_info_count; ++index) {
        const PgpSignatureInfo *signature = &certificate->signatures[index];

        if (signature->key_flags_size == 0U || !pgpmsg_signature_issuer_matches_key(signature, &certificate->primary)) continue;
        if ((signature->target_tag == PGP_SIGNATURE_TARGET_PRIMARY || signature->target_tag == PGP_SIGNATURE_TARGET_USER_ID) && pgpmsg_signature_has_any_flag(signature, 0x02U)) return 1;
    }
    return 0;
}

static int pgpmsg_certificate_subkey_allows_encrypt(const PgpCertificateInfo *certificate, size_t subkey_index) {
    size_t index;
    unsigned long long binding_created = 0ULL;
    unsigned long long revocation_created = 0ULL;
    int allowed = 0;

    for (index = 0U; index < certificate->signature_info_count; ++index) {
        const PgpSignatureInfo *signature = &certificate->signatures[index];

        if (signature->target_tag != PGP_SIGNATURE_TARGET_SUBKEY || signature->target_index != subkey_index) continue;
        if (!pgpmsg_signature_issuer_matches_key(signature, &certificate->primary)) continue;
        if (signature->signature_type == 0x28U && signature->created >= revocation_created) {
            revocation_created = signature->created;
        } else if (signature->signature_type == 0x18U && signature->created >= binding_created) {
            binding_created = signature->created;
            allowed = signature->key_flags_size != 0U ? pgpmsg_signature_has_any_flag(signature, 0x0cU) : certificate->subkeys[subkey_index].algorithm == 16U;
        }
    }
    if (!allowed) return 0;
    return revocation_created == 0ULL || binding_created > revocation_created;
}

static int parse_compression_option(const char *value, unsigned int *compression_out) {
    if (value == 0) return -1;
    if (rt_strcmp(value, "none") == 0 || rt_strcmp(value, "uncompressed") == 0) {
        *compression_out = PGPMSG_COMPRESSION_NONE;
        return 0;
    }
    if (rt_strcmp(value, "zip") == 0 || rt_strcmp(value, "ZIP") == 0) {
        *compression_out = PGPMSG_COMPRESSION_ZIP;
        return 0;
    }
    if (rt_strcmp(value, "zlib") == 0 || rt_strcmp(value, "ZLIB") == 0) {
        *compression_out = PGPMSG_COMPRESSION_ZLIB;
        return 0;
    }
    if (rt_strcmp(value, "bzip2") == 0 || rt_strcmp(value, "BZip2") == 0 || rt_strcmp(value, "bzip") == 0) {
        *compression_out = PGPMSG_COMPRESSION_BZIP2;
        return 0;
    }
    return -1;
}

static void pgpmsg_aes256_cfb_xcrypt(const unsigned char key[PGPMSG_AES256_KEY_SIZE], const unsigned char *input, unsigned char *output, size_t size, int decrypt) {
    CryptoAes256Context aes;
    unsigned char feedback[PGPMSG_AES_BLOCK_SIZE];
    unsigned char stream[PGPMSG_AES_BLOCK_SIZE];
    size_t offset = 0U;

    crypto_aes256_init(&aes, key);
    rt_memset(feedback, 0, sizeof(feedback));
    while (offset < size) {
        size_t chunk = size - offset;
        size_t index;
        unsigned char cipher_block[PGPMSG_AES_BLOCK_SIZE];

        if (chunk > PGPMSG_AES_BLOCK_SIZE) chunk = PGPMSG_AES_BLOCK_SIZE;
        crypto_aes256_encrypt_block(&aes, feedback, stream);
        if (decrypt) memcpy(cipher_block, input + offset, chunk);
        for (index = 0U; index < chunk; ++index) output[offset + index] = input[offset + index] ^ stream[index];
        if (chunk == PGPMSG_AES_BLOCK_SIZE) memcpy(feedback, decrypt ? cipher_block : output + offset, PGPMSG_AES_BLOCK_SIZE);
        offset += chunk;
    }
    crypto_secure_bzero(&aes, sizeof(aes));
    crypto_secure_bzero(feedback, sizeof(feedback));
    crypto_secure_bzero(stream, sizeof(stream));
}

static void pgpmsg_cfb_stream_init(PgpMsgCfbStream *stream, const unsigned char key[PGPMSG_AES256_KEY_SIZE]) {
    crypto_aes256_init(&stream->aes, key);
    rt_memset(stream->feedback, 0, sizeof(stream->feedback));
    stream->pending_size = 0U;
}

static int pgpmsg_partial_writer_write(PgpMsgPartialPacketWriter *writer, const unsigned char *data, size_t size) {
    size_t offset = 0U;

    while (offset < size) {
        size_t chunk = size - offset;

        if (chunk > sizeof(writer->buffer) - writer->used) chunk = sizeof(writer->buffer) - writer->used;
        memcpy(writer->buffer + writer->used, data + offset, chunk);
        writer->used += chunk;
        offset += chunk;
        if (writer->used == sizeof(writer->buffer)) {
            if (pgp_write_partial_body_length(writer->fd, sizeof(writer->buffer)) != 0 || rt_write_all(writer->fd, writer->buffer, sizeof(writer->buffer)) != 0) return -1;
            writer->used = 0U;
        }
    }
    return 0;
}

static int pgpmsg_partial_writer_finish(PgpMsgPartialPacketWriter *writer) {
    if (pgp_write_packet_length(writer->fd, writer->used) != 0) return -1;
    if (writer->used != 0U && rt_write_all(writer->fd, writer->buffer, writer->used) != 0) return -1;
    writer->used = 0U;
    return 0;
}

static int pgpmsg_cfb_stream_emit_block(PgpMsgCfbStream *stream, PgpMsgPartialPacketWriter *writer, const unsigned char block[PGPMSG_AES_BLOCK_SIZE]) {
    unsigned char key_stream[PGPMSG_AES_BLOCK_SIZE];
    unsigned char cipher[PGPMSG_AES_BLOCK_SIZE];
    size_t index;

    crypto_aes256_encrypt_block(&stream->aes, stream->feedback, key_stream);
    for (index = 0U; index < PGPMSG_AES_BLOCK_SIZE; ++index) cipher[index] = block[index] ^ key_stream[index];
    memcpy(stream->feedback, cipher, PGPMSG_AES_BLOCK_SIZE);
    if (pgpmsg_partial_writer_write(writer, cipher, sizeof(cipher)) != 0) {
        crypto_secure_bzero(key_stream, sizeof(key_stream));
        crypto_secure_bzero(cipher, sizeof(cipher));
        return -1;
    }
    crypto_secure_bzero(key_stream, sizeof(key_stream));
    crypto_secure_bzero(cipher, sizeof(cipher));
    return 0;
}

static int pgpmsg_cfb_stream_update(PgpMsgCfbStream *stream, PgpMsgPartialPacketWriter *writer, const unsigned char *input, size_t size) {
    size_t offset = 0U;

    while (offset < size) {
        size_t chunk;

        if (stream->pending_size == 0U && size - offset >= PGPMSG_AES_BLOCK_SIZE) {
            if (pgpmsg_cfb_stream_emit_block(stream, writer, input + offset) != 0) return -1;
            offset += PGPMSG_AES_BLOCK_SIZE;
            continue;
        }
        chunk = size - offset;
        if (chunk > PGPMSG_AES_BLOCK_SIZE - stream->pending_size) chunk = PGPMSG_AES_BLOCK_SIZE - stream->pending_size;
        memcpy(stream->pending + stream->pending_size, input + offset, chunk);
        stream->pending_size += chunk;
        offset += chunk;
        if (stream->pending_size == PGPMSG_AES_BLOCK_SIZE) {
            if (pgpmsg_cfb_stream_emit_block(stream, writer, stream->pending) != 0) return -1;
            stream->pending_size = 0U;
        }
    }
    return 0;
}

static int pgpmsg_cfb_stream_finish(PgpMsgCfbStream *stream, PgpMsgPartialPacketWriter *writer) {
    unsigned char key_stream[PGPMSG_AES_BLOCK_SIZE];
    unsigned char cipher[PGPMSG_AES_BLOCK_SIZE];
    size_t index;

    if (stream->pending_size == 0U) return 0;
    crypto_aes256_encrypt_block(&stream->aes, stream->feedback, key_stream);
    for (index = 0U; index < stream->pending_size; ++index) cipher[index] = stream->pending[index] ^ key_stream[index];
    if (pgpmsg_partial_writer_write(writer, cipher, stream->pending_size) != 0) {
        crypto_secure_bzero(key_stream, sizeof(key_stream));
        crypto_secure_bzero(cipher, sizeof(cipher));
        return -1;
    }
    stream->pending_size = 0U;
    crypto_secure_bzero(key_stream, sizeof(key_stream));
    crypto_secure_bzero(cipher, sizeof(cipher));
    return 0;
}

static int pgpmsg_seipd_stream_write(PgpMsgSeipdStream *stream, const unsigned char *data, size_t size, int hash) {
    if (hash) crypto_sha1_update(&stream->sha1, data, size);
    return pgpmsg_cfb_stream_update(&stream->cfb, &stream->writer, data, size);
}

static void pgpmsg_derive_ecdh_kek(const unsigned char shared[PGPMSG_X25519_KEY_SIZE], const unsigned char fingerprint[PGP_FINGERPRINT_MAX_SIZE], size_t fingerprint_size, unsigned char kek[PGPMSG_AES256_KEY_SIZE]) {
    static const unsigned char curve25519_oid[] = { 0x2bU, 0x06U, 0x01U, 0x04U, 0x01U, 0x97U, 0x55U, 0x01U, 0x05U, 0x01U };
    static const unsigned char kdf_params[] = { 0x03U, 0x01U, 0x08U, 0x09U };
    static const unsigned char anonymous_sender[] = "Anonymous Sender    ";
    CryptoSha256Context sha256;
    unsigned char counter[] = { 0x00U, 0x00U, 0x00U, 0x01U };
    unsigned char oid_size = (unsigned char)sizeof(curve25519_oid);
    unsigned char algorithm = 18U;

    crypto_sha256_init(&sha256);
    crypto_sha256_update(&sha256, counter, sizeof(counter));
    crypto_sha256_update(&sha256, shared, PGPMSG_X25519_KEY_SIZE);
    crypto_sha256_update(&sha256, &oid_size, 1U);
    crypto_sha256_update(&sha256, curve25519_oid, sizeof(curve25519_oid));
    crypto_sha256_update(&sha256, &algorithm, 1U);
    crypto_sha256_update(&sha256, kdf_params, sizeof(kdf_params));
    crypto_sha256_update(&sha256, anonymous_sender, sizeof(anonymous_sender) - 1U);
    crypto_sha256_update(&sha256, fingerprint, fingerprint_size);
    crypto_sha256_final(&sha256, kek);
    crypto_secure_bzero(&sha256, sizeof(sha256));
}

static int pgpmsg_derive_x25519_v6_kek(const unsigned char ephemeral_public[PGPMSG_X25519_KEY_SIZE], const unsigned char recipient_public[PGPMSG_X25519_KEY_SIZE], const unsigned char shared[PGPMSG_X25519_KEY_SIZE], unsigned char kek[CRYPTO_AES128_KEY_SIZE]) {
    unsigned char ikm[PGPMSG_X25519_KEY_SIZE * 3U];
    unsigned char prk[CRYPTO_SHA256_DIGEST_SIZE];
    static const unsigned char info[] = "OpenPGP X25519";
    int result;

    memcpy(ikm, ephemeral_public, PGPMSG_X25519_KEY_SIZE);
    memcpy(ikm + PGPMSG_X25519_KEY_SIZE, recipient_public, PGPMSG_X25519_KEY_SIZE);
    memcpy(ikm + PGPMSG_X25519_KEY_SIZE * 2U, shared, PGPMSG_X25519_KEY_SIZE);
    result = crypto_hkdf_sha256_extract(prk, 0, 0U, ikm, sizeof(ikm));
    if (result == 0) result = crypto_hkdf_sha256_expand(kek, CRYPTO_AES128_KEY_SIZE, prk, info, sizeof(info) - 1U);
    crypto_secure_bzero(ikm, sizeof(ikm));
    crypto_secure_bzero(prk, sizeof(prk));
    return result;
}

static int pgpmsg_aes128_key_wrap(const unsigned char kek[CRYPTO_AES128_KEY_SIZE], const unsigned char *plain, size_t plain_size, PgpMsgBuffer *wrapped) {
    CryptoAes128Context aes;
    unsigned char a[8] = { 0xa6U, 0xa6U, 0xa6U, 0xa6U, 0xa6U, 0xa6U, 0xa6U, 0xa6U };
    unsigned char r[48];
    unsigned char block[16];
    unsigned int n;
    unsigned int j;
    unsigned int i;
    int result = -1;

    if (plain_size == 0U || (plain_size % 8U) != 0U || plain_size > sizeof(r)) return -1;
    n = (unsigned int)(plain_size / 8U);
    memcpy(r, plain, plain_size);
    crypto_aes128_init(&aes, kek);
    for (j = 0U; j < 6U; ++j) {
        for (i = 0U; i < n; ++i) {
            unsigned int t = j * n + i + 1U;
            memcpy(block, a, 8U);
            memcpy(block + 8U, r + i * 8U, 8U);
            crypto_aes128_encrypt_block(&aes, block, block);
            memcpy(a, block, 8U);
            a[7] ^= (unsigned char)(t & 0xffU);
            a[6] ^= (unsigned char)((t >> 8U) & 0xffU);
            a[5] ^= (unsigned char)((t >> 16U) & 0xffU);
            a[4] ^= (unsigned char)((t >> 24U) & 0xffU);
            memcpy(r + i * 8U, block + 8U, 8U);
        }
    }
    if (msg_buffer_append_data(wrapped, a, 8U) != 0 || msg_buffer_append_data(wrapped, r, plain_size) != 0) goto cleanup;
    result = 0;

cleanup:
    crypto_secure_bzero(&aes, sizeof(aes));
    crypto_secure_bzero(a, sizeof(a));
    crypto_secure_bzero(r, sizeof(r));
    crypto_secure_bzero(block, sizeof(block));
    return result;
}

static int pgpmsg_aes128_key_unwrap(const unsigned char kek[CRYPTO_AES128_KEY_SIZE], const unsigned char *wrapped, size_t wrapped_size, unsigned char *plain, size_t *plain_size_out) {
    CryptoAes128Context aes;
    unsigned char a[8];
    unsigned char r[48];
    unsigned char block[16];
    unsigned int n;
    unsigned int j;
    unsigned int i;
    int result = -1;

    if (wrapped_size < 16U || (wrapped_size % 8U) != 0U || wrapped_size - 8U > sizeof(r)) return -1;
    n = (unsigned int)((wrapped_size / 8U) - 1U);
    memcpy(a, wrapped, 8U);
    memcpy(r, wrapped + 8U, wrapped_size - 8U);
    crypto_aes128_init(&aes, kek);
    for (j = 6U; j > 0U; --j) {
        for (i = n; i > 0U; --i) {
            unsigned int t = (j - 1U) * n + i;
            memcpy(block, a, 8U);
            block[7] ^= (unsigned char)(t & 0xffU);
            block[6] ^= (unsigned char)((t >> 8U) & 0xffU);
            block[5] ^= (unsigned char)((t >> 16U) & 0xffU);
            block[4] ^= (unsigned char)((t >> 24U) & 0xffU);
            memcpy(block + 8U, r + (i - 1U) * 8U, 8U);
            crypto_aes128_decrypt_block(&aes, block, block);
            memcpy(a, block, 8U);
            memcpy(r + (i - 1U) * 8U, block + 8U, 8U);
        }
    }
    if (!crypto_constant_time_equal(a, (const unsigned char *)"\xa6\xa6\xa6\xa6\xa6\xa6\xa6\xa6", 8U)) goto cleanup;
    memcpy(plain, r, wrapped_size - 8U);
    *plain_size_out = wrapped_size - 8U;
    result = 0;

cleanup:
    crypto_secure_bzero(&aes, sizeof(aes));
    crypto_secure_bzero(a, sizeof(a));
    crypto_secure_bzero(r, sizeof(r));
    crypto_secure_bzero(block, sizeof(block));
    return result;
}

static int pgpmsg_aes_key_wrap(const unsigned char kek[PGPMSG_AES256_KEY_SIZE], const unsigned char *plain, size_t plain_size, PgpMsgBuffer *wrapped) {
    CryptoAes256Context aes;
    unsigned char a[8] = { 0xa6U, 0xa6U, 0xa6U, 0xa6U, 0xa6U, 0xa6U, 0xa6U, 0xa6U };
    unsigned char r[48];
    unsigned char block[16];
    unsigned int n;
    unsigned int j;
    unsigned int i;

    if (plain_size == 0U || (plain_size % 8U) != 0U || plain_size > sizeof(r)) return -1;
    n = (unsigned int)(plain_size / 8U);
    memcpy(r, plain, plain_size);
    crypto_aes256_init(&aes, kek);
    for (j = 0U; j < 6U; ++j) {
        for (i = 0U; i < n; ++i) {
            unsigned int t = j * n + i + 1U;
            memcpy(block, a, 8U);
            memcpy(block + 8U, r + i * 8U, 8U);
            crypto_aes256_encrypt_block(&aes, block, block);
            memcpy(a, block, 8U);
            a[7] ^= (unsigned char)(t & 0xffU);
            a[6] ^= (unsigned char)((t >> 8U) & 0xffU);
            a[5] ^= (unsigned char)((t >> 16U) & 0xffU);
            a[4] ^= (unsigned char)((t >> 24U) & 0xffU);
            memcpy(r + i * 8U, block + 8U, 8U);
        }
    }
    if (msg_buffer_append_data(wrapped, a, 8U) != 0 || msg_buffer_append_data(wrapped, r, plain_size) != 0) return -1;
    crypto_secure_bzero(&aes, sizeof(aes));
    crypto_secure_bzero(a, sizeof(a));
    crypto_secure_bzero(r, sizeof(r));
    crypto_secure_bzero(block, sizeof(block));
    return 0;
}

static int pgpmsg_aes_key_unwrap(const unsigned char kek[PGPMSG_AES256_KEY_SIZE], const unsigned char *wrapped, size_t wrapped_size, unsigned char *plain, size_t *plain_size_out) {
    CryptoAes256Context aes;
    unsigned char a[8];
    unsigned char r[48];
    unsigned char block[16];
    unsigned int n;
    unsigned int j;
    unsigned int i;

    if (wrapped_size < 24U || (wrapped_size % 8U) != 0U || wrapped_size - 8U > sizeof(r)) return -1;
    n = (unsigned int)((wrapped_size / 8U) - 1U);
    memcpy(a, wrapped, 8U);
    memcpy(r, wrapped + 8U, wrapped_size - 8U);
    crypto_aes256_init(&aes, kek);
    for (j = 6U; j > 0U; --j) {
        for (i = n; i > 0U; --i) {
            unsigned int t = (j - 1U) * n + i;
            memcpy(block, a, 8U);
            block[7] ^= (unsigned char)(t & 0xffU);
            block[6] ^= (unsigned char)((t >> 8U) & 0xffU);
            block[5] ^= (unsigned char)((t >> 16U) & 0xffU);
            block[4] ^= (unsigned char)((t >> 24U) & 0xffU);
            memcpy(block + 8U, r + (i - 1U) * 8U, 8U);
            crypto_aes256_decrypt_block(&aes, block, block);
            memcpy(a, block, 8U);
            memcpy(r + (i - 1U) * 8U, block + 8U, 8U);
        }
    }
    if (!crypto_constant_time_equal(a, (const unsigned char *)"\xa6\xa6\xa6\xa6\xa6\xa6\xa6\xa6", 8U)) return -1;
    memcpy(plain, r, wrapped_size - 8U);
    *plain_size_out = wrapped_size - 8U;
    crypto_secure_bzero(&aes, sizeof(aes));
    crypto_secure_bzero(a, sizeof(a));
    crypto_secure_bzero(r, sizeof(r));
    crypto_secure_bzero(block, sizeof(block));
    return 0;
}

static const char *pgpmsg_symmetric_algorithm_name(unsigned int algorithm) {
    switch (algorithm) {
        case 0U: return "plaintext";
        case 1U: return "IDEA";
        case 2U: return "TripleDES";
        case 3U: return "CAST5";
        case 7U: return "AES-128";
        case 8U: return "AES-192";
        case 9U: return "AES-256";
        default: return "unknown";
    }
}

static const char *pgpmsg_aead_algorithm_name(unsigned int algorithm) {
    switch (algorithm) {
        case 1U: return "EAX";
        case 2U: return "OCB";
        case 3U: return "GCM";
        default: return "unknown";
    }
}

static int pgpmsg_base64_value(unsigned char value) {
    if (value >= 'A' && value <= 'Z') return (int)(value - 'A');
    if (value >= 'a' && value <= 'z') return (int)(value - 'a') + 26;
    if (value >= '0' && value <= '9') return (int)(value - '0') + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

static int pgpmsg_decode_crc24_text(const unsigned char *text, size_t text_size, unsigned char out[3]) {
    int a;
    int b;
    int c;
    int d;
    unsigned int value;

    if (text_size < 4U) return -1;
    a = pgpmsg_base64_value(text[0]);
    b = pgpmsg_base64_value(text[1]);
    c = pgpmsg_base64_value(text[2]);
    d = pgpmsg_base64_value(text[3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
    value = ((unsigned int)a << 18U) | ((unsigned int)b << 12U) | ((unsigned int)c << 6U) | (unsigned int)d;
    out[0] = (unsigned char)((value >> 16U) & 0xffU);
    out[1] = (unsigned char)((value >> 8U) & 0xffU);
    out[2] = (unsigned char)(value & 0xffU);
    return 0;
}

static int pgpmsg_line_starts_with(const unsigned char *line, size_t line_size, const char *prefix) {
    size_t index = 0U;

    while (prefix[index] != '\0') {
        if (index >= line_size || line[index] != (unsigned char)prefix[index]) return 0;
        index += 1U;
    }
    return 1;
}

static int pgpmsg_line_is_blank(const unsigned char *line, size_t line_size) {
    size_t index;

    for (index = 0U; index < line_size; ++index) {
        if (line[index] != ' ' && line[index] != '\t' && line[index] != '\r') return 0;
    }
    return 1;
}

static void inspect_analyze_input(const unsigned char *raw, size_t raw_size, size_t decoded_size, PgpMsgInspectInputInfo *info) {
    size_t offset = 0U;
    int in_body = 0;

    rt_memset(info, 0, sizeof(*info));
    info->raw_size = raw_size;
    info->decoded_size = decoded_size;
    while (offset < raw_size) {
        size_t line_start = offset;
        size_t line_end;
        size_t line_size;

        while (offset < raw_size && raw[offset] != '\n') offset += 1U;
        line_end = offset;
        if (offset < raw_size && raw[offset] == '\n') offset += 1U;
        if (line_end > line_start && raw[line_end - 1U] == '\r') line_end -= 1U;
        line_size = line_end - line_start;
        if (!info->armored) {
            static const char begin_prefix[] = "-----BEGIN PGP ";
            static const char end_marker[] = "-----";
            size_t kind_start = sizeof(begin_prefix) - 1U;
            size_t kind_end;
            size_t copy_size;

            if (!pgpmsg_line_starts_with(raw + line_start, line_size, begin_prefix)) break;
            info->armored = 1;
            kind_end = line_size;
            if (kind_end >= 5U && memcmp(raw + line_start + kind_end - 5U, end_marker, 5U) == 0) kind_end -= 5U;
            copy_size = kind_end > kind_start ? kind_end - kind_start : 0U;
            if (copy_size >= sizeof(info->armor_kind)) copy_size = sizeof(info->armor_kind) - 1U;
            memcpy(info->armor_kind, raw + line_start + kind_start, copy_size);
            info->armor_kind[copy_size] = '\0';
            continue;
        }
        if (pgpmsg_line_starts_with(raw + line_start, line_size, "-----END PGP ")) break;
        if (!in_body) {
            if (pgpmsg_line_is_blank(raw + line_start, line_size)) in_body = 1;
            continue;
        }
        if (line_size != 0U && raw[line_start] == '=') {
            if (pgpmsg_decode_crc24_text(raw + line_start + 1U, line_size - 1U, info->armor_crc24) == 0) info->has_armor_crc24 = 1;
            continue;
        }
        if (!pgpmsg_line_is_blank(raw + line_start, line_size)) {
            size_t index;

            info->armor_body_lines += 1ULL;
            for (index = 0U; index < line_size; ++index) {
                unsigned char ch = raw[line_start + index];
                if (ch != ' ' && ch != '\t' && ch != '\r') info->armor_base64_chars += 1ULL;
            }
        }
    }
}

static int load_decoded_input_with_info(const char *path, unsigned char **decoded_out, size_t *decoded_size_out, PgpMsgInspectInputInfo *info) {
    unsigned char *raw = 0;
    size_t raw_size = 0U;
    char error[PGPMSG_ERROR_CAPACITY];

    if (tool_read_all_input(path, &raw, &raw_size) != 0) {
        tool_write_error("pgpmsg", "cannot read input", path);
        return -1;
    }
    if (pgp_decode_input(raw, raw_size, decoded_out, decoded_size_out, error, sizeof(error)) != 0) {
        tool_write_error("pgpmsg", error, path);
        rt_free(raw);
        return -1;
    }
    if (info != 0) inspect_analyze_input(raw, raw_size, *decoded_size_out, info);
    rt_free(raw);
    return 0;
}

static int load_decoded_input(const char *path, unsigned char **decoded_out, size_t *decoded_size_out) {
    return load_decoded_input_with_info(path, decoded_out, decoded_size_out, 0);
}

static int parse_subpacket_length(const unsigned char *data, size_t size, size_t *offset_io, size_t *length_out) {
    unsigned int first;

    if (*offset_io >= size) return -1;
    first = data[*offset_io];
    *offset_io += 1U;
    if (first < 192U) {
        *length_out = first;
        return 0;
    }
    if (first < 255U) {
        unsigned int second;
        if (*offset_io >= size) return -1;
        second = data[*offset_io];
        *offset_io += 1U;
        *length_out = ((size_t)(first - 192U) << 8U) + (size_t)second + 192U;
        return 0;
    }
    if (*offset_io + 4U > size) return -1;
    *length_out = (size_t)tool_read_u32_be(data + *offset_io);
    *offset_io += 4U;
    return 0;
}

static void parse_signature_subpackets(PgpMsgSignatureSummary *summary, const unsigned char *data, size_t size) {
    size_t offset = 0U;

    while (offset < size) {
        size_t subpacket_length = 0U;
        unsigned int type;
        const unsigned char *body;
        size_t body_size;

        if (parse_subpacket_length(data, size, &offset, &subpacket_length) != 0 || subpacket_length == 0U || subpacket_length > size - offset) return;
        type = data[offset] & 0x7fU;
        body = data + offset + 1U;
        body_size = subpacket_length - 1U;
        if (type == 2U && body_size >= 4U) {
            summary->created = tool_read_u32_be(body);
        } else if (type == 16U && body_size >= PGPMSG_KEY_ID_SIZE) {
            memcpy(summary->issuer_key_id, body, PGPMSG_KEY_ID_SIZE);
            summary->has_issuer_key_id = 1;
        } else if (type == 33U && body_size >= 2U && body_size - 1U <= PGP_FINGERPRINT_MAX_SIZE) {
            summary->issuer_fingerprint_size = body_size - 1U;
            memcpy(summary->issuer_fingerprint, body + 1U, summary->issuer_fingerprint_size);
        }
        offset += subpacket_length;
    }
}

static int read_mpi_bytes(const unsigned char *data, size_t size, size_t *offset_io, unsigned char *out, size_t out_size) {
    unsigned int bits;
    size_t bytes;

    if (*offset_io + 2U > size) return -1;
    bits = read_u16_be(data + *offset_io);
    *offset_io += 2U;
    bytes = ((size_t)bits + 7U) / 8U;
    if (bytes > size - *offset_io || bytes > out_size) return -1;
    if (bytes < out_size) rt_memset(out, 0, out_size - bytes);
    memcpy(out + (out_size - bytes), data + *offset_io, bytes);
    *offset_io += bytes;
    return 0;
}

static int read_mpi_view(const unsigned char *data, size_t size, size_t *offset_io, const unsigned char **data_out, size_t *size_out, unsigned int *bits_out) {
    unsigned int bits;
    size_t bytes;

    if (*offset_io + 2U > size) return -1;
    bits = read_u16_be(data + *offset_io);
    *offset_io += 2U;
    bytes = ((size_t)bits + 7U) / 8U;
    if (bytes > size - *offset_io) return -1;
    *data_out = data + *offset_io;
    *size_out = bytes;
    *bits_out = bits;
    *offset_io += bytes;
    return 0;
}

static int parse_signature_packet_full(PgpMsgSignaturePacket *signature, const unsigned char *body, size_t body_size) {
    PgpMsgSignatureSummary *summary = &signature->summary;
    size_t hashed_size;
    size_t unhashed_offset;
    size_t unhashed_size;
    size_t signature_offset;

    rt_memset(signature, 0, sizeof(*signature));
    rt_memset(summary, 0, sizeof(*summary));
    if (body_size == 0U) return -1;
    summary->version = body[0];
    if (summary->version == 3U) {
        if (body_size < 19U || body[1] != 5U) return -1;
        summary->signature_type = body[2];
        summary->created = tool_read_u32_be(body + 3U);
        memcpy(summary->issuer_key_id, body + 7U, PGPMSG_KEY_ID_SIZE);
        summary->has_issuer_key_id = 1;
        summary->public_key_algorithm = body[15];
        summary->hash_algorithm = body[16];
        summary->present = 1;
        return 0;
    }
    if (summary->version == 6U) {
        size_t salt_size;

        if (body_size < 10U) return -1;
        summary->signature_type = body[1];
        summary->public_key_algorithm = body[2];
        summary->hash_algorithm = body[3];
        hashed_size = tool_read_u32_be(body + 4U);
        if (8U + hashed_size + 4U > body_size) return -1;
        signature->hash_part_size = 8U + hashed_size;
        parse_signature_subpackets(summary, body + 8U, hashed_size);
        unhashed_offset = 8U + hashed_size;
        unhashed_size = tool_read_u32_be(body + unhashed_offset);
        unhashed_offset += 4U;
        if (unhashed_offset + unhashed_size + 3U > body_size) return -1;
        parse_signature_subpackets(summary, body + unhashed_offset, unhashed_size);
        signature_offset = unhashed_offset + unhashed_size;
        signature->digest_prefix[0] = body[signature_offset++];
        signature->digest_prefix[1] = body[signature_offset++];
        salt_size = (size_t)body[signature_offset++];
        if (salt_size != 32U || salt_size > sizeof(signature->salt) || salt_size > body_size - signature_offset) return -1;
        signature->salt_offset = signature_offset;
        signature->salt_size = salt_size;
        memcpy(signature->salt, body + signature_offset, salt_size);
        signature_offset += salt_size;
        if (summary->public_key_algorithm == 27U) {
            if (body_size - signature_offset != PGPMSG_ED25519_SIGNATURE_SIZE) return -1;
            memcpy(signature->signature, body + signature_offset, PGPMSG_ED25519_SIGNATURE_SIZE);
            signature->has_signature = 1;
        }
        summary->present = 1;
        return 0;
    }
    if (summary->version != 4U || body_size < 6U) return -1;
    summary->signature_type = body[1];
    summary->public_key_algorithm = body[2];
    summary->hash_algorithm = body[3];
    hashed_size = read_u16_be(body + 4U);
    if (6U + hashed_size + 2U > body_size) return -1;
    signature->hash_part_size = 6U + hashed_size;
    parse_signature_subpackets(summary, body + 6U, hashed_size);
    unhashed_offset = 6U + hashed_size;
    unhashed_size = read_u16_be(body + unhashed_offset);
    unhashed_offset += 2U;
    if (unhashed_offset + unhashed_size > body_size) return -1;
    parse_signature_subpackets(summary, body + unhashed_offset, unhashed_size);
    signature_offset = unhashed_offset + unhashed_size;
    if (signature_offset + 2U > body_size) return -1;
    signature->digest_prefix[0] = body[signature_offset++];
    signature->digest_prefix[1] = body[signature_offset++];
    if (summary->public_key_algorithm == 22U) {
        if (read_mpi_bytes(body, body_size, &signature_offset, signature->signature, 32U) != 0 ||
            read_mpi_bytes(body, body_size, &signature_offset, signature->signature + 32U, 32U) != 0 ||
            signature_offset != body_size) {
            return -1;
        }
        signature->has_signature = 1;
    }
    summary->present = 1;
    return 0;
}

static int write_signature_summary_text(const PgpMsgSignatureSummary *summary) {
    if (rt_write_cstr(1, "signature: not checked\n") != 0) return -1;
    if (rt_write_cstr(1, "type: ") != 0 || rt_write_line(1, pgp_signature_type_name(summary->signature_type)) != 0) return -1;
    if (rt_write_cstr(1, "public-key-algorithm: ") != 0 || rt_write_line(1, pgp_public_key_algorithm_name(summary->public_key_algorithm)) != 0) return -1;
    if (rt_write_cstr(1, "hash: ") != 0 || rt_write_line(1, pgp_hash_algorithm_name(summary->hash_algorithm)) != 0) return -1;
    if (summary->created != 0ULL) {
        if (rt_write_cstr(1, "created: ") != 0 || pgp_write_date(1, summary->created) != 0 || rt_write_char(1, '\n') != 0) return -1;
    }
    if (summary->has_issuer_key_id) {
        if (rt_write_cstr(1, "issuer: ") != 0 || tool_write_hex_bytes(1, summary->issuer_key_id, PGPMSG_KEY_ID_SIZE) != 0 || rt_write_char(1, '\n') != 0) return -1;
    }
    if (summary->issuer_fingerprint_size != 0U) {
        if (rt_write_cstr(1, "issuer-fpr: ") != 0 || tool_write_hex_bytes(1, summary->issuer_fingerprint, summary->issuer_fingerprint_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
    }
    return rt_write_line(1, "trust: not evaluated");
}

static int write_signature_summary_json(const PgpMsgSignatureSummary *summary, const char *status) {
    if (tool_json_begin_event(1, "pgpmsg", "stdout", "signature") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"status\":") != 0 || tool_json_write_string(1, status) != 0) return -1;
    if (rt_write_cstr(1, ",\"type\":") != 0 || tool_json_write_string(1, pgp_signature_type_name(summary->signature_type)) != 0) return -1;
    if (rt_write_cstr(1, ",\"public_key_algorithm\":") != 0 || tool_json_write_string(1, pgp_public_key_algorithm_name(summary->public_key_algorithm)) != 0) return -1;
    if (rt_write_cstr(1, ",\"hash\":") != 0 || tool_json_write_string(1, pgp_hash_algorithm_name(summary->hash_algorithm)) != 0) return -1;
    if (summary->created != 0ULL && rt_write_cstr(1, ",\"created\":") != 0) return -1;
    if (summary->created != 0ULL && rt_write_uint(1, summary->created) != 0) return -1;
    if (summary->has_issuer_key_id) {
        if (rt_write_cstr(1, ",\"issuer\":\"") != 0 || tool_write_hex_bytes(1, summary->issuer_key_id, PGPMSG_KEY_ID_SIZE) != 0 || rt_write_char(1, '"') != 0) return -1;
    }
    if (summary->issuer_fingerprint_size != 0U) {
        if (rt_write_cstr(1, ",\"issuer_fingerprint\":\"") != 0 || tool_write_hex_bytes(1, summary->issuer_fingerprint, summary->issuer_fingerprint_size) != 0 || rt_write_char(1, '"') != 0) return -1;
    }
    if (rt_write_cstr(1, ",\"trust\":\"not_evaluated\"}}") != 0) return -1;
    return tool_json_end_event(1);
}

static int signature_matches_key(const PgpMsgSignatureSummary *signature, const PgpPublicKeyInfo *key) {
    if (signature->issuer_fingerprint_size != 0U && signature->issuer_fingerprint_size == key->fingerprint_size) {
        if (memcmp(signature->issuer_fingerprint, key->fingerprint, key->fingerprint_size) == 0) return 1;
    }
    if (signature->has_issuer_key_id) {
        if (memcmp(signature->issuer_key_id, key->key_id, PGP_KEY_ID_SIZE) == 0) return 1;
    }
    return 0;
}

static int public_key_find_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpMsgPublicKeyFindContext *ctx = (PgpMsgPublicKeyFindContext *)ctx_ptr;
    size_t index;

    if (ctx->found) return 0;
    if ((certificate->primary.algorithm == 22U || certificate->primary.algorithm == 27U) && certificate->primary.public_material_size == PGPMSG_ED25519_KEY_SIZE && signature_matches_key(ctx->signature, &certificate->primary)) {
        ctx->key = certificate->primary;
        ctx->found = 1;
        return 0;
    }
    for (index = 0U; index < certificate->subkey_count; ++index) {
        if ((certificate->subkeys[index].algorithm == 22U || certificate->subkeys[index].algorithm == 27U) && certificate->subkeys[index].public_material_size == PGPMSG_ED25519_KEY_SIZE && signature_matches_key(ctx->signature, &certificate->subkeys[index])) {
            ctx->key = certificate->subkeys[index];
            ctx->found = 1;
            return 0;
        }
    }
    return 0;
}

static int signer_usage_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpMsgSignerUsageContext *ctx = (PgpMsgSignerUsageContext *)ctx_ptr;

    if (ctx->found) return 0;
    if ((certificate->primary.algorithm != 22U && certificate->primary.algorithm != 27U) || certificate->primary.public_material_size != PGPMSG_ED25519_KEY_SIZE) return 0;
    if (!selector_matches_user_id(certificate, ctx->selector)) return 0;
    ctx->key = certificate->primary;
    ctx->found = 1;
    ctx->allowed = ctx->danger_anyway || pgpmsg_certificate_primary_allows_sign(certificate);
    return 0;
}

static int load_public_key_for_signature(const char *path, const PgpMsgSignatureSummary *signature, PgpPublicKeyInfo *key_out) {
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;
    PgpMsgPublicKeyFindContext ctx;
    char error[PGPMSG_ERROR_CAPACITY];

    if (path == 0) {
        tool_write_error("pgpmsg", "verification requires -k PUBRING for Ed25519 signatures", 0);
        return -1;
    }
    if (load_decoded_input(path, &decoded, &decoded_size) != 0) return -1;
    rt_memset(&ctx, 0, sizeof(ctx));
    ctx.signature = signature;
    if (pgp_for_each_certificate(decoded, decoded_size, public_key_find_callback, &ctx, error, sizeof(error)) != 0 && !ctx.found) {
        rt_free(decoded);
        tool_write_error("pgpmsg", error, path);
        return -1;
    }
    rt_free(decoded);
    if (!ctx.found) {
        tool_write_error("pgpmsg", "no matching Ed25519 public key in ", path);
        return -1;
    }
    *key_out = ctx.key;
    return 0;
}

static int hash_detached_signature_data_ex(unsigned int signature_version, const unsigned char *salt, size_t salt_size, const unsigned char *data, size_t data_size, const unsigned char *signature_body, size_t hash_part_size, unsigned char digest[CRYPTO_SHA512_DIGEST_SIZE]) {
    CryptoSha512Context sha512;
    unsigned char trailer[6];

    crypto_sha512_init(&sha512);
    if (signature_version == 6U && salt_size != 0U) crypto_sha512_update(&sha512, salt, salt_size);
    crypto_sha512_update(&sha512, data, data_size);
    crypto_sha512_update(&sha512, signature_body, hash_part_size);
    trailer[0] = (unsigned char)signature_version;
    trailer[1] = 0xffU;
    trailer[2] = (unsigned char)((hash_part_size >> 24U) & 0xffU);
    trailer[3] = (unsigned char)((hash_part_size >> 16U) & 0xffU);
    trailer[4] = (unsigned char)((hash_part_size >> 8U) & 0xffU);
    trailer[5] = (unsigned char)(hash_part_size & 0xffU);
    crypto_sha512_update(&sha512, trailer, sizeof(trailer));
    crypto_sha512_final(&sha512, digest);
    return 0;
}

static int canonicalize_cleartext_for_signature(const unsigned char *input, size_t input_size, PgpMsgBuffer *canonical) {
    size_t offset = 0U;

    while (offset < input_size) {
        size_t line_start = offset;
        size_t line_end;

        while (offset < input_size && input[offset] != '\r' && input[offset] != '\n') offset += 1U;
        line_end = offset;
        if (offset < input_size && input[offset] == '\r') offset += 1U;
        if (offset < input_size && input[offset] == '\n') offset += 1U;
        if (msg_buffer_append_data(canonical, input + line_start, line_end - line_start) != 0 ||
            msg_buffer_append_byte(canonical, '\r') != 0 ||
            msg_buffer_append_byte(canonical, '\n') != 0) return -1;
    }
    if (input_size == 0U) {
        if (msg_buffer_append_byte(canonical, '\r') != 0 || msg_buffer_append_byte(canonical, '\n') != 0) return -1;
    }
    return 0;
}

static int verify_detached_signature_packet(const PgpMsgSignaturePacket *signature, const unsigned char *signature_body, const unsigned char *data, size_t data_size, const PgpPublicKeyInfo *key) {
    unsigned char digest[CRYPTO_SHA512_DIGEST_SIZE];
    int ok;

    if (signature->summary.signature_type != 0x00U || signature->summary.hash_algorithm != 10U || !signature->has_signature) return -1;
    if (signature->summary.version == 4U) {
        if (signature->summary.public_key_algorithm != 22U || key->version != 4U || key->algorithm != 22U) return -1;
        hash_detached_signature_data_ex(4U, 0, 0U, data, data_size, signature_body, signature->hash_part_size, digest);
    } else if (signature->summary.version == 6U) {
        if (signature->summary.public_key_algorithm != 27U || key->version != 6U || key->algorithm != 27U || signature->salt_size != 32U) return -1;
        hash_detached_signature_data_ex(6U, signature->salt, signature->salt_size, data, data_size, signature_body, signature->hash_part_size, digest);
    } else {
        return -1;
    }
    if (digest[0] != signature->digest_prefix[0] || digest[1] != signature->digest_prefix[1]) {
        crypto_secure_bzero(digest, sizeof(digest));
        return 0;
    }
    ok = crypto_ed25519_verify(signature->signature, digest, sizeof(digest), key->public_material) == 0;
    crypto_secure_bzero(digest, sizeof(digest));
    return ok ? 1 : 0;
}

static int build_signature_packet_for_data(const PgpMsgSecretKey *secret, const unsigned char *data, size_t data_size, unsigned int signature_type, PgpMsgBuffer *signature_packet) {
    PgpMsgBuffer hashed;
    PgpMsgBuffer unhashed;
    PgpMsgBuffer hash_part;
    PgpMsgBuffer signature_body;
    unsigned char digest[CRYPTO_SHA512_DIGEST_SIZE];
    unsigned char signature[PGPMSG_ED25519_SIGNATURE_SIZE];
    unsigned char issuer_fingerprint[PGP_FINGERPRINT_MAX_SIZE + 1U];
    unsigned char signature_salt[32];
    unsigned long long created;
    unsigned int signature_version;
    unsigned int public_key_algorithm;
    int result = -1;

    rt_memset(&hashed, 0, sizeof(hashed));
    rt_memset(&unhashed, 0, sizeof(unhashed));
    rt_memset(&hash_part, 0, sizeof(hash_part));
    rt_memset(&signature_body, 0, sizeof(signature_body));
    rt_memset(signature_salt, 0, sizeof(signature_salt));
    if (secret == 0 || (signature_type != 0x00U && signature_type != 0x01U)) goto cleanup;
    created = platform_get_epoch_time() > 0 ? (unsigned long long)platform_get_epoch_time() : 1ULL;
    signature_version = secret->info.version == 6U ? 6U : 4U;
    public_key_algorithm = signature_version == 6U ? 27U : 22U;
    issuer_fingerprint[0] = (unsigned char)signature_version;
    memcpy(issuer_fingerprint + 1U, secret->info.fingerprint, secret->info.fingerprint_size);
    if (signature_version == 6U && platform_random_bytes(signature_salt, sizeof(signature_salt)) != 0) goto cleanup;
    if (msg_buffer_append_signature_subpacket_u32(&hashed, signature_version == 6U ? 0x82U : 2U, created) != 0 ||
        (signature_version == 4U && msg_buffer_append_signature_subpacket(&unhashed, 16U, secret->info.key_id, PGP_KEY_ID_SIZE) != 0) ||
        msg_buffer_append_signature_subpacket(signature_version == 6U ? &hashed : &unhashed, 33U, issuer_fingerprint, secret->info.fingerprint_size + 1U) != 0 ||
        msg_buffer_append_byte(&hash_part, signature_version) != 0 ||
        msg_buffer_append_byte(&hash_part, signature_type) != 0 ||
        msg_buffer_append_byte(&hash_part, public_key_algorithm) != 0 ||
        msg_buffer_append_byte(&hash_part, 10U) != 0 ||
        (signature_version == 6U ? msg_buffer_append_u32_be(&hash_part, hashed.size) : msg_buffer_append_u16_be(&hash_part, (unsigned int)hashed.size)) != 0 ||
        msg_buffer_append_data(&hash_part, hashed.data, hashed.size) != 0) goto cleanup;
    hash_detached_signature_data_ex(signature_version, signature_version == 6U ? signature_salt : 0, signature_version == 6U ? sizeof(signature_salt) : 0U, data, data_size, hash_part.data, hash_part.size, digest);
    if (crypto_ed25519_sign(signature, digest, sizeof(digest), secret->seed, secret->info.public_material) != 0) goto cleanup;
    if (msg_buffer_append_data(&signature_body, hash_part.data, hash_part.size) != 0 ||
        (signature_version == 6U ? msg_buffer_append_u32_be(&signature_body, unhashed.size) : msg_buffer_append_u16_be(&signature_body, (unsigned int)unhashed.size)) != 0 ||
        msg_buffer_append_data(&signature_body, unhashed.data, unhashed.size) != 0 ||
        msg_buffer_append_byte(&signature_body, digest[0]) != 0 ||
        msg_buffer_append_byte(&signature_body, digest[1]) != 0) goto cleanup;
    if (signature_version == 6U) {
        if (msg_buffer_append_byte(&signature_body, sizeof(signature_salt)) != 0 ||
            msg_buffer_append_data(&signature_body, signature_salt, sizeof(signature_salt)) != 0 ||
            msg_buffer_append_data(&signature_body, signature, sizeof(signature)) != 0) goto cleanup;
    } else if (msg_buffer_append_opaque_mpi(&signature_body, signature, 32U, 256U) != 0 ||
               msg_buffer_append_opaque_mpi(&signature_body, signature + 32U, 32U, 256U) != 0) {
        goto cleanup;
    }
    if (msg_buffer_append_packet(signature_packet, 2U, &signature_body) != 0) goto cleanup;
    result = 0;

cleanup:
    msg_buffer_free(&hashed);
    msg_buffer_free(&unhashed);
    msg_buffer_free(&hash_part);
    msg_buffer_free(&signature_body);
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(signature, sizeof(signature));
    crypto_secure_bzero(signature_salt, sizeof(signature_salt));
    return result;
}

static int parse_ed25519_secret_key_packet(PgpMsgSecretKey *secret, unsigned int tag, const unsigned char *body, size_t body_size, const PgpPublicKeyInfo *selected_key) {
    size_t offset = 6U;
    unsigned int oid_size;
    unsigned int point_bits;
    size_t point_bytes;
    unsigned int secret_bits;
    size_t secret_bytes;
    size_t checksum_start;
    size_t index;
    unsigned int checksum = 0U;
    unsigned int stored_checksum;

    if (pgp_parse_public_key_packet(&secret->info, tag, body, body_size, 0, 0U) != 0) return -1;
    if (secret->info.version == 6U) {
        offset = 10U + secret->info.public_material_size;
        if (secret->info.algorithm != 27U || secret->info.public_material_size != PGPMSG_ED25519_KEY_SIZE) return -1;
        if (selected_key != 0 && (secret->info.fingerprint_size != selected_key->fingerprint_size || memcmp(secret->info.fingerprint, selected_key->fingerprint, selected_key->fingerprint_size) != 0)) return -1;
        if (offset >= body_size || body[offset++] != 0U) return -1;
        if (body_size - offset != PGPMSG_ED25519_KEY_SIZE) return -1;
        memcpy(secret->seed, body + offset, PGPMSG_ED25519_KEY_SIZE);
        secret->found = 1;
        return 0;
    }
    if (secret->info.algorithm != 22U || secret->info.public_material_size != PGPMSG_ED25519_KEY_SIZE) return -1;
    if (selected_key != 0 && (secret->info.fingerprint_size != selected_key->fingerprint_size || memcmp(secret->info.fingerprint, selected_key->fingerprint, selected_key->fingerprint_size) != 0)) return -1;
    if (body_size < 6U || offset >= body_size) return -1;
    oid_size = body[offset++];
    if (oid_size > body_size - offset) return -1;
    offset += oid_size;
    if (offset + 2U > body_size) return -1;
    point_bits = read_u16_be(body + offset);
    offset += 2U;
    point_bytes = ((size_t)point_bits + 7U) / 8U;
    if (point_bytes > body_size - offset) return -1;
    offset += point_bytes;
    if (offset >= body_size || body[offset++] != 0U) return -1;
    checksum_start = offset;
    if (offset + 2U > body_size) return -1;
    secret_bits = read_u16_be(body + offset);
    offset += 2U;
    secret_bytes = ((size_t)secret_bits + 7U) / 8U;
    if (secret_bytes != PGPMSG_ED25519_KEY_SIZE || secret_bytes > body_size - offset) return -1;
    memcpy(secret->seed, body + offset, PGPMSG_ED25519_KEY_SIZE);
    offset += secret_bytes;
    if (offset + 2U != body_size) return -1;
    for (index = checksum_start; index < offset; ++index) checksum = (checksum + body[index]) & 0xffffU;
    stored_checksum = read_u16_be(body + offset);
    if (checksum != stored_checksum) return -1;
    secret->found = 1;
    return 0;
}

static int load_secret_key(const char *path, const char *selector, int danger_anyway, PgpMsgSecretKey *secret) {
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;
    PgpPacketReader reader;
    PgpMsgSignerUsageContext usage;
    char error[PGPMSG_ERROR_CAPACITY];

    if (path == 0) {
        tool_write_error("pgpmsg", "signing requires -s SECRING", 0);
        return -1;
    }
    if (load_decoded_input(path, &decoded, &decoded_size) != 0) return -1;
    rt_memset(secret, 0, sizeof(*secret));
    rt_memset(&usage, 0, sizeof(usage));
    usage.selector = selector;
    usage.danger_anyway = danger_anyway;
    if (pgp_for_each_certificate(decoded, decoded_size, signer_usage_callback, &usage, error, sizeof(error)) != 0) {
        rt_free(decoded);
        tool_write_error("pgpmsg", error, path);
        return -1;
    }
    if (usage.found && !usage.allowed) {
        rt_free(decoded);
        tool_write_error("pgpmsg", "matching Ed25519 secret key lacks signing usage flags in ", path);
        return -1;
    }
    pgp_packet_reader_init(&reader, decoded, decoded_size);
    while (1) {
        PgpPacket packet;
        int has_packet;

        if (pgp_packet_reader_next(&reader, &packet, &has_packet, error, sizeof(error)) != 0) {
            rt_free(decoded);
            tool_write_error("pgpmsg", error, path);
            return -1;
        }
        if (!has_packet) break;
        if ((packet.tag == 5U || packet.tag == 7U) && parse_ed25519_secret_key_packet(secret, packet.tag, decoded + packet.body_offset, packet.body_size, usage.found ? &usage.key : 0) == 0 && secret->found) {
            rt_free(decoded);
            return 0;
        }
    }
    rt_free(decoded);
    tool_write_error("pgpmsg", "no matching unprotected Ed25519 secret key in ", path);
    return -1;
}

static int recipient_find_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpMsgRecipientFindContext *ctx = (PgpMsgRecipientFindContext *)ctx_ptr;
    int certificate_match = selector_matches_user_id(certificate, ctx->selector);
    int best_rank = 0;
    size_t index;

    if (ctx->found) return 0;
    for (index = 0U; index < certificate->subkey_count; ++index) {
        const PgpPublicKeyInfo *subkey = &certificate->subkeys[index];
        int rank;

        if ((subkey->algorithm == 25U || subkey->algorithm == 18U) && subkey->public_material_size == PGPMSG_X25519_KEY_SIZE) rank = subkey->algorithm == 25U ? 4 : 3;
        else if ((subkey->algorithm == 1U || subkey->algorithm == 2U) && subkey->public_material_size != 0U) rank = 2;
        else if (subkey->algorithm == 16U && subkey->public_material_size != 0U) rank = 1;
        else continue;
        if (certificate_match || pgp_fingerprint_matches_text(subkey, ctx->selector)) {
            if (!ctx->danger_anyway && !pgpmsg_certificate_subkey_allows_encrypt(certificate, index)) continue;
            if (rank > best_rank) {
                ctx->key = *subkey;
                best_rank = rank;
                if (rank == 4) {
                    ctx->found = 1;
                    return 0;
                }
            }
        }
    }
    if (best_rank != 0) ctx->found = 1;
    return 0;
}

static int load_recipient_key(const char *path, const char *selector, int danger_anyway, PgpPublicKeyInfo *key_out) {
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;
    PgpMsgRecipientFindContext ctx;
    char error[PGPMSG_ERROR_CAPACITY];

    if (path == 0) {
        tool_write_error("pgpmsg", "encryption requires -k PUBRING", 0);
        return -1;
    }
    if (load_decoded_input(path, &decoded, &decoded_size) != 0) return -1;
    rt_memset(&ctx, 0, sizeof(ctx));
    ctx.selector = selector;
    ctx.danger_anyway = danger_anyway;
    if (pgp_for_each_certificate(decoded, decoded_size, recipient_find_callback, &ctx, error, sizeof(error)) != 0 && !ctx.found) {
        rt_free(decoded);
        tool_write_error("pgpmsg", error, path);
        return -1;
    }
    rt_free(decoded);
    if (!ctx.found) {
        tool_write_error("pgpmsg", danger_anyway ? "no matching supported encryption subkey in " : "no matching supported encryption subkey with encryption usage flags in ", path);
        return -1;
    }
    *key_out = ctx.key;
    return 0;
}

static int parse_x25519_secret_key_packet(PgpMsgX25519SecretKey *secret, unsigned int tag, const unsigned char *body, size_t body_size, const PgpMsgPkesk *pkesk) {
    size_t offset = 6U;
    unsigned int oid_size;
    unsigned int point_bits;
    size_t point_bytes;
    unsigned int kdf_size;
    unsigned int secret_bits;
    size_t secret_bytes;
    size_t checksum_start;
    size_t index;
    unsigned int checksum = 0U;
    unsigned int stored_checksum;

    if (pgp_parse_public_key_packet(&secret->info, tag, body, body_size, 0, 0U) != 0) return -1;
    if (secret->info.version == 6U) {
        offset = 10U + secret->info.public_material_size;
        if (secret->info.algorithm != 25U || secret->info.public_material_size != PGPMSG_X25519_KEY_SIZE) return -1;
        if (pkesk->version != 6U || pkesk->fingerprint_size != secret->info.fingerprint_size || memcmp(pkesk->fingerprint, secret->info.fingerprint, secret->info.fingerprint_size) != 0) return -1;
        if (offset >= body_size || body[offset++] != 0U) return -1;
        if (body_size - offset != PGPMSG_X25519_KEY_SIZE) return -1;
        memcpy(secret->seed, body + offset, PGPMSG_X25519_KEY_SIZE);
        secret->found = 1;
        return 0;
    }
    if (secret->info.algorithm != 18U || secret->info.public_material_size != PGPMSG_X25519_KEY_SIZE) return -1;
    if (pkesk->version != 3U || !key_id_matches_bytes(&secret->info, pkesk->key_id)) return -1;
    if (body_size < 6U || offset >= body_size) return -1;
    oid_size = body[offset++];
    if (oid_size > body_size - offset) return -1;
    offset += oid_size;
    if (offset + 2U > body_size) return -1;
    point_bits = read_u16_be(body + offset);
    offset += 2U;
    point_bytes = ((size_t)point_bits + 7U) / 8U;
    if (point_bytes > body_size - offset) return -1;
    offset += point_bytes;
    if (offset >= body_size) return -1;
    kdf_size = body[offset++];
    if (kdf_size > body_size - offset) return -1;
    offset += kdf_size;
    if (offset >= body_size || body[offset++] != 0U) return -1;
    checksum_start = offset;
    if (offset + 2U > body_size) return -1;
    secret_bits = read_u16_be(body + offset);
    offset += 2U;
    secret_bytes = ((size_t)secret_bits + 7U) / 8U;
    if (secret_bytes != PGPMSG_X25519_KEY_SIZE || secret_bytes > body_size - offset) return -1;
    memcpy(secret->seed, body + offset, PGPMSG_X25519_KEY_SIZE);
    offset += secret_bytes;
    if (offset + 2U != body_size) return -1;
    for (index = checksum_start; index < offset; ++index) checksum = (checksum + body[index]) & 0xffffU;
    stored_checksum = read_u16_be(body + offset);
    if (checksum != stored_checksum) return -1;
    secret->found = 1;
    return 0;
}

static int load_x25519_secret_key_maybe_quiet(const char *path, const PgpMsgPkesk *pkesk, PgpMsgX25519SecretKey *secret, int quiet_not_found) {
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;
    PgpPacketReader reader;
    char error[PGPMSG_ERROR_CAPACITY];

    if (path == 0) {
        tool_write_error("pgpmsg", "decryption requires -s SECRING", 0);
        return -1;
    }
    if (load_decoded_input(path, &decoded, &decoded_size) != 0) return -1;
    rt_memset(secret, 0, sizeof(*secret));
    pgp_packet_reader_init(&reader, decoded, decoded_size);
    while (1) {
        PgpPacket packet;
        int has_packet;

        if (pgp_packet_reader_next(&reader, &packet, &has_packet, error, sizeof(error)) != 0) {
            rt_free(decoded);
            tool_write_error("pgpmsg", error, path);
            return -1;
        }
        if (!has_packet) break;
        if ((packet.tag == 5U || packet.tag == 7U) && parse_x25519_secret_key_packet(secret, packet.tag, decoded + packet.body_offset, packet.body_size, pkesk) == 0 && secret->found) {
            rt_free(decoded);
            return 0;
        }
    }
    rt_free(decoded);
    if (!quiet_not_found) tool_write_error("pgpmsg", "no matching unprotected X25519 secret subkey in ", path);
    return -1;
}

static int parse_pkesk_packet(PgpMsgPkesk *pkesk, const unsigned char *body, size_t body_size) {
    const unsigned char *ephemeral = 0;
    size_t ephemeral_size = 0U;
    unsigned int ephemeral_bits = 0U;
    size_t offset = 0U;
    unsigned int wrapped_size;

    if (body_size < 1U) return -1;
    pkesk->version = body[offset++];
    if (pkesk->version == 6U) {
        unsigned int fingerprint_field_size;

        if (offset >= body_size) return -1;
        fingerprint_field_size = body[offset++];
        if (fingerprint_field_size == 0U || fingerprint_field_size > PGP_FINGERPRINT_MAX_SIZE + 1U || fingerprint_field_size > body_size - offset) return -1;
        pkesk->key_version = body[offset++];
        pkesk->fingerprint_size = (size_t)fingerprint_field_size - 1U;
        if (pkesk->fingerprint_size > sizeof(pkesk->fingerprint) || pkesk->fingerprint_size > body_size - offset) return -1;
        memcpy(pkesk->fingerprint, body + offset, pkesk->fingerprint_size);
        offset += pkesk->fingerprint_size;
        if (offset >= body_size) return -1;
        pkesk->public_key_algorithm = body[offset++];
        if (pkesk->public_key_algorithm != 25U) return -1;
        if (PGPMSG_X25519_KEY_SIZE > body_size - offset) return -1;
        memcpy(pkesk->ephemeral_public, body + offset, PGPMSG_X25519_KEY_SIZE);
        offset += PGPMSG_X25519_KEY_SIZE;
        if (offset >= body_size) return -1;
        wrapped_size = body[offset++];
        if (wrapped_size > sizeof(pkesk->wrapped_session_key) || wrapped_size != body_size - offset) return -1;
        memcpy(pkesk->wrapped_session_key, body + offset, wrapped_size);
        pkesk->wrapped_session_key_size = wrapped_size;
        pkesk->found = 1;
        return 0;
    }
    if (body_size < 11U || pkesk->version != 3U) return -1;
    memcpy(pkesk->key_id, body + offset, PGPMSG_KEY_ID_SIZE);
    offset += PGPMSG_KEY_ID_SIZE;
    pkesk->public_key_algorithm = body[offset++];
    if (pkesk->public_key_algorithm != 18U) return -1;
    if (read_mpi_view(body, body_size, &offset, &ephemeral, &ephemeral_size, &ephemeral_bits) != 0) return -1;
    if (ephemeral_bits != 263U || ephemeral_size != PGPMSG_X25519_KEY_SIZE + 1U || ephemeral[0] != 0x40U) return -1;
    memcpy(pkesk->ephemeral_public, ephemeral + 1U, PGPMSG_X25519_KEY_SIZE);
    if (offset >= body_size) return -1;
    wrapped_size = body[offset++];
    if (wrapped_size > sizeof(pkesk->wrapped_session_key) || wrapped_size != body_size - offset) return -1;
    memcpy(pkesk->wrapped_session_key, body + offset, wrapped_size);
    pkesk->wrapped_session_key_size = wrapped_size;
    pkesk->found = 1;
    return 0;
}

static unsigned int pgpmsg_session_key_checksum(const unsigned char *session_key, size_t session_key_size) {
    unsigned int checksum = 0U;
    size_t index;

    for (index = 0U; index < session_key_size; ++index) checksum = (checksum + session_key[index]) & 0xffffU;
    return checksum;
}

static size_t pgpmsg_be_skip_zeroes(const unsigned char *data, size_t size) {
    size_t offset = 0U;

    while (offset < size && data[offset] == 0U) offset += 1U;
    return offset;
}

static int pgpmsg_be_compare(const unsigned char *left, size_t left_size, const unsigned char *right, size_t right_size) {
    size_t left_offset = pgpmsg_be_skip_zeroes(left, left_size);
    size_t right_offset = pgpmsg_be_skip_zeroes(right, right_size);
    size_t left_len = left_size - left_offset;
    size_t right_len = right_size - right_offset;
    size_t index;

    if (left_len > right_len) return 1;
    if (left_len < right_len) return -1;
    for (index = 0U; index < left_len; ++index) {
        unsigned char left_byte = left[left_offset + index];
        unsigned char right_byte = right[right_offset + index];

        if (left_byte > right_byte) return 1;
        if (left_byte < right_byte) return -1;
    }
    return 0;
}

static int pgpmsg_be_is_zero(const unsigned char *data, size_t size) {
    return pgpmsg_be_skip_zeroes(data, size) == size;
}

static int pgpmsg_be_is_one(const unsigned char *data, size_t size) {
    size_t offset = pgpmsg_be_skip_zeroes(data, size);

    return offset + 1U == size && data[offset] == 1U;
}

static int pgpmsg_be_decrement_copy(unsigned char *out, const unsigned char *in, size_t size) {
    size_t index;

    memcpy(out, in, size);
    for (index = size; index > 0U; --index) {
        if (out[index - 1U] != 0U) {
            out[index - 1U] -= 1U;
            return 0;
        }
        out[index - 1U] = 0xffU;
    }
    return -1;
}

static unsigned int pgpmsg_mpi_bit_length(const unsigned char *data, size_t size) {
    size_t offset = pgpmsg_be_skip_zeroes(data, size);
    unsigned char top;
    unsigned int bits = 0U;

    if (offset == size) return 0U;
    top = data[offset];
    bits = (unsigned int)((size - offset - 1U) * 8U);
    while (top != 0U) {
        bits += 1U;
        top >>= 1U;
    }
    return bits;
}

static int pgpmsg_append_minimal_mpi(PgpMsgBuffer *buffer, const unsigned char *data, size_t size) {
    size_t offset = pgpmsg_be_skip_zeroes(data, size);
    unsigned int bits = pgpmsg_mpi_bit_length(data, size);

    return msg_buffer_append_opaque_mpi(buffer, data + offset, size - offset, bits);
}

static int pgpmsg_parse_elgamal_public_material(
    const PgpPublicKeyInfo *recipient,
    const unsigned char **p_out,
    size_t *p_size_out,
    unsigned int *p_bits_out,
    const unsigned char **g_out,
    size_t *g_size_out,
    const unsigned char **y_out,
    size_t *y_size_out
) {
    const unsigned char *p;
    const unsigned char *g;
    const unsigned char *y;
    size_t p_size;
    size_t g_size;
    size_t y_size;
    unsigned int p_bits;
    unsigned int ignored_bits;
    size_t offset = 0U;

    if (recipient->algorithm != 16U || recipient->public_material_size == 0U) return -1;
    if (read_mpi_view(recipient->public_material, recipient->public_material_size, &offset, &p, &p_size, &p_bits) != 0 ||
        read_mpi_view(recipient->public_material, recipient->public_material_size, &offset, &g, &g_size, &ignored_bits) != 0 ||
        read_mpi_view(recipient->public_material, recipient->public_material_size, &offset, &y, &y_size, &ignored_bits) != 0 ||
        offset != recipient->public_material_size) return -1;
    if (p_size == 0U || g_size == 0U || y_size == 0U || p_size > PGP_PUBLIC_MATERIAL_CAPACITY || g_size > PGP_PUBLIC_MATERIAL_CAPACITY || y_size > PGP_PUBLIC_MATERIAL_CAPACITY) return -1;
    *p_out = p;
    *p_size_out = p_size;
    *p_bits_out = p_bits;
    *g_out = g;
    *g_size_out = g_size;
    *y_out = y;
    *y_size_out = y_size;
    return 0;
}

static int pgpmsg_parse_rsa_public_material(
    const PgpPublicKeyInfo *recipient,
    const unsigned char **modulus_out,
    size_t *modulus_size_out,
    const unsigned char **exponent_out,
    size_t *exponent_size_out
) {
    const unsigned char *modulus;
    const unsigned char *exponent;
    size_t modulus_size;
    size_t exponent_size;
    unsigned int ignored_bits;
    size_t offset = 0U;

    if ((recipient->algorithm != 1U && recipient->algorithm != 2U) || recipient->public_material_size == 0U) return -1;
    if (read_mpi_view(recipient->public_material, recipient->public_material_size, &offset, &modulus, &modulus_size, &ignored_bits) != 0 ||
        read_mpi_view(recipient->public_material, recipient->public_material_size, &offset, &exponent, &exponent_size, &ignored_bits) != 0 ||
        offset != recipient->public_material_size) return -1;
    if (modulus_size == 0U || exponent_size == 0U || modulus_size > CRYPTO_RSA_MAX_MODULUS_SIZE) return -1;
    *modulus_out = modulus;
    *modulus_size_out = modulus_size;
    *exponent_out = exponent;
    *exponent_size_out = exponent_size;
    return 0;
}

static int pgpmsg_random_elgamal_exponent(unsigned char *out, size_t out_size, const unsigned char *p, size_t p_size, unsigned int p_bits) {
    unsigned char p_minus_one[PGP_PUBLIC_MATERIAL_CAPACITY];
    unsigned int excess_bits;
    unsigned int attempt;

    if (out_size == 0U || out_size > PGP_PUBLIC_MATERIAL_CAPACITY || p_size != out_size || p_bits == 0U || p_bits > out_size * 8U) return -1;
    if (pgpmsg_be_decrement_copy(p_minus_one, p, p_size) != 0) return -1;
    excess_bits = (unsigned int)(out_size * 8U - p_bits);
    for (attempt = 0U; attempt < 128U; ++attempt) {
        if (platform_random_bytes(out, out_size) != 0) return -1;
        if (excess_bits != 0U) out[0] &= (unsigned char)(0xffU >> excess_bits);
        if (pgpmsg_be_is_zero(out, out_size) || pgpmsg_be_is_one(out, out_size)) continue;
        if (pgpmsg_be_compare(out, out_size, p_minus_one, p_size) >= 0) continue;
        crypto_secure_bzero(p_minus_one, sizeof(p_minus_one));
        return 0;
    }
    crypto_secure_bzero(p_minus_one, sizeof(p_minus_one));
    return -1;
}

static int pgpmsg_fill_nonzero_random(unsigned char *out, size_t size) {
    size_t index;

    if (platform_random_bytes(out, size) != 0) return -1;
    for (index = 0U; index < size; ++index) {
        while (out[index] == 0U) {
            if (platform_random_bytes(out + index, 1U) != 0) return -1;
        }
    }
    return 0;
}

static int pgpmsg_build_elgamal_message_representative(const unsigned char session_key[PGPMSG_AES256_KEY_SIZE], unsigned char *out, size_t out_size) {
    unsigned int checksum = pgpmsg_session_key_checksum(session_key, PGPMSG_AES256_KEY_SIZE);
    size_t session_info_size = 1U + PGPMSG_AES256_KEY_SIZE + 2U;
    size_t padding_size;
    size_t offset;

    if (out_size < session_info_size + 11U) return -1;
    padding_size = out_size - session_info_size - 3U;
    out[0] = 0U;
    out[1] = 2U;
    if (pgpmsg_fill_nonzero_random(out + 2U, padding_size) != 0) return -1;
    offset = 2U + padding_size;
    out[offset++] = 0U;
    out[offset++] = 9U;
    memcpy(out + offset, session_key, PGPMSG_AES256_KEY_SIZE);
    offset += PGPMSG_AES256_KEY_SIZE;
    out[offset++] = (unsigned char)((checksum >> 8U) & 0xffU);
    out[offset++] = (unsigned char)(checksum & 0xffU);
    return offset == out_size ? 0 : -1;
}

static int build_elgamal_encrypted_session_key(const PgpPublicKeyInfo *recipient, const unsigned char session_key[PGPMSG_AES256_KEY_SIZE], PgpMsgBuffer *pkesk_packet) {
    const unsigned char *p;
    const unsigned char *g;
    const unsigned char *y;
    size_t p_size;
    size_t g_size;
    size_t y_size;
    unsigned int p_bits;
    unsigned char exponent[PGP_PUBLIC_MATERIAL_CAPACITY];
    unsigned char message[PGP_PUBLIC_MATERIAL_CAPACITY];
    unsigned char c1[PGP_PUBLIC_MATERIAL_CAPACITY];
    unsigned char y_to_k[PGP_PUBLIC_MATERIAL_CAPACITY];
    unsigned char c2[PGP_PUBLIC_MATERIAL_CAPACITY];
    PgpMsgBuffer body;
    int result = -1;

    rt_memset(&body, 0, sizeof(body));
    if (recipient->version != 4U || pgpmsg_parse_elgamal_public_material(recipient, &p, &p_size, &p_bits, &g, &g_size, &y, &y_size) != 0) goto cleanup;
    if (pgpmsg_build_elgamal_message_representative(session_key, message, p_size) != 0) goto cleanup;
    if (pgpmsg_random_elgamal_exponent(exponent, p_size, p, p_size, p_bits) != 0) goto cleanup;
    if (crypto_modexp_be(c1, p_size, g, g_size, exponent, p_size, p, p_size) != 0) goto cleanup;
    if (crypto_modexp_be(y_to_k, p_size, y, y_size, exponent, p_size, p, p_size) != 0) goto cleanup;
    if (crypto_mul_mod_be(c2, p_size, y_to_k, p_size, message, p_size, p, p_size) != 0) goto cleanup;
    if (msg_buffer_append_byte(&body, 3U) != 0 ||
        msg_buffer_append_data(&body, recipient->key_id, PGPMSG_KEY_ID_SIZE) != 0 ||
        msg_buffer_append_byte(&body, 16U) != 0 ||
        pgpmsg_append_minimal_mpi(&body, c1, p_size) != 0 ||
        pgpmsg_append_minimal_mpi(&body, c2, p_size) != 0 ||
        msg_buffer_append_packet(pkesk_packet, 1U, &body) != 0) goto cleanup;
    result = 0;

cleanup:
    msg_buffer_free(&body);
    crypto_secure_bzero(exponent, sizeof(exponent));
    crypto_secure_bzero(message, sizeof(message));
    crypto_secure_bzero(c1, sizeof(c1));
    crypto_secure_bzero(y_to_k, sizeof(y_to_k));
    crypto_secure_bzero(c2, sizeof(c2));
    return result;
}

static int build_rsa_encrypted_session_key(const PgpPublicKeyInfo *recipient, const unsigned char session_key[PGPMSG_AES256_KEY_SIZE], PgpMsgBuffer *pkesk_packet) {
    const unsigned char *modulus;
    const unsigned char *exponent;
    size_t modulus_size;
    size_t exponent_size;
    unsigned char session_info[1U + PGPMSG_AES256_KEY_SIZE + 2U];
    unsigned char encrypted[CRYPTO_RSA_MAX_MODULUS_SIZE];
    size_t encrypted_size = 0U;
    unsigned int checksum;
    PgpMsgBuffer body;
    int result = -1;

    rt_memset(&body, 0, sizeof(body));
    if (recipient->version != 4U || pgpmsg_parse_rsa_public_material(recipient, &modulus, &modulus_size, &exponent, &exponent_size) != 0) goto cleanup;
    checksum = pgpmsg_session_key_checksum(session_key, PGPMSG_AES256_KEY_SIZE);
    session_info[0] = 9U;
    memcpy(session_info + 1U, session_key, PGPMSG_AES256_KEY_SIZE);
    session_info[33] = (unsigned char)((checksum >> 8U) & 0xffU);
    session_info[34] = (unsigned char)(checksum & 0xffU);
    if (crypto_rsa_pkcs1_v15_encrypt(encrypted, sizeof(encrypted), &encrypted_size, session_info, sizeof(session_info), modulus, modulus_size, exponent, exponent_size) != 0) goto cleanup;
    if (msg_buffer_append_byte(&body, 3U) != 0 ||
        msg_buffer_append_data(&body, recipient->key_id, PGPMSG_KEY_ID_SIZE) != 0 ||
        msg_buffer_append_byte(&body, recipient->algorithm) != 0 ||
        pgpmsg_append_minimal_mpi(&body, encrypted, encrypted_size) != 0 ||
        msg_buffer_append_packet(pkesk_packet, 1U, &body) != 0) goto cleanup;
    result = 0;

cleanup:
    msg_buffer_free(&body);
    crypto_secure_bzero(session_info, sizeof(session_info));
    crypto_secure_bzero(encrypted, sizeof(encrypted));
    return result;
}

static int build_encrypted_session_key(const PgpPublicKeyInfo *recipient, const unsigned char session_key[PGPMSG_AES256_KEY_SIZE], PgpMsgBuffer *pkesk_packet) {
    unsigned char ephemeral_seed[PGPMSG_X25519_KEY_SIZE];
    unsigned char ephemeral_public[PGPMSG_X25519_KEY_SIZE];
    unsigned char shared[PGPMSG_X25519_KEY_SIZE];
    unsigned char kek[PGPMSG_AES256_KEY_SIZE];
    unsigned char session_plain[40];
    unsigned int checksum;
    unsigned int pad_size;
    PgpMsgBuffer wrapped;
    PgpMsgBuffer body;
    unsigned char point[PGPMSG_X25519_KEY_SIZE + 1U];
    int result = -1;

    rt_memset(&wrapped, 0, sizeof(wrapped));
    rt_memset(&body, 0, sizeof(body));
    if (recipient->version == 6U && recipient->algorithm == 25U) {
        unsigned char kek128[CRYPTO_AES128_KEY_SIZE];

        if (recipient->fingerprint_size != 32U) goto cleanup;
        if (platform_random_bytes(ephemeral_seed, sizeof(ephemeral_seed)) != 0) goto cleanup;
        if (crypto_x25519_scalarmult_base(ephemeral_public, ephemeral_seed) != 0) goto cleanup;
        if (crypto_x25519_scalarmult(shared, ephemeral_seed, recipient->public_material) != 0) goto cleanup;
        if (pgpmsg_derive_x25519_v6_kek(ephemeral_public, recipient->public_material, shared, kek128) != 0) goto cleanup;
        if (pgpmsg_aes128_key_wrap(kek128, session_key, PGPMSG_AES256_KEY_SIZE, &wrapped) != 0) {
            crypto_secure_bzero(kek128, sizeof(kek128));
            goto cleanup;
        }
        if (msg_buffer_append_byte(&body, 6U) != 0 ||
            msg_buffer_append_byte(&body, 1U + recipient->fingerprint_size) != 0 ||
            msg_buffer_append_byte(&body, 6U) != 0 ||
            msg_buffer_append_data(&body, recipient->fingerprint, recipient->fingerprint_size) != 0 ||
            msg_buffer_append_byte(&body, 25U) != 0 ||
            msg_buffer_append_data(&body, ephemeral_public, PGPMSG_X25519_KEY_SIZE) != 0 ||
            msg_buffer_append_byte(&body, (unsigned int)wrapped.size) != 0 ||
            msg_buffer_append_data(&body, wrapped.data, wrapped.size) != 0 ||
            msg_buffer_append_packet(pkesk_packet, 1U, &body) != 0) {
            crypto_secure_bzero(kek128, sizeof(kek128));
            goto cleanup;
        }
        crypto_secure_bzero(kek128, sizeof(kek128));
        result = 0;
        goto cleanup;
    }
    if (recipient->algorithm == 16U) {
        result = build_elgamal_encrypted_session_key(recipient, session_key, pkesk_packet);
        goto cleanup;
    }
    if (recipient->algorithm == 1U || recipient->algorithm == 2U) {
        result = build_rsa_encrypted_session_key(recipient, session_key, pkesk_packet);
        goto cleanup;
    }
    if (recipient->algorithm != 18U) goto cleanup;
    if (platform_random_bytes(ephemeral_seed, sizeof(ephemeral_seed)) != 0) goto cleanup;
    if (crypto_x25519_scalarmult_base(ephemeral_public, ephemeral_seed) != 0) goto cleanup;
    if (crypto_x25519_scalarmult(shared, ephemeral_seed, recipient->public_material) != 0) goto cleanup;
    pgpmsg_derive_ecdh_kek(shared, recipient->fingerprint, recipient->fingerprint_size, kek);
    checksum = pgpmsg_session_key_checksum(session_key, PGPMSG_AES256_KEY_SIZE);
    session_plain[0] = 9U;
    memcpy(session_plain + 1U, session_key, PGPMSG_AES256_KEY_SIZE);
    session_plain[33] = (unsigned char)((checksum >> 8U) & 0xffU);
    session_plain[34] = (unsigned char)(checksum & 0xffU);
    pad_size = (unsigned int)(8U - (35U % 8U));
    rt_memset(session_plain + 35U, (int)pad_size, pad_size);
    if (pgpmsg_aes_key_wrap(kek, session_plain, 35U + pad_size, &wrapped) != 0) goto cleanup;
    point[0] = 0x40U;
    memcpy(point + 1U, ephemeral_public, PGPMSG_X25519_KEY_SIZE);
    if (msg_buffer_append_byte(&body, 3U) != 0 ||
        msg_buffer_append_data(&body, recipient->key_id, PGPMSG_KEY_ID_SIZE) != 0 ||
        msg_buffer_append_byte(&body, 18U) != 0 ||
        msg_buffer_append_opaque_mpi(&body, point, sizeof(point), 263U) != 0 ||
        msg_buffer_append_byte(&body, (unsigned int)wrapped.size) != 0 ||
        msg_buffer_append_data(&body, wrapped.data, wrapped.size) != 0 ||
        msg_buffer_append_packet(pkesk_packet, 1U, &body) != 0) goto cleanup;
    result = 0;

cleanup:
    msg_buffer_free(&wrapped);
    msg_buffer_free(&body);
    crypto_secure_bzero(ephemeral_seed, sizeof(ephemeral_seed));
    crypto_secure_bzero(ephemeral_public, sizeof(ephemeral_public));
    crypto_secure_bzero(shared, sizeof(shared));
    crypto_secure_bzero(kek, sizeof(kek));
    crypto_secure_bzero(session_plain, sizeof(session_plain));
    return result;
}

static int build_literal_packet(const unsigned char *input, size_t input_size, PgpMsgBuffer *literal_packet) {
    PgpMsgBuffer body;
    int result;

    rt_memset(&body, 0, sizeof(body));
    result = msg_buffer_append_byte(&body, 'b') != 0 ||
             msg_buffer_append_byte(&body, 0U) != 0 ||
             msg_buffer_append_u32_be(&body, 0U) != 0 ||
             msg_buffer_append_data(&body, input, input_size) != 0 ||
             msg_buffer_append_packet(literal_packet, 11U, &body) != 0 ? -1 : 0;
    msg_buffer_free(&body);
    return result;
}

static int build_one_pass_signature_packet(const PgpMsgSecretKey *secret, PgpMsgBuffer *one_pass_packet) {
    PgpMsgBuffer body;
    unsigned int signature_version;
    unsigned int public_key_algorithm;
    int result = -1;

    rt_memset(&body, 0, sizeof(body));
    if (secret == 0) goto cleanup;
    signature_version = secret->info.version == 6U ? 6U : 4U;
    public_key_algorithm = signature_version == 6U ? 27U : 22U;
    if (msg_buffer_append_byte(&body, 3U) != 0 ||
        msg_buffer_append_byte(&body, 0x00U) != 0 ||
        msg_buffer_append_byte(&body, 10U) != 0 ||
        msg_buffer_append_byte(&body, public_key_algorithm) != 0 ||
        msg_buffer_append_data(&body, secret->info.key_id, PGPMSG_KEY_ID_SIZE) != 0 ||
        msg_buffer_append_byte(&body, 1U) != 0 ||
        msg_buffer_append_packet(one_pass_packet, 4U, &body) != 0) goto cleanup;
    result = 0;

cleanup:
    msg_buffer_free(&body);
    return result;
}

static int build_compressed_packet(unsigned int compression, const PgpMsgBuffer *literal_packet, PgpMsgBuffer *payload_packet);

static int build_message_payload(const unsigned char *input, size_t input_size, unsigned int compression, const PgpMsgSecretKey *signing_secret, PgpMsgBuffer *payload_packet) {
    PgpMsgBuffer literal_packet;
    PgpMsgBuffer one_pass_packet;
    PgpMsgBuffer signature_packet;
    PgpMsgBuffer signed_packet;
    const PgpMsgBuffer *inner_packet;
    int result = -1;

    rt_memset(&literal_packet, 0, sizeof(literal_packet));
    rt_memset(&one_pass_packet, 0, sizeof(one_pass_packet));
    rt_memset(&signature_packet, 0, sizeof(signature_packet));
    rt_memset(&signed_packet, 0, sizeof(signed_packet));
    if (build_literal_packet(input, input_size, &literal_packet) != 0) goto cleanup;
    inner_packet = &literal_packet;
    if (signing_secret != 0) {
        if (build_one_pass_signature_packet(signing_secret, &one_pass_packet) != 0 ||
            build_signature_packet_for_data(signing_secret, input, input_size, 0x00U, &signature_packet) != 0 ||
            msg_buffer_append_data(&signed_packet, one_pass_packet.data, one_pass_packet.size) != 0 ||
            msg_buffer_append_data(&signed_packet, literal_packet.data, literal_packet.size) != 0 ||
            msg_buffer_append_data(&signed_packet, signature_packet.data, signature_packet.size) != 0) goto cleanup;
        inner_packet = &signed_packet;
    }
    if (build_compressed_packet(compression, inner_packet, payload_packet) != 0) goto cleanup;
    result = 0;

cleanup:
    msg_buffer_free(&literal_packet);
    msg_buffer_free(&one_pass_packet);
    msg_buffer_free(&signature_packet);
    msg_buffer_free(&signed_packet);
    return result;
}

static int build_compressed_packet(unsigned int compression, const PgpMsgBuffer *literal_packet, PgpMsgBuffer *payload_packet) {
    PgpMsgBuffer body;
    unsigned char *compressed = 0;
    size_t compressed_capacity;
    size_t compressed_size = 0U;
    int result = -1;

    if (compression == PGPMSG_COMPRESSION_NONE) return msg_buffer_append_data(payload_packet, literal_packet->data, literal_packet->size);
    if (compression == PGPMSG_COMPRESSION_ZIP) {
        tool_write_error("pgpmsg", "ZIP compression for encryption is not implemented yet; use --compress=zlib", 0);
        return -1;
    }
    if (compression == PGPMSG_COMPRESSION_BZIP2) {
        tool_write_error("pgpmsg", "BZip2 compression is not implemented yet", 0);
        return -1;
    }
    if (compression != PGPMSG_COMPRESSION_ZLIB) return -1;
    compressed_capacity = compression_zlib_fixed_lz77_bound(literal_packet->size);
    compressed = (unsigned char *)rt_malloc(compressed_capacity == 0U ? 1U : compressed_capacity);
    if (compressed == 0) return -1;
    if (compression_zlib_fixed_lz77(literal_packet->data, literal_packet->size, compressed, compressed_capacity, &compressed_size) != 0) goto cleanup;
    rt_memset(&body, 0, sizeof(body));
    if (msg_buffer_append_byte(&body, PGPMSG_COMPRESSION_ZLIB) != 0 ||
        msg_buffer_append_data(&body, compressed, compressed_size) != 0 ||
        msg_buffer_append_packet(payload_packet, PGPMSG_PGP_COMPRESSED, &body) != 0) {
        msg_buffer_free(&body);
        goto cleanup;
    }
    msg_buffer_free(&body);
    result = 0;

cleanup:
    if (compressed != 0) rt_free(compressed);
    return result;
}

static int build_encrypted_data_packet(const unsigned char session_key[PGPMSG_AES256_KEY_SIZE], const unsigned char *input, size_t input_size, unsigned int compression, const PgpMsgSecretKey *signing_secret, PgpMsgBuffer *encrypted_packet) {
    unsigned char prefix[PGPMSG_AES_BLOCK_SIZE + 2U];
    unsigned char digest[CRYPTO_SHA1_DIGEST_SIZE];
    PgpMsgBuffer payload_packet;
    PgpMsgBuffer plain;
    PgpMsgBuffer body;
    unsigned char mdc_header[] = { 0xd3U, 0x14U };
    int result = -1;

    rt_memset(&payload_packet, 0, sizeof(payload_packet));
    rt_memset(&plain, 0, sizeof(plain));
    rt_memset(&body, 0, sizeof(body));
    if (platform_random_bytes(prefix, PGPMSG_AES_BLOCK_SIZE) != 0) goto cleanup;
    prefix[PGPMSG_AES_BLOCK_SIZE] = prefix[PGPMSG_AES_BLOCK_SIZE - 2U];
    prefix[PGPMSG_AES_BLOCK_SIZE + 1U] = prefix[PGPMSG_AES_BLOCK_SIZE - 1U];
    if (build_message_payload(input, input_size, compression, signing_secret, &payload_packet) != 0) goto cleanup;
    if (msg_buffer_append_data(&plain, prefix, sizeof(prefix)) != 0 ||
        msg_buffer_append_data(&plain, payload_packet.data, payload_packet.size) != 0 ||
        msg_buffer_append_data(&plain, mdc_header, sizeof(mdc_header)) != 0) goto cleanup;
    crypto_sha1_hash(plain.data, plain.size, digest);
    if (msg_buffer_append_data(&plain, digest, sizeof(digest)) != 0) goto cleanup;
    if (msg_buffer_append_byte(&body, 1U) != 0) goto cleanup;
    if (msg_buffer_reserve(&body, plain.size) != 0) goto cleanup;
    pgpmsg_aes256_cfb_xcrypt(session_key, plain.data, body.data + body.size, plain.size, 0);
    body.size += plain.size;
    if (msg_buffer_append_packet(encrypted_packet, 18U, &body) != 0) goto cleanup;
    result = 0;

cleanup:
    msg_buffer_free(&payload_packet);
    msg_buffer_free(&plain);
    msg_buffer_free(&body);
    crypto_secure_bzero(prefix, sizeof(prefix));
    crypto_secure_bzero(digest, sizeof(digest));
    return result;
}

static int pgpmsg_derive_v2_seipd_key(const unsigned char session_key[PGPMSG_AES256_KEY_SIZE], const unsigned char salt[PGPMSG_V2_SEIPD_SALT_SIZE], unsigned int chunk_octet, unsigned char message_key[PGPMSG_AES256_KEY_SIZE], unsigned char iv_prefix[4]) {
    unsigned char prk[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned char derived[PGPMSG_AES256_KEY_SIZE + 4U];
    unsigned char info[5];
    int result;

    info[0] = 0xd2U;
    info[1] = 2U;
    info[2] = 9U;
    info[3] = 3U;
    info[4] = (unsigned char)chunk_octet;
    result = crypto_hkdf_sha256_extract(prk, salt, PGPMSG_V2_SEIPD_SALT_SIZE, session_key, PGPMSG_AES256_KEY_SIZE);
    if (result == 0) result = crypto_hkdf_sha256_expand(derived, sizeof(derived), prk, info, sizeof(info));
    if (result == 0) {
        memcpy(message_key, derived, PGPMSG_AES256_KEY_SIZE);
        memcpy(iv_prefix, derived + PGPMSG_AES256_KEY_SIZE, 4U);
    }
    crypto_secure_bzero(prk, sizeof(prk));
    crypto_secure_bzero(derived, sizeof(derived));
    return result;
}

static void pgpmsg_make_v2_seipd_nonce(const unsigned char iv_prefix[4], unsigned long long chunk_index, unsigned char nonce[CRYPTO_AES256_GCM_IV_SIZE]) {
    memcpy(nonce, iv_prefix, 4U);
    nonce[4] = (unsigned char)((chunk_index >> 56U) & 0xffU);
    nonce[5] = (unsigned char)((chunk_index >> 48U) & 0xffU);
    nonce[6] = (unsigned char)((chunk_index >> 40U) & 0xffU);
    nonce[7] = (unsigned char)((chunk_index >> 32U) & 0xffU);
    nonce[8] = (unsigned char)((chunk_index >> 24U) & 0xffU);
    nonce[9] = (unsigned char)((chunk_index >> 16U) & 0xffU);
    nonce[10] = (unsigned char)((chunk_index >> 8U) & 0xffU);
    nonce[11] = (unsigned char)(chunk_index & 0xffU);
}

static void pgpmsg_store_u64_be(unsigned char out[8], unsigned long long value) {
    out[0] = (unsigned char)((value >> 56U) & 0xffU);
    out[1] = (unsigned char)((value >> 48U) & 0xffU);
    out[2] = (unsigned char)((value >> 40U) & 0xffU);
    out[3] = (unsigned char)((value >> 32U) & 0xffU);
    out[4] = (unsigned char)((value >> 24U) & 0xffU);
    out[5] = (unsigned char)((value >> 16U) & 0xffU);
    out[6] = (unsigned char)((value >> 8U) & 0xffU);
    out[7] = (unsigned char)(value & 0xffU);
}

static int build_encrypted_data_packet_v2(const unsigned char session_key[PGPMSG_AES256_KEY_SIZE], const unsigned char *input, size_t input_size, unsigned int compression, const PgpMsgSecretKey *signing_secret, PgpMsgBuffer *encrypted_packet) {
    PgpMsgBuffer payload_packet;
    PgpMsgBuffer body;
    unsigned char salt[PGPMSG_V2_SEIPD_SALT_SIZE];
    unsigned char message_key[PGPMSG_AES256_KEY_SIZE];
    unsigned char iv_prefix[4];
    unsigned char aad[5] = { 0xd2U, 2U, 9U, 3U, PGPMSG_V2_SEIPD_CHUNK_OCTET };
    unsigned char final_aad[13];
    unsigned char nonce[CRYPTO_AES256_GCM_IV_SIZE];
    unsigned char tag[PGPMSG_AES_GCM_TAG_SIZE];
    size_t offset = 0U;
    unsigned long long chunk_index = 0ULL;
    int result = -1;

    rt_memset(&payload_packet, 0, sizeof(payload_packet));
    rt_memset(&body, 0, sizeof(body));
    if (platform_random_bytes(salt, sizeof(salt)) != 0) goto cleanup;
    if (build_message_payload(input, input_size, compression, signing_secret, &payload_packet) != 0) goto cleanup;
    if (pgpmsg_derive_v2_seipd_key(session_key, salt, PGPMSG_V2_SEIPD_CHUNK_OCTET, message_key, iv_prefix) != 0) goto cleanup;
    if (msg_buffer_append_byte(&body, 2U) != 0 ||
        msg_buffer_append_byte(&body, 9U) != 0 ||
        msg_buffer_append_byte(&body, 3U) != 0 ||
        msg_buffer_append_byte(&body, PGPMSG_V2_SEIPD_CHUNK_OCTET) != 0 ||
        msg_buffer_append_data(&body, salt, sizeof(salt)) != 0) goto cleanup;
    while (offset < payload_packet.size) {
        size_t chunk_size = payload_packet.size - offset;
        size_t ciphertext_offset;

        if (chunk_size > PGPMSG_V2_SEIPD_CHUNK_SIZE) chunk_size = PGPMSG_V2_SEIPD_CHUNK_SIZE;
        ciphertext_offset = body.size;
        if (msg_buffer_reserve(&body, chunk_size + sizeof(tag)) != 0) goto cleanup;
        pgpmsg_make_v2_seipd_nonce(iv_prefix, chunk_index, nonce);
        if (crypto_aes256_gcm_encrypt(message_key, nonce, aad, sizeof(aad), payload_packet.data + offset, chunk_size, body.data + ciphertext_offset, tag) != 0) goto cleanup;
        body.size += chunk_size;
        if (msg_buffer_append_data(&body, tag, sizeof(tag)) != 0) goto cleanup;
        offset += chunk_size;
        chunk_index += 1ULL;
    }
    memcpy(final_aad, aad, sizeof(aad));
    pgpmsg_store_u64_be(final_aad + sizeof(aad), (unsigned long long)payload_packet.size);
    pgpmsg_make_v2_seipd_nonce(iv_prefix, chunk_index, nonce);
    if (crypto_aes256_gcm_encrypt(message_key, nonce, final_aad, sizeof(final_aad), (const unsigned char *)"", 0U, tag, tag) != 0) goto cleanup;
    if (msg_buffer_append_data(&body, tag, sizeof(tag)) != 0 || msg_buffer_append_packet(encrypted_packet, 18U, &body) != 0) goto cleanup;
    result = 0;

cleanup:
    msg_buffer_free(&payload_packet);
    msg_buffer_free(&body);
    crypto_secure_bzero(salt, sizeof(salt));
    crypto_secure_bzero(message_key, sizeof(message_key));
    crypto_secure_bzero(iv_prefix, sizeof(iv_prefix));
    crypto_secure_bzero(nonce, sizeof(nonce));
    crypto_secure_bzero(tag, sizeof(tag));
    return result;
}

static int pgpmsg_stream_feed_packet_length(PgpMsgSeipdStream *stream, size_t length) {
    PgpMsgBuffer encoded;
    int result;

    rt_memset(&encoded, 0, sizeof(encoded));
    result = msg_buffer_append_packet_length(&encoded, length) == 0 ? pgpmsg_seipd_stream_write(stream, encoded.data, encoded.size, 1) : -1;
    msg_buffer_free(&encoded);
    return result;
}

static int pgpmsg_stream_feed_partial_length(PgpMsgSeipdStream *stream, size_t length) {
    unsigned int exponent = 0U;
    size_t value = length;
    unsigned char encoded;

    if (length < 512U || length > 0x40000000ULL) return -1;
    while (value > 1U) {
        if ((value & 1U) != 0U) return -1;
        value >>= 1U;
        exponent += 1U;
    }
    encoded = (unsigned char)(224U + exponent);
    return pgpmsg_seipd_stream_write(stream, &encoded, 1U, 1);
}

static int pgpmsg_fill_literal_chunk(int input_fd, int *first_io, unsigned char *buffer, size_t capacity, size_t *size_out, int *eof_out) {
    size_t used = 0U;

    *eof_out = 0;
    if (*first_io) {
        if (capacity < 6U) return -1;
        buffer[used++] = 'b';
        buffer[used++] = 0U;
        buffer[used++] = 0U;
        buffer[used++] = 0U;
        buffer[used++] = 0U;
        buffer[used++] = 0U;
        *first_io = 0;
    }
    while (used < capacity) {
        long bytes = platform_read(input_fd, buffer + used, capacity - used);

        if (bytes < 0) return -1;
        if (bytes == 0) {
            *eof_out = 1;
            break;
        }
        used += (size_t)bytes;
    }
    *size_out = used;
    return 0;
}

static int write_stream_encrypted_data_packet(int output_fd, int input_fd, const unsigned char session_key[PGPMSG_AES256_KEY_SIZE]) {
    PgpMsgSeipdStream stream;
    unsigned char prefix[PGPMSG_AES_BLOCK_SIZE + 2U];
    unsigned char literal_header = 0xc0U | PGPMSG_PGP_LITERAL_BINARY;
    unsigned char seipd_version = 1U;
    unsigned char mdc_header[] = { 0xd3U, 0x14U };
    unsigned char digest[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char chunk[PGPMSG_STREAM_CHUNK_SIZE];
    int first_literal = 1;
    int result = -1;

    rt_memset(&stream, 0, sizeof(stream));
    if (platform_random_bytes(prefix, PGPMSG_AES_BLOCK_SIZE) != 0) return -1;
    prefix[PGPMSG_AES_BLOCK_SIZE] = prefix[PGPMSG_AES_BLOCK_SIZE - 2U];
    prefix[PGPMSG_AES_BLOCK_SIZE + 1U] = prefix[PGPMSG_AES_BLOCK_SIZE - 1U];
    stream.writer.fd = output_fd;
    pgpmsg_cfb_stream_init(&stream.cfb, session_key);
    crypto_sha1_init(&stream.sha1);
    if (pgp_write_new_packet_header(output_fd, 18U) != 0) goto cleanup;
    if (pgpmsg_partial_writer_write(&stream.writer, &seipd_version, 1U) != 0 ||
        pgpmsg_seipd_stream_write(&stream, prefix, sizeof(prefix), 1) != 0 ||
        pgpmsg_seipd_stream_write(&stream, &literal_header, 1U, 1) != 0) goto cleanup;
    while (1) {
        size_t chunk_size = 0U;
        int eof = 0;

        if (pgpmsg_fill_literal_chunk(input_fd, &first_literal, chunk, sizeof(chunk), &chunk_size, &eof) != 0) goto cleanup;
        if (chunk_size == sizeof(chunk) && !eof) {
            if (pgpmsg_stream_feed_partial_length(&stream, chunk_size) != 0 || pgpmsg_seipd_stream_write(&stream, chunk, chunk_size, 1) != 0) goto cleanup;
        } else {
            if (pgpmsg_stream_feed_packet_length(&stream, chunk_size) != 0 || (chunk_size != 0U && pgpmsg_seipd_stream_write(&stream, chunk, chunk_size, 1) != 0)) goto cleanup;
            break;
        }
    }
    if (pgpmsg_seipd_stream_write(&stream, mdc_header, sizeof(mdc_header), 1) != 0) goto cleanup;
    crypto_sha1_final(&stream.sha1, digest);
    if (pgpmsg_seipd_stream_write(&stream, digest, sizeof(digest), 0) != 0 ||
        pgpmsg_cfb_stream_finish(&stream.cfb, &stream.writer) != 0 ||
        pgpmsg_partial_writer_finish(&stream.writer) != 0) goto cleanup;
    result = 0;

cleanup:
    crypto_secure_bzero(&stream, sizeof(stream));
    crypto_secure_bzero(prefix, sizeof(prefix));
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(chunk, sizeof(chunk));
    return result;
}

static int command_encrypt_stream(const PgpMsgOptions *local_options, const PgpPublicKeyInfo *recipients, const unsigned char session_key[PGPMSG_AES256_KEY_SIZE], const char *input_path) {
    int input_fd = input_path != 0 ? platform_open_read(input_path) : 0;
    int output_fd = local_options->output_path != 0 ? platform_open_write(local_options->output_path, 0644U) : 1;
    size_t recipient_index;
    int result = -1;

    if (input_fd < 0 || output_fd < 0) goto cleanup;
    for (recipient_index = 0U; recipient_index < local_options->recipient_count; ++recipient_index) {
        PgpMsgBuffer pkesk_packet;

        rt_memset(&pkesk_packet, 0, sizeof(pkesk_packet));
        if (build_encrypted_session_key(&recipients[recipient_index], session_key, &pkesk_packet) != 0 || rt_write_all(output_fd, pkesk_packet.data, pkesk_packet.size) != 0) {
            msg_buffer_free(&pkesk_packet);
            goto cleanup;
        }
        msg_buffer_free(&pkesk_packet);
    }
    if (write_stream_encrypted_data_packet(output_fd, input_fd, session_key) != 0) goto cleanup;
    result = 0;

cleanup:
    if (input_fd > 2 && platform_close(input_fd) != 0) result = -1;
    if (output_fd > 2 && platform_close(output_fd) != 0) result = -1;
    return result;
}

static int unwrap_session_key(const PgpMsgPkesk *pkesk, const PgpMsgX25519SecretKey *secret, unsigned char session_key[PGPMSG_AES256_KEY_SIZE]) {
    unsigned char shared[PGPMSG_X25519_KEY_SIZE];
    unsigned char kek[PGPMSG_AES256_KEY_SIZE];
    unsigned char kek128[CRYPTO_AES128_KEY_SIZE];
    unsigned char padded[56];
    size_t padded_size = 0U;
    size_t message_size;
    unsigned int pad_size;
    unsigned int checksum;
    unsigned int stored_checksum;
    int result = -1;

    if (crypto_x25519_scalarmult(shared, secret->seed, pkesk->ephemeral_public) != 0) goto cleanup;
    if (pkesk->version == 6U && secret->info.version == 6U && secret->info.algorithm == 25U) {
        if (pgpmsg_derive_x25519_v6_kek(pkesk->ephemeral_public, secret->info.public_material, shared, kek128) != 0) goto cleanup;
        if (pgpmsg_aes128_key_unwrap(kek128, pkesk->wrapped_session_key, pkesk->wrapped_session_key_size, padded, &padded_size) != 0) goto cleanup;
        if (padded_size != PGPMSG_AES256_KEY_SIZE) goto cleanup;
        memcpy(session_key, padded, PGPMSG_AES256_KEY_SIZE);
        result = 0;
        goto cleanup;
    }
    pgpmsg_derive_ecdh_kek(shared, secret->info.fingerprint, secret->info.fingerprint_size, kek);
    if (pgpmsg_aes_key_unwrap(kek, pkesk->wrapped_session_key, pkesk->wrapped_session_key_size, padded, &padded_size) != 0) goto cleanup;
    if (padded_size < 8U) goto cleanup;
    pad_size = padded[padded_size - 1U];
    if (pad_size == 0U || pad_size > 8U || pad_size > padded_size) goto cleanup;
    for (message_size = padded_size - pad_size; message_size < padded_size; ++message_size) {
        if (padded[message_size] != pad_size) goto cleanup;
    }
    message_size = padded_size - pad_size;
    if (message_size != 35U || padded[0] != 9U) goto cleanup;
    memcpy(session_key, padded + 1U, PGPMSG_AES256_KEY_SIZE);
    checksum = pgpmsg_session_key_checksum(session_key, PGPMSG_AES256_KEY_SIZE);
    stored_checksum = ((unsigned int)padded[33] << 8U) | (unsigned int)padded[34];
    if (checksum != stored_checksum) goto cleanup;
    result = 0;

cleanup:
    crypto_secure_bzero(shared, sizeof(shared));
    crypto_secure_bzero(kek, sizeof(kek));
    crypto_secure_bzero(kek128, sizeof(kek128));
    crypto_secure_bzero(padded, sizeof(padded));
    return result;
}

static int literal_packet_data_view(const unsigned char *payload, size_t payload_size, const PgpPacket *packet, const unsigned char **data_out, size_t *data_size_out) {
    const unsigned char *body;
    size_t body_size;
    size_t filename_size;
    size_t data_offset;

    if (packet == 0 || packet->tag != PGPMSG_PGP_LITERAL_BINARY || packet->body_offset > payload_size || packet->body_size > payload_size - packet->body_offset) return -1;
    body = payload + packet->body_offset;
    body_size = packet->body_size;
    if (body_size < 6U) return -1;
    filename_size = body[1];
    data_offset = 2U + filename_size + 4U;
    if (data_offset > body_size) return -1;
    *data_out = body + data_offset;
    *data_size_out = body_size - data_offset;
    return 0;
}

static int write_literal_packet_data(const char *output_path, const unsigned char *payload, size_t payload_size, const PgpPacket *packet) {
    const unsigned char *data;
    size_t data_size;
    int fd;
    int result;

    if (literal_packet_data_view(payload, payload_size, packet, &data, &data_size) != 0) return -1;
    fd = output_path != 0 ? platform_open_write(output_path, 0644U) : 1;
    if (fd < 0) return -1;
    result = rt_write_all(fd, data, data_size) == 0 ? 0 : -1;
    if (fd > 2 && platform_close(fd) != 0) result = -1;
    return result;
}

static int write_decrypted_literal(const char *output_path, const unsigned char *plain, size_t plain_size) {
    PgpPacketReader reader;
    PgpPacket packet;
    int has_packet;
    char error[PGPMSG_ERROR_CAPACITY];

    pgp_packet_reader_init(&reader, plain, plain_size);
    if (pgp_packet_reader_next(&reader, &packet, &has_packet, error, sizeof(error)) != 0 || !has_packet || packet.tag != 11U) {
        tool_write_error("pgpmsg", "decrypted message does not contain a literal data packet", 0);
        return -1;
    }
    return write_literal_packet_data(output_path, plain, plain_size, &packet);
}

static int verify_embedded_signature_packet(const char *pubring, const unsigned char *payload, size_t payload_size, const PgpPacket *literal_packet, const PgpPacket *signature_packet) {
    PgpMsgSignaturePacket signature;
    PgpPublicKeyInfo key;
    const unsigned char *literal_data;
    size_t literal_data_size;
    int verified;

    if (pubring == 0) {
        rt_write_line(2, "pgpmsg: embedded signature not verified; use -k PUBRING");
        return 0;
    }
    if (literal_packet_data_view(payload, payload_size, literal_packet, &literal_data, &literal_data_size) != 0 ||
        signature_packet == 0 || signature_packet->tag != 2U || signature_packet->body_offset > payload_size || signature_packet->body_size > payload_size - signature_packet->body_offset ||
        parse_signature_packet_full(&signature, payload + signature_packet->body_offset, signature_packet->body_size) != 0 || !signature.summary.present) {
        return -1;
    }
    if (load_public_key_for_signature(pubring, &signature.summary, &key) != 0) return -1;
    verified = verify_detached_signature_packet(&signature, payload + signature_packet->body_offset, literal_data, literal_data_size, &key);
    if (verified == 1) {
        rt_write_line(2, "pgpmsg: embedded signature: good");
        return 0;
    }
    if (verified == 0) rt_write_line(2, "pgpmsg: embedded signature: bad");
    return -1;
}

static int inflate_compressed_payload(unsigned int compression, const unsigned char *input, size_t input_size, unsigned char **output_out, size_t *output_size_out) {
    size_t capacity = input_size > 1024U ? input_size * 3U : 4096U;
    unsigned char *output = 0;

    if (compression == PGPMSG_COMPRESSION_BZIP2) {
        tool_write_error("pgpmsg", "BZip2 compressed OpenPGP data is not implemented yet", 0);
        return -1;
    }
    if (compression != PGPMSG_COMPRESSION_ZIP && compression != PGPMSG_COMPRESSION_ZLIB) return -1;
    if (capacity < input_size || capacity > PGPMSG_MAX_INFLATED_SIZE) capacity = PGPMSG_MAX_INFLATED_SIZE;
    while (capacity <= PGPMSG_MAX_INFLATED_SIZE) {
        size_t written = 0U;
        int ok;

        output = (unsigned char *)rt_malloc(capacity == 0U ? 1U : capacity);
        if (output == 0) return -1;
        ok = compression == PGPMSG_COMPRESSION_ZIP ?
             compression_deflate_inflate_raw(input, input_size, output, capacity, &written) :
             compression_zlib_inflate(input, input_size, output, capacity, &written);
        if (ok == 0) {
            *output_out = output;
            *output_size_out = written;
            return 0;
        }
        rt_free(output);
        output = 0;
        if (capacity == PGPMSG_MAX_INFLATED_SIZE) break;
        if (capacity > PGPMSG_MAX_INFLATED_SIZE / 2U) capacity = PGPMSG_MAX_INFLATED_SIZE;
        else capacity *= 2U;
    }
    return -1;
}

static int write_decrypted_payload(const char *output_path, const char *pubring, const unsigned char *plain, size_t plain_size) {
    unsigned char *normalized = 0;
    size_t normalized_size = 0U;
    const unsigned char *payload = plain;
    size_t payload_size = plain_size;
    PgpPacketReader reader;
    PgpPacket packet;
    int has_packet;
    char error[PGPMSG_ERROR_CAPACITY];

    if (pgp_normalize_packets(plain, plain_size, &normalized, &normalized_size, error, sizeof(error)) == 0) {
        payload = normalized;
        payload_size = normalized_size;
    }
    pgp_packet_reader_init(&reader, payload, payload_size);
    if (pgp_packet_reader_next(&reader, &packet, &has_packet, error, sizeof(error)) != 0 || !has_packet) {
        if (normalized != 0) rt_free(normalized);
        tool_write_error("pgpmsg", "decrypted message does not contain an OpenPGP data packet", 0);
        return -1;
    }
    if (packet.tag == PGPMSG_PGP_LITERAL_BINARY) {
        int result = write_decrypted_literal(output_path, payload, payload_size);

        if (normalized != 0) rt_free(normalized);
        return result;
    }
    if (packet.tag == 4U) {
        PgpPacket literal;
        PgpPacket signature;
        if (pgp_packet_reader_next(&reader, &literal, &has_packet, error, sizeof(error)) != 0 || !has_packet || literal.tag != PGPMSG_PGP_LITERAL_BINARY) {
            if (normalized != 0) rt_free(normalized);
            tool_write_error("pgpmsg", "signed decrypted message does not contain a literal data packet", 0);
            return -1;
        }
        if (pgp_packet_reader_next(&reader, &signature, &has_packet, error, sizeof(error)) != 0 || !has_packet || signature.tag != 2U) {
            if (normalized != 0) rt_free(normalized);
            tool_write_error("pgpmsg", "signed decrypted message does not contain a signature packet", 0);
            return -1;
        }
        if (verify_embedded_signature_packet(pubring, payload, payload_size, &literal, &signature) != 0) {
            if (normalized != 0) rt_free(normalized);
            tool_write_error("pgpmsg", "embedded signature verification failed", 0);
            return -1;
        }
        {
            int result = write_literal_packet_data(output_path, payload, payload_size, &literal);
            if (normalized != 0) rt_free(normalized);
            return result;
        }
    }
    if (packet.tag == PGPMSG_PGP_COMPRESSED) {
        const unsigned char *body = payload + packet.body_offset;
        unsigned char *inflated = 0;
        size_t inflated_size = 0U;
        int result;

        if (packet.body_size < 1U) { if (normalized != 0) rt_free(normalized); return -1; }
        if (inflate_compressed_payload(body[0], body + 1U, packet.body_size - 1U, &inflated, &inflated_size) != 0) { if (normalized != 0) rt_free(normalized); return -1; }
        result = write_decrypted_payload(output_path, pubring, inflated, inflated_size);
        rt_free(inflated);
        if (normalized != 0) rt_free(normalized);
        return result;
    }
    if (normalized != 0) rt_free(normalized);
    tool_write_error("pgpmsg", "decrypted message contains unsupported data packet", pgp_packet_tag_name(packet.tag));
    return -1;
}

static int decrypt_encrypted_data_packet(const unsigned char session_key[PGPMSG_AES256_KEY_SIZE], const unsigned char *body, size_t body_size, const char *output_path, const char *pubring) {
    unsigned char *plain;
    unsigned char digest[CRYPTO_SHA1_DIGEST_SIZE];
    size_t plain_size;
    size_t mdc_offset;
    int result = -1;

    if (body_size < 1U || body[0] != 1U) return -1;
    plain_size = body_size - 1U;
    if (plain_size < PGPMSG_AES_BLOCK_SIZE + 2U + 2U + PGPMSG_MDC_SIZE) return -1;
    plain = (unsigned char *)rt_malloc(plain_size);
    if (plain == 0) return -1;
    pgpmsg_aes256_cfb_xcrypt(session_key, body + 1U, plain, plain_size, 1);
    mdc_offset = plain_size - (2U + PGPMSG_MDC_SIZE);
    if (plain[mdc_offset] != 0xd3U || plain[mdc_offset + 1U] != 0x14U) goto cleanup;
    crypto_sha1_hash(plain, mdc_offset + 2U, digest);
    if (!crypto_constant_time_equal(digest, plain + mdc_offset + 2U, PGPMSG_MDC_SIZE)) goto cleanup;
    if (write_decrypted_payload(output_path, pubring, plain + PGPMSG_AES_BLOCK_SIZE + 2U, mdc_offset - (PGPMSG_AES_BLOCK_SIZE + 2U)) != 0) goto cleanup;
    result = 0;

cleanup:
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(plain, plain_size);
    rt_free(plain);
    return result;
}

static int decrypt_encrypted_data_packet_v2(const unsigned char session_key[PGPMSG_AES256_KEY_SIZE], const unsigned char *body, size_t body_size, const char *output_path, const char *pubring) {
    unsigned char message_key[PGPMSG_AES256_KEY_SIZE];
    unsigned char iv_prefix[4];
    unsigned char aad[5];
    unsigned char final_aad[13];
    unsigned char nonce[CRYPTO_AES256_GCM_IV_SIZE];
    unsigned char empty_out[1];
    unsigned char *plain = 0;
    size_t offset;
    size_t encrypted_size;
    size_t chunked_size;
    size_t plain_capacity;
    size_t plain_size = 0U;
    size_t chunk_size;
    unsigned int chunk_octet;
    unsigned long long chunk_index = 0ULL;
    int result = -1;

    if (body_size < 4U + PGPMSG_V2_SEIPD_SALT_SIZE + PGPMSG_AES_GCM_TAG_SIZE) return -1;
    if (body[0] != 2U || body[1] != 9U || body[2] != 3U) return -1;
    chunk_octet = body[3];
    if (chunk_octet > 16U) return -1;
    chunk_size = (size_t)1U << (chunk_octet + 6U);
    aad[0] = 0xd2U;
    aad[1] = 2U;
    aad[2] = 9U;
    aad[3] = 3U;
    aad[4] = (unsigned char)chunk_octet;
    if (pgpmsg_derive_v2_seipd_key(session_key, body + 4U, chunk_octet, message_key, iv_prefix) != 0) return -1;
    offset = 4U + PGPMSG_V2_SEIPD_SALT_SIZE;
    encrypted_size = body_size - offset;
    if (encrypted_size < PGPMSG_AES_GCM_TAG_SIZE) goto cleanup;
    chunked_size = encrypted_size - PGPMSG_AES_GCM_TAG_SIZE;
    plain_capacity = chunked_size >= PGPMSG_AES_GCM_TAG_SIZE ? chunked_size - PGPMSG_AES_GCM_TAG_SIZE : 0U;
    plain = (unsigned char *)rt_malloc(plain_capacity == 0U ? 1U : plain_capacity);
    if (plain == 0) goto cleanup;
    while (chunked_size != 0U) {
        size_t one_chunk;
        const unsigned char *ciphertext;
        const unsigned char *tag;

        if (chunked_size <= PGPMSG_AES_GCM_TAG_SIZE) goto cleanup;
        one_chunk = chunked_size > chunk_size + PGPMSG_AES_GCM_TAG_SIZE ? chunk_size : chunked_size - PGPMSG_AES_GCM_TAG_SIZE;
        ciphertext = body + offset;
        tag = ciphertext + one_chunk;
        pgpmsg_make_v2_seipd_nonce(iv_prefix, chunk_index, nonce);
        if (crypto_aes256_gcm_decrypt(message_key, nonce, aad, sizeof(aad), ciphertext, one_chunk, tag, plain + plain_size) != 0) goto cleanup;
        plain_size += one_chunk;
        offset += one_chunk + PGPMSG_AES_GCM_TAG_SIZE;
        chunked_size -= one_chunk + PGPMSG_AES_GCM_TAG_SIZE;
        chunk_index += 1ULL;
    }
    memcpy(final_aad, aad, sizeof(aad));
    pgpmsg_store_u64_be(final_aad + sizeof(aad), (unsigned long long)plain_size);
    pgpmsg_make_v2_seipd_nonce(iv_prefix, chunk_index, nonce);
    if (crypto_aes256_gcm_decrypt(message_key, nonce, final_aad, sizeof(final_aad), body + offset, 0U, body + offset, empty_out) != 0) goto cleanup;
    if (write_decrypted_payload(output_path, pubring, plain, plain_size) != 0) goto cleanup;
    result = 0;

cleanup:
    if (plain != 0) {
        crypto_secure_bzero(plain, plain_capacity == 0U ? 1U : plain_capacity);
        rt_free(plain);
    }
    crypto_secure_bzero(message_key, sizeof(message_key));
    crypto_secure_bzero(iv_prefix, sizeof(iv_prefix));
    crypto_secure_bzero(nonce, sizeof(nonce));
    return result;
}

static int inspect_write_pkesk_text(const unsigned char *body, size_t body_size) {
    size_t offset = 0U;
    unsigned int version;
    unsigned int algorithm;
    unsigned int mpi_index = 0U;

    if (body_size < 1U) return 0;
    version = body[offset++];
    if (rt_write_cstr(1, "  pkesk-version: ") != 0 || rt_write_uint(1, version) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (version == 6U) {
        unsigned int fingerprint_field_size;
        unsigned int key_version;
        unsigned int wrapped_size;

        if (offset >= body_size) return 0;
        fingerprint_field_size = body[offset++];
        if (fingerprint_field_size == 0U || fingerprint_field_size > body_size - offset) return 0;
        key_version = body[offset++];
        if (fingerprint_field_size - 1U > body_size - offset) return 0;
        if (rt_write_cstr(1, "  recipient-key-version: ") != 0 || rt_write_uint(1, key_version) != 0 || rt_write_char(1, '\n') != 0) return -1;
        if (rt_write_cstr(1, "  recipient-fingerprint: ") != 0 || tool_write_hex_bytes(1, body + offset, (size_t)fingerprint_field_size - 1U) != 0 || rt_write_char(1, '\n') != 0) return -1;
        offset += (size_t)fingerprint_field_size - 1U;
        if (offset >= body_size) return 0;
        algorithm = body[offset++];
        if (rt_write_cstr(1, "  public-key-algorithm: ") != 0 || rt_write_line(1, pgp_public_key_algorithm_name(algorithm)) != 0) return -1;
        if (algorithm == 25U && PGPMSG_X25519_KEY_SIZE <= body_size - offset) {
            if (rt_write_cstr(1, "  ephemeral-public-key: ") != 0 || rt_write_uint(1, PGPMSG_X25519_KEY_SIZE) != 0 || rt_write_line(1, " bytes") != 0) return -1;
            offset += PGPMSG_X25519_KEY_SIZE;
        }
        if (offset < body_size) {
            wrapped_size = body[offset++];
            if (rt_write_cstr(1, "  wrapped-session-key: ") != 0 || rt_write_uint(1, wrapped_size) != 0 || rt_write_line(1, " bytes") != 0) return -1;
        }
        return 0;
    }
    if (version != 3U || body_size - offset < PGPMSG_KEY_ID_SIZE + 1U) return 0;
    if (rt_write_cstr(1, "  recipient-key-id: ") != 0 || tool_write_hex_bytes(1, body + offset, PGPMSG_KEY_ID_SIZE) != 0 || rt_write_char(1, '\n') != 0) return -1;
    offset += PGPMSG_KEY_ID_SIZE;
    algorithm = body[offset++];
    if (rt_write_cstr(1, "  public-key-algorithm: ") != 0 || rt_write_line(1, pgp_public_key_algorithm_name(algorithm)) != 0) return -1;
    while (offset < body_size) {
        const unsigned char *mpi = 0;
        size_t mpi_size = 0U;
        unsigned int mpi_bits = 0U;

        if (read_mpi_view(body, body_size, &offset, &mpi, &mpi_size, &mpi_bits) != 0) break;
        mpi_index += 1U;
        if (rt_write_cstr(1, "  encrypted-mpi ") != 0 || rt_write_uint(1, mpi_index) != 0 || rt_write_cstr(1, ": ") != 0 || rt_write_uint(1, mpi_bits) != 0 || rt_write_cstr(1, " bits, ") != 0 || rt_write_uint(1, (unsigned long long)mpi_size) != 0 || rt_write_line(1, " bytes") != 0) return -1;
        (void)mpi;
    }
    return 0;
}

static int inspect_write_seipd_text(const unsigned char *body, size_t body_size) {
    unsigned int version;

    if (body_size < 1U) return 0;
    version = body[0];
    if (rt_write_cstr(1, "  seipd-version: ") != 0 || rt_write_uint(1, version) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (version == 2U && body_size >= 4U + PGPMSG_V2_SEIPD_SALT_SIZE) {
        if (rt_write_cstr(1, "  symmetric-algorithm: ") != 0 || rt_write_line(1, pgpmsg_symmetric_algorithm_name(body[1])) != 0) return -1;
        if (rt_write_cstr(1, "  aead-algorithm: ") != 0 || rt_write_line(1, pgpmsg_aead_algorithm_name(body[2])) != 0) return -1;
        if (rt_write_cstr(1, "  chunk-size-octet: ") != 0 || rt_write_uint(1, body[3]) != 0 || rt_write_char(1, '\n') != 0) return -1;
        if (rt_write_cstr(1, "  salt: ") != 0 || rt_write_uint(1, PGPMSG_V2_SEIPD_SALT_SIZE) != 0 || rt_write_line(1, " bytes") != 0) return -1;
    }
    return 0;
}

static int inspect_write_packet_text_details(const PgpPacket *packet, const unsigned char *body) {
    if (packet->tag == 1U) return inspect_write_pkesk_text(body, packet->body_size);
    if (packet->tag == 18U) return inspect_write_seipd_text(body, packet->body_size);
    return 0;
}

static int inspect_write_pkesk_json(const unsigned char *body, size_t body_size) {
    size_t offset = 0U;
    unsigned int version;
    unsigned int algorithm;
    unsigned int mpi_index = 0U;

    if (body_size < 1U) return 0;
    version = body[offset++];
    if (rt_write_cstr(1, ",\"pkesk_version\":") != 0 || rt_write_uint(1, version) != 0) return -1;
    if (version == 6U) {
        unsigned int fingerprint_field_size;
        unsigned int key_version;
        unsigned int wrapped_size;

        if (offset >= body_size) return 0;
        fingerprint_field_size = body[offset++];
        if (fingerprint_field_size == 0U || fingerprint_field_size > body_size - offset) return 0;
        key_version = body[offset++];
        if (fingerprint_field_size - 1U > body_size - offset) return 0;
        if (rt_write_cstr(1, ",\"recipient_key_version\":") != 0 || rt_write_uint(1, key_version) != 0) return -1;
        if (rt_write_cstr(1, ",\"recipient_fingerprint\":\"") != 0 || tool_write_hex_bytes(1, body + offset, (size_t)fingerprint_field_size - 1U) != 0 || rt_write_char(1, '"') != 0) return -1;
        offset += (size_t)fingerprint_field_size - 1U;
        if (offset >= body_size) return 0;
        algorithm = body[offset++];
        if (rt_write_cstr(1, ",\"public_key_algorithm\":") != 0 || tool_json_write_string(1, pgp_public_key_algorithm_name(algorithm)) != 0) return -1;
        if (algorithm == 25U && PGPMSG_X25519_KEY_SIZE <= body_size - offset) {
            if (rt_write_cstr(1, ",\"ephemeral_public_key_size\":") != 0 || rt_write_uint(1, PGPMSG_X25519_KEY_SIZE) != 0) return -1;
            offset += PGPMSG_X25519_KEY_SIZE;
        }
        if (offset < body_size) {
            wrapped_size = body[offset++];
            if (rt_write_cstr(1, ",\"wrapped_session_key_size\":") != 0 || rt_write_uint(1, wrapped_size) != 0) return -1;
        }
        return 0;
    }
    if (version != 3U || body_size - offset < PGPMSG_KEY_ID_SIZE + 1U) return 0;
    if (rt_write_cstr(1, ",\"recipient_key_id\":\"") != 0 || tool_write_hex_bytes(1, body + offset, PGPMSG_KEY_ID_SIZE) != 0 || rt_write_char(1, '"') != 0) return -1;
    offset += PGPMSG_KEY_ID_SIZE;
    algorithm = body[offset++];
    if (rt_write_cstr(1, ",\"public_key_algorithm\":") != 0 || tool_json_write_string(1, pgp_public_key_algorithm_name(algorithm)) != 0) return -1;
    if (rt_write_cstr(1, ",\"encrypted_mpi_bits\":[") != 0) return -1;
    while (offset < body_size) {
        const unsigned char *mpi = 0;
        size_t mpi_size = 0U;
        unsigned int mpi_bits = 0U;

        if (read_mpi_view(body, body_size, &offset, &mpi, &mpi_size, &mpi_bits) != 0) break;
        if (mpi_index != 0U && rt_write_char(1, ',') != 0) return -1;
        if (rt_write_uint(1, mpi_bits) != 0) return -1;
        mpi_index += 1U;
        (void)mpi;
        (void)mpi_size;
    }
    return rt_write_char(1, ']');
}

static int inspect_write_seipd_json(const unsigned char *body, size_t body_size) {
    unsigned int version;

    if (body_size < 1U) return 0;
    version = body[0];
    if (rt_write_cstr(1, ",\"seipd_version\":") != 0 || rt_write_uint(1, version) != 0) return -1;
    if (version == 2U && body_size >= 4U + PGPMSG_V2_SEIPD_SALT_SIZE) {
        if (rt_write_cstr(1, ",\"symmetric_algorithm\":") != 0 || tool_json_write_string(1, pgpmsg_symmetric_algorithm_name(body[1])) != 0) return -1;
        if (rt_write_cstr(1, ",\"aead_algorithm\":") != 0 || tool_json_write_string(1, pgpmsg_aead_algorithm_name(body[2])) != 0) return -1;
        if (rt_write_cstr(1, ",\"chunk_size_octet\":") != 0 || rt_write_uint(1, body[3]) != 0) return -1;
        if (rt_write_cstr(1, ",\"salt_size\":") != 0 || rt_write_uint(1, PGPMSG_V2_SEIPD_SALT_SIZE) != 0) return -1;
    }
    return 0;
}

static int inspect_write_packet_json_details(const PgpPacket *packet, const unsigned char *body) {
    if (packet->tag == 1U) return inspect_write_pkesk_json(body, packet->body_size);
    if (packet->tag == 18U) return inspect_write_seipd_json(body, packet->body_size);
    return 0;
}

static const char *inspect_packet_length_encoding(const unsigned char *data, const PgpPacket *packet) {
    unsigned int header = data[packet->header_offset];

    if (packet->new_format) {
        unsigned int first = data[packet->header_offset + 1U];

        if (first < 192U) return "new one-octet definite";
        if (first < 224U) return "new two-octet definite";
        if (first == 255U) return "new five-octet definite";
        return "new partial";
    }
    switch (header & 0x03U) {
        case 0U: return "old one-octet definite";
        case 1U: return "old two-octet definite";
        case 2U: return "old four-octet definite";
        default: return "old indeterminate";
    }
}

static int inspect_write_input_text_details(const PgpMsgInspectInputInfo *info) {
    if (rt_write_cstr(1, "input: ") != 0 || rt_write_line(1, info->armored ? "ASCII armor" : "binary OpenPGP") != 0) return -1;
    if (rt_write_cstr(1, "  input-size: ") != 0 || rt_write_uint(1, (unsigned long long)info->raw_size) != 0 || rt_write_line(1, " bytes") != 0) return -1;
    if (info->armored) {
        if (rt_write_cstr(1, "  armor-type: PGP ") != 0 || rt_write_line(1, info->armor_kind) != 0) return -1;
        if (rt_write_cstr(1, "  armor-body-lines: ") != 0 || rt_write_uint(1, info->armor_body_lines) != 0 || rt_write_char(1, '\n') != 0) return -1;
        if (rt_write_cstr(1, "  armor-base64-chars: ") != 0 || rt_write_uint(1, info->armor_base64_chars) != 0 || rt_write_char(1, '\n') != 0) return -1;
        if (info->has_armor_crc24) {
            if (rt_write_cstr(1, "  armor-crc24: ") != 0 || tool_write_hex_bytes(1, info->armor_crc24, sizeof(info->armor_crc24)) != 0 || rt_write_line(1, " (verified)") != 0) return -1;
        }
    }
    if (rt_write_cstr(1, "  decoded-size: ") != 0 || rt_write_uint(1, (unsigned long long)info->decoded_size) != 0 || rt_write_line(1, " bytes") != 0) return -1;
    return 0;
}

static int inspect_write_packet_text_verbose(const unsigned char *decoded, const PgpPacket *packet) {
    size_t header_size = packet->body_offset - packet->header_offset;
    size_t packet_end = packet->body_offset + packet->body_size;

    if (rt_write_cstr(1, "  packet-format: ") != 0 || rt_write_line(1, packet->new_format ? "new" : "old") != 0) return -1;
    if (rt_write_cstr(1, "  header-offset: ") != 0 || rt_write_uint(1, (unsigned long long)packet->header_offset) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  header-length: ") != 0 || rt_write_uint(1, (unsigned long long)header_size) != 0 || rt_write_line(1, " bytes") != 0) return -1;
    if (rt_write_cstr(1, "  body-offset: ") != 0 || rt_write_uint(1, (unsigned long long)packet->body_offset) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  packet-end-offset: ") != 0 || rt_write_uint(1, (unsigned long long)packet_end) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  length-encoding: ") != 0 || rt_write_line(1, inspect_packet_length_encoding(decoded, packet)) != 0) return -1;
    return 0;
}

static int inspect_write_input_json_details(const PgpMsgInspectInputInfo *info) {
    if (tool_json_begin_event(1, "pgpmsg", "stdout", "input") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"format\":") != 0 || tool_json_write_string(1, info->armored ? "ASCII armor" : "binary OpenPGP") != 0) return -1;
    if (rt_write_cstr(1, ",\"input_size\":") != 0 || rt_write_uint(1, (unsigned long long)info->raw_size) != 0) return -1;
    if (rt_write_cstr(1, ",\"decoded_size\":") != 0 || rt_write_uint(1, (unsigned long long)info->decoded_size) != 0) return -1;
    if (info->armored) {
        if (rt_write_cstr(1, ",\"armor_type\":") != 0 || tool_json_write_string(1, info->armor_kind) != 0) return -1;
        if (rt_write_cstr(1, ",\"armor_body_lines\":") != 0 || rt_write_uint(1, info->armor_body_lines) != 0) return -1;
        if (rt_write_cstr(1, ",\"armor_base64_chars\":") != 0 || rt_write_uint(1, info->armor_base64_chars) != 0) return -1;
        if (info->has_armor_crc24 && (rt_write_cstr(1, ",\"armor_crc24\":\"") != 0 || tool_write_hex_bytes(1, info->armor_crc24, sizeof(info->armor_crc24)) != 0 || rt_write_char(1, '"') != 0)) return -1;
    }
    if (rt_write_cstr(1, "}}") != 0 || tool_json_end_event(1) != 0) return -1;
    return 0;
}

static int inspect_write_packet_json_verbose(const unsigned char *decoded, const PgpPacket *packet) {
    size_t header_size = packet->body_offset - packet->header_offset;
    size_t packet_end = packet->body_offset + packet->body_size;

    if (rt_write_cstr(1, ",\"packet_format\":") != 0 || tool_json_write_string(1, packet->new_format ? "new" : "old") != 0) return -1;
    if (rt_write_cstr(1, ",\"header_offset\":") != 0 || rt_write_uint(1, (unsigned long long)packet->header_offset) != 0) return -1;
    if (rt_write_cstr(1, ",\"header_length\":") != 0 || rt_write_uint(1, (unsigned long long)header_size) != 0) return -1;
    if (rt_write_cstr(1, ",\"body_offset\":") != 0 || rt_write_uint(1, (unsigned long long)packet->body_offset) != 0) return -1;
    if (rt_write_cstr(1, ",\"packet_end_offset\":") != 0 || rt_write_uint(1, (unsigned long long)packet_end) != 0) return -1;
    if (rt_write_cstr(1, ",\"length_encoding\":") != 0 || tool_json_write_string(1, inspect_packet_length_encoding(decoded, packet)) != 0) return -1;
    return 0;
}

static int inspect_packets(const char *path, int json, int verbose) {
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;
    PgpMsgInspectInputInfo input_info;
    PgpPacketReader reader;
    PgpPacket packet;
    int has_packet = 0;
    unsigned long long packet_index = 0ULL;
    char error[PGPMSG_ERROR_CAPACITY];
    int status = 0;

    if (load_decoded_input_with_info(path, &decoded, &decoded_size, &input_info) != 0) return 1;
    if (verbose) {
        if (json) {
            if (inspect_write_input_json_details(&input_info) != 0) { rt_free(decoded); return 1; }
        } else if (inspect_write_input_text_details(&input_info) != 0) { rt_free(decoded); return 1; }
    }
    pgp_packet_reader_init(&reader, decoded, decoded_size);
    while (pgp_packet_reader_next(&reader, &packet, &has_packet, error, sizeof(error)) == 0 && has_packet) {
        packet_index += 1ULL;
        if (json) {
            if (tool_json_begin_event(1, "pgpmsg", "stdout", "packet") != 0) { status = 1; break; }
            if (rt_write_cstr(1, ",\"data\":{") != 0) { status = 1; break; }
            if (rt_write_cstr(1, "\"index\":") != 0 || rt_write_uint(1, packet_index) != 0) { status = 1; break; }
            if (rt_write_cstr(1, ",\"tag\":") != 0 || rt_write_uint(1, packet.tag) != 0) { status = 1; break; }
            if (rt_write_cstr(1, ",\"name\":") != 0 || tool_json_write_string(1, pgp_packet_tag_name(packet.tag)) != 0) { status = 1; break; }
            if (rt_write_cstr(1, ",\"length\":") != 0 || rt_write_uint(1, (unsigned long long)packet.body_size) != 0) { status = 1; break; }
            if (verbose && inspect_write_packet_json_verbose(decoded, &packet) != 0) { status = 1; break; }
            if (inspect_write_packet_json_details(&packet, decoded + packet.body_offset) != 0) { status = 1; break; }
            if (rt_write_cstr(1, "}}") != 0 || tool_json_end_event(1) != 0) { status = 1; break; }
        } else {
            if (rt_write_cstr(1, "packet ") != 0 || rt_write_uint(1, packet_index) != 0 || rt_write_cstr(1, ": tag ") != 0 || rt_write_uint(1, packet.tag) != 0) { status = 1; break; }
            if (rt_write_cstr(1, " (") != 0 || rt_write_cstr(1, pgp_packet_tag_name(packet.tag)) != 0 || rt_write_cstr(1, "), length ") != 0 || rt_write_uint(1, (unsigned long long)packet.body_size) != 0 || rt_write_char(1, '\n') != 0) { status = 1; break; }
            if (verbose && inspect_write_packet_text_verbose(decoded, &packet) != 0) { status = 1; break; }
            if (inspect_write_packet_text_details(&packet, decoded + packet.body_offset) != 0) { status = 1; break; }
        }
    }
    if (!has_packet && packet_index == 0ULL) {
        tool_write_error("pgpmsg", "no OpenPGP packets found", path);
        status = 1;
    }
    rt_free(decoded);
    return status;
}

static int command_inspect(int argc, char **argv, int argi, const PgpMsgOptions *options) {
    PgpMsgOptions local_options = *options;
    const char *path = 0;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-v") == 0 || rt_strcmp(argv[argi], "--verbose") == 0) {
            local_options.verbose = 1;
            argi += 1;
            continue;
        }
        if (argv[argi][0] == '-' && argv[argi][1] != '\0' && rt_strcmp(argv[argi], "-") != 0) {
            tool_write_error("pgpmsg", "unknown inspect option: ", argv[argi]);
            print_usage();
            return 1;
        }
        if (path != 0) {
            print_usage();
            return 1;
        }
        path = argv[argi++];
    }
    return inspect_packets(path, local_options.json, local_options.verbose);
}

static int command_verify(int argc, char **argv, int argi, const PgpMsgOptions *options) {
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;
    unsigned char *signed_data = 0;
    size_t signed_data_size = 0U;
    PgpPacketReader reader;
    PgpPacket packet;
    int has_packet = 0;
    char error[PGPMSG_ERROR_CAPACITY];
    int found_signature = 0;
    int checked_signature = 0;
    int good_signature = 0;
    int bad_signature = 0;
    PgpMsgOptions local_options = *options;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-k") == 0 || rt_strcmp(argv[argi], "--keyring") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpmsg", "missing value for --keyring", 0); return 1; }
            local_options.pubring = argv[argi++];
            continue;
        }
        break;
    }
    if (argi >= argc || argi + 2 < argc) {
        print_usage();
        return 1;
    }
    if (load_decoded_input(argv[argi], &decoded, &decoded_size) != 0) return 1;
    if (argi + 1 < argc && tool_read_all_input(argv[argi + 1], &signed_data, &signed_data_size) != 0) {
        rt_free(decoded);
        tool_write_error("pgpmsg", "cannot read signed data", argv[argi + 1]);
        return 1;
    }
    pgp_packet_reader_init(&reader, decoded, decoded_size);
    while (pgp_packet_reader_next(&reader, &packet, &has_packet, error, sizeof(error)) == 0 && has_packet) {
        if (packet.tag == 2U) {
            PgpMsgSignaturePacket signature;
            if (parse_signature_packet_full(&signature, decoded + packet.body_offset, packet.body_size) == 0 && signature.summary.present) {
                int packet_checked = 0;
                int packet_good = 0;

                found_signature = 1;
                if (signed_data != 0 && (signature.summary.public_key_algorithm == 22U || signature.summary.public_key_algorithm == 27U) && signature.summary.hash_algorithm == 10U) {
                    PgpPublicKeyInfo key;
                    int verified;

                    if (load_public_key_for_signature(local_options.pubring, &signature.summary, &key) != 0) {
                        rt_free(decoded);
                        rt_free(signed_data);
                        return 1;
                    }
                    verified = verify_detached_signature_packet(&signature, decoded + packet.body_offset, signed_data, signed_data_size, &key);
                    packet_checked = 1;
                    packet_good = verified == 1;
                    checked_signature = 1;
                    if (packet_good) good_signature = 1;
                    else bad_signature = 1;
                }
                if (local_options.json) {
                    const char *status = packet_checked ? (packet_good ? "good" : "bad") : "not_checked";
                    if (write_signature_summary_json(&signature.summary, status) != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                } else {
                    if (packet_checked && packet_good) {
                        if (rt_write_line(1, "signature: good") != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                    } else if (packet_checked) {
                        if (rt_write_line(1, "signature: bad") != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                    } else if (write_signature_summary_text(&signature.summary) != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                    if (packet_checked) {
                        if (rt_write_cstr(1, "type: ") != 0 || rt_write_line(1, pgp_signature_type_name(signature.summary.signature_type)) != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                        if (rt_write_cstr(1, "public-key-algorithm: ") != 0 || rt_write_line(1, pgp_public_key_algorithm_name(signature.summary.public_key_algorithm)) != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                        if (rt_write_cstr(1, "hash: ") != 0 || rt_write_line(1, pgp_hash_algorithm_name(signature.summary.hash_algorithm)) != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                        if (signature.summary.has_issuer_key_id) {
                            if (rt_write_cstr(1, "issuer: ") != 0 || tool_write_hex_bytes(1, signature.summary.issuer_key_id, PGPMSG_KEY_ID_SIZE) != 0 || rt_write_char(1, '\n') != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                        }
                        if (signature.summary.issuer_fingerprint_size != 0U) {
                            if (rt_write_cstr(1, "issuer-fpr: ") != 0 || tool_write_hex_bytes(1, signature.summary.issuer_fingerprint, signature.summary.issuer_fingerprint_size) != 0 || rt_write_char(1, '\n') != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                        }
                        if (rt_write_line(1, "trust: not evaluated") != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                    }
                }
            }
        }
    }
    rt_free(decoded);
    if (signed_data != 0) rt_free(signed_data);
    if (!found_signature) {
        tool_write_error("pgpmsg", "no signature packet found", argv[argi]);
        return 1;
    }
    if (checked_signature) return good_signature && !bad_signature ? 0 : 1;
    if (!local_options.json) {
        tool_write_error("pgpmsg", "cryptographic verification is not implemented yet", argv[argi]);
    }
    return 2;
}

static int command_encrypt(int argc, char **argv, int argi, const PgpMsgOptions *options) {
    PgpMsgOptions local_options = *options;
    PgpPublicKeyInfo recipients[PGPMSG_MAX_RECIPIENTS];
    size_t recipient_index;
    unsigned char *input = 0;
    size_t input_size = 0U;
    unsigned char session_key[PGPMSG_AES256_KEY_SIZE];
    PgpMsgSecretKey signing_secret;
    PgpMsgBuffer message;
    PgpMsgBuffer encrypted_packet;
    const char *input_path;
    int have_signing_secret = 0;
    int result = 1;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-k") == 0 || rt_strcmp(argv[argi], "--keyring") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpmsg", "missing value for --keyring", 0); return 1; }
            local_options.pubring = argv[argi++];
        } else if (rt_strcmp(argv[argi], "-r") == 0 || rt_strcmp(argv[argi], "--recipient") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpmsg", "missing value for --recipient", 0); return 1; }
            if (add_recipient_option(&local_options, argv[argi++]) != 0) return 1;
        } else if (rt_strcmp(argv[argi], "-o") == 0 || rt_strcmp(argv[argi], "--output") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpmsg", "missing value for --output", 0); return 1; }
            local_options.output_path = argv[argi++];
        } else if (rt_strcmp(argv[argi], "--armor") == 0) {
            local_options.armor = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--stream") == 0) {
            local_options.stream = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--sign") == 0) {
            local_options.sign = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-s") == 0 || rt_strcmp(argv[argi], "--secring") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpmsg", "missing value for --secring", 0); return 1; }
            local_options.secring = argv[argi++];
        } else if (rt_strcmp(argv[argi], "-u") == 0 || rt_strcmp(argv[argi], "--user") == 0 || rt_strcmp(argv[argi], "--signer") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpmsg", "missing value for --signer", 0); return 1; }
            local_options.signer = argv[argi++];
        } else if (rt_strcmp(argv[argi], "--DANGER-anyway") == 0) {
            local_options.danger_anyway = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--compress") == 0) {
            argi += 1;
            if (argi >= argc || parse_compression_option(argv[argi], &local_options.compression) != 0) { tool_write_error("pgpmsg", "invalid compression algorithm", 0); return 1; }
            argi += 1;
        } else if (tool_starts_with(argv[argi], "--compress=")) {
            if (parse_compression_option(argv[argi] + 11, &local_options.compression) != 0) { tool_write_error("pgpmsg", "invalid compression algorithm", argv[argi] + 11); return 1; }
            argi += 1;
        } else {
            break;
        }
    }
    if (argi + 1 < argc) {
        print_usage();
        return 1;
    }
    input_path = argi < argc ? argv[argi] : 0;
    if (local_options.recipient_count == 0U) {
        tool_write_error("pgpmsg", "encryption requires -r RECIPIENT", 0);
        return 1;
    }
    if (local_options.stream && local_options.armor) {
        tool_write_error("pgpmsg", "--stream does not support --armor yet", 0);
        return 1;
    }
    if (local_options.stream && local_options.compression != PGPMSG_COMPRESSION_NONE) {
        tool_write_error("pgpmsg", "--stream currently supports only --compress=none", 0);
        return 1;
    }
    if (local_options.stream && local_options.sign) {
        tool_write_error("pgpmsg", "--stream does not support --sign yet", 0);
        return 1;
    }
    for (recipient_index = 0U; recipient_index < local_options.recipient_count; ++recipient_index) {
        if (load_recipient_key(local_options.pubring, local_options.recipients[recipient_index], local_options.danger_anyway, &recipients[recipient_index]) != 0) return 1;
    }
    for (recipient_index = 1U; recipient_index < local_options.recipient_count; ++recipient_index) {
        if ((recipients[0].version == 6U) != (recipients[recipient_index].version == 6U)) {
            tool_write_error("pgpmsg", "cannot mix RFC 9580 v6 and legacy v4 recipients in one encrypted message", 0);
            return 1;
        }
    }
    if (local_options.stream) {
        for (recipient_index = 0U; recipient_index < local_options.recipient_count; ++recipient_index) {
            if (recipients[recipient_index].version == 6U) {
                tool_write_error("pgpmsg", "--stream is currently legacy-v4 only; omit --stream for RFC 9580 AEAD encryption", 0);
                return 1;
            }
        }
        if (platform_random_bytes(session_key, sizeof(session_key)) != 0 || command_encrypt_stream(&local_options, recipients, session_key, input_path) != 0) {
            crypto_secure_bzero(session_key, sizeof(session_key));
            tool_write_error("pgpmsg", "streaming encryption failed", 0);
            return 1;
        }
        crypto_secure_bzero(session_key, sizeof(session_key));
        return 0;
    }
    if (tool_read_all_input(input_path, &input, &input_size) != 0) {
        tool_write_error("pgpmsg", "cannot read input", input_path);
        return 1;
    }
    rt_memset(&signing_secret, 0, sizeof(signing_secret));
    if (local_options.sign) {
        if (load_secret_key(local_options.secring, local_options.signer, local_options.danger_anyway, &signing_secret) != 0) goto cleanup;
        have_signing_secret = 1;
    }
    rt_memset(&message, 0, sizeof(message));
    rt_memset(&encrypted_packet, 0, sizeof(encrypted_packet));
    if (platform_random_bytes(session_key, sizeof(session_key)) != 0) goto cleanup;
    for (recipient_index = 0U; recipient_index < local_options.recipient_count; ++recipient_index) {
        PgpMsgBuffer pkesk_packet;

        rt_memset(&pkesk_packet, 0, sizeof(pkesk_packet));
        if (build_encrypted_session_key(&recipients[recipient_index], session_key, &pkesk_packet) != 0 ||
            msg_buffer_append_data(&message, pkesk_packet.data, pkesk_packet.size) != 0) {
            msg_buffer_free(&pkesk_packet);
            goto cleanup;
        }
        msg_buffer_free(&pkesk_packet);
    }
    if ((recipients[0].version == 6U ? build_encrypted_data_packet_v2(session_key, input, input_size, local_options.compression, have_signing_secret ? &signing_secret : 0, &encrypted_packet) : build_encrypted_data_packet(session_key, input, input_size, local_options.compression, have_signing_secret ? &signing_secret : 0, &encrypted_packet)) != 0 ||
        msg_buffer_append_data(&message, encrypted_packet.data, encrypted_packet.size) != 0 ||
        write_message_output_data(local_options.output_path, message.data, message.size, local_options.armor) != 0) goto cleanup;
    result = 0;

cleanup:
    if (input != 0) rt_free(input);
    msg_buffer_free(&message);
    msg_buffer_free(&encrypted_packet);
    if (have_signing_secret) crypto_secure_bzero(&signing_secret, sizeof(signing_secret));
    crypto_secure_bzero(session_key, sizeof(session_key));
    if (result != 0) tool_write_error("pgpmsg", "encryption failed", 0);
    return result;
}

static int command_decrypt(int argc, char **argv, int argi, const PgpMsgOptions *options) {
    PgpMsgOptions local_options = *options;
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;
    PgpPacketReader reader;
    const unsigned char *encrypted_body = 0;
    size_t encrypted_body_size = 0U;
    unsigned char session_key[PGPMSG_AES256_KEY_SIZE];
    char error[PGPMSG_ERROR_CAPACITY];
    const char *input_path;
    int found_pkesk = 0;
    int found_session_key = 0;
    int result = 1;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-k") == 0 || rt_strcmp(argv[argi], "--keyring") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpmsg", "missing value for --keyring", 0); return 1; }
            local_options.pubring = argv[argi++];
        } else if (rt_strcmp(argv[argi], "-s") == 0 || rt_strcmp(argv[argi], "--secring") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpmsg", "missing value for --secring", 0); return 1; }
            local_options.secring = argv[argi++];
        } else if (rt_strcmp(argv[argi], "-o") == 0 || rt_strcmp(argv[argi], "--output") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpmsg", "missing value for --output", 0); return 1; }
            local_options.output_path = argv[argi++];
        } else {
            break;
        }
    }
    if (argi + 1 < argc) {
        print_usage();
        return 1;
    }
    input_path = argi < argc ? argv[argi] : 0;
    if (load_decoded_input(input_path, &decoded, &decoded_size) != 0) return 1;
    pgp_packet_reader_init(&reader, decoded, decoded_size);
    while (1) {
        PgpPacket packet;
        int has_packet;

        if (pgp_packet_reader_next(&reader, &packet, &has_packet, error, sizeof(error)) != 0) {
            tool_write_error("pgpmsg", error, input_path);
            goto cleanup;
        }
        if (!has_packet) break;
        if (packet.tag == 1U) {
            PgpMsgPkesk pkesk;

            rt_memset(&pkesk, 0, sizeof(pkesk));
            if (parse_pkesk_packet(&pkesk, decoded + packet.body_offset, packet.body_size) == 0 && pkesk.found) {
                PgpMsgX25519SecretKey secret;

                found_pkesk = 1;
                rt_memset(&secret, 0, sizeof(secret));
                if (!found_session_key && load_x25519_secret_key_maybe_quiet(local_options.secring, &pkesk, &secret, 1) == 0) {
                    if (unwrap_session_key(&pkesk, &secret, session_key) == 0) found_session_key = 1;
                    crypto_secure_bzero(&secret, sizeof(secret));
                }
            }
            crypto_secure_bzero(&pkesk, sizeof(pkesk));
        } else if (packet.tag == 18U && encrypted_body == 0) {
            encrypted_body = decoded + packet.body_offset;
            encrypted_body_size = packet.body_size;
        }
    }
    if (!found_pkesk || encrypted_body == 0) {
        tool_write_error("pgpmsg", "message does not contain supported ECDH encrypted data", input_path);
        goto cleanup;
    }
    if (!found_session_key) {
        tool_write_error("pgpmsg", "no matching encrypted session key for secret keyring", local_options.secring);
        goto cleanup;
    }
    if ((encrypted_body_size != 0U && encrypted_body[0] == 2U ? decrypt_encrypted_data_packet_v2(session_key, encrypted_body, encrypted_body_size, local_options.output_path, local_options.pubring) : decrypt_encrypted_data_packet(session_key, encrypted_body, encrypted_body_size, local_options.output_path, local_options.pubring)) != 0) {
        tool_write_error("pgpmsg", "decryption failed", input_path);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (decoded != 0) rt_free(decoded);
    crypto_secure_bzero(session_key, sizeof(session_key));
    return result;
}

static int command_sign(int argc, char **argv, int argi, const PgpMsgOptions *options) {
    PgpMsgOptions local_options = *options;
    PgpMsgSecretKey secret;
    unsigned char *input = 0;
    size_t input_size = 0U;
    PgpMsgBuffer hashed;
    PgpMsgBuffer unhashed;
    PgpMsgBuffer hash_part;
    PgpMsgBuffer signature_body;
    PgpMsgBuffer signature_packet;
    PgpMsgBuffer canonical_text;
    unsigned char digest[CRYPTO_SHA512_DIGEST_SIZE];
    unsigned char signature[PGPMSG_ED25519_SIGNATURE_SIZE];
    unsigned char issuer_fingerprint[PGP_FINGERPRINT_MAX_SIZE + 1U];
    unsigned char signature_salt[32];
    unsigned long long created;
    unsigned int signature_version;
    unsigned int public_key_algorithm;
    const char *input_path;
    int result;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-s") == 0 || rt_strcmp(argv[argi], "--secring") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpmsg", "missing value for --secring", 0); return 1; }
            local_options.secring = argv[argi++];
        } else if (rt_strcmp(argv[argi], "-u") == 0 || rt_strcmp(argv[argi], "--user") == 0 || rt_strcmp(argv[argi], "--signer") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpmsg", "missing value for --signer", 0); return 1; }
            local_options.signer = argv[argi++];
        } else if (rt_strcmp(argv[argi], "-o") == 0 || rt_strcmp(argv[argi], "--output") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpmsg", "missing value for --output", 0); return 1; }
            local_options.output_path = argv[argi++];
        } else if (rt_strcmp(argv[argi], "--armor") == 0) {
            local_options.armor = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--detach") == 0) {
            local_options.detach = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--cleartext") == 0) {
            local_options.cleartext = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--DANGER-anyway") == 0) {
            local_options.danger_anyway = 1;
            argi += 1;
        } else {
            break;
        }
    }
    if (!local_options.detach && !local_options.cleartext) {
        tool_write_error("pgpmsg", "sign requires --detach or --cleartext", 0);
        return 1;
    }
    if (local_options.detach && local_options.cleartext) {
        tool_write_error("pgpmsg", "choose either --detach or --cleartext", 0);
        return 1;
    }
    if (local_options.cleartext && local_options.armor) {
        tool_write_error("pgpmsg", "--cleartext output is always armored", 0);
        return 1;
    }
    if (local_options.cleartext && local_options.output_path == 0) {
        tool_write_error("pgpmsg", "cleartext signatures require -o OUT", 0);
        return 1;
    }
    if (local_options.cleartext && local_options.json) {
        tool_write_error("pgpmsg", "cleartext signing does not emit JSON", 0);
        return 2;
    }
    if (argi + 1 < argc) {
        print_usage();
        return 1;
    }
    input_path = argi < argc ? argv[argi] : 0;
    if (load_secret_key(local_options.secring, local_options.signer, local_options.danger_anyway, &secret) != 0) return 1;
    if (tool_read_all_input(input_path, &input, &input_size) != 0) {
        crypto_secure_bzero(&secret, sizeof(secret));
        tool_write_error("pgpmsg", "cannot read input", input_path);
        return 1;
    }
    rt_memset(&hashed, 0, sizeof(hashed));
    rt_memset(&unhashed, 0, sizeof(unhashed));
    rt_memset(&hash_part, 0, sizeof(hash_part));
    rt_memset(&signature_body, 0, sizeof(signature_body));
    rt_memset(&signature_packet, 0, sizeof(signature_packet));
    rt_memset(&canonical_text, 0, sizeof(canonical_text));
    if (local_options.cleartext && canonicalize_cleartext_for_signature(input, input_size, &canonical_text) != 0) {
        result = 1;
        goto cleanup;
    }
    created = platform_get_epoch_time() > 0 ? (unsigned long long)platform_get_epoch_time() : 1ULL;
    signature_version = secret.info.version == 6U ? 6U : 4U;
    public_key_algorithm = signature_version == 6U ? 27U : 22U;
    issuer_fingerprint[0] = (unsigned char)signature_version;
    memcpy(issuer_fingerprint + 1U, secret.info.fingerprint, secret.info.fingerprint_size);
    if (signature_version == 6U && platform_random_bytes(signature_salt, sizeof(signature_salt)) != 0) {
        result = 1;
        goto cleanup;
    }
    if (msg_buffer_append_signature_subpacket_u32(&hashed, signature_version == 6U ? 0x82U : 2U, created) != 0 ||
        (signature_version == 4U && msg_buffer_append_signature_subpacket(&unhashed, 16U, secret.info.key_id, PGP_KEY_ID_SIZE) != 0) ||
        msg_buffer_append_signature_subpacket(signature_version == 6U ? &hashed : &unhashed, 33U, issuer_fingerprint, secret.info.fingerprint_size + 1U) != 0 ||
        msg_buffer_append_byte(&hash_part, signature_version) != 0 ||
        msg_buffer_append_byte(&hash_part, local_options.cleartext ? 0x01U : 0x00U) != 0 ||
        msg_buffer_append_byte(&hash_part, public_key_algorithm) != 0 ||
        msg_buffer_append_byte(&hash_part, 10U) != 0 ||
        (signature_version == 6U ? msg_buffer_append_u32_be(&hash_part, hashed.size) : msg_buffer_append_u16_be(&hash_part, (unsigned int)hashed.size)) != 0 ||
        msg_buffer_append_data(&hash_part, hashed.data, hashed.size) != 0) {
        result = 1;
        goto cleanup;
    }
    if (local_options.cleartext) hash_detached_signature_data_ex(signature_version, signature_version == 6U ? signature_salt : 0, signature_version == 6U ? sizeof(signature_salt) : 0U, canonical_text.data, canonical_text.size, hash_part.data, hash_part.size, digest);
    else hash_detached_signature_data_ex(signature_version, signature_version == 6U ? signature_salt : 0, signature_version == 6U ? sizeof(signature_salt) : 0U, input, input_size, hash_part.data, hash_part.size, digest);
    if (crypto_ed25519_sign(signature, digest, sizeof(digest), secret.seed, secret.info.public_material) != 0) {
        result = 1;
        goto cleanup;
    }
    if (msg_buffer_append_data(&signature_body, hash_part.data, hash_part.size) != 0 ||
        (signature_version == 6U ? msg_buffer_append_u32_be(&signature_body, unhashed.size) : msg_buffer_append_u16_be(&signature_body, (unsigned int)unhashed.size)) != 0 ||
        msg_buffer_append_data(&signature_body, unhashed.data, unhashed.size) != 0 ||
        msg_buffer_append_byte(&signature_body, digest[0]) != 0 ||
        msg_buffer_append_byte(&signature_body, digest[1]) != 0) {
        result = 1;
        goto cleanup;
    }
    if (signature_version == 6U) {
        if (msg_buffer_append_byte(&signature_body, sizeof(signature_salt)) != 0 ||
            msg_buffer_append_data(&signature_body, signature_salt, sizeof(signature_salt)) != 0 ||
            msg_buffer_append_data(&signature_body, signature, sizeof(signature)) != 0) {
            result = 1;
            goto cleanup;
        }
    } else if (msg_buffer_append_opaque_mpi(&signature_body, signature, 32U, 256U) != 0 ||
               msg_buffer_append_opaque_mpi(&signature_body, signature + 32U, 32U, 256U) != 0) {
        result = 1;
        goto cleanup;
    }
    if (msg_buffer_append_packet(&signature_packet, 2U, &signature_body) != 0) {
        result = 1;
        goto cleanup;
    }
    if ((local_options.cleartext ? write_cleartext_signed_output(local_options.output_path, input, input_size, signature_packet.data, signature_packet.size) : write_output_data(local_options.output_path, signature_packet.data, signature_packet.size, local_options.armor)) != 0) {
        result = 1;
        goto cleanup;
    }
    result = 0;

cleanup:
    if (input != 0) rt_free(input);
    msg_buffer_free(&hashed);
    msg_buffer_free(&unhashed);
    msg_buffer_free(&hash_part);
    msg_buffer_free(&signature_body);
    msg_buffer_free(&signature_packet);
    msg_buffer_free(&canonical_text);
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(signature, sizeof(signature));
    crypto_secure_bzero(signature_salt, sizeof(signature_salt));
    crypto_secure_bzero(&secret, sizeof(secret));
    if (result != 0) tool_write_error("pgpmsg", "signing failed", 0);
    return result;
}

int main(int argc, char **argv) {
    PgpMsgOptions options;
    ToolOptState opt;
    int option_result;
    const char *command;

    rt_memset(&options, 0, sizeof(options));
    tool_opt_init(&opt, argc, argv, "pgpmsg", PGPMSG_USAGE);
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-k") == 0 || rt_strcmp(opt.flag, "--keyring") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.pubring = opt.value;
        } else if (rt_strcmp(opt.flag, "-s") == 0 || rt_strcmp(opt.flag, "--secring") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.secring = opt.value;
        } else if (rt_strcmp(opt.flag, "-o") == 0 || rt_strcmp(opt.flag, "--output") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.output_path = opt.value;
        } else if (rt_strcmp(opt.flag, "-r") == 0 || rt_strcmp(opt.flag, "--recipient") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            if (add_recipient_option(&options, opt.value) != 0) return 1;
        } else if (rt_strcmp(opt.flag, "-u") == 0 || rt_strcmp(opt.flag, "--user") == 0 || rt_strcmp(opt.flag, "--signer") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.signer = opt.value;
        } else if (rt_strcmp(opt.flag, "--armor") == 0) {
            options.armor = 1;
        } else if (rt_strcmp(opt.flag, "--detach") == 0) {
            options.detach = 1;
        } else if (rt_strcmp(opt.flag, "--cleartext") == 0) {
            options.cleartext = 1;
        } else if (rt_strcmp(opt.flag, "--DANGER-anyway") == 0) {
            options.danger_anyway = 1;
        } else if (rt_strcmp(opt.flag, "--stream") == 0) {
            options.stream = 1;
        } else if (rt_strcmp(opt.flag, "--sign") == 0) {
            options.sign = 1;
        } else if (rt_strcmp(opt.flag, "-v") == 0 || rt_strcmp(opt.flag, "--verbose") == 0) {
            options.verbose = 1;
        } else if (rt_strcmp(opt.flag, "--compress") == 0) {
            if (tool_opt_require_value(&opt) != 0 || parse_compression_option(opt.value, &options.compression) != 0) return 1;
        } else if (tool_starts_with(opt.flag, "--compress=")) {
            if (parse_compression_option(opt.flag + 11, &options.compression) != 0) return 1;
        } else {
            tool_write_error("pgpmsg", "unknown option: ", opt.flag);
            print_usage();
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        print_usage();
        return 0;
    }
    options.json = tool_json_is_enabled();
    if (opt.argi >= argc) {
        print_usage();
        return 1;
    }
    command = argv[opt.argi++];
    if (rt_strcmp(command, "inspect") == 0 || rt_strcmp(command, "packets") == 0) return command_inspect(argc, argv, opt.argi, &options);
    if (rt_strcmp(command, "verify") == 0) return command_verify(argc, argv, opt.argi, &options);
    if (rt_strcmp(command, "encrypt") == 0) return command_encrypt(argc, argv, opt.argi, &options);
    if (rt_strcmp(command, "decrypt") == 0) return command_decrypt(argc, argv, opt.argi, &options);
    if (rt_strcmp(command, "sign") == 0) return command_sign(argc, argv, opt.argi, &options);
    tool_write_error("pgpmsg", "unknown command: ", command);
    print_usage();
    return 1;
}
