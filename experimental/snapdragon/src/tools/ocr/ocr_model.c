#include "crypto/sha256.h"
#ifdef OCR_TEXT_DECODER
int ocr_prefill_run(const unsigned short *, const unsigned short *, const unsigned short *, const unsigned short *, const unsigned short *, unsigned int);
int ocr_prefill_input_test(const unsigned short *);
int ocr_generate_run(const unsigned short *, const unsigned short *, const unsigned short *, const unsigned short *, const unsigned short *, const unsigned short *, unsigned int, unsigned int);
int ocr_generation_test(const unsigned short *);
#endif
#ifdef OCR_VISION_RUN
int ocr_vision_run(const unsigned short *, const unsigned short *, const unsigned short *, const unsigned short *);
int ocr_vision_check(const unsigned short *);
#endif
#ifdef OCR_HTP_TEST
int ocr_htp_test(const unsigned short *library, const unsigned short *fixtures, const unsigned short *capture);
#endif
#if defined(OCR_IMAGE_TEST) || defined(OCR_IMAGE_FILE)
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
static OcrWide arguments[9][32768];

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
        if (count == 9U) return 0;
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
#ifdef OCR_TEXT_DECODER
    } else if (count == 3U && word(arguments[1], "--test-generation")) {
        result = ocr_generation_test(arguments[2]);
    } else if (count == 9U && (word(arguments[1], "--generate-text") || word(arguments[1], "--generate-formula") || word(arguments[1], "--generate-table"))) {
        unsigned int limit = 0, digits = 0;
        while (arguments[8][digits] >= '0' && arguments[8][digits] <= '9' && digits < 3) {
            limit = limit*10+arguments[8][digits++]-'0';
        }
        if (!digits || arguments[8][digits] || !limit || limit > 256) { ExitProcess(2U); return; }
        unsigned int task = word(arguments[1], "--generate-text") ? 0 : word(arguments[1], "--generate-formula") ? 1 : 2;
        result = ocr_generate_run(arguments[2],arguments[3],arguments[4],arguments[5],arguments[6],arguments[7],task,limit);
        ExitProcess(result == 1 ? 0U : result == 2 ? 3U : 1U); return;
    } else if (count == 3U && word(arguments[1], "--test-text-input")) {
        result = ocr_prefill_input_test(arguments[2]);
    } else if (count == 7U && (word(arguments[1], "--prefill-text") || word(arguments[1], "--prefill-formula") || word(arguments[1], "--prefill-table"))) {
        unsigned int task = word(arguments[1], "--prefill-text") ? 0 : word(arguments[1], "--prefill-formula") ? 1 : 2;
        result = ocr_prefill_run(arguments[2],arguments[3],arguments[4],arguments[5],arguments[6],task);
#endif
#ifdef OCR_VISION_RUN
    } else if (count == 6U && word(arguments[1], "--vision")) {
        result = ocr_vision_run(arguments[2],arguments[3],arguments[4],arguments[5]);
    } else if (count == 3U && word(arguments[1], "--check-vision")) {
        result = ocr_vision_check(arguments[2]);
#endif
#ifdef OCR_IMAGE_FILE
    } else if (count == 4U && (word(arguments[1], "--prepare-image") || word(arguments[1], "--prepare-image-fit"))) {
        result = ocr_image_export(arguments[2], arguments[3], word(arguments[1], "--prepare-image-fit"));
        if (!result) print("FAIL image preparation: require bounded BMP24 or static PNG and a new writable output path; no inference performed\n", (unsigned int)-12);
        ExitProcess(result ? 0U : 1U);
        return;
#endif
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
    #ifdef OCR_TEXT_DECODER
        print("Generation: --generate-text|--generate-formula|--generate-table DLL VISION_DIR TEXT_DIR GENERATION_DIR INPUT_IMAGE CAPTURE_DIR MAX_NEW_TOKENS (1..256); exits 0 EOS, 3 incomplete limit, 1 failure\n", (unsigned int)-12);
        print("Prefill: --prefill-text|--prefill-formula|--prefill-table DLL VISION_DIR TEXT_DIR INPUT_IMAGE CAPTURE_DIR\n", (unsigned int)-12);
    #endif
    #ifdef OCR_VISION_RUN
        print("Vision: --vision QnnHtp.dll WEIGHTS_DIR INPUT_IMAGE NEW_CAPTURE_DIR | --check-vision WEIGHTS_DIR\n", (unsigned int)-12);
    #endif
#ifdef OCR_IMAGE_FILE
        print("Image preprocessing only: --prepare-image INPUT_IMAGE OUTPUT.f32; --prepare-image-fit fits the large OCR raster; BMP24 or static PNG\n", (unsigned int)-12);
#endif
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