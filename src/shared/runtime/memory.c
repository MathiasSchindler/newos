#include "runtime.h"

/*
 * Keep these implementations strictly byte-wise and volatile-backed so the
 * compiler does not fold them back into builtin memcpy or memset calls.
 * That would recurse here in hosted optimized builds.
 */
void *memcpy(void *dst, const void *src, size_t count) {
    volatile unsigned char *out = (volatile unsigned char *)dst;
    const volatile unsigned char *in = (const volatile unsigned char *)src;
    size_t i;

    for (i = 0; i < count; ++i) {
        out[i] = in[i];
    }

    return dst;
}

void *memset(void *buffer, int byte_value, size_t count) {
    volatile unsigned char *out = (volatile unsigned char *)buffer;
    unsigned char value = (unsigned char)byte_value;
    size_t i;

    for (i = 0; i < count; ++i) {
        out[i] = value;
    }

    return buffer;
}

void *memmove(void *dst, const void *src, size_t count) {
    volatile unsigned char *out = (volatile unsigned char *)dst;
    const volatile unsigned char *in = (const volatile unsigned char *)src;
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
