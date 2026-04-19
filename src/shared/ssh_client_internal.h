#ifndef NEWOS_SSH_CLIENT_INTERNAL_H
#define NEWOS_SSH_CLIENT_INTERNAL_H

/*
 * ssh_client_internal.h - private declarations shared across the ssh_client
 * implementation modules.
 *
 * Nothing outside of src/shared/ssh_client*.c should include this header.
 */

#include "crypto/chacha20_poly1305.h"
#include "crypto/crypto_util.h"
#include "crypto/curve25519.h"
#include "crypto/ed25519.h"
#include "crypto/sha256.h"
#include "crypto/ssh_kdf.h"
#include "platform.h"
#include "runtime.h"
#include "ssh_core.h"
#include "ssh_known_hosts.h"
#include "tool_util.h"

#include <stddef.h>

#define SSH_CLIENT_BANNER_TEXT "SSH-2.0-newos_ssh_0.1"
#define SSH_CLIENT_BANNER_WIRE "SSH-2.0-newos_ssh_0.1\r\n"
#define SSH_PACKET_BUFFER_CAPACITY 8192U
#define SSH_PASSWORD_CAPACITY 256U
#define SSH_IDENTITY_PATH_CAPACITY 1024U

typedef struct {
    unsigned char key_c_to_s[64];
    unsigned char key_s_to_c[64];
} SshTransportKeys;

typedef struct {
    SshStringView host_key_blob;
    SshStringView server_public_key;
    SshStringView signature_blob;
} SshEcdhReply;

typedef struct {
    unsigned int local_id;
    unsigned int remote_id;
    unsigned int local_window;
    unsigned int remote_window;
    unsigned int max_packet;
    int eof_sent;
    int close_sent;
} SshChannelState;

typedef struct {
    unsigned char seed[32];
    unsigned char public_key[32];
    char source_path[SSH_IDENTITY_PATH_CAPACITY];
    int loaded;
} SshIdentity;

/* ── ssh_client_io ───────────────────────────────────────────────────────── */

int ssh_write_all(int fd, const void *buffer, size_t count);
int ssh_read_exact(int fd, void *buffer, size_t count);
void ssh_store_be32(unsigned char out[4], unsigned int value);
unsigned int ssh_read_be32(const unsigned char in[4]);
int ssh_copy_view_text(const SshStringView *view, char *buffer, size_t buffer_size);
int ssh_view_equals_text(const SshStringView *view, const char *text);
int ssh_find_text_span(const char *text, size_t text_len, const char *needle, size_t *pos_out);
int ssh_send_packet(int fd, const unsigned char *payload, size_t payload_len);
int ssh_read_packet(int fd, unsigned char *payload, size_t payload_capacity, size_t *payload_len_out);
int ssh_send_encrypted_packet(int fd, const unsigned char key[64], unsigned int seqnr, const unsigned char *payload, size_t payload_len);
int ssh_read_encrypted_packet(int fd, const unsigned char key[64], unsigned int seqnr, unsigned char *payload, size_t payload_capacity, size_t *payload_len_out);
int ssh_read_banner(int fd, char *buffer, size_t buffer_size);

/* ── ssh_client_identity ─────────────────────────────────────────────────── */

int ssh_parse_ed25519_public_key_blob(const SshStringView *blob, unsigned char public_key[32]);
int ssh_parse_ed25519_signature_blob(const SshStringView *blob, unsigned char signature[64]);
int ssh_try_load_identity(const char *identity_path, SshIdentity *identity);

/* ── ssh_client_kex ──────────────────────────────────────────────────────── */

int ssh_compute_curve25519_session_hash(
    const char *server_banner,
    const unsigned char *client_kex_payload,
    size_t client_kex_len,
    const unsigned char *server_kex_payload,
    size_t server_kex_len,
    const SshEcdhReply *reply,
    const unsigned char client_public_key[32],
    const unsigned char shared_secret[32],
    unsigned char out_hash[32]
);
int ssh_parse_ecdh_reply(const unsigned char *payload, size_t payload_len, SshEcdhReply *reply);
int ssh_confirm_host_key(const char *host, unsigned int port, const SshStringView *host_key_blob);

/* ── ssh_client_auth ─────────────────────────────────────────────────────── */

int ssh_authenticate_user(
    int sock,
    const char *user,
    const char *host,
    const char *password_in,
    const unsigned char session_id[32],
    const unsigned char key_c_to_s[64],
    const unsigned char key_s_to_c[64],
    unsigned int *client_seq_io,
    unsigned int *server_seq_io,
    const SshIdentity *identity,
    int verbose
);

/* ── ssh_client_channel ──────────────────────────────────────────────────── */

int ssh_start_interactive_shell(
    int sock,
    const SshTransportKeys *keys,
    unsigned int *client_seq_io,
    unsigned int *server_seq_io,
    int verbose
);

#endif /* NEWOS_SSH_CLIENT_INTERNAL_H */
