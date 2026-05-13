#ifndef NEWOS_TLS13_CLIENT_H
#define NEWOS_TLS13_CLIENT_H

#include <stddef.h>

#include "crypto/x509.h"

#define TLS13_MAX_PEER_CERTS 8U
#define TLS13_MAX_PEER_CERT_DER_SIZE 8192U

// Minimal TLS 1.3 client state for tool-to-core reuse.
//
// This is intentionally small and pragmatic:
// - Certificate chain and hostname validation are handled by platform wrappers
// - Only the subset needed by existing tls13 tool and higher-level clients
// - IPv6/DNS/connect handled by tools; core operates on an already-connected fd

typedef struct Tls13Client {
	int fd;
	unsigned int timeout_ms;
	int debug;

	unsigned char c_ap_key[16];
	unsigned char c_ap_iv[12];
	unsigned char s_ap_key[16];
	unsigned char s_ap_iv[12];
	unsigned long long c_ap_seq;
	unsigned long long s_ap_seq;
	unsigned char pending_app[16384];
	size_t pending_app_len;
	size_t pending_app_offset;
	unsigned char peer_cert_der[TLS13_MAX_PEER_CERTS][TLS13_MAX_PEER_CERT_DER_SIZE];
	size_t peer_cert_len[TLS13_MAX_PEER_CERTS];
	size_t peer_cert_count;

	int handshake_done;
	const char *last_error;
} Tls13Client;

void tls13_client_init(struct Tls13Client *c, int fd, unsigned int timeout_ms);
const char *tls13_client_last_error(const struct Tls13Client *c);
size_t tls13_client_peer_certificates(const struct Tls13Client *c, CryptoX509DerCert *certs, size_t cert_capacity);

// Performs a live TLS 1.3 handshake on c->fd and derives application traffic keys.
// sni may be NULL to omit SNI; if provided, length must be 1..255.
// Returns 0 on success, -1 on error.
int tls13_client_handshake(struct Tls13Client *c, const char *sni, size_t sni_len);

// Encrypts and writes application data.
// Returns total bytes written from plaintext (len) on success, -1 on error.
long tls13_client_write_app(struct Tls13Client *c, const unsigned char *buf, size_t len);

// Reads and decrypts the next application-data plaintext chunk.
// Returns:
// - >0: number of plaintext bytes written to buf
// - 0: orderly TLS close (close_notify) / EOF
// - -1: error
long tls13_client_read_app(struct Tls13Client *c, unsigned char *buf, size_t cap);

// Sends an encrypted close_notify alert.
// Returns 0 on success, -1 on error.
int tls13_client_close_notify(struct Tls13Client *c);

#endif
