#ifndef NEWOS_CRYPTO_AES_CMAC_H
#define NEWOS_CRYPTO_AES_CMAC_H

#include <stddef.h>

#define CRYPTO_AES_CMAC_SIZE 16U

int crypto_aes128_cmac(const unsigned char key[16], const unsigned char *data, size_t data_length, unsigned char out[CRYPTO_AES_CMAC_SIZE]);

#endif