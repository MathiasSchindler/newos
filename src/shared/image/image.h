#ifndef NEWOS_IMAGE_H
#define NEWOS_IMAGE_H

#include <stddef.h>

#define IMAGE_INFO_HAS_DIMENSIONS (1U << 0)
#define IMAGE_INFO_HAS_BIT_DEPTH  (1U << 1)
#define IMAGE_INFO_HAS_CHANNELS   (1U << 2)
#define IMAGE_INFO_HAS_FRAMES     (1U << 3)
#define IMAGE_INFO_HAS_VARIANT    (1U << 4)
#define IMAGE_INFO_HAS_COLOR      (1U << 5)
#define IMAGE_INFO_HAS_COMPRESSION (1U << 6)
#define IMAGE_INFO_HAS_ORIENTATION (1U << 7)
#define IMAGE_INFO_HAS_DENSITY    (1U << 8)

#define IMAGE_PROPERTY_ALPHA      (1U << 0)
#define IMAGE_PROPERTY_PALETTE    (1U << 1)
#define IMAGE_PROPERTY_INTERLACED (1U << 2)
#define IMAGE_PROPERTY_ANIMATED   (1U << 3)
#define IMAGE_PROPERTY_PROGRESSIVE (1U << 4)
#define IMAGE_PROPERTY_LOSSLESS   (1U << 5)
#define IMAGE_PROPERTY_EXIF       (1U << 6)
#define IMAGE_PROPERTY_ICC        (1U << 7)
#define IMAGE_PROPERTY_XMP        (1U << 8)
#define IMAGE_PROPERTY_TOP_DOWN   (1U << 9)
#define IMAGE_PROPERTY_LOOPING    (1U << 10)
#define IMAGE_PROPERTY_ORIENTATION (1U << 11)

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
    unsigned int orientation;
    unsigned int density_x;
    unsigned int density_y;
    unsigned int property_flags;
    const char *variant;
    const char *color_model;
    const char *compression;
    const char *density_unit;
} ImageInfo;

typedef struct {
    ImageFormat format;
    int valid;
    const char *message;
} ImageValidation;

void image_info_init(ImageInfo *info);
int image_probe(const unsigned char *data, size_t size, ImageInfo *info_out);
int image_validate(const unsigned char *data, size_t size, ImageValidation *validation_out);
const char *image_format_name(ImageFormat format);
const char *image_format_extension(ImageFormat format);
const char *image_format_mime(ImageFormat format);
const char *image_channel_description(const ImageInfo *info);
const char *image_property_name(unsigned int property);
const char *image_orientation_description(unsigned int orientation);

#endif
