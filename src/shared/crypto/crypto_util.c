#include "crypto/crypto_util.h"
#include "platform.h"

void crypto_secure_bzero(void *buffer, size_t count) {
    volatile unsigned char *bytes = (volatile unsigned char *)buffer;
    size_t i;

    if (buffer == 0) {
        return;
    }

    for (i = 0; i < count; ++i) {
        bytes[i] = 0U;
    }
}

int crypto_constant_time_equal(const unsigned char *left, const unsigned char *right, size_t count) {
    unsigned char diff = 0U;
    size_t i;

    if (left == 0 || right == 0) {
        return 0;
    }

    for (i = 0; i < count; ++i) {
        diff |= (unsigned char)(left[i] ^ right[i]);
    }

    return diff == 0U;
}

int crypto_random_bytes(unsigned char *buffer, size_t count) {
    return platform_random_bytes(buffer, count);
}
