#include "crypto/crypto_util.h"
#include "crypto/curve25519.h"
#include "crypto/ed25519.h"
#include "crypto/sha512.h"
#include "pgp.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PGPKEY_USAGE "[-k KEYRING] [-v] [--color[=WHEN]|--no-color] COMMAND [ARGS...]\n       pgpkey show [-v] [FILE ...]\n       pgpkey packets FILE ...\n       pgpkey issuers [--external] [FILE ...]\n       pgpkey generate --userid USERID --out SECRET.asc --public-out PUBLIC.asc --no-passphrase [--expires DURATION]\n       pgpkey import FILE ...\n       pgpkey list\n       pgpkey export FINGERPRINT [OUTPUT]"
#define PGPKEY_PATH_CAPACITY 1024U
#define PGPKEY_ERROR_CAPACITY 160U
#define PGPKEY_ED25519_KEY_SIZE 32U
#define PGPKEY_ED25519_SIGNATURE_SIZE 64U
#define PGPKEY_MAX_ISSUERS 2048U

typedef struct {
    const char *keyring_path;
    int json;
    int verbose;
    int color_mode;
} PgpKeyOptions;

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
    char fingerprint[PGP_FINGERPRINT_MAX_SIZE * 2U + 1U];
    int is_secret;
} PgpKeyFirstCertificateContext;

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
    PgpKeyBuffer subkey_signature_packet;
    PgpKeyBuffer public_certificate;
    PgpKeyBuffer secret_certificate;
} PgpKeyGeneratedKey;

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

static int write_expiration_status(int fd, unsigned long long expires, unsigned long long now, int color_mode) {
    int expired = now != 0ULL && expires != 0ULL && expires <= now;
    const char *text = expired ? "expired" : "valid";
    int style = expired ? TOOL_STYLE_BOLD_RED : TOOL_STYLE_BOLD_GREEN;

    if (rt_write_cstr(fd, " (") != 0) return -1;
    tool_write_styled(fd, color_mode, style, text);
    return rt_write_char(fd, ')');
}

static int write_expiration_date_with_status(int fd, unsigned long long expires, unsigned long long now, int color_mode) {
    if (expires == 0ULL) {
        if (rt_write_cstr(fd, "never") != 0) return -1;
        return write_expiration_status(fd, expires, now, color_mode);
    }
    if (write_date(fd, expires) != 0) return -1;
    return write_expiration_status(fd, expires, now, color_mode);
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

static int write_key_line(const PgpPublicKeyInfo *key, const char *label) {
    if (rt_write_cstr(1, label) != 0 || rt_write_cstr(1, ": ") != 0) return -1;
    if (rt_write_cstr(1, pgp_key_kind_name(key->tag)) != 0 || rt_write_cstr(1, ", v") != 0) return -1;
    if (rt_write_uint(1, key->version) != 0 || rt_write_cstr(1, ", ") != 0) return -1;
    if (rt_write_cstr(1, pgp_public_key_algorithm_name(key->algorithm)) != 0) return -1;
    if (key->bits != 0U) {
        if (rt_write_cstr(1, ", ") != 0 || rt_write_uint(1, key->bits) != 0 || rt_write_cstr(1, " bits") != 0) return -1;
    }
    if (key->created != 0ULL) {
        if (rt_write_cstr(1, ", created ") != 0 || write_date(1, key->created) != 0) return -1;
    }
    return rt_write_char(1, '\n');
}

static int write_signature_summary_text(const PgpCertificateInfo *certificate, unsigned long long now, int color_mode) {
    const PgpSignatureInfo *primary_signature;
    size_t primary_uid_index;

    primary_signature = latest_primary_user_id_signature(certificate, &primary_uid_index);
    if (primary_signature != 0 && primary_uid_index < certificate->user_id_count) {
        if (rt_write_cstr(1, "primary-uid: ") != 0 || tool_write_visible_line(1, certificate->user_ids[primary_uid_index]) != 0) return -1;
    } else if (certificate->user_id_count != 0U) {
        primary_signature = latest_self_signature_for_target(certificate, PGP_SIGNATURE_TARGET_USER_ID, 0U, 0);
    }
    if (primary_signature != 0) {
        if (primary_signature->key_flags_size != 0U) {
            if (rt_write_cstr(1, "key-flags: ") != 0 || write_key_flags(1, primary_signature->key_flags, primary_signature->key_flags_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
        if (primary_signature->has_key_expiration) {
            if (rt_write_cstr(1, "key-expires: ") != 0) return -1;
            if (primary_signature->key_expiration_seconds == 0ULL) {
                if (write_expiration_date_with_status(1, 0ULL, now, color_mode) != 0) return -1;
            } else if (write_expiration_date_with_status(1, certificate->primary.created + primary_signature->key_expiration_seconds, now, color_mode) != 0) {
                return -1;
            }
            if (rt_write_char(1, '\n') != 0) return -1;
        }
        if (primary_signature->preferred_symmetric_count != 0U) {
            if (rt_write_cstr(1, "preferred-symmetric: ") != 0 || write_algorithm_list(1, primary_signature->preferred_symmetric, primary_signature->preferred_symmetric_count, pgp_symmetric_algorithm_name, pgpkey_symmetric_algorithm_style, color_mode) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
        if (primary_signature->preferred_hash_count != 0U) {
            if (rt_write_cstr(1, "preferred-hash: ") != 0 || write_algorithm_list(1, primary_signature->preferred_hash, primary_signature->preferred_hash_count, pgp_hash_algorithm_name, pgpkey_hash_algorithm_style, color_mode) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
        if (primary_signature->preferred_compression_count != 0U) {
            if (rt_write_cstr(1, "preferred-compression: ") != 0 || write_algorithm_list(1, primary_signature->preferred_compression, primary_signature->preferred_compression_count, pgp_compression_algorithm_name, pgpkey_compression_algorithm_style, color_mode) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
    }
    return 0;
}

static int write_subkey_signature_summary_text(const PgpCertificateInfo *certificate, size_t subkey_index, unsigned long long now, int color_mode) {
    const PgpSignatureInfo *subkey_signature = latest_self_signature_for_target(certificate, PGP_SIGNATURE_TARGET_SUBKEY, subkey_index, 0);

    if (subkey_signature == 0) return 0;
    if (subkey_signature->key_flags_size != 0U) {
        if (rt_write_cstr(1, "subkey-flags: ") != 0 || write_key_flags(1, subkey_signature->key_flags, subkey_signature->key_flags_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
    }
    if (subkey_signature->has_key_expiration) {
        if (rt_write_cstr(1, "subkey-expires: ") != 0) return -1;
        if (subkey_signature->key_expiration_seconds == 0ULL) {
            if (write_expiration_date_with_status(1, 0ULL, now, color_mode) != 0) return -1;
        } else if (write_expiration_date_with_status(1, certificate->subkeys[subkey_index].created + subkey_signature->key_expiration_seconds, now, color_mode) != 0) {
            return -1;
        }
        if (rt_write_char(1, '\n') != 0) return -1;
    }
    return 0;
}

static int write_signature_verbose_line(const PgpCertificateInfo *certificate, const PgpSignatureInfo *signature, unsigned long long now, int color_mode) {
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
    if (signature->has_signature_expiration) {
        if (rt_write_cstr(1, ", signature-expires ") != 0) return -1;
        if (signature->created != 0ULL && signature->signature_expiration_seconds != 0ULL) {
            if (write_expiration_date_with_status(1, signature->created + signature->signature_expiration_seconds, now, color_mode) != 0) return -1;
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
            if (write_expiration_date_with_status(1, base + signature->key_expiration_seconds, now, color_mode) != 0) return -1;
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

static int write_verbose_signatures_text(const PgpCertificateInfo *certificate, unsigned long long now, int color_mode) {
    size_t index;

    if (certificate->signature_info_count == 0U) return 0;
    if (rt_write_line(1, "signatures:") != 0) return -1;
    for (index = 0U; index < certificate->signature_info_count; ++index) {
        if (write_signature_verbose_line(certificate, &certificate->signatures[index], now, color_mode) != 0) return -1;
    }
    return 0;
}

static int write_certificate_text(const PgpCertificateInfo *certificate, const char *source, size_t index, int verbose, int color_mode) {
    size_t uid_index;
    size_t subkey_index;
    unsigned long long now = current_epoch_or_zero();

    if (source != 0) {
        if (rt_write_cstr(1, "source: ") != 0 || rt_write_line(1, source) != 0) return -1;
    }
    if (rt_write_cstr(1, "certificate: ") != 0 || rt_write_uint(1, (unsigned long long)index) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (write_key_line(&certificate->primary, "primary") != 0) return -1;
    if (certificate->primary.fingerprint_size != 0U) {
        if (rt_write_cstr(1, "fingerprint: ") != 0 || write_hex_bytes(1, certificate->primary.fingerprint, certificate->primary.fingerprint_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
        if (rt_write_cstr(1, "key-id: ") != 0 || write_hex_bytes(1, certificate->primary.key_id, PGP_KEY_ID_SIZE) != 0 || rt_write_char(1, '\n') != 0) return -1;
    } else if (rt_write_line(1, "fingerprint: unsupported") != 0) {
        return -1;
    }
    for (uid_index = 0U; uid_index < certificate->user_id_count; ++uid_index) {
        if (rt_write_cstr(1, "uid: ") != 0 || tool_write_visible_line(1, certificate->user_ids[uid_index]) != 0) return -1;
    }
    if (write_signature_summary_text(certificate, now, color_mode) != 0) return -1;
    for (subkey_index = 0U; subkey_index < certificate->subkey_count; ++subkey_index) {
        if (write_key_line(&certificate->subkeys[subkey_index], "subkey") != 0) return -1;
        if (certificate->subkeys[subkey_index].fingerprint_size != 0U) {
            if (rt_write_cstr(1, "subkey-fingerprint: ") != 0 || write_hex_bytes(1, certificate->subkeys[subkey_index].fingerprint, certificate->subkeys[subkey_index].fingerprint_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
        if (write_subkey_signature_summary_text(certificate, subkey_index, now, color_mode) != 0) return -1;
    }
    if (rt_write_cstr(1, "packets: ") != 0 || rt_write_uint(1, certificate->packet_count) != 0) return -1;
    if (rt_write_cstr(1, ", signatures: ") != 0 || rt_write_uint(1, certificate->signature_count) != 0) return -1;
    if (rt_write_cstr(1, ", user-attributes: ") != 0 || rt_write_uint(1, certificate->user_attribute_count) != 0) return -1;
    if (rt_write_char(1, '\n') != 0) return -1;
    if (verbose && write_verbose_signatures_text(certificate, now, color_mode) != 0) return -1;
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
} PgpKeyShowContext;

static int show_certificate_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpKeyShowContext *ctx = (PgpKeyShowContext *)ctx_ptr;

    ctx->count += 1U;
    if (ctx->json) return write_certificate_json(certificate, ctx->source, ctx->count);
    return write_certificate_text(certificate, ctx->source, ctx->count, ctx->verbose, ctx->color_mode);
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

static int show_one_path(const char *path, int json, int verbose, int color_mode) {
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

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-v") == 0 || rt_strcmp(argv[argi], "--verbose") == 0) {
            verbose = 1;
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

    if (argi >= argc) {
        return show_one_path(options->keyring_path, options->json, verbose, color_mode);
    }
    while (argi < argc) {
        if (show_one_path(argv[argi], options->json, verbose, color_mode) != 0) status = 1;
        argi += 1;
    }
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

static int first_certificate_fingerprint_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpKeyFirstCertificateContext *ctx = (PgpKeyFirstCertificateContext *)ctx_ptr;
    size_t offset = 0U;
    size_t index;
    static const char hex[] = "0123456789abcdef";

    if (certificate->primary.fingerprint_size == 0U) return 0;
    ctx->is_secret = certificate->primary.tag == 5U;
    for (index = 0U; index < certificate->primary.fingerprint_size; ++index) {
        ctx->fingerprint[offset++] = hex[(certificate->primary.fingerprint[index] >> 4U) & 0x0fU];
        ctx->fingerprint[offset++] = hex[certificate->primary.fingerprint[index] & 0x0fU];
    }
    ctx->fingerprint[offset] = '\0';
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
    unsigned char preferred_compression[] = { 0U };
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

static int build_generated_key(const PgpKeyGenerateOptions *options, PgpKeyGeneratedKey *generated, char *error, size_t error_size) {
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

static int write_generate_json(const PgpKeyGenerateOptions *options, const PgpKeyGeneratedKey *generated) {
    if (tool_json_begin_event(1, "pgpkey", "stdout", "generate") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"algorithm\":\"EdDSA\",\"curve\":\"Ed25519\",\"fingerprint\":\"") != 0) return -1;
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
    if (rt_write_cstr(1, "key-id: ") != 0 || write_hex_bytes(1, generated->primary_info.key_id, PGP_KEY_ID_SIZE) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "secret: ") != 0 || rt_write_line(1, options->secret_out) != 0) return -1;
    if (rt_write_cstr(1, "public: ") != 0 || rt_write_line(1, options->public_out) != 0) return -1;
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
        PgpKeyFirstCertificateContext first;
        int fd;

        rt_memset(&first, 0, sizeof(first));
        if (load_openpgp_file(argv[argi], &data, &data_size, error, sizeof(error)) != 0) {
            pgpkey_write_error_path(error, argv[argi]);
            status = 1;
            argi += 1;
            continue;
        }
        (void)pgp_for_each_certificate(data, data_size, first_certificate_fingerprint_callback, &first, error, sizeof(error));
        if (first.fingerprint[0] == '\0') {
            tool_write_error("pgpkey", "no importable public key in ", argv[argi]);
            rt_free(data);
            status = 1;
            argi += 1;
            continue;
        }
        if (first.is_secret) {
            tool_write_error("pgpkey", "refusing to import private key into public keyring: ", argv[argi]);
            rt_free(data);
            status = 1;
            argi += 1;
            continue;
        }
        if (keyring_contains(options->keyring_path, first.fingerprint)) {
            if (rt_write_cstr(1, "unchanged: ") != 0 || rt_write_line(1, first.fingerprint) != 0) return 1;
            rt_free(data);
            argi += 1;
            continue;
        }
        fd = platform_open_append(options->keyring_path, 0600U);
        if (fd < 0 || write_all_fd(fd, data, data_size) != 0 || platform_close(fd) != 0) {
            if (fd >= 0) (void)platform_close(fd);
            tool_write_error("pgpkey", "cannot write keyring: ", options->keyring_path);
            rt_free(data);
            return 1;
        }
        if (options->json) {
            if (tool_json_begin_event(1, "pgpkey", "stdout", "import") != 0) return 1;
            if (rt_write_cstr(1, ",\"data\":{\"fingerprint\":") != 0 || tool_json_write_string(1, first.fingerprint) != 0) return 1;
            if (rt_write_cstr(1, ",\"keyring\":") != 0 || tool_json_write_string(1, options->keyring_path) != 0 || rt_write_char(1, '}') != 0 || tool_json_end_event(1) != 0) return 1;
        } else if (rt_write_cstr(1, "imported: ") != 0 || rt_write_line(1, first.fingerprint) != 0) {
            return 1;
        }
        rt_free(data);
        argi += 1;
    }
    return status;
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
    const char *command;

    rt_memset(&options, 0, sizeof(options));
    options.color_mode = TOOL_COLOR_AUTO;
    tool_opt_init(&opt, argc, argv, "pgpkey", PGPKEY_USAGE);
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-k") == 0 || rt_strcmp(opt.flag, "--keyring") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.keyring_path = opt.value;
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
    if (options.keyring_path == 0) {
        if (resolve_default_keyring(default_keyring, sizeof(default_keyring)) != 0) {
            tool_write_error("pgpkey", "set HOME, PGPKEY_KEYRING, or pass --keyring", 0);
            return 1;
        }
        options.keyring_path = default_keyring;
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
    if (rt_strcmp(command, "generate") == 0 || rt_strcmp(command, "gen") == 0) {
        return command_generate(&options, argc, argv, opt.argi);
    }
    if (rt_strcmp(command, "import") == 0) {
        return command_import(&options, argc, argv, opt.argi);
    }
    if (rt_strcmp(command, "list") == 0 || rt_strcmp(command, "ls") == 0) {
        if (opt.argi != argc) {
            print_usage();
            return 1;
        }
        return show_one_path(options.keyring_path, options.json, options.verbose, options.color_mode);
    }
    if (rt_strcmp(command, "export") == 0) {
        return command_export(&options, argc, argv, opt.argi);
    }
    tool_write_error("pgpkey", "unknown command: ", command);
    print_usage();
    return 1;
}