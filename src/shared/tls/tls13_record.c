#include "tls/tls13_record.h"

#include "crypto/aes128_gcm.h"
#include "runtime.h"

static void store_be16(unsigned char *p, unsigned short v) {
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)(v >> 0);
}

static unsigned short load_be16(const unsigned char *p) {
	return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static void make_nonce(unsigned char nonce[12], const unsigned char iv[12], unsigned long long seq) {
	memcpy(nonce, iv, 12);
	// nonce = iv XOR seq (seq is 64-bit BE, left-padded to 12 bytes)
	for (int i = 0; i < 8; i++) {
		nonce[11 - i] ^= (unsigned char)(seq >> (8 * i));
	}
}

int tls13_record_encrypt(
	const unsigned char key[16],
	const unsigned char iv[12],
	unsigned long long seq,
	unsigned char inner_type,
	const unsigned char *plaintext, size_t pt_len,
	unsigned char *record_out, size_t record_cap, size_t *record_len_out
) {
	if (!record_out || !record_len_out) return -1;
	if (!plaintext && pt_len) return -1;

	// TLSInnerPlaintext = content || type || padding
	size_t inner_len = pt_len + 1u;
	size_t ct_len = inner_len;
	size_t rec_len = TLS_RECORD_HEADER_SIZE + ct_len + CRYPTO_AES128_GCM_TAG_SIZE;
	if (rec_len > record_cap) return -1;
	if (ct_len > 0xffffu) return -1;

	unsigned char *hdr = record_out;
	hdr[0] = (unsigned char)TLS_CONTENT_APPLICATION_DATA; // TLSCiphertext.opaque_type
	hdr[1] = 0x03;
	hdr[2] = 0x03; // legacy_record_version
	store_be16(hdr + 3, (unsigned short)(ct_len + CRYPTO_AES128_GCM_TAG_SIZE));

	unsigned char nonce[12];
	make_nonce(nonce, iv, seq);

	// Plaintext buffer for AEAD.
	unsigned char *ct = record_out + TLS_RECORD_HEADER_SIZE;
	if (pt_len) memcpy(ct, plaintext, pt_len);
	ct[pt_len] = inner_type;

	unsigned char tag[16];
	if (crypto_aes128_gcm_encrypt(key, nonce, hdr, TLS_RECORD_HEADER_SIZE, ct, inner_len, ct, tag) != 0) return -1;
	memcpy(ct + ct_len, tag, 16);

	*record_len_out = rec_len;
	return 0;
}

int tls13_record_decrypt(
	const unsigned char key[16],
	const unsigned char iv[12],
	unsigned long long seq,
	const unsigned char *record, size_t record_len,
	unsigned char *inner_type_out,
	unsigned char *plaintext_out, size_t plaintext_cap, size_t *pt_len_out
) {
	if (!record || record_len < TLS_RECORD_HEADER_SIZE + CRYPTO_AES128_GCM_TAG_SIZE) return -1;
	if (!inner_type_out || !plaintext_out || !pt_len_out) return -1;

	const unsigned char *hdr = record;
	if (hdr[0] != (unsigned char)TLS_CONTENT_APPLICATION_DATA) return -1;
	if (hdr[1] != 0x03 || hdr[2] != 0x03) return -1;

	unsigned short len16 = load_be16(hdr + 3);
	size_t enc_len = (size_t)len16;
	if (TLS_RECORD_HEADER_SIZE + enc_len != record_len) return -1;
	if (enc_len < CRYPTO_AES128_GCM_TAG_SIZE) return -1;

	size_t ct_len = enc_len - CRYPTO_AES128_GCM_TAG_SIZE;
	const unsigned char *ct = record + TLS_RECORD_HEADER_SIZE;
	const unsigned char *tag = ct + ct_len;

	if (ct_len > plaintext_cap) return -1;

	unsigned char nonce[12];
	make_nonce(nonce, iv, seq);

	// Decrypt into plaintext_out. We keep the full TLSInnerPlaintext first.
	if (crypto_aes128_gcm_decrypt(key, nonce, hdr, TLS_RECORD_HEADER_SIZE, ct, ct_len, tag, plaintext_out) != 0) {
		return -1;
	}

	// Remove zero padding, then split off the trailing content type.
	size_t i = ct_len;
	while (i > 0 && plaintext_out[i - 1] == 0) i--;
	if (i == 0) return -1;
	unsigned char inner_type = plaintext_out[i - 1];
	size_t content_len = i - 1;

	*inner_type_out = inner_type;
	*pt_len_out = content_len;
	return 0;
}
