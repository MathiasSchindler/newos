#include "ocr_tokenizer.h"
#include "crypto/sha256.h"

#define MISSING 0xffffffffU

static unsigned int load32(const unsigned char *data) {
    return data[0] | (unsigned int)data[1] << 8 | (unsigned int)data[2] << 16 | (unsigned int)data[3] << 24;
}

static int compare(const unsigned char *left, unsigned int left_size, const unsigned char *right, unsigned int right_size) {
    unsigned int size = left_size < right_size ? left_size : right_size;
    for (unsigned int index = 0; index < size; ++index)
        if (left[index] != right[index]) return left[index] < right[index] ? -1 : 1;
    return left_size == right_size ? 0 : left_size < right_size ? -1 : 1;
}

int ocr_artifact(const unsigned char *data, unsigned int size, unsigned int kind) {
    static const char identity[] = "2e85a62840ccac27daa451df36c736c4636b8628"
        "9f4a549a14a96217569648aa7627c6674ad94fe9" "04b9992511183247d5115f176a5b9f0360a36a1c";
    static const char image_identity[] = "2e85a62840ccac27daa451df36c736c4636b8628"
        "308553695af766b3e3d05e68279d2c690e73273e" "bdc21c05f82f7a5f3a4bd4caa74f4bf81365fdd7";
    const char *source_identity = kind >= 3 && kind <= 19 ? image_identity : identity;
    static const char hex[] = "0123456789abcdef";
    unsigned char hash[32];
    CryptoSha256Context context;
    if (!data || size < 128 || size > (kind == 3 ? 256U : kind == 13 ? 192U : kind == 9 ? 144U : kind == 8 ? 80U : 64U) * 1024U * 1024U ||
        compare(data, 8, (const unsigned char *)"GLMOCR2\0", 8) ||
        load32(data + 8) != 1 || load32(data + 12) != kind || load32(data + 16) != size - 128) return 0;
    for (unsigned int index = 20; index < 32; ++index) if (data[index]) return 0;
    for (unsigned int index = 92; index < 96; ++index) if (data[index]) return 0;
    for (unsigned int index = 0; index < 60; ++index)
        if (hex[data[32 + index] >> 4] != source_identity[index * 2] || hex[data[32 + index] & 15] != source_identity[index * 2 + 1]) return 0;
    crypto_sha256_init(&context);
    crypto_sha256_update(&context, data, 96);
    crypto_sha256_update(&context, data + 128, size - 128);
    crypto_sha256_final(&context, hash);
    return !compare(hash, 32, data + 96, 32);
}

static const unsigned char *piece(const OcrTokenizer *tokenizer, unsigned int token, unsigned int *size) {
    const unsigned char *record = tokenizer->pieces + token * 12;
    *size = load32(record + 4);
    return tokenizer->strings + load32(record);
}

int ocr_tokenizer_open(OcrTokenizer *tokenizer, const unsigned char *data, unsigned int size) {
    OcrTokenizer parsed = {0};
    unsigned int strings, offset = 0, previous = 0;
    if (!tokenizer) return 0;
    *tokenizer = parsed;
    if (!ocr_artifact(data, size, 1) || size < 160) return 0;
    data += 128;
    strings = load32(data + 12);
    parsed.range_count = load32(data + 8);
    if (load32(data) != 59282 || load32(data + 4) != 106026 || load32(data + 16) != 59246 ||
        load32(data + 20) != 36 || load32(data + 24) || load32(data + 28) ||
        parsed.range_count > 4096 || strings > 8U * 1024U * 1024U ||
        size != 160U + 59282U * 12U + 59246U * 4U + 106026U * 16U + parsed.range_count * 12U + 1024U + strings) return 0;
    parsed.pieces = data + 32;
    parsed.lexical = parsed.pieces + 59282U * 12U;
    parsed.merges = parsed.lexical + 59246U * 4U;
    parsed.ranges = parsed.merges + 106026U * 16U;
    parsed.bytes = parsed.ranges + parsed.range_count * 12U;
    parsed.strings = parsed.bytes + 1024;
    for (unsigned int token = 0; token < 59282; ++token) {
        const unsigned char *record = parsed.pieces + token * 12;
        unsigned int length = load32(record + 4), flags = load32(record + 8);
        if (load32(record) != offset || !length || length > 96 || length > strings - offset ||
            flags > 3 || (token < 59246 ? flags != 0 : !(flags & 1))) return 0;
        offset += length;
    }
    if (offset != strings) return 0;
    for (unsigned int index = 0; index < 59246; ++index) {
        unsigned int token = load32(parsed.lexical + index * 4), length, prior_length;
        if (token >= 59246) return 0;
        const unsigned char *text = piece(&parsed, token, &length);
        if (index) {
            const unsigned char *prior = piece(&parsed, previous, &prior_length);
            if (compare(prior, prior_length, text, length) >= 0) return 0;
        }
        previous = token;
    }
    for (unsigned int index = 0; index < 106026; ++index) {
        const unsigned char *record = parsed.merges + index * 16;
        unsigned int left = load32(record), right = load32(record + 4), merged = load32(record + 12);
        unsigned int left_size, right_size, merged_size;
        if (left >= 59246 || right >= 59246 || merged >= 59246 || load32(record + 8) >= 106026) return 0;
        if (index && (left < load32(record - 16) || (left == load32(record - 16) && right <= load32(record - 12)))) return 0;
        const unsigned char *left_text = piece(&parsed, left, &left_size);
        const unsigned char *right_text = piece(&parsed, right, &right_size);
        const unsigned char *merged_text = piece(&parsed, merged, &merged_size);
        if (merged_size != left_size + right_size || compare(left_text, left_size, merged_text, left_size) ||
            compare(right_text, right_size, merged_text + left_size, right_size)) return 0;
    }
    for (unsigned int index = 0; index < parsed.range_count; ++index) {
        const unsigned char *record = parsed.ranges + index * 12;
        unsigned int start = load32(record), end = load32(record + 4), flags = load32(record + 8);
        if (start > end || end > 0x10ffff || !flags || flags > 7 || (index && start <= previous)) return 0;
        previous = end;
    }
    for (unsigned int value = 0; value < 256; ++value) {
        unsigned int token = load32(parsed.bytes + value * 4), length;
        if (token >= 59246) return 0;
        const unsigned char *text = piece(&parsed, token, &length);
        if (length != 1 || text[0] != value) return 0;
    }
    *tokenizer = parsed;
    return 1;
}

static unsigned int utf8(const unsigned char *text, unsigned int size, unsigned int *value) {
    unsigned int width, code;
    if (!size) return 0;
    if (text[0] < 128) { *value = text[0]; return 1; }
    if (text[0] >= 0xc2 && text[0] <= 0xdf) { width = 2; code = text[0] & 31; }
    else if (text[0] >= 0xe0 && text[0] <= 0xef) { width = 3; code = text[0] & 15; }
    else if (text[0] >= 0xf0 && text[0] <= 0xf4) { width = 4; code = text[0] & 7; }
    else return 0;
    if (width > size) return 0;
    for (unsigned int index = 1; index < width; ++index) {
        if ((text[index] & 0xc0) != 0x80) return 0;
        code = code * 64 + (text[index] & 63);
    }
    if ((width == 3 && code < 0x800) || (width == 4 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return 0;
    *value = code;
    return width;
}

static unsigned int category(const OcrTokenizer *tokenizer, unsigned int code) {
    unsigned int lower = 0, upper = tokenizer->range_count;
    while (lower < upper) {
        unsigned int middle = lower + (upper - lower) / 2;
        const unsigned char *record = tokenizer->ranges + middle * 12;
        if (code < load32(record)) upper = middle;
        else if (code > load32(record + 4)) lower = middle + 1;
        else return load32(record + 8);
    }
    return 0;
}

static unsigned int lookup(const OcrTokenizer *tokenizer, const unsigned char *text, unsigned int size) {
    unsigned int lower = 0, upper = 59246;
    while (lower < upper) {
        unsigned int middle = lower + (upper - lower) / 2, length;
        unsigned int token = load32(tokenizer->lexical + middle * 4);
        const unsigned char *candidate = piece(tokenizer, token, &length);
        int order = compare(text, size, candidate, length);
        if (!order) return token;
        if (order < 0) upper = middle;
        else lower = middle + 1;
    }
    return MISSING;
}

static const unsigned char *merge_pair(const OcrTokenizer *tokenizer, unsigned int left, unsigned int right) {
    unsigned int lower = 0, upper = 106026;
    while (lower < upper) {
        unsigned int middle = lower + (upper - lower) / 2;
        const unsigned char *record = tokenizer->merges + middle * 16;
        unsigned int first = load32(record), second = load32(record + 4);
        if (first == left && second == right) return record;
        if (left < first || (left == first && right < second)) upper = middle;
        else lower = middle + 1;
    }
    return 0;
}

static int bpe(const OcrTokenizer *tokenizer, const unsigned char *text, unsigned int size, unsigned int *output, unsigned int capacity) {
    unsigned int tokens[OCR_TOKENIZER_LIMIT], count = size;
    unsigned int direct = lookup(tokenizer, text, size);
    if (direct != MISSING) { if (!capacity) return -1; output[0] = direct; return 1; }
    for (unsigned int index = 0; index < size; ++index) tokens[index] = load32(tokenizer->bytes + text[index] * 4);
    for (;;) {
        unsigned int rank = MISSING, position = 0, merged = 0;
        for (unsigned int index = 0; index + 1 < count; ++index) {
            const unsigned char *record = merge_pair(tokenizer, tokens[index], tokens[index + 1]);
            if (record && load32(record + 8) < rank) { rank = load32(record + 8); position = index; merged = load32(record + 12); }
        }
        if (rank == MISSING) break;
        tokens[position] = merged;
        --count;
        for (unsigned int index = position + 1; index < count; ++index) tokens[index] = tokens[index + 1];
    }
    if (count > capacity) return -1;
    for (unsigned int index = 0; index < count; ++index) output[index] = tokens[index];
    return (int)count;
}

static unsigned int split(const unsigned int *codes, const unsigned int *flags, unsigned int count) {
    unsigned int end = 0;
    if (codes[0] == '\'' && count > 1) {
        unsigned int next = codes[1];
        if (next >= 'A' && next <= 'Z') next += 32;
        if (next == 0x17f) next = 's';
        if (next == 's' || next == 't' || next == 'm' || next == 'd') return 2;
        if (count > 2) {
            unsigned int third = codes[2];
            if (third >= 'A' && third <= 'Z') third += 32;
            if ((next == 'r' && third == 'e') || (next == 'v' && third == 'e') || (next == 'l' && third == 'l')) return 3;
        }
    }
    if (!(flags[0] & 3) && codes[0] != '\r' && codes[0] != '\n' && count > 1 && (flags[1] & 1)) end = 1;
    if (flags[end] & 1) { do { ++end; } while (end < count && (flags[end] & 1)); return end; }
    if (flags[0] & 2) { do { ++end; } while (end < count && end < 3 && (flags[end] & 2)); return end; }
    end = codes[0] == ' ' && count > 1 ? 1 : 0;
    if (!(flags[end] & 7)) {
        do { ++end; } while (end < count && !(flags[end] & 7));
        while (end < count && (codes[end] == '\r' || codes[end] == '\n')) ++end;
        return end;
    }
    end = 0;
    while (end < count && (flags[end] & 4)) ++end;
    if (end) {
        for (unsigned int index = end; index; --index)
            if (codes[index - 1] == '\r' || codes[index - 1] == '\n') return index;
        if (end == count) return end;
        return end > 1 ? end - 1 : end;
    }
    return 1;
}

int ocr_encode(const OcrTokenizer *tokenizer, const unsigned char *text, unsigned int size, unsigned int *ids, unsigned int capacity) {
    unsigned int codes[OCR_TOKENIZER_LIMIT], flags[OCR_TOKENIZER_LIMIT], offsets[OCR_TOKENIZER_LIMIT + 1];
    unsigned int cursor = 0, total = 0;
    if (!tokenizer || !tokenizer->pieces || (!text && size) || !ids || size > OCR_TOKENIZER_LIMIT) return -1;
    while (cursor < size) {
        unsigned int span = cursor, special = MISSING, special_size = 0;
        while (span < size) {
            for (unsigned int token = 59246; token < 59282; ++token) {
                unsigned int length;
                const unsigned char *candidate = piece(tokenizer, token, &length);
                if (length <= size - span && length > special_size && !compare(text + span, length, candidate, length)) {
                    special = token; special_size = length;
                }
            }
            if (special != MISSING) break;
            ++span;
        }
        unsigned int count = 0, position = cursor;
        while (position < span) {
            unsigned int width = utf8(text + position, span - position, &codes[count]);
            if (!width) return -1;
            offsets[count] = position;
            flags[count] = category(tokenizer, codes[count]);
            ++count; position += width;
        }
        offsets[count] = span;
        for (unsigned int index = 0; index < count;) {
            unsigned int width = split(codes + index, flags + index, count - index);
            int added = bpe(tokenizer, text + offsets[index], offsets[index + width] - offsets[index], ids + total, capacity - total);
            if (added < 0) return -1;
            total += (unsigned int)added; index += width;
        }
        cursor = span;
        if (special != MISSING) {
            if (total == capacity) return -1;
            ids[total++] = special; cursor += special_size;
        }
    }
    return (int)total;
}

int ocr_decode_prefix(const OcrTokenizer *tokenizer, const unsigned int *ids, unsigned int count, int skip_special, unsigned char *output, unsigned int capacity, int final) {
    unsigned char raw[OCR_TOKENIZER_LIMIT * 4];
    unsigned int length = 0, total = 0, cursor = 0;
    if (!tokenizer || !tokenizer->pieces || !ids || !output || count > OCR_TOKENIZER_LIMIT) return -1;
    for (unsigned int index = 0; index < count; ++index) {
        unsigned int size, token = ids[index];
        if (token >= 59282) return -1;
        if (skip_special && (load32(tokenizer->pieces + token * 12 + 8) & 2)) continue;
        const unsigned char *text = piece(tokenizer, token, &size);
        if (size > sizeof(raw) - length) return -1;
        for (unsigned int offset = 0; offset < size; ++offset) raw[length++] = text[offset];
    }
    while (cursor < length) {
        unsigned int code, width = utf8(raw + cursor, length - cursor, &code);
        if (width) {
            if (width > capacity - total) return -1;
            for (unsigned int index = 0; index < width; ++index) output[total++] = raw[cursor++];
        } else {
            unsigned int lead = raw[cursor], expected = lead >= 0xc2 && lead <= 0xdf ? 2 : lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 1;
            unsigned int consumed = 1;
            while (consumed < expected && cursor + consumed < length) {
                unsigned int next = raw[cursor + consumed];
                if ((next & 0xc0) != 0x80 || (consumed == 1 && ((lead == 0xe0 && next < 0xa0) ||
                    (lead == 0xed && next >= 0xa0) || (lead == 0xf0 && next < 0x90) || (lead == 0xf4 && next >= 0x90)))) break;
                ++consumed;
            }
            if (!final && consumed < expected && cursor+consumed == length) break;
            if (capacity - total < 3) return -1;
            output[total++] = 0xef; output[total++] = 0xbf; output[total++] = 0xbd;
            cursor += consumed;
        }
    }
    return (int)total;
}

int ocr_decode(const OcrTokenizer *tokenizer, const unsigned int *ids, unsigned int count, int skip_special, unsigned char *output, unsigned int capacity) {
    return ocr_decode_prefix(tokenizer,ids,count,skip_special,output,capacity,1);
}

int ocr_prompt(unsigned int task, unsigned int images, unsigned int no_think, unsigned int *ids, unsigned int capacity) {
    static const unsigned int prefix[] = {59248, 59250, 59253, 10, 59256};
    unsigned int count = 0;
    if (!ids || task > 2 || !images || images > OCR_TOKENIZER_LIMIT - 18 || no_think > 1 ||
        capacity < images + 12 + (task == 1) + no_think * 4) return -1;
    for (unsigned int index = 0; index < 5; ++index) ids[count++] = prefix[index];
    for (unsigned int index = 0; index < images; ++index) ids[count++] = 59280;
    ids[count++] = 59257;
    ids[count++] = task == 0 ? 3649 : task == 1 ? 4000 : 5964;
    if (task == 1) ids[count++] = 6062;
    ids[count++] = 7404; ids[count++] = 49600; ids[count++] = 58;
    if (no_think) ids[count++] = 59277;
    ids[count++] = 59254; ids[count++] = 10;
    if (no_think) { ids[count++] = 59267; ids[count++] = 59268; ids[count++] = 10; }
    return (int)count;
}