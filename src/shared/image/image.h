#ifndef NEWOS_IMAGE_H
#define NEWOS_IMAGE_H

#include <stddef.h>

#define IMAGE_INFO_HAS_DIMENSIONS (1U << 0)
#define IMAGE_INFO_HAS_BIT_DEPTH  (1U << 1)
#define IMAGE_INFO_HAS_CHANNELS   (1U << 2)
#define IMAGE_INFO_HAS_FRAMES     (1U << 3)

typedef enum {
    IMAGE_FORMAT_UNKNOWN = 0,
    IMAGE_FORMAT_PNG,
    IMAGE_FORMAT_JPEG,
    IMAGE_FORMAT_GIF,
    IMAGE_FORMAT_TIFF,
    IMAGE_FORMAT_WEBP,
    IMAGE_FORMAT_BMP
} ImageFormat;

typedef struct {
    ImageFormat format;
    unsigned int flags;
    unsigned int width;
    unsigned int height;
    unsigned int bit_depth;
    unsigned int channel_count;
    unsigned int frame_count;
} ImageInfo;

void image_info_init(ImageInfo *info);
int image_probe(const unsigned char *data, size_t size, ImageInfo *info_out);
const char *image_format_name(ImageFormat format);
const char *image_format_extension(ImageFormat format);
const char *image_format_mime(ImageFormat format);
const char *image_channel_description(const ImageInfo *info);

#endif
