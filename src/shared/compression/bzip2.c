#include "compression/bzip2.h"

#include "runtime.h"

#define BZIP2_IO_BUFFER_SIZE 32768U
#define BZIP2_MAX_GROUPS 6U
#define BZIP2_MAX_ALPHA_SIZE 258U
#define BZIP2_MAX_CODE_BITS 23U
#define BZIP2_SELECTOR_GROUP_SIZE 50U
#define BZIP2_STREAM_MAGIC 0x314159265359ULL
#define BZIP2_END_MAGIC 0x177245385090ULL

typedef struct {
    CompressionBzip2ReadFn read_fn;
    void *read_context;
    unsigned char buffer[BZIP2_IO_BUFFER_SIZE];
    size_t offset;
    size_t available;
    unsigned long long bit_buffer;
    unsigned int bit_count;
} Bzip2BitReader;

typedef struct {
    unsigned int min_length;
    unsigned int max_length;
    unsigned int first_code[BZIP2_MAX_CODE_BITS + 2U];
    unsigned int symbol_count[BZIP2_MAX_CODE_BITS + 2U];
    unsigned int first_symbol[BZIP2_MAX_CODE_BITS + 2U];
    unsigned short symbols[BZIP2_MAX_ALPHA_SIZE];
} Bzip2HuffmanTable;

typedef struct {
    CompressionBzip2WriteFn write_fn;
    void *write_context;
    unsigned char buffer[BZIP2_IO_BUFFER_SIZE];
    size_t size;
    unsigned int crc;
    unsigned char last;
    unsigned int repeat_count;
    int have_last;
    int await_run_extra;
} Bzip2Output;

typedef struct {
    unsigned int block_size;
    unsigned char *ll8;
    unsigned int *tt;
} Bzip2Decoder;

static void bzip2_reader_init(Bzip2BitReader *reader, CompressionBzip2ReadFn read_fn, void *read_context) {
    rt_memset(reader, 0, sizeof(*reader));
    reader->read_fn = read_fn;
    reader->read_context = read_context;
}

static int bzip2_reader_fill(Bzip2BitReader *reader) {
    size_t amount = 0U;

    if (reader->read_fn(reader->read_context, reader->buffer, sizeof(reader->buffer), &amount) != 0) return -1;
    reader->offset = 0U;
    reader->available = amount;
    return amount == 0U ? -1 : 0;
}

static int bzip2_read_byte(Bzip2BitReader *reader, unsigned int *value_out) {
    if (reader->offset == reader->available && bzip2_reader_fill(reader) != 0) return -1;
    *value_out = reader->buffer[reader->offset++];
    return 0;
}

static int bzip2_read_bits(Bzip2BitReader *reader, unsigned int count, unsigned int *value_out) {
    unsigned int value;

    if (count == 0U || count > 32U) return -1;
    while (reader->bit_count < count) {
        unsigned int byte_value;
        if (bzip2_read_byte(reader, &byte_value) != 0) return -1;
        reader->bit_buffer = (reader->bit_buffer << 8U) | byte_value;
        reader->bit_count += 8U;
    }
    value = (unsigned int)((reader->bit_buffer >> (reader->bit_count - count)) & ((count == 32U) ? 0xffffffffULL : ((1ULL << count) - 1ULL)));
    reader->bit_count -= count;
    if (reader->bit_count == 0U) {
        reader->bit_buffer = 0U;
    } else {
        reader->bit_buffer &= (1ULL << reader->bit_count) - 1ULL;
    }
    *value_out = value;
    return 0;
}

static int bzip2_read_magic(Bzip2BitReader *reader, unsigned long long *magic_out) {
    unsigned int high;
    unsigned int low;

    if (bzip2_read_bits(reader, 24U, &high) != 0 || bzip2_read_bits(reader, 24U, &low) != 0) return -1;
    *magic_out = ((unsigned long long)high << 24U) | low;
    return 0;
}

static unsigned int bzip2_crc_update_byte(unsigned int crc, unsigned char value) {
    unsigned int index;

    crc ^= (unsigned int)value << 24U;
    for (index = 0U; index < 8U; ++index) {
        if ((crc & 0x80000000U) != 0U) {
            crc = (crc << 1U) ^ 0x04c11db7U;
        } else {
            crc <<= 1U;
        }
    }
    return crc;
}

static int bzip2_output_flush(Bzip2Output *output) {
    if (output->size == 0U) return 0;
    if (output->write_fn(output->write_context, output->buffer, output->size) != 0) return -1;
    output->size = 0U;
    return 0;
}

static int bzip2_output_byte(Bzip2Output *output, unsigned char value) {
    if (output->size == sizeof(output->buffer) && bzip2_output_flush(output) != 0) return -1;
    output->buffer[output->size++] = value;
    output->crc = bzip2_crc_update_byte(output->crc, value);
    return 0;
}

static int bzip2_output_rle_byte(Bzip2Output *output, unsigned char value) {
    unsigned int index;

    if (output->await_run_extra) {
        for (index = 0U; index < (unsigned int)value; ++index) {
            if (bzip2_output_byte(output, output->last) != 0) return -1;
        }
        output->await_run_extra = 0;
        output->have_last = 0;
        output->repeat_count = 0U;
        return 0;
    }
    if (bzip2_output_byte(output, value) != 0) return -1;
    if (output->have_last && output->last == value) {
        output->repeat_count += 1U;
    } else {
        output->last = value;
        output->repeat_count = 1U;
        output->have_last = 1;
    }
    if (output->repeat_count == 4U) output->await_run_extra = 1;
    return 0;
}

static void bzip2_output_init(Bzip2Output *output, CompressionBzip2WriteFn write_fn, void *write_context) {
    rt_memset(output, 0, sizeof(*output));
    output->write_fn = write_fn;
    output->write_context = write_context;
    output->crc = 0xffffffffU;
}

static int bzip2_huffman_build(Bzip2HuffmanTable *table, const unsigned char *lengths, unsigned int alpha_size) {
    unsigned int length;
    unsigned int symbol;
    unsigned int code = 0U;
    unsigned int next_symbol = 0U;

    rt_memset(table, 0, sizeof(*table));
    table->min_length = 0xffffffffU;
    for (symbol = 0U; symbol < alpha_size; ++symbol) {
        length = lengths[symbol];
        if (length == 0U || length > BZIP2_MAX_CODE_BITS) return -1;
        table->symbol_count[length] += 1U;
        if (length < table->min_length) table->min_length = length;
        if (length > table->max_length) table->max_length = length;
    }
    if (table->min_length == 0xffffffffU || table->max_length > BZIP2_MAX_CODE_BITS) return -1;
    for (length = 1U; length <= BZIP2_MAX_CODE_BITS; ++length) {
        code = (code + table->symbol_count[length - 1U]) << 1U;
        table->first_code[length] = code;
        table->first_symbol[length] = next_symbol;
        next_symbol += table->symbol_count[length];
    }
    rt_memset(table->symbol_count, 0, sizeof(table->symbol_count));
    for (symbol = 0U; symbol < alpha_size; ++symbol) {
        length = lengths[symbol];
        table->symbols[table->first_symbol[length] + table->symbol_count[length]] = (unsigned short)symbol;
        table->symbol_count[length] += 1U;
    }
    return 0;
}

static int bzip2_huffman_decode(Bzip2BitReader *reader, const Bzip2HuffmanTable *table, unsigned int *symbol_out) {
    unsigned int code = 0U;
    unsigned int length;

    for (length = 1U; length <= table->max_length; ++length) {
        unsigned int bit;
        if (bzip2_read_bits(reader, 1U, &bit) != 0) return -1;
        code = (code << 1U) | bit;
        if (length >= table->min_length && code >= table->first_code[length] && code < table->first_code[length] + table->symbol_count[length]) {
            *symbol_out = table->symbols[table->first_symbol[length] + code - table->first_code[length]];
            return 0;
        }
    }
    return -1;
}

static int bzip2_read_selectors(Bzip2BitReader *reader, unsigned char *selectors, unsigned int selector_count, unsigned int group_count) {
    unsigned char mtf[BZIP2_MAX_GROUPS];
    unsigned int index;

    for (index = 0U; index < group_count; ++index) mtf[index] = (unsigned char)index;
    for (index = 0U; index < selector_count; ++index) {
        unsigned int mtf_index = 0U;
        unsigned int bit;
        unsigned char value;
        unsigned int move;

        for (;;) {
            if (bzip2_read_bits(reader, 1U, &bit) != 0) return -1;
            if (bit == 0U) break;
            mtf_index += 1U;
            if (mtf_index >= group_count) return -1;
        }
        value = mtf[mtf_index];
        for (move = mtf_index; move > 0U; --move) mtf[move] = mtf[move - 1U];
        mtf[0] = value;
        selectors[index] = value;
    }
    return 0;
}

static int bzip2_read_tables(Bzip2BitReader *reader, Bzip2HuffmanTable *tables, unsigned char *selectors, unsigned int *selector_count_out, unsigned int alpha_size) {
    unsigned int group_count;
    unsigned int selector_count;
    unsigned int group;

    if (bzip2_read_bits(reader, 3U, &group_count) != 0 || bzip2_read_bits(reader, 15U, &selector_count) != 0) return -1;
    if (group_count < 2U || group_count > BZIP2_MAX_GROUPS || selector_count == 0U || selector_count > 18002U) return -1;
    if (bzip2_read_selectors(reader, selectors, selector_count, group_count) != 0) return -1;
    for (group = 0U; group < group_count; ++group) {
        unsigned char lengths[BZIP2_MAX_ALPHA_SIZE];
        unsigned int symbol;
        unsigned int current;

        if (bzip2_read_bits(reader, 5U, &current) != 0 || current == 0U || current > BZIP2_MAX_CODE_BITS) return -1;
        for (symbol = 0U; symbol < alpha_size; ++symbol) {
            unsigned int bit;
            for (;;) {
                if (bzip2_read_bits(reader, 1U, &bit) != 0) return -1;
                if (bit == 0U) break;
                if (bzip2_read_bits(reader, 1U, &bit) != 0) return -1;
                if (bit != 0U) {
                    if (current == 0U) return -1;
                    current -= 1U;
                } else {
                    current += 1U;
                    if (current > BZIP2_MAX_CODE_BITS) return -1;
                }
            }
            lengths[symbol] = (unsigned char)current;
        }
        if (bzip2_huffman_build(&tables[group], lengths, alpha_size) != 0) return -1;
    }
    *selector_count_out = selector_count;
    return 0;
}

static int bzip2_next_symbol(Bzip2BitReader *reader, const Bzip2HuffmanTable *tables, const unsigned char *selectors, unsigned int selector_count, unsigned int *selector_index_io, unsigned int *selector_remaining_io, unsigned int *symbol_out) {
    unsigned int selector;

    if (*selector_remaining_io == 0U) {
        if (*selector_index_io >= selector_count) return -1;
        *selector_remaining_io = BZIP2_SELECTOR_GROUP_SIZE;
        *selector_index_io += 1U;
    }
    selector = selectors[*selector_index_io - 1U];
    *selector_remaining_io -= 1U;
    return bzip2_huffman_decode(reader, &tables[selector], symbol_out);
}

static int bzip2_read_used_bytes(Bzip2BitReader *reader, unsigned char *seq_to_unseq, unsigned int *used_count_out) {
    unsigned int groups = 0U;
    unsigned int index;
    unsigned int count = 0U;

    for (index = 0U; index < 16U; ++index) {
        unsigned int bit;
        if (bzip2_read_bits(reader, 1U, &bit) != 0) return -1;
        if (bit != 0U) groups |= 1U << index;
    }
    for (index = 0U; index < 16U; ++index) {
        unsigned int sub;
        if ((groups & (1U << index)) == 0U) continue;
        for (sub = 0U; sub < 16U; ++sub) {
            unsigned int bit;
            if (bzip2_read_bits(reader, 1U, &bit) != 0) return -1;
            if (bit != 0U) seq_to_unseq[count++] = (unsigned char)(index * 16U + sub);
        }
    }
    if (count == 0U) return -1;
    *used_count_out = count;
    return 0;
}

static int bzip2_decode_huffman_data(Bzip2BitReader *reader, Bzip2Decoder *decoder, unsigned int used_count, const unsigned char *seq_to_unseq, const Bzip2HuffmanTable *tables, const unsigned char *selectors, unsigned int selector_count, unsigned int *block_used_out) {
    unsigned char mtf[256];
    unsigned int selector_index = 0U;
    unsigned int selector_remaining = 0U;
    unsigned int block_used = 0U;
    unsigned int eob = used_count + 1U;
    unsigned int index;

    for (index = 0U; index < used_count; ++index) mtf[index] = (unsigned char)index;
    for (;;) {
        unsigned int symbol;
        if (bzip2_next_symbol(reader, tables, selectors, selector_count, &selector_index, &selector_remaining, &symbol) != 0) return -1;
        if (symbol == eob) break;
        if (symbol == 0U || symbol == 1U) {
            unsigned int run_length = 0U;
            unsigned int run_power = 1U;
            unsigned char value;

            do {
                if (symbol == 0U) run_length += run_power;
                else run_length += run_power << 1U;
                run_power <<= 1U;
                if (bzip2_next_symbol(reader, tables, selectors, selector_count, &selector_index, &selector_remaining, &symbol) != 0) return -1;
            } while (symbol == 0U || symbol == 1U);
            value = seq_to_unseq[mtf[0]];
            while (run_length > 0U) {
                if (block_used >= decoder->block_size) return -1;
                decoder->ll8[block_used++] = value;
                run_length -= 1U;
            }
            if (symbol == eob) break;
        }
        if (symbol > 1U && symbol < eob) {
            unsigned int mtf_index = symbol - 1U;
            unsigned char mtf_value;
            if (mtf_index >= used_count) return -1;
            mtf_value = mtf[mtf_index];
            while (mtf_index > 0U) {
                mtf[mtf_index] = mtf[mtf_index - 1U];
                mtf_index -= 1U;
            }
            mtf[0] = mtf_value;
            if (block_used >= decoder->block_size) return -1;
            decoder->ll8[block_used++] = seq_to_unseq[mtf_value];
        } else if (symbol != eob) {
            return -1;
        }
    }
    *block_used_out = block_used;
    return 0;
}

static int bzip2_inverse_bwt_emit(Bzip2Decoder *decoder, unsigned int block_used, unsigned int original_pointer, Bzip2Output *output) {
    unsigned int counts[257];
    unsigned int index;
    unsigned int next;

    if (block_used == 0U) return 0;
    if (original_pointer >= block_used) return -1;
    rt_memset(counts, 0, sizeof(counts));
    for (index = 0U; index < block_used; ++index) counts[(unsigned int)decoder->ll8[index] + 1U] += 1U;
    for (index = 1U; index < 257U; ++index) counts[index] += counts[index - 1U];
    for (index = 0U; index < block_used; ++index) {
        unsigned char value = decoder->ll8[index];
        decoder->tt[counts[value]++] = index;
    }
    next = decoder->tt[original_pointer];
    for (index = 0U; index < block_used; ++index) {
        unsigned char value = decoder->ll8[next];
        if (bzip2_output_rle_byte(output, value) != 0) return -1;
        next = decoder->tt[next];
    }
    return output->await_run_extra ? -1 : 0;
}

static int bzip2_decode_block(Bzip2BitReader *reader, Bzip2Decoder *decoder, CompressionBzip2WriteFn write_fn, void *write_context, unsigned int *block_crc_out) {
    Bzip2HuffmanTable tables[BZIP2_MAX_GROUPS];
    unsigned char selectors[18002];
    unsigned char seq_to_unseq[256];
    Bzip2Output output;
    unsigned int stored_crc;
    unsigned int randomized;
    unsigned int original_pointer;
    unsigned int used_count;
    unsigned int selector_count;
    unsigned int block_used;
    unsigned int alpha_size;

    if (bzip2_read_bits(reader, 32U, &stored_crc) != 0 || bzip2_read_bits(reader, 1U, &randomized) != 0 || bzip2_read_bits(reader, 24U, &original_pointer) != 0) return -1;
    if (randomized != 0U) return -1;
    if (bzip2_read_used_bytes(reader, seq_to_unseq, &used_count) != 0) return -1;
    alpha_size = used_count + 2U;
    if (alpha_size > BZIP2_MAX_ALPHA_SIZE) return -1;
    if (bzip2_read_tables(reader, tables, selectors, &selector_count, alpha_size) != 0) return -1;
    if (bzip2_decode_huffman_data(reader, decoder, used_count, seq_to_unseq, tables, selectors, selector_count, &block_used) != 0) return -1;
    bzip2_output_init(&output, write_fn, write_context);
    if (bzip2_inverse_bwt_emit(decoder, block_used, original_pointer, &output) != 0 || bzip2_output_flush(&output) != 0) return -1;
    output.crc ^= 0xffffffffU;
    if (output.crc != stored_crc) return -1;
    *block_crc_out = stored_crc;
    return 0;
}

int compression_bzip2_decompress_stream(CompressionBzip2ReadFn read_fn, void *read_context, CompressionBzip2WriteFn write_fn, void *write_context) {
    Bzip2BitReader reader;
    Bzip2Decoder decoder;
    unsigned int byte_value;
    unsigned int block_digit;
    unsigned int combined_crc = 0U;
    int result = -1;

    if (read_fn == 0 || write_fn == 0) return -1;
    rt_memset(&decoder, 0, sizeof(decoder));
    bzip2_reader_init(&reader, read_fn, read_context);
    if (bzip2_read_byte(&reader, &byte_value) != 0 || byte_value != 'B' ||
        bzip2_read_byte(&reader, &byte_value) != 0 || byte_value != 'Z' ||
        bzip2_read_byte(&reader, &byte_value) != 0 || byte_value != 'h' ||
        bzip2_read_byte(&reader, &block_digit) != 0 || block_digit < '1' || block_digit > '9') {
        return -1;
    }
    decoder.block_size = (block_digit - '0') * 100000U;
    decoder.ll8 = (unsigned char *)rt_malloc(decoder.block_size);
    decoder.tt = (unsigned int *)rt_malloc_array(decoder.block_size, sizeof(decoder.tt[0]));
    if (decoder.ll8 == 0 || decoder.tt == 0) goto done;
    for (;;) {
        unsigned long long magic;
        if (bzip2_read_magic(&reader, &magic) != 0) goto done;
        if (magic == BZIP2_END_MAGIC) {
            unsigned int stored_combined_crc;
            if (bzip2_read_bits(&reader, 32U, &stored_combined_crc) != 0) goto done;
            if (stored_combined_crc != combined_crc) goto done;
            result = 0;
            break;
        }
        if (magic == BZIP2_STREAM_MAGIC) {
            unsigned int block_crc;
            if (bzip2_decode_block(&reader, &decoder, write_fn, write_context, &block_crc) != 0) goto done;
            combined_crc = ((combined_crc << 1U) | (combined_crc >> 31U)) ^ block_crc;
        } else {
            goto done;
        }
    }

done:
    rt_free(decoder.ll8);
    rt_free(decoder.tt);
    return result;
}
