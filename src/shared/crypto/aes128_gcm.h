#ifndef NEWOS_CRYPTO_AES128_GCM_H
#define NEWOS_CRYPTO_AES128_GCM_H

#include <stddef.h>
#include "crypto/aes128.h"

#define CRYPTO_AES128_GCM_TAG_SIZE 16
#define CRYPTO_AES128_GCM_IV_SIZE 12
#define CRYPTO_AES256_GCM_TAG_SIZE 16
#define CRYPTO_AES256_GCM_IV_SIZE 12

typedef struct {
	CryptoAes256Context aes;
	unsigned char h[16];
	unsigned char ghash_table[32][16][16];
} CryptoAes256GcmContext;

// AES-128-GCM (AEAD) one-shot helpers.
//
// Returns 0 on success.
int crypto_aes128_gcm_encrypt(
	const unsigned char key[16],
	const unsigned char iv[CRYPTO_AES128_GCM_IV_SIZE],
	const unsigned char *aad, size_t aad_len,
	const unsigned char *plaintext, size_t pt_len,
	unsigned char *ciphertext,
	unsigned char tag[CRYPTO_AES128_GCM_TAG_SIZE]
);

// Returns 0 on success, -1 on authentication failure.
int crypto_aes128_gcm_decrypt(
	const unsigned char key[16],
	const unsigned char iv[CRYPTO_AES128_GCM_IV_SIZE],
	const unsigned char *aad, size_t aad_len,
	const unsigned char *ciphertext, size_t ct_len,
	const unsigned char tag[CRYPTO_AES128_GCM_TAG_SIZE],
	unsigned char *plaintext
);

int crypto_aes256_gcm_encrypt(
	const unsigned char key[32],
	const unsigned char iv[CRYPTO_AES256_GCM_IV_SIZE],
	const unsigned char *aad, size_t aad_len,
	const unsigned char *plaintext, size_t pt_len,
	unsigned char *ciphertext,
	unsigned char tag[CRYPTO_AES256_GCM_TAG_SIZE]
);

int crypto_aes256_gcm_decrypt(
	const unsigned char key[32],
	const unsigned char iv[CRYPTO_AES256_GCM_IV_SIZE],
	const unsigned char *aad, size_t aad_len,
	const unsigned char *ciphertext, size_t ct_len,
	const unsigned char tag[CRYPTO_AES256_GCM_TAG_SIZE],
	unsigned char *plaintext
);

void crypto_aes256_gcm_context_init(CryptoAes256GcmContext *ctx, const unsigned char key[32]);
void crypto_aes256_gcm_context_clear(CryptoAes256GcmContext *ctx);

int crypto_aes256_gcm_context_encrypt(
	const CryptoAes256GcmContext *ctx,
	const unsigned char iv[CRYPTO_AES256_GCM_IV_SIZE],
	const unsigned char *aad, size_t aad_len,
	const unsigned char *plaintext, size_t pt_len,
	unsigned char *ciphertext,
	unsigned char tag[CRYPTO_AES256_GCM_TAG_SIZE]
);

int crypto_aes256_gcm_context_decrypt(
	const CryptoAes256GcmContext *ctx,
	const unsigned char iv[CRYPTO_AES256_GCM_IV_SIZE],
	const unsigned char *aad, size_t aad_len,
	const unsigned char *ciphertext, size_t ct_len,
	const unsigned char tag[CRYPTO_AES256_GCM_TAG_SIZE],
	unsigned char *plaintext
);

#endif
