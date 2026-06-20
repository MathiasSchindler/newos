#include "crypto/aes128_gcm.h"
#include "crypto/aes128.h"
#include "crypto/crypto_util.h"
#include "runtime.h"

static void xor16(unsigned char out[16], const unsigned char a[16], const unsigned char b[16]) {
	for (int i = 0; i < 16; i++) out[i] = (unsigned char)(a[i] ^ b[i]);
}

static void store_be64(unsigned char *p, unsigned long long v) {
	p[0] = (unsigned char)(v >> 56);
	p[1] = (unsigned char)(v >> 48);
	p[2] = (unsigned char)(v >> 40);
	p[3] = (unsigned char)(v >> 32);
	p[4] = (unsigned char)(v >> 24);
	p[5] = (unsigned char)(v >> 16);
	p[6] = (unsigned char)(v >> 8);
	p[7] = (unsigned char)(v >> 0);
}

// Increment the low 32 bits, treated as a big-endian integer.
static void inc32(unsigned char counter[16]) {
	unsigned int x = ((unsigned int)counter[12] << 24) | ((unsigned int)counter[13] << 16) | ((unsigned int)counter[14] << 8) | (unsigned int)counter[15];
	x++;
	counter[12] = (unsigned char)(x >> 24);
	counter[13] = (unsigned char)(x >> 16);
	counter[14] = (unsigned char)(x >> 8);
	counter[15] = (unsigned char)(x >> 0);
}

static void shift_right_one(unsigned char v[16]) {
	unsigned char carry = 0;
	for (int i = 0; i < 16; i++) {
		unsigned char b = v[i];
		unsigned char new_carry = (unsigned char)(b & 1u);
		v[i] = (unsigned char)((b >> 1) | (carry ? 0x80u : 0x00u));
		carry = new_carry;
	}
}

// Multiply X by H in GF(2^128) with the GCM reduction polynomial.
// Inputs/outputs are 16-byte big-endian values.
static void ghash_mul(unsigned char x[16], const unsigned char h[16]) {
	unsigned char z[16];
	unsigned char v[16];
	memset(z, 0, sizeof(z));
	memcpy(v, h, sizeof(v));

	for (int byte = 0; byte < 16; byte++) {
		unsigned char xb = x[byte];
		for (int bit = 0; bit < 8; bit++) {
			if (xb & 0x80u) {
				for (int i = 0; i < 16; i++) z[i] = (unsigned char)(z[i] ^ v[i]);
			}
			unsigned char lsb = (unsigned char)(v[15] & 1u);
			shift_right_one(v);
			if (lsb) {
				// R = 0xe1 || 0^120
				v[0] = (unsigned char)(v[0] ^ 0xe1u);
			}
			xb = (unsigned char)(xb << 1);
		}
	}

	memcpy(x, z, 16);
}

static void ghash_build_table(unsigned char table[32][16][16], const unsigned char h[16]) {
	unsigned char v[16];
	unsigned char powers[4][16];

	memset(table, 0, 32U * 16U * 16U);
	memcpy(v, h, sizeof(v));
	for (int position = 0; position < 32; position++) {
		for (int bit = 0; bit < 4; bit++) {
			memcpy(powers[bit], v, 16);
			unsigned char lsb = (unsigned char)(v[15] & 1u);
			shift_right_one(v);
			if (lsb) v[0] = (unsigned char)(v[0] ^ 0xe1u);
		}
		for (int nibble = 1; nibble < 16; nibble++) {
			for (int bit = 0; bit < 4; bit++) {
				if (nibble & (8 >> bit)) {
					for (int i = 0; i < 16; i++) table[position][nibble][i] = (unsigned char)(table[position][nibble][i] ^ powers[bit][i]);
				}
			}
		}
	}
	crypto_secure_bzero(powers, sizeof(powers));
	crypto_secure_bzero(v, sizeof(v));
}

static void ghash_mul_table(unsigned char x[16], const unsigned char table[32][16][16]) {
	unsigned char z[16];
	memset(z, 0, sizeof(z));
	for (int byte = 0; byte < 16; byte++) {
		unsigned int high = (unsigned int)(x[byte] >> 4);
		unsigned int low = (unsigned int)(x[byte] & 0x0fu);
		const unsigned char *high_entry = table[byte * 2][high];
		const unsigned char *low_entry = table[byte * 2 + 1][low];
		for (int i = 0; i < 16; i++) z[i] = (unsigned char)(z[i] ^ high_entry[i] ^ low_entry[i]);
	}
	memcpy(x, z, 16);
}

static void ghash_update(unsigned char y[16], const unsigned char h[16], const unsigned char *data, size_t len) {
	unsigned char block[16];
	while (len >= 16) {
		for (int i = 0; i < 16; i++) y[i] = (unsigned char)(y[i] ^ data[i]);
		ghash_mul(y, h);
		data += 16;
		len -= 16;
	}
	if (len) {
		memset(block, 0, sizeof(block));
		memcpy(block, data, len);
		for (int i = 0; i < 16; i++) y[i] = (unsigned char)(y[i] ^ block[i]);
		ghash_mul(y, h);
	}
}

static void ghash_final_lengths(unsigned char y[16], const unsigned char h[16], size_t aad_len, size_t ct_len) {
	unsigned char lens[16];
	unsigned long long a_bits = (unsigned long long)aad_len * 8u;
	unsigned long long c_bits = (unsigned long long)ct_len * 8u;
	store_be64(lens + 0, a_bits);
	store_be64(lens + 8, c_bits);
	for (int i = 0; i < 16; i++) y[i] = (unsigned char)(y[i] ^ lens[i]);
	ghash_mul(y, h);
}

static void ghash_update_table(unsigned char y[16], const unsigned char table[32][16][16], const unsigned char *data, size_t len) {
	unsigned char block[16];
	while (len >= 16) {
		for (int i = 0; i < 16; i++) y[i] = (unsigned char)(y[i] ^ data[i]);
		ghash_mul_table(y, table);
		data += 16;
		len -= 16;
	}
	if (len) {
		memset(block, 0, sizeof(block));
		memcpy(block, data, len);
		for (int i = 0; i < 16; i++) y[i] = (unsigned char)(y[i] ^ block[i]);
		ghash_mul_table(y, table);
	}
}

static void ghash_final_lengths_table(unsigned char y[16], const unsigned char table[32][16][16], size_t aad_len, size_t ct_len) {
	unsigned char lens[16];
	unsigned long long a_bits = (unsigned long long)aad_len * 8u;
	unsigned long long c_bits = (unsigned long long)ct_len * 8u;
	store_be64(lens + 0, a_bits);
	store_be64(lens + 8, c_bits);
	for (int i = 0; i < 16; i++) y[i] = (unsigned char)(y[i] ^ lens[i]);
	ghash_mul_table(y, table);
}

static void aes128_ctr_xor(const CryptoAes128Context *aes, unsigned char counter[16], const unsigned char *in, unsigned char *out, size_t len) {
	unsigned char stream[16];
	while (len) {
		crypto_aes128_encrypt_block(aes, counter, stream);
		inc32(counter);
		size_t n = (len < 16) ? len : 16;
		for (size_t i = 0; i < n; i++) out[i] = (unsigned char)(in[i] ^ stream[i]);
		in += n;
		out += n;
		len -= n;
	}
}

static void aes256_ctr_xor(const CryptoAes256Context *aes, unsigned char counter[16], const unsigned char *in, unsigned char *out, size_t len) {
	unsigned char stream[16];
	while (len) {
		crypto_aes256_encrypt_block(aes, counter, stream);
		inc32(counter);
		size_t n = (len < 16) ? len : 16;
		for (size_t i = 0; i < n; i++) out[i] = (unsigned char)(in[i] ^ stream[i]);
		in += n;
		out += n;
		len -= n;
	}
}

static int ct_memeq(const unsigned char *a, const unsigned char *b, size_t n) {
	unsigned char diff = 0;
	for (size_t i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

int crypto_aes128_gcm_encrypt(
	const unsigned char key[16],
	const unsigned char iv[CRYPTO_AES128_GCM_IV_SIZE],
	const unsigned char *aad, size_t aad_len,
	const unsigned char *plaintext, size_t pt_len,
	unsigned char *ciphertext,
	unsigned char tag[CRYPTO_AES128_GCM_TAG_SIZE]
) {
	CryptoAes128Context aes;
	crypto_aes128_init(&aes, key);

	// H = AES_K(0^128)
	unsigned char h[16];
	unsigned char zero[16];
	memset(zero, 0, sizeof(zero));
	crypto_aes128_encrypt_block(&aes, zero, h);

	// J0 = IV || 0x00000001 (96-bit IV case, as used by TLS 1.3)
	unsigned char j0[16];
	memcpy(j0, iv, CRYPTO_AES128_GCM_IV_SIZE);
	j0[12] = 0;
	j0[13] = 0;
	j0[14] = 0;
	j0[15] = 1;

	// Encrypt: C = P XOR AES_K(inc32(J0)) stream
	unsigned char ctr[16];
	memcpy(ctr, j0, sizeof(ctr));
	inc32(ctr);
	if (pt_len) aes128_ctr_xor(&aes, ctr, plaintext, ciphertext, pt_len);

	// GHASH over AAD and ciphertext, then lengths.
	unsigned char y[16];
	memset(y, 0, sizeof(y));
	if (aad_len) ghash_update(y, h, aad, aad_len);
	if (pt_len) ghash_update(y, h, ciphertext, pt_len);
	ghash_final_lengths(y, h, aad_len, pt_len);

	// Tag = AES_K(J0) XOR GHASH
	unsigned char s[16];
	crypto_aes128_encrypt_block(&aes, j0, s);
	xor16(tag, s, y);
	return 0;
}

int crypto_aes128_gcm_decrypt(
	const unsigned char key[16],
	const unsigned char iv[CRYPTO_AES128_GCM_IV_SIZE],
	const unsigned char *aad, size_t aad_len,
	const unsigned char *ciphertext, size_t ct_len,
	const unsigned char tag[CRYPTO_AES128_GCM_TAG_SIZE],
	unsigned char *plaintext
) {
	CryptoAes128Context aes;
	crypto_aes128_init(&aes, key);

	unsigned char h[16];
	unsigned char zero[16];
	memset(zero, 0, sizeof(zero));
	crypto_aes128_encrypt_block(&aes, zero, h);

	unsigned char j0[16];
	memcpy(j0, iv, CRYPTO_AES128_GCM_IV_SIZE);
	j0[12] = 0;
	j0[13] = 0;
	j0[14] = 0;
	j0[15] = 1;

	unsigned char y[16];
	memset(y, 0, sizeof(y));
	if (aad_len) ghash_update(y, h, aad, aad_len);
	if (ct_len) ghash_update(y, h, ciphertext, ct_len);
	ghash_final_lengths(y, h, aad_len, ct_len);

	unsigned char s[16];
	crypto_aes128_encrypt_block(&aes, j0, s);
	unsigned char exp_tag[16];
	xor16(exp_tag, s, y);

	if (!ct_memeq(exp_tag, tag, 16)) {
		return -1;
	}

	unsigned char ctr[16];
	memcpy(ctr, j0, sizeof(ctr));
	inc32(ctr);
	if (ct_len) aes128_ctr_xor(&aes, ctr, ciphertext, plaintext, ct_len);
	return 0;
}

int crypto_aes256_gcm_encrypt(
	const unsigned char key[32],
	const unsigned char iv[CRYPTO_AES256_GCM_IV_SIZE],
	const unsigned char *aad, size_t aad_len,
	const unsigned char *plaintext, size_t pt_len,
	unsigned char *ciphertext,
	unsigned char tag[CRYPTO_AES256_GCM_TAG_SIZE]
) {
	CryptoAes256GcmContext ctx;
	int result;

	crypto_aes256_gcm_context_init(&ctx, key);
	result = crypto_aes256_gcm_context_encrypt(&ctx, iv, aad, aad_len, plaintext, pt_len, ciphertext, tag);
	crypto_aes256_gcm_context_clear(&ctx);
	return result;
}

int crypto_aes256_gcm_decrypt(
	const unsigned char key[32],
	const unsigned char iv[CRYPTO_AES256_GCM_IV_SIZE],
	const unsigned char *aad, size_t aad_len,
	const unsigned char *ciphertext, size_t ct_len,
	const unsigned char tag[CRYPTO_AES256_GCM_TAG_SIZE],
	unsigned char *plaintext
) {
	CryptoAes256GcmContext ctx;
	int result;

	crypto_aes256_gcm_context_init(&ctx, key);
	result = crypto_aes256_gcm_context_decrypt(&ctx, iv, aad, aad_len, ciphertext, ct_len, tag, plaintext);
	crypto_aes256_gcm_context_clear(&ctx);
	return result;
}

void crypto_aes256_gcm_context_init(CryptoAes256GcmContext *ctx, const unsigned char key[32]) {
	unsigned char zero[16];

	crypto_aes256_init(&ctx->aes, key);
	memset(zero, 0, sizeof(zero));
	crypto_aes256_encrypt_block(&ctx->aes, zero, ctx->h);
	ghash_build_table(ctx->ghash_table, ctx->h);
	memset(zero, 0, sizeof(zero));
}

void crypto_aes256_gcm_context_clear(CryptoAes256GcmContext *ctx) {
	if (ctx != 0) crypto_secure_bzero(ctx, sizeof(*ctx));
}

int crypto_aes256_gcm_context_encrypt(
	const CryptoAes256GcmContext *ctx,
	const unsigned char iv[CRYPTO_AES256_GCM_IV_SIZE],
	const unsigned char *aad, size_t aad_len,
	const unsigned char *plaintext, size_t pt_len,
	unsigned char *ciphertext,
	unsigned char tag[CRYPTO_AES256_GCM_TAG_SIZE]
) {
	if (ctx == 0) return -1;

	unsigned char j0[16];
	memcpy(j0, iv, CRYPTO_AES256_GCM_IV_SIZE);
	j0[12] = 0;
	j0[13] = 0;
	j0[14] = 0;
	j0[15] = 1;

	unsigned char ctr[16];
	memcpy(ctr, j0, sizeof(ctr));
	inc32(ctr);
	if (pt_len) aes256_ctr_xor(&ctx->aes, ctr, plaintext, ciphertext, pt_len);

	unsigned char y[16];
	memset(y, 0, sizeof(y));
	if (aad_len) ghash_update_table(y, ctx->ghash_table, aad, aad_len);
	if (pt_len) ghash_update_table(y, ctx->ghash_table, ciphertext, pt_len);
	ghash_final_lengths_table(y, ctx->ghash_table, aad_len, pt_len);

	unsigned char s[16];
	crypto_aes256_encrypt_block(&ctx->aes, j0, s);
	xor16(tag, s, y);
	return 0;
}

int crypto_aes256_gcm_context_decrypt(
	const CryptoAes256GcmContext *ctx,
	const unsigned char iv[CRYPTO_AES256_GCM_IV_SIZE],
	const unsigned char *aad, size_t aad_len,
	const unsigned char *ciphertext, size_t ct_len,
	const unsigned char tag[CRYPTO_AES256_GCM_TAG_SIZE],
	unsigned char *plaintext
) {
	if (ctx == 0) return -1;

	unsigned char j0[16];
	memcpy(j0, iv, CRYPTO_AES256_GCM_IV_SIZE);
	j0[12] = 0;
	j0[13] = 0;
	j0[14] = 0;
	j0[15] = 1;

	unsigned char y[16];
	memset(y, 0, sizeof(y));
	if (aad_len) ghash_update_table(y, ctx->ghash_table, aad, aad_len);
	if (ct_len) ghash_update_table(y, ctx->ghash_table, ciphertext, ct_len);
	ghash_final_lengths_table(y, ctx->ghash_table, aad_len, ct_len);

	unsigned char s[16];
	crypto_aes256_encrypt_block(&ctx->aes, j0, s);
	unsigned char exp_tag[16];
	xor16(exp_tag, s, y);
	if (!ct_memeq(exp_tag, tag, 16)) return -1;

	unsigned char ctr[16];
	memcpy(ctr, j0, sizeof(ctr));
	inc32(ctr);
	if (ct_len) aes256_ctr_xor(&ctx->aes, ctr, ciphertext, plaintext, ct_len);
	return 0;
}
