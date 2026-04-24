#ifndef NEWOS_SSH_TRANSPORT_H
#define NEWOS_SSH_TRANSPORT_H

#include "crypto/chacha20_poly1305.h"
#include "crypto/crypto_util.h"
#include "crypto/curve25519.h"
#include "crypto/ed25519.h"
#include "crypto/sha256.h"
#include "crypto/ssh_kdf.h"
#include "platform.h"
#include "runtime.h"
#include "ssh_core.h"

#include <stddef.h>

#define SSH_CLIENT_BANNER_TEXT "SSH-2.0-newos_ssh_0.1"
#define SSH_CLIENT_BANNER_WIRE "SSH-2.0-newos_ssh_0.1\r\n"
#define SSH_SERVER_BANNER_TEXT "SSH-2.0-newos_sshd_0.1"
#define SSH_SERVER_BANNER_WIRE "SSH-2.0-newos_sshd_0.1\r\n"
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

#endif
