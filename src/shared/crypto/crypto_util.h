#ifndef NEWOS_CRYPTO_UTIL_H
#define NEWOS_CRYPTO_UTIL_H

#include <stddef.h>

void crypto_secure_bzero(void *buffer, size_t count);
int crypto_constant_time_equal(const unsigned char *left, const unsigned char *right, size_t count);
int crypto_random_bytes(unsigned char *buffer, size_t count);

#endif
