#include "image_internal.h"

static void image_validation_set(ImageValidation *validation, ImageFormat format, int valid, const char *message) {
    if (validation == 0) {
        return;
    }
    validation->format = format;
    validation->valid = valid;
    validation->message = message;
}

static unsigned int png_crc32(const unsigned char *data, size_t size) {
    unsigned int crc = 0xffffffffU;
    size_t offset;

    for (offset = 0U; offset < size; ++offset) {
        unsigned int byte_index;

        crc ^= (unsigned int)data[offset];
        for (byte_index = 0U; byte_index < 8U; ++byte_index) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xedb88320U;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc ^ 0xffffffffU;
}

static int png_color_bit_depth_is_valid(unsigned char color_type, unsigned char bit_depth) {
    if (color_type == 0U) {
        return bit_depth == 1U || bit_depth == 2U || bit_depth == 4U || bit_depth == 8U || bit_depth == 16U;
    }
    if (color_type == 2U) {
        return bit_depth == 8U || bit_depth == 16U;
    }
    if (color_type == 3U) {
        return bit_depth == 1U || bit_depth == 2U || bit_depth == 4U || bit_depth == 8U;
    }
    if (color_type == 4U) {
        return bit_depth == 8U || bit_depth == 16U;
    }
    if (color_type == 6U) {
        return bit_depth == 8U || bit_depth == 16U;
    }
    return 0;
}

int image_validate_png(const unsigned char *data, size_t size, ImageValidation *validation) {
    static const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    size_t offset;
    int seen_ihdr = 0;
    int seen_plte = 0;
    int seen_idat = 0;
    int seen_non_idat_after_idat = 0;
    int seen_iend = 0;
    unsigned char color_type = 0U;

    if (size < 8U || !image_byte_arrays_equal(data, signature, sizeof(signature))) {
        image_validation_set(validation, IMAGE_FORMAT_UNKNOWN, 0, "not a PNG image");
        return -1;
    }
    image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "truncated PNG stream");
    offset = 8U;
    while (offset + 12U <= size) {
        unsigned int length = image_read_u32_be(data + offset);
        const unsigned char *type = data + offset + 4U;
        size_t payload = offset + 8U;
        size_t crc_offset;
        unsigned int expected_crc;
        unsigned int actual_crc;

        if ((size_t)length > size - offset - 12U) {
            image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "chunk length exceeds file size");
            return -1;
        }
        crc_offset = payload + (size_t)length;
        expected_crc = image_read_u32_be(data + crc_offset);
        actual_crc = png_crc32(type, (size_t)length + 4U);
        if (actual_crc != expected_crc) {
            image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "chunk CRC mismatch");
            return -1;
        }
        if (!seen_ihdr) {
            if (!image_bytes_equal(type, "IHDR", 4U)) {
                image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "first chunk is not IHDR");
                return -1;
            }
            if (length != 13U) {
                image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "IHDR chunk has invalid length");
                return -1;
            }
            if (image_read_u32_be(data + payload) == 0U || image_read_u32_be(data + payload + 4U) == 0U) {
                image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "IHDR dimensions must be nonzero");
                return -1;
            }
            color_type = data[payload + 9U];
            if (!png_color_bit_depth_is_valid(color_type, data[payload + 8U])) {
                image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "invalid PNG color type and bit depth combination");
                return -1;
            }
            if (data[payload + 10U] != 0U || data[payload + 11U] != 0U || data[payload + 12U] > 1U) {
                image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "IHDR compression filter or interlace method is invalid");
                return -1;
            }
            seen_ihdr = 1;
        } else if (image_bytes_equal(type, "IHDR", 4U)) {
            image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "duplicate IHDR chunk");
            return -1;
        } else if (image_bytes_equal(type, "PLTE", 4U)) {
            if (seen_idat) {
                image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "PLTE chunk appears after IDAT");
                return -1;
            }
            if (seen_plte) {
                image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "duplicate PLTE chunk");
                return -1;
            }
            if (length == 0U || (length % 3U) != 0U || length > 768U) {
                image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "PLTE chunk has invalid length");
                return -1;
            }
            seen_plte = 1;
        } else if (image_bytes_equal(type, "IDAT", 4U)) {
            if (seen_non_idat_after_idat) {
                image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "IDAT chunks are not consecutive");
                return -1;
            }
            seen_idat = 1;
        } else if (image_bytes_equal(type, "tRNS", 4U)) {
            if (seen_idat) {
                image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "tRNS chunk appears after IDAT");
                return -1;
            }
            if (color_type == 3U && !seen_plte) {
                image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "indexed tRNS chunk appears before PLTE");
                return -1;
            }
        } else if (image_bytes_equal(type, "IEND", 4U)) {
            if (length != 0U) {
                image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "IEND chunk has invalid length");
                return -1;
            }
            seen_iend = 1;
            offset = crc_offset + 4U;
            break;
        } else if (seen_idat) {
            seen_non_idat_after_idat = 1;
        }
        offset = crc_offset + 4U;
    }
    if (!seen_ihdr) {
        image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "missing IHDR chunk");
        return -1;
    }
    if (color_type == 3U && !seen_plte) {
        image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "indexed PNG is missing PLTE");
        return -1;
    }
    if (!seen_idat) {
        image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "missing IDAT chunk");
        return -1;
    }
    if (!seen_iend) {
        image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "missing IEND chunk");
        return -1;
    }
    if (offset != size) {
        image_validation_set(validation, IMAGE_FORMAT_PNG, 0, "trailing data after IEND");
        return -1;
    }
    image_validation_set(validation, IMAGE_FORMAT_PNG, 1, "valid PNG image");
    return 0;
}

static int gif_color_table_size(unsigned char packed, size_t *size_out) {
    *size_out = (size_t)3U << ((packed & 0x07U) + 1U);
    return 0;
}

static int gif_skip_sub_blocks(const unsigned char *data, size_t size, size_t *offset_io) {
    size_t offset = *offset_io;

    while (offset < size) {
        unsigned int block_size = data[offset++];

        if (block_size == 0U) {
            *offset_io = offset;
            return 0;
        }
        if ((size_t)block_size > size - offset) {
            return -1;
        }
        offset += (size_t)block_size;
    }
    return -1;
}

int image_validate_gif(const unsigned char *data, size_t size, ImageValidation *validation) {
    size_t offset;
    size_t table_size;
    int saw_trailer = 0;
    unsigned int frame_count = 0U;

    if (size < 6U || !image_bytes_equal(data, "GIF", 3U) || data[3] != '8' ||
        (data[4] != '7' && data[4] != '9') || data[5] != 'a') {
        image_validation_set(validation, IMAGE_FORMAT_UNKNOWN, 0, "not a GIF image");
        return -1;
    }
    image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "truncated GIF stream");
    if (size < 13U) {
        return -1;
    }
    if (image_read_u16_le(data + 6U) == 0U || image_read_u16_le(data + 8U) == 0U) {
        image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "logical screen dimensions must be nonzero");
        return -1;
    }
    offset = 13U;
    if ((data[10] & 0x80U) != 0U) {
        gif_color_table_size(data[10], &table_size);
        if (table_size > size - offset) {
            image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "global color table is truncated");
            return -1;
        }
        offset += table_size;
    }
    while (offset < size) {
        unsigned char block = data[offset++];

        if (block == 0x3bU) {
            saw_trailer = 1;
            break;
        }
        if (block == 0x2cU) {
            unsigned char packed;

            if (offset + 9U > size) {
                image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "image descriptor is truncated");
                return -1;
            }
            if (image_read_u16_le(data + offset + 4U) == 0U || image_read_u16_le(data + offset + 6U) == 0U) {
                image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "image dimensions must be nonzero");
                return -1;
            }
            packed = data[offset + 8U];
            offset += 9U;
            if ((packed & 0x80U) != 0U) {
                gif_color_table_size(packed, &table_size);
                if (table_size > size - offset) {
                    image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "local color table is truncated");
                    return -1;
                }
                offset += table_size;
            }
            if (offset >= size) {
                image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "missing LZW minimum code size");
                return -1;
            }
            if (data[offset] < 2U || data[offset] > 12U) {
                image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "invalid LZW minimum code size");
                return -1;
            }
            offset += 1U;
            if (gif_skip_sub_blocks(data, size, &offset) != 0) {
                image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "image data sub-blocks are truncated");
                return -1;
            }
            frame_count += 1U;
        } else if (block == 0x21U) {
            unsigned char label;

            if (offset >= size) {
                image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "extension block is truncated");
                return -1;
            }
            label = data[offset++];
            if (label == 0xf9U) {
                if (offset + 6U > size || data[offset] != 4U || data[offset + 5U] != 0U) {
                    image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "graphic control extension is invalid");
                    return -1;
                }
                offset += 6U;
            } else if (gif_skip_sub_blocks(data, size, &offset) != 0) {
                image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "extension sub-blocks are truncated");
                return -1;
            }
        } else {
            image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "unknown GIF block type");
            return -1;
        }
    }
    if (!saw_trailer) {
        image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "missing GIF trailer");
        return -1;
    }
    if (offset != size) {
        image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "trailing data after GIF trailer");
        return -1;
    }
    if (frame_count == 0U) {
        image_validation_set(validation, IMAGE_FORMAT_GIF, 0, "missing GIF image frame");
        return -1;
    }
    image_validation_set(validation, IMAGE_FORMAT_GIF, 1, "valid GIF image");
    return 0;
}

static int bmp_bit_depth_is_valid(unsigned int bits) {
    return bits == 1U || bits == 4U || bits == 8U || bits == 16U || bits == 24U || bits == 32U;
}

static int bmp_signed_u32_is_zero(unsigned int value) {
    return value == 0U;
}

static unsigned int bmp_abs_i32_as_u32(unsigned int value) {
    if ((value & 0x80000000U) == 0U) {
        return value;
    }
    return 0U - value;
}

static int bmp_row_stride(unsigned int width, unsigned int bits, size_t *stride_out) {
    size_t width_size = (size_t)width;
    size_t bits_size = (size_t)bits;
    size_t row_bits;

    if (width_size != 0U && bits_size > (((size_t)-1) - 31U) / width_size) {
        return -1;
    }
    row_bits = width_size * bits_size;
    *stride_out = ((row_bits + 31U) / 32U) * 4U;
    return 0;
}

static int bmp_validate_pixel_span(unsigned int width,
                                   unsigned int height,
                                   unsigned int bits,
                                   size_t pixel_offset,
                                   size_t size,
                                   ImageValidation *validation) {
    size_t stride;
    size_t needed;

    if (bmp_row_stride(width, bits, &stride) != 0 || height > ((size_t)-1) / stride) {
        image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP pixel array size overflows");
        return -1;
    }
    needed = stride * (size_t)height;
    if (pixel_offset > size || needed > size - pixel_offset) {
        image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP pixel array is truncated");
        return -1;
    }
    return 0;
}

int image_validate_bmp(const unsigned char *data, size_t size, ImageValidation *validation) {
    unsigned int file_size;
    unsigned int pixel_offset;
    unsigned int dib_size;
    unsigned int bits;
    unsigned int width;
    unsigned int height;
    size_t min_pixel_offset;
    size_t color_table_entries = 0U;
    size_t color_table_bytes = 0U;

    if (size < 2U || data[0] != 'B' || data[1] != 'M') {
        image_validation_set(validation, IMAGE_FORMAT_UNKNOWN, 0, "not a BMP image");
        return -1;
    }
    image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "truncated BMP stream");
    if (size < 26U) {
        return -1;
    }
    file_size = image_read_u32_le(data + 2U);
    pixel_offset = image_read_u32_le(data + 10U);
    dib_size = image_read_u32_le(data + 14U);
    if (file_size != 0U && (size_t)file_size != size) {
        image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP file size field does not match input size");
        return -1;
    }
    if (dib_size == 12U) {
        unsigned int planes;

        if (size < 26U) {
            return -1;
        }
        width = image_read_u16_le(data + 18U);
        height = image_read_u16_le(data + 20U);
        planes = image_read_u16_le(data + 22U);
        bits = image_read_u16_le(data + 24U);
        if (width == 0U || height == 0U) {
            image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP dimensions must be nonzero");
            return -1;
        }
        if (planes != 1U) {
            image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP plane count must be one");
            return -1;
        }
        if (!bmp_bit_depth_is_valid(bits)) {
            image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP bit depth is unsupported");
            return -1;
        }
        if (bits <= 8U) {
            color_table_entries = (size_t)1U << bits;
            color_table_bytes = color_table_entries * 3U;
        }
        min_pixel_offset = 14U + 12U + color_table_bytes;
        if ((size_t)pixel_offset < min_pixel_offset || (size_t)pixel_offset > size) {
            image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP pixel offset is invalid");
            return -1;
        }
        if (bmp_validate_pixel_span(width, height, bits, (size_t)pixel_offset, size, validation) != 0) {
            return -1;
        }
    } else if (dib_size >= 40U) {
        unsigned int planes;
        unsigned int compression;
        unsigned int image_size;
        unsigned int colors_used = 0U;
        size_t masks_size = 0U;

        if (dib_size > size - 14U || size < 54U) {
            image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP DIB header is truncated");
            return -1;
        }
        width = image_read_u32_le(data + 18U);
        height = image_read_u32_le(data + 22U);
        planes = image_read_u16_le(data + 26U);
        bits = image_read_u16_le(data + 28U);
        compression = image_read_u32_le(data + 30U);
        image_size = image_read_u32_le(data + 34U);
        if (dib_size >= 40U) {
            colors_used = image_read_u32_le(data + 46U);
        }
        if (bmp_signed_u32_is_zero(width) || bmp_signed_u32_is_zero(height)) {
            image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP dimensions must be nonzero");
            return -1;
        }
        width = bmp_abs_i32_as_u32(width);
        height = bmp_abs_i32_as_u32(height);
        if (planes != 1U) {
            image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP plane count must be one");
            return -1;
        }
        if (!bmp_bit_depth_is_valid(bits)) {
            image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP bit depth is unsupported");
            return -1;
        }
        if ((compression == 1U && bits != 8U) || (compression == 2U && bits != 4U) ||
            (compression == 3U && bits != 16U && bits != 32U) || compression > 5U) {
            image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP compression is incompatible with bit depth");
            return -1;
        }
        if (compression == 3U && dib_size == 40U) {
            masks_size = 12U;
        }
        if (bits <= 8U) {
            size_t maximum_entries = (size_t)1U << bits;

            color_table_entries = colors_used == 0U ? maximum_entries : (size_t)colors_used;
            if (color_table_entries > maximum_entries) {
                image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP color table is too large for bit depth");
                return -1;
            }
            color_table_bytes = color_table_entries * 4U;
        }
        min_pixel_offset = 14U + (size_t)dib_size + masks_size + color_table_bytes;
        if ((size_t)pixel_offset < min_pixel_offset || (size_t)pixel_offset > size) {
            image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP pixel offset is invalid");
            return -1;
        }
        if (compression == 0U || compression == 3U) {
            if (bmp_validate_pixel_span(width, height, bits, (size_t)pixel_offset, size, validation) != 0) {
                return -1;
            }
        } else if (image_size != 0U && ((size_t)pixel_offset > size || (size_t)image_size > size - (size_t)pixel_offset)) {
            image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP compressed image data is truncated");
            return -1;
        }
    } else {
        image_validation_set(validation, IMAGE_FORMAT_BMP, 0, "BMP DIB header size is unsupported");
        return -1;
    }
    image_validation_set(validation, IMAGE_FORMAT_BMP, 1, "valid BMP image");
    return 0;
}
