#include "compression/bzip2.h"

#include "runtime.h"

#define BZIP2_IO_BUFFER_SIZE 32768U
#define BZIP2_MAX_GROUPS 6U
#define BZIP2_MAX_ALPHA_SIZE 258U
#define BZIP2_MAX_CODE_BITS 23U
#define BZIP2_SELECTOR_GROUP_SIZE 50U
#define BZIP2_STREAM_MAGIC 0x314159265359ULL
#define BZIP2_END_MAGIC 0x177245385090ULL
#define BZIP2_HUFFMAN_FAST_BITS 10U
#define BZIP2_HUFFMAN_FAST_SIZE (1U << BZIP2_HUFFMAN_FAST_BITS)

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
    unsigned short fast_symbol[BZIP2_HUFFMAN_FAST_SIZE];
    unsigned char fast_bits[BZIP2_HUFFMAN_FAST_SIZE];
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

static int bzip2_ensure_bits(Bzip2BitReader *reader, unsigned int count) {
    if (count == 0U || count > 32U) return -1;
    while (reader->bit_count < count) {
        unsigned int byte_value;
        if (bzip2_read_byte(reader, &byte_value) != 0) return -1;
        reader->bit_buffer = (reader->bit_buffer << 8U) | byte_value;
        reader->bit_count += 8U;
    }
    return 0;
}

static unsigned int bzip2_peek_bits(const Bzip2BitReader *reader, unsigned int count) {
    return (unsigned int)((reader->bit_buffer >> (reader->bit_count - count)) & ((count == 32U) ? 0xffffffffULL : ((1ULL << count) - 1ULL)));
}

static void bzip2_drop_bits(Bzip2BitReader *reader, unsigned int count) {
    reader->bit_count -= count;
    if (reader->bit_count == 0U) {
        reader->bit_buffer = 0U;
    } else {
        reader->bit_buffer &= (1ULL << reader->bit_count) - 1ULL;
    }
}

static int bzip2_read_bits(Bzip2BitReader *reader, unsigned int count, unsigned int *value_out) {
    unsigned int value;

    if (bzip2_ensure_bits(reader, count) != 0) return -1;
    value = bzip2_peek_bits(reader, count);
    bzip2_drop_bits(reader, count);
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

static const unsigned int bzip2_crc_table[256] = {
    0x00000000U, 0x04c11db7U, 0x09823b6eU, 0x0d4326d9U, 0x130476dcU, 0x17c56b6bU, 0x1a864db2U, 0x1e475005U, 0x2608edb8U, 0x22c9f00fU, 0x2f8ad6d6U, 0x2b4bcb61U, 0x350c9b64U, 0x31cd86d3U, 0x3c8ea00aU, 0x384fbdbdU,
    0x4c11db70U, 0x48d0c6c7U, 0x4593e01eU, 0x4152fda9U, 0x5f15adacU, 0x5bd4b01bU, 0x569796c2U, 0x52568b75U, 0x6a1936c8U, 0x6ed82b7fU, 0x639b0da6U, 0x675a1011U, 0x791d4014U, 0x7ddc5da3U, 0x709f7b7aU, 0x745e66cdU,
    0x9823b6e0U, 0x9ce2ab57U, 0x91a18d8eU, 0x95609039U, 0x8b27c03cU, 0x8fe6dd8bU, 0x82a5fb52U, 0x8664e6e5U, 0xbe2b5b58U, 0xbaea46efU, 0xb7a96036U, 0xb3687d81U, 0xad2f2d84U, 0xa9ee3033U, 0xa4ad16eaU, 0xa06c0b5dU,
    0xd4326d90U, 0xd0f37027U, 0xddb056feU, 0xd9714b49U, 0xc7361b4cU, 0xc3f706fbU, 0xceb42022U, 0xca753d95U, 0xf23a8028U, 0xf6fb9d9fU, 0xfbb8bb46U, 0xff79a6f1U, 0xe13ef6f4U, 0xe5ffeb43U, 0xe8bccd9aU, 0xec7dd02dU,
    0x34867077U, 0x30476dc0U, 0x3d044b19U, 0x39c556aeU, 0x278206abU, 0x23431b1cU, 0x2e003dc5U, 0x2ac12072U, 0x128e9dcfU, 0x164f8078U, 0x1b0ca6a1U, 0x1fcdbb16U, 0x018aeb13U, 0x054bf6a4U, 0x0808d07dU, 0x0cc9cdcaU,
    0x7897ab07U, 0x7c56b6b0U, 0x71159069U, 0x75d48ddeU, 0x6b93dddbU, 0x6f52c06cU, 0x6211e6b5U, 0x66d0fb02U, 0x5e9f46bfU, 0x5a5e5b08U, 0x571d7dd1U, 0x53dc6066U, 0x4d9b3063U, 0x495a2dd4U, 0x44190b0dU, 0x40d816baU,
    0xaca5c697U, 0xa864db20U, 0xa527fdf9U, 0xa1e6e04eU, 0xbfa1b04bU, 0xbb60adfcU, 0xb6238b25U, 0xb2e29692U, 0x8aad2b2fU, 0x8e6c3698U, 0x832f1041U, 0x87ee0df6U, 0x99a95df3U, 0x9d684044U, 0x902b669dU, 0x94ea7b2aU,
    0xe0b41de7U, 0xe4750050U, 0xe9362689U, 0xedf73b3eU, 0xf3b06b3bU, 0xf771768cU, 0xfa325055U, 0xfef34de2U, 0xc6bcf05fU, 0xc27dede8U, 0xcf3ecb31U, 0xcbffd686U, 0xd5b88683U, 0xd1799b34U, 0xdc3abdedU, 0xd8fba05aU,
    0x690ce0eeU, 0x6dcdfd59U, 0x608edb80U, 0x644fc637U, 0x7a089632U, 0x7ec98b85U, 0x738aad5cU, 0x774bb0ebU, 0x4f040d56U, 0x4bc510e1U, 0x46863638U, 0x42472b8fU, 0x5c007b8aU, 0x58c1663dU, 0x558240e4U, 0x51435d53U,
    0x251d3b9eU, 0x21dc2629U, 0x2c9f00f0U, 0x285e1d47U, 0x36194d42U, 0x32d850f5U, 0x3f9b762cU, 0x3b5a6b9bU, 0x0315d626U, 0x07d4cb91U, 0x0a97ed48U, 0x0e56f0ffU, 0x1011a0faU, 0x14d0bd4dU, 0x19939b94U, 0x1d528623U,
    0xf12f560eU, 0xf5ee4bb9U, 0xf8ad6d60U, 0xfc6c70d7U, 0xe22b20d2U, 0xe6ea3d65U, 0xeba91bbcU, 0xef68060bU, 0xd727bbb6U, 0xd3e6a601U, 0xdea580d8U, 0xda649d6fU, 0xc423cd6aU, 0xc0e2d0ddU, 0xcda1f604U, 0xc960ebb3U,
    0xbd3e8d7eU, 0xb9ff90c9U, 0xb4bcb610U, 0xb07daba7U, 0xae3afba2U, 0xaafbe615U, 0xa7b8c0ccU, 0xa379dd7bU, 0x9b3660c6U, 0x9ff77d71U, 0x92b45ba8U, 0x9675461fU, 0x8832161aU, 0x8cf30badU, 0x81b02d74U, 0x857130c3U,
    0x5d8a9099U, 0x594b8d2eU, 0x5408abf7U, 0x50c9b640U, 0x4e8ee645U, 0x4a4ffbf2U, 0x470cdd2bU, 0x43cdc09cU, 0x7b827d21U, 0x7f436096U, 0x7200464fU, 0x76c15bf8U, 0x68860bfdU, 0x6c47164aU, 0x61043093U, 0x65c52d24U,
    0x119b4be9U, 0x155a565eU, 0x18197087U, 0x1cd86d30U, 0x029f3d35U, 0x065e2082U, 0x0b1d065bU, 0x0fdc1becU, 0x3793a651U, 0x3352bbe6U, 0x3e119d3fU, 0x3ad08088U, 0x2497d08dU, 0x2056cd3aU, 0x2d15ebe3U, 0x29d4f654U,
    0xc5a92679U, 0xc1683bceU, 0xcc2b1d17U, 0xc8ea00a0U, 0xd6ad50a5U, 0xd26c4d12U, 0xdf2f6bcbU, 0xdbee767cU, 0xe3a1cbc1U, 0xe760d676U, 0xea23f0afU, 0xeee2ed18U, 0xf0a5bd1dU, 0xf464a0aaU, 0xf9278673U, 0xfde69bc4U,
    0x89b8fd09U, 0x8d79e0beU, 0x803ac667U, 0x84fbdbd0U, 0x9abc8bd5U, 0x9e7d9662U, 0x933eb0bbU, 0x97ffad0cU, 0xafb010b1U, 0xab710d06U, 0xa6322bdfU, 0xa2f33668U, 0xbcb4666dU, 0xb8757bdaU, 0xb5365d03U, 0xb1f740b4U
};

static unsigned int bzip2_crc_update_byte(unsigned int crc, unsigned char value) {
    return (crc << 8U) ^ bzip2_crc_table[((crc >> 24U) ^ (unsigned int)value) & 0xffU];
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
    unsigned int index;

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
    for (length = table->min_length; length <= table->max_length && length <= BZIP2_HUFFMAN_FAST_BITS; ++length) {
        unsigned int count = table->symbol_count[length];
        unsigned int first = table->first_symbol[length];

        for (index = 0U; index < count; ++index) {
            unsigned int huff_code = table->first_code[length] + index;
            unsigned int prefix = huff_code << (BZIP2_HUFFMAN_FAST_BITS - length);
            unsigned int fill = 1U << (BZIP2_HUFFMAN_FAST_BITS - length);
            unsigned int slot;

            for (slot = 0U; slot < fill; ++slot) {
                table->fast_symbol[prefix + slot] = table->symbols[first + index];
                table->fast_bits[prefix + slot] = (unsigned char)length;
            }
        }
    }
    return 0;
}

static int bzip2_huffman_decode(Bzip2BitReader *reader, const Bzip2HuffmanTable *table, unsigned int *symbol_out) {
    unsigned int code = 0U;
    unsigned int length;

    if (reader->bit_count >= BZIP2_HUFFMAN_FAST_BITS || bzip2_ensure_bits(reader, BZIP2_HUFFMAN_FAST_BITS) == 0) {
        unsigned int fast_index = bzip2_peek_bits(reader, BZIP2_HUFFMAN_FAST_BITS);
        unsigned int fast_bits = table->fast_bits[fast_index];

        if (fast_bits != 0U) {
            bzip2_drop_bits(reader, fast_bits);
            *symbol_out = table->fast_symbol[fast_index];
            return 0;
        }
    }
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
