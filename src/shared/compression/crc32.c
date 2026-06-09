#include "compression/crc32.h"

unsigned int compression_crc32_update(unsigned int crc, const unsigned char *data, size_t length) {
    static const unsigned int table[16] = {
        0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU,
        0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
        0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
        0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU
    };
    size_t index;

    for (index = 0U; index < length; ++index) {
        crc ^= (unsigned int)data[index];
        crc = (crc >> 4U) ^ table[crc & 0x0fU];
        crc = (crc >> 4U) ^ table[crc & 0x0fU];
    }
    return crc;
}

unsigned int compression_crc32_finish(unsigned int crc) {
    return crc ^ 0xffffffffU;
}

unsigned int compression_crc32(const unsigned char *data, size_t length) {
    return compression_crc32_finish(compression_crc32_update(0xffffffffU, data, length));
}