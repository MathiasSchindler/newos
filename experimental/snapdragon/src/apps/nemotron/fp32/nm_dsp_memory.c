#include <stddef.h>

__attribute__((visibility("hidden")))
void *memset(void *destination, int value, size_t bytes) {
    volatile unsigned char *p = destination;
    size_t i;
    for (i = 0; i < bytes; ++i) p[i] = (unsigned char)value;
    return destination;
}
