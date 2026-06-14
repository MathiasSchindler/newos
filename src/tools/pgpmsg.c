#include "pgp.h"
#include "runtime.h"
#include "tool_util.h"

#define PGPMSG_USAGE "[--json] COMMAND [ARGS...]\n       pgpmsg inspect [FILE]\n       pgpmsg verify [-k PUBRING] SIGNATURE [FILE]\n       pgpmsg encrypt -r RECIPIENT [-k PUBRING] [-o OUT] [--armor] [FILE]\n       pgpmsg decrypt [-s SECRING] [-o OUT] [FILE]\n       pgpmsg sign -u SIGNER [-s SECRING] [-o OUT] [--armor] [--detach|--cleartext] [FILE]"
#define PGPMSG_ERROR_CAPACITY 160U
#define PGPMSG_KEY_ID_SIZE 8U

typedef struct {
    int json;
    const char *pubring;
    const char *secring;
    const char *output_path;
    const char *recipient;
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

static void print_usage(void) {
    tool_write_usage("pgpmsg", PGPMSG_USAGE);
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

static int parse_signature_packet(PgpMsgSignatureSummary *summary, const unsigned char *body, size_t body_size) {
    size_t hashed_size;
    size_t unhashed_offset;
    size_t unhashed_size;

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
    parse_signature_subpackets(summary, body + 6U, hashed_size);
    unhashed_offset = 6U + hashed_size;
    unhashed_size = read_u16_be(body + unhashed_offset);
    unhashed_offset += 2U;
    if (unhashed_offset + unhashed_size > body_size) return -1;
    parse_signature_subpackets(summary, body + unhashed_offset, unhashed_size);
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

static int write_signature_summary_json(const PgpMsgSignatureSummary *summary) {
    if (tool_json_begin_event(1, "pgpmsg", "stdout", "signature") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"status\":\"not_checked\"") != 0) return -1;
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
    PgpPacketReader reader;
    PgpPacket packet;
    int has_packet = 0;
    char error[PGPMSG_ERROR_CAPACITY];
    int found_signature = 0;

    (void)options->pubring;
    if (argi >= argc || argi + 2 < argc) {
        print_usage();
        return 1;
    }
    if (load_decoded_input(argv[argi], &decoded, &decoded_size) != 0) return 1;
    pgp_packet_reader_init(&reader, decoded, decoded_size);
    while (pgp_packet_reader_next(&reader, &packet, &has_packet, error, sizeof(error)) == 0 && has_packet) {
        if (packet.tag == 2U) {
            PgpMsgSignatureSummary summary;
            if (parse_signature_packet(&summary, decoded + packet.body_offset, packet.body_size) == 0 && summary.present) {
                found_signature = 1;
                if (options->json) {
                    if (write_signature_summary_json(&summary) != 0) { rt_free(decoded); return 1; }
                } else {
                    if (write_signature_summary_text(&summary) != 0) { rt_free(decoded); return 1; }
                }
            }
        }
    }
    rt_free(decoded);
    if (!found_signature) {
        tool_write_error("pgpmsg", "no signature packet found", argv[argi]);
        return 1;
    }
    if (argc - argi == 2 && options->json) {
        if (tool_json_write_diagnostic("pgpmsg", "warning", "detached signature data was accepted but cryptographic verification is not implemented yet", argv[argi + 1]) != 0) return 1;
    } else if (argc - argi == 2) {
        tool_write_error("pgpmsg", "cryptographic verification is not implemented yet for data", argv[argi + 1]);
    } else if (!options->json) {
        tool_write_error("pgpmsg", "cryptographic verification is not implemented yet", argv[argi]);
    }
    return 2;
}

static int unsupported_crypto_command(const char *command, const char *detail) {
    tool_write_error("pgpmsg", detail, command);
    return 2;
}

static int command_encrypt(int argc, char **argv, int argi, const PgpMsgOptions *options) {
    (void)argc;
    (void)argv;
    (void)argi;
    (void)options;
    return unsupported_crypto_command("encrypt", "OpenPGP encryption is not implemented yet");
}

static int command_decrypt(int argc, char **argv, int argi, const PgpMsgOptions *options) {
    (void)argc;
    (void)argv;
    (void)argi;
    (void)options;
    return unsupported_crypto_command("decrypt", "OpenPGP decryption is not implemented yet");
}

static int command_sign(int argc, char **argv, int argi, const PgpMsgOptions *options) {
    (void)argc;
    (void)argv;
    (void)argi;
    (void)options;
    return unsupported_crypto_command("sign", "OpenPGP signing is not implemented yet");
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
            options.recipient = opt.value;
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
