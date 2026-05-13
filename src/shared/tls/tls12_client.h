#ifndef NEWOS_TLS12_CLIENT_H
#define NEWOS_TLS12_CLIENT_H

#include <stddef.h>

#include "crypto/x509.h"

#define TLS12_MAX_PEER_CERTS 8U
#define TLS12_MAX_PEER_CERT_DER_SIZE 8192U

typedef struct Tls12Client {
    int fd;
    unsigned int timeout_ms;
    int debug;

    unsigned char client_write_key[32];
    unsigned char server_write_key[32];
    unsigned char client_write_iv[4];
    unsigned char server_write_iv[4];
    unsigned long long client_seq;
    unsigned long long server_seq;
    unsigned char pending_app[16384];
    size_t pending_app_len;
    size_t pending_app_offset;
    unsigned char peer_cert_der[TLS12_MAX_PEER_CERTS][TLS12_MAX_PEER_CERT_DER_SIZE];
    size_t peer_cert_len[TLS12_MAX_PEER_CERTS];
    size_t peer_cert_count;

    int handshake_done;
    const char *last_error;
} Tls12Client;

void tls12_client_init(struct Tls12Client *c, int fd, unsigned int timeout_ms);
const char *tls12_client_last_error(const struct Tls12Client *c);
size_t tls12_client_peer_certificates(const struct Tls12Client *c, CryptoX509DerCert *certs, size_t cert_capacity);
int tls12_client_handshake(struct Tls12Client *c, const char *sni, size_t sni_len);
long tls12_client_write_app(struct Tls12Client *c, const unsigned char *buf, size_t len);
long tls12_client_read_app(struct Tls12Client *c, unsigned char *buf, size_t cap);
int tls12_client_close_notify(struct Tls12Client *c);

#endif
