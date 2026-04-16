#include "archive_util.h"

unsigned int archive_crc32_update(unsigned int crc, const unsigned char *data, size_t length) {
    size_t i;

    for (i = 0; i < length; ++i) {
        unsigned int value = (crc ^ data[i]) & 0xffU;
        int bit;

        for (bit = 0; bit < 8; ++bit) {
            if ((value & 1U) != 0U) {
                value = (value >> 1) ^ 0xedb88320U;
            } else {
                value >>= 1;
            }
        }

        crc = (crc >> 8) ^ value;
    }

    return crc;
}

void archive_write_octal(char *field, size_t field_size, unsigned long long value) {
    size_t i;

    for (i = 0; i < field_size; ++i) {
        field[i] = '0';
    }

    if (field_size > 0) {
        field[field_size - 1] = ' ';
    }

    if (field_size > 1) {
        size_t pos = field_size - 2;
        field[pos] = '\0';

        do {
            field[pos] = (char)('0' + (value & 7ULL));
            value >>= 3;
            if (pos == 0) {
                break;
            }
            pos -= 1;
        } while (value != 0ULL);
    }
}

unsigned long long archive_parse_octal(const char *field, size_t field_size) {
    unsigned long long value = 0;
    size_t i = 0;

    while (i < field_size && (field[i] == ' ' || field[i] == '\0')) {
        i += 1;
    }

    while (i < field_size && field[i] >= '0' && field[i] <= '7') {
        value = (value * 8ULL) + (unsigned long long)(field[i] - '0');
        i += 1;
    }

    return value;
}
