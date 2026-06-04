#ifndef NEWOS_TLS13_RECORD_H
#define NEWOS_TLS13_RECORD_H

#include <stddef.h>

// TLS 1.3 record layer helpers (AES-128-GCM).
//
// These helpers implement the TLSCiphertext framing and TLSInnerPlaintext
// padding/type scheme (no padding added currently).

#define TLS_RECORD_HEADER_SIZE 5
#define TLS13_RECORD_PLAINTEXT_CAPACITY 65536U

#define TLS_CONTENT_CHANGE_CIPHER_SPEC 20
#define TLS_CONTENT_ALERT 21
#define TLS_CONTENT_HANDSHAKE 22
#define TLS_CONTENT_APPLICATION_DATA 23

// Encrypts a TLS 1.3 record.
//
// outer type is always application_data (23) for encrypted TLS 1.3 records.
// legacy_record_version is 0x0303.
//
// Returns 0 on success, -1 on error.
int tls13_record_encrypt(
	const unsigned char key[16],
	const unsigned char iv[12],
	unsigned long long seq,
	unsigned char inner_type,
	const unsigned char *plaintext, size_t pt_len,
	unsigned char *record_out, size_t record_cap, size_t *record_len_out
);

// Decrypts a TLS 1.3 record produced by tls13_record_encrypt.
//
// On success, writes the inner content type to inner_type_out and the
// plaintext (without the trailing type/padding) to plaintext_out.
// Returns 0 on success, -1 on auth failure or parse error.
int tls13_record_decrypt(
	const unsigned char key[16],
	const unsigned char iv[12],
	unsigned long long seq,
	const unsigned char *record, size_t record_len,
	unsigned char *inner_type_out,
	unsigned char *plaintext_out, size_t *pt_len_out
);

#endif
