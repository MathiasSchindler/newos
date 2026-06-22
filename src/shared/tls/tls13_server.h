#ifndef NEWOS_TLS13_SERVER_H
#define NEWOS_TLS13_SERVER_H

#include <stddef.h>

#include "crypto/rsa.h"

#define TLS13_SERVER_MAX_CERT_DER_SIZE 8192U

typedef struct {
    const unsigned char *cert_der;
    size_t cert_der_len;
    const CryptoRsaPrivateKey *rsa_key;
} Tls13ServerCredentials;

typedef struct Tls13Server {
    int fd;
    unsigned int timeout_ms;
    int debug;
    Tls13ServerCredentials credentials;

    unsigned char c_ap_key[16];
    unsigned char c_ap_iv[12];
    unsigned char s_ap_key[16];
    unsigned char s_ap_iv[12];
    unsigned long long c_ap_seq;
    unsigned long long s_ap_seq;
    unsigned char pending_app[16384];
    size_t pending_app_len;
    size_t pending_app_offset;

    int handshake_done;
    const char *last_error;
} Tls13Server;

void tls13_server_init(Tls13Server *server, int fd, const Tls13ServerCredentials *credentials, unsigned int timeout_ms);
const char *tls13_server_last_error(const Tls13Server *server);
int tls13_server_handshake(Tls13Server *server);
long tls13_server_write_app(Tls13Server *server, const unsigned char *buffer, size_t length);
long tls13_server_read_app(Tls13Server *server, unsigned char *buffer, size_t capacity);
int tls13_server_close_notify(Tls13Server *server);

#endif
