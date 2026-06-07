#include "crypto/aes_cmac.h"

#include "crypto/aes128.h"
#include "runtime.h"

static void cmac_left_shift_one(const unsigned char in[16], unsigned char out[16]) {
    unsigned int carry = 0U;
    size_t index;

    for (index = 16U; index > 0U; --index) {
        unsigned int value = in[index - 1U];
        out[index - 1U] = (unsigned char)((value << 1U) | carry);
        carry = (value >> 7U) & 1U;
    }
}

static void cmac_make_subkeys(const CryptoAes128Context *aes, unsigned char k1[16], unsigned char k2[16]) {
    unsigned char zero[16];
    unsigned char l[16];

    rt_memset(zero, 0, sizeof(zero));
    crypto_aes128_encrypt_block(aes, zero, l);
    cmac_left_shift_one(l, k1);
    if ((l[0] & 0x80U) != 0U) k1[15] ^= 0x87U;
    cmac_left_shift_one(k1, k2);
    if ((k1[0] & 0x80U) != 0U) k2[15] ^= 0x87U;
    rt_memset(l, 0, sizeof(l));
}

static void cmac_xor_block(unsigned char block[16], const unsigned char *right) {
    size_t index;

    for (index = 0U; index < 16U; ++index) block[index] ^= right[index];
}

int crypto_aes128_cmac(const unsigned char key[16], const unsigned char *data, size_t data_length, unsigned char out[CRYPTO_AES_CMAC_SIZE]) {
    CryptoAes128Context aes;
    unsigned char k1[16];
    unsigned char k2[16];
    unsigned char state[16];
    unsigned char last[16];
    size_t block_count;
    size_t complete_last;
    size_t index;

    if (key == 0 || out == 0 || (data == 0 && data_length != 0U)) return -1;
    crypto_aes128_init(&aes, key);
    cmac_make_subkeys(&aes, k1, k2);
    rt_memset(state, 0, sizeof(state));
    rt_memset(last, 0, sizeof(last));

    block_count = (data_length + 15U) / 16U;
    complete_last = data_length != 0U && (data_length % 16U) == 0U;
    if (block_count == 0U) block_count = 1U;

    for (index = 0U; index + 1U < block_count; ++index) {
        cmac_xor_block(state, data + index * 16U);
        crypto_aes128_encrypt_block(&aes, state, state);
    }

    if (complete_last) {
        memcpy(last, data + (block_count - 1U) * 16U, 16U);
        cmac_xor_block(last, k1);
    } else {
        size_t remaining = data_length % 16U;
        if (remaining != 0U) memcpy(last, data + (block_count - 1U) * 16U, remaining);
        last[remaining] = 0x80U;
        cmac_xor_block(last, k2);
    }

    cmac_xor_block(state, last);
    crypto_aes128_encrypt_block(&aes, state, out);
    rt_memset(&aes, 0, sizeof(aes));
    rt_memset(k1, 0, sizeof(k1));
    rt_memset(k2, 0, sizeof(k2));
    rt_memset(state, 0, sizeof(state));
    rt_memset(last, 0, sizeof(last));
    return 0;
}