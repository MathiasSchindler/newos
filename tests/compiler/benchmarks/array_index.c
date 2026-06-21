static unsigned char data[4096];
static volatile unsigned int sink;

int main(void) {
    unsigned int sum = 0;
    unsigned int i;

    for (i = 0; i < sizeof(data); ++i) {
        data[i] = (unsigned char)((i * 17U + 3U) & 255U);
    }
    for (unsigned int round = 0; round < 2000U; ++round) {
        for (i = 1; i + 1U < sizeof(data); ++i) {
            sum += (unsigned int)(data[i - 1U] ^ data[i] ^ data[i + 1U]);
        }
    }
    sink = sum;
    return 0;
}
