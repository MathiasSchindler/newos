#include "ocr_image.h"

__declspec(dllimport) void *CreateFileW(const unsigned short *, unsigned int, unsigned int, void *, unsigned int, unsigned int, void *);
__declspec(dllimport) int GetFileSizeEx(void *, long long *);
__declspec(dllimport) int ReadFile(void *, void *, unsigned int, unsigned int *, void *);
__declspec(dllimport) int WriteFile(void *, const void *, unsigned int, unsigned int *, void *);
__declspec(dllimport) int CloseHandle(void *);
__declspec(dllimport) void *VirtualAlloc(void *, unsigned long long, unsigned int, unsigned int);
__declspec(dllimport) int VirtualFree(void *, unsigned long long, unsigned int);
__declspec(dllimport) void *GetStdHandle(unsigned int);

void ocr_image_release(OcrPreparedImage *image) {
    if (!image) return;
    if (image->patches) VirtualFree(image->patches, 0, 0x8000);
    *image = (OcrPreparedImage){0};
}

static int image_load(const unsigned short *path, OcrPreparedImage *image, int fit) {
    if (!image) return 0;
    *image = (OcrPreparedImage){0};
    if (!path) return 0;
    void *file = CreateFileW(path, 0x80000000U, 1, 0, 3, 0x08000080U, 0);
    if (file == (void *)~0ULL) return 0;
    unsigned char *encoded = 0, *workspace = 0;
    long long size = 0;
    int good = 0;
    unsigned int received, total = 0;
    int (*decode)(const unsigned char *, unsigned long long, unsigned char *, unsigned long long, unsigned int *, unsigned int *) = ocr_image_bmp;
    if (!GetFileSizeEx(file, &size) || size < 54 || size > 64LL * 1024 * 1024) goto done;
    encoded = VirtualAlloc(0, (unsigned long long)size, 0x3000, 4);
    if (!encoded) goto done;
    while (total < (unsigned int)size) {
        if (!ReadFile(file, encoded + total, (unsigned int)size - total, &received, 0) || !received) goto done;
        total += received;
    }
    unsigned char extra;
    if (encoded[0] == 137) decode = ocr_image_png;
    if (!ReadFile(file, &extra, 1, &received, 0) || received ||
        !decode(encoded, (unsigned long long)size, 0, 0, &image->source_height, &image->source_width) ||
                !(fit < 0 ? ocr_image_shape(image->source_height, image->source_width, &image->shape) :
                    ocr_image_fit_shape(image->source_height, image->source_width, fit, &image->shape))) goto done;
    unsigned long long rgb_size = (unsigned long long)image->source_height * image->source_width * 3;
    unsigned long long scratch_size = (unsigned long long)image->source_height * image->shape.width * 3;
    unsigned long long resized_size = (unsigned long long)image->shape.height * image->shape.width * 3;
    unsigned long long workspace_size = rgb_size + scratch_size + resized_size;
    image->value_count = resized_size * 2;
    unsigned long long decode_workspace = decode == ocr_image_png ? (unsigned long long)size+(8ULL*image->source_width+16)*image->source_height+64 : 0;
    if ((unsigned long long)size + workspace_size + decode_workspace + image->value_count * sizeof(float) > 256ULL * 1024 * 1024) goto done;
    workspace = VirtualAlloc(0, workspace_size, 0x3000, 4);
    image->patches = VirtualAlloc(0, image->value_count * sizeof(float), 0x3000, 4);
    if (!workspace || !image->patches) goto done;
    unsigned char *scratch = workspace + rgb_size, *resized = scratch + scratch_size;
    if (!decode(encoded, (unsigned long long)size, workspace, rgb_size, &image->source_height, &image->source_width) ||
        !(fit < 0 ? ocr_image_resize(workspace, rgb_size, image->source_height, image->source_width, image->source_width * 3,
                         scratch, scratch_size, resized, resized_size) :
          ocr_image_resize_fit(workspace, rgb_size, image->source_height, image->source_width, image->source_width * 3, fit,
                       scratch, scratch_size, resized, resized_size)) ||
        !ocr_image_patchify(resized, resized_size, image->shape.height, image->shape.width,
                            image->patches, image->value_count)) goto done;
    good = 1;
done:
    if (workspace && !VirtualFree(workspace, 0, 0x8000)) good = 0;
    if (encoded && !VirtualFree(encoded, 0, 0x8000)) good = 0;
    if (!CloseHandle(file)) good = 0;
    if (!good) ocr_image_release(image);
    return good;
}

int ocr_image_load(const unsigned short *path, OcrPreparedImage *image) {
    return image_load(path, image, -1);
}

int ocr_image_load_fitted(const unsigned short *path, OcrPreparedImage *image, int large) {
    return image_load(path, image, large != 0);
}

static unsigned int append(char *text, unsigned int cursor, const char *label, unsigned long long value) {
    char digits[24];
    unsigned int count = 0;
    while (*label) text[cursor++] = *label++;
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) text[cursor++] = digits[--count];
    return cursor;
}

int ocr_image_export(const unsigned short *input_path, const unsigned short *output_path, int fitted) {
    OcrPreparedImage image = {0};
    if (!input_path || !output_path || !image_load(input_path, &image, fitted ? 1 : -1)) return 0;
    void *file = CreateFileW(output_path, 0x40000000U, 0, 0, 1, 0x80, 0);
    int good = 0;
    if (file == (void *)~0ULL) goto done;
    unsigned int total = 0, written = 0, size = (unsigned int)(image.value_count * sizeof(float));
    while (total < size) {
        if (!WriteFile(file, (const unsigned char *)image.patches + total, size - total, &written, 0) || !written) break;
        total += written;
    }
    good = CloseHandle(file) && total == size;
    if (good) {
        char text[512];
        unsigned int length = append(text, 0, "{\"source_width\":", image.source_width);
        length = append(text, length, ",\"source_height\":", image.source_height);
        length = append(text, length, ",\"width\":", image.shape.width);
        length = append(text, length, ",\"height\":", image.shape.height);
        length = append(text, length, ",\"grid_width\":", image.shape.grid_width);
        length = append(text, length, ",\"grid_height\":", image.shape.grid_height);
        length = append(text, length, ",\"image_tokens\":", image.shape.image_tokens);
        length = append(text, length, ",\"patches\":", image.shape.grid_height * image.shape.grid_width);
        length = append(text, length, ",\"features\":", 1176);
        length = append(text, length, ",\"float32_values\":", image.value_count);
        text[length++] = '}'; text[length++] = '\n';
        good = WriteFile(GetStdHandle((unsigned int)-11), text, length, &written, 0) && written == length;
    }
done:
    ocr_image_release(&image);
    return good;
}