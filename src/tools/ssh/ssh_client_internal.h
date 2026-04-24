#ifndef NEWOS_SSH_CLIENT_INTERNAL_H
#define NEWOS_SSH_CLIENT_INTERNAL_H

/*
 * ssh_client_internal.h - private declarations shared across the ssh_client
 * implementation modules.
 *
 * Nothing outside of src/tools/ssh should include this header.
 */

#include "platform.h"
#include "runtime.h"
#include "ssh_core.h"
#include "ssh_known_hosts.h"
#include "ssh_transport.h"
#include "tool_util.h"

#include <stddef.h>

typedef struct {
    unsigned char seed[32];
    unsigned char public_key[32];
    char source_path[SSH_IDENTITY_PATH_CAPACITY];
    int loaded;
} SshIdentity;

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
