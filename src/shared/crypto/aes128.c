#include "crypto/aes128.h"
#include "runtime.h"

#ifdef CRYPTO_AES_TRACE
void crypto_aes_trace(unsigned int tag, const unsigned char st[16]);
#endif

static const unsigned char aes_sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const unsigned char *aes_inv_sbox_ptr(void) {
	return (const unsigned char *)
		"\x52\x09\x6a\xd5\x30\x36\xa5\x38\xbf\x40\xa3\x9e\x81\xf3\xd7\xfb"
		"\x7c\xe3\x39\x82\x9b\x2f\xff\x87\x34\x8e\x43\x44\xc4\xde\xe9\xcb"
		"\x54\x7b\x94\x32\xa6\xc2\x23\x3d\xee\x4c\x95\x0b\x42\xfa\xc3\x4e"
		"\x08\x2e\xa1\x66\x28\xd9\x24\xb2\x76\x5b\xa2\x49\x6d\x8b\xd1\x25"
		"\x72\xf8\xf6\x64\x86\x68\x98\x16\xd4\xa4\x5c\xcc\x5d\x65\xb6\x92"
		"\x6c\x70\x48\x50\xfd\xed\xb9\xda\x5e\x15\x46\x57\xa7\x8d\x9d\x84"
		"\x90\xd8\xab\x00\x8c\xbc\xd3\x0a\xf7\xe4\x58\x05\xb8\xb3\x45\x06"
		"\xd0\x2c\x1e\x8f\xca\x3f\x0f\x02\xc1\xaf\xbd\x03\x01\x13\x8a\x6b"
		"\x3a\x91\x11\x41\x4f\x67\xdc\xea\x97\xf2\xcf\xce\xf0\xb4\xe6\x73"
		"\x96\xac\x74\x22\xe7\xad\x35\x85\xe2\xf9\x37\xe8\x1c\x75\xdf\x6e"
		"\x47\xf1\x1a\x71\x1d\x29\xc5\x89\x6f\xb7\x62\x0e\xaa\x18\xbe\x1b"
		"\xfc\x56\x3e\x4b\xc6\xd2\x79\x20\x9a\xdb\xc0\xfe\x78\xcd\x5a\xf4"
		"\x1f\xdd\xa8\x33\x88\x07\xc7\x31\xb1\x12\x10\x59\x27\x80\xec\x5f"
		"\x60\x51\x7f\xa9\x19\xb5\x4a\x0d\x2d\xe5\x7a\x9f\x93\xc9\x9c\xef"
		"\xa0\xe0\x3b\x4d\xae\x2a\xf5\xb0\xc8\xeb\xbb\x3c\x83\x53\x99\x61"
		"\x17\x2b\x04\x7e\xba\x77\xd6\x26\xe1\x69\x14\x63\x55\x21\x0c\x7d";
}

static unsigned int load_be32(const unsigned char *p) {
	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | ((unsigned int)p[3]);
}

static unsigned int rot_word(unsigned int w) {
	return (w << 8) | (w >> 24);
}

static unsigned char aes_sbox_byte(unsigned char value) {
	return aes_sbox[value];
}

static unsigned int sub_word(unsigned int w) {
	unsigned int out = 0;
	out |= (unsigned int)aes_sbox_byte((unsigned char)(w >> 24)) << 24;
	out |= (unsigned int)aes_sbox_byte((unsigned char)(w >> 16)) << 16;
	out |= (unsigned int)aes_sbox_byte((unsigned char)(w >> 8)) << 8;
	out |= (unsigned int)aes_sbox_byte((unsigned char)(w >> 0)) << 0;
	return out;
}

static unsigned char xtime(unsigned char x) {
	return (unsigned char)((x << 1) ^ ((x & 0x80u) ? 0x1bu : 0x00u));
}

void crypto_aes128_init(CryptoAes128Context *ctx, const unsigned char key[CRYPTO_AES128_KEY_SIZE]) {
	// Key expansion, AES-128: 4 words key, 44 words total.
	for (unsigned int i = 0; i < 4; i++) {
		ctx->rk[i] = load_be32(key + (size_t)(i * 4u));
	}
	unsigned char rc = 0x01u;
	for (unsigned int i = 4; i < 44; i++) {
		unsigned int temp = ctx->rk[i - 1];
		if ((i & 3u) == 0u) {
			temp = sub_word(rot_word(temp)) ^ ((unsigned int)rc << 24);
			rc = xtime(rc);
		}
		ctx->rk[i] = ctx->rk[i - 4] ^ temp;
	}
}

void crypto_aes256_init(CryptoAes256Context *ctx, const unsigned char key[CRYPTO_AES256_KEY_SIZE]) {
	for (unsigned int i = 0; i < 8; i++) {
		ctx->rk[i] = load_be32(key + (size_t)(i * 4u));
	}
	unsigned char rc = 0x01u;
	for (unsigned int i = 8; i < 60; i++) {
		unsigned int temp = ctx->rk[i - 1];
		if ((i & 7u) == 0u) {
			temp = sub_word(rot_word(temp)) ^ ((unsigned int)rc << 24);
			rc = xtime(rc);
		} else if ((i & 7u) == 4u) {
			temp = sub_word(temp);
		}
		ctx->rk[i] = ctx->rk[i - 8] ^ temp;
	}
}

static unsigned char gf_mul(unsigned char a, unsigned char b) {
	unsigned char r = 0;
	for (unsigned int i = 0; i < 8; i++) {
		if (b & 1u) r ^= a;
		unsigned char hi = (unsigned char)(a & 0x80u);
		a <<= 1;
		if (hi) a ^= 0x1bu;
		b >>= 1;
	}
	return r;
}

static void add_round_key(unsigned char st[16], const unsigned int *rk) {
	for (unsigned int c = 0; c < 4; c++) {
		unsigned int w = rk[c];
		st[c*4u + 0u] ^= (unsigned char)(w >> 24);
		st[c*4u + 1u] ^= (unsigned char)(w >> 16);
		st[c*4u + 2u] ^= (unsigned char)(w >> 8);
		st[c*4u + 3u] ^= (unsigned char)(w);
	}
}

static void sub_bytes(unsigned char st[16]) {
	for (unsigned int i = 0; i < 16; i++) st[i] = aes_sbox_byte(st[i]);
}

static void inv_sub_bytes(unsigned char st[16]) {
	const unsigned char *inv_sbox = aes_inv_sbox_ptr();
	for (unsigned int i = 0; i < 16; i++) st[i] = inv_sbox[st[i]];
}

static void shift_rows(unsigned char st[16]) {
	unsigned char t;
	// Row 1 shift left 1
	t = st[1]; st[1]=st[5]; st[5]=st[9]; st[9]=st[13]; st[13]=t;
	// Row 2 shift left 2
	t = st[2]; st[2]=st[10]; st[10]=t; t = st[6]; st[6]=st[14]; st[14]=t;
	// Row 3 shift left 3
	t = st[3]; st[3]=st[15]; st[15]=st[11]; st[11]=st[7]; st[7]=t;
}

static void inv_shift_rows(unsigned char st[16]) {
	unsigned char t;
	// Row 1 shift right 1
	t = st[13]; st[13]=st[9]; st[9]=st[5]; st[5]=st[1]; st[1]=t;
	// Row 2 shift right 2
	t = st[2]; st[2]=st[10]; st[10]=t; t = st[6]; st[6]=st[14]; st[14]=t;
	// Row 3 shift right 3 (left 1)
	t = st[3]; st[3]=st[7]; st[7]=st[11]; st[11]=st[15]; st[15]=t;
}

static void mix_columns(unsigned char st[16]) {
	for (unsigned int c = 0; c < 4; c++) {
		unsigned char *col = &st[c*4u];
		unsigned char a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
		unsigned char r0 = (unsigned char)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
		unsigned char r1 = (unsigned char)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
		unsigned char r2 = (unsigned char)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
		unsigned char r3 = (unsigned char)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
		col[0] = r0; col[1] = r1; col[2] = r2; col[3] = r3;
	}
}

static void inv_mix_columns(unsigned char st[16]) {
	for (unsigned int c = 0; c < 4; c++) {
		unsigned char *col = &st[c*4u];
		unsigned char a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
		col[0] = (unsigned char)(gf_mul(a0,0x0e) ^ gf_mul(a1,0x0b) ^ gf_mul(a2,0x0d) ^ gf_mul(a3,0x09));
		col[1] = (unsigned char)(gf_mul(a0,0x09) ^ gf_mul(a1,0x0e) ^ gf_mul(a2,0x0b) ^ gf_mul(a3,0x0d));
		col[2] = (unsigned char)(gf_mul(a0,0x0d) ^ gf_mul(a1,0x09) ^ gf_mul(a2,0x0e) ^ gf_mul(a3,0x0b));
		col[3] = (unsigned char)(gf_mul(a0,0x0b) ^ gf_mul(a1,0x0d) ^ gf_mul(a2,0x09) ^ gf_mul(a3,0x0e));
	}
}

void crypto_aes128_encrypt_block(const CryptoAes128Context *ctx, const unsigned char in[CRYPTO_AES128_BLOCK_SIZE], unsigned char out[CRYPTO_AES128_BLOCK_SIZE]) {
	unsigned char st[16];
	memcpy(st, in, 16);
	#ifdef CRYPTO_AES_TRACE
	crypto_aes_trace(0x0000u, st);
	#endif

	add_round_key(st, &ctx->rk[0]);
	#ifdef CRYPTO_AES_TRACE
	crypto_aes_trace(0x0100u, st);
	#endif

	for (unsigned int round = 1; round < CRYPTO_AES128_ROUNDS; round++) {
		sub_bytes(st);
		#ifdef CRYPTO_AES_TRACE
		crypto_aes_trace(0x0200u | round, st);
		#endif
		shift_rows(st);
		#ifdef CRYPTO_AES_TRACE
		crypto_aes_trace(0x0300u | round, st);
		#endif
		mix_columns(st);
		#ifdef CRYPTO_AES_TRACE
		crypto_aes_trace(0x0400u | round, st);
		#endif
		add_round_key(st, &ctx->rk[round * 4u]);
		#ifdef CRYPTO_AES_TRACE
		crypto_aes_trace(0x0500u | round, st);
		#endif
	}

	sub_bytes(st);
	#ifdef CRYPTO_AES_TRACE
	crypto_aes_trace(0x0200u | CRYPTO_AES128_ROUNDS, st);
	#endif
	shift_rows(st);
	#ifdef CRYPTO_AES_TRACE
	crypto_aes_trace(0x0300u | CRYPTO_AES128_ROUNDS, st);
	#endif
	add_round_key(st, &ctx->rk[CRYPTO_AES128_ROUNDS * 4u]);
	#ifdef CRYPTO_AES_TRACE
	crypto_aes_trace(0x0500u | CRYPTO_AES128_ROUNDS, st);
	#endif

	memcpy(out, st, 16);
	memset(st, 0, sizeof(st));
}

void crypto_aes256_encrypt_block(const CryptoAes256Context *ctx, const unsigned char in[CRYPTO_AES256_BLOCK_SIZE], unsigned char out[CRYPTO_AES256_BLOCK_SIZE]) {
	unsigned char st[16];
	memcpy(st, in, 16);

	add_round_key(st, &ctx->rk[0]);
	for (unsigned int round = 1; round < CRYPTO_AES256_ROUNDS; round++) {
		sub_bytes(st);
		shift_rows(st);
		mix_columns(st);
		add_round_key(st, &ctx->rk[round * 4u]);
	}
	sub_bytes(st);
	shift_rows(st);
	add_round_key(st, &ctx->rk[CRYPTO_AES256_ROUNDS * 4u]);

	memcpy(out, st, 16);
	memset(st, 0, sizeof(st));
}

void crypto_aes128_decrypt_block(const CryptoAes128Context *ctx, const unsigned char in[CRYPTO_AES128_BLOCK_SIZE], unsigned char out[CRYPTO_AES128_BLOCK_SIZE]) {
	unsigned char st[16];
	memcpy(st, in, 16);

	add_round_key(st, &ctx->rk[CRYPTO_AES128_ROUNDS * 4u]);

	for (unsigned int round = CRYPTO_AES128_ROUNDS - 1; round > 0; round--) {
		inv_shift_rows(st);
		inv_sub_bytes(st);
		add_round_key(st, &ctx->rk[round * 4u]);
		inv_mix_columns(st);
	}

	inv_shift_rows(st);
	inv_sub_bytes(st);
	add_round_key(st, &ctx->rk[0]);

	memcpy(out, st, 16);
	memset(st, 0, sizeof(st));
}

void crypto_aes256_decrypt_block(const CryptoAes256Context *ctx, const unsigned char in[CRYPTO_AES256_BLOCK_SIZE], unsigned char out[CRYPTO_AES256_BLOCK_SIZE]) {
	unsigned char st[16];
	memcpy(st, in, 16);

	add_round_key(st, &ctx->rk[CRYPTO_AES256_ROUNDS * 4u]);

	for (unsigned int round = CRYPTO_AES256_ROUNDS - 1; round > 0; round--) {
		inv_shift_rows(st);
		inv_sub_bytes(st);
		add_round_key(st, &ctx->rk[round * 4u]);
		inv_mix_columns(st);
	}

	inv_shift_rows(st);
	inv_sub_bytes(st);
	add_round_key(st, &ctx->rk[0]);

	memcpy(out, st, 16);
	memset(st, 0, sizeof(st));
}
