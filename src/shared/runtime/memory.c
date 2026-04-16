#include "runtime.h"

void *memcpy(void *dst, const void *src, size_t count) {
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    size_t i;

    for (i = 0; i < count; ++i) {
        out[i] = in[i];
    }

    return dst;
}

void *memset(void *buffer, int byte_value, size_t count) {
    unsigned char *out = (unsigned char *)buffer;
    size_t i;

    for (i = 0; i < count; ++i) {
        out[i] = (unsigned char)byte_value;
    }

    return buffer;
}

void *memmove(void *dst, const void *src, size_t count) {
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    size_t i;

    if (out == in) {
        return dst;
    }

    if (out < in) {
        for (i = 0; i < count; ++i) {
            out[i] = in[i];
        }
    } else {
        for (i = count; i > 0; --i) {
            out[i - 1] = in[i - 1];
        }
    }

    return dst;
}

void rt_memset(void *buffer, int byte_value, size_t count) {
    (void)memset(buffer, byte_value, count);
}
