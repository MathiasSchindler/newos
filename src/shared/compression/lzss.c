#include "compression/lzss.h"

static size_t compression_lzss_find_match(const unsigned char *input, size_t input_size, size_t position, unsigned int *distance_out) {
    size_t window_start = position > COMPRESSION_LZSS_WINDOW_SIZE ? position - COMPRESSION_LZSS_WINDOW_SIZE : 0U;
    size_t best_length = 0U;
    unsigned int best_distance = 0U;
    size_t candidate;

    if (position >= input_size) {
        *distance_out = 0U;
        return 0U;
    }

    for (candidate = window_start; candidate < position; ++candidate) {
        size_t length = 0U;
        size_t max_length = input_size - position;
        if (max_length > COMPRESSION_LZSS_MAX_MATCH) {
            max_length = COMPRESSION_LZSS_MAX_MATCH;
        }
        while (length < max_length && input[candidate + length] == input[position + length]) {
            length += 1U;
        }
        if (length > best_length && length >= COMPRESSION_LZSS_MIN_MATCH) {
            best_length = length;
            best_distance = (unsigned int)(position - candidate);
            if (best_length == COMPRESSION_LZSS_MAX_MATCH) {
                break;
            }
        }
    }

    *distance_out = best_distance;
    return best_length;
}

size_t compression_lzss_bound(size_t input_size) {
    size_t flag_count = (input_size + 7U) / 8U;

    if (input_size > ((size_t)-1) - flag_count) {
        return 0U;
    }
    return input_size + flag_count;
}

int compression_lzss_compress(const unsigned char *input, size_t input_size, unsigned char *output, size_t output_capacity, size_t *output_size_out) {
    size_t input_offset = 0U;
    size_t output_offset = 0U;

    if ((input == 0 && input_size != 0U) || output == 0 || output_size_out == 0) {
        return -1;
    }

    while (input_offset < input_size) {
        size_t flag_offset;
        unsigned char flags = 0U;
        unsigned int bit;

        if (output_offset >= output_capacity) {
            return -1;
        }
        flag_offset = output_offset++;

        for (bit = 0U; bit < 8U && input_offset < input_size; ++bit) {
            unsigned int distance = 0U;
            size_t match_length = compression_lzss_find_match(input, input_size, input_offset, &distance);

            if (match_length >= COMPRESSION_LZSS_MIN_MATCH) {
                unsigned int token_distance = distance - 1U;
                unsigned int token_length = (unsigned int)(match_length - COMPRESSION_LZSS_MIN_MATCH);

                if (output_offset + 2U > output_capacity) {
                    return -1;
                }
                flags |= (unsigned char)(1U << bit);
                output[output_offset++] = (unsigned char)(token_distance & 0xffU);
                output[output_offset++] = (unsigned char)(((token_distance >> 8U) << 4U) | token_length);
                input_offset += match_length;
            } else {
                if (output_offset >= output_capacity) {
                    return -1;
                }
                output[output_offset++] = input[input_offset++];
            }
        }
        output[flag_offset] = flags;
    }

    *output_size_out = output_offset;
    return 0;
}

int compression_lzss_decompress(const unsigned char *input, size_t input_size, unsigned char *output, size_t output_capacity, size_t *output_size_out) {
    size_t input_offset = 0U;
    size_t output_offset = 0U;

    if ((input == 0 && input_size != 0U) || output == 0 || output_size_out == 0) {
        return -1;
    }

    while (input_offset < input_size) {
        unsigned char flags = input[input_offset++];
        unsigned int bit;

        for (bit = 0U; bit < 8U && input_offset < input_size; ++bit) {
            if ((flags & (unsigned char)(1U << bit)) == 0U) {
                if (output_offset >= output_capacity) {
                    return -1;
                }
                output[output_offset++] = input[input_offset++];
            } else {
                unsigned int token;
                unsigned int distance;
                unsigned int length;
                unsigned int index;

                if (input_offset + 2U > input_size) {
                    return -1;
                }
                token = (unsigned int)input[input_offset] | ((unsigned int)input[input_offset + 1U] << 8U);
                input_offset += 2U;
                distance = ((token & 0x00ffU) | ((token >> 4U) & 0x0f00U)) + 1U;
                length = ((token >> 8U) & 0x0fU) + COMPRESSION_LZSS_MIN_MATCH;
                if (distance > output_offset || output_offset + (size_t)length > output_capacity) {
                    return -1;
                }
                for (index = 0U; index < length; ++index) {
                    output[output_offset] = output[output_offset - distance];
                    output_offset += 1U;
                }
            }
        }
    }

    *output_size_out = output_offset;
    return 0;
}
