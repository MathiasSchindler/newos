#include "tls/tls13_transcript.h"

#include "runtime.h"

void tls13_transcript_init(struct Tls13Transcript *t) {
	if (!t) return;
	crypto_sha256_init(&t->sha);
}

void tls13_transcript_update(struct Tls13Transcript *t, const unsigned char *data, size_t len) {
	if (!t) return;
	if (!data && len) return;
	crypto_sha256_update(&t->sha, data, len);
}

void tls13_transcript_final(const struct Tls13Transcript *t, unsigned char out[CRYPTO_SHA256_DIGEST_SIZE]) {
	if (!out) return;
	if (!t) {
		memset(out, 0, CRYPTO_SHA256_DIGEST_SIZE);
		return;
	}
	CryptoSha256Context tmp = t->sha;
	crypto_sha256_final(&tmp, out);
	memset(&tmp, 0, sizeof(tmp));
}
