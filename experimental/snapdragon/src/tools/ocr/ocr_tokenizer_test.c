#include "ocr_tokenizer.h"
#include "crypto/sha256.h"

__declspec(dllimport) void *CreateFileW(const unsigned short *, unsigned int, unsigned int, void *, unsigned int, unsigned int, void *);
__declspec(dllimport) int GetFileSizeEx(void *, long long *);
__declspec(dllimport) int ReadFile(void *, void *, unsigned int, unsigned int *, void *);
__declspec(dllimport) int CloseHandle(void *);
__declspec(dllimport) void *GetStdHandle(unsigned int);
__declspec(dllimport) int WriteFile(void *, const void *, unsigned int, unsigned int *, void *);

static unsigned char table[8U * 1024U * 1024U], fixtures[64U * 1024U * 1024U];
static unsigned int actual[OCR_TOKENIZER_LIMIT], expected[OCR_TOKENIZER_LIMIT];
static unsigned char decoded[OCR_TOKENIZER_LIMIT * 4];

static unsigned int number(const unsigned char *data) {
    return data[0] | (unsigned int)data[1] << 8 | (unsigned int)data[2] << 16 | (unsigned int)data[3] << 24;
}

static void report(const char *label, unsigned int value) {
    char text[128], digits[16];
    unsigned int length = 0, count = 0, written;
    while (label[length]) { text[length] = label[length]; ++length; }
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) text[length++] = digits[--count];
    text[length++] = '\n';
    WriteFile(GetStdHandle((unsigned int)-11), text, length, &written, 0);
}

static unsigned int read_all(const unsigned short *path, unsigned char *output, unsigned int capacity) {
    void *file = CreateFileW(path, 0x80000000U, 1U, 0, 3U, 0x08000080U, 0);
    unsigned int total = 0, received;
    long long length = 0;
    int good = 0;
    if (file == (void *)~0ULL) return 0;
    if (!GetFileSizeEx(file, &length) || length < 128 || length > capacity) goto done;
    while (total < (unsigned int)length) {
        if (!ReadFile(file, output + total, (unsigned int)length - total, &received, 0) || !received) goto done;
        total += received;
    }
    unsigned char extra;
    if (!ReadFile(file, &extra, 1, &received, 0) || received) goto done;
    good = 1;
done:
    if (!CloseHandle(file)) good = 0;
    return good ? total : 0;
}

static void rehash(unsigned int size) {
    CryptoSha256Context context;
    crypto_sha256_init(&context);
    crypto_sha256_update(&context, table, 96);
    crypto_sha256_update(&context, table + 128, size - 128);
    crypto_sha256_final(&context, table + 96);
}

int ocr_tokenizer_test(const unsigned short *table_path, const unsigned short *fixture_path) {
    OcrTokenizer tokenizer, rejected;
    unsigned int table_size = read_all(table_path, table, sizeof(table));
    unsigned int fixture_size = read_all(fixture_path, fixtures, sizeof(fixtures));
    unsigned int cursor = 132;
    if (!ocr_tokenizer_open(&tokenizer, table, table_size) || !ocr_artifact(fixtures, fixture_size, 2) || fixture_size < cursor) return 0;
    for (unsigned int index = 32; index < 92; ++index) if (table[index] != fixtures[index]) return 0;
    unsigned int count = number(fixtures + 128);
    if (!count || count > 1000000) return 0;
    for (unsigned int index = 0; index < count; ++index) {
        if (fixture_size - cursor < 16) return 0;
        unsigned int mode = number(fixtures + cursor), text_size = number(fixtures + cursor + 4);
        unsigned int id_count = number(fixtures + cursor + 8), output_size = number(fixtures + cursor + 12);
        cursor += 16;
        if (mode > 3 || text_size > OCR_TOKENIZER_LIMIT || id_count > OCR_TOKENIZER_LIMIT || output_size > sizeof(decoded) ||
            text_size + id_count * 4 + output_size > fixture_size - cursor) return 0;
        const unsigned char *text = fixtures + cursor;
        cursor += text_size;
        for (unsigned int token = 0; token < id_count; ++token) expected[token] = number(fixtures + cursor + token * 4);
        cursor += id_count * 4;
        const unsigned char *output = fixtures + cursor;
        cursor += output_size;
        int result;
        if (mode == 0 || mode == 3) {
            if (mode == 3 && text_size != 12) return 0;
            result = mode == 0 ? ocr_encode(&tokenizer, text, text_size, actual, OCR_TOKENIZER_LIMIT) :
                ocr_prompt(number(text), number(text + 4), number(text + 8), actual, OCR_TOKENIZER_LIMIT);
            if (result != (int)id_count) { report("FAIL encode count fixture ", index); return 0; }
            for (unsigned int token = 0; token < id_count; ++token)
                if (actual[token] != expected[token]) { report("FAIL encode IDs fixture ", index); return 0; }
            if (id_count && (mode == 0 ? ocr_encode(&tokenizer, text, text_size, actual, id_count - 1) :
                ocr_prompt(number(text), number(text + 4), number(text + 8), actual, id_count - 1)) != -1) return 0;
        }
        result = ocr_decode(&tokenizer, expected, id_count, mode == 2, decoded, sizeof(decoded));
        if (result != (int)output_size) { report("FAIL decode count fixture ", index); return 0; }
        for (unsigned int offset = 0; offset < output_size; ++offset)
            if (decoded[offset] != output[offset]) { report("FAIL decode bytes fixture ", index); return 0; }
        if (output_size && ocr_decode(&tokenizer, expected, id_count, mode == 2, decoded, output_size - 1) != -1) return 0;
    }
    if (cursor != fixture_size) return 0;
    static const unsigned char invalid[][4] = {{0xc0,0x80,0,0}, {0xed,0xa0,0x80,0}, {0xf4,0x90,0x80,0x80}, {0xe2,0x82,0,0}};
    static const unsigned int lengths[] = {2,3,4,2};
    for (unsigned int index = 0; index < 4; ++index)
        if (ocr_encode(&tokenizer, invalid[index], lengths[index], actual, OCR_TOKENIZER_LIMIT) != -1) return 0;
    if (ocr_encode(&tokenizer, decoded, OCR_TOKENIZER_LIMIT + 1, actual, OCR_TOKENIZER_LIMIT) != -1 ||
        ocr_prompt(3, 1, 0, actual, OCR_TOKENIZER_LIMIT) != -1 || ocr_prompt(0, 0, 0, actual, OCR_TOKENIZER_LIMIT) != -1 ||
        ocr_prompt(0, OCR_TOKENIZER_LIMIT, 0, actual, OCR_TOKENIZER_LIMIT) != -1) return 0;
    actual[0] = 59391;
    if (ocr_decode(&tokenizer, actual, 1, 0, decoded, sizeof(decoded)) != -1) return 0;
    if (ocr_tokenizer_open(&rejected, table, table_size - 1)) return 0;
    table[table_size - 1] ^= 1;
    if (ocr_tokenizer_open(&rejected, table, table_size)) return 0;
    table[table_size - 1] ^= 1;
    const unsigned int corrupt_offsets[] = {8,12,20,32,52,72,128,132,136,140,144,148,152,156,160,164,168,
        (unsigned int)(tokenizer.lexical - table) + 3, (unsigned int)(tokenizer.merges - table) + 3,
        (unsigned int)(tokenizer.merges - table) + 11, (unsigned int)(tokenizer.merges - table) + 15,
        (unsigned int)(tokenizer.ranges - table) + 7, (unsigned int)(tokenizer.ranges - table) + 8,
        (unsigned int)(tokenizer.bytes - table) + 3};
    for (unsigned int index = 0; index < sizeof(corrupt_offsets) / sizeof(corrupt_offsets[0]); ++index) {
        unsigned int offset = corrupt_offsets[index];
        unsigned char old = table[offset];
        table[offset] = 0xff;
        rehash(table_size);
        if (ocr_tokenizer_open(&rejected, table, table_size)) { report("FAIL malformed table offset ", offset); return 0; }
        table[offset] = old;
        rehash(table_size);
    }
    if (!ocr_tokenizer_open(&tokenizer, table, table_size)) return 0;
    report("PASS native tokenizer oracle fixtures: ", count);
    report("PASS tokenizer negative checks: ", 35);
    return 1;
}