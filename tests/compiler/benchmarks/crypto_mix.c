static volatile unsigned int sink;

static unsigned int rotr32(unsigned int value, unsigned int count) {
    return (value >> count) | (value << (32U - count));
}

int main(void) {
    unsigned int a = 0x6a09e667U;
    unsigned int b = 0xbb67ae85U;
    unsigned int c = 0x3c6ef372U;
    unsigned int d = 0xa54ff53aU;

    for (unsigned int i = 0; i < 400000U; ++i) {
        unsigned int s0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
        unsigned int s1 = rotr32(d, 6U) ^ rotr32(d, 11U) ^ rotr32(d, 25U);
        unsigned int ch = (d & b) ^ ((~d) & c);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t = s1 + ch + i;

        d = c + t;
        c = b;
        b = a;
        a = t + s0 + maj;
    }
    sink = a ^ b ^ c ^ d;
    return 0;
}
