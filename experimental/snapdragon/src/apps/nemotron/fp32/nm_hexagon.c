#include <stdint.h>

/* Compiler-emitted arithmetic helpers; no firmware/libc arithmetic dependency. */
uint32_t __hexagon_udivsi3(uint32_t numerator, uint32_t denominator) {
    uint32_t quotient = 0;
    unsigned int shift;
    if (!denominator) return UINT32_MAX;
    if (numerator < denominator) return 0;
    if (!(denominator & (denominator - 1))) return numerator >> __builtin_ctz(denominator);
    shift = (unsigned int)(__builtin_clz(denominator) - __builtin_clz(numerator));
    denominator <<= shift;
    do {
        quotient <<= 1;
        if (numerator >= denominator) { numerator -= denominator; quotient |= 1; }
        denominator >>= 1;
    } while (shift--);
    return quotient;
}
uint32_t __hexagon_umodsi3(uint32_t numerator, uint32_t denominator) {
    if (denominator && !(denominator & (denominator - 1))) return numerator & (denominator - 1);
    return numerator - __hexagon_udivsi3(numerator, denominator) * denominator;
}
uint64_t __hexagon_udivdi3(uint64_t numerator, uint64_t denominator) {
    uint64_t quotient = 0;
    unsigned int shift;
    if (!denominator) return UINT64_MAX;
    if (numerator < denominator) return 0;
    if (!(denominator & (denominator - 1))) return numerator >> __builtin_ctzll(denominator);
    shift = (unsigned int)(__builtin_clzll(denominator) - __builtin_clzll(numerator));
    denominator <<= shift;
    do {
        quotient <<= 1;
        if (numerator >= denominator) { numerator -= denominator; quotient |= 1; }
        denominator >>= 1;
    } while (shift--);
    return quotient;
}
