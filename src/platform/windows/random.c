#include "platform.h"

#define WIN_BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002UL

__declspec(dllimport) long __stdcall BCryptGenRandom(void *algorithm, unsigned char *buffer, unsigned long count, unsigned long flags);

int platform_random_bytes(unsigned char *buffer, size_t count) {
    while (count > 0U) {
        unsigned long chunk = count > 0xffffffffUL ? 0xffffffffUL : (unsigned long)count;
        if (BCryptGenRandom(0, buffer, chunk, WIN_BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return -1;
        buffer += chunk;
        count -= chunk;
    }
    return 0;
}
