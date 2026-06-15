#include "crypto/aes128.h"
#include "crypto/crypto_util.h"
#include "crypto/curve25519.h"
#include "crypto/ed25519.h"
#include "crypto/sha1.h"
#include "crypto/sha256.h"
#include "crypto/sha512.h"
#include "pgp.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PGPMSG_USAGE "[--json] COMMAND [ARGS...]\n       pgpmsg inspect [FILE]\n       pgpmsg verify [-k PUBRING] SIGNATURE [FILE]\n       pgpmsg encrypt -r RECIPIENT [-r RECIPIENT ...] [-k PUBRING] [-o OUT] [--armor] [FILE]\n       pgpmsg decrypt [-s SECRING] [-o OUT] [FILE]\n       pgpmsg sign -u SIGNER [-s SECRING] [-o OUT] [--armor] [--detach|--cleartext] [FILE]"
#define PGPMSG_ERROR_CAPACITY 160U
#define PGPMSG_KEY_ID_SIZE 8U
#define PGPMSG_ED25519_KEY_SIZE 32U
#define PGPMSG_ED25519_SIGNATURE_SIZE 64U
#define PGPMSG_X25519_KEY_SIZE 32U
#define PGPMSG_AES256_KEY_SIZE 32U
#define PGPMSG_AES_BLOCK_SIZE 16U
#define PGPMSG_MDC_SIZE 20U
#define PGPMSG_MAX_RECIPIENTS 16U

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
} PgpMsgOptions;

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
    int found;
} PgpMsgRecipientFindContext;

typedef struct {
    unsigned char key_id[PGPMSG_KEY_ID_SIZE];
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

static unsigned long long read_u32_be(const unsigned char *data) {
    return ((unsigned long long)data[0] << 24U) |
           ((unsigned long long)data[1] << 16U) |
           ((unsigned long long)data[2] << 8U) |
           (unsigned long long)data[3];
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
    if (memcmp(a, "\xa6\xa6\xa6\xa6\xa6\xa6\xa6\xa6", 8U) != 0) return -1;
    memcpy(plain, r, wrapped_size - 8U);
    *plain_size_out = wrapped_size - 8U;
    crypto_secure_bzero(&aes, sizeof(aes));
    crypto_secure_bzero(a, sizeof(a));
    crypto_secure_bzero(r, sizeof(r));
    crypto_secure_bzero(block, sizeof(block));
    return 0;
}

static int write_hex_bytes(int fd, const unsigned char *data, size_t size) {
    static const char hex[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (rt_write_char(fd, hex[(data[index] >> 4U) & 0x0fU]) != 0) return -1;
        if (rt_write_char(fd, hex[data[index] & 0x0fU]) != 0) return -1;
    }
    return 0;
}

static int write_date(int fd, unsigned long long epoch) {
    long long days = (long long)(epoch / 86400ULL);
    int year;
    unsigned int month;
    unsigned int day;

    tool_civil_from_days(days, &year, &month, &day);
    if (rt_write_uint(fd, (unsigned long long)year) != 0 || rt_write_char(fd, '-') != 0) return -1;
    if (month < 10U && rt_write_char(fd, '0') != 0) return -1;
    if (rt_write_uint(fd, month) != 0 || rt_write_char(fd, '-') != 0) return -1;
    if (day < 10U && rt_write_char(fd, '0') != 0) return -1;
    return rt_write_uint(fd, day);
}

static int load_decoded_input(const char *path, unsigned char **decoded_out, size_t *decoded_size_out) {
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
    rt_free(raw);
    return 0;
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
    *length_out = (size_t)read_u32_be(data + *offset_io);
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
            summary->created = read_u32_be(body);
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
        summary->created = read_u32_be(body + 3U);
        memcpy(summary->issuer_key_id, body + 7U, PGPMSG_KEY_ID_SIZE);
        summary->has_issuer_key_id = 1;
        summary->public_key_algorithm = body[15];
        summary->hash_algorithm = body[16];
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
        if (rt_write_cstr(1, "created: ") != 0 || write_date(1, summary->created) != 0 || rt_write_char(1, '\n') != 0) return -1;
    }
    if (summary->has_issuer_key_id) {
        if (rt_write_cstr(1, "issuer: ") != 0 || write_hex_bytes(1, summary->issuer_key_id, PGPMSG_KEY_ID_SIZE) != 0 || rt_write_char(1, '\n') != 0) return -1;
    }
    if (summary->issuer_fingerprint_size != 0U) {
        if (rt_write_cstr(1, "issuer-fpr: ") != 0 || write_hex_bytes(1, summary->issuer_fingerprint, summary->issuer_fingerprint_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
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
        if (rt_write_cstr(1, ",\"issuer\":\"") != 0 || write_hex_bytes(1, summary->issuer_key_id, PGPMSG_KEY_ID_SIZE) != 0 || rt_write_char(1, '"') != 0) return -1;
    }
    if (summary->issuer_fingerprint_size != 0U) {
        if (rt_write_cstr(1, ",\"issuer_fingerprint\":\"") != 0 || write_hex_bytes(1, summary->issuer_fingerprint, summary->issuer_fingerprint_size) != 0 || rt_write_char(1, '"') != 0) return -1;
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

    if (certificate->primary.algorithm == 22U && certificate->primary.public_material_size == PGPMSG_ED25519_KEY_SIZE && signature_matches_key(ctx->signature, &certificate->primary)) {
        ctx->key = certificate->primary;
        ctx->found = 1;
        return 1;
    }
    for (index = 0U; index < certificate->subkey_count; ++index) {
        if (certificate->subkeys[index].algorithm == 22U && certificate->subkeys[index].public_material_size == PGPMSG_ED25519_KEY_SIZE && signature_matches_key(ctx->signature, &certificate->subkeys[index])) {
            ctx->key = certificate->subkeys[index];
            ctx->found = 1;
            return 1;
        }
    }
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

static int hash_detached_signature_data(const unsigned char *data, size_t data_size, const unsigned char *signature_body, size_t hash_part_size, unsigned char digest[CRYPTO_SHA512_DIGEST_SIZE]) {
    CryptoSha512Context sha512;
    unsigned char trailer[6];

    crypto_sha512_init(&sha512);
    crypto_sha512_update(&sha512, data, data_size);
    crypto_sha512_update(&sha512, signature_body, hash_part_size);
    trailer[0] = 4U;
    trailer[1] = 0xffU;
    trailer[2] = (unsigned char)((hash_part_size >> 24U) & 0xffU);
    trailer[3] = (unsigned char)((hash_part_size >> 16U) & 0xffU);
    trailer[4] = (unsigned char)((hash_part_size >> 8U) & 0xffU);
    trailer[5] = (unsigned char)(hash_part_size & 0xffU);
    crypto_sha512_update(&sha512, trailer, sizeof(trailer));
    crypto_sha512_final(&sha512, digest);
    return 0;
}

static int verify_detached_signature_packet(const PgpMsgSignaturePacket *signature, const unsigned char *signature_body, const unsigned char *data, size_t data_size, const PgpPublicKeyInfo *key) {
    unsigned char digest[CRYPTO_SHA512_DIGEST_SIZE];
    int ok;

    if (signature->summary.version != 4U || signature->summary.signature_type != 0x00U || signature->summary.public_key_algorithm != 22U || signature->summary.hash_algorithm != 10U || !signature->has_signature) return -1;
    hash_detached_signature_data(data, data_size, signature_body, signature->hash_part_size, digest);
    if (digest[0] != signature->digest_prefix[0] || digest[1] != signature->digest_prefix[1]) {
        crypto_secure_bzero(digest, sizeof(digest));
        return 0;
    }
    ok = crypto_ed25519_verify(signature->signature, digest, sizeof(digest), key->public_material) == 0;
    crypto_secure_bzero(digest, sizeof(digest));
    return ok ? 1 : 0;
}

static int parse_ed25519_secret_key_packet(PgpMsgSecretKey *secret, unsigned int tag, const unsigned char *body, size_t body_size, const char *selector) {
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
    if (secret->info.algorithm != 22U || secret->info.public_material_size != PGPMSG_ED25519_KEY_SIZE) return -1;
    if (selector != 0 && selector[0] != '\0' && !pgp_fingerprint_matches_text(&secret->info, selector)) return -1;
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

static int load_secret_key(const char *path, const char *selector, PgpMsgSecretKey *secret) {
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;
    PgpPacketReader reader;
    char error[PGPMSG_ERROR_CAPACITY];

    if (path == 0) {
        tool_write_error("pgpmsg", "signing requires -s SECRING", 0);
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
        if ((packet.tag == 5U || packet.tag == 7U) && parse_ed25519_secret_key_packet(secret, packet.tag, decoded + packet.body_offset, packet.body_size, selector) == 0 && secret->found) {
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
    size_t index;

    for (index = 0U; index < certificate->subkey_count; ++index) {
        const PgpPublicKeyInfo *subkey = &certificate->subkeys[index];

        if (subkey->algorithm != 18U || subkey->public_material_size != PGPMSG_X25519_KEY_SIZE) continue;
        if (certificate_match || pgp_fingerprint_matches_text(subkey, ctx->selector)) {
            ctx->key = *subkey;
            ctx->found = 1;
            return 1;
        }
    }
    return 0;
}

static int load_recipient_key(const char *path, const char *selector, PgpPublicKeyInfo *key_out) {
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
    if (pgp_for_each_certificate(decoded, decoded_size, recipient_find_callback, &ctx, error, sizeof(error)) != 0 && !ctx.found) {
        rt_free(decoded);
        tool_write_error("pgpmsg", error, path);
        return -1;
    }
    rt_free(decoded);
    if (!ctx.found) {
        tool_write_error("pgpmsg", "no matching X25519 encryption subkey in ", path);
        return -1;
    }
    *key_out = ctx.key;
    return 0;
}

static int parse_x25519_secret_key_packet(PgpMsgX25519SecretKey *secret, unsigned int tag, const unsigned char *body, size_t body_size, const unsigned char key_id[PGPMSG_KEY_ID_SIZE]) {
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
    if (secret->info.algorithm != 18U || secret->info.public_material_size != PGPMSG_X25519_KEY_SIZE) return -1;
    if (!key_id_matches_bytes(&secret->info, key_id)) return -1;
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

static int load_x25519_secret_key_maybe_quiet(const char *path, const unsigned char key_id[PGPMSG_KEY_ID_SIZE], PgpMsgX25519SecretKey *secret, int quiet_not_found) {
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
        if ((packet.tag == 5U || packet.tag == 7U) && parse_x25519_secret_key_packet(secret, packet.tag, decoded + packet.body_offset, packet.body_size, key_id) == 0 && secret->found) {
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

    if (body_size < 11U || body[offset++] != 3U) return -1;
    memcpy(pkesk->key_id, body + offset, PGPMSG_KEY_ID_SIZE);
    offset += PGPMSG_KEY_ID_SIZE;
    if (body[offset++] != 18U) return -1;
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

static int build_encrypted_data_packet(const unsigned char session_key[PGPMSG_AES256_KEY_SIZE], const unsigned char *input, size_t input_size, PgpMsgBuffer *encrypted_packet) {
    unsigned char prefix[PGPMSG_AES_BLOCK_SIZE + 2U];
    unsigned char digest[CRYPTO_SHA1_DIGEST_SIZE];
    PgpMsgBuffer literal_packet;
    PgpMsgBuffer plain;
    PgpMsgBuffer body;
    unsigned char mdc_header[] = { 0xd3U, 0x14U };
    int result = -1;

    rt_memset(&literal_packet, 0, sizeof(literal_packet));
    rt_memset(&plain, 0, sizeof(plain));
    rt_memset(&body, 0, sizeof(body));
    if (platform_random_bytes(prefix, PGPMSG_AES_BLOCK_SIZE) != 0) goto cleanup;
    prefix[PGPMSG_AES_BLOCK_SIZE] = prefix[PGPMSG_AES_BLOCK_SIZE - 2U];
    prefix[PGPMSG_AES_BLOCK_SIZE + 1U] = prefix[PGPMSG_AES_BLOCK_SIZE - 1U];
    if (build_literal_packet(input, input_size, &literal_packet) != 0) goto cleanup;
    if (msg_buffer_append_data(&plain, prefix, sizeof(prefix)) != 0 ||
        msg_buffer_append_data(&plain, literal_packet.data, literal_packet.size) != 0 ||
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
    msg_buffer_free(&literal_packet);
    msg_buffer_free(&plain);
    msg_buffer_free(&body);
    crypto_secure_bzero(prefix, sizeof(prefix));
    crypto_secure_bzero(digest, sizeof(digest));
    return result;
}

static int unwrap_session_key(const PgpMsgPkesk *pkesk, const PgpMsgX25519SecretKey *secret, unsigned char session_key[PGPMSG_AES256_KEY_SIZE]) {
    unsigned char shared[PGPMSG_X25519_KEY_SIZE];
    unsigned char kek[PGPMSG_AES256_KEY_SIZE];
    unsigned char padded[56];
    size_t padded_size = 0U;
    size_t message_size;
    unsigned int pad_size;
    unsigned int checksum;
    unsigned int stored_checksum;
    int result = -1;

    if (crypto_x25519_scalarmult(shared, secret->seed, pkesk->ephemeral_public) != 0) goto cleanup;
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
    crypto_secure_bzero(padded, sizeof(padded));
    return result;
}

static int write_decrypted_literal(const char *output_path, const unsigned char *plain, size_t plain_size) {
    PgpPacketReader reader;
    PgpPacket packet;
    int has_packet;
    char error[PGPMSG_ERROR_CAPACITY];
    const unsigned char *body;
    size_t body_size;
    size_t filename_size;
    size_t data_offset;
    int fd;
    int result;

    pgp_packet_reader_init(&reader, plain, plain_size);
    if (pgp_packet_reader_next(&reader, &packet, &has_packet, error, sizeof(error)) != 0 || !has_packet || packet.tag != 11U) {
        tool_write_error("pgpmsg", "decrypted message does not contain a literal data packet", 0);
        return -1;
    }
    body = plain + packet.body_offset;
    body_size = packet.body_size;
    if (body_size < 6U) return -1;
    filename_size = body[1];
    data_offset = 2U + filename_size + 4U;
    if (data_offset > body_size) return -1;
    fd = output_path != 0 ? platform_open_write(output_path, 0644U) : 1;
    if (fd < 0) return -1;
    result = rt_write_all(fd, body + data_offset, body_size - data_offset) == 0 ? 0 : -1;
    if (fd > 2 && platform_close(fd) != 0) result = -1;
    return result;
}

static int decrypt_encrypted_data_packet(const unsigned char session_key[PGPMSG_AES256_KEY_SIZE], const unsigned char *body, size_t body_size, const char *output_path) {
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
    if (plain[PGPMSG_AES_BLOCK_SIZE] != plain[PGPMSG_AES_BLOCK_SIZE - 2U] || plain[PGPMSG_AES_BLOCK_SIZE + 1U] != plain[PGPMSG_AES_BLOCK_SIZE - 1U]) goto cleanup;
    mdc_offset = plain_size - (2U + PGPMSG_MDC_SIZE);
    if (plain[mdc_offset] != 0xd3U || plain[mdc_offset + 1U] != 0x14U) goto cleanup;
    crypto_sha1_hash(plain, mdc_offset + 2U, digest);
    if (memcmp(digest, plain + mdc_offset + 2U, PGPMSG_MDC_SIZE) != 0) goto cleanup;
    if (write_decrypted_literal(output_path, plain + PGPMSG_AES_BLOCK_SIZE + 2U, mdc_offset - (PGPMSG_AES_BLOCK_SIZE + 2U)) != 0) goto cleanup;
    result = 0;

cleanup:
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(plain, plain_size);
    rt_free(plain);
    return result;
}

static int inspect_packets(const char *path, int json) {
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;
    PgpPacketReader reader;
    PgpPacket packet;
    int has_packet = 0;
    unsigned long long packet_index = 0ULL;
    char error[PGPMSG_ERROR_CAPACITY];
    int status = 0;

    if (load_decoded_input(path, &decoded, &decoded_size) != 0) return 1;
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
            if (rt_write_cstr(1, "}}") != 0 || tool_json_end_event(1) != 0) { status = 1; break; }
        } else {
            if (rt_write_cstr(1, "packet ") != 0 || rt_write_uint(1, packet_index) != 0 || rt_write_cstr(1, ": tag ") != 0 || rt_write_uint(1, packet.tag) != 0) { status = 1; break; }
            if (rt_write_cstr(1, " (") != 0 || rt_write_cstr(1, pgp_packet_tag_name(packet.tag)) != 0 || rt_write_cstr(1, "), length ") != 0 || rt_write_uint(1, (unsigned long long)packet.body_size) != 0 || rt_write_char(1, '\n') != 0) { status = 1; break; }
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
    if (argi + 1 < argc) {
        print_usage();
        return 1;
    }
    return inspect_packets(argi < argc ? argv[argi] : 0, options->json);
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
                found_signature = 1;
                if (signed_data != 0 && signature.summary.public_key_algorithm == 22U && signature.summary.hash_algorithm == 10U) {
                    PgpPublicKeyInfo key;
                    int verified;

                    if (load_public_key_for_signature(local_options.pubring, &signature.summary, &key) != 0) {
                        rt_free(decoded);
                        rt_free(signed_data);
                        return 1;
                    }
                    verified = verify_detached_signature_packet(&signature, decoded + packet.body_offset, signed_data, signed_data_size, &key);
                    checked_signature = 1;
                    if (verified == 1) good_signature = 1;
                }
                if (local_options.json) {
                    const char *status = checked_signature ? (good_signature ? "good" : "bad") : "not_checked";
                    if (write_signature_summary_json(&signature.summary, status) != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                } else {
                    if (checked_signature && good_signature) {
                        if (rt_write_line(1, "signature: good") != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                    } else if (checked_signature) {
                        if (rt_write_line(1, "signature: bad") != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                    } else if (write_signature_summary_text(&signature.summary) != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                    if (checked_signature) {
                        if (rt_write_cstr(1, "type: ") != 0 || rt_write_line(1, pgp_signature_type_name(signature.summary.signature_type)) != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                        if (rt_write_cstr(1, "public-key-algorithm: ") != 0 || rt_write_line(1, pgp_public_key_algorithm_name(signature.summary.public_key_algorithm)) != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                        if (rt_write_cstr(1, "hash: ") != 0 || rt_write_line(1, pgp_hash_algorithm_name(signature.summary.hash_algorithm)) != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
                        if (signature.summary.has_issuer_key_id) {
                            if (rt_write_cstr(1, "issuer: ") != 0 || write_hex_bytes(1, signature.summary.issuer_key_id, PGPMSG_KEY_ID_SIZE) != 0 || rt_write_char(1, '\n') != 0) { rt_free(decoded); rt_free(signed_data); return 1; }
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
    if (checked_signature) return good_signature ? 0 : 1;
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
    PgpMsgBuffer message;
    PgpMsgBuffer encrypted_packet;
    const char *input_path;
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
    for (recipient_index = 0U; recipient_index < local_options.recipient_count; ++recipient_index) {
        if (load_recipient_key(local_options.pubring, local_options.recipients[recipient_index], &recipients[recipient_index]) != 0) return 1;
    }
    if (tool_read_all_input(input_path, &input, &input_size) != 0) {
        tool_write_error("pgpmsg", "cannot read input", input_path);
        return 1;
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
    if (build_encrypted_data_packet(session_key, input, input_size, &encrypted_packet) != 0 ||
        msg_buffer_append_data(&message, encrypted_packet.data, encrypted_packet.size) != 0 ||
        write_message_output_data(local_options.output_path, message.data, message.size, local_options.armor) != 0) goto cleanup;
    result = 0;

cleanup:
    if (input != 0) rt_free(input);
    msg_buffer_free(&message);
    msg_buffer_free(&encrypted_packet);
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
        if (rt_strcmp(argv[argi], "-s") == 0 || rt_strcmp(argv[argi], "--secring") == 0) {
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
                if (!found_session_key && load_x25519_secret_key_maybe_quiet(local_options.secring, pkesk.key_id, &secret, 1) == 0) {
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
    if (decrypt_encrypted_data_packet(session_key, encrypted_body, encrypted_body_size, local_options.output_path) != 0) {
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
    unsigned char digest[CRYPTO_SHA512_DIGEST_SIZE];
    unsigned char signature[PGPMSG_ED25519_SIGNATURE_SIZE];
    unsigned char issuer_fingerprint[PGP_FINGERPRINT_MAX_SIZE + 1U];
    unsigned long long created;
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
        } else {
            break;
        }
    }
    if (!local_options.detach || local_options.cleartext) {
        tool_write_error("pgpmsg", "only detached binary signatures are implemented", 0);
        return 2;
    }
    if (argi + 1 < argc) {
        print_usage();
        return 1;
    }
    input_path = argi < argc ? argv[argi] : 0;
    if (load_secret_key(local_options.secring, local_options.signer, &secret) != 0) return 1;
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
    created = platform_get_epoch_time() > 0 ? (unsigned long long)platform_get_epoch_time() : 1ULL;
    issuer_fingerprint[0] = 4U;
    memcpy(issuer_fingerprint + 1U, secret.info.fingerprint, secret.info.fingerprint_size);
    if (msg_buffer_append_signature_subpacket_u32(&hashed, 2U, created) != 0 ||
        msg_buffer_append_signature_subpacket(&unhashed, 16U, secret.info.key_id, PGP_KEY_ID_SIZE) != 0 ||
        msg_buffer_append_signature_subpacket(&unhashed, 33U, issuer_fingerprint, secret.info.fingerprint_size + 1U) != 0 ||
        msg_buffer_append_byte(&hash_part, 4U) != 0 ||
        msg_buffer_append_byte(&hash_part, 0x00U) != 0 ||
        msg_buffer_append_byte(&hash_part, 22U) != 0 ||
        msg_buffer_append_byte(&hash_part, 10U) != 0 ||
        msg_buffer_append_u16_be(&hash_part, (unsigned int)hashed.size) != 0 ||
        msg_buffer_append_data(&hash_part, hashed.data, hashed.size) != 0) {
        result = 1;
        goto cleanup;
    }
    hash_detached_signature_data(input, input_size, hash_part.data, hash_part.size, digest);
    if (crypto_ed25519_sign(signature, digest, sizeof(digest), secret.seed, secret.info.public_material) != 0) {
        result = 1;
        goto cleanup;
    }
    if (msg_buffer_append_data(&signature_body, hash_part.data, hash_part.size) != 0 ||
        msg_buffer_append_u16_be(&signature_body, (unsigned int)unhashed.size) != 0 ||
        msg_buffer_append_data(&signature_body, unhashed.data, unhashed.size) != 0 ||
        msg_buffer_append_byte(&signature_body, digest[0]) != 0 ||
        msg_buffer_append_byte(&signature_body, digest[1]) != 0 ||
        msg_buffer_append_opaque_mpi(&signature_body, signature, 32U, 256U) != 0 ||
        msg_buffer_append_opaque_mpi(&signature_body, signature + 32U, 32U, 256U) != 0 ||
        msg_buffer_append_packet(&signature_packet, 2U, &signature_body) != 0 ||
        write_output_data(local_options.output_path, signature_packet.data, signature_packet.size, local_options.armor) != 0) {
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
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(signature, sizeof(signature));
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
