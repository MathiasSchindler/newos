#include "nemotron_tokenizer.h"
#include "runtime.h"

static uint32_t word(const unsigned char *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int token_error(NmTokenizer *tokenizer, const char *error) {
    if (tokenizer) tokenizer->error = error;
    return 0;
}
int nm_tokenizer_open(NmTokenizer *tokenizer, const void *data, uint64_t bytes) {
    const unsigned char *p = data;
    static const unsigned char source_sha[32] = {
        0x3f,0x3d,0x48,0x1d,0xeb,0x07,0x3b,0x64,0xc2,0x08,0x2e,0x8c,0x78,0x60,0xd4,0x87,
        0xa3,0xa6,0x27,0x74,0xbf,0x4e,0x9e,0x4f,0xaa,0xc8,0x30,0x07,0xe1,0x81,0xf2,0x46
    };
    uint32_t index, strings, cursor = 0;
    const unsigned char *entries, *text;
    if (!tokenizer) return 0;
    memset(tokenizer, 0, sizeof(*tokenizer));
    if (!p || bytes < 64 || bytes > 4194304 || memcmp(p, "NMTOK001", 8) ||
        word(p + 8) != 1 || word(p + 12) != 13089 || word(p + 16) != 13087 ||
        word(p + 20) != 13087 || word(p + 28) || memcmp(p + 32, source_sha, 32))
        return token_error(tokenizer, "invalid pinned tokenizer table header");
    strings = word(p + 24);
    if (bytes != 64 + (uint64_t)13089 * 12 + strings)
        return token_error(tokenizer, "tokenizer table size mismatch");
    entries = p + 64; text = p + 64 + 13089 * 12;
    for (index = 0; index < 13089; ++index) {
        const unsigned char *entry = entries + index * 12;
        uint32_t offset = word(entry), size = word(entry + 4), flags = word(entry + 8);
        if (offset != cursor || size > 256 || size > strings - cursor || flags > 1 ||
            !size || (index >= 13087 && flags != 1) ||
            rt_utf8_validate((const char *)text + offset, size) != 0)
            return token_error(tokenizer, "invalid tokenizer piece bounds/UTF8/flags");
        cursor += size;
    }
    if (cursor != strings) return token_error(tokenizer, "unreferenced tokenizer strings");
    tokenizer->entries = entries; tokenizer->strings = text;
    tokenizer->count = 13089; tokenizer->string_bytes = strings;
    return 1;
}
int nm_tokenizer_decode(NmTokenizer *tokenizer, const uint32_t *tokens, size_t count,
                        int skip_special, char *output, size_t capacity, size_t *bytes) {
    size_t used = 0, index; int first = 1;
    if (!tokenizer || !tokenizer->entries || (!tokens && count) || !output || !capacity || !bytes)
        return token_error(tokenizer, "invalid tokenizer decode arguments");
    *bytes = 0; output[0] = 0;
    for (index = 0; index < count; ++index) {
        const unsigned char *entry, *piece;
        uint32_t position = 0, size;
        if (tokens[index] >= tokenizer->count) return token_error(tokenizer, "token id outside vocabulary");
        entry = tokenizer->entries + tokens[index] * 12;
        if (skip_special && word(entry + 8)) continue;
        piece = tokenizer->strings + word(entry); size = word(entry + 4);
        if (!size) continue;
        while (position < size) {
            if (size - position >= 3 && piece[position] == 0xe2 &&
                piece[position + 1] == 0x96 && piece[position + 2] == 0x81) {
                if (!(first && !position)) {
                    if (used == capacity - 1) return token_error(tokenizer, "decoded text exceeds bound");
                    output[used++] = ' ';
                }
                position += 3;
            } else {
                if (used == capacity - 1) return token_error(tokenizer, "decoded text exceeds bound");
                output[used++] = (char)piece[position++];
            }
        }
        first = 0;
    }
    output[used] = 0; *bytes = used;
    if (rt_utf8_validate(output, used) != 0) return token_error(tokenizer, "decoded text is not UTF8");
    tokenizer->error = 0; return 1;
}
