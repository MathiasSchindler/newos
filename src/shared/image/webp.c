#include "image_internal.h"

int image_probe_webp(const unsigned char *data, size_t size, ImageInfo *info) {
    const unsigned char *chunk;
    unsigned int chunk_size;

    if (size < 20U || !image_bytes_equal(data, "RIFF", 4U) || !image_bytes_equal(data + 8U, "WEBP", 4U)) {
        return 0;
    }

    info->format = IMAGE_FORMAT_WEBP;
    chunk = data + 12U;
    chunk_size = image_read_u32_le(chunk + 4U);
    if (image_bytes_equal(chunk, "VP8X", 4U) && chunk_size >= 10U && size >= 30U) {
        image_set_variant(info, "extended WebP");
        image_set_compression(info, "VP8/VP8L");
        image_set_dimensions(info, image_read_u24_le(data + 24U) + 1U, image_read_u24_le(data + 27U) + 1U);
        image_set_channels(info, (data[20] & 0x10U) != 0U ? 4U : 3U);
        image_set_color_model(info, (data[20] & 0x10U) != 0U ? "rgba" : "rgb");
        if ((data[20] & 0x02U) != 0U) {
            info->property_flags |= IMAGE_PROPERTY_ANIMATED;
        }
        if ((data[20] & 0x04U) != 0U) {
            info->property_flags |= IMAGE_PROPERTY_XMP;
        }
        if ((data[20] & 0x08U) != 0U) {
            info->property_flags |= IMAGE_PROPERTY_EXIF;
        }
        if ((data[20] & 0x10U) != 0U) {
            info->property_flags |= IMAGE_PROPERTY_ALPHA;
        }
        if ((data[20] & 0x20U) != 0U) {
            info->property_flags |= IMAGE_PROPERTY_ICC;
        }
    } else if (image_bytes_equal(chunk, "VP8 ", 4U) && chunk_size >= 10U && size >= 30U &&
               data[23] == 0x9dU && data[24] == 0x01U && data[25] == 0x2aU) {
        image_set_variant(info, "lossy WebP");
        image_set_compression(info, "VP8");
        image_set_dimensions(info, image_read_u16_le(data + 26U) & 0x3fffU, image_read_u16_le(data + 28U) & 0x3fffU);
        image_set_channels(info, 3U);
        image_set_color_model(info, "rgb");
    } else if (image_bytes_equal(chunk, "VP8L", 4U) && chunk_size >= 5U && size >= 25U && data[20] == 0x2fU) {
        unsigned int packed = image_read_u32_le(data + 21U);
        image_set_variant(info, "lossless WebP");
        image_set_compression(info, "VP8L");
        info->property_flags |= IMAGE_PROPERTY_LOSSLESS | IMAGE_PROPERTY_ALPHA;
        image_set_dimensions(info, (packed & 0x3fffU) + 1U, ((packed >> 14) & 0x3fffU) + 1U);
        image_set_channels(info, 4U);
        image_set_color_model(info, "rgba");
    }
    return 1;
}
