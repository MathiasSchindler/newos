static const unsigned char expack_deflate_bcj_stub_x86_64[] = {
#include "deflate_stub.inc"
};

/* The runner intentionally accepts only the single final dynamic block emitted and verified below. */
#define EXPACK_DEFLATE_MIN_MATCH 3U
#define EXPACK_DEFLATE_MAX_MATCH 258U
#define EXPACK_DEFLATE_MAX_DISTANCE 32768U
#define EXPACK_DEFLATE_HASH_BITS 16U
#define EXPACK_DEFLATE_HASH_SIZE (1U << EXPACK_DEFLATE_HASH_BITS)
#define EXPACK_DEFLATE_CHAIN_LIMIT 96U

typedef struct {
    unsigned short length;
    unsigned short distance;
    unsigned char literal;
} ExpackDeflateToken;

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
    unsigned int bit_buffer;
    unsigned int bit_count;
    int failed;
} ExpackDeflateWriter;

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t offset;
    unsigned int bit_buffer;
    unsigned int bit_count;
    int failed;
} ExpackDeflateReader;

typedef struct {
    unsigned short table[32768];
    unsigned char bits[288];
    unsigned short codes[288];
    unsigned int table_bits;
} ExpackDeflateHuff;

static const unsigned short expack_deflate_len_base[29] = {
    3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 13U, 15U, 17U, 19U, 23U, 27U,
    31U, 35U, 43U, 51U, 59U, 67U, 83U, 99U, 115U, 131U, 163U, 195U, 227U, 258U
};

static const unsigned char expack_deflate_len_extra[29] = {
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 2U, 2U, 2U,
    2U, 3U, 3U, 3U, 3U, 4U, 4U, 4U, 4U, 5U, 5U, 5U, 5U, 0U
};

static const unsigned short expack_deflate_dist_base[30] = {
    1U, 2U, 3U, 4U, 5U, 7U, 9U, 13U, 17U, 25U, 33U, 49U, 65U, 97U, 129U,
    193U, 257U, 385U, 513U, 769U, 1025U, 1537U, 2049U, 3073U, 4097U, 6145U,
    8193U, 12289U, 16385U, 24577U
};

static const unsigned char expack_deflate_dist_extra[30] = {
    0U, 0U, 0U, 0U, 1U, 1U, 2U, 2U, 3U, 3U, 4U, 4U, 5U, 5U, 6U,
    6U, 7U, 7U, 8U, 8U, 9U, 9U, 10U, 10U, 11U, 11U, 12U, 12U, 13U, 13U
};

static unsigned int expack_deflate_hash3(const unsigned char *data) {
    unsigned int value = ((unsigned int)data[0] << 16) ^ ((unsigned int)data[1] << 8) ^ (unsigned int)data[2];
    return (value * 2654435761U) >> (32U - EXPACK_DEFLATE_HASH_BITS);
}

static void expack_deflate_insert(const unsigned char *data, size_t size, size_t position, int *head, int *prev) {
    unsigned int hash;

    if (position + 2U >= size) {
        return;
    }
    hash = expack_deflate_hash3(data + position);
    prev[position] = head[hash];
    head[hash] = (int)position;
}

static void expack_deflate_find_match(const unsigned char *data, size_t size, size_t position, const int *head, const int *prev, unsigned int *length_out, unsigned int *distance_out) {
    unsigned int best_length = 0U;
    unsigned int best_distance = 0U;
    unsigned int hash;
    int candidate;
    unsigned int chain_count = 0U;
    unsigned int max_length;

    if (position + EXPACK_DEFLATE_MIN_MATCH > size) {
        *length_out = 0U;
        *distance_out = 0U;
        return;
    }
    max_length = (unsigned int)(size - position);
    if (max_length > EXPACK_DEFLATE_MAX_MATCH) {
        max_length = EXPACK_DEFLATE_MAX_MATCH;
    }
    hash = expack_deflate_hash3(data + position);
    candidate = head[hash];
    while (candidate >= 0 && chain_count < EXPACK_DEFLATE_CHAIN_LIMIT) {
        unsigned int distance = (unsigned int)(position - (size_t)candidate);
        unsigned int length = 0U;

        if (distance > EXPACK_DEFLATE_MAX_DISTANCE) {
            break;
        }
        if (data[(size_t)candidate + best_length] == data[position + best_length]) {
            while (length < max_length && data[(size_t)candidate + length] == data[position + length]) {
                length += 1U;
            }
            if (length >= EXPACK_DEFLATE_MIN_MATCH && length > best_length) {
                best_length = length;
                best_distance = distance;
                if (length == max_length) {
                    break;
                }
            }
        }
        candidate = prev[candidate];
        chain_count += 1U;
    }
    *length_out = best_length;
    *distance_out = best_distance;
}

static int expack_deflate_make_tokens(const unsigned char *data, size_t size, ExpackDeflateToken **tokens_out, size_t *token_count_out) {
    ExpackDeflateToken *tokens;
    int *head;
    int *prev;
    size_t position = 0U;
    size_t token_count = 0U;
    unsigned int index;

    if (size > 0x7fffffffU) {
        return -1;
    }
    tokens = (ExpackDeflateToken *)rt_malloc((size + 1U) * sizeof(ExpackDeflateToken));
    head = (int *)rt_malloc(EXPACK_DEFLATE_HASH_SIZE * sizeof(int));
    prev = (int *)rt_malloc((size == 0U ? 1U : size) * sizeof(int));
    if (tokens == 0 || head == 0 || prev == 0) {
        if (tokens != 0) rt_free(tokens);
        if (head != 0) rt_free(head);
        if (prev != 0) rt_free(prev);
        return -1;
    }
    for (index = 0U; index < EXPACK_DEFLATE_HASH_SIZE; ++index) {
        head[index] = -1;
    }
    while (position < size) {
        unsigned int length;
        unsigned int distance;

        expack_deflate_find_match(data, size, position, head, prev, &length, &distance);
        expack_deflate_insert(data, size, position, head, prev);
        if (length >= EXPACK_DEFLATE_MIN_MATCH && position + 1U < size) {
            unsigned int next_length;
            unsigned int next_distance;

            expack_deflate_find_match(data, size, position + 1U, head, prev, &next_length, &next_distance);
            (void)next_distance;
            if (next_length > length + 1U) {
                length = 0U;
            }
        }
        if (length >= EXPACK_DEFLATE_MIN_MATCH) {
            unsigned int offset;

            tokens[token_count].length = (unsigned short)length;
            tokens[token_count].distance = (unsigned short)distance;
            tokens[token_count].literal = 0U;
            token_count += 1U;
            for (offset = 1U; offset < length; ++offset) {
                expack_deflate_insert(data, size, position + offset, head, prev);
            }
            position += length;
        } else {
            tokens[token_count].length = 0U;
            tokens[token_count].distance = 0U;
            tokens[token_count].literal = data[position];
            token_count += 1U;
            position += 1U;
        }
    }
    rt_free(head);
    rt_free(prev);
    *tokens_out = tokens;
    *token_count_out = token_count;
    return 0;
}

static unsigned int expack_deflate_length_symbol(unsigned int length, unsigned int *extra_value_out, unsigned int *extra_bits_out) {
    unsigned int index;

    for (index = 0U; index < 29U; ++index) {
        unsigned int base = expack_deflate_len_base[index];
        unsigned int limit = base + ((1U << expack_deflate_len_extra[index]) - 1U);

        if (length <= limit) {
            *extra_value_out = length - base;
            *extra_bits_out = expack_deflate_len_extra[index];
            return 257U + index;
        }
    }
    *extra_value_out = 0U;
    *extra_bits_out = 0U;
    return 285U;
}

static unsigned int expack_deflate_distance_symbol(unsigned int distance, unsigned int *extra_value_out, unsigned int *extra_bits_out) {
    unsigned int index;

    for (index = 0U; index < 30U; ++index) {
        unsigned int base = expack_deflate_dist_base[index];
        unsigned int limit = base + ((1U << expack_deflate_dist_extra[index]) - 1U);

        if (distance <= limit) {
            *extra_value_out = distance - base;
            *extra_bits_out = expack_deflate_dist_extra[index];
            return index;
        }
    }
    *extra_value_out = 0U;
    *extra_bits_out = 0U;
    return 29U;
}

static void expack_deflate_count_tokens(const ExpackDeflateToken *tokens, size_t token_count, unsigned int *literal_freq, unsigned int *distance_freq) {
    size_t index;

    memset(literal_freq, 0, 286U * sizeof(unsigned int));
    memset(distance_freq, 0, 30U * sizeof(unsigned int));
    for (index = 0U; index < token_count; ++index) {
        if (tokens[index].length == 0U) {
            literal_freq[tokens[index].literal] += 1U;
        } else {
            unsigned int extra_value;
            unsigned int extra_bits;
            unsigned int symbol = expack_deflate_length_symbol(tokens[index].length, &extra_value, &extra_bits);
            unsigned int distance_symbol = expack_deflate_distance_symbol(tokens[index].distance, &extra_value, &extra_bits);
            literal_freq[symbol] += 1U;
            distance_freq[distance_symbol] += 1U;
        }
    }
    literal_freq[256] += 1U;
    if (distance_freq[0] == 0U) {
        distance_freq[0] = 1U;
    }
}

static int expack_deflate_lowest_node(const unsigned int *freq, const int *active, unsigned int active_count) {
    unsigned int index;
    unsigned int best_index = 0U;

    for (index = 1U; index < active_count; ++index) {
        if (freq[active[index]] < freq[active[best_index]] ||
            (freq[active[index]] == freq[active[best_index]] && active[index] < active[best_index])) {
            best_index = index;
        }
    }
    return (int)best_index;
}

static void expack_deflate_build_lengths(const unsigned int *freq, unsigned int symbol_count, unsigned int max_bits, unsigned char *lengths) {
    unsigned int work_freq[576];
    unsigned int node_freq[576];
    short parent[576];
    short leaf_node[576];
    int active[576];
    unsigned int symbol;
    unsigned int scale_pass;

    memset(lengths, 0, symbol_count);
    for (symbol = 0U; symbol < symbol_count; ++symbol) {
        work_freq[symbol] = freq[symbol];
    }
    for (scale_pass = 0U; scale_pass < 32U; ++scale_pass) {
        unsigned int node_count = 0U;
        unsigned int active_count = 0U;
        unsigned int max_length = 0U;

        for (symbol = 0U; symbol < symbol_count; ++symbol) {
            leaf_node[symbol] = -1;
        }
        for (symbol = 0U; symbol < symbol_count; ++symbol) {
            if (work_freq[symbol] != 0U) {
                node_freq[node_count] = work_freq[symbol];
                parent[node_count] = -1;
                leaf_node[symbol] = (short)node_count;
                active[active_count++] = (int)node_count;
                node_count += 1U;
            }
        }
        if (active_count == 0U) {
            return;
        }
        if (active_count == 1U) {
            for (symbol = 0U; symbol < symbol_count; ++symbol) {
                if (work_freq[symbol] != 0U) {
                    lengths[symbol] = 1U;
                    return;
                }
            }
        }
        while (active_count > 1U) {
            int first_slot = expack_deflate_lowest_node(node_freq, active, active_count);
            int first = active[first_slot];
            int second_slot;
            int second;

            active[first_slot] = active[active_count - 1U];
            active_count -= 1U;
            second_slot = expack_deflate_lowest_node(node_freq, active, active_count);
            second = active[second_slot];
            active[second_slot] = (int)node_count;
            node_freq[node_count] = node_freq[first] + node_freq[second];
            parent[first] = (short)node_count;
            parent[second] = (short)node_count;
            parent[node_count] = -1;
            node_count += 1U;
        }
        for (symbol = 0U; symbol < symbol_count; ++symbol) {
            if (work_freq[symbol] != 0U) {
                unsigned int depth = 0U;
                short node = leaf_node[symbol];

                while (parent[node] >= 0) {
                    depth += 1U;
                    node = parent[node];
                }
                lengths[symbol] = (unsigned char)depth;
                if (depth > max_length) {
                    max_length = depth;
                }
            }
        }
        if (max_length <= max_bits) {
            return;
        }
        for (symbol = 0U; symbol < symbol_count; ++symbol) {
            if (work_freq[symbol] > 1U) {
                work_freq[symbol] = (work_freq[symbol] + 1U) >> 1;
            }
        }
    }
}

static unsigned int expack_deflate_reverse_bits(unsigned int code, unsigned int length) {
    unsigned int result = 0U;
    unsigned int index;

    for (index = 0U; index < length; ++index) {
        result = (result << 1) | (code & 1U);
        code >>= 1;
    }
    return result;
}

static void expack_deflate_make_codes(const unsigned char *lengths, unsigned int symbol_count, unsigned short *codes) {
    unsigned int count[16];
    unsigned int next_code[16];
    unsigned int code = 0U;
    unsigned int bits;
    unsigned int symbol;

    memset(count, 0, sizeof(count));
    memset(next_code, 0, sizeof(next_code));
    for (symbol = 0U; symbol < symbol_count; ++symbol) {
        if (lengths[symbol] != 0U) {
            count[lengths[symbol]] += 1U;
        }
    }
    for (bits = 1U; bits <= 15U; ++bits) {
        code = (code + count[bits - 1U]) << 1;
        next_code[bits] = code;
    }
    for (symbol = 0U; symbol < symbol_count; ++symbol) {
        unsigned int length = lengths[symbol];

        if (length != 0U) {
            codes[symbol] = (unsigned short)expack_deflate_reverse_bits(next_code[length], length);
            next_code[length] += 1U;
        } else {
            codes[symbol] = 0U;
        }
    }
}

static int expack_deflate_writer_init(ExpackDeflateWriter *writer, size_t input_size) {
    size_t capacity = input_size + (input_size >> 1) + 65536U;

    writer->data = (unsigned char *)rt_malloc(capacity);
    if (writer->data == 0) {
        return -1;
    }
    writer->size = 0U;
    writer->capacity = capacity;
    writer->bit_buffer = 0U;
    writer->bit_count = 0U;
    writer->failed = 0;
    return 0;
}

static void expack_deflate_write_bits(ExpackDeflateWriter *writer, unsigned int value, unsigned int bit_count) {
    writer->bit_buffer |= value << writer->bit_count;
    writer->bit_count += bit_count;
    while (writer->bit_count >= 8U) {
        if (writer->size >= writer->capacity) {
            writer->failed = 1;
            return;
        }
        writer->data[writer->size++] = (unsigned char)(writer->bit_buffer & 0xffU);
        writer->bit_buffer >>= 8;
        writer->bit_count -= 8U;
    }
}

static void expack_deflate_flush_bits(ExpackDeflateWriter *writer) {
    if (writer->bit_count != 0U) {
        if (writer->size >= writer->capacity) {
            writer->failed = 1;
            return;
        }
        writer->data[writer->size++] = (unsigned char)(writer->bit_buffer & 0xffU);
        writer->bit_buffer = 0U;
        writer->bit_count = 0U;
    }
}

static void expack_deflate_emit_code(ExpackDeflateWriter *writer, const unsigned short *codes, const unsigned char *lengths, unsigned int symbol) {
    expack_deflate_write_bits(writer, codes[symbol], lengths[symbol]);
}

static unsigned int expack_deflate_rle_lengths(const unsigned char *lengths, unsigned int count, unsigned char *symbols, unsigned char *extra) {
    unsigned int position = 0U;
    unsigned int output_count = 0U;

    while (position < count) {
        unsigned int value = lengths[position];
        unsigned int run = 1U;

        while (position + run < count && lengths[position + run] == value) {
            run += 1U;
        }
        if (value == 0U) {
            while (run >= 11U) {
                unsigned int chunk = run > 138U ? 138U : run;
                symbols[output_count] = 18U;
                extra[output_count++] = (unsigned char)(chunk - 11U);
                run -= chunk;
            }
            if (run >= 3U) {
                symbols[output_count] = 17U;
                extra[output_count++] = (unsigned char)(run - 3U);
                run = 0U;
            }
            while (run != 0U) {
                symbols[output_count] = 0U;
                extra[output_count++] = 0U;
                run -= 1U;
            }
        } else {
            symbols[output_count] = (unsigned char)value;
            extra[output_count++] = 0U;
            run -= 1U;
            while (run >= 3U) {
                unsigned int chunk = run > 6U ? 6U : run;
                symbols[output_count] = 16U;
                extra[output_count++] = (unsigned char)(chunk - 3U);
                run -= chunk;
            }
            while (run != 0U) {
                symbols[output_count] = (unsigned char)value;
                extra[output_count++] = 0U;
                run -= 1U;
            }
        }
        position += 1U;
        while (position < count && lengths[position] == value) {
            position += 1U;
        }
    }
    return output_count;
}

static int expack_deflate_encode_dynamic(const unsigned char *data, size_t size, unsigned char **payload_out, size_t *payload_size_out) {
    ExpackDeflateToken *tokens = 0;
    size_t token_count = 0U;
    unsigned int literal_freq[286];
    unsigned int distance_freq[30];
    unsigned char literal_lengths[286];
    unsigned char distance_lengths[30];
    unsigned short literal_codes[286];
    unsigned short distance_codes[30];
    unsigned char all_lengths[316];
    unsigned char rle_symbols[640];
    unsigned char rle_extra[640];
    unsigned int code_length_freq[19];
    unsigned char code_length_lengths[19];
    unsigned short code_length_codes[19];
    static const unsigned char code_length_order[19] = {
        16U, 17U, 18U, 0U, 8U, 7U, 9U, 6U, 10U, 5U, 11U, 4U, 12U, 3U, 13U, 2U, 14U, 1U, 15U
    };
    ExpackDeflateWriter writer;
    unsigned int literal_count;
    unsigned int distance_count;
    unsigned int rle_count;
    unsigned int hclen_count;
    size_t index;

    if (expack_deflate_make_tokens(data, size, &tokens, &token_count) != 0) {
        return -1;
    }
    expack_deflate_count_tokens(tokens, token_count, literal_freq, distance_freq);
    expack_deflate_build_lengths(literal_freq, 286U, 15U, literal_lengths);
    expack_deflate_build_lengths(distance_freq, 30U, 15U, distance_lengths);
    expack_deflate_make_codes(literal_lengths, 286U, literal_codes);
    expack_deflate_make_codes(distance_lengths, 30U, distance_codes);

    literal_count = 286U;
    while (literal_count > 257U && literal_lengths[literal_count - 1U] == 0U) {
        literal_count -= 1U;
    }
    distance_count = 30U;
    while (distance_count > 1U && distance_lengths[distance_count - 1U] == 0U) {
        distance_count -= 1U;
    }
    memcpy(all_lengths, literal_lengths, literal_count);
    memcpy(all_lengths + literal_count, distance_lengths, distance_count);
    rle_count = expack_deflate_rle_lengths(all_lengths, literal_count + distance_count, rle_symbols, rle_extra);
    memset(code_length_freq, 0, sizeof(code_length_freq));
    for (index = 0U; index < rle_count; ++index) {
        code_length_freq[rle_symbols[index]] += 1U;
    }
    expack_deflate_build_lengths(code_length_freq, 19U, 7U, code_length_lengths);
    expack_deflate_make_codes(code_length_lengths, 19U, code_length_codes);
    hclen_count = 19U;
    while (hclen_count > 4U && code_length_lengths[code_length_order[hclen_count - 1U]] == 0U) {
        hclen_count -= 1U;
    }
    if (expack_deflate_writer_init(&writer, size) != 0) {
        rt_free(tokens);
        return -1;
    }
    expack_deflate_write_bits(&writer, 1U, 1U);
    expack_deflate_write_bits(&writer, 2U, 2U);
    expack_deflate_write_bits(&writer, literal_count - 257U, 5U);
    expack_deflate_write_bits(&writer, distance_count - 1U, 5U);
    expack_deflate_write_bits(&writer, hclen_count - 4U, 4U);
    for (index = 0U; index < hclen_count; ++index) {
        expack_deflate_write_bits(&writer, code_length_lengths[code_length_order[index]], 3U);
    }
    for (index = 0U; index < rle_count; ++index) {
        unsigned int symbol = rle_symbols[index];

        expack_deflate_emit_code(&writer, code_length_codes, code_length_lengths, symbol);
        if (symbol == 16U) {
            expack_deflate_write_bits(&writer, rle_extra[index], 2U);
        } else if (symbol == 17U) {
            expack_deflate_write_bits(&writer, rle_extra[index], 3U);
        } else if (symbol == 18U) {
            expack_deflate_write_bits(&writer, rle_extra[index], 7U);
        }
    }
    for (index = 0U; index < token_count; ++index) {
        if (tokens[index].length == 0U) {
            expack_deflate_emit_code(&writer, literal_codes, literal_lengths, tokens[index].literal);
        } else {
            unsigned int extra_value;
            unsigned int extra_bits;
            unsigned int symbol = expack_deflate_length_symbol(tokens[index].length, &extra_value, &extra_bits);
            unsigned int distance_symbol;

            expack_deflate_emit_code(&writer, literal_codes, literal_lengths, symbol);
            if (extra_bits != 0U) {
                expack_deflate_write_bits(&writer, extra_value, extra_bits);
            }
            distance_symbol = expack_deflate_distance_symbol(tokens[index].distance, &extra_value, &extra_bits);
            expack_deflate_emit_code(&writer, distance_codes, distance_lengths, distance_symbol);
            if (extra_bits != 0U) {
                expack_deflate_write_bits(&writer, extra_value, extra_bits);
            }
        }
    }
    expack_deflate_emit_code(&writer, literal_codes, literal_lengths, 256U);
    expack_deflate_flush_bits(&writer);
    rt_free(tokens);
    if (writer.failed) {
        rt_free(writer.data);
        return -1;
    }
    *payload_out = writer.data;
    *payload_size_out = writer.size;
    return 0;
}

static void expack_deflate_reader_init(ExpackDeflateReader *reader, const unsigned char *data, size_t size) {
    reader->data = data;
    reader->size = size;
    reader->offset = 0U;
    reader->bit_buffer = 0U;
    reader->bit_count = 0U;
    reader->failed = 0;
}

static void expack_deflate_reader_need(ExpackDeflateReader *reader, unsigned int bit_count) {
    while (reader->bit_count < bit_count && reader->offset < reader->size) {
        reader->bit_buffer |= (unsigned int)reader->data[reader->offset++] << reader->bit_count;
        reader->bit_count += 8U;
    }
}

static unsigned int expack_deflate_read_bits(ExpackDeflateReader *reader, unsigned int bit_count) {
    unsigned int value;

    expack_deflate_reader_need(reader, bit_count);
    if (reader->bit_count < bit_count) {
        reader->failed = 1;
        return 0U;
    }
    value = reader->bit_buffer & ((1U << bit_count) - 1U);
    reader->bit_buffer >>= bit_count;
    reader->bit_count -= bit_count;
    return value;
}

static int expack_deflate_build_decode(ExpackDeflateHuff *huff, const unsigned char *lengths, unsigned int symbol_count, unsigned int table_bits) {
    unsigned short codes[288];
    unsigned int symbol;
    unsigned int table_size = 1U << table_bits;

    memset(huff->table, 0xff, table_size * sizeof(unsigned short));
    memset(huff->bits, 0, sizeof(huff->bits));
    huff->table_bits = table_bits;
    expack_deflate_make_codes(lengths, symbol_count, codes);
    for (symbol = 0U; symbol < symbol_count; ++symbol) {
        unsigned int length = lengths[symbol];

        huff->bits[symbol] = (unsigned char)length;
        huff->codes[symbol] = codes[symbol];
        if (length != 0U) {
            unsigned int fill;
            unsigned int fill_count = 1U << (table_bits - length);

            for (fill = 0U; fill < fill_count; ++fill) {
                huff->table[codes[symbol] | (fill << length)] = (unsigned short)((length << 9) | symbol);
            }
        }
    }
    return 0;
}

static unsigned int expack_deflate_decode_symbol(ExpackDeflateReader *reader, const ExpackDeflateHuff *huff) {
    unsigned short entry;
    unsigned int length;

    expack_deflate_reader_need(reader, huff->table_bits);
    entry = huff->table[reader->bit_buffer & ((1U << huff->table_bits) - 1U)];
    if (entry == 0xffffU) {
        reader->failed = 1;
        return 0U;
    }
    length = entry >> 9;
    if (reader->bit_count < length) {
        reader->failed = 1;
        return 0U;
    }
    reader->bit_buffer >>= length;
    reader->bit_count -= length;
    return entry & 0x1ffU;
}

static int expack_deflate_verify_dynamic(const unsigned char *payload, size_t payload_size, const unsigned char *expected, size_t expected_size) {
    static const unsigned char code_length_order[19] = {
        16U, 17U, 18U, 0U, 8U, 7U, 9U, 6U, 10U, 5U, 11U, 4U, 12U, 3U, 13U, 2U, 14U, 1U, 15U
    };
    ExpackDeflateReader reader;
    ExpackDeflateHuff code_length_huff;
    ExpackDeflateHuff literal_huff;
    ExpackDeflateHuff distance_huff;
    unsigned char code_length_lengths[19];
    unsigned char lengths[316];
    unsigned int final_block;
    unsigned int block_type;
    unsigned int literal_count;
    unsigned int distance_count;
    unsigned int hclen_count;
    unsigned int length_count;
    size_t output_offset = 0U;
    unsigned int index;

    expack_deflate_reader_init(&reader, payload, payload_size);
    final_block = expack_deflate_read_bits(&reader, 1U);
    block_type = expack_deflate_read_bits(&reader, 2U);
    if (reader.failed || final_block != 1U || block_type != 2U) {
        return -1;
    }
    literal_count = expack_deflate_read_bits(&reader, 5U) + 257U;
    distance_count = expack_deflate_read_bits(&reader, 5U) + 1U;
    hclen_count = expack_deflate_read_bits(&reader, 4U) + 4U;
    if (reader.failed || literal_count > 286U || distance_count > 30U) {
        return -1;
    }
    memset(code_length_lengths, 0, sizeof(code_length_lengths));
    for (index = 0U; index < hclen_count; ++index) {
        code_length_lengths[code_length_order[index]] = (unsigned char)expack_deflate_read_bits(&reader, 3U);
    }
    expack_deflate_build_decode(&code_length_huff, code_length_lengths, 19U, 7U);
    length_count = 0U;
    while (length_count < literal_count + distance_count) {
        unsigned int symbol = expack_deflate_decode_symbol(&reader, &code_length_huff);

        if (reader.failed) {
            return -1;
        }
        if (symbol <= 15U) {
            lengths[length_count++] = (unsigned char)symbol;
        } else if (symbol == 16U) {
            unsigned int repeat;
            unsigned char value;

            if (length_count == 0U) {
                return -1;
            }
            value = lengths[length_count - 1U];
            repeat = expack_deflate_read_bits(&reader, 2U) + 3U;
            while (repeat-- != 0U && length_count < literal_count + distance_count) {
                lengths[length_count++] = value;
            }
        } else if (symbol == 17U) {
            unsigned int repeat = expack_deflate_read_bits(&reader, 3U) + 3U;
            while (repeat-- != 0U && length_count < literal_count + distance_count) {
                lengths[length_count++] = 0U;
            }
        } else if (symbol == 18U) {
            unsigned int repeat = expack_deflate_read_bits(&reader, 7U) + 11U;
            while (repeat-- != 0U && length_count < literal_count + distance_count) {
                lengths[length_count++] = 0U;
            }
        } else {
            return -1;
        }
    }
    expack_deflate_build_decode(&literal_huff, lengths, literal_count, 15U);
    expack_deflate_build_decode(&distance_huff, lengths + literal_count, distance_count, 15U);
    for (;;) {
        unsigned int symbol = expack_deflate_decode_symbol(&reader, &literal_huff);

        if (reader.failed) {
            return -1;
        }
        if (symbol < 256U) {
            if (output_offset >= expected_size || expected[output_offset] != (unsigned char)symbol) {
                return -1;
            }
            output_offset += 1U;
        } else if (symbol == 256U) {
            return output_offset == expected_size ? 0 : -1;
        } else if (symbol <= 285U) {
            unsigned int len_index = symbol - 257U;
            unsigned int length = expack_deflate_len_base[len_index];
            unsigned int distance_symbol;
            unsigned int distance;
            unsigned int copy_index;

            if (expack_deflate_len_extra[len_index] != 0U) {
                length += expack_deflate_read_bits(&reader, expack_deflate_len_extra[len_index]);
            }
            distance_symbol = expack_deflate_decode_symbol(&reader, &distance_huff);
            if (distance_symbol >= 30U) {
                return -1;
            }
            distance = expack_deflate_dist_base[distance_symbol];
            if (expack_deflate_dist_extra[distance_symbol] != 0U) {
                distance += expack_deflate_read_bits(&reader, expack_deflate_dist_extra[distance_symbol]);
            }
            if (distance > output_offset) {
                return -1;
            }
            for (copy_index = 0U; copy_index < length; ++copy_index) {
                unsigned char value;

                if (output_offset >= expected_size) {
                    return -1;
                }
                value = expected[output_offset - distance];
                if (expected[output_offset] != value) {
                    return -1;
                }
                output_offset += 1U;
            }
        } else {
            return -1;
        }
    }
}

static int expack_compress_deflate_bcj(const unsigned char *input_data, size_t input_size, unsigned char **payload_out, size_t *payload_size_out) {
    unsigned char *transformed;
    unsigned char *payload = 0;
    size_t payload_size = 0U;
    int result;

    transformed = (unsigned char *)rt_malloc(input_size == 0U ? 1U : input_size);
    if (transformed == 0) {
        return -1;
    }
    memcpy(transformed, input_data, input_size);
    expack_x86_bcj_transform(transformed, input_size, 0);
    result = expack_deflate_encode_dynamic(transformed, input_size, &payload, &payload_size);
    if (result == 0 && expack_deflate_verify_dynamic(payload, payload_size, transformed, input_size) != 0) {
        rt_free(payload);
        result = -1;
    }
    rt_free(transformed);
    if (result != 0) {
        return -1;
    }
    *payload_out = payload;
    *payload_size_out = payload_size;
    return 0;
}
