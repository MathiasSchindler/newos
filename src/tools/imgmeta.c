#include "image/image.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define IMGMETA_INITIAL_CAPACITY (64U * 1024U)

static void print_usage(void) {
    tool_write_usage("imgmeta", "show FILE ... | strip -o OUTPUT FILE");
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
    if (rt_strcmp(command, "strip") == 0) {
        return run_strip(argc, argv, 2);
    }
    tool_write_error("imgmeta", "unknown command: ", command);
    print_usage();
    return 1;
}
