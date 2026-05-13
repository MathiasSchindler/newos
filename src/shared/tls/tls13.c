#include "tls/tls13.h"

#include "crypto/hkdf_sha256.h"
#include "crypto/hmac_sha256.h"
#include "runtime.h"

static void store_be16(unsigned char *p, unsigned short v) {
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)(v >> 0);
}

static size_t cstr_len(const char *s) {
	size_t n = 0;
	if (!s) return 0;
	while (s[n]) n++;
	return n;
}

int tls13_hkdf_expand_label(
	const unsigned char secret[CRYPTO_SHA256_DIGEST_SIZE],
	const char *label,
	const unsigned char *context,
	size_t context_len,
	unsigned char *out,
	size_t out_len
) {
	if (!out || out_len == 0) return -1;
	if (!secret) {
		memset(out, 0, out_len);
		return -1;
	}

	// HKDFLabel:
	// struct {
	//   uint16 length;
	//   opaque label<7..255>; // "tls13 " + label
	//   opaque context<0..255>;
	// } HKDFLabel;
	const char *prefix = "tls13 ";
	size_t prefix_len = 6u;
	size_t label_len = cstr_len(label);
	size_t full_label_len = prefix_len + label_len;
	if (full_label_len > 255u) return -1;
	if (context_len > 255u) return -1;
	if (out_len > 0xffffu) return -1;

	unsigned char info[2u + 1u + 255u + 1u + 255u];
	size_t off = 0;
	store_be16(info + off, (unsigned short)out_len);
	off += 2;
	info[off++] = (unsigned char)full_label_len;
	memcpy(info + off, prefix, prefix_len);
	off += prefix_len;
	if (label_len) {
		memcpy(info + off, label, label_len);
		off += label_len;
	}
	info[off++] = (unsigned char)context_len;
	if (context_len) {
		if (!context) return -1;
		memcpy(info + off, context, context_len);
		off += context_len;
	}

	if (crypto_hkdf_sha256_expand(out, out_len, secret, info, off) != 0) return -1;
	memset(info, 0, sizeof(info));
	return 0;
}

int tls13_derive_secret(
	const unsigned char secret[CRYPTO_SHA256_DIGEST_SIZE],
	const char *label,
	const unsigned char transcript_hash[CRYPTO_SHA256_DIGEST_SIZE],
	unsigned char out[CRYPTO_SHA256_DIGEST_SIZE]
) {
	if (!out) return -1;
	if (!transcript_hash) {
		memset(out, 0, CRYPTO_SHA256_DIGEST_SIZE);
		return -1;
	}
	return tls13_hkdf_expand_label(secret, label, transcript_hash, CRYPTO_SHA256_DIGEST_SIZE, out, CRYPTO_SHA256_DIGEST_SIZE);
}

int tls13_finished_key(
	const unsigned char base_key[CRYPTO_SHA256_DIGEST_SIZE],
	unsigned char out[CRYPTO_SHA256_DIGEST_SIZE]
) {
	return tls13_hkdf_expand_label(base_key, "finished", 0, 0, out, CRYPTO_SHA256_DIGEST_SIZE);
}

void tls13_finished_verify_data(
	const unsigned char finished_key[CRYPTO_SHA256_DIGEST_SIZE],
	const unsigned char transcript_hash[CRYPTO_SHA256_DIGEST_SIZE],
	unsigned char out[CRYPTO_SHA256_DIGEST_SIZE]
) {
	if (!out) return;
	if (!finished_key || !transcript_hash) {
		memset(out, 0, CRYPTO_SHA256_DIGEST_SIZE);
		return;
	}
	crypto_hmac_sha256(out, finished_key, CRYPTO_SHA256_DIGEST_SIZE, transcript_hash, CRYPTO_SHA256_DIGEST_SIZE);
}
