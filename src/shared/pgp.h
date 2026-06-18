#ifndef NEWOS_PGP_H
#define NEWOS_PGP_H

#include <stddef.h>

#define PGP_FINGERPRINT_MAX_SIZE 32U
#define PGP_KEY_ID_SIZE 8U
#define PGP_USER_ID_CAPACITY 160U
#define PGP_MAX_USER_IDS 32U
#define PGP_MAX_SUBKEYS 32U
#define PGP_MAX_SIGNATURES 4096U
#define PGP_MAX_SIGNATURE_INFOS 256U
#define PGP_SIGNATURE_PREFERENCE_CAPACITY 16U
#define PGP_SIGNATURE_FEATURE_CAPACITY 8U
#define PGP_SIGNATURE_KEY_FLAGS_CAPACITY 8U
#define PGP_PUBLIC_MATERIAL_CAPACITY 1024U

#define PGP_SIGNATURE_TARGET_PRIMARY 6U
#define PGP_SIGNATURE_TARGET_USER_ID 13U
#define PGP_SIGNATURE_TARGET_SUBKEY 14U
#define PGP_SIGNATURE_TARGET_USER_ATTRIBUTE 17U

typedef struct {
    unsigned int tag;
    unsigned int version;
    unsigned int algorithm;
    unsigned int bits;
    unsigned long long created;
    unsigned char fingerprint[PGP_FINGERPRINT_MAX_SIZE];
    size_t fingerprint_size;
    unsigned char key_id[PGP_KEY_ID_SIZE];
    unsigned char public_material[PGP_PUBLIC_MATERIAL_CAPACITY];
    size_t public_material_size;
    int present;
} PgpPublicKeyInfo;

typedef struct {
    unsigned int version;
    unsigned int signature_type;
    unsigned int public_key_algorithm;
    unsigned int hash_algorithm;
    unsigned int target_tag;
    size_t target_index;
    unsigned long long packet_index;
    unsigned long long created;
    unsigned long long key_expiration_seconds;
    unsigned long long signature_expiration_seconds;
    int has_key_expiration;
    int has_signature_expiration;
    int has_primary_user_id;
    int primary_user_id;
    unsigned char key_flags[PGP_SIGNATURE_KEY_FLAGS_CAPACITY];
    size_t key_flags_size;
    unsigned char preferred_symmetric[PGP_SIGNATURE_PREFERENCE_CAPACITY];
    size_t preferred_symmetric_count;
    unsigned char preferred_hash[PGP_SIGNATURE_PREFERENCE_CAPACITY];
    size_t preferred_hash_count;
    unsigned char preferred_compression[PGP_SIGNATURE_PREFERENCE_CAPACITY];
    size_t preferred_compression_count;
    unsigned char features[PGP_SIGNATURE_FEATURE_CAPACITY];
    size_t feature_count;
    unsigned char issuer_key_id[PGP_KEY_ID_SIZE];
    int has_issuer_key_id;
    unsigned char issuer_fingerprint[PGP_FINGERPRINT_MAX_SIZE];
    size_t issuer_fingerprint_size;
    int present;
} PgpSignatureInfo;

typedef struct {
    PgpPublicKeyInfo primary;
    PgpPublicKeyInfo subkeys[PGP_MAX_SUBKEYS];
    size_t subkey_count;
    char user_ids[PGP_MAX_USER_IDS][PGP_USER_ID_CAPACITY];
    size_t user_id_count;
    PgpSignatureInfo signatures[PGP_MAX_SIGNATURE_INFOS];
    size_t signature_info_count;
    unsigned long long signature_count;
    unsigned long long user_attribute_count;
    unsigned long long packet_count;
    size_t start_offset;
    size_t end_offset;
} PgpCertificateInfo;

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t offset;
} PgpPacketReader;

typedef struct {
    unsigned int tag;
    int new_format;
    size_t header_offset;
    size_t body_offset;
    size_t body_size;
} PgpPacket;

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} PgpBuffer;

typedef int (*PgpCertificateCallback)(const PgpCertificateInfo *certificate, void *ctx);

int pgp_decode_input(const unsigned char *input, size_t input_size, unsigned char **data_out, size_t *size_out, char *error, size_t error_size);
int pgp_normalize_packets(const unsigned char *input, size_t input_size, unsigned char **data_out, size_t *size_out, char *error, size_t error_size);
int pgp_write_public_key_armor(int fd, const unsigned char *data, size_t size);
int pgp_write_private_key_armor(int fd, const unsigned char *data, size_t size);
int pgp_write_message_armor(int fd, const unsigned char *data, size_t size);
int pgp_write_signature_armor(int fd, const unsigned char *data, size_t size);
int pgp_write_new_packet_header(int fd, unsigned int tag);
int pgp_write_packet_length(int fd, size_t length);
int pgp_write_partial_body_length(int fd, size_t length);
int pgp_write_date(int fd, unsigned long long epoch);
void pgp_buffer_free(PgpBuffer *buffer);
int pgp_buffer_reserve(PgpBuffer *buffer, size_t extra);
int pgp_buffer_append_byte(PgpBuffer *buffer, unsigned int value);
int pgp_buffer_append_data(PgpBuffer *buffer, const unsigned char *data, size_t size);
int pgp_buffer_append_u16_be(PgpBuffer *buffer, unsigned int value);
int pgp_buffer_append_u32_be(PgpBuffer *buffer, unsigned long long value);
int pgp_buffer_append_packet_length(PgpBuffer *buffer, size_t length);
int pgp_buffer_append_packet(PgpBuffer *buffer, unsigned int tag, const PgpBuffer *body);
int pgp_buffer_append_signature_subpacket(PgpBuffer *buffer, unsigned int type, const unsigned char *body, size_t body_size);
int pgp_buffer_append_signature_subpacket_u32(PgpBuffer *buffer, unsigned int type, unsigned long long value);
int pgp_buffer_append_opaque_mpi(PgpBuffer *buffer, const unsigned char *data, size_t size, unsigned int bit_count);
void pgp_packet_reader_init(PgpPacketReader *reader, const unsigned char *data, size_t size);
int pgp_packet_reader_next(PgpPacketReader *reader, PgpPacket *packet_out, int *has_packet_out, char *error, size_t error_size);
int pgp_parse_public_key_packet(PgpPublicKeyInfo *info, unsigned int tag, const unsigned char *body, size_t body_size, char *error, size_t error_size);
int pgp_for_each_certificate(const unsigned char *data, size_t size, PgpCertificateCallback callback, void *ctx, char *error, size_t error_size);
int pgp_parse_fingerprint_text(const char *text, unsigned char out[PGP_FINGERPRINT_MAX_SIZE], size_t *size_out);
int pgp_fingerprint_matches_text(const PgpPublicKeyInfo *key, const char *text);
const char *pgp_packet_tag_name(unsigned int tag);
const char *pgp_public_key_algorithm_name(unsigned int algorithm);
const char *pgp_signature_type_name(unsigned int signature_type);
const char *pgp_hash_algorithm_name(unsigned int algorithm);
const char *pgp_symmetric_algorithm_name(unsigned int algorithm);
const char *pgp_compression_algorithm_name(unsigned int algorithm);
const char *pgp_key_kind_name(unsigned int tag);

#endif