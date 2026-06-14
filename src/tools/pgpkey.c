#include "pgp.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PGPKEY_USAGE "[-k KEYRING] [-v] COMMAND [ARGS...]\n       pgpkey show [-v] [FILE ...]\n       pgpkey packets FILE ...\n       pgpkey import FILE ...\n       pgpkey list\n       pgpkey export FINGERPRINT [OUTPUT]"
#define PGPKEY_PATH_CAPACITY 1024U
#define PGPKEY_ERROR_CAPACITY 160U

typedef struct {
    const char *keyring_path;
    int json;
    int verbose;
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

static void pgpkey_set_error(char *error, size_t error_size, const char *message) {
    if (error != 0 && error_size > 0U) {
        rt_copy_string(error, error_size, message != 0 ? message : "pgpkey error");
    }
}

static void print_usage(void) {
    tool_write_usage("pgpkey", PGPKEY_USAGE);
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

static int write_date_plus_seconds(int fd, unsigned long long start, unsigned long long seconds) {
    return write_date(fd, start + seconds);
}

static int write_algorithm_list(int fd, const unsigned char *values, size_t count, const char *(*name_fn)(unsigned int)) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (index != 0U && rt_write_cstr(fd, ", ") != 0) return -1;
        if (rt_write_cstr(fd, name_fn(values[index])) != 0) return -1;
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

static int write_signature_summary_text(const PgpCertificateInfo *certificate) {
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
                if (rt_write_cstr(1, "never") != 0) return -1;
            } else if (write_date_plus_seconds(1, certificate->primary.created, primary_signature->key_expiration_seconds) != 0) {
                return -1;
            }
            if (rt_write_char(1, '\n') != 0) return -1;
        }
        if (primary_signature->preferred_symmetric_count != 0U) {
            if (rt_write_cstr(1, "preferred-symmetric: ") != 0 || write_algorithm_list(1, primary_signature->preferred_symmetric, primary_signature->preferred_symmetric_count, pgp_symmetric_algorithm_name) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
        if (primary_signature->preferred_hash_count != 0U) {
            if (rt_write_cstr(1, "preferred-hash: ") != 0 || write_algorithm_list(1, primary_signature->preferred_hash, primary_signature->preferred_hash_count, pgp_hash_algorithm_name) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
        if (primary_signature->preferred_compression_count != 0U) {
            if (rt_write_cstr(1, "preferred-compression: ") != 0 || write_algorithm_list(1, primary_signature->preferred_compression, primary_signature->preferred_compression_count, pgp_compression_algorithm_name) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
    }
    return 0;
}

static int write_subkey_signature_summary_text(const PgpCertificateInfo *certificate, size_t subkey_index) {
    const PgpSignatureInfo *subkey_signature = latest_self_signature_for_target(certificate, PGP_SIGNATURE_TARGET_SUBKEY, subkey_index, 0);

    if (subkey_signature == 0) return 0;
    if (subkey_signature->key_flags_size != 0U) {
        if (rt_write_cstr(1, "subkey-flags: ") != 0 || write_key_flags(1, subkey_signature->key_flags, subkey_signature->key_flags_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
    }
    if (subkey_signature->has_key_expiration) {
        if (rt_write_cstr(1, "subkey-expires: ") != 0) return -1;
        if (subkey_signature->key_expiration_seconds == 0ULL) {
            if (rt_write_cstr(1, "never") != 0) return -1;
        } else if (write_date_plus_seconds(1, certificate->subkeys[subkey_index].created, subkey_signature->key_expiration_seconds) != 0) {
            return -1;
        }
        if (rt_write_char(1, '\n') != 0) return -1;
    }
    return 0;
}

static int write_signature_verbose_line(const PgpCertificateInfo *certificate, const PgpSignatureInfo *signature) {
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
        if (rt_write_cstr(1, ", signature-expires ") != 0 || write_duration_from_seconds(1, signature->signature_expiration_seconds) != 0) return -1;
    }
    if (signature->has_key_expiration) {
        if (rt_write_cstr(1, ", key-expires-after ") != 0 || write_duration_from_seconds(1, signature->key_expiration_seconds) != 0) return -1;
    }
    if (signature->key_flags_size != 0U) {
        if (rt_write_cstr(1, ", flags ") != 0 || write_key_flags(1, signature->key_flags, signature->key_flags_size) != 0) return -1;
    }
    if (signature->has_primary_user_id) {
        if (rt_write_cstr(1, signature->primary_user_id ? ", primary-uid yes" : ", primary-uid no") != 0) return -1;
    }
    return rt_write_char(1, '\n');
}

static int write_verbose_signatures_text(const PgpCertificateInfo *certificate) {
    size_t index;

    if (certificate->signature_info_count == 0U) return 0;
    if (rt_write_line(1, "signatures:") != 0) return -1;
    for (index = 0U; index < certificate->signature_info_count; ++index) {
        if (write_signature_verbose_line(certificate, &certificate->signatures[index]) != 0) return -1;
    }
    return 0;
}

static int write_certificate_text(const PgpCertificateInfo *certificate, const char *source, size_t index, int verbose) {
    size_t uid_index;
    size_t subkey_index;

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
    if (write_signature_summary_text(certificate) != 0) return -1;
    for (subkey_index = 0U; subkey_index < certificate->subkey_count; ++subkey_index) {
        if (write_key_line(&certificate->subkeys[subkey_index], "subkey") != 0) return -1;
        if (certificate->subkeys[subkey_index].fingerprint_size != 0U) {
            if (rt_write_cstr(1, "subkey-fingerprint: ") != 0 || write_hex_bytes(1, certificate->subkeys[subkey_index].fingerprint, certificate->subkeys[subkey_index].fingerprint_size) != 0 || rt_write_char(1, '\n') != 0) return -1;
        }
        if (write_subkey_signature_summary_text(certificate, subkey_index) != 0) return -1;
    }
    if (rt_write_cstr(1, "packets: ") != 0 || rt_write_uint(1, certificate->packet_count) != 0) return -1;
    if (rt_write_cstr(1, ", signatures: ") != 0 || rt_write_uint(1, certificate->signature_count) != 0) return -1;
    if (rt_write_cstr(1, ", user-attributes: ") != 0 || rt_write_uint(1, certificate->user_attribute_count) != 0) return -1;
    if (rt_write_char(1, '\n') != 0) return -1;
    if (verbose && write_verbose_signatures_text(certificate) != 0) return -1;
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
} PgpKeyShowContext;

static int show_certificate_callback(const PgpCertificateInfo *certificate, void *ctx_ptr) {
    PgpKeyShowContext *ctx = (PgpKeyShowContext *)ctx_ptr;

    ctx->count += 1U;
    if (ctx->json) return write_certificate_json(certificate, ctx->source, ctx->count);
    return write_certificate_text(certificate, ctx->source, ctx->count, ctx->verbose);
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

static int show_one_path(const char *path, int json, int verbose) {
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

    while (argi < argc && (rt_strcmp(argv[argi], "-v") == 0 || rt_strcmp(argv[argi], "--verbose") == 0)) {
        verbose = 1;
        argi += 1;
    }

    if (argi >= argc) {
        return show_one_path(options->keyring_path, options->json, verbose);
    }
    while (argi < argc) {
        if (show_one_path(argv[argi], options->json, verbose) != 0) status = 1;
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
            tool_write_error("pgpkey", error, argv[argi]);
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
    char *buffer = (char *)ctx_ptr;
    size_t offset = 0U;
    size_t index;
    static const char hex[] = "0123456789abcdef";

    if (certificate->primary.fingerprint_size == 0U) return 0;
    for (index = 0U; index < certificate->primary.fingerprint_size; ++index) {
        buffer[offset++] = hex[(certificate->primary.fingerprint[index] >> 4U) & 0x0fU];
        buffer[offset++] = hex[certificate->primary.fingerprint[index] & 0x0fU];
    }
    buffer[offset] = '\0';
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
        char fingerprint[PGP_FINGERPRINT_MAX_SIZE * 2U + 1U];
        int fd;

        fingerprint[0] = '\0';
        if (load_openpgp_file(argv[argi], &data, &data_size, error, sizeof(error)) != 0) {
            tool_write_error("pgpkey", error, argv[argi]);
            status = 1;
            argi += 1;
            continue;
        }
        (void)pgp_for_each_certificate(data, data_size, first_certificate_fingerprint_callback, fingerprint, error, sizeof(error));
        if (fingerprint[0] == '\0') {
            tool_write_error("pgpkey", "no importable public key in ", argv[argi]);
            rt_free(data);
            status = 1;
            argi += 1;
            continue;
        }
        if (keyring_contains(options->keyring_path, fingerprint)) {
            if (rt_write_cstr(1, "unchanged: ") != 0 || rt_write_line(1, fingerprint) != 0) return 1;
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
            if (rt_write_cstr(1, ",\"data\":{\"fingerprint\":") != 0 || tool_json_write_string(1, fingerprint) != 0) return 1;
            if (rt_write_cstr(1, ",\"keyring\":") != 0 || tool_json_write_string(1, options->keyring_path) != 0 || rt_write_char(1, '}') != 0 || tool_json_end_event(1) != 0) return 1;
        } else if (rt_write_cstr(1, "imported: ") != 0 || rt_write_line(1, fingerprint) != 0) {
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
    tool_opt_init(&opt, argc, argv, "pgpkey", PGPKEY_USAGE);
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-k") == 0 || rt_strcmp(opt.flag, "--keyring") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.keyring_path = opt.value;
        } else if (rt_strcmp(opt.flag, "-v") == 0 || rt_strcmp(opt.flag, "--verbose") == 0) {
            options.verbose = 1;
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
    if (rt_strcmp(command, "import") == 0) {
        return command_import(&options, argc, argv, opt.argi);
    }
    if (rt_strcmp(command, "list") == 0 || rt_strcmp(command, "ls") == 0) {
        if (opt.argi != argc) {
            print_usage();
            return 1;
        }
        return show_one_path(options.keyring_path, options.json, options.verbose);
    }
    if (rt_strcmp(command, "export") == 0) {
        return command_export(&options, argc, argv, opt.argi);
    }
    tool_write_error("pgpkey", "unknown command: ", command);
    print_usage();
    return 1;
}