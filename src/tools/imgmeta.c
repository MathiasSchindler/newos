#include "image/image.h"
#include "compression/crc32.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define IMGMETA_INITIAL_CAPACITY (64U * 1024U)

static void print_usage(void) {
    tool_write_usage("imgmeta", "show FILE ... | list-text FILE ... | strip -o OUTPUT FILE | copy -o OUTPUT FILE | edit [--set-text KEY=VALUE|--remove-text KEY] -o OUTPUT FILE");
}

static unsigned int read_u16_be(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 8) | (unsigned int)bytes[1];
}

static unsigned int read_u32_be(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 24) |
           ((unsigned int)bytes[1] << 16) |
           ((unsigned int)bytes[2] << 8) |
           (unsigned int)bytes[3];
}

static unsigned int read_u32_le(const unsigned char *bytes) {
    return (unsigned int)bytes[0] |
           ((unsigned int)bytes[1] << 8) |
           ((unsigned int)bytes[2] << 16) |
           ((unsigned int)bytes[3] << 24);
}

static void write_u32_le(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24) & 0xffU);
}

static void write_u32_be(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)((value >> 24) & 0xffU);
    bytes[1] = (unsigned char)((value >> 16) & 0xffU);
    bytes[2] = (unsigned char)((value >> 8) & 0xffU);
    bytes[3] = (unsigned char)(value & 0xffU);
}

static int bytes_equal(const unsigned char *bytes, const char *text, size_t length) {
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != (unsigned char)text[index]) {
            return 0;
        }
    }
    return 1;
}

static int read_all_input(const char *path, unsigned char **data_out, size_t *size_out) {
    int fd;
    int should_close;
    unsigned char *buffer;
    size_t capacity = IMGMETA_INITIAL_CAPACITY;
    size_t used = 0U;

    *data_out = 0;
    *size_out = 0U;
    if (tool_open_input(path, &fd, &should_close) != 0) {
        tool_write_error("imgmeta", "cannot open: ", path ? path : "stdin");
        return -1;
    }
    buffer = (unsigned char *)rt_malloc(capacity);
    if (buffer == 0) {
        tool_close_input(fd, should_close);
        tool_write_error("imgmeta", "out of memory: ", path ? path : "stdin");
        return -1;
    }
    while (1) {
        long bytes_read;

        if (used == capacity) {
            unsigned char *resized;
            size_t next_capacity = capacity * 2U;

            if (next_capacity <= capacity) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                tool_write_error("imgmeta", "input too large: ", path ? path : "stdin");
                return -1;
            }
            resized = (unsigned char *)rt_realloc(buffer, next_capacity);
            if (resized == 0) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                tool_write_error("imgmeta", "out of memory: ", path ? path : "stdin");
                return -1;
            }
            buffer = resized;
            capacity = next_capacity;
        }
        bytes_read = platform_read(fd, buffer + used, capacity - used);
        if (bytes_read < 0) {
            rt_free(buffer);
            tool_close_input(fd, should_close);
            tool_write_error("imgmeta", "read failed: ", path ? path : "stdin");
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }
        used += (size_t)bytes_read;
    }
    tool_close_input(fd, should_close);
    *data_out = buffer;
    *size_out = used;
    return 0;
}

static int write_file(const char *path, const unsigned char *data, size_t size) {
    int fd = platform_open_write(path, 0644U);

    if (fd < 0) {
        tool_write_error("imgmeta", "cannot write: ", path);
        return -1;
    }
    if (rt_write_all(fd, data, size) != 0) {
        platform_close(fd);
        tool_write_error("imgmeta", "write failed: ", path);
        return -1;
    }
    platform_close(fd);
    return 0;
}

static void write_property_list(unsigned int property_flags) {
    static const unsigned int properties[] = {
        IMAGE_PROPERTY_EXIF,
        IMAGE_PROPERTY_ICC,
        IMAGE_PROPERTY_XMP,
        IMAGE_PROPERTY_ORIENTATION,
        IMAGE_PROPERTY_ALPHA,
        IMAGE_PROPERTY_PALETTE,
        IMAGE_PROPERTY_ANIMATED,
        IMAGE_PROPERTY_LOOPING
    };
    size_t index;
    int wrote = 0;

    for (index = 0U; index < sizeof(properties) / sizeof(properties[0]); ++index) {
        if ((property_flags & properties[index]) != 0U) {
            const char *name = image_property_name(properties[index]);
            if (name == 0) {
                continue;
            }
            if (wrote) {
                rt_write_cstr(1, ", ");
            }
            rt_write_cstr(1, name);
            wrote = 1;
        }
    }
    if (!wrote) {
        rt_write_char(1, '-');
    }
}

static int show_path(const char *path) {
    unsigned char *data;
    size_t size;
    ImageInfo info;
    const char *label = path ? path : "stdin";

    if (read_all_input(path, &data, &size) != 0) {
        return -1;
    }
    if (image_probe(data, size, &info) != 0) {
        rt_free(data);
        tool_write_error("imgmeta", "unsupported image format: ", label);
        return -1;
    }
    rt_free(data);
    rt_write_cstr(1, label);
    rt_write_line(1, ":");
    rt_write_cstr(1, "  format: ");
    rt_write_line(1, image_format_name(info.format));
    rt_write_cstr(1, "  metadata: ");
    write_property_list(info.property_flags);
    rt_write_char(1, '\n');
    if ((info.flags & IMAGE_INFO_HAS_ORIENTATION) != 0U) {
        rt_write_cstr(1, "  orientation: ");
        rt_write_uint(1, (unsigned long long)info.orientation);
        rt_write_cstr(1, " (");
        rt_write_cstr(1, image_orientation_description(info.orientation));
        rt_write_line(1, ")");
    }
    if ((info.flags & IMAGE_INFO_HAS_DENSITY) != 0U) {
        rt_write_cstr(1, "  density: ");
        rt_write_uint(1, (unsigned long long)info.density_x);
        rt_write_char(1, 'x');
        rt_write_uint(1, (unsigned long long)info.density_y);
        rt_write_char(1, ' ');
        rt_write_line(1, info.density_unit);
    }
    return 0;
}

static int png_chunk_is_metadata(const unsigned char *type) {
    return bytes_equal(type, "eXIf", 4U) ||
           bytes_equal(type, "iCCP", 4U) ||
           bytes_equal(type, "iTXt", 4U) ||
           bytes_equal(type, "tEXt", 4U) ||
           bytes_equal(type, "zTXt", 4U) ||
           bytes_equal(type, "tIME", 4U);
}

static int strip_png(const unsigned char *data, size_t size, unsigned char **out_data, size_t *out_size) {
    static const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    unsigned char *output;
    size_t input_offset = 8U;
    size_t output_size = 0U;

    if (size < 8U || !bytes_equal(data, (const char *)signature, sizeof(signature))) {
        return -1;
    }
    output = (unsigned char *)rt_malloc(size == 0U ? 1U : size);
    if (output == 0) {
        return -1;
    }
    memcpy(output, data, 8U);
    output_size = 8U;
    while (input_offset + 12U <= size) {
        unsigned int length = read_u32_be(data + input_offset);
        const unsigned char *type = data + input_offset + 4U;
        size_t chunk_size;

        if ((size_t)length > size - input_offset - 12U) {
            rt_free(output);
            return -1;
        }
        chunk_size = (size_t)length + 12U;
        if (!png_chunk_is_metadata(type)) {
            memcpy(output + output_size, data + input_offset, chunk_size);
            output_size += chunk_size;
        }
        input_offset += chunk_size;
        if (bytes_equal(type, "IEND", 4U)) {
            break;
        }
    }
    *out_data = output;
    *out_size = output_size;
    return 0;
}

static int png_text_key_matches(const unsigned char *payload, unsigned int length, const char *key, size_t key_length) {
    if ((size_t)length == key_length) {
        return bytes_equal(payload, key, key_length);
    }
    if ((size_t)length < key_length + 1U) {
        return 0;
    }
    if (payload[key_length] != 0U) {
        return 0;
    }
    return bytes_equal(payload, key, key_length);
}

static size_t png_text_key_length(const unsigned char *payload, unsigned int length) {
    size_t index;

    for (index = 0U; index < (size_t)length; ++index) {
        if (payload[index] == 0U) {
            return index;
        }
    }
    return (size_t)length;
}

static int png_text_key_is_valid(const char *key, size_t key_length) {
    size_t index;

    if (key_length == 0U || key_length > 79U) {
        return 0;
    }
    for (index = 0U; index < key_length; ++index) {
        unsigned char ch = (unsigned char)key[index];

        if (ch == 0U || ch < 32U || ch > 126U) {
            return 0;
        }
    }
    return 1;
}

static void write_png_text_chunk(unsigned char *output, size_t offset, const char *key, size_t key_length, const char *value, size_t value_length) {
    unsigned int crc;
    unsigned int payload_length = (unsigned int)(key_length + 1U + value_length);

    write_u32_be(output + offset, payload_length);
    memcpy(output + offset + 4U, "tEXt", 4U);
    memcpy(output + offset + 8U, key, key_length);
    output[offset + 8U + key_length] = 0U;
    memcpy(output + offset + 9U + key_length, value, value_length);
    crc = compression_crc32(output + offset + 4U, (size_t)payload_length + 4U);
    write_u32_be(output + offset + 8U + payload_length, crc);
}

static int edit_png_text(const unsigned char *data,
                         size_t size,
                         const char *key,
                         size_t key_length,
                         const char *value,
                         size_t value_length,
                         unsigned char **out_data,
                         size_t *out_size) {
    static const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    unsigned char *output;
    size_t input_offset = 8U;
    size_t output_size = 8U;
    size_t text_chunk_size = key_length + 1U + value_length + 12U;
    int wrote_text = 0;

    if (size < 8U || !bytes_equal(data, (const char *)signature, sizeof(signature)) || !png_text_key_is_valid(key, key_length)) {
        return -1;
    }
    if (text_chunk_size > 0xffffffffU || size > ((size_t)-1) - text_chunk_size) {
        return -1;
    }
    output = (unsigned char *)rt_malloc(size + text_chunk_size);
    if (output == 0) {
        return -1;
    }
    memcpy(output, data, 8U);
    while (input_offset + 12U <= size) {
        unsigned int length = read_u32_be(data + input_offset);
        const unsigned char *type = data + input_offset + 4U;
        const unsigned char *payload = data + input_offset + 8U;
        size_t chunk_size;

        if ((size_t)length > size - input_offset - 12U) {
            rt_free(output);
            return -1;
        }
        chunk_size = (size_t)length + 12U;
        if (!wrote_text && (bytes_equal(type, "IDAT", 4U) || bytes_equal(type, "IEND", 4U))) {
            write_png_text_chunk(output, output_size, key, key_length, value, value_length);
            output_size += text_chunk_size;
            wrote_text = 1;
        }
        if (bytes_equal(type, "tEXt", 4U) && png_text_key_matches(payload, length, key, key_length)) {
            if (!wrote_text) {
                write_png_text_chunk(output, output_size, key, key_length, value, value_length);
                output_size += text_chunk_size;
                wrote_text = 1;
            }
        } else {
            memcpy(output + output_size, data + input_offset, chunk_size);
            output_size += chunk_size;
        }
        input_offset += chunk_size;
        if (bytes_equal(type, "IEND", 4U)) {
            break;
        }
    }
    if (!wrote_text || input_offset > size) {
        rt_free(output);
        return -1;
    }
    *out_data = output;
    *out_size = output_size;
    return 0;
}

static int remove_png_text(const unsigned char *data,
                           size_t size,
                           const char *key,
                           size_t key_length,
                           unsigned char **out_data,
                           size_t *out_size) {
    static const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    unsigned char *output;
    size_t input_offset = 8U;
    size_t output_size = 8U;
    int removed = 0;

    if (size < 8U || !bytes_equal(data, (const char *)signature, sizeof(signature)) || !png_text_key_is_valid(key, key_length)) {
        return -1;
    }
    output = (unsigned char *)rt_malloc(size == 0U ? 1U : size);
    if (output == 0) {
        return -1;
    }
    memcpy(output, data, 8U);
    while (input_offset + 12U <= size) {
        unsigned int length = read_u32_be(data + input_offset);
        const unsigned char *type = data + input_offset + 4U;
        const unsigned char *payload = data + input_offset + 8U;
        size_t chunk_size;

        if ((size_t)length > size - input_offset - 12U) {
            rt_free(output);
            return -1;
        }
        chunk_size = (size_t)length + 12U;
        if (bytes_equal(type, "tEXt", 4U) && png_text_key_matches(payload, length, key, key_length)) {
            removed = 1;
        } else {
            memcpy(output + output_size, data + input_offset, chunk_size);
            output_size += chunk_size;
        }
        input_offset += chunk_size;
        if (bytes_equal(type, "IEND", 4U)) {
            break;
        }
    }
    if (!removed) {
        rt_free(output);
        return -1;
    }
    *out_data = output;
    *out_size = output_size;
    return 0;
}

static int list_png_text(const unsigned char *data, size_t size, const char *label) {
    static const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    size_t offset = 8U;

    if (size < 8U || !bytes_equal(data, (const char *)signature, sizeof(signature))) {
        tool_write_error("imgmeta", "text listing is implemented for PNG only: ", label);
        return -1;
    }
    while (offset + 12U <= size) {
        unsigned int length = read_u32_be(data + offset);
        const unsigned char *type = data + offset + 4U;
        const unsigned char *payload = data + offset + 8U;
        size_t key_length;

        if ((size_t)length > size - offset - 12U) {
            tool_write_error("imgmeta", "truncated PNG chunk while listing text: ", label);
            return -1;
        }
        if (bytes_equal(type, "tEXt", 4U)) {
            key_length = png_text_key_length(payload, length);
            rt_write_cstr(1, label);
            rt_write_cstr(1, "\ttEXt\t");
            rt_write_all(1, payload, key_length);
            rt_write_char(1, '\t');
            if (key_length + 1U < (size_t)length) {
                rt_write_all(1, payload + key_length + 1U, (size_t)length - key_length - 1U);
            }
            rt_write_char(1, '\n');
        } else if (bytes_equal(type, "iTXt", 4U) || bytes_equal(type, "zTXt", 4U)) {
            key_length = png_text_key_length(payload, length);
            rt_write_cstr(1, label);
            rt_write_char(1, '\t');
            rt_write_all(1, type, 4U);
            rt_write_char(1, '\t');
            rt_write_all(1, payload, key_length);
            rt_write_cstr(1, "\t-");
            rt_write_char(1, '\n');
        } else if (bytes_equal(type, "IEND", 4U)) {
            break;
        }
        offset += 12U + (size_t)length;
    }
    return 0;
}

static int jpeg_segment_is_metadata(unsigned char marker, const unsigned char *segment, unsigned int segment_size) {
    if (marker == 0xfeU) {
        return 1;
    }
    if (marker == 0xe1U && segment_size >= 8U && bytes_equal(segment + 2U, "Exif", 4U)) {
        return 1;
    }
    if (marker == 0xe1U && segment_size >= 31U && bytes_equal(segment + 2U, "http://ns.adobe.com/xap/1.0/", 29U)) {
        return 1;
    }
    if (marker == 0xe2U && segment_size >= 13U && bytes_equal(segment + 2U, "ICC_PROFILE", 11U)) {
        return 1;
    }
    return 0;
}

static int strip_jpeg(const unsigned char *data, size_t size, unsigned char **out_data, size_t *out_size) {
    unsigned char *output;
    size_t input_offset = 2U;
    size_t output_size = 0U;

    if (size < 2U || data[0] != 0xffU || data[1] != 0xd8U) {
        return -1;
    }
    output = (unsigned char *)rt_malloc(size == 0U ? 1U : size);
    if (output == 0) {
        return -1;
    }
    output[output_size++] = data[0];
    output[output_size++] = data[1];
    while (input_offset < size) {
        unsigned char marker;
        unsigned int segment_size;
        size_t marker_start;

        marker_start = input_offset;
        if (data[input_offset] != 0xffU) {
            memcpy(output + output_size, data + input_offset, size - input_offset);
            output_size += size - input_offset;
            break;
        }
        while (input_offset < size && data[input_offset] == 0xffU) {
            input_offset += 1U;
        }
        if (input_offset >= size) {
            rt_free(output);
            return -1;
        }
        marker = data[input_offset++];
        if (marker == 0xdaU || marker == 0xd9U) {
            memcpy(output + output_size, data + marker_start, size - marker_start);
            output_size += size - marker_start;
            break;
        }
        if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd7U)) {
            memcpy(output + output_size, data + marker_start, input_offset - marker_start);
            output_size += input_offset - marker_start;
            continue;
        }
        if (input_offset + 2U > size) {
            rt_free(output);
            return -1;
        }
        segment_size = read_u16_be(data + input_offset);
        if (segment_size < 2U || (size_t)segment_size > size - input_offset) {
            rt_free(output);
            return -1;
        }
        if (!jpeg_segment_is_metadata(marker, data + input_offset, segment_size)) {
            memcpy(output + output_size, data + marker_start, input_offset - marker_start + (size_t)segment_size);
            output_size += input_offset - marker_start + (size_t)segment_size;
        }
        input_offset += (size_t)segment_size;
    }
    *out_data = output;
    *out_size = output_size;
    return 0;
}

static int webp_chunk_is_metadata(const unsigned char *type) {
    return bytes_equal(type, "EXIF", 4U) ||
           bytes_equal(type, "XMP ", 4U) ||
           bytes_equal(type, "ICCP", 4U);
}

static int strip_webp(const unsigned char *data, size_t size, unsigned char **out_data, size_t *out_size) {
    unsigned char *output;
    size_t input_offset = 12U;
    size_t output_size = 12U;

    if (size < 20U || !bytes_equal(data, "RIFF", 4U) || !bytes_equal(data + 8U, "WEBP", 4U)) {
        return -1;
    }
    output = (unsigned char *)rt_malloc(size == 0U ? 1U : size);
    if (output == 0) {
        return -1;
    }
    memcpy(output, data, 12U);
    while (input_offset + 8U <= size) {
        const unsigned char *type = data + input_offset;
        unsigned int chunk_length = read_u32_le(data + input_offset + 4U);
        size_t chunk_total = 8U + (size_t)chunk_length + ((chunk_length & 1U) != 0U ? 1U : 0U);

        if (chunk_total < 8U || chunk_total > size - input_offset) {
            rt_free(output);
            return -1;
        }
        if (!webp_chunk_is_metadata(type)) {
            memcpy(output + output_size, data + input_offset, chunk_total);
            if (bytes_equal(type, "VP8X", 4U) && chunk_length >= 10U) {
                output[output_size + 8U] &= (unsigned char)~0x2cU;
            }
            output_size += chunk_total;
        }
        input_offset += chunk_total;
    }
    if (input_offset != size || output_size < 12U || output_size - 8U > 0xffffffffU) {
        rt_free(output);
        return -1;
    }
    write_u32_le(output + 4U, (unsigned int)(output_size - 8U));
    *out_data = output;
    *out_size = output_size;
    return 0;
}

static int strip_path(const char *input_path, const char *output_path) {
    unsigned char *data;
    unsigned char *stripped = 0;
    size_t size;
    size_t stripped_size = 0U;
    ImageInfo info;
    int result = -1;

    if (read_all_input(input_path, &data, &size) != 0) {
        return -1;
    }
    if (image_probe(data, size, &info) != 0) {
        tool_write_error("imgmeta", "unsupported image format: ", input_path);
        rt_free(data);
        return -1;
    }
    if (info.format == IMAGE_FORMAT_PNG) {
        result = strip_png(data, size, &stripped, &stripped_size);
    } else if (info.format == IMAGE_FORMAT_JPEG) {
        result = strip_jpeg(data, size, &stripped, &stripped_size);
    } else if (info.format == IMAGE_FORMAT_WEBP) {
        result = strip_webp(data, size, &stripped, &stripped_size);
    } else {
        tool_write_error("imgmeta", "strip is not implemented for: ", image_format_extension(info.format));
        rt_free(data);
        return -1;
    }
    rt_free(data);
    if (result != 0 || stripped == 0) {
        tool_write_error("imgmeta", "could not strip metadata: ", input_path);
        return -1;
    }
    result = write_file(output_path, stripped, stripped_size);
    rt_free(stripped);
    return result;
}

static int copy_path(const char *input_path, const char *output_path) {
    unsigned char *data;
    size_t size;
    ImageInfo info;
    int result;

    if (read_all_input(input_path, &data, &size) != 0) {
        return -1;
    }
    if (image_probe(data, size, &info) != 0) {
        tool_write_error("imgmeta", "unsupported image format: ", input_path);
        rt_free(data);
        return -1;
    }
    result = write_file(output_path, data, size);
    rt_free(data);
    return result;
}

static int edit_text_path(const char *input_path, const char *output_path, const char *assignment) {
    unsigned char *data;
    unsigned char *edited = 0;
    size_t size;
    size_t edited_size = 0U;
    ImageInfo info;
    const char *equals;
    int result;

    equals = assignment;
    while (*equals != '\0' && *equals != '=') {
        equals += 1;
    }
    if (*equals != '=') {
        tool_write_error("imgmeta", "text metadata must be KEY=VALUE", 0);
        return -1;
    }
    if (read_all_input(input_path, &data, &size) != 0) {
        return -1;
    }
    if (image_probe(data, size, &info) != 0) {
        tool_write_error("imgmeta", "unsupported image format: ", input_path);
        rt_free(data);
        return -1;
    }
    if (info.format != IMAGE_FORMAT_PNG) {
        tool_write_error("imgmeta", "text editing is not implemented for: ", image_format_extension(info.format));
        rt_free(data);
        return -1;
    }
    result = edit_png_text(data, size, assignment, (size_t)(equals - assignment), equals + 1, rt_strlen(equals + 1), &edited, &edited_size);
    rt_free(data);
    if (result != 0 || edited == 0) {
        tool_write_error("imgmeta", "could not edit PNG text metadata: ", input_path);
        return -1;
    }
    result = write_file(output_path, edited, edited_size);
    rt_free(edited);
    return result;
}

static int remove_text_path(const char *input_path, const char *output_path, const char *key) {
    unsigned char *data;
    unsigned char *edited = 0;
    size_t size;
    size_t edited_size = 0U;
    ImageInfo info;
    int result;

    if (read_all_input(input_path, &data, &size) != 0) {
        return -1;
    }
    if (image_probe(data, size, &info) != 0) {
        tool_write_error("imgmeta", "unsupported image format: ", input_path);
        rt_free(data);
        return -1;
    }
    if (info.format != IMAGE_FORMAT_PNG) {
        tool_write_error("imgmeta", "text removal is not implemented for: ", image_format_extension(info.format));
        rt_free(data);
        return -1;
    }
    result = remove_png_text(data, size, key, rt_strlen(key), &edited, &edited_size);
    rt_free(data);
    if (result != 0 || edited == 0) {
        tool_write_error("imgmeta", "could not remove PNG text metadata: ", input_path);
        return -1;
    }
    result = write_file(output_path, edited, edited_size);
    rt_free(edited);
    return result;
}

static int list_text_path(const char *path) {
    unsigned char *data;
    size_t size;
    const char *label = path ? path : "stdin";
    int result;

    if (read_all_input(path, &data, &size) != 0) {
        return -1;
    }
    result = list_png_text(data, size, label);
    rt_free(data);
    return result;
}

static int run_show(int argc, char **argv, int arg_index) {
    int status = 0;

    if (arg_index >= argc) {
        return show_path(0) == 0 ? 0 : 1;
    }
    while (arg_index < argc) {
        if (show_path(argv[arg_index]) != 0) {
            status = 1;
        }
        arg_index += 1;
    }
    return status;
}

static int run_list_text(int argc, char **argv, int arg_index) {
    int status = 0;

    if (arg_index >= argc) {
        return list_text_path(0) == 0 ? 0 : 1;
    }
    while (arg_index < argc) {
        if (list_text_path(argv[arg_index]) != 0) {
            status = 1;
        }
        arg_index += 1;
    }
    return status;
}

static int run_strip(int argc, char **argv, int arg_index) {
    const char *output_path = 0;
    const char *input_path = 0;

    while (arg_index < argc) {
        const char *arg = argv[arg_index];

        if ((rt_strcmp(arg, "-o") == 0 || rt_strcmp(arg, "--output") == 0) && arg_index + 1 < argc) {
            output_path = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if (rt_strcmp(arg, "--") == 0) {
            arg_index += 1;
            break;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            tool_write_error("imgmeta", "unknown option: ", arg);
            print_usage();
            return 1;
        }
        if (input_path != 0) {
            tool_write_error("imgmeta", "extra operand: ", arg);
            return 1;
        }
        input_path = arg;
        arg_index += 1;
    }
    if (input_path == 0 && arg_index < argc) {
        input_path = argv[arg_index++];
    }
    if (input_path == 0 || output_path == 0) {
        tool_write_error("imgmeta", "strip requires -o OUTPUT and one input file", 0);
        print_usage();
        return 1;
    }
    return strip_path(input_path, output_path) == 0 ? 0 : 1;
}

static int run_copy(int argc, char **argv, int arg_index) {
    const char *output_path = 0;
    const char *input_path = 0;

    while (arg_index < argc) {
        const char *arg = argv[arg_index];

        if ((rt_strcmp(arg, "-o") == 0 || rt_strcmp(arg, "--output") == 0) && arg_index + 1 < argc) {
            output_path = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if (rt_strcmp(arg, "--") == 0) {
            arg_index += 1;
            break;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            tool_write_error("imgmeta", "unknown option: ", arg);
            print_usage();
            return 1;
        }
        if (input_path != 0) {
            tool_write_error("imgmeta", "extra operand: ", arg);
            return 1;
        }
        input_path = arg;
        arg_index += 1;
    }
    if (input_path == 0 && arg_index < argc) {
        input_path = argv[arg_index++];
    }
    if (input_path == 0 || output_path == 0) {
        tool_write_error("imgmeta", "copy requires -o OUTPUT and one input file", 0);
        print_usage();
        return 1;
    }
    return copy_path(input_path, output_path) == 0 ? 0 : 1;
}

static int run_edit(int argc, char **argv, int arg_index) {
    const char *output_path = 0;
    const char *input_path = 0;
    const char *text_assignment = 0;
    const char *remove_text_key = 0;

    while (arg_index < argc) {
        const char *arg = argv[arg_index];

        if ((rt_strcmp(arg, "-o") == 0 || rt_strcmp(arg, "--output") == 0) && arg_index + 1 < argc) {
            output_path = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if (rt_strcmp(arg, "--set-text") == 0 && arg_index + 1 < argc) {
            text_assignment = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if (rt_strcmp(arg, "--remove-text") == 0 && arg_index + 1 < argc) {
            remove_text_key = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if (rt_strcmp(arg, "--") == 0) {
            arg_index += 1;
            break;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            tool_write_error("imgmeta", "unknown option: ", arg);
            print_usage();
            return 1;
        }
        if (input_path != 0) {
            tool_write_error("imgmeta", "extra operand: ", arg);
            return 1;
        }
        input_path = arg;
        arg_index += 1;
    }
    if (input_path == 0 && arg_index < argc) {
        input_path = argv[arg_index++];
    }
    if (input_path == 0 || output_path == 0 || (text_assignment == 0 && remove_text_key == 0) ||
        (text_assignment != 0 && remove_text_key != 0)) {
        tool_write_error("imgmeta", "edit requires one of --set-text KEY=VALUE or --remove-text KEY, -o OUTPUT, and one input file", 0);
        print_usage();
        return 1;
    }
    if (remove_text_key != 0) {
        return remove_text_path(input_path, output_path, remove_text_key) == 0 ? 0 : 1;
    }
    return edit_text_path(input_path, output_path, text_assignment) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *command;

    if (argc < 2) {
        print_usage();
        return 1;
    }
    command = argv[1];
    if (rt_strcmp(command, "-h") == 0 || rt_strcmp(command, "--help") == 0) {
        print_usage();
        return 0;
    }
    if (rt_strcmp(command, "show") == 0) {
        return run_show(argc, argv, 2);
    }
    if (rt_strcmp(command, "list-text") == 0 || rt_strcmp(command, "--list-text") == 0) {
        return run_list_text(argc, argv, 2);
    }
    if (rt_strcmp(command, "strip") == 0) {
        return run_strip(argc, argv, 2);
    }
    if (rt_strcmp(command, "copy") == 0) {
        return run_copy(argc, argv, 2);
    }
    if (rt_strcmp(command, "edit") == 0) {
        return run_edit(argc, argv, 2);
    }
    tool_write_error("imgmeta", "unknown command: ", command);
    print_usage();
    return 1;
}
