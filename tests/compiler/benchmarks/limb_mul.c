static unsigned int left[8] = {
    0x243f6a88U, 0x85a308d3U, 0x13198a2eU, 0x03707344U,
    0xa4093822U, 0x299f31d0U, 0x082efa98U, 0xec4e6c89U
};
static unsigned int right[8] = {
    0x452821e6U, 0x38d01377U, 0xbe5466cfU, 0x34e90c6cU,
    0xc0ac29b7U, 0xc97c50ddU, 0x3f84d5b5U, 0xb5470917U
};
static volatile unsigned int sink;

int main(void) {
    unsigned int out[8];

    for (unsigned int round = 0; round < 100000U; ++round) {
        unsigned long long carry = 0ULL;
        for (unsigned int i = 0; i < 8U; ++i) {
            unsigned long long acc = carry;
            for (unsigned int j = 0; j <= i; ++j) {
                acc += (unsigned long long)left[j] * (unsigned long long)right[i - j];
            }
            out[i] = (unsigned int)acc;
            carry = acc >> 32;
        }
        left[round & 7U] ^= out[(round + 3U) & 7U] + (unsigned int)carry;
    }
    sink = left[0];
    return 0;
}
