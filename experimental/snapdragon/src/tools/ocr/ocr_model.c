#include "crypto/sha256.h"
#ifdef OCR_HTP_TEST
int ocr_htp_test(const unsigned short *library, const unsigned short *fixtures, const unsigned short *capture);
#endif
#ifdef OCR_IMAGE_TEST
#include "ocr_image.h"
#endif
#ifdef OCR_TOKENIZER_TEST
#include "ocr_tokenizer.h"
#endif

typedef unsigned long long OcrSize;
typedef unsigned short OcrWide;

__declspec(dllimport) void *GetStdHandle(unsigned int);
__declspec(dllimport) OcrWide *GetCommandLineW(void);
__declspec(dllimport) void *CreateFileW(const OcrWide *, unsigned int, unsigned int, void *, unsigned int, unsigned int, void *);
__declspec(dllimport) int GetFileSizeEx(void *, long long *);
__declspec(dllimport) int ReadFile(void *, void *, unsigned int, unsigned int *, void *);
__declspec(dllimport) int WriteFile(void *, const void *, unsigned int, unsigned int *, void *);
__declspec(dllimport) int CloseHandle(void *);
__declspec(dllimport) void ExitProcess(unsigned int);

void *memset(void *destination, int value, OcrSize length) {
    unsigned char *bytes = destination;
    for (OcrSize index = 0; index < length; ++index) bytes[index] = (unsigned char)value;
    return destination;
}

void *memcpy(void *destination, const void *source, OcrSize length) {
    unsigned char *output = destination;
    const unsigned char *input = source;
    for (OcrSize index = 0; index < length; ++index) output[index] = input[index];
    return destination;
}

static unsigned char buffer[1024U * 1024U];
static OcrWide arguments[5][32768];

static int print(const char *text, unsigned int stream) {
    unsigned int length = 0, written = 0;
    while (text[length]) ++length;
    return WriteFile(GetStdHandle(stream), text, length, &written, 0) && written == length;
}

static int same(const unsigned char *left, const unsigned char *right, unsigned int length) {
    for (unsigned int index = 0; index < length; ++index) if (left[index] != right[index]) return 0;
    return 1;
}

static int word(const OcrWide *wide, const char *text) {
    unsigned int index = 0;
    while (text[index] && wide[index] == (unsigned char)text[index]) ++index;
    return !text[index] && !wide[index];
}

static unsigned int parse_arguments(void) {
    const OcrWide *cursor = GetCommandLineW();
    unsigned int count = 0;
    while (*cursor) {
        unsigned int length = 0;
        int quoted = 0;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (!*cursor) break;
        if (count == 5U) return 0;
        while (*cursor && (quoted || (*cursor != ' ' && *cursor != '\t'))) {
            unsigned int slashes = 0;
            while (*cursor == '\\') { ++slashes; ++cursor; }
            unsigned int copies = *cursor == '"' ? slashes / 2U : slashes;
            while (copies--) {
                if (length == 32767U) return 0;
                arguments[count][length++] = '\\';
            }
            if (*cursor == '"') {
                if (slashes & 1U) {
                    if (length == 32767U) return 0;
                    arguments[count][length++] = '"';
                } else quoted = !quoted;
                ++cursor;
            } else {
                if (!*cursor || (!quoted && (*cursor == ' ' || *cursor == '\t'))) break;
                if (length == 32767U) return 0;
                arguments[count][length++] = *cursor++;
            }
        }
        if (quoted) return 0;
        arguments[count++][length] = 0;
    }
    return count;
}

static int hex_digit(OcrWide value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int parse_hash(const OcrWide *text, unsigned char *hash) {
    unsigned int length = 0;
    while (text[length]) ++length;
    if (length != 64U) return 0;
    for (unsigned int index = 0; index < 32U; ++index) {
        int high = hex_digit(text[index * 2U]), low = hex_digit(text[index * 2U + 1U]);
        if (high < 0 || low < 0) return 0;
        hash[index] = (unsigned char)(high * 16 + low);
    }
    return 1;
}

static int verify(const OcrWide *path, const unsigned char *expected, int weights) {
    void *file = CreateFileW(path, 0x80000000U, 1U, 0, 3U, 0x08000080U, 0);
    long long file_size = 0;
    unsigned long long total = 0;
    unsigned char digest[32], prefix[8];
    unsigned int prefix_size = 0;
    CryptoSha256Context context;
    int result = 0;
    if (file == (void *)(OcrSize)-1) return 0;
    if (!GetFileSizeEx(file, &file_size) || file_size < 0) goto done;
    crypto_sha256_init(&context);
    for (;;) {
        unsigned int received = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &received, 0)) goto done;
        if (!received) break;
        for (unsigned int index = 0; index < received && prefix_size < 8U; ++index)
            prefix[prefix_size++] = buffer[index];
        crypto_sha256_update(&context, buffer, received);
        total += received;
        if (total > (unsigned long long)file_size) goto done;
    }
    if (total != (unsigned long long)file_size) goto done;
    crypto_sha256_final(&context, digest);
    if (!same(digest, expected, 32U)) goto done;
    if (weights) {
        unsigned long long header_size = 0;
        if (prefix_size != 8U) goto done;
        for (unsigned int index = 0; index < 8U; ++index)
            header_size |= (unsigned long long)prefix[index] << (8U * index);
        if (header_size < 2U || header_size > 16U * 1024U * 1024U ||
            total < 8U || header_size > total - 8U) goto done;
    }
    result = 1;
done:
    if (!CloseHandle(file)) result = 0;
    return result;
}

static int self_test(void) {
    static const unsigned char empty_hash[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
    };
    static const unsigned char abc_hash[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    unsigned char digest[32];
    CryptoSha256Context context;
    crypto_sha256_init(&context);
    crypto_sha256_final(&context, digest);
    if (!same(digest, empty_hash, 32U)) return 0;
    crypto_sha256_init(&context);
    crypto_sha256_update(&context, (const unsigned char *)"a", 1);
    crypto_sha256_update(&context, (const unsigned char *)"bc", 2);
    crypto_sha256_final(&context, digest);
    return same(digest, abc_hash, 32U);
}

void mainCRTStartup(void) {
    unsigned int count = parse_arguments();
    unsigned char expected[32];
    int result;
    if (count == 2U && word(arguments[1], "--self-test")) {
        result = self_test();
#ifdef OCR_HTP_TEST
    } else if (count == 4U && word(arguments[1], "--test-htp")) {
        result = ocr_htp_test(arguments[2], arguments[3],0);
    } else if (count == 5U && word(arguments[1], "--capture-htp")) {
        result = ocr_htp_test(arguments[2], arguments[3],arguments[4]);
#endif
#ifdef OCR_IMAGE_TEST
    } else if (count == 4U && word(arguments[1], "--test-images")) {
        result = ocr_image_test(arguments[2], arguments[3]);
#endif
#ifdef OCR_TOKENIZER_TEST
    } else if (count == 4U && word(arguments[1], "--test-tokenizer")) {
        result = ocr_tokenizer_test(arguments[2], arguments[3]);
#endif
    } else if (count == 4U && (word(arguments[1], "--verify") || word(arguments[1], "--weights")) &&
               parse_hash(arguments[3], expected)) {
        result = verify(arguments[2], expected, word(arguments[1], "--weights"));
    } else {
        print("Usage: ocr-model --self-test | --verify FILE SHA256 | --weights FILE SHA256\n", (unsigned int)-12);
        ExitProcess(2U);
        return;
    }
    if (!result) {
        print("FAIL OCR artifact verification\n", (unsigned int)-12);
        ExitProcess(1U);
        return;
    }
    ExitProcess(print("PASS OCR artifact verification\n", (unsigned int)-11) ? 0U : 1U);
}