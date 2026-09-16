#include "ocr_image.h"

static unsigned int bmp_u32(const unsigned char *bytes) {
    return (unsigned int)bytes[0] | (unsigned int)bytes[1] << 8 |
           (unsigned int)bytes[2] << 16 | (unsigned int)bytes[3] << 24;
}

int ocr_image_bmp(const unsigned char *data, unsigned long long size,
                  unsigned char *rgb, unsigned long long capacity,
                  unsigned int *height, unsigned int *width) {
    if (!height || !width) return 0;
    *height = *width = 0;
    if (!data || size < 54 || size > 64ULL * 1024 * 1024 || data[0] != 'B' || data[1] != 'M' ||
        bmp_u32(data + 2) != size || bmp_u32(data + 6) || bmp_u32(data + 14) != 40 ||
        data[26] != 1 || data[27] || data[28] != 24 || data[29] || bmp_u32(data + 30) ||
        bmp_u32(data + 46) || bmp_u32(data + 50)) return 0;
    unsigned int source_width = bmp_u32(data + 18), encoded_height = bmp_u32(data + 22);
    int top_down = (encoded_height & 0x80000000U) != 0;
    unsigned int source_height = top_down ? 0U - encoded_height : encoded_height;
    if (!source_width || source_width > 10000 || !source_height || source_height > 10000 ||
        (unsigned long long)source_width * source_height > 16000000) return 0;
    unsigned int offset = bmp_u32(data + 10), stride = (source_width * 3 + 3) & ~3U;
    unsigned long long pixel_size = (unsigned long long)stride * source_height;
    if (offset < 54 || offset > size || pixel_size != size - offset ||
        (bmp_u32(data + 34) && bmp_u32(data + 34) != pixel_size)) return 0;
    if (rgb) {
        if (capacity < (unsigned long long)source_width * source_height * 3) return 0;
        for (unsigned int row = 0; row < source_height; ++row) {
            const unsigned char *source = data + offset +
                (unsigned long long)(top_down ? row : source_height - 1 - row) * stride;
            unsigned char *target = rgb + (unsigned long long)row * source_width * 3;
            for (unsigned int column = 0; column < source_width; ++column) {
                target[column * 3] = source[column * 3 + 2];
                target[column * 3 + 1] = source[column * 3 + 1];
                target[column * 3 + 2] = source[column * 3];
            }
        }
    }
    *height = source_height;
    *width = source_width;
    return 1;
}

int ocr_image_positions(const unsigned int *ids, unsigned int count,
                         unsigned int grid_height, unsigned int grid_width,
                         int *positions, unsigned int capacity,
                         unsigned char *modalities, unsigned int modality_capacity, int *delta) {
    if (!ids || !positions || !modalities || !delta || !count || count > 8192 || capacity < count * 3 || modality_capacity < count ||
        !grid_height || !grid_width || grid_height % 2 || grid_width % 2 ||
        (unsigned long long)grid_height * grid_width > 8192 * 4) return 0;
    unsigned int begin = 0, end, image_count = grid_height * grid_width / 4;
    while (begin < count && ids[begin] != 59280) ++begin;
    end = begin;
    while (end < count && ids[end] == 59280) ++end;
    if (end - begin != image_count) return 0;
    for (unsigned int index = 0; index < count; ++index)
        if (ids[index] >= 59282 || (index >= end && ids[index] == 59280)) return 0;
    unsigned int next = begin + (grid_height > grid_width ? grid_height : grid_width) / 2;
    for (unsigned int index = 0; index < count; ++index) {
        modalities[index] = index >= begin && index < end;
        if (modalities[index]) {
            positions[index] = (int)begin;
            positions[count + index] = (int)(begin + (index - begin) / (grid_width / 2));
            positions[2 * count + index] = (int)(begin + (index - begin) % (grid_width / 2));
        } else {
            int value = (int)(index < begin ? index : next + index - end);
            positions[index] = value;
            positions[count + index] = value;
            positions[2 * count + index] = value;
        }
    }
    *delta = (int)next - (int)end;
    return 1;
}

static unsigned int round_factor(unsigned int value) {
    unsigned int quotient = value / 28, remainder = value % 28;
    return (quotient + (remainder > 14 || (remainder == 14 && (quotient & 1)))) * 28;
}

int ocr_image_shape(unsigned int height, unsigned int width, OcrImageShape *shape) {
    OcrImageShape result = {0};
    if (!shape) return 0;
    *shape = result;
    if (!height || !width || height > 10000 || width > 10000) return 0;
    if (height < 28 || width < 28) {
        double scale = 28.0 / (height < width ? height : width);
        height = (unsigned int)(height * scale);
        width = (unsigned int)(width * scale);
    }
    if ((double)(height > width ? height : width) / (height < width ? height : width) > 200.0) return 0;
    result.height = round_factor(height);
    result.width = round_factor(width);
    unsigned long long volume = 2ULL * result.height * result.width;
    if (volume > 9633792) {
        double beta = __builtin_sqrt((2.0 * height * width) / 9633792.0);
        result.height = (unsigned int)(height / beta / 28) * 28;
        result.width = (unsigned int)(width / beta / 28) * 28;
        if (result.height < 28) result.height = 28;
        if (result.width < 28) result.width = 28;
    } else if (volume < 12544) {
        double beta = __builtin_sqrt(12544.0 / (2.0 * height * width));
        double scaled_height = height * beta / 28, scaled_width = width * beta / 28;
        result.height = ((unsigned int)scaled_height + (scaled_height > (unsigned int)scaled_height)) * 28;
        result.width = ((unsigned int)scaled_width + (scaled_width > (unsigned int)scaled_width)) * 28;
    }
    result.grid_height = result.height / 14;
    result.grid_width = result.width / 14;
    result.image_tokens = result.grid_height * result.grid_width / 4;
    *shape = result;
    return 1;
}

static double cubic(double distance) {
    if (distance < 0) distance = -distance;
    if (distance < 1) return ((1.5 * distance - 2.5) * distance) * distance + 1;
    if (distance < 2) return ((-0.5 * distance + 2.5) * distance - 4) * distance + 2;
    return 0;
}

static unsigned int coefficients(unsigned int source, unsigned int target, unsigned int position,
                                  double *weights, unsigned int *begin) {
    double scale = (double)source / target;
    double support = scale >= 1 ? 2 * scale : 2;
    double center = scale * (position + 0.5);
    double inverse = scale >= 1 ? 1 / scale : 1;
    int first = (int)(center - support + 0.5), end = (int)(center + support + 0.5);
    if (first < 0) first = 0;
    if (end > (int)source) end = (int)source;
    unsigned int count = (unsigned int)(end - first);
    if (!count || count > 2048) return 0;
    double total = 0;
    for (unsigned int index = 0; index < count; ++index) {
        weights[index] = cubic((index + first - center + 0.5) * inverse);
        total += weights[index];
    }
    if (!total) return 0;
    for (unsigned int index = 0; index < count; ++index) weights[index] /= total;
    *begin = (unsigned int)first;
    return count;
}

static unsigned int precision(unsigned int source, unsigned int target, double *weights) {
    double maximum = 0;
    for (unsigned int position = 0; position < target; ++position) {
        unsigned int begin, count = coefficients(source, target, position, weights, &begin);
        if (!count) return 0;
        for (unsigned int index = 0; index < count; ++index)
            if (weights[index] > maximum) maximum = weights[index];
    }
    unsigned int bits = 0;
    while (bits < 22 && (int)(0.5 + maximum * (1U << (bits + 1))) < 32768) ++bits;
    return bits;
}

static unsigned char pixel(int sum, unsigned int bits) {
    sum >>= bits;
    return sum < 0 ? 0 : sum > 255 ? 255 : (unsigned char)sum;
}

int ocr_image_resize(const unsigned char *rgb, unsigned long long size,
                     unsigned int height, unsigned int width, unsigned int stride,
                     unsigned char *scratch, unsigned long long scratch_size,
                     unsigned char *output, unsigned long long output_size) {
    OcrImageShape shape;
    double weights[2048];
    int fixed[2048];
    if (!rgb || !scratch || !output || !ocr_image_shape(height, width, &shape) ||
        (unsigned long long)height * width > 16000000 || stride < width * 3 ||
        size < (unsigned long long)(height - 1) * stride + width * 3 ||
        scratch_size < (unsigned long long)height * shape.width * 3 ||
        output_size < (unsigned long long)shape.height * shape.width * 3) return 0;
    unsigned int horizontal = precision(width, shape.width, weights);
    unsigned int vertical = precision(height, shape.height, weights);
    if (!horizontal || !vertical) return 0;
    for (unsigned int column = 0; column < shape.width; ++column) {
        unsigned int begin, count = coefficients(width, shape.width, column, weights, &begin);
        if (!count) return 0;
        for (unsigned int index = 0; index < count; ++index) {
            double value = weights[index] * (1U << horizontal);
            fixed[index] = (int)(value < 0 ? value - 0.5 : value + 0.5);
        }
        for (unsigned int row = 0; row < height; ++row) {
            for (unsigned int channel = 0; channel < 3; ++channel) {
                int sum = 1 << (horizontal - 1);
                for (unsigned int index = 0; index < count; ++index)
                    sum += rgb[(unsigned long long)row * stride + (begin + index) * 3 + channel] * fixed[index];
                scratch[((unsigned long long)row * shape.width + column) * 3 + channel] = pixel(sum, horizontal);
            }
        }
    }
    for (unsigned int row = 0; row < shape.height; ++row) {
        unsigned int begin, count = coefficients(height, shape.height, row, weights, &begin);
        if (!count) return 0;
        for (unsigned int index = 0; index < count; ++index) {
            double value = weights[index] * (1U << vertical);
            fixed[index] = (int)(value < 0 ? value - 0.5 : value + 0.5);
        }
        for (unsigned int column = 0; column < shape.width * 3; ++column) {
            int sum = 1 << (vertical - 1);
            for (unsigned int index = 0; index < count; ++index)
                sum += scratch[(unsigned long long)(begin + index) * shape.width * 3 + column] * fixed[index];
            output[(unsigned long long)row * shape.width * 3 + column] = pixel(sum, vertical);
        }
    }
    return 1;
}

int ocr_image_patchify(const unsigned char *rgb, unsigned long long size,
                       unsigned int height, unsigned int width, float *output, unsigned long long capacity) {
    static const float mean[3] = {0.48145466f,0.4578275f,0.40821073f};
    static const float deviation[3] = {0.26862954f,0.26130258f,0.27577711f};
    float normalized[3][256];
    if (!rgb || !output || !height || !width || height % 28 || width % 28 || height > 32768 || width > 32768 ||
        (unsigned long long)height * width > 4816896 || size < (unsigned long long)height * width * 3 ||
        capacity < (unsigned long long)height * width * 6) return 0;
    for (unsigned int channel = 0; channel < 3; ++channel)
        for (unsigned int value = 0; value < 256; ++value)
            normalized[channel][value] = ((float)value - mean[channel] * 255.0f) / (deviation[channel] * 255.0f);
    unsigned long long cursor = 0;
    for (unsigned int block_row = 0; block_row < height; block_row += 28)
        for (unsigned int block_column = 0; block_column < width; block_column += 28)
            for (unsigned int local_row = 0; local_row < 2; ++local_row)
                for (unsigned int local_column = 0; local_column < 2; ++local_column)
                    for (unsigned int channel = 0; channel < 3; ++channel)
                        for (unsigned int temporal = 0; temporal < 2; ++temporal)
                            for (unsigned int row = 0; row < 14; ++row)
                                for (unsigned int column = 0; column < 14; ++column) {
                                    unsigned int source_row = block_row + local_row * 14 + row;
                                    unsigned int source_column = block_column + local_column * 14 + column;
                                    output[cursor++] = normalized[channel][rgb[((unsigned long long)source_row * width + source_column) * 3 + channel]];
                                }
    return 1;
}