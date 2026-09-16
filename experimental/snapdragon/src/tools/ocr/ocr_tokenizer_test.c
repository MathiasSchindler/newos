#include "ocr_tokenizer.h"
#include "crypto/sha256.h"
#ifdef OCR_IMAGE_TEST
#include "ocr_image.h"
#include "ocr_text.h"
#endif

__declspec(dllimport) void *CreateFileW(const unsigned short *, unsigned int, unsigned int, void *, unsigned int, unsigned int, void *);
__declspec(dllimport) int GetFileSizeEx(void *, long long *);
__declspec(dllimport) int ReadFile(void *, void *, unsigned int, unsigned int *, void *);
__declspec(dllimport) int CloseHandle(void *);
__declspec(dllimport) void *GetStdHandle(unsigned int);
__declspec(dllimport) int WriteFile(void *, const void *, unsigned int, unsigned int *, void *);

#ifdef OCR_IMAGE_TEST
static unsigned char table[16U * 1024U * 1024U], fixtures[256U * 1024U * 1024U];
#else
static unsigned char table[8U * 1024U * 1024U], fixtures[64U * 1024U * 1024U];
#endif
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

#ifdef OCR_IMAGE_TEST
static unsigned char image_scratch[32U * 1024U * 1024U];
static float image_patches[32U * 1024U * 1024U];
static int image_positions[OCR_TOKENIZER_LIMIT * 3];
static unsigned short text_embeddings[OCR_TEXT_VOCAB*OCR_TEXT_WIDTH];
static unsigned short text_features[32*OCR_TEXT_WIDTH];
static OcrTextInput text_input;

static int text_input_test(void) {
    unsigned int checks = 0;
    for (unsigned int index = 0; index < 32*OCR_TEXT_WIDTH; ++index) text_features[index] = (unsigned short)(0x3000+index%1024);
    for (unsigned int grid_width = 8; grid_width <= 16; grid_width += 8) {
        for (unsigned int task = 0; task < 3; ++task) {
            for (unsigned int no_think = 0; no_think < 2; ++no_think) {
                int count = ocr_prompt(task,grid_width*2,no_think,actual,OCR_TEXT_CONTEXT);
                if (count <= 0) return 0;
                for (int row = 0; row < count; ++row)
                    for (unsigned int channel = 0; channel < OCR_TEXT_WIDTH; ++channel)
                        text_embeddings[actual[row]*OCR_TEXT_WIDTH+channel] = (unsigned short)(0x2000+actual[row]%1024);
                if (!ocr_text_prepare(text_features,grid_width*2,text_embeddings,(unsigned long long)OCR_TEXT_VOCAB*OCR_TEXT_WIDTH,8,grid_width,task,no_think,&text_input) ||
                    text_input.count != (unsigned int)count || text_input.image_tokens != grid_width*2) return 0;
                unsigned int image_index = 0;
                for (unsigned int row = 0; row < OCR_TEXT_CONTEXT; ++row) {
                    if (text_input.ids[row] != (row < (unsigned int)count ? actual[row] : 59246)) return 0;
                    for (unsigned int channel = 0; channel < OCR_TEXT_WIDTH; ++channel) {
                        unsigned short expected_value = row >= (unsigned int)count ? 0 : actual[row] == 59280 ?
                            text_features[image_index*OCR_TEXT_WIDTH+channel] : text_embeddings[actual[row]*OCR_TEXT_WIDTH+channel];
                        if (text_input.embeddings[row*OCR_TEXT_WIDTH+channel] != expected_value) return 0;
                    }
                    if (row < (unsigned int)count && actual[row] == 59280) ++image_index;
                    for (unsigned int column = 0; column < OCR_TEXT_CONTEXT; ++column)
                        if (text_input.mask[row*OCR_TEXT_CONTEXT+column] != (column <= row && column < (unsigned int)count ? 0 : 0xfbff)) return 0;
                }
                ++checks;
            }
        }
    }
    unsigned long long elements = (unsigned long long)OCR_TEXT_VOCAB*OCR_TEXT_WIDTH;
    if (ocr_text_prepare(text_features,15,text_embeddings,elements,8,8,0,0,&text_input) || text_input.count ||
        ocr_text_prepare(text_features,16,text_embeddings,elements-1,8,8,0,0,&text_input) ||
        ocr_text_prepare(text_features,16,text_embeddings,elements,8,8,3,0,&text_input) ||
        ocr_text_prepare(text_features,16,text_embeddings,elements,8,8,0,2,&text_input) ||
        ocr_text_prepare(0,16,text_embeddings,elements,8,8,0,0,&text_input) ||
        ocr_text_prepare(text_features,16,0,elements,8,8,0,0,&text_input) ||
        ocr_text_prepare(text_features,16,text_embeddings,elements,8,8,0,0,0) ||
        ocr_text_prepare(text_features,16,text_embeddings,elements,4,16,0,0,&text_input)) return 0;
    text_features[0] = 0x7e00;
    if (ocr_text_prepare(text_features,16,text_embeddings,elements,8,8,0,0,&text_input) || text_input.count) return 0;
    text_features[0] = 0x3000;
    text_embeddings[59248*OCR_TEXT_WIDTH] = 0x7c00;
    if (ocr_text_prepare(text_features,16,text_embeddings,elements,8,8,0,0,&text_input) || text_input.count) return 0;
    report("PASS multimodal assembly cases: ",checks);
    report("PASS multimodal rejection checks: ",10);
    return 1;
}

static void bmp_store(unsigned char *bytes, unsigned int value) {
    for (unsigned int index = 0; index < 4; ++index) bytes[index] = (unsigned char)(value >> (index * 8));
}

static int bmp_test(void) {
    unsigned char bmp[256] = {0}, rgb[64];
    unsigned int height, width, checks = 0;
    bmp[0] = 'B'; bmp[1] = 'M'; bmp[26] = 1; bmp[28] = 24;
    bmp_store(bmp + 10, 54); bmp_store(bmp + 14, 40);
    for (unsigned int columns = 1; columns <= 4; ++columns) {
        unsigned int stride = (columns * 3 + 3) & ~3U, size = 54 + stride * 3;
        bmp_store(bmp + 2, size); bmp_store(bmp + 18, columns);
        for (unsigned int orientation = 0; orientation < 2; ++orientation) {
            bmp_store(bmp + 22, orientation ? 0U - 3U : 3U);
            for (unsigned int row = 0; row < 3; ++row) {
                unsigned int source_row = orientation ? row : 2 - row;
                for (unsigned int offset = 0; offset < stride; ++offset) bmp[54 + source_row * stride + offset] = 0xa5;
                for (unsigned int column = 0; column < columns; ++column)
                    for (unsigned int channel = 0; channel < 3; ++channel)
                        bmp[54 + source_row * stride + column * 3 + 2 - channel] =
                            (unsigned char)(row * 60 + column * 3 + channel);
            }
            for (unsigned int index = 0; index < sizeof(rgb); ++index) rgb[index] = 0xcc;
            if (!ocr_image_bmp(bmp, size, rgb + 1, columns * 9, &height, &width) || height != 3 || width != columns ||
                rgb[0] != 0xcc || rgb[columns * 9 + 1] != 0xcc) return 0;
            for (unsigned int row = 0; row < 3; ++row)
                for (unsigned int offset = 0; offset < columns * 3; ++offset)
                    if (rgb[1 + row * columns * 3 + offset] != row * 60 + offset) return 0;
            if (!ocr_image_bmp(bmp, size, 0, 0, &height, &width) || height != 3 || width != columns ||
                ocr_image_bmp(bmp, size, rgb, columns * 9 - 1, &height, &width) || height || width) return 0;
            for (unsigned int length = 0; length < size; ++length) {
                if (ocr_image_bmp(bmp, length, rgb, sizeof(rgb), &height, &width)) return 0;
                ++checks;
            }
            static const unsigned int invalid[][2] = {
                {0,0}, {2,0}, {6,1}, {10,53}, {10,0xffffffffU}, {14,108},
                {18,0}, {18,10001}, {18,0xffffffffU}, {22,0}, {22,10001}, {22,0x80000000U},
                {26,24U << 16}, {28,32}, {30,1}, {34,1}, {46,1}, {50,1}
            };
            for (unsigned int index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
                unsigned int offset = invalid[index][0], saved = number(bmp + offset);
                bmp_store(bmp + offset, invalid[index][1]);
                if (ocr_image_bmp(bmp, size, rgb, sizeof(rgb), &height, &width) || height || width) return 0;
                bmp_store(bmp + offset, saved);
                ++checks;
            }
            bmp_store(bmp + 34, stride * 3);
            if (!ocr_image_bmp(bmp, size, rgb, sizeof(rgb), &height, &width)) return 0;
            bmp_store(bmp + 34, 0);
            checks += 4;
        }
    }
    if (ocr_image_bmp(0, 54, rgb, sizeof(rgb), &height, &width) ||
        ocr_image_bmp(bmp, sizeof(bmp), rgb, sizeof(rgb), 0, &width) ||
        ocr_image_bmp(bmp, sizeof(bmp), rgb, sizeof(rgb), &height, 0) ||
        ocr_image_bmp(bmp, 64ULL * 1024 * 1024 + 1, rgb, sizeof(rgb), &height, &width)) return 0;
    report("PASS BMP24 decode and rejection checks: ", checks + 4);
    return 1;
}

static int position_test(const unsigned short *path) {
    unsigned int size = read_all(path, fixtures, sizeof(fixtures)), cursor = 132;
    if (!ocr_artifact(fixtures, size, 4) || size < cursor) return 0;
    unsigned int count = number(fixtures + 128);
    if (!count || count > 10000) return 0;
    for (unsigned int index = 0; index < count; ++index) {
        if (size - cursor < 24) return 0;
        unsigned int task = number(fixtures + cursor), height = number(fixtures + cursor + 4), width = number(fixtures + cursor + 8);
        unsigned int no_think = number(fixtures + cursor + 12), length = number(fixtures + cursor + 16);
        int expected_delta = (int)number(fixtures + cursor + 20), delta;
        cursor += 24;
        if (!length || length > OCR_TOKENIZER_LIMIT || length * 17 > size - cursor || !height || !width ||
            (unsigned long long)height * width > OCR_TOKENIZER_LIMIT * 4) return 0;
        if (ocr_prompt(task, height * width / 4, no_think, actual, OCR_TOKENIZER_LIMIT) != (int)length) return 0;
        for (unsigned int token = 0; token < length; ++token)
            if (actual[token] != number(fixtures + cursor + token * 4)) return 0;
        cursor += length * 4;
        if (!ocr_image_positions(actual, length, height, width, image_positions, OCR_TOKENIZER_LIMIT * 3, decoded, sizeof(decoded), &delta) || delta != expected_delta) return 0;
        for (unsigned int value = 0; value < length * 3; ++value)
            if (image_positions[value] != (int)number(fixtures + cursor + value * 4)) { report("FAIL position fixture ", index); return 0; }
        cursor += length * 12;
        for (unsigned int token = 0; token < length; ++token)
            if (decoded[token] != fixtures[cursor + token]) return 0;
        cursor += length;
        if (ocr_image_positions(actual, length, height, width, image_positions, length * 3 - 1, decoded, sizeof(decoded), &delta) ||
            ocr_image_positions(actual, length, height, width, image_positions, OCR_TOKENIZER_LIMIT * 3, decoded, length - 1, &delta) ||
            ocr_image_positions(actual, length, height + 1, width, image_positions, OCR_TOKENIZER_LIMIT * 3, decoded, sizeof(decoded), &delta)) return 0;
        actual[length - 1] = 59280;
        if (ocr_image_positions(actual, length, height, width, image_positions, OCR_TOKENIZER_LIMIT * 3, decoded, sizeof(decoded), &delta)) return 0;
    }
    if (cursor != size) return 0;
    report("PASS exact image mRoPE/prompt fixtures: ", count);
    report("PASS position negative checks: ", count * 4);
    return 1;
}

int ocr_image_test(const unsigned short *fixture_path, const unsigned short *position_path) {
    if (!bmp_test() || !text_input_test()) return 0;
    unsigned int fixture_size = read_all(fixture_path, fixtures, sizeof(fixtures));
    if (!ocr_artifact(fixtures, fixture_size, 3) || fixture_size < 136) return 0;
    unsigned int geometry_count = number(fixtures + 128), image_count = number(fixtures + 132), cursor = 136;
    if (!geometry_count || geometry_count > 100000 || !image_count || image_count > 1000 || geometry_count * 16 > fixture_size - cursor) return 0;
    for (unsigned int index = 0; index < geometry_count; ++index) {
        OcrImageShape shape;
        unsigned int height = number(fixtures + cursor), width = number(fixtures + cursor + 4);
        unsigned int target_height = number(fixtures + cursor + 8), target_width = number(fixtures + cursor + 12);
        cursor += 16;
        int valid = ocr_image_shape(height, width, &shape);
        if (valid != (target_height != 0) || (valid && (shape.height != target_height || shape.width != target_width ||
            shape.grid_height != target_height / 14 || shape.grid_width != target_width / 14 ||
            shape.image_tokens != target_height * target_width / 784))) {
            report("FAIL image geometry fixture ", index); return 0;
        }
    }
    unsigned int checked_pixels = 0, checked_values = 0;
    float maximum_error = 0;
    for (unsigned int index = 0; index < image_count; ++index) {
        if (fixture_size - cursor < 28) return 0;
        unsigned int height = number(fixtures + cursor), width = number(fixtures + cursor + 4);
        unsigned int target_height = number(fixtures + cursor + 8), target_width = number(fixtures + cursor + 12);
        unsigned int source_size = number(fixtures + cursor + 16), target_size = number(fixtures + cursor + 20);
        unsigned int patch_size = number(fixtures + cursor + 24);
        cursor += 28;
        if (!height || !width || (unsigned long long)height * width * 3 != source_size ||
            (unsigned long long)target_height * target_width * 3 != target_size ||
            (unsigned long long)target_size * 8 != patch_size || target_size > sizeof(table) || patch_size > sizeof(image_patches) ||
            (unsigned long long)source_size + target_size + patch_size > fixture_size - cursor) return 0;
        const unsigned char *source = fixtures + cursor, *target = source + source_size, *patches = target + target_size;
        cursor += source_size + target_size + patch_size;
        if (!ocr_image_resize(source, source_size, height, width, width * 3, image_scratch, sizeof(image_scratch), table, sizeof(table))) return 0;
        for (unsigned int offset = 0; offset < target_size; ++offset)
            if (table[offset] != target[offset]) {
                report("FAIL resize image fixture ", index); report("pixel offset ", offset);
                report("actual ", table[offset]); report("expected ", target[offset]); return 0;
            }
        if (!ocr_image_patchify(table, target_size, target_height, target_width, image_patches, sizeof(image_patches) / sizeof(float))) return 0;
        for (unsigned int offset = 0; offset < patch_size / 4; ++offset) {
            union { unsigned int bits; float value; } expected_value;
            expected_value.bits = number(patches + offset * 4);
            float error = image_patches[offset] - expected_value.value;
            if (error < 0) error = -error;
            if (!(error <= 0.000001f)) { report("FAIL image patch fixture ", index); report("value offset ", offset); return 0; }
            if (error > maximum_error) maximum_error = error;
        }
        checked_pixels += target_size;
        checked_values += patch_size / 4;
        unsigned int padded_stride = width * 3 + 7;
        if ((unsigned long long)height * padded_stride > sizeof(fixtures) - fixture_size) return 0;
        unsigned char *padded = fixtures + fixture_size;
        for (unsigned int row = 0; row < height; ++row)
            for (unsigned int column = 0; column < padded_stride; ++column)
                padded[row * padded_stride + column] = column < width * 3 ? source[row * width * 3 + column] : 0xa5;
        if (!ocr_image_resize(padded, (unsigned long long)height * padded_stride, height, width, padded_stride,
                              image_scratch, sizeof(image_scratch), table, sizeof(table))) return 0;
        for (unsigned int offset = 0; offset < target_size; ++offset) if (table[offset] != target[offset]) return 0;
        if (ocr_image_resize(source, source_size - 1, height, width, width * 3, image_scratch, sizeof(image_scratch), table, sizeof(table)) ||
            ocr_image_resize(source, source_size, height, width, width * 3 - 1, image_scratch, sizeof(image_scratch), table, sizeof(table)) ||
            ocr_image_resize(source, source_size, height, width, width * 3, image_scratch, (unsigned long long)height * target_width * 3 - 1, table, sizeof(table)) ||
            ocr_image_resize(source, source_size, height, width, width * 3, image_scratch, sizeof(image_scratch), table, target_size - 1) ||
            ocr_image_patchify(table, target_size - 1, target_height, target_width, image_patches, sizeof(image_patches) / sizeof(float)) ||
            ocr_image_patchify(table, target_size, target_height, target_width, image_patches, patch_size / 4 - 1)) return 0;
    }
    OcrImageShape shape;
    if (cursor != fixture_size || ocr_image_shape(0, 1, &shape) || ocr_image_shape(1, 0, &shape) ||
        ocr_image_shape(10001, 28, &shape) || ocr_image_shape(28, 10001, &shape) ||
        ocr_image_shape(1, 201, &shape) || ocr_image_shape(28, 28, 0) ||
        ocr_image_patchify(table, sizeof(table), 27, 28, image_patches, sizeof(image_patches) / sizeof(float)) ||
        ocr_artifact(fixtures, fixture_size - 1, 3)) return 0;
    fixtures[fixture_size - 1] ^= 1;
    if (ocr_artifact(fixtures, fixture_size, 3)) return 0;
    fixtures[fixture_size - 1] ^= 1;
    report("PASS image geometry fixtures: ", geometry_count);
    report("PASS byte-exact resized RGB values: ", checked_pixels);
    report("PASS normalized patch values: ", checked_values);
    report("Maximum absolute patch error (nanounits, rounded up): ", (unsigned int)(maximum_error * 1000000000.0f + 0.999f));
    report("PASS padded-row image fixtures: ", image_count);
    report("PASS image negative checks: ", image_count * 6 + 9);
    return position_test(position_path);
}
#endif

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