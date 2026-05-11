#include "compression/zlib.h"

static unsigned int compression_adler32(const unsigned char *data, size_t length) {
    unsigned int s1 = 1U;
    unsigned int s2 = 0U;
    size_t index;

    for (index = 0U; index < length; ++index) {
        s1 += (unsigned int)data[index];
        if (s1 >= 65521U) {
            s1 %= 65521U;
        }
        s2 += s1;
        if (s2 >= 65521U) {
            s2 %= 65521U;
        }
    }
    return (s2 << 16U) | s1;
}

size_t compression_zlib_store_bound(size_t input_size) {
    size_t block_count = input_size / 65535U + 1U;

    if (input_size > ((size_t)-1) - 6U || block_count > (((size_t)-1) - input_size - 6U) / 5U) {
        return 0U;
    }
    return 2U + input_size + block_count * 5U + 4U;
}

int compression_zlib_store(const unsigned char *input, size_t input_size, unsigned char *output, size_t output_capacity, size_t *output_size_out) {
    size_t bound = compression_zlib_store_bound(input_size);
    size_t input_offset = 0U;
    size_t output_offset = 0U;
    unsigned int adler;

    if (input == 0 || output == 0 || output_size_out == 0 || bound == 0U || output_capacity < bound) {
        return -1;
    }
    output[output_offset++] = 0x78U;
    output[output_offset++] = 0x01U;
    while (input_offset < input_size || input_size == 0U) {
        size_t remaining = input_size - input_offset;
        unsigned int block_size = remaining > 65535U ? 65535U : (unsigned int)remaining;
        int final_block = input_offset + (size_t)block_size == input_size;
        unsigned int nlen = (~block_size) & 0xffffU;

        output[output_offset++] = final_block ? 0x01U : 0x00U;
        output[output_offset++] = (unsigned char)(block_size & 0xffU);
        output[output_offset++] = (unsigned char)((block_size >> 8U) & 0xffU);
        output[output_offset++] = (unsigned char)(nlen & 0xffU);
        output[output_offset++] = (unsigned char)((nlen >> 8U) & 0xffU);
        if (block_size != 0U) {
            size_t index;

            for (index = 0U; index < (size_t)block_size; ++index) {
                output[output_offset + index] = input[input_offset + index];
            }
            output_offset += (size_t)block_size;
        }
        input_offset += (size_t)block_size;
        if (final_block) {
            break;
        }
    }
    adler = compression_adler32(input, input_size);
    output[output_offset++] = (unsigned char)((adler >> 24U) & 0xffU);
    output[output_offset++] = (unsigned char)((adler >> 16U) & 0xffU);
    output[output_offset++] = (unsigned char)((adler >> 8U) & 0xffU);
    output[output_offset++] = (unsigned char)(adler & 0xffU);
    *output_size_out = output_offset;
    return 0;
}
