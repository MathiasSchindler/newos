#include "crypto/crypto_util.h"
#include "crypto/curve25519.h"
#include "crypto/ed25519.h"
#include "crypto/sha512.h"
#include "pgp.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PGPKEY_USAGE "[-k KEYRING] [--store DIR] [-v] [--keystore CATALOG] [--color[=WHEN]|--no-color] [--json] COMMAND [ARGS...]\n       pgpkey show [-v] [--store DIR|--keystore CATALOG] [FILE ...]\n       pgpkey packets FILE ...\n       pgpkey issuers [--external] [FILE ...]\n       pgpkey catalog-sql [FILE ...]\n       pgpkey store init DIR\n       pgpkey store import DIR FILE ...\n       pgpkey store rebuild-index DIR\n       pgpkey generate --userid USERID --out SECRET.asc --public-out PUBLIC.asc --no-passphrase [--expires DURATION] [--profile rfc9580|legacy-v4]\n       pgpkey edit SECRET.asc --out SECRET.asc [--public-out PUBLIC.asc] OPERATION\n       pgpkey import FILE ...\n       pgpkey list\n       pgpkey export FINGERPRINT [OUTPUT]"
#define PGPKEY_PATH_CAPACITY 1024U
#define PGPKEY_ERROR_CAPACITY 160U
#define PGPKEY_ED25519_KEY_SIZE 32U
#define PGPKEY_X25519_KEY_SIZE 32U
#define PGPKEY_ED25519_SIGNATURE_SIZE 64U
#define PGPKEY_MAX_ISSUERS 2048U
#define PGPKEY_MAX_EDIT_TARGETS 32U
#define PGPKEY_KEYSTORE_TEXT_SIZE 512U
#define PGPKEY_STORE_KEYRING_NAME "keyring.pgp"
#define PGPKEY_STORE_INDEX_NAME "pgpkeys.sqs"

typedef struct {
    const char *keyring_path;
    const char *keystore_path;
    const char *store_path;
    int json;
    int verbose;
    int color_mode;
} PgpKeyOptions;

typedef struct {
    char fingerprint[(PGP_FINGERPRINT_MAX_SIZE * 2U) + 1U];
    char key_id[(PGP_KEY_ID_SIZE * 2U) + 1U];
    char primary_uid[PGPKEY_KEYSTORE_TEXT_SIZE];
} PgpKeyStoreEntry;

typedef struct {
    PgpKeyStoreEntry *entries;
    size_t count;
    size_t capacity;
} PgpKeyStore;

static int pgpkey_resolve_store_paths(const char *store_path, char *keyring_path, size_t keyring_path_size, char *keystore_path, size_t keystore_path_size);

typedef struct {
    const unsigned char *data;
    size_t size;
    const char *selector;
    int found;
    const char *output_path;
} PgpKeyExportContext;

typedef struct {
    const char *selector;
    int found;
} PgpKeyFindContext;

typedef struct {
    int public_count;
    int secret_found;
} PgpKeyImportScanContext;

typedef struct {
    const PgpKeyOptions *options;
    const unsigned char *data;
    int status;
} PgpKeyImportContext;

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} PgpKeyBuffer;

typedef struct {
    unsigned char key_id[PGP_KEY_ID_SIZE];
    unsigned char fingerprint[PGP_FINGERPRINT_MAX_SIZE];
    size_t fingerprint_size;
    int has_fingerprint;
} PgpKeyIssuerEntry;

typedef struct {
    PgpKeyIssuerEntry issuers[PGPKEY_MAX_ISSUERS];
    size_t issuer_count;
    unsigned char present_key_ids[PGPKEY_MAX_ISSUERS][PGP_KEY_ID_SIZE];
    size_t present_key_id_count;
    int external_only;
    int json;
    int overflow;
} PgpKeyIssuerContext;

typedef struct {
    const char *user_id;
    const char *secret_out;
    const char *public_out;
    unsigned long long key_expiration_seconds;
    int no_passphrase;
    int legacy_v4;
} PgpKeyGenerateOptions;

typedef struct {
    unsigned char primary_seed[PGPKEY_ED25519_KEY_SIZE];
    unsigned char primary_public[PGPKEY_ED25519_KEY_SIZE];
    unsigned char encryption_seed[PGPKEY_ED25519_KEY_SIZE];
    unsigned char encryption_public[PGPKEY_ED25519_KEY_SIZE];
    PgpPublicKeyInfo primary_info;
    PgpPublicKeyInfo encryption_info;
    PgpKeyBuffer public_body;
    PgpKeyBuffer secret_body;
    PgpKeyBuffer encryption_public_body;
    PgpKeyBuffer encryption_secret_body;
    PgpKeyBuffer user_id_packet;
    PgpKeyBuffer signature_packet;
    PgpKeyBuffer direct_signature_packet;
    PgpKeyBuffer user_signature_packet;
    PgpKeyBuffer subkey_signature_packet;
    PgpKeyBuffer public_certificate;
    PgpKeyBuffer secret_certificate;
} PgpKeyGeneratedKey;

typedef struct {
    const unsigned char *body;
    size_t body_size;
    size_t public_body_size;
    size_t insert_offset;
    PgpPublicKeyInfo info;
} PgpKeyEditPacketTarget;

typedef struct {
    PgpCertificateInfo certificate;
    int have_certificate;
} PgpKeyEditCertificateContext;

typedef struct {
    PgpKeyEditPacketTarget primary;
    PgpKeyEditPacketTarget user_ids[PGPKEY_MAX_EDIT_TARGETS];
    size_t user_id_count;
    PgpKeyEditPacketTarget subkeys[PGPKEY_MAX_EDIT_TARGETS];
    size_t subkey_count;
    int is_secret;
} PgpKeyEditTargets;

typedef struct {
    const char *input_path;
    const char *output_path;
    const char *public_output_path;
    const char *add_uid;
    const char *revoke_uid;
    const char *primary_uid;
    const char *revoke_subkey;
    int add_subkey;
    int refresh;
    int has_expiration;
    unsigned long long expiration_seconds;
} PgpKeyEditOptions;

typedef struct {
    PgpPublicKeyInfo info;
    unsigned char seed[PGPKEY_ED25519_KEY_SIZE];
    unsigned char public_key[PGPKEY_ED25519_KEY_SIZE];
    int found;
} PgpKeyEditSecret;

static int write_all_fd(int fd, const unsigned char *data, size_t size);

static void pgpkey_set_error(char *error, size_t error_size, const char *message) {
    if (error != 0 && error_size > 0U) {
        rt_copy_string(error, error_size, message != 0 ? message : "pgpkey error");
    }
}

static void pgpkey_write_error_path(const char *message, const char *path) {
    char formatted[PGPKEY_ERROR_CAPACITY + 3U];
    size_t length = message != 0 ? rt_strlen(message) : 0U;

    if (length > PGPKEY_ERROR_CAPACITY - 1U) length = PGPKEY_ERROR_CAPACITY - 1U;
    if (length != 0U) memcpy(formatted, message, length);
    formatted[length++] = ':';
    formatted[length++] = ' ';
    formatted[length] = '\0';
    tool_write_error("pgpkey", formatted, path != 0 ? path : "stdin");
}

static void print_usage(void) {
    tool_write_usage("pgpkey", PGPKEY_USAGE);
}

static void pgpkey_buffer_free(PgpKeyBuffer *buffer) {
    if (buffer->data != 0) rt_free(buffer->data);
    buffer->data = 0;
    buffer->size = 0U;
    buffer->capacity = 0U;
}

static int pgpkey_buffer_reserve(PgpKeyBuffer *buffer, size_t extra) {
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

static int pgpkey_buffer_append_byte(PgpKeyBuffer *buffer, unsigned int value) {
    if (pgpkey_buffer_reserve(buffer, 1U) != 0) return -1;
    buffer->data[buffer->size++] = (unsigned char)(value & 0xffU);
    return 0;
}

static int pgpkey_buffer_append_data(PgpKeyBuffer *buffer, const unsigned char *data, size_t size) {
    if (size == 0U) return 0;
    if (pgpkey_buffer_reserve(buffer, size) != 0) return -1;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 0;
}

static int pgpkey_buffer_append_u16_be(PgpKeyBuffer *buffer, unsigned int value) {
    return pgpkey_buffer_append_byte(buffer, (value >> 8U) & 0xffU) != 0 ||
           pgpkey_buffer_append_byte(buffer, value & 0xffU) != 0 ? -1 : 0;
}

static int pgpkey_buffer_append_u32_be(PgpKeyBuffer *buffer, unsigned long long value) {
    return pgpkey_buffer_append_byte(buffer, (unsigned int)((value >> 24U) & 0xffU)) != 0 ||
           pgpkey_buffer_append_byte(buffer, (unsigned int)((value >> 16U) & 0xffU)) != 0 ||
           pgpkey_buffer_append_byte(buffer, (unsigned int)((value >> 8U) & 0xffU)) != 0 ||
           pgpkey_buffer_append_byte(buffer, (unsigned int)(value & 0xffU)) != 0 ? -1 : 0;
}

static int pgpkey_buffer_append_packet_length(PgpKeyBuffer *buffer, size_t length) {
    if (length < 192U) {
        return pgpkey_buffer_append_byte(buffer, (unsigned int)length);
    }
    if (length <= 8383U) {
        size_t encoded = length - 192U;

        return pgpkey_buffer_append_byte(buffer, (unsigned int)((encoded >> 8U) + 192U)) != 0 ||
               pgpkey_buffer_append_byte(buffer, (unsigned int)(encoded & 0xffU)) != 0 ? -1 : 0;
    }
    if (length > 0xffffffffULL) return -1;
    return pgpkey_buffer_append_byte(buffer, 255U) != 0 || pgpkey_buffer_append_u32_be(buffer, (unsigned long long)length) != 0 ? -1 : 0;
}

static int pgpkey_buffer_append_packet(PgpKeyBuffer *buffer, unsigned int tag, const PgpKeyBuffer *body) {
    if (tag > 63U) return -1;
    return pgpkey_buffer_append_byte(buffer, 0xc0U | tag) != 0 ||
           pgpkey_buffer_append_packet_length(buffer, body->size) != 0 ||
           pgpkey_buffer_append_data(buffer, body->data, body->size) != 0 ? -1 : 0;
}

static int pgpkey_buffer_append_opaque_mpi(PgpKeyBuffer *buffer, const unsigned char *data, size_t size, unsigned int bit_count) {
    if (bit_count > 65535U) return -1;
    return pgpkey_buffer_append_u16_be(buffer, bit_count) != 0 || pgpkey_buffer_append_data(buffer, data, size) != 0 ? -1 : 0;
}

static int pgpkey_buffer_append_signature_subpacket(PgpKeyBuffer *buffer, unsigned int type, const unsigned char *body, size_t body_size) {
    size_t subpacket_size = body_size + 1U;

    if (pgpkey_buffer_append_packet_length(buffer, subpacket_size) != 0) return -1;
    if (pgpkey_buffer_append_byte(buffer, type) != 0) return -1;
    return pgpkey_buffer_append_data(buffer, body, body_size);
}

static int pgpkey_buffer_append_signature_subpacket_u32(PgpKeyBuffer *buffer, unsigned int type, unsigned long long value) {
    unsigned char body[4];

    body[0] = (unsigned char)((value >> 24U) & 0xffU);
    body[1] = (unsigned char)((value >> 16U) & 0xffU);
    body[2] = (unsigned char)((value >> 8U) & 0xffU);
    body[3] = (unsigned char)(value & 0xffU);
    return pgpkey_buffer_append_signature_subpacket(buffer, type, body, sizeof(body));
}

static unsigned int pgpkey_read_u16_be(const unsigned char *data) {
    return ((unsigned int)data[0] << 8U) | (unsigned int)data[1];
}

static unsigned long long pgpkey_read_u32_be(const unsigned char *data) {
    return ((unsigned long long)data[0] << 24U) |
           ((unsigned long long)data[1] << 16U) |
           ((unsigned long long)data[2] << 8U) |
           (unsigned long long)data[3];
}

static int pgpkey_packet_end(const PgpPacket *packet, size_t *end_out) {
    if (packet->body_offset > ((size_t)-1) - packet->body_size) return -1;
    *end_out = packet->body_offset + packet->body_size;
    return 0;
}

static int pgpkey_public_body_size(unsigned int tag, const unsigned char *body, size_t body_size, size_t *public_size_out) {
    size_t offset = 6U;
    unsigned int oid_size;
    unsigned int point_bits;
    size_t point_bytes;

    if (tag == 6U || tag == 14U) {
        *public_size_out = body_size;
        return 0;
    }
    if ((tag != 5U && tag != 7U) || body_size < 7U) return -1;
    if (body[0] == 6U) {
        unsigned long long material_size;

        if (body_size < 10U) return -1;
        material_size = pgpkey_read_u32_be(body + 6U);
        if (material_size > (unsigned long long)(body_size - 10U)) return -1;
        *public_size_out = 10U + (size_t)material_size;
        return *public_size_out < body_size ? 0 : -1;
    }
    if (body[0] != 4U) return -1;
    oid_size = body[offset++];
    if (oid_size > body_size - offset) return -1;
    offset += oid_size;
    if (offset + 2U > body_size) return -1;
    point_bits = pgpkey_read_u16_be(body + offset);
    offset += 2U;
    point_bytes = ((size_t)point_bits + 7U) / 8U;
    if (point_bytes > body_size - offset) return -1;
    offset += point_bytes;
    if (body[5] == 18U) {
        unsigned int kdf_size;

        if (offset >= body_size) return -1;
        kdf_size = body[offset++];
        if (kdf_size > body_size - offset) return -1;
        offset += kdf_size;
    }
    if (offset >= body_size) return -1;
    *public_size_out = offset;
    return 0;
}

static int pgpkey_parse_ed25519_secret(const unsigned char *body, size_t body_size, const char *selector, PgpKeyEditSecret *secret) {
    size_t public_size;
    size_t offset;
    unsigned int secret_bits;
    size_t secret_bytes;
    unsigned int checksum = 0U;
    unsigned int stored_checksum;
    size_t index;

    if (pgp_parse_public_key_packet(&secret->info, 5U, body, body_size, 0, 0U) != 0) return -1;
    if (secret->info.version != 4U || secret->info.algorithm != 22U || secret->info.public_material_size != PGPKEY_ED25519_KEY_SIZE) return -1;
    if (selector != 0 && selector[0] != '\0' && !pgp_fingerprint_matches_text(&secret->info, selector)) return -1;
    if (pgpkey_public_body_size(5U, body, body_size, &public_size) != 0 || public_size >= body_size || body[public_size] != 0U) return -1;
    offset = public_size + 1U;
    if (offset + 2U > body_size) return -1;
    secret_bits = pgpkey_read_u16_be(body + offset);
    offset += 2U;
    secret_bytes = ((size_t)secret_bits + 7U) / 8U;
    if (secret_bytes != PGPKEY_ED25519_KEY_SIZE || secret_bytes > body_size - offset) return -1;
    for (index = public_size + 1U; index < offset + secret_bytes; ++index) checksum = (checksum + body[index]) & 0xffffU;
    memcpy(secret->seed, body + offset, PGPKEY_ED25519_KEY_SIZE);
    offset += secret_bytes;
    if (offset + 2U != body_size) return -1;
    stored_checksum = pgpkey_read_u16_be(body + offset);
    if (checksum != stored_checksum) return -1;
    memcpy(secret->public_key, secret->info.public_material, PGPKEY_ED25519_KEY_SIZE);
    secret->found = 1;
    return 0;
}

static int pgpkey_edit_certificate_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpKeyEditCertificateContext *ctx = (PgpKeyEditCertificateContext *)ctx_ptr;

    if (ctx->have_certificate) return 1;
    ctx->certificate = *certificate;
    ctx->have_certificate = 1;
    return 0;
}

static int pgpkey_scan_edit_targets(const unsigned char *data, size_t data_size, const PgpCertificateInfo *certificate, PgpKeyEditTargets *targets, char *error, size_t error_size) {
    PgpPacketReader reader;
    unsigned int active_tag = 0U;
    size_t active_index = 0U;
    size_t packet_end;

    rt_memset(targets, 0, sizeof(*targets));
    pgp_packet_reader_init(&reader, data, data_size);
    while (1) {
        PgpPacket packet;
        int has_packet;

        if (pgp_packet_reader_next(&reader, &packet, &has_packet, error, error_size) != 0) return -1;
        if (!has_packet || packet.header_offset >= certificate->end_offset) break;
        if (packet.header_offset < certificate->start_offset) continue;
        if (pgpkey_packet_end(&packet, &packet_end) != 0) return -1;
        if (packet.tag == 5U || packet.tag == 6U || packet.tag == 7U || packet.tag == 14U || packet.tag == 13U || packet.tag == 17U) {
            if (active_tag == PGP_SIGNATURE_TARGET_USER_ID && active_index < targets->user_id_count && targets->user_ids[active_index].insert_offset == 0U) targets->user_ids[active_index].insert_offset = packet.header_offset;
            if (active_tag == PGP_SIGNATURE_TARGET_SUBKEY && active_index < targets->subkey_count && targets->subkeys[active_index].insert_offset == 0U) targets->subkeys[active_index].insert_offset = packet.header_offset;
        }
        if (packet.tag == 5U || packet.tag == 6U) {
            size_t public_size;

            if (pgpkey_public_body_size(packet.tag, data + packet.body_offset, packet.body_size, &public_size) != 0) return -1;
            targets->primary.body = data + packet.body_offset;
            targets->primary.body_size = packet.body_size;
            targets->primary.public_body_size = public_size;
            targets->primary.insert_offset = packet_end;
            targets->is_secret = packet.tag == 5U;
            active_tag = PGP_SIGNATURE_TARGET_PRIMARY;
            active_index = 0U;
        } else if (packet.tag == 13U && targets->user_id_count < PGPKEY_MAX_EDIT_TARGETS) {
            PgpKeyEditPacketTarget *target = &targets->user_ids[targets->user_id_count];

            target->body = data + packet.body_offset;
            target->body_size = packet.body_size;
            target->public_body_size = packet.body_size;
            target->insert_offset = packet_end;
            active_tag = PGP_SIGNATURE_TARGET_USER_ID;
            active_index = targets->user_id_count++;
        } else if ((packet.tag == 7U || packet.tag == 14U) && targets->subkey_count < PGPKEY_MAX_EDIT_TARGETS) {
            PgpKeyEditPacketTarget *target = &targets->subkeys[targets->subkey_count];
            size_t public_size;

            if (pgpkey_public_body_size(packet.tag, data + packet.body_offset, packet.body_size, &public_size) != 0) return -1;
            target->body = data + packet.body_offset;
            target->body_size = packet.body_size;
            target->public_body_size = public_size;
            target->insert_offset = packet_end;
            if (pgp_parse_public_key_packet(&target->info, packet.tag, data + packet.body_offset, packet.body_size, error, error_size) != 0) return -1;
            active_tag = PGP_SIGNATURE_TARGET_SUBKEY;
            active_index = targets->subkey_count++;
        }
    }
    if (active_tag == PGP_SIGNATURE_TARGET_USER_ID && active_index < targets->user_id_count && targets->user_ids[active_index].insert_offset == 0U) targets->user_ids[active_index].insert_offset = certificate->end_offset;
    if (active_tag == PGP_SIGNATURE_TARGET_SUBKEY && active_index < targets->subkey_count && targets->subkeys[active_index].insert_offset == 0U) targets->subkeys[active_index].insert_offset = certificate->end_offset;
    return targets->primary.body != 0 ? 0 : -1;
}

static int pgpkey_uid_target_matches(const PgpKeyEditPacketTarget *target, const char *user_id) {
    size_t length = user_id != 0 ? rt_strlen(user_id) : 0U;

    return length == target->body_size && memcmp(target->body, user_id, length) == 0;
}

static int pgpkey_find_uid_target(const PgpKeyEditTargets *targets, const char *user_id, size_t *index_out) {
    size_t index;

    for (index = 0U; index < targets->user_id_count; ++index) {
        if (pgpkey_uid_target_matches(&targets->user_ids[index], user_id)) {
            *index_out = index;
            return 0;
        }
    }
    return -1;
}

static int pgpkey_find_subkey_target(const PgpKeyEditTargets *targets, const char *selector, size_t *index_out) {
    size_t index;

    for (index = 0U; index < targets->subkey_count; ++index) {
        if (pgp_fingerprint_matches_text(&targets->subkeys[index].info, selector)) {
            *index_out = index;
            return 0;
        }
    }
    return -1;
}

static int pgpkey_build_edit_signature(PgpKeyBuffer *packet_out, const PgpKeyEditSecret *secret, const PgpKeyEditPacketTarget *primary, unsigned int signature_type, unsigned int target_tag, const unsigned char *target_body, size_t target_body_size, const unsigned char *target_public_body, size_t target_public_body_size, unsigned long long created, const unsigned char *key_flags, size_t key_flags_size, int primary_user_id, int include_preferences, int include_expiration, unsigned long long expiration_seconds) {
    PgpKeyBuffer hashed;
    PgpKeyBuffer unhashed;
    PgpKeyBuffer hash_part;
    PgpKeyBuffer signature_body;
    CryptoSha512Context sha512;
    unsigned char digest[CRYPTO_SHA512_DIGEST_SIZE];
    unsigned char signature[PGPKEY_ED25519_SIGNATURE_SIZE];
    unsigned char issuer_fpr[PGP_FINGERPRINT_MAX_SIZE + 1U];
    unsigned char prefix[5];
    unsigned char trailer[6];
    unsigned char preferred_symmetric[] = { 9U, 8U, 7U };
    unsigned char preferred_hash[] = { 10U, 8U, 9U };
    unsigned char preferred_compression[] = { 2U, 1U, 0U };
    unsigned char features[] = { 0x01U };
    unsigned char primary_flag[1];
    unsigned char reason[] = { 0U };
    int result = -1;

    rt_memset(&hashed, 0, sizeof(hashed));
    rt_memset(&unhashed, 0, sizeof(unhashed));
    rt_memset(&hash_part, 0, sizeof(hash_part));
    rt_memset(&signature_body, 0, sizeof(signature_body));
    if (pgpkey_buffer_append_signature_subpacket_u32(&hashed, 2U, created) != 0) goto cleanup;
    if (key_flags_size != 0U && pgpkey_buffer_append_signature_subpacket(&hashed, 27U, key_flags, key_flags_size) != 0) goto cleanup;
    if (include_expiration && pgpkey_buffer_append_signature_subpacket_u32(&hashed, 9U, expiration_seconds) != 0) goto cleanup;
    if (include_preferences) {
        if (pgpkey_buffer_append_signature_subpacket(&hashed, 11U, preferred_symmetric, sizeof(preferred_symmetric)) != 0 ||
            pgpkey_buffer_append_signature_subpacket(&hashed, 21U, preferred_hash, sizeof(preferred_hash)) != 0 ||
            pgpkey_buffer_append_signature_subpacket(&hashed, 22U, preferred_compression, sizeof(preferred_compression)) != 0 ||
            pgpkey_buffer_append_signature_subpacket(&hashed, 30U, features, sizeof(features)) != 0) goto cleanup;
    }
    if (primary_user_id >= 0) {
        primary_flag[0] = primary_user_id ? 1U : 0U;
        if (pgpkey_buffer_append_signature_subpacket(&hashed, 25U, primary_flag, sizeof(primary_flag)) != 0) goto cleanup;
    }
    if ((signature_type == 0x28U || signature_type == 0x30U) && pgpkey_buffer_append_signature_subpacket(&hashed, 29U, reason, sizeof(reason)) != 0) goto cleanup;
    if (pgpkey_buffer_append_signature_subpacket(&unhashed, 16U, secret->info.key_id, PGP_KEY_ID_SIZE) != 0) goto cleanup;
    issuer_fpr[0] = 4U;
    memcpy(issuer_fpr + 1U, secret->info.fingerprint, secret->info.fingerprint_size);
    if (pgpkey_buffer_append_signature_subpacket(&unhashed, 33U, issuer_fpr, secret->info.fingerprint_size + 1U) != 0) goto cleanup;
    if (pgpkey_buffer_append_byte(&hash_part, 4U) != 0 ||
        pgpkey_buffer_append_byte(&hash_part, signature_type) != 0 ||
        pgpkey_buffer_append_byte(&hash_part, 22U) != 0 ||
        pgpkey_buffer_append_byte(&hash_part, 10U) != 0 ||
        pgpkey_buffer_append_u16_be(&hash_part, (unsigned int)hashed.size) != 0 ||
        pgpkey_buffer_append_data(&hash_part, hashed.data, hashed.size) != 0) goto cleanup;
    crypto_sha512_init(&sha512);
    prefix[0] = 0x99U;
    prefix[1] = (unsigned char)((primary->public_body_size >> 8U) & 0xffU);
    prefix[2] = (unsigned char)(primary->public_body_size & 0xffU);
    crypto_sha512_update(&sha512, prefix, 3U);
    crypto_sha512_update(&sha512, primary->body, primary->public_body_size);
    if (target_tag == PGP_SIGNATURE_TARGET_USER_ID) {
        prefix[0] = 0xb4U;
        prefix[1] = (unsigned char)((target_body_size >> 24U) & 0xffU);
        prefix[2] = (unsigned char)((target_body_size >> 16U) & 0xffU);
        prefix[3] = (unsigned char)((target_body_size >> 8U) & 0xffU);
        prefix[4] = (unsigned char)(target_body_size & 0xffU);
        crypto_sha512_update(&sha512, prefix, sizeof(prefix));
        crypto_sha512_update(&sha512, target_body, target_body_size);
    } else if (target_tag == PGP_SIGNATURE_TARGET_SUBKEY) {
        prefix[0] = 0x99U;
        prefix[1] = (unsigned char)((target_public_body_size >> 8U) & 0xffU);
        prefix[2] = (unsigned char)(target_public_body_size & 0xffU);
        crypto_sha512_update(&sha512, prefix, 3U);
        crypto_sha512_update(&sha512, target_public_body, target_public_body_size);
    }
    crypto_sha512_update(&sha512, hash_part.data, hash_part.size);
    trailer[0] = 4U;
    trailer[1] = 0xffU;
    trailer[2] = (unsigned char)((hash_part.size >> 24U) & 0xffU);
    trailer[3] = (unsigned char)((hash_part.size >> 16U) & 0xffU);
    trailer[4] = (unsigned char)((hash_part.size >> 8U) & 0xffU);
    trailer[5] = (unsigned char)(hash_part.size & 0xffU);
    crypto_sha512_update(&sha512, trailer, sizeof(trailer));
    crypto_sha512_final(&sha512, digest);
    if (crypto_ed25519_sign(signature, digest, sizeof(digest), secret->seed, secret->public_key) != 0) goto cleanup;
    if (pgpkey_buffer_append_data(&signature_body, hash_part.data, hash_part.size) != 0 ||
        pgpkey_buffer_append_u16_be(&signature_body, (unsigned int)unhashed.size) != 0 ||
        pgpkey_buffer_append_data(&signature_body, unhashed.data, unhashed.size) != 0 ||
        pgpkey_buffer_append_byte(&signature_body, digest[0]) != 0 ||
        pgpkey_buffer_append_byte(&signature_body, digest[1]) != 0 ||
        pgpkey_buffer_append_opaque_mpi(&signature_body, signature, 32U, 256U) != 0 ||
        pgpkey_buffer_append_opaque_mpi(&signature_body, signature + 32U, 32U, 256U) != 0 ||
        pgpkey_buffer_append_packet(packet_out, 2U, &signature_body) != 0) goto cleanup;
    result = 0;

cleanup:
    pgpkey_buffer_free(&hashed);
    pgpkey_buffer_free(&unhashed);
    pgpkey_buffer_free(&hash_part);
    pgpkey_buffer_free(&signature_body);
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(signature, sizeof(signature));
    return result;
}

static int write_generated_file(const char *path, int private_key, const unsigned char *data, size_t size) {
    int fd = platform_open_write(path, private_key ? 0600U : 0644U);
    int result;

    if (fd < 0) return -1;
    result = private_key ? pgp_write_private_key_armor(fd, data, size) : pgp_write_public_key_armor(fd, data, size);
    if (fd > 2 && platform_close(fd) != 0) result = -1;
    return result;
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

static int write_duration_from_seconds(int fd, unsigned long long seconds) {
    unsigned long long days = seconds / 86400ULL;

    if (days != 0ULL && seconds % 86400ULL == 0ULL) {
        if (rt_write_uint(fd, days) != 0) return -1;
        return rt_write_cstr(fd, days == 1ULL ? " day" : " days");
    }
    if (rt_write_uint(fd, seconds) != 0) return -1;
    return rt_write_cstr(fd, seconds == 1ULL ? " second" : " seconds");
}

static unsigned long long current_epoch_or_zero(void) {
    long long now = platform_get_epoch_time();

    return now > 0 ? (unsigned long long)now : 0ULL;
}

static int write_expiration_status(int fd, unsigned long long expires, int color_mode) {
    unsigned long long now = current_epoch_or_zero();
    int expired = now != 0ULL && expires != 0ULL && expires <= now;
    const char *text = expired ? "expired" : "not expired";
    int style = expired ? TOOL_STYLE_BOLD_RED : TOOL_STYLE_BOLD_GREEN;

    if (rt_write_cstr(fd, " (") != 0) return -1;
    tool_write_styled(fd, color_mode, style, text);
    return rt_write_char(fd, ')');
}

static int write_expiration_date_with_status(int fd, unsigned long long expires, int color_mode) {
    if (expires == 0ULL) {
        if (rt_write_cstr(fd, "never") != 0) return -1;
        return write_expiration_status(fd, expires, color_mode);
    }
    if (write_date(fd, expires) != 0) return -1;
    return write_expiration_status(fd, expires, color_mode);
}

static int pgpkey_hash_algorithm_style(unsigned int algorithm) {
    if (algorithm == 1U || algorithm == 2U || algorithm == 3U) return TOOL_STYLE_BOLD_RED;
    if (algorithm == 11U) return TOOL_STYLE_BOLD_YELLOW;
    return TOOL_STYLE_PLAIN;
}

static int pgpkey_symmetric_algorithm_style(unsigned int algorithm) {
    if (algorithm == 0U) return TOOL_STYLE_BOLD_RED;
    if (algorithm == 1U || algorithm == 2U || algorithm == 3U) return TOOL_STYLE_BOLD_YELLOW;
    return TOOL_STYLE_PLAIN;
}

static int pgpkey_public_key_algorithm_style(unsigned int algorithm) {
    if (algorithm == 16U) return TOOL_STYLE_BOLD_YELLOW;
    return TOOL_STYLE_PLAIN;
}

static int pgpkey_compression_algorithm_style(unsigned int algorithm) {
    (void)algorithm;
    return TOOL_STYLE_PLAIN;
}

static int write_algorithm_list(int fd, const unsigned char *values, size_t count, const char *(*name_fn)(unsigned int), int (*style_fn)(unsigned int), int color_mode) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        const char *name = name_fn(values[index]);
        int style = style_fn != 0 ? style_fn(values[index]) : TOOL_STYLE_PLAIN;

        if (index != 0U && rt_write_cstr(fd, ", ") != 0) return -1;
        if (style != TOOL_STYLE_PLAIN) {
            tool_write_styled(fd, color_mode, style, name);
        } else if (rt_write_cstr(fd, name) != 0) {
            return -1;
        }
    }
    return 0;
}

static int write_key_flags(int fd, const unsigned char *flags, size_t flags_size) {
    int wrote = 0;
    unsigned int first = flags_size != 0U ? flags[0] : 0U;

    if ((first & 0x01U) != 0U) { if (rt_write_cstr(fd, "certify") != 0) return -1; wrote = 1; }
    if ((first & 0x02U) != 0U) { if (wrote && rt_write_cstr(fd, ", ") != 0) return -1; if (rt_write_cstr(fd, "sign") != 0) return -1; wrote = 1; }
    if ((first & 0x04U) != 0U) { if (wrote && rt_write_cstr(fd, ", ") != 0) return -1; if (rt_write_cstr(fd, "encrypt communications") != 0) return -1; wrote = 1; }
    if ((first & 0x08U) != 0U) { if (wrote && rt_write_cstr(fd, ", ") != 0) return -1; if (rt_write_cstr(fd, "encrypt storage") != 0) return -1; wrote = 1; }
    if ((first & 0x10U) != 0U) { if (wrote && rt_write_cstr(fd, ", ") != 0) return -1; if (rt_write_cstr(fd, "split private key") != 0) return -1; wrote = 1; }
    if ((first & 0x20U) != 0U) { if (wrote && rt_write_cstr(fd, ", ") != 0) return -1; if (rt_write_cstr(fd, "authenticate") != 0) return -1; wrote = 1; }
    if ((first & 0x80U) != 0U) { if (wrote && rt_write_cstr(fd, ", ") != 0) return -1; if (rt_write_cstr(fd, "shared private key") != 0) return -1; wrote = 1; }
    if (!wrote) return rt_write_cstr(fd, "none");
    return 0;
}

static int signature_is_self_issued(const PgpCertificateInfo *certificate, const PgpSignatureInfo *signature) {
    if (certificate->primary.fingerprint_size != 0U && signature->issuer_fingerprint_size == certificate->primary.fingerprint_size) {
        if (memcmp(signature->issuer_fingerprint, certificate->primary.fingerprint, certificate->primary.fingerprint_size) == 0) return 1;
    }
    if (signature->has_issuer_key_id) {
        if (memcmp(signature->issuer_key_id, certificate->primary.key_id, PGP_KEY_ID_SIZE) == 0) return 1;
    }
    return 0;
}

static int signature_is_user_certification(const PgpSignatureInfo *signature) {
    return signature->signature_type >= 0x10U && signature->signature_type <= 0x13U;
}

static const PgpSignatureInfo *latest_self_signature_for_target(const PgpCertificateInfo *certificate, unsigned int target_tag, size_t target_index, int require_key_flags) {
    const PgpSignatureInfo *best = 0;
    size_t index;

    for (index = 0U; index < certificate->signature_info_count; ++index) {
        const PgpSignatureInfo *signature = &certificate->signatures[index];

        if (signature->target_tag != target_tag || signature->target_index != target_index) continue;
        if (!signature_is_self_issued(certificate, signature)) continue;
        if (target_tag == PGP_SIGNATURE_TARGET_USER_ID && !signature_is_user_certification(signature)) continue;
        if (target_tag == PGP_SIGNATURE_TARGET_SUBKEY && signature->signature_type != 0x18U) continue;
        if (require_key_flags && signature->key_flags_size == 0U) continue;
        if (best == 0 || signature->created >= best->created) best = signature;
    }
    return best;
}

static const PgpSignatureInfo *latest_primary_user_id_signature(const PgpCertificateInfo *certificate, size_t *user_index_out) {
    const PgpSignatureInfo *best = 0;
    size_t index;

    *user_index_out = 0U;
    for (index = 0U; index < certificate->signature_info_count; ++index) {
        const PgpSignatureInfo *signature = &certificate->signatures[index];

        if (signature->target_tag != PGP_SIGNATURE_TARGET_USER_ID || !signature_is_user_certification(signature)) continue;
        if (!signature->has_primary_user_id || !signature->primary_user_id) continue;
        if (!signature_is_self_issued(certificate, signature)) continue;
        if (best == 0 || signature->created >= best->created) {
            best = signature;
            *user_index_out = signature->target_index;
        }
    }
    return best;
}

static int write_key_line(const PgpPublicKeyInfo *key, const char *label, int color_mode) {
    const char *algorithm_name = pgp_public_key_algorithm_name(key->algorithm);
    int algorithm_style = pgpkey_public_key_algorithm_style(key->algorithm);

    if (rt_write_cstr(1, label) != 0 || rt_write_cstr(1, ": ") != 0) return -1;
    if (rt_write_cstr(1, pgp_key_kind_name(key->tag)) != 0 || rt_write_cstr(1, ", v") != 0) return -1;
    if (rt_write_uint(1, key->version) != 0 || rt_write_cstr(1, ", ") != 0) return -1;
    if (algorithm_style != TOOL_STYLE_PLAIN) tool_write_styled(1, color_mode, algorithm_style, algorithm_name);
    else if (rt_write_cstr(1, algorithm_name) != 0) return -1;
    if (key->bits != 0U) {
        if (rt_write_cstr(1, ", ") != 0 || rt_write_uint(1, key->bits) != 0 || rt_write_cstr(1, " bits") != 0) return -1;
    }
    if (key->created != 0ULL) {
        if (rt_write_cstr(1, ", created ") != 0 || write_date(1, key->created) != 0) return -1;
    }
    return rt_write_char(1, '\n');
}

static int write_signature_summary_text(const PgpCertificateInfo *certificate, int color_mode) {
    const PgpSignatureInfo *primary_signature;
    const PgpSignatureInfo *direct_signature;
    const PgpSignatureInfo *metadata_signature;
    size_t primary_uid_index;

    primary_signature = latest_primary_user_id_signature(certificate, &primary_uid_index);
    direct_signature = latest_self_signature_for_target(certificate, PGP_SIGNATURE_TARGET_PRIMARY, 0U, 0);
    if (primary_signature != 0 && primary_uid_index < certificate->user_id_count) {
        if (rt_write_cstr(1, "primary-uid: ") != 0 || tool_write_visible_line(1, certificate->user_ids[primary_uid_index]) != 0) return -1;
    } else if (certificate->user_id_count != 0U) {
        primary_signature = latest_self_signature_for_target(certificate, PGP_SIGNATURE_TARGET_USER_ID, 0U, 0);
    }
    metadata_signature = direct_signature != 0 ? direct_signature : primary_signature;
    if (metadata_signature != 0) {
        if (metadata_signature->key_flags_size != 0U) {
            if (rt_write_cstr(1, "key-flags: ") != 0 || write_key_flags(1, metadata_signature->key_flags, metadata_signature->key_flags_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
        if (metadata_signature->has_key_expiration) {
            if (rt_write_cstr(1, "key-expires: ") != 0) return -1;
            if (metadata_signature->key_expiration_seconds == 0ULL) {
                if (write_expiration_date_with_status(1, 0ULL, color_mode) != 0) return -1;
            } else if (write_expiration_date_with_status(1, certificate->primary.created + metadata_signature->key_expiration_seconds, color_mode) != 0) {
                return -1;
            }
            if (rt_write_char(1, '\n') != 0) return -1;
        }
        if (metadata_signature->preferred_symmetric_count != 0U) {
            if (rt_write_cstr(1, "preferred-symmetric: ") != 0 || write_algorithm_list(1, metadata_signature->preferred_symmetric, metadata_signature->preferred_symmetric_count, pgp_symmetric_algorithm_name, pgpkey_symmetric_algorithm_style, color_mode) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
        if (metadata_signature->preferred_hash_count != 0U) {
            if (rt_write_cstr(1, "preferred-hash: ") != 0 || write_algorithm_list(1, metadata_signature->preferred_hash, metadata_signature->preferred_hash_count, pgp_hash_algorithm_name, pgpkey_hash_algorithm_style, color_mode) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
        if (metadata_signature->preferred_compression_count != 0U) {
            if (rt_write_cstr(1, "preferred-compression: ") != 0 || write_algorithm_list(1, metadata_signature->preferred_compression, metadata_signature->preferred_compression_count, pgp_compression_algorithm_name, pgpkey_compression_algorithm_style, color_mode) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
    }
    return 0;
}

static int write_subkey_signature_summary_text(const PgpCertificateInfo *certificate, size_t subkey_index, int color_mode) {
    const PgpSignatureInfo *subkey_signature = latest_self_signature_for_target(certificate, PGP_SIGNATURE_TARGET_SUBKEY, subkey_index, 0);

    if (subkey_signature == 0) return 0;
    if (subkey_signature->key_flags_size != 0U) {
        if (rt_write_cstr(1, "subkey-flags: ") != 0 || write_key_flags(1, subkey_signature->key_flags, subkey_signature->key_flags_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
    }
    if (subkey_signature->has_key_expiration) {
        if (rt_write_cstr(1, "subkey-expires: ") != 0) return -1;
        if (subkey_signature->key_expiration_seconds == 0ULL) {
            if (write_expiration_date_with_status(1, 0ULL, color_mode) != 0) return -1;
        } else if (write_expiration_date_with_status(1, certificate->subkeys[subkey_index].created + subkey_signature->key_expiration_seconds, color_mode) != 0) {
            return -1;
        }
        if (rt_write_char(1, '\n') != 0) return -1;
    }
    return 0;
}

static void pgpkey_hex_bytes_to_text(const unsigned char *data, size_t size, char *out, size_t out_size) {
    static const char hex[] = "0123456789abcdef";
    size_t index;

    if (out_size == 0U) return;
    if ((size * 2U) + 1U > out_size) {
        out[0] = '\0';
        return;
    }
    for (index = 0U; index < size; ++index) {
        out[index * 2U] = hex[(data[index] >> 4) & 0x0fU];
        out[index * 2U + 1U] = hex[data[index] & 0x0fU];
    }
    out[size * 2U] = '\0';
}

static int pgpkey_keystore_line_next(char **cursor_io, char **line_out) {
    char *cursor = *cursor_io;
    char *line = cursor;

    if (cursor == 0 || *cursor == '\0') return 0;
    while (*cursor != '\0' && *cursor != '\n') cursor += 1U;
    if (*cursor == '\n') {
        *cursor = '\0';
        cursor += 1U;
    }
    *cursor_io = cursor;
    *line_out = line;
    return 1;
}

static char *pgpkey_keystore_next_delimited_field(char **cursor_io, char delimiter) {
    char *field = *cursor_io;
    char *cursor = field;

    while (*cursor != '\0' && *cursor != delimiter) cursor += 1U;
    if (*cursor == delimiter) {
        *cursor = '\0';
        cursor += 1U;
    }
    *cursor_io = cursor;
    return field;
}

static int pgpkey_keystore_read_sqs_value(char **cursor_io, char *dst, size_t dst_size) {
    char *cursor = *cursor_io;
    char digits[16];
    size_t digit_count = 0U;
    unsigned long long length;
    size_t index;

    while (*cursor >= '0' && *cursor <= '9') {
        if (digit_count + 1U >= sizeof(digits)) return -1;
        digits[digit_count++] = *cursor++;
    }
    digits[digit_count] = '\0';
    if (digit_count == 0U || *cursor != ':' || rt_parse_uint(digits, &length) != 0 || length + 1ULL > (unsigned long long)dst_size) return -1;
    cursor += 1U;
    for (index = 0U; index < (size_t)length; ++index) {
        if (cursor[index] == '\0') return -1;
        dst[index] = cursor[index];
    }
    dst[(size_t)length] = '\0';
    *cursor_io = cursor + length;
    return 0;
}

static int pgpkey_keystore_add(PgpKeyStore *store, const char *fingerprint, const char *key_id, const char *primary_uid) {
    PgpKeyStoreEntry *next;

    if (key_id[0] == '\0' || primary_uid[0] == '\0') return 0;
    if (store->count == store->capacity) {
        size_t next_capacity = store->capacity == 0U ? 16U : store->capacity * 2U;

        if (next_capacity <= store->capacity) return -1;
        next = (PgpKeyStoreEntry *)rt_realloc_array(store->entries, next_capacity, sizeof(PgpKeyStoreEntry));
        if (next == 0) return -1;
        store->entries = next;
        store->capacity = next_capacity;
    }
    rt_copy_string(store->entries[store->count].fingerprint, sizeof(store->entries[store->count].fingerprint), fingerprint);
    rt_copy_string(store->entries[store->count].key_id, sizeof(store->entries[store->count].key_id), key_id);
    rt_copy_string(store->entries[store->count].primary_uid, sizeof(store->entries[store->count].primary_uid), primary_uid);
    store->count += 1U;
    return 0;
}

static int pgpkey_keystore_parse_certs_row(PgpKeyStore *store, char *line) {
    char *cursor = line + 2U;
    char fingerprint[PGPKEY_KEYSTORE_TEXT_SIZE];
    char key_id[PGPKEY_KEYSTORE_TEXT_SIZE];
    char primary_uid[PGPKEY_KEYSTORE_TEXT_SIZE];
    char scratch[PGPKEY_KEYSTORE_TEXT_SIZE];
    unsigned int column;

    fingerprint[0] = '\0';
    key_id[0] = '\0';
    primary_uid[0] = '\0';
    for (column = 0U; column < 11U; ++column) {
        char *out = scratch;

        if (column == 0U) out = fingerprint;
        else if (column == 1U) out = key_id;
        else if (column == 4U) out = primary_uid;
        if (pgpkey_keystore_read_sqs_value(&cursor, out, PGPKEY_KEYSTORE_TEXT_SIZE) != 0) return -1;
    }
    if (*cursor != '\0') return -1;
    return pgpkey_keystore_add(store, fingerprint, key_id, primary_uid);
}

static int pgpkey_keystore_load(const char *path, PgpKeyStore *store) {
    unsigned char *raw = 0;
    char *text;
    char *cursor;
    char *line;
    size_t raw_size = 0U;
    int in_certs = 0;
    int result = -1;

    store->entries = 0;
    store->count = 0U;
    store->capacity = 0U;
    if (tool_read_all_input(path, &raw, &raw_size) != 0) return -1;
    text = (char *)rt_malloc(raw_size + 1U);
    if (text == 0) goto out_raw;
    memcpy(text, raw, raw_size);
    text[raw_size] = '\0';
    cursor = text;
    if (!pgpkey_keystore_line_next(&cursor, &line) || rt_strcmp(line, "SQS1") != 0) goto out_text;
    while (pgpkey_keystore_line_next(&cursor, &line)) {
        if (line[0] == '\0') continue;
        if (line[0] == 'T' && line[1] == ' ') {
            char *field_cursor = line + 2U;
            char *name = pgpkey_keystore_next_delimited_field(&field_cursor, ' ');

            in_certs = rt_strcmp(name, "certs") == 0;
        } else if (line[0] == 'E' && line[1] == '\0') {
            in_certs = 0;
        } else if (in_certs && line[0] == 'R' && line[1] == ' ') {
            if (pgpkey_keystore_parse_certs_row(store, line) != 0) goto out_text;
        }
    }
    result = 0;

out_text:
    rt_free(text);
out_raw:
    rt_free(raw);
    if (result != 0) {
        rt_free(store->entries);
        store->entries = 0;
        store->count = 0U;
        store->capacity = 0U;
    }
    return result;
}

static void pgpkey_keystore_free(PgpKeyStore *store) {
    rt_free(store->entries);
    store->entries = 0;
    store->count = 0U;
    store->capacity = 0U;
}

static const char *pgpkey_keystore_lookup_text(const PgpKeyStore *store, const char *key_id, const char *fingerprint) {
    size_t index;

    if (store == 0) return 0;
    for (index = 0U; index < store->count; ++index) {
        if (key_id != 0 && key_id[0] != '\0' && rt_strcmp(store->entries[index].key_id, key_id) == 0) return store->entries[index].primary_uid;
        if (fingerprint != 0 && fingerprint[0] != '\0' && rt_strcmp(store->entries[index].fingerprint, fingerprint) == 0) return store->entries[index].primary_uid;
    }
    return 0;
}

static const char *pgpkey_keystore_lookup_signature(const PgpKeyStore *store, const PgpSignatureInfo *signature) {
    char key_id[(PGP_KEY_ID_SIZE * 2U) + 1U];
    char fingerprint[(PGP_FINGERPRINT_MAX_SIZE * 2U) + 1U];

    key_id[0] = '\0';
    fingerprint[0] = '\0';
    if (signature->has_issuer_key_id) pgpkey_hex_bytes_to_text(signature->issuer_key_id, PGP_KEY_ID_SIZE, key_id, sizeof(key_id));
    if (signature->issuer_fingerprint_size != 0U) pgpkey_hex_bytes_to_text(signature->issuer_fingerprint, signature->issuer_fingerprint_size, fingerprint, sizeof(fingerprint));
    return pgpkey_keystore_lookup_text(store, key_id, fingerprint);
}

static int write_signature_verbose_line(const PgpCertificateInfo *certificate, const PgpSignatureInfo *signature, int color_mode, const PgpKeyStore *keystore) {
    const char *issuer_uid = pgpkey_keystore_lookup_signature(keystore, signature);

    if (rt_write_cstr(1, "signature ") != 0 || rt_write_uint(1, signature->packet_index) != 0 || rt_write_cstr(1, ": ") != 0) return -1;
    if (rt_write_cstr(1, pgp_signature_type_name(signature->signature_type)) != 0) return -1;
    if (rt_write_cstr(1, ", v") != 0 || rt_write_uint(1, signature->version) != 0) return -1;
    if (rt_write_cstr(1, ", ") != 0 || rt_write_cstr(1, pgp_public_key_algorithm_name(signature->public_key_algorithm)) != 0) return -1;
    if (rt_write_cstr(1, ", ") != 0 || rt_write_cstr(1, pgp_hash_algorithm_name(signature->hash_algorithm)) != 0) return -1;
    if (signature->created != 0ULL) {
        if (rt_write_cstr(1, ", created ") != 0 || write_date(1, signature->created) != 0) return -1;
    }
    if (signature->target_tag == PGP_SIGNATURE_TARGET_USER_ID && signature->target_index < certificate->user_id_count) {
        if (rt_write_cstr(1, ", uid ") != 0 || rt_write_uint(1, (unsigned long long)(signature->target_index + 1U)) != 0) return -1;
    } else if (signature->target_tag == PGP_SIGNATURE_TARGET_SUBKEY) {
        if (rt_write_cstr(1, ", subkey ") != 0 || rt_write_uint(1, (unsigned long long)(signature->target_index + 1U)) != 0) return -1;
    }
    if (signature->has_issuer_key_id) {
        if (rt_write_cstr(1, ", issuer ") != 0 || write_hex_bytes(1, signature->issuer_key_id, PGP_KEY_ID_SIZE) != 0) return -1;
    }
    if (signature->issuer_fingerprint_size != 0U) {
        if (rt_write_cstr(1, ", issuer-fpr ") != 0 || write_hex_bytes(1, signature->issuer_fingerprint, signature->issuer_fingerprint_size) != 0) return -1;
    }
    if (issuer_uid != 0) {
        if (rt_write_cstr(1, ", issuer-uid ") != 0 || tool_write_visible(1, issuer_uid, rt_strlen(issuer_uid)) != 0) return -1;
    }
    if (signature->has_signature_expiration) {
        if (rt_write_cstr(1, ", signature-expires ") != 0) return -1;
        if (signature->created != 0ULL && signature->signature_expiration_seconds != 0ULL) {
            if (write_expiration_date_with_status(1, signature->created + signature->signature_expiration_seconds, color_mode) != 0) return -1;
            if (rt_write_cstr(1, " after ") != 0) return -1;
        }
        if (write_duration_from_seconds(1, signature->signature_expiration_seconds) != 0) return -1;
    }
    if (signature->has_key_expiration) {
        unsigned long long base = 0ULL;

        if (signature->target_tag == PGP_SIGNATURE_TARGET_SUBKEY && signature->target_index < certificate->subkey_count) {
            base = certificate->subkeys[signature->target_index].created;
        } else {
            base = certificate->primary.created;
        }
        if (rt_write_cstr(1, ", key-expires ") != 0) return -1;
        if (base != 0ULL && signature->key_expiration_seconds != 0ULL) {
            if (write_expiration_date_with_status(1, base + signature->key_expiration_seconds, color_mode) != 0) return -1;
            if (rt_write_cstr(1, " after ") != 0) return -1;
        }
        if (write_duration_from_seconds(1, signature->key_expiration_seconds) != 0) return -1;
    }
    if (signature->key_flags_size != 0U) {
        if (rt_write_cstr(1, ", flags ") != 0 || write_key_flags(1, signature->key_flags, signature->key_flags_size) != 0) return -1;
    }
    if (signature->has_primary_user_id) {
        if (rt_write_cstr(1, signature->primary_user_id ? ", primary-uid yes" : ", primary-uid no") != 0) return -1;
    }
    return rt_write_char(1, '\n');
}

static int write_verbose_signatures_text(const PgpCertificateInfo *certificate, int color_mode, const PgpKeyStore *keystore) {
    size_t index;

    if (certificate->signature_info_count == 0U) return 0;
    if (rt_write_line(1, "signatures:") != 0) return -1;
    for (index = 0U; index < certificate->signature_info_count; ++index) {
        if (write_signature_verbose_line(certificate, &certificate->signatures[index], color_mode, keystore) != 0) return -1;
    }
    return 0;
}

static int write_certificate_text(const PgpCertificateInfo *certificate, const char *source, size_t index, int verbose, int color_mode, const PgpKeyStore *keystore) {
    size_t uid_index;
    size_t subkey_index;
    if (source != 0) {
        if (rt_write_cstr(1, "source: ") != 0 || rt_write_line(1, source) != 0) return -1;
    }
    if (rt_write_cstr(1, "certificate: ") != 0 || rt_write_uint(1, (unsigned long long)index) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (write_key_line(&certificate->primary, "primary", color_mode) != 0) return -1;
    if (certificate->primary.fingerprint_size != 0U) {
        if (rt_write_cstr(1, "fingerprint: ") != 0 || write_hex_bytes(1, certificate->primary.fingerprint, certificate->primary.fingerprint_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
        if (rt_write_cstr(1, "key-id: ") != 0 || write_hex_bytes(1, certificate->primary.key_id, PGP_KEY_ID_SIZE) != 0 || rt_write_char(1, '\n') != 0) return -1;
    } else if (rt_write_line(1, "fingerprint: unsupported") != 0) {
        return -1;
    }
    for (uid_index = 0U; uid_index < certificate->user_id_count; ++uid_index) {
        if (rt_write_cstr(1, "uid: ") != 0 || tool_write_visible_line(1, certificate->user_ids[uid_index]) != 0) return -1;
    }
    if (write_signature_summary_text(certificate, color_mode) != 0) return -1;
    for (subkey_index = 0U; subkey_index < certificate->subkey_count; ++subkey_index) {
        if (write_key_line(&certificate->subkeys[subkey_index], "subkey", color_mode) != 0) return -1;
        if (certificate->subkeys[subkey_index].fingerprint_size != 0U) {
            if (rt_write_cstr(1, "subkey-fingerprint: ") != 0 || write_hex_bytes(1, certificate->subkeys[subkey_index].fingerprint, certificate->subkeys[subkey_index].fingerprint_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
        if (write_subkey_signature_summary_text(certificate, subkey_index, color_mode) != 0) return -1;
    }
    if (rt_write_cstr(1, "packets: ") != 0 || rt_write_uint(1, certificate->packet_count) != 0) return -1;
    if (rt_write_cstr(1, ", signatures: ") != 0 || rt_write_uint(1, certificate->signature_count) != 0) return -1;
    if (rt_write_cstr(1, ", user-attributes: ") != 0 || rt_write_uint(1, certificate->user_attribute_count) != 0) return -1;
    if (rt_write_char(1, '\n') != 0) return -1;
    if (verbose && write_verbose_signatures_text(certificate, color_mode, keystore) != 0) return -1;
    return rt_write_cstr(1, "\n\n");
}

static int write_certificate_json(const PgpCertificateInfo *certificate, const char *source, size_t index) {
    size_t uid_index;

    if (tool_json_begin_event(1, "pgpkey", "stdout", "certificate") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"index\":") != 0 || rt_write_uint(1, (unsigned long long)index) != 0) return -1;
    if (source != 0) {
        if (rt_write_cstr(1, ",\"source\":") != 0 || tool_json_write_string(1, source) != 0) return -1;
    }
    if (rt_write_cstr(1, ",\"algorithm\":") != 0 || tool_json_write_string(1, pgp_public_key_algorithm_name(certificate->primary.algorithm)) != 0) return -1;
    if (rt_write_cstr(1, ",\"fingerprint\":\"") != 0) return -1;
    if (write_hex_bytes(1, certificate->primary.fingerprint, certificate->primary.fingerprint_size) != 0) return -1;
    if (rt_write_cstr(1, "\",\"key_id\":\"") != 0) return -1;
    if (write_hex_bytes(1, certificate->primary.key_id, PGP_KEY_ID_SIZE) != 0) return -1;
    if (rt_write_cstr(1, "\",\"user_ids\":[") != 0) return -1;
    for (uid_index = 0U; uid_index < certificate->user_id_count; ++uid_index) {
        if (uid_index != 0U && rt_write_char(1, ',') != 0) return -1;
        if (tool_json_write_string(1, certificate->user_ids[uid_index]) != 0) return -1;
    }
    if (rt_write_cstr(1, "],\"subkeys\":") != 0 || rt_write_uint(1, (unsigned long long)certificate->subkey_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"signatures\":") != 0 || rt_write_uint(1, certificate->signature_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"signature_infos\":") != 0 || rt_write_uint(1, (unsigned long long)certificate->signature_info_count) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

typedef struct {
    const char *source;
    size_t count;
    int json;
    int verbose;
    int color_mode;
    const PgpKeyStore *keystore;
} PgpKeyShowContext;

static int show_certificate_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpKeyShowContext *ctx = (PgpKeyShowContext *)ctx_ptr;

    ctx->count += 1U;
    if (ctx->json) return write_certificate_json(certificate, ctx->source, ctx->count);
    return write_certificate_text(certificate, ctx->source, ctx->count, ctx->verbose, ctx->color_mode, ctx->keystore);
}

static int load_openpgp_file(const char *path, unsigned char **data_out, size_t *size_out, char *error, size_t error_size) {
    unsigned char *raw = 0;
    size_t raw_size = 0U;

    if (tool_read_all_input(path, &raw, &raw_size) != 0) {
        pgpkey_set_error(error, error_size, "cannot read input");
        return -1;
    }
    if (pgp_decode_input(raw, raw_size, data_out, size_out, error, error_size) != 0) {
        rt_free(raw);
        return -1;
    }
    rt_free(raw);
    return 0;
}

static int show_one_path(const char *path, int json, int verbose, int color_mode, const PgpKeyStore *keystore) {
    unsigned char *data = 0;
    size_t data_size = 0U;
    char error[PGPKEY_ERROR_CAPACITY];
    PgpKeyShowContext ctx;

    if (load_openpgp_file(path, &data, &data_size, error, sizeof(error)) != 0) {
        tool_write_error("pgpkey", error, path != 0 ? ": input failed" : 0);
        return 1;
    }
    ctx.source = path;
    ctx.count = 0U;
    ctx.json = json;
    ctx.verbose = verbose;
    ctx.color_mode = color_mode;
    ctx.keystore = keystore;
    if (pgp_for_each_certificate(data, data_size, show_certificate_callback, &ctx, error, sizeof(error)) != 0) {
        rt_free(data);
        tool_write_error("pgpkey", error, 0);
        return 1;
    }
    rt_free(data);
    if (ctx.count == 0U) {
        tool_write_error("pgpkey", "no OpenPGP certificate found in ", path != 0 ? path : "stdin");
        return 1;
    }
    return 0;
}

static int command_show(const PgpKeyOptions *options, int argc, char **argv, int argi) {
    int status = 0;
    int verbose = options->verbose;
    int color_mode = options->color_mode;
    const char *keystore_path = options->keystore_path;
    const char *show_keyring_path = options->keyring_path;
    const char *store_path = options->store_path;
    char store_keyring_path[PGPKEY_PATH_CAPACITY];
    char store_keystore_path[PGPKEY_PATH_CAPACITY];
    PgpKeyStore keystore;
    const PgpKeyStore *keystore_ptr = 0;
    int scan;
    int have_path = 0;
    int explicit_keystore = keystore_path != 0;

    rt_memset(&keystore, 0, sizeof(keystore));

    for (scan = argi; scan < argc; ++scan) {
        if (rt_strcmp(argv[scan], "-v") == 0 || rt_strcmp(argv[scan], "--verbose") == 0) {
            verbose = 1;
            continue;
        }
        if (rt_strcmp(argv[scan], "--keystore") == 0) {
            if (scan + 1 >= argc) {
                tool_write_error("pgpkey", "missing value for --keystore", 0);
                return 1;
            }
            keystore_path = argv[++scan];
            explicit_keystore = 1;
            continue;
        }
        if (rt_strncmp(argv[scan], "--keystore=", 11U) == 0) {
            keystore_path = argv[scan] + 11U;
            explicit_keystore = 1;
            continue;
        }
        if (rt_strcmp(argv[scan], "--store") == 0) {
            if (scan + 1 >= argc) {
                tool_write_error("pgpkey", "missing value for --store", 0);
                return 1;
            }
            store_path = argv[++scan];
            continue;
        }
        if (rt_strncmp(argv[scan], "--store=", 8U) == 0) {
            store_path = argv[scan] + 8U;
            continue;
        }
        if (rt_strcmp(argv[scan], "--color") == 0 || rt_strcmp(argv[scan], "--no-color") == 0 || rt_strncmp(argv[scan], "--color=", 8U) == 0) {
            continue;
        }
        have_path = 1;
    }

    if (store_path != 0) {
        if (pgpkey_resolve_store_paths(store_path, store_keyring_path, sizeof(store_keyring_path), store_keystore_path, sizeof(store_keystore_path)) != 0) {
            tool_write_error("pgpkey", "invalid store path: ", store_path);
            return 1;
        }
        if (!have_path) show_keyring_path = store_keyring_path;
        if (!explicit_keystore) keystore_path = store_keystore_path;
    }

    if (keystore_path != 0) {
        if (pgpkey_keystore_load(keystore_path, &keystore) != 0) {
            tool_write_error("pgpkey", "cannot read keystore: ", keystore_path);
            return 1;
        }
        keystore_ptr = &keystore;
    }

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-v") == 0 || rt_strcmp(argv[argi], "--verbose") == 0) {
            verbose = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--keystore") == 0) {
            argi += 2;
            continue;
        }
        if (rt_strncmp(argv[argi], "--keystore=", 11U) == 0) {
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--store") == 0) {
            argi += 2;
            continue;
        }
        if (rt_strncmp(argv[argi], "--store=", 8U) == 0) {
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--color") == 0) {
            color_mode = TOOL_COLOR_AUTO;
            argi += 1;
            continue;
        }
        if (rt_strncmp(argv[argi], "--color=", 8U) == 0) {
            if (tool_parse_color_mode(argv[argi] + 8, &color_mode) != 0) {
                tool_write_error("pgpkey", "invalid color mode: ", argv[argi] + 8);
                return 1;
            }
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--no-color") == 0) {
            color_mode = TOOL_COLOR_NEVER;
            argi += 1;
            continue;
        }
        break;
    }

    if (!have_path) {
        status = show_one_path(show_keyring_path, options->json, verbose, color_mode, keystore_ptr);
        pgpkey_keystore_free(&keystore);
        return status;
    }
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-v") == 0 || rt_strcmp(argv[argi], "--verbose") == 0) {
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--keystore") == 0) {
            argi += 2;
            continue;
        }
        if (rt_strncmp(argv[argi], "--keystore=", 11U) == 0) {
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--store") == 0) {
            argi += 2;
            continue;
        }
        if (rt_strncmp(argv[argi], "--store=", 8U) == 0) {
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--color") == 0 || rt_strcmp(argv[argi], "--no-color") == 0 || rt_strncmp(argv[argi], "--color=", 8U) == 0) {
            argi += 1;
            continue;
        }
        if (show_one_path(argv[argi], options->json, verbose, color_mode, keystore_ptr) != 0) status = 1;
        argi += 1;
    }
    pgpkey_keystore_free(&keystore);
    return status;
}

static int command_packets(int argc, char **argv, int argi) {
    int status = 0;

    if (argi >= argc) {
        print_usage();
        return 1;
    }
    while (argi < argc) {
        unsigned char *data = 0;
        size_t data_size = 0U;
        PgpPacketReader reader;
        unsigned long long count = 0ULL;
        char error[PGPKEY_ERROR_CAPACITY];

        if (load_openpgp_file(argv[argi], &data, &data_size, error, sizeof(error)) != 0) {
            pgpkey_write_error_path(error, argv[argi]);
            status = 1;
            argi += 1;
            continue;
        }
        pgp_packet_reader_init(&reader, data, data_size);
        while (1) {
            PgpPacket packet;
            int has_packet;

            if (pgp_packet_reader_next(&reader, &packet, &has_packet, error, sizeof(error)) != 0) {
                tool_write_error("pgpkey", error, 0);
                status = 1;
                break;
            }
            if (!has_packet) break;
            count += 1ULL;
            if (rt_write_cstr(1, "packet ") != 0 || rt_write_uint(1, count) != 0 || rt_write_cstr(1, ": tag ") != 0) return 1;
            if (rt_write_uint(1, packet.tag) != 0 || rt_write_cstr(1, " (") != 0 || rt_write_cstr(1, pgp_packet_tag_name(packet.tag)) != 0) return 1;
            if (rt_write_cstr(1, "), length ") != 0 || rt_write_uint(1, (unsigned long long)packet.body_size) != 0 || rt_write_char(1, '\n') != 0) return 1;
        }
        rt_free(data);
        argi += 1;
    }
    return status;
}

static int pgpkey_key_id_list_contains(unsigned char ids[PGPKEY_MAX_ISSUERS][PGP_KEY_ID_SIZE], size_t count, const unsigned char key_id[PGP_KEY_ID_SIZE]) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (memcmp(ids[index], key_id, PGP_KEY_ID_SIZE) == 0) return 1;
    }
    return 0;
}

static int pgpkey_add_present_key_id(PgpKeyIssuerContext *ctx, const unsigned char key_id[PGP_KEY_ID_SIZE]) {
    if (pgpkey_key_id_list_contains(ctx->present_key_ids, ctx->present_key_id_count, key_id)) return 0;
    if (ctx->present_key_id_count >= PGPKEY_MAX_ISSUERS) {
        ctx->overflow = 1;
        return -1;
    }
    memcpy(ctx->present_key_ids[ctx->present_key_id_count++], key_id, PGP_KEY_ID_SIZE);
    return 0;
}

static int pgpkey_add_issuer(PgpKeyIssuerContext *ctx, const unsigned char key_id[PGP_KEY_ID_SIZE], const unsigned char *fingerprint, size_t fingerprint_size) {
    size_t index;

    for (index = 0U; index < ctx->issuer_count; ++index) {
        if (memcmp(ctx->issuers[index].key_id, key_id, PGP_KEY_ID_SIZE) == 0) {
            if (!ctx->issuers[index].has_fingerprint && fingerprint != 0 && fingerprint_size <= PGP_FINGERPRINT_MAX_SIZE) {
                memcpy(ctx->issuers[index].fingerprint, fingerprint, fingerprint_size);
                ctx->issuers[index].fingerprint_size = fingerprint_size;
                ctx->issuers[index].has_fingerprint = 1;
            }
            return 0;
        }
    }
    if (ctx->issuer_count >= PGPKEY_MAX_ISSUERS) {
        ctx->overflow = 1;
        return -1;
    }
    memcpy(ctx->issuers[ctx->issuer_count].key_id, key_id, PGP_KEY_ID_SIZE);
    if (fingerprint != 0 && fingerprint_size <= PGP_FINGERPRINT_MAX_SIZE) {
        memcpy(ctx->issuers[ctx->issuer_count].fingerprint, fingerprint, fingerprint_size);
        ctx->issuers[ctx->issuer_count].fingerprint_size = fingerprint_size;
        ctx->issuers[ctx->issuer_count].has_fingerprint = 1;
    }
    ctx->issuer_count += 1U;
    return 0;
}

static int collect_issuer_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpKeyIssuerContext *ctx = (PgpKeyIssuerContext *)ctx_ptr;
    size_t index;

    if (certificate->primary.fingerprint_size != 0U) (void)pgpkey_add_present_key_id(ctx, certificate->primary.key_id);
    for (index = 0U; index < certificate->subkey_count; ++index) {
        if (certificate->subkeys[index].fingerprint_size != 0U) (void)pgpkey_add_present_key_id(ctx, certificate->subkeys[index].key_id);
    }
    for (index = 0U; index < certificate->signature_info_count; ++index) {
        const PgpSignatureInfo *signature = &certificate->signatures[index];
        unsigned char key_id[PGP_KEY_ID_SIZE];
        const unsigned char *fingerprint = 0;
        size_t fingerprint_size = 0U;

        if (signature->has_issuer_key_id) {
            memcpy(key_id, signature->issuer_key_id, PGP_KEY_ID_SIZE);
        } else if (signature->issuer_fingerprint_size >= PGP_KEY_ID_SIZE) {
            memcpy(key_id, signature->issuer_fingerprint + signature->issuer_fingerprint_size - PGP_KEY_ID_SIZE, PGP_KEY_ID_SIZE);
        } else {
            continue;
        }
        if (signature->issuer_fingerprint_size != 0U) {
            fingerprint = signature->issuer_fingerprint;
            fingerprint_size = signature->issuer_fingerprint_size;
        }
        (void)pgpkey_add_issuer(ctx, key_id, fingerprint, fingerprint_size);
    }
    return 0;
}

static int collect_issuers_from_path(const char *path, PgpKeyIssuerContext *ctx) {
    unsigned char *data = 0;
    size_t data_size = 0U;
    char error[PGPKEY_ERROR_CAPACITY];

    if (load_openpgp_file(path, &data, &data_size, error, sizeof(error)) != 0) {
        tool_write_error("pgpkey", "input failed: ", path != 0 ? path : "stdin");
        return -1;
    }
    if (pgp_for_each_certificate(data, data_size, collect_issuer_callback, ctx, error, sizeof(error)) != 0) {
        rt_free(data);
        tool_write_error("pgpkey", error, 0);
        return -1;
    }
    rt_free(data);
    return ctx->overflow ? -1 : 0;
}

static int write_issuer_entry(const PgpKeyIssuerContext *ctx, const PgpKeyIssuerEntry *entry) {
    if (ctx->json) {
        if (tool_json_begin_event(1, "pgpkey", "stdout", "issuer") != 0) return -1;
        if (rt_write_cstr(1, ",\"data\":{\"key_id\":\"") != 0 || write_hex_bytes(1, entry->key_id, PGP_KEY_ID_SIZE) != 0 || rt_write_char(1, '"') != 0) return -1;
        if (entry->has_fingerprint) {
            if (rt_write_cstr(1, ",\"fingerprint\":\"") != 0 || write_hex_bytes(1, entry->fingerprint, entry->fingerprint_size) != 0 || rt_write_char(1, '"') != 0) return -1;
        }
        if (rt_write_char(1, '}') != 0 || tool_json_end_event(1) != 0) return -1;
        return 0;
    }
    if (write_hex_bytes(1, entry->key_id, PGP_KEY_ID_SIZE) != 0) return -1;
    if (entry->has_fingerprint) {
        if (rt_write_cstr(1, " ") != 0 || write_hex_bytes(1, entry->fingerprint, entry->fingerprint_size) != 0) return -1;
    }
    return rt_write_char(1, '\n');
}

static int command_issuers(const PgpKeyOptions *options, int argc, char **argv, int argi) {
    PgpKeyIssuerContext ctx;
    size_t index;
    int status = 0;

    rt_memset(&ctx, 0, sizeof(ctx));
    ctx.json = options->json;
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--external") == 0) {
            ctx.external_only = 1;
            argi += 1;
            continue;
        }
        break;
    }
    if (argi >= argc) {
        if (collect_issuers_from_path(options->keyring_path, &ctx) != 0) status = 1;
    } else {
        while (argi < argc) {
            if (collect_issuers_from_path(argv[argi], &ctx) != 0) status = 1;
            argi += 1;
        }
    }
    if (ctx.overflow) {
        tool_write_error("pgpkey", "too many issuer IDs", 0);
        status = 1;
    }
    if (status != 0) return status;
    for (index = 0U; index < ctx.issuer_count; ++index) {
        if (ctx.external_only && pgpkey_key_id_list_contains(ctx.present_key_ids, ctx.present_key_id_count, ctx.issuers[index].key_id)) continue;
        if (write_issuer_entry(&ctx, &ctx.issuers[index]) != 0) return 1;
    }
    return 0;
}

static int find_certificate_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpKeyFindContext *ctx = (PgpKeyFindContext *)ctx_ptr;

    if (pgp_fingerprint_matches_text(&certificate->primary, ctx->selector)) {
        ctx->found = 1;
        return 1;
    }
    return 0;
}

#define PGPKEY_SQL_TEXT_LIMIT 500U

typedef struct {
    const char *source;
    int status;
} PgpKeyCatalogSqlContext;

static int pgpkey_sql_write_text(int fd, const char *text) {
    size_t index;
    size_t written = 0U;

    if (rt_write_char(fd, '\'') != 0) return -1;
    if (text != 0) {
        for (index = 0U; text[index] != '\0' && written < PGPKEY_SQL_TEXT_LIMIT; ++index) {
            unsigned char ch = (unsigned char)text[index];

            if (ch == '\'' || ch == '\\') {
                if (rt_write_char(fd, '\\') != 0 || rt_write_char(fd, (char)ch) != 0) return -1;
            } else if (ch < 0x20U || ch == 0x7fU) {
                if (rt_write_char(fd, ' ') != 0) return -1;
            } else if (rt_write_char(fd, (char)ch) != 0) {
                return -1;
            }
            written += 1U;
        }
    }
    return rt_write_char(fd, '\'');
}

static int pgpkey_sql_write_hex_value(int fd, const unsigned char *data, size_t size) {
    if (rt_write_char(fd, '\'') != 0) return -1;
    if (write_hex_bytes(fd, data, size) != 0) return -1;
    return rt_write_char(fd, '\'');
}

static const char *pgpkey_catalog_signature_target_name(unsigned int target_tag) {
    if (target_tag == PGP_SIGNATURE_TARGET_PRIMARY) return "primary";
    if (target_tag == PGP_SIGNATURE_TARGET_USER_ID) return "uid";
    if (target_tag == PGP_SIGNATURE_TARGET_SUBKEY) return "subkey";
    if (target_tag == PGP_SIGNATURE_TARGET_USER_ATTRIBUTE) return "userattr";
    return "unknown";
}

static const char *pgpkey_catalog_primary_uid(const PgpCertificateInfo *certificate) {
    const PgpSignatureInfo *signature;
    size_t primary_index = 0U;

    signature = latest_primary_user_id_signature(certificate, &primary_index);
    if (signature != 0 && primary_index < certificate->user_id_count) return certificate->user_ids[primary_index];
    if (certificate->user_id_count != 0U) return certificate->user_ids[0];
    return "";
}

static int pgpkey_catalog_write_schema_sql(void) {
    return rt_write_line(1, "CREATE TABLE IF NOT EXISTS certs(fingerprint TEXT, key_id TEXT, version INTEGER, algorithm TEXT, primary_uid TEXT, created INTEGER, packet_count INTEGER, signature_count INTEGER, source TEXT, cert_offset INTEGER, cert_length INTEGER);") != 0 ||
           rt_write_line(1, "CREATE TABLE IF NOT EXISTS keys(cert_fingerprint TEXT, fingerprint TEXT, key_id TEXT, role TEXT, version INTEGER, algorithm TEXT, bits INTEGER, created INTEGER);") != 0 ||
           rt_write_line(1, "CREATE TABLE IF NOT EXISTS user_ids(cert_fingerprint TEXT, uid_index INTEGER, uid TEXT, primary_uid INTEGER);") != 0 ||
           rt_write_line(1, "CREATE TABLE IF NOT EXISTS signatures(cert_fingerprint TEXT, packet_index INTEGER, signature_type TEXT, target TEXT, target_index INTEGER, issuer_key_id TEXT, issuer_fingerprint TEXT, created INTEGER, signature_expires INTEGER);") != 0 ||
           rt_write_line(1, "DELETE FROM certs;") != 0 ||
           rt_write_line(1, "DELETE FROM keys;") != 0 ||
           rt_write_line(1, "DELETE FROM user_ids;") != 0 ||
           rt_write_line(1, "DELETE FROM signatures;") != 0 ? -1 : 0;
}

static int pgpkey_catalog_write_key_sql(const PgpPublicKeyInfo *key, const PgpPublicKeyInfo *primary, const char *role) {
    if (rt_write_cstr(1, "INSERT INTO keys VALUES(") != 0 ||
        pgpkey_sql_write_hex_value(1, primary->fingerprint, primary->fingerprint_size) != 0 ||
        rt_write_char(1, ',') != 0 ||
        pgpkey_sql_write_hex_value(1, key->fingerprint, key->fingerprint_size) != 0 ||
        rt_write_char(1, ',') != 0 ||
        pgpkey_sql_write_hex_value(1, key->key_id, PGP_KEY_ID_SIZE) != 0 ||
        rt_write_char(1, ',') != 0 ||
        pgpkey_sql_write_text(1, role) != 0 ||
        rt_write_char(1, ',') != 0 ||
        rt_write_uint(1, key->version) != 0 ||
        rt_write_char(1, ',') != 0 ||
        pgpkey_sql_write_text(1, pgp_public_key_algorithm_name(key->algorithm)) != 0 ||
        rt_write_char(1, ',') != 0 ||
        rt_write_uint(1, key->bits) != 0 ||
        rt_write_char(1, ',') != 0 ||
        rt_write_uint(1, key->created) != 0 ||
        rt_write_line(1, ");") != 0) return -1;
    return 0;
}

static int pgpkey_catalog_write_certificate_sql(const PgpCertificateInfo *certificate, const char *source) {
    size_t index;
    const char *primary_uid = pgpkey_catalog_primary_uid(certificate);

    if (rt_write_cstr(1, "INSERT INTO certs VALUES(") != 0 ||
        pgpkey_sql_write_hex_value(1, certificate->primary.fingerprint, certificate->primary.fingerprint_size) != 0 ||
        rt_write_char(1, ',') != 0 ||
        pgpkey_sql_write_hex_value(1, certificate->primary.key_id, PGP_KEY_ID_SIZE) != 0 ||
        rt_write_char(1, ',') != 0 ||
        rt_write_uint(1, certificate->primary.version) != 0 ||
        rt_write_char(1, ',') != 0 ||
        pgpkey_sql_write_text(1, pgp_public_key_algorithm_name(certificate->primary.algorithm)) != 0 ||
        rt_write_char(1, ',') != 0 ||
        pgpkey_sql_write_text(1, primary_uid) != 0 ||
        rt_write_char(1, ',') != 0 ||
        rt_write_uint(1, certificate->primary.created) != 0 ||
        rt_write_char(1, ',') != 0 ||
        rt_write_uint(1, certificate->packet_count) != 0 ||
        rt_write_char(1, ',') != 0 ||
        rt_write_uint(1, certificate->signature_count) != 0 ||
        rt_write_char(1, ',') != 0 ||
        pgpkey_sql_write_text(1, source) != 0 ||
        rt_write_char(1, ',') != 0 ||
        rt_write_uint(1, (unsigned long long)certificate->start_offset) != 0 ||
        rt_write_char(1, ',') != 0 ||
        rt_write_uint(1, (unsigned long long)(certificate->end_offset - certificate->start_offset)) != 0 ||
        rt_write_line(1, ");") != 0) return -1;
    if (pgpkey_catalog_write_key_sql(&certificate->primary, &certificate->primary, "primary") != 0) return -1;
    for (index = 0U; index < certificate->subkey_count; ++index) {
        if (pgpkey_catalog_write_key_sql(&certificate->subkeys[index], &certificate->primary, "subkey") != 0) return -1;
    }
    for (index = 0U; index < certificate->user_id_count; ++index) {
        int is_primary = rt_strcmp(certificate->user_ids[index], primary_uid) == 0;

        if (rt_write_cstr(1, "INSERT INTO user_ids VALUES(") != 0 ||
            pgpkey_sql_write_hex_value(1, certificate->primary.fingerprint, certificate->primary.fingerprint_size) != 0 ||
            rt_write_char(1, ',') != 0 ||
            rt_write_uint(1, (unsigned long long)(index + 1U)) != 0 ||
            rt_write_char(1, ',') != 0 ||
            pgpkey_sql_write_text(1, certificate->user_ids[index]) != 0 ||
            rt_write_char(1, ',') != 0 ||
            rt_write_uint(1, is_primary ? 1ULL : 0ULL) != 0 ||
            rt_write_line(1, ");") != 0) return -1;
    }
    for (index = 0U; index < certificate->signature_info_count; ++index) {
        const PgpSignatureInfo *signature = &certificate->signatures[index];

        if (rt_write_cstr(1, "INSERT INTO signatures VALUES(") != 0 ||
            pgpkey_sql_write_hex_value(1, certificate->primary.fingerprint, certificate->primary.fingerprint_size) != 0 ||
            rt_write_char(1, ',') != 0 ||
            rt_write_uint(1, signature->packet_index) != 0 ||
            rt_write_char(1, ',') != 0 ||
            pgpkey_sql_write_text(1, pgp_signature_type_name(signature->signature_type)) != 0 ||
            rt_write_char(1, ',') != 0 ||
            pgpkey_sql_write_text(1, pgpkey_catalog_signature_target_name(signature->target_tag)) != 0 ||
            rt_write_char(1, ',') != 0 ||
            rt_write_uint(1, (unsigned long long)(signature->target_index + 1U)) != 0 ||
            rt_write_char(1, ',') != 0) return -1;
        if (signature->has_issuer_key_id) {
            if (pgpkey_sql_write_hex_value(1, signature->issuer_key_id, PGP_KEY_ID_SIZE) != 0) return -1;
        } else if (pgpkey_sql_write_text(1, "") != 0) return -1;
        if (rt_write_char(1, ',') != 0) return -1;
        if (signature->issuer_fingerprint_size != 0U) {
            if (pgpkey_sql_write_hex_value(1, signature->issuer_fingerprint, signature->issuer_fingerprint_size) != 0) return -1;
        } else if (pgpkey_sql_write_text(1, "") != 0) return -1;
        if (rt_write_char(1, ',') != 0 ||
            rt_write_uint(1, signature->created) != 0 ||
            rt_write_char(1, ',') != 0 ||
            rt_write_uint(1, signature->has_signature_expiration ? signature->signature_expiration_seconds : 0ULL) != 0 ||
            rt_write_line(1, ");") != 0) return -1;
    }
    return 0;
}

static int pgpkey_catalog_certificate_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpKeyCatalogSqlContext *ctx = (PgpKeyCatalogSqlContext *)ctx_ptr;

    if (pgpkey_catalog_write_certificate_sql(certificate, ctx->source) != 0) ctx->status = 1;
    return ctx->status != 0 ? -1 : 0;
}

static int pgpkey_catalog_collect_path(const char *path) {
    unsigned char *data = 0;
    size_t data_size = 0U;
    char error[PGPKEY_ERROR_CAPACITY];
    PgpKeyCatalogSqlContext ctx;
    int result = 0;

    if (load_openpgp_file(path, &data, &data_size, error, sizeof(error)) != 0) {
        pgpkey_write_error_path(error, path);
        return -1;
    }
    ctx.source = path;
    ctx.status = 0;
    if (pgp_for_each_certificate(data, data_size, pgpkey_catalog_certificate_callback, &ctx, error, sizeof(error)) != 0 || ctx.status != 0) {
        pgpkey_write_error_path(error, path);
        result = -1;
    }
    rt_free(data);
    return result;
}

static int pgpkey_resolve_store_paths(const char *store_path, char *keyring_path, size_t keyring_path_size, char *keystore_path, size_t keystore_path_size) {
    if (store_path == 0 || store_path[0] == '\0') return -1;
    if (rt_join_path(store_path, PGPKEY_STORE_KEYRING_NAME, keyring_path, keyring_path_size) != 0) return -1;
    if (rt_join_path(store_path, PGPKEY_STORE_INDEX_NAME, keystore_path, keystore_path_size) != 0) return -1;
    return 0;
}

static int pgpkey_sqs_write_text_value(int fd, const char *text) {
    char cleaned[PGPKEY_KEYSTORE_TEXT_SIZE];
    size_t index = 0U;

    if (text != 0) {
        while (text[index] != '\0' && index + 1U < sizeof(cleaned)) {
            unsigned char ch = (unsigned char)text[index];

            cleaned[index] = (ch < 0x20U || ch == 0x7fU) ? ' ' : (char)ch;
            index += 1U;
        }
    }
    cleaned[index] = '\0';
    return rt_write_uint(fd, index) != 0 || rt_write_char(fd, ':') != 0 || rt_write_cstr(fd, cleaned) != 0 ? -1 : 0;
}

static int pgpkey_sqs_write_hex_value(int fd, const unsigned char *data, size_t size) {
    if (rt_write_uint(fd, (unsigned long long)(size * 2U)) != 0 || rt_write_char(fd, ':') != 0) return -1;
    return write_hex_bytes(fd, data, size);
}

static int pgpkey_sqs_write_uint_value(int fd, unsigned long long value) {
    char buffer[32];

    rt_unsigned_to_string(value, buffer, sizeof(buffer));
    return pgpkey_sqs_write_text_value(fd, buffer);
}

static int pgpkey_sqs_write_integer_column(int fd, unsigned int index) {
    return rt_write_cstr(fd, "C ") != 0 || rt_write_uint(fd, index) != 0 || rt_write_cstr(fd, " 32 0:\n") != 0 ? -1 : 0;
}

static int pgpkey_store_write_index_schema(int fd) {
    return rt_write_cstr(fd, "SQS1\n") != 0 ||
           rt_write_line(fd, "T certs 11 fingerprint key_id version algorithm primary_uid created packet_count signature_count source cert_offset cert_length") != 0 ||
           pgpkey_sqs_write_integer_column(fd, 2U) != 0 ||
           pgpkey_sqs_write_integer_column(fd, 5U) != 0 ||
           pgpkey_sqs_write_integer_column(fd, 6U) != 0 ||
           pgpkey_sqs_write_integer_column(fd, 7U) != 0 ||
           pgpkey_sqs_write_integer_column(fd, 9U) != 0 ||
           pgpkey_sqs_write_integer_column(fd, 10U) != 0 ? -1 : 0;
}

static int pgpkey_store_write_index_table_after_certs(int fd) {
    return rt_write_line(fd, "E") != 0 ||
           rt_write_line(fd, "T keys 8 cert_fingerprint fingerprint key_id role version algorithm bits created") != 0 ||
           pgpkey_sqs_write_integer_column(fd, 4U) != 0 ||
           pgpkey_sqs_write_integer_column(fd, 6U) != 0 ||
           pgpkey_sqs_write_integer_column(fd, 7U) != 0 ? -1 : 0;
}

static int pgpkey_store_write_index_table_after_keys(int fd) {
    return rt_write_line(fd, "E") != 0 ||
           rt_write_line(fd, "T user_ids 4 cert_fingerprint uid_index uid primary_uid") != 0 ||
           pgpkey_sqs_write_integer_column(fd, 1U) != 0 ||
           pgpkey_sqs_write_integer_column(fd, 3U) != 0 ? -1 : 0;
}

static int pgpkey_store_write_index_table_after_user_ids(int fd) {
    return rt_write_line(fd, "E") != 0 ||
           rt_write_line(fd, "T signatures 9 cert_fingerprint packet_index signature_type target target_index issuer_key_id issuer_fingerprint created signature_expires") != 0 ||
           pgpkey_sqs_write_integer_column(fd, 1U) != 0 ||
           pgpkey_sqs_write_integer_column(fd, 4U) != 0 ||
           pgpkey_sqs_write_integer_column(fd, 7U) != 0 ||
           pgpkey_sqs_write_integer_column(fd, 8U) != 0 ? -1 : 0;
}

typedef struct {
    int fd;
    const char *source;
    int table;
    int status;
} PgpKeyStoreIndexContext;

static int pgpkey_store_write_key_row(int fd, const PgpPublicKeyInfo *key, const PgpPublicKeyInfo *primary, const char *role) {
    return rt_write_cstr(fd, "R ") != 0 ||
           pgpkey_sqs_write_hex_value(fd, primary->fingerprint, primary->fingerprint_size) != 0 ||
           pgpkey_sqs_write_hex_value(fd, key->fingerprint, key->fingerprint_size) != 0 ||
           pgpkey_sqs_write_hex_value(fd, key->key_id, PGP_KEY_ID_SIZE) != 0 ||
           pgpkey_sqs_write_text_value(fd, role) != 0 ||
           pgpkey_sqs_write_uint_value(fd, key->version) != 0 ||
           pgpkey_sqs_write_text_value(fd, pgp_public_key_algorithm_name(key->algorithm)) != 0 ||
           pgpkey_sqs_write_uint_value(fd, key->bits) != 0 ||
           pgpkey_sqs_write_uint_value(fd, key->created) != 0 ||
           rt_write_char(fd, '\n') != 0 ? -1 : 0;
}

static int pgpkey_store_write_index_certificate_row(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpKeyStoreIndexContext *ctx = (PgpKeyStoreIndexContext *)ctx_ptr;
    const char *primary_uid = pgpkey_catalog_primary_uid(certificate);
    size_t index;

    if (ctx->table == 0) {
        if (rt_write_cstr(ctx->fd, "R ") != 0 ||
            pgpkey_sqs_write_hex_value(ctx->fd, certificate->primary.fingerprint, certificate->primary.fingerprint_size) != 0 ||
            pgpkey_sqs_write_hex_value(ctx->fd, certificate->primary.key_id, PGP_KEY_ID_SIZE) != 0 ||
            pgpkey_sqs_write_uint_value(ctx->fd, certificate->primary.version) != 0 ||
            pgpkey_sqs_write_text_value(ctx->fd, pgp_public_key_algorithm_name(certificate->primary.algorithm)) != 0 ||
            pgpkey_sqs_write_text_value(ctx->fd, primary_uid) != 0 ||
            pgpkey_sqs_write_uint_value(ctx->fd, certificate->primary.created) != 0 ||
            pgpkey_sqs_write_uint_value(ctx->fd, certificate->packet_count) != 0 ||
            pgpkey_sqs_write_uint_value(ctx->fd, certificate->signature_count) != 0 ||
            pgpkey_sqs_write_text_value(ctx->fd, ctx->source) != 0 ||
            pgpkey_sqs_write_uint_value(ctx->fd, (unsigned long long)certificate->start_offset) != 0 ||
            pgpkey_sqs_write_uint_value(ctx->fd, (unsigned long long)(certificate->end_offset - certificate->start_offset)) != 0 ||
            rt_write_char(ctx->fd, '\n') != 0) ctx->status = 1;
    } else if (ctx->table == 1) {
        if (pgpkey_store_write_key_row(ctx->fd, &certificate->primary, &certificate->primary, "primary") != 0) ctx->status = 1;
        for (index = 0U; ctx->status == 0 && index < certificate->subkey_count; ++index) {
            if (pgpkey_store_write_key_row(ctx->fd, &certificate->subkeys[index], &certificate->primary, "subkey") != 0) ctx->status = 1;
        }
    } else if (ctx->table == 2) {
        for (index = 0U; ctx->status == 0 && index < certificate->user_id_count; ++index) {
            if (rt_write_cstr(ctx->fd, "R ") != 0 ||
                pgpkey_sqs_write_hex_value(ctx->fd, certificate->primary.fingerprint, certificate->primary.fingerprint_size) != 0 ||
                pgpkey_sqs_write_uint_value(ctx->fd, (unsigned long long)(index + 1U)) != 0 ||
                pgpkey_sqs_write_text_value(ctx->fd, certificate->user_ids[index]) != 0 ||
                pgpkey_sqs_write_uint_value(ctx->fd, rt_strcmp(certificate->user_ids[index], primary_uid) == 0 ? 1ULL : 0ULL) != 0 ||
                rt_write_char(ctx->fd, '\n') != 0) ctx->status = 1;
        }
    } else if (ctx->table == 3) {
        for (index = 0U; ctx->status == 0 && index < certificate->signature_info_count; ++index) {
            const PgpSignatureInfo *signature = &certificate->signatures[index];

            if (rt_write_cstr(ctx->fd, "R ") != 0 ||
                pgpkey_sqs_write_hex_value(ctx->fd, certificate->primary.fingerprint, certificate->primary.fingerprint_size) != 0 ||
                pgpkey_sqs_write_uint_value(ctx->fd, signature->packet_index) != 0 ||
                pgpkey_sqs_write_text_value(ctx->fd, pgp_signature_type_name(signature->signature_type)) != 0 ||
                pgpkey_sqs_write_text_value(ctx->fd, pgpkey_catalog_signature_target_name(signature->target_tag)) != 0 ||
                pgpkey_sqs_write_uint_value(ctx->fd, (unsigned long long)(signature->target_index + 1U)) != 0) ctx->status = 1;
            if (ctx->status == 0 && signature->has_issuer_key_id) {
                if (pgpkey_sqs_write_hex_value(ctx->fd, signature->issuer_key_id, PGP_KEY_ID_SIZE) != 0) ctx->status = 1;
            } else if (ctx->status == 0 && pgpkey_sqs_write_text_value(ctx->fd, "") != 0) ctx->status = 1;
            if (ctx->status == 0 && signature->issuer_fingerprint_size != 0U) {
                if (pgpkey_sqs_write_hex_value(ctx->fd, signature->issuer_fingerprint, signature->issuer_fingerprint_size) != 0) ctx->status = 1;
            } else if (ctx->status == 0 && pgpkey_sqs_write_text_value(ctx->fd, "") != 0) ctx->status = 1;
            if (ctx->status == 0 && (pgpkey_sqs_write_uint_value(ctx->fd, signature->created) != 0 ||
                pgpkey_sqs_write_uint_value(ctx->fd, signature->has_signature_expiration ? signature->signature_expiration_seconds : 0ULL) != 0 ||
                rt_write_char(ctx->fd, '\n') != 0)) ctx->status = 1;
        }
    }
    return ctx->status != 0 ? -1 : 0;
}

static int pgpkey_store_write_index_table(const unsigned char *data, size_t data_size, const char *source, int fd, int table) {
    PgpKeyStoreIndexContext ctx;
    char error[PGPKEY_ERROR_CAPACITY];

    ctx.fd = fd;
    ctx.source = source;
    ctx.table = table;
    ctx.status = 0;
    if (pgp_for_each_certificate(data, data_size, pgpkey_store_write_index_certificate_row, &ctx, error, sizeof(error)) != 0 || ctx.status != 0) {
        return -1;
    }
    return 0;
}

static int pgpkey_store_rebuild_index_file(const char *keyring_path, const char *index_path) {
    unsigned char *data = 0;
    size_t data_size = 0U;
    char error[PGPKEY_ERROR_CAPACITY];
    int fd;
    int result = -1;

    if (load_openpgp_file(keyring_path, &data, &data_size, error, sizeof(error)) != 0) {
        tool_write_error("pgpkey", error, ": keyring failed");
        return -1;
    }
    fd = platform_open_write(index_path, 0644U);
    if (fd < 0) {
        rt_free(data);
        return -1;
    }
    if (pgpkey_store_write_index_schema(fd) != 0) goto out;
    if (pgpkey_store_write_index_table(data, data_size, keyring_path, fd, 0) != 0) goto out;
    if (pgpkey_store_write_index_table_after_certs(fd) != 0) goto out;
    if (pgpkey_store_write_index_table(data, data_size, keyring_path, fd, 1) != 0) goto out;
    if (pgpkey_store_write_index_table_after_keys(fd) != 0) goto out;
    if (pgpkey_store_write_index_table(data, data_size, keyring_path, fd, 2) != 0) goto out;
    if (pgpkey_store_write_index_table_after_user_ids(fd) != 0) goto out;
    if (pgpkey_store_write_index_table(data, data_size, keyring_path, fd, 3) != 0) goto out;
    if (rt_write_line(fd, "E") != 0) goto out;
    result = 0;

out:
    if (platform_close(fd) != 0) result = -1;
    rt_free(data);
    return result;
}

static int pgpkey_store_write_empty_index(const char *index_path) {
    int fd = platform_open_write(index_path, 0644U);
    int result = -1;

    if (fd < 0) return -1;
    if (pgpkey_store_write_index_schema(fd) != 0) goto out;
    if (pgpkey_store_write_index_table_after_certs(fd) != 0) goto out;
    if (pgpkey_store_write_index_table_after_keys(fd) != 0) goto out;
    if (pgpkey_store_write_index_table_after_user_ids(fd) != 0) goto out;
    if (rt_write_line(fd, "E") != 0) goto out;
    result = 0;

out:
    if (platform_close(fd) != 0) result = -1;
    return result;
}

static int command_catalog_sql(const PgpKeyOptions *options, int argc, char **argv, int argi) {
    int status = 0;

    if (pgpkey_catalog_write_schema_sql() != 0) return 1;
    if (argi >= argc) {
        return pgpkey_catalog_collect_path(options->keyring_path) == 0 ? 0 : 1;
    }
    while (argi < argc) {
        if (pgpkey_catalog_collect_path(argv[argi]) != 0) status = 1;
        argi += 1;
    }
    return status;
}

static int keyring_contains(const char *keyring_path, const char *selector) {
    unsigned char *data = 0;
    size_t data_size = 0U;
    char error[PGPKEY_ERROR_CAPACITY];
    PgpKeyFindContext ctx;

    if (tool_read_all_input(keyring_path, &data, &data_size) != 0) {
        return 0;
    }
    ctx.selector = selector;
    ctx.found = 0;
    (void)pgp_for_each_certificate(data, data_size, find_certificate_callback, &ctx, error, sizeof(error));
    rt_free(data);
    return ctx.found;
}

static void pgpkey_fingerprint_to_text(const PgpPublicKeyInfo *key, char out[PGP_FINGERPRINT_MAX_SIZE * 2U + 1U]) {
    static const char hex[] = "0123456789abcdef";
    size_t offset = 0U;
    size_t index;

    for (index = 0U; index < key->fingerprint_size && index < PGP_FINGERPRINT_MAX_SIZE; ++index) {
        out[offset++] = hex[(key->fingerprint[index] >> 4U) & 0x0fU];
        out[offset++] = hex[key->fingerprint[index] & 0x0fU];
    }
    out[offset] = '\0';
}

static int import_scan_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpKeyImportScanContext *ctx = (PgpKeyImportScanContext *)ctx_ptr;

    if (certificate->primary.tag == 5U) ctx->secret_found = 1;
    else if (certificate->primary.tag == 6U) ctx->public_count += 1;
    return 0;
}

static int import_certificate_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpKeyImportContext *ctx = (PgpKeyImportContext *)ctx_ptr;
    char fingerprint[PGP_FINGERPRINT_MAX_SIZE * 2U + 1U];
    int fd;

    if (certificate->primary.tag != 6U) return 0;
    pgpkey_fingerprint_to_text(&certificate->primary, fingerprint);
    if (fingerprint[0] == '\0') return 0;
    if (keyring_contains(ctx->options->keyring_path, fingerprint)) {
        if (rt_write_cstr(1, "unchanged: ") != 0 || rt_write_line(1, fingerprint) != 0) ctx->status = 1;
        return 0;
    }
    fd = platform_open_append(ctx->options->keyring_path, 0600U);
    if (fd < 0 || certificate->end_offset < certificate->start_offset || write_all_fd(fd, ctx->data + certificate->start_offset, certificate->end_offset - certificate->start_offset) != 0 || platform_close(fd) != 0) {
        if (fd >= 0) (void)platform_close(fd);
        tool_write_error("pgpkey", "cannot write keyring: ", ctx->options->keyring_path);
        ctx->status = 1;
        return 0;
    }
    if (ctx->options->json) {
        if (tool_json_begin_event(1, "pgpkey", "stdout", "import") != 0 ||
            rt_write_cstr(1, ",\"data\":{\"fingerprint\":") != 0 || tool_json_write_string(1, fingerprint) != 0 ||
            rt_write_cstr(1, ",\"keyring\":") != 0 || tool_json_write_string(1, ctx->options->keyring_path) != 0 || rt_write_char(1, '}') != 0 || tool_json_end_event(1) != 0) ctx->status = 1;
    } else if (rt_write_cstr(1, "imported: ") != 0 || rt_write_line(1, fingerprint) != 0) {
        ctx->status = 1;
    }
    return 0;
}

static int write_all_fd(int fd, const unsigned char *data, size_t size) {
    size_t written = 0U;

    while (written < size) {
        long chunk = platform_write(fd, data + written, size - written);
        if (chunk <= 0) return -1;
        written += (size_t)chunk;
    }
    return 0;
}

static void generated_key_free(PgpKeyGeneratedKey *generated) {
    crypto_secure_bzero(generated->primary_seed, sizeof(generated->primary_seed));
    crypto_secure_bzero(generated->primary_public, sizeof(generated->primary_public));
    crypto_secure_bzero(generated->encryption_seed, sizeof(generated->encryption_seed));
    crypto_secure_bzero(generated->encryption_public, sizeof(generated->encryption_public));
    pgpkey_buffer_free(&generated->public_body);
    pgpkey_buffer_free(&generated->secret_body);
    pgpkey_buffer_free(&generated->encryption_public_body);
    pgpkey_buffer_free(&generated->encryption_secret_body);
    pgpkey_buffer_free(&generated->user_id_packet);
    pgpkey_buffer_free(&generated->signature_packet);
    pgpkey_buffer_free(&generated->direct_signature_packet);
    pgpkey_buffer_free(&generated->user_signature_packet);
    pgpkey_buffer_free(&generated->subkey_signature_packet);
    pgpkey_buffer_free(&generated->public_certificate);
    pgpkey_buffer_free(&generated->secret_certificate);
    rt_memset(&generated->primary_info, 0, sizeof(generated->primary_info));
    rt_memset(&generated->encryption_info, 0, sizeof(generated->encryption_info));
}

static int parse_expiration_duration(const char *text, unsigned long long *seconds_out) {
    unsigned long long value = 0ULL;
    unsigned long long multiplier = 1ULL;
    size_t length;
    size_t digit_count;
    size_t index;

    if (text == 0 || text[0] == '\0') return -1;
    if (rt_strcmp(text, "never") == 0 || rt_strcmp(text, "none") == 0 || rt_strcmp(text, "0") == 0) {
        *seconds_out = 0ULL;
        return 0;
    }
    length = rt_strlen(text);
    digit_count = length;
    if (length != 0U) {
        char suffix = text[length - 1U];

        if (suffix == 's') { multiplier = 1ULL; digit_count -= 1U; }
        else if (suffix == 'd') { multiplier = 86400ULL; digit_count -= 1U; }
        else if (suffix == 'w') { multiplier = 7ULL * 86400ULL; digit_count -= 1U; }
        else if (suffix == 'm') { multiplier = 30ULL * 86400ULL; digit_count -= 1U; }
        else if (suffix == 'y') { multiplier = 365ULL * 86400ULL; digit_count -= 1U; }
    }
    if (digit_count == 0U) return -1;
    for (index = 0U; index < digit_count; ++index) {
        if (text[index] < '0' || text[index] > '9') return -1;
        if (value > 0xffffffffULL / 10ULL) return -1;
        value = value * 10ULL + (unsigned long long)(text[index] - '0');
    }
    if (value > 0xffffffffULL / multiplier) return -1;
    *seconds_out = value * multiplier;
    return 0;
}

static int build_generated_public_body(PgpKeyGeneratedKey *generated, unsigned long long created, char *error, size_t error_size) {
    static const unsigned char ed25519_oid[] = { 0x2bU, 0x06U, 0x01U, 0x04U, 0x01U, 0xdaU, 0x47U, 0x0fU, 0x01U };
    unsigned char point[PGPKEY_ED25519_KEY_SIZE + 1U];

    point[0] = 0x40U;
    memcpy(point + 1U, generated->primary_public, PGPKEY_ED25519_KEY_SIZE);
    if (pgpkey_buffer_append_byte(&generated->public_body, 4U) != 0 ||
        pgpkey_buffer_append_u32_be(&generated->public_body, created) != 0 ||
        pgpkey_buffer_append_byte(&generated->public_body, 22U) != 0 ||
        pgpkey_buffer_append_byte(&generated->public_body, (unsigned int)sizeof(ed25519_oid)) != 0 ||
        pgpkey_buffer_append_data(&generated->public_body, ed25519_oid, sizeof(ed25519_oid)) != 0 ||
        pgpkey_buffer_append_opaque_mpi(&generated->public_body, point, sizeof(point), 263U) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing public key packet");
        return -1;
    }
    if (pgp_parse_public_key_packet(&generated->primary_info, 6U, generated->public_body.data, generated->public_body.size, error, error_size) != 0) return -1;
    return 0;
}

static int build_generated_secret_body(PgpKeyGeneratedKey *generated, char *error, size_t error_size) {
    PgpKeyBuffer secret_mpi;
    unsigned int checksum = 0U;
    size_t index;

    rt_memset(&secret_mpi, 0, sizeof(secret_mpi));
    if (pgpkey_buffer_append_opaque_mpi(&secret_mpi, generated->primary_seed, sizeof(generated->primary_seed), 256U) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing secret key material");
        return -1;
    }
    for (index = 0U; index < secret_mpi.size; ++index) {
        checksum = (checksum + secret_mpi.data[index]) & 0xffffU;
    }
    if (pgpkey_buffer_append_data(&generated->secret_body, generated->public_body.data, generated->public_body.size) != 0 ||
        pgpkey_buffer_append_byte(&generated->secret_body, 0U) != 0 ||
        pgpkey_buffer_append_data(&generated->secret_body, secret_mpi.data, secret_mpi.size) != 0 ||
        pgpkey_buffer_append_u16_be(&generated->secret_body, checksum) != 0) {
        pgpkey_buffer_free(&secret_mpi);
        pgpkey_set_error(error, error_size, "out of memory while writing secret key packet");
        return -1;
    }
    pgpkey_buffer_free(&secret_mpi);
    return 0;
}

static int build_generated_encryption_public_body(PgpKeyGeneratedKey *generated, unsigned long long created, char *error, size_t error_size) {
    static const unsigned char curve25519_oid[] = { 0x2bU, 0x06U, 0x01U, 0x04U, 0x01U, 0x97U, 0x55U, 0x01U, 0x05U, 0x01U };
    unsigned char point[PGPKEY_ED25519_KEY_SIZE + 1U];
    unsigned char kdf[] = { 0x01U, 0x08U, 0x09U };

    point[0] = 0x40U;
    memcpy(point + 1U, generated->encryption_public, PGPKEY_ED25519_KEY_SIZE);
    if (pgpkey_buffer_append_byte(&generated->encryption_public_body, 4U) != 0 ||
        pgpkey_buffer_append_u32_be(&generated->encryption_public_body, created) != 0 ||
        pgpkey_buffer_append_byte(&generated->encryption_public_body, 18U) != 0 ||
        pgpkey_buffer_append_byte(&generated->encryption_public_body, (unsigned int)sizeof(curve25519_oid)) != 0 ||
        pgpkey_buffer_append_data(&generated->encryption_public_body, curve25519_oid, sizeof(curve25519_oid)) != 0 ||
        pgpkey_buffer_append_opaque_mpi(&generated->encryption_public_body, point, sizeof(point), 263U) != 0 ||
        pgpkey_buffer_append_byte(&generated->encryption_public_body, (unsigned int)sizeof(kdf)) != 0 ||
        pgpkey_buffer_append_data(&generated->encryption_public_body, kdf, sizeof(kdf)) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing encryption subkey packet");
        return -1;
    }
    if (pgp_parse_public_key_packet(&generated->encryption_info, 14U, generated->encryption_public_body.data, generated->encryption_public_body.size, error, error_size) != 0) return -1;
    return 0;
}

static int build_generated_encryption_secret_body(PgpKeyGeneratedKey *generated, char *error, size_t error_size) {
    PgpKeyBuffer secret_mpi;
    unsigned int checksum = 0U;
    size_t index;

    rt_memset(&secret_mpi, 0, sizeof(secret_mpi));
    if (pgpkey_buffer_append_opaque_mpi(&secret_mpi, generated->encryption_seed, sizeof(generated->encryption_seed), 256U) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing encryption secret material");
        return -1;
    }
    for (index = 0U; index < secret_mpi.size; ++index) checksum = (checksum + secret_mpi.data[index]) & 0xffffU;
    if (pgpkey_buffer_append_data(&generated->encryption_secret_body, generated->encryption_public_body.data, generated->encryption_public_body.size) != 0 ||
        pgpkey_buffer_append_byte(&generated->encryption_secret_body, 0U) != 0 ||
        pgpkey_buffer_append_data(&generated->encryption_secret_body, secret_mpi.data, secret_mpi.size) != 0 ||
        pgpkey_buffer_append_u16_be(&generated->encryption_secret_body, checksum) != 0) {
        pgpkey_buffer_free(&secret_mpi);
        pgpkey_set_error(error, error_size, "out of memory while writing encryption secret packet");
        return -1;
    }
    pgpkey_buffer_free(&secret_mpi);
    return 0;
}

static int append_generated_self_signature(PgpKeyGeneratedKey *generated, unsigned long long created, unsigned long long expiration_seconds, char *error, size_t error_size) {
    PgpKeyBuffer hashed;
    PgpKeyBuffer unhashed;
    PgpKeyBuffer hash_part;
    PgpKeyBuffer signature_body;
    CryptoSha512Context sha512;
    unsigned char digest[CRYPTO_SHA512_DIGEST_SIZE];
    unsigned char signature[PGPKEY_ED25519_SIGNATURE_SIZE];
    unsigned char body[PGP_FINGERPRINT_MAX_SIZE + 1U];
    unsigned char prefix[5];
    unsigned char trailer[6];
    unsigned char key_flags[] = { 0x03U };
    unsigned char preferred_symmetric[] = { 9U, 8U, 7U };
    unsigned char preferred_hash[] = { 10U, 8U, 9U };
    unsigned char preferred_compression[] = { 2U, 1U, 0U };
    unsigned char features[] = { 0x01U };
    unsigned char primary_user_id[] = { 0x01U };

    rt_memset(&hashed, 0, sizeof(hashed));
    rt_memset(&unhashed, 0, sizeof(unhashed));
    rt_memset(&hash_part, 0, sizeof(hash_part));
    rt_memset(&signature_body, 0, sizeof(signature_body));

    if (pgpkey_buffer_append_signature_subpacket_u32(&hashed, 2U, created) != 0 ||
        pgpkey_buffer_append_signature_subpacket(&hashed, 27U, key_flags, sizeof(key_flags)) != 0 ||
        pgpkey_buffer_append_signature_subpacket_u32(&hashed, 9U, expiration_seconds) != 0 ||
        pgpkey_buffer_append_signature_subpacket(&hashed, 11U, preferred_symmetric, sizeof(preferred_symmetric)) != 0 ||
        pgpkey_buffer_append_signature_subpacket(&hashed, 21U, preferred_hash, sizeof(preferred_hash)) != 0 ||
        pgpkey_buffer_append_signature_subpacket(&hashed, 22U, preferred_compression, sizeof(preferred_compression)) != 0 ||
        pgpkey_buffer_append_signature_subpacket(&hashed, 30U, features, sizeof(features)) != 0 ||
        pgpkey_buffer_append_signature_subpacket(&hashed, 25U, primary_user_id, sizeof(primary_user_id)) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing self-signature subpackets");
        goto fail;
    }

    if (pgpkey_buffer_append_signature_subpacket(&unhashed, 16U, generated->primary_info.key_id, PGP_KEY_ID_SIZE) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing issuer key ID");
        goto fail;
    }
    body[0] = 4U;
    memcpy(body + 1U, generated->primary_info.fingerprint, generated->primary_info.fingerprint_size);
    if (pgpkey_buffer_append_signature_subpacket(&unhashed, 33U, body, generated->primary_info.fingerprint_size + 1U) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing issuer fingerprint");
        goto fail;
    }

    if (pgpkey_buffer_append_byte(&hash_part, 4U) != 0 ||
        pgpkey_buffer_append_byte(&hash_part, 0x13U) != 0 ||
        pgpkey_buffer_append_byte(&hash_part, 22U) != 0 ||
        pgpkey_buffer_append_byte(&hash_part, 10U) != 0 ||
        pgpkey_buffer_append_u16_be(&hash_part, (unsigned int)hashed.size) != 0 ||
        pgpkey_buffer_append_data(&hash_part, hashed.data, hashed.size) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing self-signature hash data");
        goto fail;
    }

    crypto_sha512_init(&sha512);
    prefix[0] = 0x99U;
    prefix[1] = (unsigned char)((generated->public_body.size >> 8U) & 0xffU);
    prefix[2] = (unsigned char)(generated->public_body.size & 0xffU);
    crypto_sha512_update(&sha512, prefix, 3U);
    crypto_sha512_update(&sha512, generated->public_body.data, generated->public_body.size);
    prefix[0] = 0xb4U;
    prefix[1] = (unsigned char)((generated->user_id_packet.size >> 24U) & 0xffU);
    prefix[2] = (unsigned char)((generated->user_id_packet.size >> 16U) & 0xffU);
    prefix[3] = (unsigned char)((generated->user_id_packet.size >> 8U) & 0xffU);
    prefix[4] = (unsigned char)(generated->user_id_packet.size & 0xffU);
    crypto_sha512_update(&sha512, prefix, sizeof(prefix));
    crypto_sha512_update(&sha512, generated->user_id_packet.data, generated->user_id_packet.size);
    crypto_sha512_update(&sha512, hash_part.data, hash_part.size);
    trailer[0] = 4U;
    trailer[1] = 0xffU;
    trailer[2] = (unsigned char)((hash_part.size >> 24U) & 0xffU);
    trailer[3] = (unsigned char)((hash_part.size >> 16U) & 0xffU);
    trailer[4] = (unsigned char)((hash_part.size >> 8U) & 0xffU);
    trailer[5] = (unsigned char)(hash_part.size & 0xffU);
    crypto_sha512_update(&sha512, trailer, sizeof(trailer));
    crypto_sha512_final(&sha512, digest);

    if (crypto_ed25519_sign(signature, digest, sizeof(digest), generated->primary_seed, generated->primary_public) != 0) {
        pgpkey_set_error(error, error_size, "Ed25519 self-signature failed");
        goto fail;
    }

    if (pgpkey_buffer_append_data(&signature_body, hash_part.data, hash_part.size) != 0 ||
        pgpkey_buffer_append_u16_be(&signature_body, (unsigned int)unhashed.size) != 0 ||
        pgpkey_buffer_append_data(&signature_body, unhashed.data, unhashed.size) != 0 ||
        pgpkey_buffer_append_byte(&signature_body, digest[0]) != 0 ||
        pgpkey_buffer_append_byte(&signature_body, digest[1]) != 0 ||
        pgpkey_buffer_append_opaque_mpi(&signature_body, signature, 32U, 256U) != 0 ||
        pgpkey_buffer_append_opaque_mpi(&signature_body, signature + 32U, 32U, 256U) != 0 ||
        pgpkey_buffer_append_packet(&generated->signature_packet, 2U, &signature_body) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing self-signature packet");
        goto fail;
    }

    pgpkey_buffer_free(&hashed);
    pgpkey_buffer_free(&unhashed);
    pgpkey_buffer_free(&hash_part);
    pgpkey_buffer_free(&signature_body);
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(signature, sizeof(signature));
    return 0;

fail:
    pgpkey_buffer_free(&hashed);
    pgpkey_buffer_free(&unhashed);
    pgpkey_buffer_free(&hash_part);
    pgpkey_buffer_free(&signature_body);
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(signature, sizeof(signature));
    return -1;
}

static int append_generated_subkey_binding_signature(PgpKeyGeneratedKey *generated, unsigned long long created, unsigned long long expiration_seconds, char *error, size_t error_size) {
    PgpKeyBuffer hashed;
    PgpKeyBuffer unhashed;
    PgpKeyBuffer hash_part;
    PgpKeyBuffer signature_body;
    CryptoSha512Context sha512;
    unsigned char digest[CRYPTO_SHA512_DIGEST_SIZE];
    unsigned char signature[PGPKEY_ED25519_SIGNATURE_SIZE];
    unsigned char body[PGP_FINGERPRINT_MAX_SIZE + 1U];
    unsigned char prefix[3];
    unsigned char trailer[6];
    unsigned char key_flags[] = { 0x0cU };

    rt_memset(&hashed, 0, sizeof(hashed));
    rt_memset(&unhashed, 0, sizeof(unhashed));
    rt_memset(&hash_part, 0, sizeof(hash_part));
    rt_memset(&signature_body, 0, sizeof(signature_body));
    if (pgpkey_buffer_append_signature_subpacket_u32(&hashed, 2U, created) != 0 ||
        pgpkey_buffer_append_signature_subpacket(&hashed, 27U, key_flags, sizeof(key_flags)) != 0 ||
        pgpkey_buffer_append_signature_subpacket_u32(&hashed, 9U, expiration_seconds) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing subkey binding subpackets");
        goto fail;
    }
    if (pgpkey_buffer_append_signature_subpacket(&unhashed, 16U, generated->primary_info.key_id, PGP_KEY_ID_SIZE) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing subkey binding issuer key ID");
        goto fail;
    }
    body[0] = 4U;
    memcpy(body + 1U, generated->primary_info.fingerprint, generated->primary_info.fingerprint_size);
    if (pgpkey_buffer_append_signature_subpacket(&unhashed, 33U, body, generated->primary_info.fingerprint_size + 1U) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing subkey binding issuer fingerprint");
        goto fail;
    }
    if (pgpkey_buffer_append_byte(&hash_part, 4U) != 0 ||
        pgpkey_buffer_append_byte(&hash_part, 0x18U) != 0 ||
        pgpkey_buffer_append_byte(&hash_part, 22U) != 0 ||
        pgpkey_buffer_append_byte(&hash_part, 10U) != 0 ||
        pgpkey_buffer_append_u16_be(&hash_part, (unsigned int)hashed.size) != 0 ||
        pgpkey_buffer_append_data(&hash_part, hashed.data, hashed.size) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing subkey binding hash data");
        goto fail;
    }
    crypto_sha512_init(&sha512);
    prefix[0] = 0x99U;
    prefix[1] = (unsigned char)((generated->public_body.size >> 8U) & 0xffU);
    prefix[2] = (unsigned char)(generated->public_body.size & 0xffU);
    crypto_sha512_update(&sha512, prefix, sizeof(prefix));
    crypto_sha512_update(&sha512, generated->public_body.data, generated->public_body.size);
    prefix[0] = 0x99U;
    prefix[1] = (unsigned char)((generated->encryption_public_body.size >> 8U) & 0xffU);
    prefix[2] = (unsigned char)(generated->encryption_public_body.size & 0xffU);
    crypto_sha512_update(&sha512, prefix, sizeof(prefix));
    crypto_sha512_update(&sha512, generated->encryption_public_body.data, generated->encryption_public_body.size);
    crypto_sha512_update(&sha512, hash_part.data, hash_part.size);
    trailer[0] = 4U;
    trailer[1] = 0xffU;
    trailer[2] = (unsigned char)((hash_part.size >> 24U) & 0xffU);
    trailer[3] = (unsigned char)((hash_part.size >> 16U) & 0xffU);
    trailer[4] = (unsigned char)((hash_part.size >> 8U) & 0xffU);
    trailer[5] = (unsigned char)(hash_part.size & 0xffU);
    crypto_sha512_update(&sha512, trailer, sizeof(trailer));
    crypto_sha512_final(&sha512, digest);
    if (crypto_ed25519_sign(signature, digest, sizeof(digest), generated->primary_seed, generated->primary_public) != 0) {
        pgpkey_set_error(error, error_size, "Ed25519 subkey binding signature failed");
        goto fail;
    }
    if (pgpkey_buffer_append_data(&signature_body, hash_part.data, hash_part.size) != 0 ||
        pgpkey_buffer_append_u16_be(&signature_body, (unsigned int)unhashed.size) != 0 ||
        pgpkey_buffer_append_data(&signature_body, unhashed.data, unhashed.size) != 0 ||
        pgpkey_buffer_append_byte(&signature_body, digest[0]) != 0 ||
        pgpkey_buffer_append_byte(&signature_body, digest[1]) != 0 ||
        pgpkey_buffer_append_opaque_mpi(&signature_body, signature, 32U, 256U) != 0 ||
        pgpkey_buffer_append_opaque_mpi(&signature_body, signature + 32U, 32U, 256U) != 0 ||
        pgpkey_buffer_append_packet(&generated->subkey_signature_packet, 2U, &signature_body) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing subkey binding signature packet");
        goto fail;
    }
    pgpkey_buffer_free(&hashed);
    pgpkey_buffer_free(&unhashed);
    pgpkey_buffer_free(&hash_part);
    pgpkey_buffer_free(&signature_body);
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(signature, sizeof(signature));
    return 0;

fail:
    pgpkey_buffer_free(&hashed);
    pgpkey_buffer_free(&unhashed);
    pgpkey_buffer_free(&hash_part);
    pgpkey_buffer_free(&signature_body);
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(signature, sizeof(signature));
    return -1;
}

static int build_generated_key_legacy(const PgpKeyGenerateOptions *options, PgpKeyGeneratedKey *generated, char *error, size_t error_size) {
    unsigned long long created = current_epoch_or_zero();

    rt_memset(generated, 0, sizeof(*generated));
    if (created == 0ULL) created = 1ULL;
    if (platform_random_bytes(generated->primary_seed, sizeof(generated->primary_seed)) != 0) {
        pgpkey_set_error(error, error_size, "cannot read random bytes");
        return -1;
    }
    if (platform_random_bytes(generated->encryption_seed, sizeof(generated->encryption_seed)) != 0) {
        pgpkey_set_error(error, error_size, "cannot read random bytes for encryption subkey");
        return -1;
    }
    if (crypto_ed25519_public_key_from_seed(generated->primary_public, generated->primary_seed) != 0) {
        pgpkey_set_error(error, error_size, "Ed25519 public key generation failed");
        return -1;
    }
    if (crypto_x25519_scalarmult_base(generated->encryption_public, generated->encryption_seed) != 0) {
        pgpkey_set_error(error, error_size, "X25519 public subkey generation failed");
        return -1;
    }
    if (build_generated_public_body(generated, created, error, error_size) != 0) return -1;
    if (build_generated_secret_body(generated, error, error_size) != 0) return -1;
    if (build_generated_encryption_public_body(generated, created, error, error_size) != 0) return -1;
    if (build_generated_encryption_secret_body(generated, error, error_size) != 0) return -1;
    if (pgpkey_buffer_append_data(&generated->user_id_packet, (const unsigned char *)options->user_id, rt_strlen(options->user_id)) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing user ID packet");
        return -1;
    }
    if (append_generated_self_signature(generated, created, options->key_expiration_seconds, error, error_size) != 0) return -1;
    if (append_generated_subkey_binding_signature(generated, created, options->key_expiration_seconds, error, error_size) != 0) return -1;
    if (pgpkey_buffer_append_packet(&generated->public_certificate, 6U, &generated->public_body) != 0 ||
        pgpkey_buffer_append_packet(&generated->public_certificate, 13U, &generated->user_id_packet) != 0 ||
        pgpkey_buffer_append_data(&generated->public_certificate, generated->signature_packet.data, generated->signature_packet.size) != 0 ||
        pgpkey_buffer_append_packet(&generated->public_certificate, 14U, &generated->encryption_public_body) != 0 ||
        pgpkey_buffer_append_data(&generated->public_certificate, generated->subkey_signature_packet.data, generated->subkey_signature_packet.size) != 0 ||
        pgpkey_buffer_append_packet(&generated->secret_certificate, 5U, &generated->secret_body) != 0 ||
        pgpkey_buffer_append_packet(&generated->secret_certificate, 13U, &generated->user_id_packet) != 0 ||
        pgpkey_buffer_append_data(&generated->secret_certificate, generated->signature_packet.data, generated->signature_packet.size) != 0 ||
        pgpkey_buffer_append_packet(&generated->secret_certificate, 7U, &generated->encryption_secret_body) != 0 ||
        pgpkey_buffer_append_data(&generated->secret_certificate, generated->subkey_signature_packet.data, generated->subkey_signature_packet.size) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing generated certificates");
        return -1;
    }
    return 0;
}

static int build_generated_public_body_v6(PgpKeyGeneratedKey *generated, unsigned long long created, char *error, size_t error_size) {
    if (pgpkey_buffer_append_byte(&generated->public_body, 6U) != 0 ||
        pgpkey_buffer_append_u32_be(&generated->public_body, created) != 0 ||
        pgpkey_buffer_append_byte(&generated->public_body, 27U) != 0 ||
        pgpkey_buffer_append_u32_be(&generated->public_body, PGPKEY_ED25519_KEY_SIZE) != 0 ||
        pgpkey_buffer_append_data(&generated->public_body, generated->primary_public, PGPKEY_ED25519_KEY_SIZE) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing v6 public key packet");
        return -1;
    }
    return pgp_parse_public_key_packet(&generated->primary_info, 6U, generated->public_body.data, generated->public_body.size, error, error_size);
}

static int build_generated_secret_body_v6(PgpKeyGeneratedKey *generated, char *error, size_t error_size) {
    if (pgpkey_buffer_append_data(&generated->secret_body, generated->public_body.data, generated->public_body.size) != 0 ||
        pgpkey_buffer_append_byte(&generated->secret_body, 0U) != 0 ||
        pgpkey_buffer_append_data(&generated->secret_body, generated->primary_seed, PGPKEY_ED25519_KEY_SIZE) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing v6 secret key packet");
        return -1;
    }
    return 0;
}

static int build_generated_encryption_public_body_v6(PgpKeyGeneratedKey *generated, unsigned long long created, char *error, size_t error_size) {
    if (pgpkey_buffer_append_byte(&generated->encryption_public_body, 6U) != 0 ||
        pgpkey_buffer_append_u32_be(&generated->encryption_public_body, created) != 0 ||
        pgpkey_buffer_append_byte(&generated->encryption_public_body, 25U) != 0 ||
        pgpkey_buffer_append_u32_be(&generated->encryption_public_body, PGPKEY_X25519_KEY_SIZE) != 0 ||
        pgpkey_buffer_append_data(&generated->encryption_public_body, generated->encryption_public, PGPKEY_X25519_KEY_SIZE) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing v6 encryption subkey packet");
        return -1;
    }
    return pgp_parse_public_key_packet(&generated->encryption_info, 14U, generated->encryption_public_body.data, generated->encryption_public_body.size, error, error_size);
}

static int build_generated_encryption_secret_body_v6(PgpKeyGeneratedKey *generated, char *error, size_t error_size) {
    if (pgpkey_buffer_append_data(&generated->encryption_secret_body, generated->encryption_public_body.data, generated->encryption_public_body.size) != 0 ||
        pgpkey_buffer_append_byte(&generated->encryption_secret_body, 0U) != 0 ||
        pgpkey_buffer_append_data(&generated->encryption_secret_body, generated->encryption_seed, PGPKEY_X25519_KEY_SIZE) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing v6 encryption secret subkey packet");
        return -1;
    }
    return 0;
}

static void pgpkey_sha512_update_v6_key(CryptoSha512Context *sha512, const PgpKeyBuffer *body) {
    unsigned char prefix[5];

    prefix[0] = 0x9bU;
    prefix[1] = (unsigned char)((body->size >> 24U) & 0xffU);
    prefix[2] = (unsigned char)((body->size >> 16U) & 0xffU);
    prefix[3] = (unsigned char)((body->size >> 8U) & 0xffU);
    prefix[4] = (unsigned char)(body->size & 0xffU);
    crypto_sha512_update(sha512, prefix, sizeof(prefix));
    crypto_sha512_update(sha512, body->data, body->size);
}

static int append_generated_signature_v6(PgpKeyGeneratedKey *generated, PgpKeyBuffer *packet_out, unsigned int signature_type, const unsigned char *target_body, size_t target_body_size, unsigned long long created, unsigned long long expiration_seconds, const unsigned char *key_flags, size_t key_flags_size, int include_primary_user_id, int include_preferences, char *error, size_t error_size) {
    PgpKeyBuffer hashed;
    PgpKeyBuffer unhashed;
    PgpKeyBuffer hash_part;
    PgpKeyBuffer signature_body;
    CryptoSha512Context sha512;
    unsigned char digest[CRYPTO_SHA512_DIGEST_SIZE];
    unsigned char signature[PGPKEY_ED25519_SIGNATURE_SIZE];
    unsigned char salt[32];
    unsigned char issuer_fpr[PGP_FINGERPRINT_MAX_SIZE + 1U];
    unsigned char preferred_symmetric[] = { 9U, 7U };
    unsigned char preferred_hash[] = { 10U, 8U };
    unsigned char preferred_compression[] = { 0U };
    unsigned char features[] = { 0x09U };
    unsigned char preferred_aead[] = { 9U, 3U, 7U, 3U };
    unsigned char primary_user_id[] = { 0x01U };
    unsigned char trailer[6];
    int result = -1;

    rt_memset(&hashed, 0, sizeof(hashed));
    rt_memset(&unhashed, 0, sizeof(unhashed));
    rt_memset(&hash_part, 0, sizeof(hash_part));
    rt_memset(&signature_body, 0, sizeof(signature_body));
    if (platform_random_bytes(salt, sizeof(salt)) != 0) {
        pgpkey_set_error(error, error_size, "cannot read random bytes for v6 signature salt");
        goto cleanup;
    }
    issuer_fpr[0] = 6U;
    memcpy(issuer_fpr + 1U, generated->primary_info.fingerprint, generated->primary_info.fingerprint_size);
    if (pgpkey_buffer_append_signature_subpacket_u32(&hashed, 0x82U, created) != 0 ||
        (key_flags_size != 0U && pgpkey_buffer_append_signature_subpacket(&hashed, 0x9bU, key_flags, key_flags_size) != 0) ||
        pgpkey_buffer_append_signature_subpacket(&hashed, 33U, issuer_fpr, generated->primary_info.fingerprint_size + 1U) != 0) goto cleanup;
    if (expiration_seconds != 0ULL && pgpkey_buffer_append_signature_subpacket_u32(&hashed, 0x89U, expiration_seconds) != 0) goto cleanup;
    if (include_primary_user_id && pgpkey_buffer_append_signature_subpacket(&hashed, 25U, primary_user_id, sizeof(primary_user_id)) != 0) goto cleanup;
    if (include_preferences) {
        if (pgpkey_buffer_append_signature_subpacket(&hashed, 11U, preferred_symmetric, sizeof(preferred_symmetric)) != 0 ||
            pgpkey_buffer_append_signature_subpacket(&hashed, 21U, preferred_hash, sizeof(preferred_hash)) != 0 ||
            pgpkey_buffer_append_signature_subpacket(&hashed, 22U, preferred_compression, sizeof(preferred_compression)) != 0 ||
            pgpkey_buffer_append_signature_subpacket(&hashed, 30U, features, sizeof(features)) != 0 ||
            pgpkey_buffer_append_signature_subpacket(&hashed, 39U, preferred_aead, sizeof(preferred_aead)) != 0) goto cleanup;
    }
    if (pgpkey_buffer_append_byte(&hash_part, 6U) != 0 ||
        pgpkey_buffer_append_byte(&hash_part, signature_type) != 0 ||
        pgpkey_buffer_append_byte(&hash_part, 27U) != 0 ||
        pgpkey_buffer_append_byte(&hash_part, 10U) != 0 ||
        pgpkey_buffer_append_u32_be(&hash_part, hashed.size) != 0 ||
        pgpkey_buffer_append_data(&hash_part, hashed.data, hashed.size) != 0) goto cleanup;

    crypto_sha512_init(&sha512);
    crypto_sha512_update(&sha512, salt, sizeof(salt));
    pgpkey_sha512_update_v6_key(&sha512, &generated->public_body);
    if (signature_type >= 0x10U && signature_type <= 0x13U) {
        unsigned char prefix[5];

        prefix[0] = 0xb4U;
        prefix[1] = (unsigned char)((target_body_size >> 24U) & 0xffU);
        prefix[2] = (unsigned char)((target_body_size >> 16U) & 0xffU);
        prefix[3] = (unsigned char)((target_body_size >> 8U) & 0xffU);
        prefix[4] = (unsigned char)(target_body_size & 0xffU);
        crypto_sha512_update(&sha512, prefix, sizeof(prefix));
        crypto_sha512_update(&sha512, target_body, target_body_size);
    } else if (signature_type == 0x18U || signature_type == 0x28U) {
        pgpkey_sha512_update_v6_key(&sha512, &generated->encryption_public_body);
    }
    crypto_sha512_update(&sha512, hash_part.data, hash_part.size);
    trailer[0] = 6U;
    trailer[1] = 0xffU;
    trailer[2] = (unsigned char)((hash_part.size >> 24U) & 0xffU);
    trailer[3] = (unsigned char)((hash_part.size >> 16U) & 0xffU);
    trailer[4] = (unsigned char)((hash_part.size >> 8U) & 0xffU);
    trailer[5] = (unsigned char)(hash_part.size & 0xffU);
    crypto_sha512_update(&sha512, trailer, sizeof(trailer));
    crypto_sha512_final(&sha512, digest);
    if (crypto_ed25519_sign(signature, digest, sizeof(digest), generated->primary_seed, generated->primary_public) != 0) goto cleanup;
    if (pgpkey_buffer_append_data(&signature_body, hash_part.data, hash_part.size) != 0 ||
        pgpkey_buffer_append_u32_be(&signature_body, unhashed.size) != 0 ||
        pgpkey_buffer_append_data(&signature_body, unhashed.data, unhashed.size) != 0 ||
        pgpkey_buffer_append_byte(&signature_body, digest[0]) != 0 ||
        pgpkey_buffer_append_byte(&signature_body, digest[1]) != 0 ||
        pgpkey_buffer_append_byte(&signature_body, sizeof(salt)) != 0 ||
        pgpkey_buffer_append_data(&signature_body, salt, sizeof(salt)) != 0 ||
        pgpkey_buffer_append_data(&signature_body, signature, sizeof(signature)) != 0 ||
        pgpkey_buffer_append_packet(packet_out, 2U, &signature_body) != 0) goto cleanup;
    result = 0;

cleanup:
    if (result != 0) pgpkey_set_error(error, error_size, "out of memory while writing v6 signature packet");
    pgpkey_buffer_free(&hashed);
    pgpkey_buffer_free(&unhashed);
    pgpkey_buffer_free(&hash_part);
    pgpkey_buffer_free(&signature_body);
    crypto_secure_bzero(digest, sizeof(digest));
    crypto_secure_bzero(signature, sizeof(signature));
    crypto_secure_bzero(salt, sizeof(salt));
    return result;
}

static int build_generated_key_v6(const PgpKeyGenerateOptions *options, PgpKeyGeneratedKey *generated, char *error, size_t error_size) {
    unsigned long long created = current_epoch_or_zero();
    unsigned char primary_flags[] = { 0x03U };
    unsigned char subkey_flags[] = { 0x0cU };

    rt_memset(generated, 0, sizeof(*generated));
    if (created == 0ULL) created = 1ULL;
    if (platform_random_bytes(generated->primary_seed, sizeof(generated->primary_seed)) != 0 ||
        platform_random_bytes(generated->encryption_seed, sizeof(generated->encryption_seed)) != 0) {
        pgpkey_set_error(error, error_size, "cannot read random bytes");
        return -1;
    }
    if (crypto_ed25519_public_key_from_seed(generated->primary_public, generated->primary_seed) != 0 ||
        crypto_x25519_scalarmult_base(generated->encryption_public, generated->encryption_seed) != 0) {
        pgpkey_set_error(error, error_size, "public key generation failed");
        return -1;
    }
    if (build_generated_public_body_v6(generated, created, error, error_size) != 0 ||
        build_generated_secret_body_v6(generated, error, error_size) != 0 ||
        build_generated_encryption_public_body_v6(generated, created, error, error_size) != 0 ||
        build_generated_encryption_secret_body_v6(generated, error, error_size) != 0) return -1;
    if (pgpkey_buffer_append_data(&generated->user_id_packet, (const unsigned char *)options->user_id, rt_strlen(options->user_id)) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing user ID packet");
        return -1;
    }
    if (append_generated_signature_v6(generated, &generated->direct_signature_packet, 0x1fU, 0, 0U, created, options->key_expiration_seconds, primary_flags, sizeof(primary_flags), 0, 1, error, error_size) != 0 ||
        append_generated_signature_v6(generated, &generated->user_signature_packet, 0x13U, generated->user_id_packet.data, generated->user_id_packet.size, created, options->key_expiration_seconds, primary_flags, sizeof(primary_flags), 1, 0, error, error_size) != 0 ||
        append_generated_signature_v6(generated, &generated->subkey_signature_packet, 0x18U, 0, 0U, created, options->key_expiration_seconds, subkey_flags, sizeof(subkey_flags), 0, 0, error, error_size) != 0) return -1;
    if (pgpkey_buffer_append_packet(&generated->public_certificate, 6U, &generated->public_body) != 0 ||
        pgpkey_buffer_append_data(&generated->public_certificate, generated->direct_signature_packet.data, generated->direct_signature_packet.size) != 0 ||
        pgpkey_buffer_append_packet(&generated->public_certificate, 13U, &generated->user_id_packet) != 0 ||
        pgpkey_buffer_append_data(&generated->public_certificate, generated->user_signature_packet.data, generated->user_signature_packet.size) != 0 ||
        pgpkey_buffer_append_packet(&generated->public_certificate, 14U, &generated->encryption_public_body) != 0 ||
        pgpkey_buffer_append_data(&generated->public_certificate, generated->subkey_signature_packet.data, generated->subkey_signature_packet.size) != 0 ||
        pgpkey_buffer_append_packet(&generated->secret_certificate, 5U, &generated->secret_body) != 0 ||
        pgpkey_buffer_append_data(&generated->secret_certificate, generated->direct_signature_packet.data, generated->direct_signature_packet.size) != 0 ||
        pgpkey_buffer_append_packet(&generated->secret_certificate, 13U, &generated->user_id_packet) != 0 ||
        pgpkey_buffer_append_data(&generated->secret_certificate, generated->user_signature_packet.data, generated->user_signature_packet.size) != 0 ||
        pgpkey_buffer_append_packet(&generated->secret_certificate, 7U, &generated->encryption_secret_body) != 0 ||
        pgpkey_buffer_append_data(&generated->secret_certificate, generated->subkey_signature_packet.data, generated->subkey_signature_packet.size) != 0) {
        pgpkey_set_error(error, error_size, "out of memory while writing generated v6 certificates");
        return -1;
    }
    return 0;
}

static int build_generated_key(const PgpKeyGenerateOptions *options, PgpKeyGeneratedKey *generated, char *error, size_t error_size) {
    return options->legacy_v4 ? build_generated_key_legacy(options, generated, error, error_size) : build_generated_key_v6(options, generated, error, error_size);
}

static int write_generate_json(const PgpKeyGenerateOptions *options, const PgpKeyGeneratedKey *generated) {
    if (tool_json_begin_event(1, "pgpkey", "stdout", "generate") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"profile\":") != 0 || tool_json_write_string(1, options->legacy_v4 ? "legacy-v4" : "rfc9580") != 0) return -1;
    if (rt_write_cstr(1, ",\"version\":") != 0 || rt_write_uint(1, options->legacy_v4 ? 4ULL : 6ULL) != 0) return -1;
    if (rt_write_cstr(1, ",\"algorithm\":") != 0 || tool_json_write_string(1, options->legacy_v4 ? "EdDSA legacy" : "Ed25519") != 0) return -1;
    if (rt_write_cstr(1, ",\"curve\":\"Ed25519\",\"fingerprint\":\"") != 0) return -1;
    if (write_hex_bytes(1, generated->primary_info.fingerprint, generated->primary_info.fingerprint_size) != 0) return -1;
    if (rt_write_cstr(1, "\",\"key_id\":\"") != 0) return -1;
    if (write_hex_bytes(1, generated->primary_info.key_id, PGP_KEY_ID_SIZE) != 0) return -1;
    if (rt_write_cstr(1, "\",\"user_id\":") != 0 || tool_json_write_string(1, options->user_id) != 0) return -1;
    if (rt_write_cstr(1, ",\"secret_out\":") != 0 || tool_json_write_string(1, options->secret_out) != 0) return -1;
    if (rt_write_cstr(1, ",\"public_out\":") != 0 || tool_json_write_string(1, options->public_out) != 0) return -1;
    if (rt_write_cstr(1, ",\"protected\":false}") != 0) return -1;
    return tool_json_end_event(1);
}

static int write_generate_text(const PgpKeyGenerateOptions *options, const PgpKeyGeneratedKey *generated) {
    if (rt_write_cstr(1, "generated: ") != 0 || write_hex_bytes(1, generated->primary_info.fingerprint, generated->primary_info.fingerprint_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "profile: ") != 0 || rt_write_line(1, options->legacy_v4 ? "legacy-v4" : "rfc9580") != 0) return -1;
    if (rt_write_cstr(1, "key-id: ") != 0 || write_hex_bytes(1, generated->primary_info.key_id, PGP_KEY_ID_SIZE) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "secret: ") != 0 || rt_write_line(1, options->secret_out) != 0) return -1;
    if (rt_write_cstr(1, "public: ") != 0 || rt_write_line(1, options->public_out) != 0) return -1;
    if (options->legacy_v4 && rt_write_line(1, "warning: legacy v4 key material is deprecated by RFC 9580") != 0) return -1;
    return rt_write_line(1, "warning: secret key is not passphrase-protected");
}

static int command_generate(const PgpKeyOptions *options, int argc, char **argv, int argi) {
    PgpKeyGenerateOptions generate_options;
    PgpKeyGeneratedKey generated;
    char error[PGPKEY_ERROR_CAPACITY];
    int status;

    rt_memset(&generate_options, 0, sizeof(generate_options));
    while (argi < argc) {
        const char *arg = argv[argi];

        if (rt_strcmp(arg, "--userid") == 0 || rt_strcmp(arg, "--user-id") == 0 || rt_strcmp(arg, "-u") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpkey", "missing value for ", arg); return 1; }
            generate_options.user_id = argv[argi++];
        } else if (rt_strcmp(arg, "--out") == 0 || rt_strcmp(arg, "--secret-out") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpkey", "missing value for ", arg); return 1; }
            generate_options.secret_out = argv[argi++];
        } else if (rt_strcmp(arg, "--public-out") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpkey", "missing value for ", arg); return 1; }
            generate_options.public_out = argv[argi++];
        } else if (rt_strcmp(arg, "--expires") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpkey", "missing value for ", arg); return 1; }
            if (parse_expiration_duration(argv[argi], &generate_options.key_expiration_seconds) != 0) {
                tool_write_error("pgpkey", "invalid expiration duration: ", argv[argi]);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(arg, "--algorithm") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpkey", "missing value for ", arg); return 1; }
            if (rt_strcmp(argv[argi], "ed25519") != 0 && rt_strcmp(argv[argi], "Ed25519") != 0) {
                tool_write_error("pgpkey", "only Ed25519 generation is implemented: ", argv[argi]);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(arg, "--profile") == 0) {
            argi += 1;
            if (argi >= argc) { tool_write_error("pgpkey", "missing value for ", arg); return 1; }
            if (rt_strcmp(argv[argi], "rfc9580") == 0 || rt_strcmp(argv[argi], "v6") == 0) {
                generate_options.legacy_v4 = 0;
            } else if (rt_strcmp(argv[argi], "legacy-v4") == 0 || rt_strcmp(argv[argi], "v4") == 0) {
                generate_options.legacy_v4 = 1;
            } else {
                tool_write_error("pgpkey", "unknown generate profile: ", argv[argi]);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(arg, "--legacy-v4") == 0) {
            generate_options.legacy_v4 = 1;
            argi += 1;
        } else if (rt_strcmp(arg, "--no-passphrase") == 0) {
            generate_options.no_passphrase = 1;
            argi += 1;
        } else if (rt_strcmp(arg, "--armor") == 0) {
            argi += 1;
        } else {
            tool_write_error("pgpkey", "unknown generate option: ", arg);
            print_usage();
            return 1;
        }
    }
    if (generate_options.user_id == 0 || generate_options.user_id[0] == '\0') {
        tool_write_error("pgpkey", "generate requires --userid", 0);
        return 1;
    }
    if (generate_options.secret_out == 0 || generate_options.public_out == 0) {
        tool_write_error("pgpkey", "generate requires --out SECRET.asc and --public-out PUBLIC.asc", 0);
        return 1;
    }
    if (!generate_options.no_passphrase) {
        tool_write_error("pgpkey", "passphrase-protected secret keys are not implemented yet; pass --no-passphrase to create an unprotected key", 0);
        return 1;
    }
    if (rt_strcmp(generate_options.secret_out, "-") == 0 || rt_strcmp(generate_options.public_out, "-") == 0) {
        tool_write_error("pgpkey", "generate requires file outputs for secret and public keys", 0);
        return 1;
    }
    rt_memset(&generated, 0, sizeof(generated));
    if (build_generated_key(&generate_options, &generated, error, sizeof(error)) != 0) {
        generated_key_free(&generated);
        tool_write_error("pgpkey", error, 0);
        return 1;
    }
    if (write_generated_file(generate_options.secret_out, 1, generated.secret_certificate.data, generated.secret_certificate.size) != 0) {
        generated_key_free(&generated);
        tool_write_error("pgpkey", "cannot write secret key: ", generate_options.secret_out);
        return 1;
    }
    if (write_generated_file(generate_options.public_out, 0, generated.public_certificate.data, generated.public_certificate.size) != 0) {
        generated_key_free(&generated);
        tool_write_error("pgpkey", "cannot write public key: ", generate_options.public_out);
        return 1;
    }
    status = options->json ? write_generate_json(&generate_options, &generated) : write_generate_text(&generate_options, &generated);
    generated_key_free(&generated);
    return status == 0 ? 0 : 1;
}

static unsigned long long pgpkey_certificate_expiration_or_zero(const PgpCertificateInfo *certificate) {
    size_t primary_index = 0U;
    const PgpSignatureInfo *signature = latest_primary_user_id_signature(certificate, &primary_index);

    if (signature == 0 && certificate->user_id_count != 0U) signature = latest_self_signature_for_target(certificate, PGP_SIGNATURE_TARGET_USER_ID, 0U, 0);
    return signature != 0 && signature->has_key_expiration ? signature->key_expiration_seconds : 0ULL;
}

static int pgpkey_write_publicized_armor(const char *path, const unsigned char *data, size_t data_size) {
    PgpPacketReader reader;
    PgpKeyBuffer public_data;
    char error[PGPKEY_ERROR_CAPACITY];
    int fd;
    int result = -1;

    rt_memset(&public_data, 0, sizeof(public_data));
    pgp_packet_reader_init(&reader, data, data_size);
    while (1) {
        PgpPacket packet;
        int has_packet;
        size_t packet_end;

        if (pgp_packet_reader_next(&reader, &packet, &has_packet, error, sizeof(error)) != 0) goto cleanup;
        if (!has_packet) break;
        if (pgpkey_packet_end(&packet, &packet_end) != 0) goto cleanup;
        if (packet.tag == 5U || packet.tag == 7U) {
            PgpKeyBuffer body;
            size_t public_size;

            rt_memset(&body, 0, sizeof(body));
            if (pgpkey_public_body_size(packet.tag, data + packet.body_offset, packet.body_size, &public_size) != 0 ||
                pgpkey_buffer_append_data(&body, data + packet.body_offset, public_size) != 0 ||
                pgpkey_buffer_append_packet(&public_data, packet.tag == 5U ? 6U : 14U, &body) != 0) {
                pgpkey_buffer_free(&body);
                goto cleanup;
            }
            pgpkey_buffer_free(&body);
        } else if (pgpkey_buffer_append_data(&public_data, data + packet.header_offset, packet_end - packet.header_offset) != 0) {
            goto cleanup;
        }
    }
    fd = platform_open_write(path, 0644U);
    if (fd < 0) goto cleanup;
    result = pgp_write_public_key_armor(fd, public_data.data, public_data.size);
    if (platform_close(fd) != 0) result = -1;

cleanup:
    pgpkey_buffer_free(&public_data);
    return result;
}

static int pgpkey_write_edited_secret(const char *path, const unsigned char *data, size_t data_size) {
    int fd = platform_open_write(path, 0600U);
    int result;

    if (fd < 0) return -1;
    result = pgp_write_private_key_armor(fd, data, data_size);
    if (platform_close(fd) != 0) result = -1;
    return result;
}

static int pgpkey_build_inserted_data(const unsigned char *data, size_t data_size, size_t insert_offset, const PgpKeyBuffer *insert, PgpKeyBuffer *updated) {
    if (insert_offset > data_size) return -1;
    return pgpkey_buffer_append_data(updated, data, insert_offset) != 0 ||
           pgpkey_buffer_append_data(updated, insert->data, insert->size) != 0 ||
           pgpkey_buffer_append_data(updated, data + insert_offset, data_size - insert_offset) != 0 ? -1 : 0;
}

static int pgpkey_buffer_insert_at(PgpKeyBuffer *buffer, size_t offset, const PgpKeyBuffer *insert) {
    unsigned char *grown;

    if (offset > buffer->size || insert->size == 0U) return offset <= buffer->size ? 0 : -1;
    if (pgpkey_buffer_reserve(buffer, insert->size) != 0) return -1;
    grown = buffer->data;
    memmove(grown + offset + insert->size, grown + offset, buffer->size - offset);
    memcpy(grown + offset, insert->data, insert->size);
    buffer->size += insert->size;
    return 0;
}

static int command_edit(const PgpKeyOptions *options, int argc, char **argv, int argi) {
    PgpKeyEditOptions edit;
    unsigned char *data = 0;
    size_t data_size = 0U;
    PgpKeyEditCertificateContext cert_ctx;
    PgpKeyEditTargets targets;
    PgpKeyEditSecret secret;
    PgpKeyBuffer insert;
    PgpKeyBuffer updated;
    char error[PGPKEY_ERROR_CAPACITY];
    unsigned long long created;
    unsigned long long expiration;
    unsigned int operation_count = 0U;
    int status = 1;
    (void)options;

    rt_memset(&edit, 0, sizeof(edit));
    rt_memset(&cert_ctx, 0, sizeof(cert_ctx));
    rt_memset(&targets, 0, sizeof(targets));
    rt_memset(&secret, 0, sizeof(secret));
    rt_memset(&insert, 0, sizeof(insert));
    rt_memset(&updated, 0, sizeof(updated));
    if (argi >= argc) { print_usage(); return 1; }
    edit.input_path = argv[argi++];
    while (argi < argc) {
        const char *arg = argv[argi++];

        if (rt_strcmp(arg, "--out") == 0) {
            if (argi >= argc) { tool_write_error("pgpkey", "missing value for --out", 0); return 1; }
            edit.output_path = argv[argi++];
        } else if (rt_strcmp(arg, "--public-out") == 0) {
            if (argi >= argc) { tool_write_error("pgpkey", "missing value for --public-out", 0); return 1; }
            edit.public_output_path = argv[argi++];
        } else if (rt_strcmp(arg, "--add-uid") == 0 || rt_strcmp(arg, "--add-userid") == 0) {
            if (argi >= argc) { tool_write_error("pgpkey", "missing value for ", arg); return 1; }
            edit.add_uid = argv[argi++]; operation_count += 1U;
        } else if (rt_strcmp(arg, "--revoke-uid") == 0 || rt_strcmp(arg, "--revoke-userid") == 0) {
            if (argi >= argc) { tool_write_error("pgpkey", "missing value for ", arg); return 1; }
            edit.revoke_uid = argv[argi++]; operation_count += 1U;
        } else if (rt_strcmp(arg, "--set-primary-uid") == 0) {
            if (argi >= argc) { tool_write_error("pgpkey", "missing value for --set-primary-uid", 0); return 1; }
            edit.primary_uid = argv[argi++]; operation_count += 1U;
        } else if (rt_strcmp(arg, "--change-expiration") == 0 || rt_strcmp(arg, "--expires") == 0) {
            if (argi >= argc || parse_expiration_duration(argv[argi], &edit.expiration_seconds) != 0) { tool_write_error("pgpkey", "invalid expiration duration", 0); return 1; }
            edit.has_expiration = 1; argi += 1; operation_count += 1U;
        } else if (rt_strcmp(arg, "--refresh-self-signatures") == 0) {
            edit.refresh = 1; operation_count += 1U;
        } else if (rt_strcmp(arg, "--add-subkey") == 0) {
            edit.add_subkey = 1; operation_count += 1U;
        } else if (rt_strcmp(arg, "--revoke-subkey") == 0) {
            if (argi >= argc) { tool_write_error("pgpkey", "missing value for --revoke-subkey", 0); return 1; }
            edit.revoke_subkey = argv[argi++]; operation_count += 1U;
        } else {
            tool_write_error("pgpkey", "unknown edit option: ", arg);
            return 1;
        }
    }
    if (edit.output_path == 0) { tool_write_error("pgpkey", "edit requires --out", 0); return 1; }
    if (operation_count != 1U) { tool_write_error("pgpkey", "edit requires exactly one operation", 0); return 1; }
    if (load_openpgp_file(edit.input_path, &data, &data_size, error, sizeof(error)) != 0) { pgpkey_write_error_path(error, edit.input_path); return 1; }
    if (pgp_for_each_certificate(data, data_size, pgpkey_edit_certificate_callback, &cert_ctx, error, sizeof(error)) != 0 || !cert_ctx.have_certificate) {
        tool_write_error("pgpkey", "no editable certificate in ", edit.input_path);
        goto cleanup;
    }
    if (pgpkey_scan_edit_targets(data, data_size, &cert_ctx.certificate, &targets, error, sizeof(error)) != 0 || !targets.is_secret) {
        tool_write_error("pgpkey", "edit requires an unprotected secret key file", 0);
        goto cleanup;
    }
    if (pgpkey_parse_ed25519_secret(targets.primary.body, targets.primary.body_size, 0, &secret) != 0) {
        tool_write_error("pgpkey", "cannot read unprotected Ed25519 primary secret key", 0);
        goto cleanup;
    }
    created = current_epoch_or_zero();
    if (created == 0ULL) created = 1ULL;
    expiration = edit.has_expiration ? edit.expiration_seconds : pgpkey_certificate_expiration_or_zero(&cert_ctx.certificate);
    if (edit.add_uid != 0) {
        PgpKeyBuffer uid_body;
        unsigned char flags[] = { 0x03U };

        rt_memset(&uid_body, 0, sizeof(uid_body));
        if (pgpkey_buffer_append_data(&uid_body, (const unsigned char *)edit.add_uid, rt_strlen(edit.add_uid)) != 0 ||
            pgpkey_buffer_append_packet(&insert, 13U, &uid_body) != 0 ||
            pgpkey_build_edit_signature(&insert, &secret, &targets.primary, 0x13U, PGP_SIGNATURE_TARGET_USER_ID, uid_body.data, uid_body.size, 0, 0U, created, flags, sizeof(flags), 0, 1, 1, expiration) != 0) {
            pgpkey_buffer_free(&uid_body);
            goto cleanup;
        }
        pgpkey_buffer_free(&uid_body);
        if (pgpkey_build_inserted_data(data, data_size, cert_ctx.certificate.end_offset, &insert, &updated) != 0) goto cleanup;
    } else if (edit.primary_uid != 0 || edit.revoke_uid != 0 || edit.has_expiration) {
        size_t uid_index;
        const char *uid = edit.primary_uid != 0 ? edit.primary_uid : edit.revoke_uid;
        unsigned char flags[] = { 0x03U };
        unsigned int sig_type = edit.revoke_uid != 0 ? 0x30U : 0x13U;
        int primary_uid = edit.primary_uid != 0 || edit.has_expiration ? 1 : -1;

        if (edit.has_expiration) {
            const PgpSignatureInfo *primary_sig = latest_primary_user_id_signature(&cert_ctx.certificate, &uid_index);
            if (primary_sig == 0) uid_index = 0U;
            if (uid_index >= targets.user_id_count) { tool_write_error("pgpkey", "no user ID to refresh expiration", 0); goto cleanup; }
        } else if (pgpkey_find_uid_target(&targets, uid, &uid_index) != 0) {
            tool_write_error("pgpkey", "user ID not found: ", uid);
            goto cleanup;
        }
        if (pgpkey_build_edit_signature(&insert, &secret, &targets.primary, sig_type, PGP_SIGNATURE_TARGET_USER_ID, targets.user_ids[uid_index].body, targets.user_ids[uid_index].body_size, 0, 0U, created, edit.revoke_uid != 0 ? 0 : flags, edit.revoke_uid != 0 ? 0U : sizeof(flags), primary_uid, edit.revoke_uid == 0, edit.revoke_uid == 0, expiration) != 0 ||
            pgpkey_build_inserted_data(data, data_size, targets.user_ids[uid_index].insert_offset, &insert, &updated) != 0) goto cleanup;
    } else if (edit.add_subkey) {
        PgpKeyGeneratedKey generated;

        rt_memset(&generated, 0, sizeof(generated));
        memcpy(generated.primary_seed, secret.seed, sizeof(generated.primary_seed));
        memcpy(generated.primary_public, secret.public_key, sizeof(generated.primary_public));
        generated.primary_info = secret.info;
        if (platform_random_bytes(generated.encryption_seed, sizeof(generated.encryption_seed)) != 0 ||
            crypto_x25519_scalarmult_base(generated.encryption_public, generated.encryption_seed) != 0 ||
            pgpkey_buffer_append_data(&generated.public_body, targets.primary.body, targets.primary.public_body_size) != 0 ||
            build_generated_encryption_public_body(&generated, created, error, sizeof(error)) != 0 ||
            build_generated_encryption_secret_body(&generated, error, sizeof(error)) != 0 ||
            append_generated_subkey_binding_signature(&generated, created, expiration, error, sizeof(error)) != 0 ||
            pgpkey_buffer_append_packet(&insert, 7U, &generated.encryption_secret_body) != 0 ||
            pgpkey_buffer_append_data(&insert, generated.subkey_signature_packet.data, generated.subkey_signature_packet.size) != 0 ||
            pgpkey_build_inserted_data(data, data_size, cert_ctx.certificate.end_offset, &insert, &updated) != 0) {
            generated_key_free(&generated);
            goto cleanup;
        }
        generated_key_free(&generated);
    } else if (edit.revoke_subkey != 0) {
        size_t subkey_index;

        if (pgpkey_find_subkey_target(&targets, edit.revoke_subkey, &subkey_index) != 0) { tool_write_error("pgpkey", "subkey not found: ", edit.revoke_subkey); goto cleanup; }
        if (pgpkey_build_edit_signature(&insert, &secret, &targets.primary, 0x28U, PGP_SIGNATURE_TARGET_SUBKEY, 0, 0U, targets.subkeys[subkey_index].body, targets.subkeys[subkey_index].public_body_size, created, 0, 0U, -1, 0, 0, 0ULL) != 0 ||
            pgpkey_build_inserted_data(data, data_size, targets.subkeys[subkey_index].insert_offset, &insert, &updated) != 0) goto cleanup;
    } else if (edit.refresh) {
        size_t primary_uid_index = 0U;
        size_t index;
        unsigned char flags[] = { 0x03U };
        unsigned char subkey_flags[] = { 0x0cU };

        if (targets.user_id_count == 0U) { tool_write_error("pgpkey", "no user ID to refresh", 0); goto cleanup; }
        (void)latest_primary_user_id_signature(&cert_ctx.certificate, &primary_uid_index);
        if (primary_uid_index >= targets.user_id_count) primary_uid_index = 0U;
        if (pgpkey_buffer_append_data(&updated, data, data_size) != 0) goto cleanup;
        for (index = targets.subkey_count; index > 0U; --index) {
            size_t subkey_index = index - 1U;
            PgpKeyBuffer one;

            rt_memset(&one, 0, sizeof(one));
            if (pgpkey_build_edit_signature(&one, &secret, &targets.primary, 0x18U, PGP_SIGNATURE_TARGET_SUBKEY, 0, 0U, targets.subkeys[subkey_index].body, targets.subkeys[subkey_index].public_body_size, created, subkey_flags, sizeof(subkey_flags), -1, 0, 1, expiration) != 0 ||
                pgpkey_buffer_insert_at(&updated, targets.subkeys[subkey_index].insert_offset, &one) != 0) {
                pgpkey_buffer_free(&one);
                goto cleanup;
            }
            pgpkey_buffer_free(&one);
        }
        for (index = targets.user_id_count; index > 0U; --index) {
            size_t uid_index = index - 1U;
            PgpKeyBuffer one;

            rt_memset(&one, 0, sizeof(one));
            if (pgpkey_build_edit_signature(&one, &secret, &targets.primary, 0x13U, PGP_SIGNATURE_TARGET_USER_ID, targets.user_ids[uid_index].body, targets.user_ids[uid_index].body_size, 0, 0U, created, flags, sizeof(flags), uid_index == primary_uid_index ? 1 : 0, 1, 1, expiration) != 0 ||
                pgpkey_buffer_insert_at(&updated, targets.user_ids[uid_index].insert_offset, &one) != 0) {
                pgpkey_buffer_free(&one);
                goto cleanup;
            }
            pgpkey_buffer_free(&one);
        }
    }
    if (updated.size == 0U) goto cleanup;
    if (pgpkey_write_edited_secret(edit.output_path, updated.data, updated.size) != 0) { tool_write_error("pgpkey", "cannot write edited secret key: ", edit.output_path); goto cleanup; }
    if (edit.public_output_path != 0 && pgpkey_write_publicized_armor(edit.public_output_path, updated.data, updated.size) != 0) { tool_write_error("pgpkey", "cannot write edited public key: ", edit.public_output_path); goto cleanup; }
    if (rt_write_line(1, "edited") != 0) goto cleanup;
    status = 0;

cleanup:
    if (data != 0) rt_free(data);
    pgpkey_buffer_free(&insert);
    pgpkey_buffer_free(&updated);
    crypto_secure_bzero(&secret, sizeof(secret));
    return status;
}

static int command_import(const PgpKeyOptions *options, int argc, char **argv, int argi) {
    int status = 0;

    if (argi >= argc) {
        print_usage();
        return 1;
    }
    while (argi < argc) {
        unsigned char *data = 0;
        size_t data_size = 0U;
        char error[PGPKEY_ERROR_CAPACITY];
        PgpKeyImportScanContext scan;
        PgpKeyImportContext import;

        rt_memset(&scan, 0, sizeof(scan));
        if (load_openpgp_file(argv[argi], &data, &data_size, error, sizeof(error)) != 0) {
            pgpkey_write_error_path(error, argv[argi]);
            status = 1;
            argi += 1;
            continue;
        }
        if (pgp_for_each_certificate(data, data_size, import_scan_callback, &scan, error, sizeof(error)) != 0) {
            pgpkey_write_error_path(error, argv[argi]);
            rt_free(data);
            status = 1;
            argi += 1;
            continue;
        }
        if (scan.secret_found) {
            tool_write_error("pgpkey", "refusing to import private key into public keyring: ", argv[argi]);
            rt_free(data);
            status = 1;
            argi += 1;
            continue;
        }
        if (scan.public_count == 0) {
            tool_write_error("pgpkey", "no importable public key in ", argv[argi]);
            rt_free(data);
            status = 1;
            argi += 1;
            continue;
        }
        import.options = options;
        import.data = data;
        import.status = 0;
        if (pgp_for_each_certificate(data, data_size, import_certificate_callback, &import, error, sizeof(error)) != 0) {
            pgpkey_write_error_path(error, argv[argi]);
            rt_free(data);
            status = 1;
            argi += 1;
            continue;
        }
        if (import.status != 0) status = 1;
        rt_free(data);
        argi += 1;
    }
    return status;
}

static int pgpkey_store_init(const char *store_path) {
    char keyring_path[PGPKEY_PATH_CAPACITY];
    char index_path[PGPKEY_PATH_CAPACITY];
    PlatformDirEntry index_info;
    int is_dir = 0;
    int fd;

    if (pgpkey_resolve_store_paths(store_path, keyring_path, sizeof(keyring_path), index_path, sizeof(index_path)) != 0) {
        tool_write_error("pgpkey", "invalid store path: ", store_path);
        return 1;
    }
    if (platform_make_directory(store_path, 0700U) != 0 && (platform_path_is_directory(store_path, &is_dir) != 0 || !is_dir)) {
        tool_write_error("pgpkey", "cannot create store directory: ", store_path);
        return 1;
    }
    fd = platform_open_append(keyring_path, 0600U);
    if (fd < 0 || platform_close(fd) != 0) {
        tool_write_error("pgpkey", "cannot create store keyring: ", keyring_path);
        if (fd >= 0) (void)platform_close(fd);
        return 1;
    }
    rt_memset(&index_info, 0, sizeof(index_info));
    if (platform_get_path_info(index_path, &index_info) != 0 && pgpkey_store_write_empty_index(index_path) != 0) {
        tool_write_error("pgpkey", "cannot create store index: ", index_path);
        return 1;
    }
    if (rt_write_cstr(1, "store: ") != 0 || rt_write_line(1, store_path) != 0) return 1;
    return 0;
}

static int command_store(const PgpKeyOptions *options, int argc, char **argv, int argi) {
    const char *operation;
    const char *store_path;
    char keyring_path[PGPKEY_PATH_CAPACITY];
    char index_path[PGPKEY_PATH_CAPACITY];
    PgpKeyOptions store_options;
    int result;

    if (argi >= argc) {
        print_usage();
        return 1;
    }
    operation = argv[argi++];
    if (rt_strcmp(operation, "init") == 0) {
        if (argi + 1 != argc) {
            print_usage();
            return 1;
        }
        return pgpkey_store_init(argv[argi]);
    }
    if (argi >= argc) {
        print_usage();
        return 1;
    }
    store_path = argv[argi++];
    if (pgpkey_resolve_store_paths(store_path, keyring_path, sizeof(keyring_path), index_path, sizeof(index_path)) != 0) {
        tool_write_error("pgpkey", "invalid store path: ", store_path);
        return 1;
    }
    store_options = *options;
    store_options.store_path = store_path;
    store_options.keyring_path = keyring_path;
    store_options.keystore_path = index_path;
    if (rt_strcmp(operation, "import") == 0) {
        if (argi >= argc) {
            print_usage();
            return 1;
        }
        result = command_import(&store_options, argc, argv, argi);
        if (result != 0) return result;
        if (pgpkey_store_rebuild_index_file(keyring_path, index_path) != 0) {
            tool_write_error("pgpkey", "cannot rebuild store index: ", index_path);
            return 1;
        }
        return 0;
    }
    if (rt_strcmp(operation, "rebuild-index") == 0 || rt_strcmp(operation, "index") == 0) {
        if (argi != argc) {
            print_usage();
            return 1;
        }
        if (pgpkey_store_rebuild_index_file(keyring_path, index_path) != 0) {
            tool_write_error("pgpkey", "cannot rebuild store index: ", index_path);
            return 1;
        }
        if (rt_write_cstr(1, "indexed: ") != 0 || rt_write_line(1, index_path) != 0) return 1;
        return 0;
    }
    tool_write_error("pgpkey", "unknown store operation: ", operation);
    print_usage();
    return 1;
}

static int export_certificate_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpKeyExportContext *ctx = (PgpKeyExportContext *)ctx_ptr;
    int fd = 1;
    int close_fd = 0;
    int result;

    if (!pgp_fingerprint_matches_text(&certificate->primary, ctx->selector)) {
        return 0;
    }
    ctx->found = 1;
    if (ctx->output_path != 0) {
        fd = platform_open_write(ctx->output_path, 0644U);
        if (fd < 0) return -1;
        close_fd = 1;
    }
    result = pgp_write_public_key_armor(fd, ctx->data + certificate->start_offset, certificate->end_offset - certificate->start_offset);
    if (close_fd && platform_close(fd) != 0) result = -1;
    return result != 0 ? -1 : 0;
}

static int command_export(const PgpKeyOptions *options, int argc, char **argv, int argi) {
    unsigned char *data = 0;
    size_t data_size = 0U;
    char error[PGPKEY_ERROR_CAPACITY];
    PgpKeyExportContext ctx;

    if (argi >= argc || argi + 2 < argc) {
        print_usage();
        return 1;
    }
    if (tool_read_all_input(options->keyring_path, &data, &data_size) != 0) {
        tool_write_error("pgpkey", "cannot read keyring: ", options->keyring_path);
        return 1;
    }
    ctx.data = data;
    ctx.size = data_size;
    ctx.selector = argv[argi];
    ctx.found = 0;
    ctx.output_path = argi + 1 < argc ? argv[argi + 1] : 0;
    if (pgp_for_each_certificate(data, data_size, export_certificate_callback, &ctx, error, sizeof(error)) < 0) {
        rt_free(data);
        tool_write_error("pgpkey", "export failed", 0);
        return 1;
    }
    rt_free(data);
    if (!ctx.found) {
        tool_write_error("pgpkey", "key not found: ", argv[argi]);
        return 1;
    }
    return 0;
}

static int resolve_default_keyring(char *buffer, size_t buffer_size) {
    const char *env_path = platform_getenv("PGPKEY_KEYRING");
    const char *home;

    if (env_path != 0 && env_path[0] != '\0') {
        rt_copy_string(buffer, buffer_size, env_path);
        return 0;
    }
    home = platform_getenv("HOME");
    if (home == 0 || home[0] == '\0') {
        return -1;
    }
    return rt_join_path(home, ".pgpkeyring", buffer, buffer_size);
}

int main(int argc, char **argv) {
    PgpKeyOptions options;
    ToolOptState opt;
    int option_result;
    char default_keyring[PGPKEY_PATH_CAPACITY];
    char store_keyring[PGPKEY_PATH_CAPACITY];
    char store_keystore[PGPKEY_PATH_CAPACITY];
    const char *command;
    const char *initial_command;

    rt_memset(&options, 0, sizeof(options));
    options.color_mode = TOOL_COLOR_AUTO;
    tool_opt_init(&opt, argc, argv, "pgpkey", PGPKEY_USAGE);
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-k") == 0 || rt_strcmp(opt.flag, "--keyring") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.keyring_path = opt.value;
        } else if (rt_strcmp(opt.flag, "--keystore") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.keystore_path = opt.value;
        } else if (rt_strcmp(opt.flag, "--store") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.store_path = opt.value;
        } else if (rt_strncmp(opt.flag, "--store=", 8U) == 0) {
            options.store_path = opt.flag + 8U;
        } else if (rt_strcmp(opt.flag, "-v") == 0 || rt_strcmp(opt.flag, "--verbose") == 0) {
            options.verbose = 1;
        } else if (rt_strcmp(opt.flag, "--color") == 0) {
            options.color_mode = TOOL_COLOR_AUTO;
            tool_set_global_color_mode(options.color_mode);
        } else if (rt_strncmp(opt.flag, "--color=", 8U) == 0) {
            if (tool_parse_color_mode(opt.flag + 8, &options.color_mode) != 0) {
                tool_write_error("pgpkey", "invalid color mode: ", opt.flag + 8);
                print_usage();
                return 1;
            }
            tool_set_global_color_mode(options.color_mode);
        } else if (rt_strcmp(opt.flag, "--no-color") == 0) {
            options.color_mode = TOOL_COLOR_NEVER;
            tool_set_global_color_mode(options.color_mode);
        } else {
            tool_write_error("pgpkey", "unknown option: ", opt.flag);
            print_usage();
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        print_usage();
        return 0;
    }
    options.json = tool_json_is_enabled();
    initial_command = opt.argi < argc ? argv[opt.argi] : 0;
    if (options.store_path != 0) {
        if (pgpkey_resolve_store_paths(options.store_path, store_keyring, sizeof(store_keyring), store_keystore, sizeof(store_keystore)) != 0) {
            tool_write_error("pgpkey", "invalid store path: ", options.store_path);
            return 1;
        }
        if (options.keyring_path == 0) options.keyring_path = store_keyring;
        if (options.keystore_path == 0) options.keystore_path = store_keystore;
    }
    if (options.keyring_path == 0) {
        if (initial_command != 0 && rt_strcmp(initial_command, "store") == 0) {
            options.keyring_path = "";
        } else {
            if (resolve_default_keyring(default_keyring, sizeof(default_keyring)) != 0) {
                tool_write_error("pgpkey", "set HOME, PGPKEY_KEYRING, or pass --keyring", 0);
                return 1;
            }
            options.keyring_path = default_keyring;
        }
    }
    if (opt.argi >= argc) {
        return command_show(&options, argc, argv, opt.argi);
    }
    command = argv[opt.argi++];
    if (rt_strcmp(command, "show") == 0 || rt_strcmp(command, "inspect") == 0) {
        return command_show(&options, argc, argv, opt.argi);
    }
    if (rt_strcmp(command, "packets") == 0) {
        return command_packets(argc, argv, opt.argi);
    }
    if (rt_strcmp(command, "issuers") == 0) {
        return command_issuers(&options, argc, argv, opt.argi);
    }
    if (rt_strcmp(command, "catalog-sql") == 0 || rt_strcmp(command, "index-sql") == 0) {
        return command_catalog_sql(&options, argc, argv, opt.argi);
    }
    if (rt_strcmp(command, "store") == 0) {
        return command_store(&options, argc, argv, opt.argi);
    }
    if (rt_strcmp(command, "generate") == 0 || rt_strcmp(command, "gen") == 0) {
        return command_generate(&options, argc, argv, opt.argi);
    }
    if (rt_strcmp(command, "edit") == 0) {
        return command_edit(&options, argc, argv, opt.argi);
    }
    if (rt_strcmp(command, "import") == 0) {
        int result = command_import(&options, argc, argv, opt.argi);

        if (result == 0 && options.store_path != 0 && options.keystore_path != 0 && pgpkey_store_rebuild_index_file(options.keyring_path, options.keystore_path) != 0) {
            tool_write_error("pgpkey", "cannot rebuild store index: ", options.keystore_path);
            return 1;
        }
        return result;
    }
    if (rt_strcmp(command, "list") == 0 || rt_strcmp(command, "ls") == 0) {
        if (opt.argi != argc) {
            print_usage();
            return 1;
        }
        return command_show(&options, argc, argv, opt.argi);
    }
    if (rt_strcmp(command, "export") == 0) {
        return command_export(&options, argc, argv, opt.argi);
    }
    tool_write_error("pgpkey", "unknown command: ", command);
    print_usage();
    return 1;
}