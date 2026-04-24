#include "image/image.h"
#include "runtime.h"

static unsigned int read_u16_le(const unsigned char *bytes) {
    return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8);
}

static unsigned int read_u16_be(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 8) | (unsigned int)bytes[1];
}

static unsigned int read_u24_le(const unsigned char *bytes) {
    return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8) | ((unsigned int)bytes[2] << 16);
}

static unsigned int read_u32_le(const unsigned char *bytes) {
    return (unsigned int)bytes[0] |
           ((unsigned int)bytes[1] << 8) |
           ((unsigned int)bytes[2] << 16) |
           ((unsigned int)bytes[3] << 24);
}

static unsigned int read_u32_be(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 24) |
           ((unsigned int)bytes[1] << 16) |
           ((unsigned int)bytes[2] << 8) |
           (unsigned int)bytes[3];
}

static int bytes_equal(const unsigned char *bytes, const char *text, size_t length) {
    size_t i;

    for (i = 0; i < length; ++i) {
        if (bytes[i] != (unsigned char)text[i]) {
            return 0;
        }
    }
    return 1;
}

static int byte_arrays_equal(const unsigned char *left, const unsigned char *right, size_t length) {
    size_t i;

    for (i = 0; i < length; ++i) {
        if (left[i] != right[i]) {
            return 0;
        }
    }
    return 1;
}

void image_info_init(ImageInfo *info) {
    if (info == 0) {
        return;
    }
    info->format = IMAGE_FORMAT_UNKNOWN;
    info->flags = 0U;
    info->width = 0U;
    info->height = 0U;
    info->bit_depth = 0U;
    info->channel_count = 0U;
    info->frame_count = 0U;
}

static void set_dimensions(ImageInfo *info, unsigned int width, unsigned int height) {
    if (width != 0U && height != 0U) {
        info->width = width;
        info->height = height;
        info->flags |= IMAGE_INFO_HAS_DIMENSIONS;
    }
}

static void set_bit_depth(ImageInfo *info, unsigned int bit_depth) {
    if (bit_depth != 0U) {
        info->bit_depth = bit_depth;
        info->flags |= IMAGE_INFO_HAS_BIT_DEPTH;
    }
}

static void set_channels(ImageInfo *info, unsigned int channel_count) {
    if (channel_count != 0U) {
        info->channel_count = channel_count;
        info->flags |= IMAGE_INFO_HAS_CHANNELS;
    }
}

static int probe_png(const unsigned char *data, size_t size, ImageInfo *info) {
    static const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    unsigned char color_type;
    unsigned int channels = 0U;

    if (size < 8U || !byte_arrays_equal(data, signature, sizeof(signature))) {
        return 0;
    }

    info->format = IMAGE_FORMAT_PNG;
    if (size >= 33U && read_u32_be(data + 8) == 13U && bytes_equal(data + 12, "IHDR", 4U)) {
        set_dimensions(info, read_u32_be(data + 16), read_u32_be(data + 20));
        set_bit_depth(info, data[24]);
        color_type = data[25];
        if (color_type == 0U) {
            channels = 1U;
        } else if (color_type == 2U) {
            channels = 3U;
        } else if (color_type == 3U) {
            channels = 1U;
        } else if (color_type == 4U) {
            channels = 2U;
        } else if (color_type == 6U) {
            channels = 4U;
        }
        set_channels(info, channels);
    }
    return 1;
}

static int probe_gif(const unsigned char *data, size_t size, ImageInfo *info) {
    if (size < 13U || !bytes_equal(data, "GIF", 3U) || data[3] != '8' || (data[4] != '7' && data[4] != '9') || data[5] != 'a') {
        return 0;
    }

    info->format = IMAGE_FORMAT_GIF;
    set_dimensions(info, read_u16_le(data + 6), read_u16_le(data + 8));
    set_bit_depth(info, (unsigned int)((data[10] & 0x07U) + 1U));
    set_channels(info, 3U);
    return 1;
}

static int is_jpeg_sof(unsigned char marker) {
    return (marker >= 0xc0U && marker <= 0xc3U) ||
           (marker >= 0xc5U && marker <= 0xc7U) ||
           (marker >= 0xc9U && marker <= 0xcbU) ||
           (marker >= 0xcdU && marker <= 0xcfU);
}

static int probe_jpeg(const unsigned char *data, size_t size, ImageInfo *info) {
    size_t offset = 2U;

    if (size < 3U || data[0] != 0xffU || data[1] != 0xd8U || data[2] != 0xffU) {
        return 0;
    }

    info->format = IMAGE_FORMAT_JPEG;
    while (offset + 3U < size) {
        unsigned char marker;
        unsigned int segment_size;

        while (offset < size && data[offset] != 0xffU) {
            offset += 1U;
        }
        while (offset < size && data[offset] == 0xffU) {
            offset += 1U;
        }
        if (offset >= size) {
            break;
        }
        marker = data[offset++];
        if (marker == 0xd9U || marker == 0xdaU) {
            break;
        }
        if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd7U)) {
            continue;
        }
        if (offset + 2U > size) {
            break;
        }
        segment_size = read_u16_be(data + offset);
        if (segment_size < 2U) {
            break;
        }
        if (is_jpeg_sof(marker)) {
            if (offset + 8U <= size) {
                set_bit_depth(info, data[offset + 2U]);
                set_dimensions(info, read_u16_be(data + offset + 5U), read_u16_be(data + offset + 3U));
                set_channels(info, data[offset + 7U]);
            }
            break;
        }
        if ((size_t)segment_size > size - offset) {
            break;
        }
        offset += (size_t)segment_size;
    }
    return 1;
}

static unsigned int tiff_read_u16(const unsigned char *bytes, int little_endian) {
    return little_endian ? read_u16_le(bytes) : read_u16_be(bytes);
}

static unsigned int tiff_read_u32(const unsigned char *bytes, int little_endian) {
    return little_endian ? read_u32_le(bytes) : read_u32_be(bytes);
}

static int tiff_value_u32(const unsigned char *data,
                          size_t size,
                          int little_endian,
                          unsigned int type,
                          unsigned int count,
                          const unsigned char *value_field,
                          unsigned int *value_out) {
    unsigned int offset;

    if (count == 0U) {
        return -1;
    }
    if (type == 3U) {
        if (count == 1U) {
            *value_out = tiff_read_u16(value_field, little_endian);
            return 0;
        }
        offset = tiff_read_u32(value_field, little_endian);
        if ((size_t)offset + 2U <= size) {
            *value_out = tiff_read_u16(data + offset, little_endian);
            return 0;
        }
    } else if (type == 4U) {
        if (count == 1U) {
            *value_out = tiff_read_u32(value_field, little_endian);
            return 0;
        }
        offset = tiff_read_u32(value_field, little_endian);
        if ((size_t)offset + 4U <= size) {
            *value_out = tiff_read_u32(data + offset, little_endian);
            return 0;
        }
    }
    return -1;
}

static int probe_tiff(const unsigned char *data, size_t size, ImageInfo *info) {
    int little_endian;
    unsigned int ifd_offset;
    unsigned int entry_count;
    unsigned int i;
    unsigned int width = 0U;
    unsigned int height = 0U;
    unsigned int bit_depth = 0U;
    unsigned int channels = 0U;

    if (size < 8U) {
        return 0;
    }
    if (data[0] == 'I' && data[1] == 'I') {
        little_endian = 1;
    } else if (data[0] == 'M' && data[1] == 'M') {
        little_endian = 0;
    } else {
        return 0;
    }
    if (tiff_read_u16(data + 2U, little_endian) != 42U) {
        return 0;
    }

    info->format = IMAGE_FORMAT_TIFF;
    ifd_offset = tiff_read_u32(data + 4U, little_endian);
    if ((size_t)ifd_offset + 2U > size) {
        return 1;
    }
    entry_count = tiff_read_u16(data + ifd_offset, little_endian);
    if ((size_t)entry_count > (size - (size_t)ifd_offset - 2U) / 12U) {
        entry_count = (unsigned int)((size - (size_t)ifd_offset - 2U) / 12U);
    }
    for (i = 0U; i < entry_count; ++i) {
        const unsigned char *entry = data + ifd_offset + 2U + (size_t)i * 12U;
        unsigned int tag = tiff_read_u16(entry, little_endian);
        unsigned int type = tiff_read_u16(entry + 2U, little_endian);
        unsigned int count = tiff_read_u32(entry + 4U, little_endian);
        unsigned int value = 0U;

        if (tiff_value_u32(data, size, little_endian, type, count, entry + 8U, &value) != 0) {
            continue;
        }
        if (tag == 256U) {
            width = value;
        } else if (tag == 257U) {
            height = value;
        } else if (tag == 258U) {
            bit_depth = value;
        } else if (tag == 277U) {
            channels = value;
        }
    }
    set_dimensions(info, width, height);
    set_bit_depth(info, bit_depth);
    set_channels(info, channels);
    return 1;
}

static int probe_webp(const unsigned char *data, size_t size, ImageInfo *info) {
    const unsigned char *chunk;
    unsigned int chunk_size;

    if (size < 20U || !bytes_equal(data, "RIFF", 4U) || !bytes_equal(data + 8U, "WEBP", 4U)) {
        return 0;
    }

    info->format = IMAGE_FORMAT_WEBP;
    chunk = data + 12U;
    chunk_size = read_u32_le(chunk + 4U);
    if (bytes_equal(chunk, "VP8X", 4U) && chunk_size >= 10U && size >= 30U) {
        set_dimensions(info, read_u24_le(data + 24U) + 1U, read_u24_le(data + 27U) + 1U);
        set_channels(info, (data[20] & 0x10U) != 0U ? 4U : 3U);
    } else if (bytes_equal(chunk, "VP8 ", 4U) && chunk_size >= 10U && size >= 30U &&
               data[23] == 0x9dU && data[24] == 0x01U && data[25] == 0x2aU) {
        set_dimensions(info, read_u16_le(data + 26U) & 0x3fffU, read_u16_le(data + 28U) & 0x3fffU);
        set_channels(info, 3U);
    } else if (bytes_equal(chunk, "VP8L", 4U) && chunk_size >= 5U && size >= 25U && data[20] == 0x2fU) {
        unsigned int packed = read_u32_le(data + 21U);
        set_dimensions(info, (packed & 0x3fffU) + 1U, ((packed >> 14) & 0x3fffU) + 1U);
        set_channels(info, 4U);
    }
    return 1;
}

static int probe_bmp(const unsigned char *data, size_t size, ImageInfo *info) {
    unsigned int dib_size;
    unsigned int width;
    unsigned int height;
    unsigned int bits;

    if (size < 26U || data[0] != 'B' || data[1] != 'M') {
        return 0;
    }

    info->format = IMAGE_FORMAT_BMP;
    dib_size = read_u32_le(data + 14U);
    if (dib_size == 12U && size >= 26U) {
        width = read_u16_le(data + 18U);
        height = read_u16_le(data + 20U);
        bits = read_u16_le(data + 24U);
        set_dimensions(info, width, height);
        set_bit_depth(info, bits);
        if (bits == 24U) {
            set_channels(info, 3U);
        } else if (bits == 8U || bits == 4U || bits == 1U) {
            set_channels(info, 1U);
        }
    } else if (dib_size >= 40U && size >= 30U) {
        width = read_u32_le(data + 18U);
        height = read_u32_le(data + 22U);
        if ((height & 0x80000000U) != 0U) {
            height = (unsigned int)(0U - height);
        }
        bits = read_u16_le(data + 28U);
        set_dimensions(info, width, height);
        set_bit_depth(info, bits);
        if (bits == 32U) {
            set_channels(info, 4U);
        } else if (bits == 24U) {
            set_channels(info, 3U);
        } else if (bits == 8U || bits == 4U || bits == 1U) {
            set_channels(info, 1U);
        }
    }
    return 1;
}

int image_probe(const unsigned char *data, size_t size, ImageInfo *info_out) {
    ImageInfo info;

    if (data == 0 || info_out == 0) {
        return -1;
    }
    image_info_init(&info);

    if (probe_png(data, size, &info) ||
        probe_jpeg(data, size, &info) ||
        probe_gif(data, size, &info) ||
        probe_tiff(data, size, &info) ||
        probe_webp(data, size, &info) ||
        probe_bmp(data, size, &info)) {
        *info_out = info;
        return 0;
    }

    *info_out = info;
    return -1;
}

const char *image_format_name(ImageFormat format) {
    switch (format) {
        case IMAGE_FORMAT_PNG: return "PNG";
        case IMAGE_FORMAT_JPEG: return "JPEG";
        case IMAGE_FORMAT_GIF: return "GIF";
        case IMAGE_FORMAT_TIFF: return "TIFF";
        case IMAGE_FORMAT_WEBP: return "WebP";
        case IMAGE_FORMAT_BMP: return "BMP";
        default: return "unknown";
    }
}

const char *image_format_extension(ImageFormat format) {
    switch (format) {
        case IMAGE_FORMAT_PNG: return "png";
        case IMAGE_FORMAT_JPEG: return "jpeg";
        case IMAGE_FORMAT_GIF: return "gif";
        case IMAGE_FORMAT_TIFF: return "tiff";
        case IMAGE_FORMAT_WEBP: return "webp";
        case IMAGE_FORMAT_BMP: return "bmp";
        default: return "unknown";
    }
}

const char *image_format_mime(ImageFormat format) {
    switch (format) {
        case IMAGE_FORMAT_PNG: return "image/png";
        case IMAGE_FORMAT_JPEG: return "image/jpeg";
        case IMAGE_FORMAT_GIF: return "image/gif";
        case IMAGE_FORMAT_TIFF: return "image/tiff";
        case IMAGE_FORMAT_WEBP: return "image/webp";
        case IMAGE_FORMAT_BMP: return "image/bmp";
        default: return "application/octet-stream";
    }
}

const char *image_channel_description(const ImageInfo *info) {
    if (info == 0 || (info->flags & IMAGE_INFO_HAS_CHANNELS) == 0U) {
        return "unknown";
    }
    if (info->format == IMAGE_FORMAT_PNG && info->channel_count == 1U) {
        return "gray/indexed";
    }
    if (info->channel_count == 1U) {
        return "gray";
    }
    if (info->channel_count == 2U) {
        return "gray-alpha";
    }
    if (info->channel_count == 3U) {
        return "rgb";
    }
    if (info->channel_count == 4U) {
        return "rgba";
    }
    return "multi-channel";
}
