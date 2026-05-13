#ifndef NEWOS_TLS13_H
#define NEWOS_TLS13_H

#include <stddef.h>

#include "crypto/sha256.h"

// TLS 1.3 key schedule helpers (SHA-256 only).
// Implements RFC 8446 HKDF-Expand-Label / Derive-Secret and Finished.

// HKDF-Expand-Label(Secret, Label, Context, Length)
// Label is the TLS label without the "tls13 " prefix.
// Returns 0 on success, -1 on error.
int tls13_hkdf_expand_label(
	const unsigned char secret[CRYPTO_SHA256_DIGEST_SIZE],
	const char *label,
	const unsigned char *context,
	size_t context_len,
	unsigned char *out,
	size_t out_len
);

// Derive-Secret(Secret, Label, Messages)
// Here, transcript_hash is Hash(Messages).
// Returns 0 on success, -1 on error.
int tls13_derive_secret(
	const unsigned char secret[CRYPTO_SHA256_DIGEST_SIZE],
	const char *label,
	const unsigned char transcript_hash[CRYPTO_SHA256_DIGEST_SIZE],
	unsigned char out[CRYPTO_SHA256_DIGEST_SIZE]
);

// finished_key = HKDF-Expand-Label(base_key, "finished", "", Hash.length)
// Returns 0 on success, -1 on error.
int tls13_finished_key(
	const unsigned char base_key[CRYPTO_SHA256_DIGEST_SIZE],
	unsigned char out[CRYPTO_SHA256_DIGEST_SIZE]
);

// verify_data = HMAC(finished_key, transcript_hash)
void tls13_finished_verify_data(
	const unsigned char finished_key[CRYPTO_SHA256_DIGEST_SIZE],
	const unsigned char transcript_hash[CRYPTO_SHA256_DIGEST_SIZE],
	unsigned char out[CRYPTO_SHA256_DIGEST_SIZE]
);

#endif
