#include "gemma_tokenizer.h"

#define NO_TOKEN 0xffffffffU

static unsigned int read_u32(const unsigned char *bytes) {
    return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8) |
        ((unsigned int)bytes[2] << 16) | ((unsigned int)bytes[3] << 24);
}

static unsigned int utf8_next(const unsigned char *text, unsigned int size,
                              unsigned int *codepoint) {
    unsigned int width, value, minimum, index;
    if (!size) return 0;
    if (text[0] < 0x80U) { *codepoint = text[0]; return 1; }
    if (text[0] >= 0xc2U && text[0] <= 0xdfU) {
        width = 2; value = text[0] & 31U; minimum = 0x80U;
    } else if (text[0] >= 0xe0U && text[0] <= 0xefU) {
        width = 3; value = text[0] & 15U; minimum = 0x800U;
    } else if (text[0] >= 0xf0U && text[0] <= 0xf4U) {
        width = 4; value = text[0] & 7U; minimum = 0x10000U;
    } else return 0;
    if (width > size) return 0;
    for (index = 1; index < width; ++index) {
        if ((text[index] & 0xc0U) != 0x80U) return 0;
        value = (value << 6) | (text[index] & 63U);
    }
    if (value < minimum || value > 0x10ffffU ||
        (value >= 0xd800U && value <= 0xdfffU)) return 0;
    *codepoint = value;
    return width;
}

int gemma_utf8_valid(const unsigned char *text, unsigned int size) {
    unsigned int offset = 0, codepoint;
    if (!text && size) return 0;
    while (offset < size) {
        unsigned int width = utf8_next(text + offset, size - offset, &codepoint);
        if (!width) return 0;
        offset += width;
    }
    return 1;
}

static int compare(const unsigned char *left, unsigned int left_size,
                   const unsigned char *right, unsigned int right_size) {
    unsigned int index, size = left_size < right_size ? left_size : right_size;
    for (index = 0; index < size; ++index) {
        if (left[index] != right[index]) return left[index] < right[index] ? -1 : 1;
    }
    return left_size == right_size ? 0 : (left_size < right_size ? -1 : 1);
}

static const unsigned char *piece(const GemmaTokenizer *tokenizer, unsigned int token,
                                  unsigned int *size, unsigned int *flags) {
    const unsigned char *record = tokenizer->pieces + token * 12U;
    *size = read_u32(record + 4);
    *flags = read_u32(record + 8);
    return tokenizer->strings + read_u32(record);
}

static int string_range(const GemmaTokenizer *tokenizer, const unsigned char *record) {
    unsigned int offset = read_u32(record), size = read_u32(record + 4);
    return size && size <= 256U && offset <= tokenizer->string_bytes &&
        size <= tokenizer->string_bytes - offset &&
        gemma_utf8_valid(tokenizer->strings + offset, size);
}

static int sorted_ids(const GemmaTokenizer *tokenizer, const unsigned char *ids,
                      unsigned int count, int added) {
    unsigned int index, previous_size = 0, size, flags;
    const unsigned char *previous = 0;
    for (index = 0; index < count; ++index) {
        unsigned int token = read_u32(ids + index * 4U);
        const unsigned char *text;
        if (token >= (added ? tokenizer->piece_count : tokenizer->lexical_count)) return 0;
        text = piece(tokenizer, token, &size, &flags);
        if ((added && !(flags & 1U)) ||
            (previous && compare(previous, previous_size, text, size) >= 0)) return 0;
        previous = text; previous_size = size;
    }
    return 1;
}

int gemma_tokenizer_open(GemmaTokenizer *tokenizer, const unsigned char *artifact,
                         unsigned long long size) {
    GemmaArtifactHeader header;
    GemmaTokenizer parsed = {0};
    const unsigned char *data;
    unsigned long long expected;
    unsigned int index, added_count = 0, byte_count = 0;
    if (!tokenizer) return 0;
    *tokenizer = parsed;
    if (!artifact || size < GEMMA_ARTIFACT_HEADER_SIZE + 32U ||
        size > GEMMA_TOKENIZER_MAX_ARTIFACT_BYTES) return 0;
    if (!gemma_artifact_decode_header(artifact, &header) ||
        !gemma_artifact_header_valid(&header, gemma_model_translategemma_4b()) ||
        !gemma_artifact_header_matches_name(&header, GEMMA_TOKENIZER_TABLE_NAME) ||
        header.kind != GEMMA_ARTIFACT_KIND_TOKENIZER ||
        header.element_type != GEMMA_ARTIFACT_ELEMENT_U8 ||
        header.layout != GEMMA_ARTIFACT_LAYOUT_OPAQUE || header.rank != 1U ||
        header.payload_size != size - GEMMA_ARTIFACT_HEADER_SIZE ||
        !gemma_artifact_payload_valid(&header, artifact + GEMMA_ARTIFACT_HEADER_SIZE,
                                     header.payload_size)) return 0;
    data = artifact + GEMMA_ARTIFACT_HEADER_SIZE;
    if (read_u32(data) != 0x34544d47U || read_u32(data + 4) != 1U) return 0;
    parsed.piece_count = read_u32(data + 8);
    parsed.lexical_count = read_u32(data + 12);
    parsed.added_count = read_u32(data + 16);
    parsed.merge_count = read_u32(data + 20);
    parsed.language_count = read_u32(data + 24);
    parsed.string_bytes = read_u32(data + 28);
    if (parsed.piece_count != 262145U || parsed.lexical_count != 262144U ||
        parsed.added_count != 6415U || parsed.merge_count != 514906U ||
        parsed.language_count != 581U) return 0;
    expected = 32ULL + parsed.piece_count * 12ULL + parsed.lexical_count * 4ULL +
        parsed.added_count * 4ULL + parsed.merge_count * 16ULL +
        parsed.language_count * 16ULL + parsed.string_bytes;
    if (expected != header.payload_size) return 0;
    parsed.pieces = data + 32;
    parsed.lexical = parsed.pieces + parsed.piece_count * 12U;
    parsed.added = parsed.lexical + parsed.lexical_count * 4U;
    parsed.merges = parsed.added + parsed.added_count * 4U;
    parsed.languages = parsed.merges + parsed.merge_count * 16U;
    parsed.strings = parsed.languages + parsed.language_count * 16U;
    for (index = 0; index < 256; ++index) parsed.byte_ids[index] = NO_TOKEN;
    for (index = 0; index < parsed.piece_count; ++index) {
        const unsigned char *record = parsed.pieces + index * 12U;
        unsigned int flags = read_u32(record + 8);
        if (!string_range(&parsed, record) || (flags & ~0xff07U) ||
            ((flags & 2U) && !(flags & 1U)) || (!(flags & 4U) && (flags >> 8))) return 0;
        if (flags & 1U) ++added_count;
        if (flags & 4U) {
            static const unsigned char hex[] = "0123456789ABCDEF";
            const unsigned char *text = parsed.strings + read_u32(record);
            unsigned int value = flags >> 8;
            if (read_u32(record + 4) != 6U || text[0] != '<' || text[1] != '0' ||
                text[2] != 'x' || text[3] != hex[value >> 4] ||
                text[4] != hex[value & 15U] || text[5] != '>') return 0;
            if (parsed.byte_ids[flags >> 8] != NO_TOKEN) return 0;
            parsed.byte_ids[flags >> 8] = index;
            ++byte_count;
        }
    }
    if (byte_count != 256U || added_count != parsed.added_count ||
        !sorted_ids(&parsed, parsed.lexical, parsed.lexical_count, 0) ||
        !sorted_ids(&parsed, parsed.added, parsed.added_count, 1)) return 0;
    for (index = 0; index < parsed.merge_count; ++index) {
        const unsigned char *record = parsed.merges + index * 16U;
        unsigned int left = read_u32(record), right = read_u32(record + 4);
        unsigned int merged = read_u32(record + 8), rank = read_u32(record + 12);
        unsigned int left_size, right_size, merged_size, flags;
        const unsigned char *left_text, *right_text, *merged_text;
        if (left >= parsed.piece_count || right >= parsed.piece_count ||
            merged >= parsed.piece_count || rank >= parsed.merge_count) return 0;
        if (index && (left < read_u32(record - 16) ||
            (left == read_u32(record - 16) && right <= read_u32(record - 12)))) return 0;
        left_text = piece(&parsed, left, &left_size, &flags);
        right_text = piece(&parsed, right, &right_size, &flags);
        merged_text = piece(&parsed, merged, &merged_size, &flags);
        if (merged_size != left_size + right_size ||
            compare(left_text, left_size, merged_text, left_size) ||
            compare(right_text, right_size, merged_text + left_size, right_size)) return 0;
    }
    for (index = 0; index < parsed.language_count; ++index) {
        const unsigned char *record = parsed.languages + index * 16U;
        if (!string_range(&parsed, record) || !string_range(&parsed, record + 8) ||
            read_u32(record + 4) >= 32U) return 0;
        if (index && compare(parsed.strings + read_u32(record - 16), read_u32(record - 12),
                             parsed.strings + read_u32(record), read_u32(record + 4)) >= 0) return 0;
    }
    *tokenizer = parsed;
    return 1;
}

static unsigned int lookup(const GemmaTokenizer *tokenizer, const unsigned char *ids,
    unsigned int count, const unsigned char *text, unsigned int size, int *prefix) {
    unsigned int low = 0, high = count, length, flags, token;
    const unsigned char *candidate;
    *prefix = 0;
    while (low < high) {
        unsigned int middle = low + (high - low) / 2U;
        token = read_u32(ids + middle * 4U);
        candidate = piece(tokenizer, token, &length, &flags);
        if (compare(candidate, length, text, size) < 0) low = middle + 1U;
        else high = middle;
    }
    if (low == count) return NO_TOKEN;
    token = read_u32(ids + low * 4U);
    candidate = piece(tokenizer, token, &length, &flags);
    if (length >= size && !compare(candidate, size, text, size)) {
        *prefix = 1;
        if (length == size) return token;
    }
    return NO_TOKEN;
}

static unsigned int added_at(const GemmaTokenizer *tokenizer, const unsigned char *text,
                             unsigned int size, unsigned int *matched) {
    unsigned int width, result = NO_TOKEN;
    *matched = 0;
    for (width = 1; width <= size && width <= 256U; ++width) {
        int prefix;
        unsigned int token = lookup(tokenizer, tokenizer->added, tokenizer->added_count,
                                     text, width, &prefix);
        if (token != NO_TOKEN) { result = token; *matched = width; }
        if (!prefix) break;
    }
    return result;
}

static int heap_before(const GemmaTokenizerWork *work, unsigned int left, unsigned int right) {
    return work->nodes[left].rank < work->nodes[right].rank ||
        (work->nodes[left].rank == work->nodes[right].rank && left < right);
}

static void heap_swap(GemmaTokenizerWork *work, unsigned int left, unsigned int right) {
    unsigned int saved = work->heap[left];
    work->heap[left] = work->heap[right]; work->heap[right] = saved;
    work->nodes[work->heap[left]].heap_position = left;
    work->nodes[work->heap[right]].heap_position = right;
}

static void heap_fix(GemmaTokenizerWork *work, unsigned int position) {
    while (position && heap_before(work, work->heap[position], work->heap[(position - 1U) / 2U])) {
        unsigned int parent = (position - 1U) / 2U;
        heap_swap(work, position, parent); position = parent;
    }
    for (;;) {
        unsigned int child = position * 2U + 1U;
        if (child >= work->heap_size) break;
        if (child + 1U < work->heap_size && heap_before(work, work->heap[child + 1U], work->heap[child])) ++child;
        if (!heap_before(work, work->heap[child], work->heap[position])) break;
        heap_swap(work, position, child); position = child;
    }
}

static void heap_remove(GemmaTokenizerWork *work, unsigned int node) {
    unsigned int position = work->nodes[node].heap_position;
    if (position == NO_TOKEN) return;
    --work->heap_size;
    work->nodes[node].heap_position = NO_TOKEN;
    if (position < work->heap_size) {
        work->heap[position] = work->heap[work->heap_size];
        work->nodes[work->heap[position]].heap_position = position;
        heap_fix(work, position);
    }
}

static void update_pair(const GemmaTokenizer *tokenizer, GemmaTokenizerWork *work,
                        unsigned int node) {
    unsigned int low = 0, high = tokenizer->merge_count, left, right;
    GemmaBpeNode *current;
    if (node == NO_TOKEN) return;
    heap_remove(work, node);
    current = work->nodes + node;
    if (current->next == NO_TOKEN) return;
    left = current->token; right = work->nodes[current->next].token;
    while (low < high) {
        unsigned int middle = low + (high - low) / 2U;
        const unsigned char *record = tokenizer->merges + middle * 16U;
        unsigned int candidate_left = read_u32(record), candidate_right = read_u32(record + 4);
        if (candidate_left < left || (candidate_left == left && candidate_right < right)) low = middle + 1U;
        else high = middle;
    }
    if (low < tokenizer->merge_count) {
        const unsigned char *record = tokenizer->merges + low * 16U;
        if (read_u32(record) == left && read_u32(record + 4) == right) {
            current->merged = read_u32(record + 8); current->rank = read_u32(record + 12);
            current->heap_position = work->heap_size;
            work->heap[work->heap_size++] = node;
            heap_fix(work, current->heap_position);
        }
    }
}

static int encode_segment(const GemmaTokenizer *tokenizer, GemmaTokenizerWork *work,
    const unsigned char *text, unsigned int size, unsigned int *tokens,
    unsigned int capacity, unsigned int *count) {
    static const unsigned char space[] = {0xe2, 0x96, 0x81};
    unsigned int offset = 0, nodes = 0, index;
    work->heap_size = 0;
    while (offset < size) {
        unsigned int codepoint, width = utf8_next(text + offset, size - offset, &codepoint);
        const unsigned char *bytes = codepoint == 32U ? space : text + offset;
        unsigned int byte_count = codepoint == 32U ? 3U : width;
        unsigned int token;
        int prefix;
        token = lookup(tokenizer, tokenizer->lexical, tokenizer->lexical_count, bytes, byte_count, &prefix);
        for (index = 0; index < (token == NO_TOKEN ? byte_count : 1U); ++index) {
            GemmaBpeNode *node;
            if (nodes == GEMMA_TOKENIZER_MAX_BYTES) return 0;
            node = work->nodes + nodes;
            node->token = token == NO_TOKEN ? tokenizer->byte_ids[bytes[index]] : token;
            node->previous = nodes ? nodes - 1U : NO_TOKEN;
            node->next = nodes + 1U; node->heap_position = NO_TOKEN;
            ++nodes;
        }
        offset += width;
    }
    if (!nodes) return 1;
    work->nodes[nodes - 1U].next = NO_TOKEN;
    for (index = 0; index < nodes; ++index) update_pair(tokenizer, work, index);
    while (work->heap_size) {
        unsigned int left = work->heap[0], right = work->nodes[left].next;
        GemmaBpeNode *node = work->nodes + left;
        unsigned int merged = node->merged;
        heap_remove(work, left); heap_remove(work, right);
        node->token = merged; node->next = work->nodes[right].next;
        if (node->next != NO_TOKEN) work->nodes[node->next].previous = left;
        update_pair(tokenizer, work, node->previous); update_pair(tokenizer, work, left);
    }
    for (index = 0; index != NO_TOKEN; index = work->nodes[index].next) {
        if (*count == capacity) return 0;
        tokens[(*count)++] = work->nodes[index].token;
    }
    return 1;
}

int gemma_tokenizer_encode(const GemmaTokenizer *tokenizer, GemmaTokenizerWork *work,
    const unsigned char *text, unsigned int size, int add_bos,
    unsigned int *tokens, unsigned int capacity, unsigned int *count) {
    unsigned int offset = 0, start = 0, used = 0;
    if (!count) return 0;
    *count = 0;
    if (!tokenizer || !tokenizer->strings || !work || !tokens || !capacity ||
        size > GEMMA_TOKENIZER_MAX_BYTES || !gemma_utf8_valid(text, size)) return 0;
    if (capacity > GEMMA_TRANSLATEGEMMA_4B_DEPLOYMENT_CONTEXT) capacity = GEMMA_TRANSLATEGEMMA_4B_DEPLOYMENT_CONTEXT;
    if (add_bos) tokens[used++] = GEMMA_TRANSLATEGEMMA_4B_BOS_TOKEN_ID;
    while (offset < size) {
        unsigned int matched, token = added_at(tokenizer, text + offset, size - offset, &matched);
        if (token != NO_TOKEN) {
            if (!encode_segment(tokenizer, work, text + start, offset - start, tokens, capacity, &used) ||
                used == capacity) return 0;
            tokens[used++] = token; offset += matched; start = offset;
        } else ++offset;
    }
    if (!encode_segment(tokenizer, work, text ? text + start : text, size - start, tokens, capacity, &used)) return 0;
    *count = used;
    return 1;
}

static int append(unsigned char *output, unsigned int capacity, unsigned int *used,
                   const unsigned char *bytes, unsigned int size) {
    unsigned int index;
    if (size > capacity - *used) return 0;
    for (index = 0; index < size; ++index) output[(*used)++] = bytes[index];
    return 1;
}

static int finish_bytes(unsigned char *output, unsigned int capacity, unsigned int *used,
                        unsigned int start) {
    static const unsigned char replacement[] = {0xef, 0xbf, 0xbd};
    unsigned int count = *used - start, index;
    if (gemma_utf8_valid(output + start, count)) return 1;
    *used = start;
    for (index = 0; index < count; ++index) {
        if (!append(output, capacity, used, replacement, 3)) return 0;
    }
    return 1;
}

static int decode_prefix(const GemmaTokenizer *tokenizer,
    const unsigned int *tokens, unsigned int count, int skip_special,
    unsigned char *text, unsigned int capacity, unsigned int *size, int final) {
    unsigned int index, used = 0, byte_start = NO_TOKEN;
    if (!size) return 0;
    *size = 0;
    if (!tokenizer || !tokenizer->strings || (!tokens && count) || !text ||
        count > GEMMA_TRANSLATEGEMMA_4B_DEPLOYMENT_CONTEXT) return 0;
    for (index = 0; index < count; ++index) {
        unsigned int length, flags, offset;
        const unsigned char *bytes;
        if (tokens[index] >= tokenizer->piece_count) return 0;
        bytes = piece(tokenizer, tokens[index], &length, &flags);
        if (skip_special && (flags & 2U)) continue;
        if (flags & 4U) {
            unsigned char value = (unsigned char)(flags >> 8);
            if (byte_start == NO_TOKEN) byte_start = used;
            if (!append(text, capacity, &used, &value, 1)) return 0;
            continue;
        }
        if (byte_start != NO_TOKEN && !finish_bytes(text, capacity, &used, byte_start)) return 0;
        byte_start = NO_TOKEN;
        for (offset = 0; offset < length;) {
            if (length - offset >= 3U && bytes[offset] == 0xe2U &&
                bytes[offset + 1U] == 0x96U && bytes[offset + 2U] == 0x81U) {
                static const unsigned char space = ' ';
                if (!append(text, capacity, &used, &space, 1)) return 0;
                offset += 3;
            } else if (!append(text, capacity, &used, bytes + offset++, 1)) return 0;
        }
    }
    if (byte_start != NO_TOKEN) {
        if (!final) used = byte_start;
        else if (!finish_bytes(text, capacity, &used, byte_start)) return 0;
    }
    *size = used;
    return 1;
}

int gemma_tokenizer_decode(const GemmaTokenizer *tokenizer,
    const unsigned int *tokens, unsigned int count, int skip_special,
    unsigned char *text, unsigned int capacity, unsigned int *size) {
    return decode_prefix(tokenizer, tokens, count, skip_special, text, capacity, size, 1);
}

int gemma_tokenizer_decode_stable(const GemmaTokenizer *tokenizer,
    const unsigned int *tokens, unsigned int count, int skip_special,
    unsigned char *text, unsigned int capacity, unsigned int *size) {
    return decode_prefix(tokenizer, tokens, count, skip_special, text, capacity, size, 0);
}

static unsigned int text_length(const char *text) {
    unsigned int size = 0;
    while (text[size]) ++size;
    return size;
}

static const unsigned char *language(const GemmaTokenizer *tokenizer, const char *code,
                                     unsigned char canonical[32], unsigned int *size) {
    unsigned int low = 0, high = tokenizer->language_count;
    *size = 0;
    if (!code) return 0;
    while (code[*size]) {
        if (*size == 31U) return 0;
        canonical[*size] = code[*size] == '_' ? '-' : (unsigned char)code[*size];
        ++*size;
    }
    while (low < high) {
        unsigned int middle = low + (high - low) / 2U;
        const unsigned char *record = tokenizer->languages + middle * 16U;
        int order = compare(tokenizer->strings + read_u32(record), read_u32(record + 4), canonical, *size);
        if (!order) return record;
        if (order < 0) low = middle + 1U; else high = middle;
    }
    return 0;
}

static int whitespace(unsigned int codepoint) {
    return (codepoint >= 9U && codepoint <= 13U) ||
        (codepoint >= 28U && codepoint <= 32U) || codepoint == 0x85U ||
        codepoint == 0xa0U || codepoint == 0x1680U ||
        (codepoint >= 0x2000U && codepoint <= 0x200aU) || codepoint == 0x2028U ||
        codepoint == 0x2029U || codepoint == 0x202fU || codepoint == 0x205fU || codepoint == 0x3000U;
}

int gemma_tokenizer_prompt(const GemmaTokenizer *tokenizer, GemmaTokenizerWork *work,
    const char *source_language, const char *target_language,
    const unsigned char *text, unsigned int size, unsigned int maximum_new_tokens,
    unsigned int *tokens, unsigned int capacity, unsigned int *count) {
    unsigned char source_code[32], target_code[32];
    unsigned int source_size, target_size, used = 0, offset = 0, first = size, last = 0;
    const unsigned char *source, *target;
    const unsigned char *parts[25];
    unsigned int lengths[25], index, part_count = 0;
    if (!count) return 0;
    *count = 0;
    if (!tokenizer || !tokenizer->strings || !work || !text ||
        size > GEMMA_TOKENIZER_MAX_BYTES || !gemma_utf8_valid(text, size) ||
        !maximum_new_tokens || maximum_new_tokens >= GEMMA_TRANSLATEGEMMA_4B_DEPLOYMENT_CONTEXT) return 0;
    source = language(tokenizer, source_language, source_code, &source_size);
    target = language(tokenizer, target_language, target_code, &target_size);
    if (!source || !target) return 0;
    while (offset < size) {
        unsigned int codepoint, width = utf8_next(text + offset, size - offset, &codepoint);
        if (!whitespace(codepoint)) { if (first == size) first = offset; last = offset + width; }
        offset += width;
    }
    if (first == size) first = last = 0;
#define LITERAL(value) do { parts[part_count] = (const unsigned char *)(value); lengths[part_count++] = text_length(value); } while (0)
#define NAME(record) do { parts[part_count] = tokenizer->strings + read_u32((record) + 8); lengths[part_count++] = read_u32((record) + 12); } while (0)
#define CODE(value, length) do { parts[part_count] = (value); lengths[part_count++] = (length); } while (0)
    LITERAL("<bos><start_of_turn>user\nYou are a professional ");
    NAME(source); LITERAL(" ("); CODE(source_code, source_size); LITERAL(") to ");
    NAME(target); LITERAL(" ("); CODE(target_code, target_size);
    LITERAL(") translator. Your goal is to accurately convey the meaning and nuances of the original ");
    NAME(source); LITERAL(" text while adhering to "); NAME(target);
    LITERAL(" grammar, vocabulary, and cultural sensitivities.\nProduce only the ");
    NAME(target); LITERAL(" translation, without any additional explanations or commentary. Please translate the following ");
    NAME(source); LITERAL(" text into "); NAME(target); LITERAL(":\n\n\n");
    CODE(text + first, last - first); LITERAL("<end_of_turn>\n<start_of_turn>model\n");
#undef LITERAL
#undef NAME
#undef CODE
    for (index = 0; index < part_count; ++index) {
        if (!append(work->prompt, GEMMA_TOKENIZER_MAX_BYTES, &used, parts[index], lengths[index])) return 0;
    }
    if (capacity > GEMMA_TRANSLATEGEMMA_4B_DEPLOYMENT_CONTEXT - maximum_new_tokens)
        capacity = GEMMA_TRANSLATEGEMMA_4B_DEPLOYMENT_CONTEXT - maximum_new_tokens;
    return gemma_tokenizer_encode(tokenizer, work, work->prompt, used, 0, tokens, capacity, count);
}