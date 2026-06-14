#ifndef NEWOS_CRYPTO_AES128_H
#define NEWOS_CRYPTO_AES128_H

#include <stddef.h>

#define CRYPTO_AES128_KEY_SIZE 16u
#define CRYPTO_AES256_KEY_SIZE 32u
#define CRYPTO_AES128_BLOCK_SIZE 16u
#define CRYPTO_AES256_BLOCK_SIZE 16u
#define CRYPTO_AES128_ROUNDS 10u
#define CRYPTO_AES256_ROUNDS 14u

typedef struct {
	/* 11 round keys x 16 bytes = 176 bytes = 44 u32 words. */
	unsigned int rk[44];
} CryptoAes128Context;

typedef struct {
	/* 15 round keys x 16 bytes = 240 bytes = 60 u32 words. */
	unsigned int rk[60];
} CryptoAes256Context;

void crypto_aes128_init(CryptoAes128Context *ctx, const unsigned char key[CRYPTO_AES128_KEY_SIZE]);
void crypto_aes256_init(CryptoAes256Context *ctx, const unsigned char key[CRYPTO_AES256_KEY_SIZE]);

void crypto_aes128_encrypt_block(const CryptoAes128Context *ctx, const unsigned char in[CRYPTO_AES128_BLOCK_SIZE], unsigned char out[CRYPTO_AES128_BLOCK_SIZE]);
void crypto_aes128_decrypt_block(const CryptoAes128Context *ctx, const unsigned char in[CRYPTO_AES128_BLOCK_SIZE], unsigned char out[CRYPTO_AES128_BLOCK_SIZE]);
void crypto_aes256_encrypt_block(const CryptoAes256Context *ctx, const unsigned char in[CRYPTO_AES256_BLOCK_SIZE], unsigned char out[CRYPTO_AES256_BLOCK_SIZE]);
void crypto_aes256_decrypt_block(const CryptoAes256Context *ctx, const unsigned char in[CRYPTO_AES256_BLOCK_SIZE], unsigned char out[CRYPTO_AES256_BLOCK_SIZE]);

#endif
