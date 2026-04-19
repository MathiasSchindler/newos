#ifndef NEWOS_SSH_CORE_H
#define NEWOS_SSH_CORE_H

#include <stddef.h>

#define SSH_DEFAULT_PORT 22U
#define SSH_USER_CAPACITY 128
#define SSH_HOST_CAPACITY 256
#define SSH_DESTINATION_CAPACITY 512
#define SSH_BANNER_CAPACITY 256
#define SSH_FINGERPRINT_CAPACITY 96
#define SSH_ALGORITHM_LIST_CAPACITY 128
#define SSH_KEX_COOKIE_SIZE 16U

#define SSH_MSG_DISCONNECT 1U
#define SSH_MSG_IGNORE 2U
#define SSH_MSG_UNIMPLEMENTED 3U
#define SSH_MSG_DEBUG 4U
#define SSH_MSG_SERVICE_REQUEST 5U
#define SSH_MSG_SERVICE_ACCEPT 6U
#define SSH_MSG_EXT_INFO 7U
#define SSH_MSG_KEXINIT 20U
#define SSH_MSG_NEWKEYS 21U
#define SSH_MSG_KEX_ECDH_INIT 30U
#define SSH_MSG_KEX_ECDH_REPLY 31U
#define SSH_MSG_USERAUTH_REQUEST 50U
#define SSH_MSG_USERAUTH_FAILURE 51U
#define SSH_MSG_USERAUTH_SUCCESS 52U
#define SSH_MSG_USERAUTH_BANNER 53U
#define SSH_MSG_USERAUTH_PK_OK 60U
#define SSH_MSG_GLOBAL_REQUEST 80U
#define SSH_MSG_REQUEST_SUCCESS 81U
#define SSH_MSG_REQUEST_FAILURE 82U
#define SSH_MSG_CHANNEL_OPEN 90U
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91U
#define SSH_MSG_CHANNEL_OPEN_FAILURE 92U
#define SSH_MSG_CHANNEL_WINDOW_ADJUST 93U
#define SSH_MSG_CHANNEL_DATA 94U
#define SSH_MSG_CHANNEL_EXTENDED_DATA 95U
#define SSH_MSG_CHANNEL_EOF 96U
#define SSH_MSG_CHANNEL_CLOSE 97U
#define SSH_MSG_CHANNEL_REQUEST 98U
#define SSH_MSG_CHANNEL_SUCCESS 99U
#define SSH_MSG_CHANNEL_FAILURE 100U

typedef struct {
    const unsigned char *data;
    size_t length;
} SshStringView;

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t offset;
} SshCursor;

typedef struct {
    unsigned char *data;
    size_t capacity;
    size_t length;
} SshBuilder;

typedef struct {
    char user[SSH_USER_CAPACITY];
    char host[SSH_HOST_CAPACITY];
    unsigned int port;
    int has_user;
} SshDestination;

typedef struct {
    char kex_algorithms[SSH_ALGORITHM_LIST_CAPACITY];
    char host_key_algorithms[SSH_ALGORITHM_LIST_CAPACITY];
    char ciphers_c_to_s[SSH_ALGORITHM_LIST_CAPACITY];
    char ciphers_s_to_c[SSH_ALGORITHM_LIST_CAPACITY];
    char macs_c_to_s[SSH_ALGORITHM_LIST_CAPACITY];
    char macs_s_to_c[SSH_ALGORITHM_LIST_CAPACITY];
    char compression_c_to_s[SSH_ALGORITHM_LIST_CAPACITY];
    char compression_s_to_c[SSH_ALGORITHM_LIST_CAPACITY];
} SshAlgorithmProfile;

typedef struct {
    unsigned char cookie[SSH_KEX_COOKIE_SIZE];
    SshStringView kex_algorithms;
    SshStringView host_key_algorithms;
    SshStringView ciphers_c_to_s;
    SshStringView ciphers_s_to_c;
    SshStringView macs_c_to_s;
    SshStringView macs_s_to_c;
    SshStringView compression_c_to_s;
    SshStringView compression_s_to_c;
    SshStringView languages_c_to_s;
    SshStringView languages_s_to_c;
    unsigned char first_kex_packet_follows;
    unsigned int reserved;
} SshKexInitMessage;

void ssh_cursor_init(SshCursor *cursor, const void *data, size_t length);
size_t ssh_cursor_remaining(const SshCursor *cursor);
int ssh_cursor_take_u8(SshCursor *cursor, unsigned char *value_out);
int ssh_cursor_take_u32(SshCursor *cursor, unsigned int *value_out);
int ssh_cursor_take_bytes(SshCursor *cursor, const unsigned char **data_out, size_t length);
int ssh_cursor_take_string(SshCursor *cursor, SshStringView *view_out);

void ssh_builder_init(SshBuilder *builder, unsigned char *buffer, size_t capacity);
int ssh_builder_put_u8(SshBuilder *builder, unsigned char value);
int ssh_builder_put_u32(SshBuilder *builder, unsigned int value);
int ssh_builder_put_bytes(SshBuilder *builder, const unsigned char *data, size_t length);
int ssh_builder_put_string(SshBuilder *builder, const unsigned char *data, size_t length);
int ssh_builder_put_cstring(SshBuilder *builder, const char *text);

void ssh_profile_init_default_client(SshAlgorithmProfile *profile);
void ssh_profile_init_default_server(SshAlgorithmProfile *profile);
int ssh_build_kexinit_payload(
    const SshAlgorithmProfile *profile,
    const unsigned char cookie[SSH_KEX_COOKIE_SIZE],
    unsigned char *buffer,
    size_t buffer_size,
    size_t *length_out
);
int ssh_parse_kexinit_payload(const unsigned char *payload, size_t payload_length, SshKexInitMessage *message_out);

int ssh_validate_banner_line(const char *line);
int ssh_parse_destination(const char *text, const char *default_user, unsigned int default_port, SshDestination *out);
int ssh_format_destination(const SshDestination *destination, int include_user, char *buffer, size_t buffer_size);
int ssh_name_list_contains(const SshStringView *list, const char *name);
int ssh_select_algorithm(const char *preferred_csv, const SshStringView *peer_list, char *buffer, size_t buffer_size);

int ssh_base64_encode(const unsigned char *data, size_t length, char *buffer, size_t buffer_size, size_t *length_out);
int ssh_base64_decode(const char *text, unsigned char *buffer, size_t buffer_size, size_t *length_out);
int ssh_format_fingerprint_sha256(const unsigned char *data, size_t length, char *buffer, size_t buffer_size);

#endif
