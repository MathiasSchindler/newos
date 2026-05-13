#ifndef NEWOS_TLS13_TRANSCRIPT_H
#define NEWOS_TLS13_TRANSCRIPT_H

#include <stddef.h>

#include "crypto/sha256.h"

// TLS 1.3 transcript hash (SHA-256 only).
// The transcript is the concatenation of handshake messages (including
// their 4-byte handshake headers).

struct Tls13Transcript {
	CryptoSha256Context sha;
};

void tls13_transcript_init(struct Tls13Transcript *t);
void tls13_transcript_update(struct Tls13Transcript *t, const unsigned char *data, size_t len);
void tls13_transcript_final(const struct Tls13Transcript *t, unsigned char out[CRYPTO_SHA256_DIGEST_SIZE]);

#endif
