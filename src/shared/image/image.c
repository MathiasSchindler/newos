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
    info->orientation = 0U;
    info->density_x = 0U;
    info->density_y = 0U;
    info->property_flags = 0U;
    info->variant = 0;
    info->color_model = 0;
    info->compression = 0;
    info->density_unit = 0;
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

static void set_frames(ImageInfo *info, unsigned int frame_count) {
    if (frame_count != 0U) {
        info->frame_count = frame_count;
        info->flags |= IMAGE_INFO_HAS_FRAMES;
        if (frame_count > 1U) {
            info->property_flags |= IMAGE_PROPERTY_ANIMATED;
        }
    }
}

static void set_variant(ImageInfo *info, const char *variant) {
    if (variant != 0 && variant[0] != '\0') {
        info->variant = variant;
        info->flags |= IMAGE_INFO_HAS_VARIANT;
    }
}

static void set_color_model(ImageInfo *info, const char *color_model) {
    if (color_model != 0 && color_model[0] != '\0') {
        info->color_model = color_model;
        info->flags |= IMAGE_INFO_HAS_COLOR;
    }
}

static void set_compression(ImageInfo *info, const char *compression) {
    if (compression != 0 && compression[0] != '\0') {
        info->compression = compression;
        info->flags |= IMAGE_INFO_HAS_COMPRESSION;
    }
}

static void set_orientation(ImageInfo *info, unsigned int orientation) {
    if (orientation >= 1U && orientation <= 8U) {
        info->orientation = orientation;
        info->flags |= IMAGE_INFO_HAS_ORIENTATION;
        if (orientation != 1U) {
            info->property_flags |= IMAGE_PROPERTY_ORIENTATION;
        }
    }
}

static void set_density(ImageInfo *info, unsigned int density_x, unsigned int density_y, const char *unit) {
    if (density_x != 0U && density_y != 0U && unit != 0) {
        info->density_x = density_x;
        info->density_y = density_y;
        info->density_unit = unit;
        info->flags |= IMAGE_INFO_HAS_DENSITY;
    }
}

static int tiff_extract_metadata(const unsigned char *data,
                                 size_t size,
                                 unsigned int *width_out,
                                 unsigned int *height_out,
                                 unsigned int *bit_depth_out,
                                 unsigned int *channels_out,
                                 unsigned int *compression_out,
                                 unsigned int *photometric_out,
                                 int *has_photometric_out,
                                 unsigned int *orientation_out);

static size_t chunk_payload_end(size_t offset, unsigned int length, size_t size) {
    if ((size_t)length > size - offset) {
        return size;
    }
    return offset + (size_t)length;
}

static int probe_png(const unsigned char *data, size_t size, ImageInfo *info) {
    static const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    unsigned char color_type;
    unsigned int channels = 0U;
    size_t offset;

    if (size < 8U || !byte_arrays_equal(data, signature, sizeof(signature))) {
        return 0;
    }

    info->format = IMAGE_FORMAT_PNG;
    set_variant(info, "PNG");
    set_compression(info, "deflate");
    if (size >= 33U && read_u32_be(data + 8) == 13U && bytes_equal(data + 12, "IHDR", 4U)) {
        set_dimensions(info, read_u32_be(data + 16), read_u32_be(data + 20));
        set_bit_depth(info, data[24]);
        color_type = data[25];
        if (color_type == 0U) {
            channels = 1U;
            set_color_model(info, "grayscale");
        } else if (color_type == 2U) {
            channels = 3U;
            set_color_model(info, "truecolor");
        } else if (color_type == 3U) {
            channels = 1U;
            info->property_flags |= IMAGE_PROPERTY_PALETTE;
            set_color_model(info, "indexed-color");
        } else if (color_type == 4U) {
            channels = 2U;
            info->property_flags |= IMAGE_PROPERTY_ALPHA;
            set_color_model(info, "grayscale-alpha");
        } else if (color_type == 6U) {
            channels = 4U;
            info->property_flags |= IMAGE_PROPERTY_ALPHA;
            set_color_model(info, "truecolor-alpha");
        }
        set_channels(info, channels);
        if (data[28] != 0U) {
            info->property_flags |= IMAGE_PROPERTY_INTERLACED;
        }
    }
    offset = 8U;
    while (offset + 12U <= size) {
        unsigned int length = read_u32_be(data + offset);
        const unsigned char *type = data + offset + 4U;
        size_t payload = offset + 8U;

        if (bytes_equal(type, "tRNS", 4U)) {
            info->property_flags |= IMAGE_PROPERTY_ALPHA;
        } else if (bytes_equal(type, "iCCP", 4U)) {
            info->property_flags |= IMAGE_PROPERTY_ICC;
        } else if (bytes_equal(type, "eXIf", 4U)) {
            info->property_flags |= IMAGE_PROPERTY_EXIF;
            if (payload < size) {
                unsigned int orientation = 0U;
                tiff_extract_metadata(data + payload, chunk_payload_end(payload, length, size) - payload,
                                      0, 0, 0, 0, 0, 0, 0, &orientation);
                set_orientation(info, orientation);
            }
        } else if (bytes_equal(type, "pHYs", 4U) && length >= 9U && payload + 9U <= size) {
            set_density(info, read_u32_be(data + payload), read_u32_be(data + payload + 4U),
                        data[payload + 8U] == 1U ? "pixels/meter" : "pixels/unit");
        } else if (bytes_equal(type, "acTL", 4U)) {
            if (payload + 8U <= chunk_payload_end(payload, length, size)) {
                set_frames(info, read_u32_be(data + payload));
                if (read_u32_be(data + payload + 4U) != 0U) {
                    info->property_flags |= IMAGE_PROPERTY_LOOPING;
                }
            }
        } else if (bytes_equal(type, "IEND", 4U)) {
            break;
        }
        if ((size_t)length > size - offset - 12U) {
            break;
        }
        offset += 12U + (size_t)length;
    }
    return 1;
}

static int probe_gif(const unsigned char *data, size_t size, ImageInfo *info) {
    size_t offset;
    unsigned int frames = 0U;

    if (size < 13U || !bytes_equal(data, "GIF", 3U) || data[3] != '8' || (data[4] != '7' && data[4] != '9') || data[5] != 'a') {
        return 0;
    }

    info->format = IMAGE_FORMAT_GIF;
    set_variant(info, data[4] == '9' ? "GIF89a" : "GIF87a");
    set_compression(info, "LZW");
    set_dimensions(info, read_u16_le(data + 6), read_u16_le(data + 8));
    set_bit_depth(info, (unsigned int)((data[10] & 0x07U) + 1U));
    set_channels(info, 3U);
    set_color_model(info, "indexed-color");
    info->property_flags |= IMAGE_PROPERTY_PALETTE;

    offset = 13U;
    if ((data[10] & 0x80U) != 0U) {
        offset += (size_t)3U << ((data[10] & 0x07U) + 1U);
    }
    while (offset < size) {
        unsigned char block = data[offset++];

        if (block == 0x2cU) {
            if (offset + 9U > size) {
                break;
            }
            frames += 1U;
            if ((data[offset + 8U] & 0x40U) != 0U) {
                info->property_flags |= IMAGE_PROPERTY_INTERLACED;
            }
            offset += 9U;
            if (offset > size) {
                break;
            }
            if ((data[offset - 1U] & 0x80U) != 0U) {
                offset += (size_t)3U << ((data[offset - 1U] & 0x07U) + 1U);
            }
            if (offset >= size) {
                break;
            }
            offset += 1U;
            while (offset < size) {
                unsigned int sub_size = data[offset++];
                if (sub_size == 0U) {
                    break;
                }
                if ((size_t)sub_size > size - offset) {
                    offset = size;
                    break;
                }
                offset += sub_size;
            }
        } else if (block == 0x21U) {
            unsigned char label;
            if (offset >= size) {
                break;
            }
            label = data[offset++];
            if (label == 0xf9U && offset + 5U <= size && data[offset] == 4U) {
                if ((data[offset + 1U] & 0x01U) != 0U) {
                    info->property_flags |= IMAGE_PROPERTY_ALPHA;
                }
            } else if (label == 0xffU && offset + 12U <= size && data[offset] == 11U &&
                       bytes_equal((const unsigned char *)(data + offset + 1U), "NETSCAPE2.0", 11U)) {
                info->property_flags |= IMAGE_PROPERTY_LOOPING;
            }
            while (offset < size) {
                unsigned int sub_size = data[offset++];
                if (sub_size == 0U) {
                    break;
                }
                if ((size_t)sub_size > size - offset) {
                    offset = size;
                    break;
                }
                offset += sub_size;
            }
        } else if (block == 0x3bU) {
            break;
        } else {
            break;
        }
    }
    set_frames(info, frames);
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
    set_compression(info, "JPEG DCT");
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
                if (marker == 0xc0U) {
                    set_variant(info, "baseline DCT");
                } else if (marker == 0xc2U) {
                    set_variant(info, "progressive DCT");
                    info->property_flags |= IMAGE_PROPERTY_PROGRESSIVE;
                } else if (marker == 0xc1U) {
                    set_variant(info, "extended sequential DCT");
                } else if (marker == 0xc3U) {
                    set_variant(info, "lossless Huffman");
                    info->property_flags |= IMAGE_PROPERTY_LOSSLESS;
                } else {
                    set_variant(info, "JPEG");
                }
                if (data[offset + 7U] == 1U) {
                    set_color_model(info, "grayscale");
                } else if (data[offset + 7U] == 3U) {
                    set_color_model(info, "YCbCr/RGB");
                } else if (data[offset + 7U] == 4U) {
                    set_color_model(info, "CMYK/YCCK");
                }
            }
            break;
        }
        if (marker == 0xe1U && offset + 8U <= size) {
            if (bytes_equal(data + offset + 2U, "Exif", 4U)) {
                unsigned int orientation = 0U;
                info->property_flags |= IMAGE_PROPERTY_EXIF;
                if (offset + 8U < size) {
                    tiff_extract_metadata(data + offset + 8U, size - offset - 8U,
                                          0, 0, 0, 0, 0, 0, 0, &orientation);
                    set_orientation(info, orientation);
                }
            } else if (segment_size >= 31U && offset + 31U <= size &&
                       bytes_equal(data + offset + 2U, "http://ns.adobe.com/xap/1.0/", 29U)) {
                info->property_flags |= IMAGE_PROPERTY_XMP;
            }
        } else if (marker == 0xe2U && offset + 13U <= size && bytes_equal(data + offset + 2U, "ICC_PROFILE", 11U)) {
            info->property_flags |= IMAGE_PROPERTY_ICC;
        } else if (marker == 0xe0U && segment_size >= 16U && offset + 16U <= size &&
                   bytes_equal(data + offset + 2U, "JFIF", 4U) && data[offset + 6U] == 0U) {
            unsigned char units = data[offset + 9U];
            const char *unit_name = 0;

            if (units == 1U) {
                unit_name = "dpi";
            } else if (units == 2U) {
                unit_name = "dpcm";
            } else if (units == 0U) {
                unit_name = "pixel-aspect";
            }
            set_density(info, read_u16_be(data + offset + 10U), read_u16_be(data + offset + 12U), unit_name);
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

static int tiff_extract_metadata(const unsigned char *data,
                                 size_t size,
                                 unsigned int *width_out,
                                 unsigned int *height_out,
                                 unsigned int *bit_depth_out,
                                 unsigned int *channels_out,
                                 unsigned int *compression_out,
                                 unsigned int *photometric_out,
                                 int *has_photometric_out,
                                 unsigned int *orientation_out) {
    int little_endian;
    unsigned int ifd_offset;
    unsigned int entry_count;
    unsigned int i;

    if (width_out != 0) {
        *width_out = 0U;
    }
    if (height_out != 0) {
        *height_out = 0U;
    }
    if (bit_depth_out != 0) {
        *bit_depth_out = 0U;
    }
    if (channels_out != 0) {
        *channels_out = 0U;
    }
    if (compression_out != 0) {
        *compression_out = 0U;
    }
    if (photometric_out != 0) {
        *photometric_out = 0U;
    }
    if (has_photometric_out != 0) {
        *has_photometric_out = 0;
    }
    if (orientation_out != 0) {
        *orientation_out = 0U;
    }

    if (size < 8U) {
        return -1;
    }
    if (data[0] == 'I' && data[1] == 'I') {
        little_endian = 1;
    } else if (data[0] == 'M' && data[1] == 'M') {
        little_endian = 0;
    } else {
        return -1;
    }
    if (tiff_read_u16(data + 2U, little_endian) != 42U) {
        return -1;
    }

    ifd_offset = tiff_read_u32(data + 4U, little_endian);
    if ((size_t)ifd_offset + 2U > size) {
        return 0;
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
        if (tag == 256U && width_out != 0) {
            *width_out = value;
        } else if (tag == 257U && height_out != 0) {
            *height_out = value;
        } else if (tag == 258U && bit_depth_out != 0) {
            *bit_depth_out = value;
        } else if (tag == 259U && compression_out != 0) {
            *compression_out = value;
        } else if (tag == 262U && photometric_out != 0) {
            *photometric_out = value;
            if (has_photometric_out != 0) {
                *has_photometric_out = 1;
            }
        } else if (tag == 274U && orientation_out != 0) {
            *orientation_out = value;
        } else if (tag == 277U && channels_out != 0) {
            *channels_out = value;
        }
    }
    return 0;
}

static int probe_tiff(const unsigned char *data, size_t size, ImageInfo *info) {
    int little_endian;
    unsigned int width = 0U;
    unsigned int height = 0U;
    unsigned int bit_depth = 0U;
    unsigned int channels = 0U;
    unsigned int compression = 0U;
    unsigned int photometric = 0U;
    int has_photometric = 0;
    unsigned int orientation = 0U;

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
    set_variant(info, little_endian ? "classic TIFF little-endian" : "classic TIFF big-endian");
    tiff_extract_metadata(data, size, &width, &height, &bit_depth, &channels, &compression,
                          &photometric, &has_photometric, &orientation);
    set_dimensions(info, width, height);
    set_bit_depth(info, bit_depth);
    set_channels(info, channels);
    if (compression == 1U) {
        set_compression(info, "uncompressed");
    } else if (compression == 5U) {
        set_compression(info, "LZW");
    } else if (compression == 6U || compression == 7U) {
        set_compression(info, "JPEG");
    } else if (compression == 8U) {
        set_compression(info, "deflate");
    } else if (compression != 0U) {
        set_compression(info, "other");
    }
    if (has_photometric && photometric == 0U) {
        set_color_model(info, "white-is-zero grayscale");
    } else if (has_photometric && photometric == 1U) {
        set_color_model(info, "black-is-zero grayscale");
    } else if (has_photometric && photometric == 2U) {
        set_color_model(info, "rgb");
    } else if (has_photometric && photometric == 3U) {
        set_color_model(info, "palette");
        info->property_flags |= IMAGE_PROPERTY_PALETTE;
    } else if (has_photometric && photometric == 5U) {
        set_color_model(info, "cmyk");
    } else if (has_photometric && photometric == 6U) {
        set_color_model(info, "YCbCr");
    }
    set_orientation(info, orientation);
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
        set_variant(info, "extended WebP");
        set_compression(info, "VP8/VP8L");
        set_dimensions(info, read_u24_le(data + 24U) + 1U, read_u24_le(data + 27U) + 1U);
        set_channels(info, (data[20] & 0x10U) != 0U ? 4U : 3U);
        set_color_model(info, (data[20] & 0x10U) != 0U ? "rgba" : "rgb");
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
    } else if (bytes_equal(chunk, "VP8 ", 4U) && chunk_size >= 10U && size >= 30U &&
               data[23] == 0x9dU && data[24] == 0x01U && data[25] == 0x2aU) {
        set_variant(info, "lossy WebP");
        set_compression(info, "VP8");
        set_dimensions(info, read_u16_le(data + 26U) & 0x3fffU, read_u16_le(data + 28U) & 0x3fffU);
        set_channels(info, 3U);
        set_color_model(info, "rgb");
    } else if (bytes_equal(chunk, "VP8L", 4U) && chunk_size >= 5U && size >= 25U && data[20] == 0x2fU) {
        unsigned int packed = read_u32_le(data + 21U);
        set_variant(info, "lossless WebP");
        set_compression(info, "VP8L");
        info->property_flags |= IMAGE_PROPERTY_LOSSLESS | IMAGE_PROPERTY_ALPHA;
        set_dimensions(info, (packed & 0x3fffU) + 1U, ((packed >> 14) & 0x3fffU) + 1U);
        set_channels(info, 4U);
        set_color_model(info, "rgba");
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
        set_variant(info, "OS/2 bitmap core header");
        set_compression(info, "uncompressed");
        width = read_u16_le(data + 18U);
        height = read_u16_le(data + 20U);
        bits = read_u16_le(data + 24U);
        set_dimensions(info, width, height);
        set_bit_depth(info, bits);
        if (bits == 24U) {
            set_channels(info, 3U);
            set_color_model(info, "rgb");
        } else if (bits == 8U || bits == 4U || bits == 1U) {
            set_channels(info, 1U);
            set_color_model(info, "indexed-color");
            info->property_flags |= IMAGE_PROPERTY_PALETTE;
        }
    } else if (dib_size >= 40U && size >= 30U) {
        unsigned int compression = size >= 34U ? read_u32_le(data + 30U) : 0U;
        set_variant(info, dib_size == 40U ? "Windows bitmap info header" : "extended Windows bitmap header");
        width = read_u32_le(data + 18U);
        height = read_u32_le(data + 22U);
        if ((height & 0x80000000U) != 0U) {
            height = (unsigned int)(0U - height);
            info->property_flags |= IMAGE_PROPERTY_TOP_DOWN;
        }
        bits = read_u16_le(data + 28U);
        set_dimensions(info, width, height);
        set_bit_depth(info, bits);
        if (compression == 0U) {
            set_compression(info, "uncompressed");
        } else if (compression == 1U) {
            set_compression(info, "RLE8");
        } else if (compression == 2U) {
            set_compression(info, "RLE4");
        } else if (compression == 3U) {
            set_compression(info, "bitfields");
        } else if (compression == 4U) {
            set_compression(info, "JPEG");
        } else if (compression == 5U) {
            set_compression(info, "PNG");
        } else {
            set_compression(info, "other");
        }
        if (bits == 32U) {
            set_channels(info, 4U);
            set_color_model(info, "rgba");
            info->property_flags |= IMAGE_PROPERTY_ALPHA;
        } else if (bits == 24U) {
            set_channels(info, 3U);
            set_color_model(info, "rgb");
        } else if (bits == 8U || bits == 4U || bits == 1U) {
            set_channels(info, 1U);
            set_color_model(info, "indexed-color");
            info->property_flags |= IMAGE_PROPERTY_PALETTE;
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

const char *image_property_name(unsigned int property) {
    switch (property) {
        case IMAGE_PROPERTY_ALPHA: return "alpha";
        case IMAGE_PROPERTY_PALETTE: return "palette";
        case IMAGE_PROPERTY_INTERLACED: return "interlaced";
        case IMAGE_PROPERTY_ANIMATED: return "animated";
        case IMAGE_PROPERTY_PROGRESSIVE: return "progressive";
        case IMAGE_PROPERTY_LOSSLESS: return "lossless";
        case IMAGE_PROPERTY_EXIF: return "exif";
        case IMAGE_PROPERTY_ICC: return "icc-profile";
        case IMAGE_PROPERTY_XMP: return "xmp";
        case IMAGE_PROPERTY_TOP_DOWN: return "top-down";
        case IMAGE_PROPERTY_LOOPING: return "looping";
        case IMAGE_PROPERTY_ORIENTATION: return "orientation";
        default: return 0;
    }
}

const char *image_orientation_description(unsigned int orientation) {
    switch (orientation) {
        case 1U: return "normal";
        case 2U: return "mirrored horizontal";
        case 3U: return "rotated 180";
        case 4U: return "mirrored vertical";
        case 5U: return "mirrored horizontal then rotated 270";
        case 6U: return "rotated 90 clockwise";
        case 7U: return "mirrored horizontal then rotated 90";
        case 8U: return "rotated 270 clockwise";
        default: return "unknown";
    }
}
