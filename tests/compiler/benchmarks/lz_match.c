static unsigned char data[8192];
static volatile unsigned int sink;

static unsigned int match_at(unsigned int position, unsigned int distance) {
    unsigned int length = 0;

    while (position + length < sizeof(data) &&
           length < 128U &&
           data[position + length] == data[position - distance + length]) {
        length += 1U;
    }
    return length;
}

int main(void) {
    unsigned int total = 0;

    for (unsigned int i = 0; i < sizeof(data); ++i) {
        data[i] = (unsigned char)((i * 13U) ^ (i >> 3U));
    }
    for (unsigned int round = 0; round < 200U; ++round) {
        for (unsigned int position = 64U; position + 128U < sizeof(data); position += 3U) {
            unsigned int best = 0;
            for (unsigned int distance = 1U; distance <= 64U; ++distance) {
                unsigned int length = match_at(position, distance);
                if (length > best) {
                    best = length;
                }
            }
            total += best;
        }
    }
    sink = total;
    return 0;
}
