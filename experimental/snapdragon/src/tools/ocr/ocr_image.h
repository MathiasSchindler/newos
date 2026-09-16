#ifndef OCR_IMAGE_H
#define OCR_IMAGE_H

typedef struct {
    unsigned int height, width, grid_height, grid_width, image_tokens;
} OcrImageShape;

int ocr_image_shape(unsigned int height, unsigned int width, OcrImageShape *shape);
int ocr_image_resize(const unsigned char *rgb, unsigned long long size,
                     unsigned int height, unsigned int width, unsigned int stride,
                     unsigned char *scratch, unsigned long long scratch_size,
                     unsigned char *output, unsigned long long output_size);
int ocr_image_patchify(const unsigned char *rgb, unsigned long long size,
                       unsigned int height, unsigned int width,
                       float *output, unsigned long long capacity);
int ocr_image_positions(const unsigned int *ids, unsigned int count,
                         unsigned int grid_height, unsigned int grid_width,
                         int *positions, unsigned int capacity,
                         unsigned char *modalities, unsigned int modality_capacity, int *delta);
int ocr_image_test(const unsigned short *fixture_path, const unsigned short *position_path);
#endif