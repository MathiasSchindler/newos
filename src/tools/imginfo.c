#include "image/image.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define IMGINFO_READ_LIMIT (256U * 1024U)
#define IMGINFO_ENTRY_CAPACITY 512U
#define IMGINFO_PATH_CAPACITY 2048U

typedef struct {
    int mime_only;
    int machine_output;
    int detailed_output;
    int json_output;
    int canonical_ext_only;
    int recursive;
} ImginfoOptions;

static void print_usage(void) {
    tool_write_usage("imginfo", "[-m|--mime] [-p|--plain] [-d|--details] [--json] [--canonical-ext] [-R|--recursive] [file ...]");
}

static int read_probe_data(const char *path, unsigned char *buffer, size_t buffer_size, size_t *size_out) {
    int fd;
    int should_close;
    long bytes_read;

    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }
    bytes_read = platform_read(fd, buffer, buffer_size);
    tool_close_input(fd, should_close);
    if (bytes_read < 0) {
        tool_write_error("imginfo", "read failed: ", path ? path : "stdin");
        return -1;
    }
    *size_out = (size_t)bytes_read;
    return 0;
}

static int read_all_input(const char *path, unsigned char **data_out, size_t *size_out) {
    int fd;
    int should_close;
    unsigned char *buffer;
    size_t capacity = IMGINFO_READ_LIMIT;
    size_t used = 0U;

    *data_out = 0;
    *size_out = 0U;
    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }
    buffer = (unsigned char *)rt_malloc(capacity);
    if (buffer == 0) {
        tool_close_input(fd, should_close);
        tool_write_error("imginfo", "out of memory: ", path ? path : "stdin");
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
                tool_write_error("imginfo", "input too large: ", path ? path : "stdin");
                return -1;
            }
            resized = (unsigned char *)rt_realloc(buffer, next_capacity);
            if (resized == 0) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                tool_write_error("imginfo", "out of memory: ", path ? path : "stdin");
                return -1;
            }
            buffer = resized;
            capacity = next_capacity;
        }
        bytes_read = platform_read(fd, buffer + used, capacity - used);
        if (bytes_read < 0) {
            rt_free(buffer);
            tool_close_input(fd, should_close);
            tool_write_error("imginfo", "read failed: ", path ? path : "stdin");
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

static int write_unknown_field(void) {
    return rt_write_char(1, '-');
}

static int write_uint_field(unsigned int value) {
    return rt_write_uint(1, (unsigned long long)value);
}

static void write_unknown_value(void) {
    rt_write_line(1, "-");
}

static int write_property_list(unsigned int property_flags) {
    static const unsigned int properties[] = {
        IMAGE_PROPERTY_ALPHA,
        IMAGE_PROPERTY_PALETTE,
        IMAGE_PROPERTY_INTERLACED,
        IMAGE_PROPERTY_ANIMATED,
        IMAGE_PROPERTY_PROGRESSIVE,
        IMAGE_PROPERTY_LOSSLESS,
        IMAGE_PROPERTY_EXIF,
        IMAGE_PROPERTY_ICC,
        IMAGE_PROPERTY_XMP,
        IMAGE_PROPERTY_C2PA,
        IMAGE_PROPERTY_TOP_DOWN,
        IMAGE_PROPERTY_LOOPING,
        IMAGE_PROPERTY_ORIENTATION
    };
    size_t i;
    int wrote = 0;

    for (i = 0U; i < sizeof(properties) / sizeof(properties[0]); ++i) {
        if ((property_flags & properties[i]) != 0U) {
            const char *name = image_property_name(properties[i]);
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
    return wrote;
}

static void write_json_string(const char *text) {
    size_t index;

    rt_write_char(1, '"');
    if (text != 0) {
        for (index = 0U; text[index] != '\0'; ++index) {
            unsigned char ch = (unsigned char)text[index];

            if (ch == '"' || ch == '\\') {
                rt_write_char(1, '\\');
                rt_write_char(1, (char)ch);
            } else if (ch == '\n') {
                rt_write_cstr(1, "\\n");
            } else if (ch == '\r') {
                rt_write_cstr(1, "\\r");
            } else if (ch == '\t') {
                rt_write_cstr(1, "\\t");
            } else if (ch < 32U) {
                rt_write_cstr(1, "\\u00");
                rt_write_char(1, "0123456789abcdef"[(ch >> 4U) & 0x0fU]);
                rt_write_char(1, "0123456789abcdef"[ch & 0x0fU]);
            } else {
                rt_write_char(1, (char)ch);
            }
        }
    }
    rt_write_char(1, '"');
}

static void write_json_uint_or_null(unsigned int value, int known) {
    if (known) {
        rt_write_uint(1, (unsigned long long)value);
    } else {
        rt_write_cstr(1, "null");
    }
}

static void write_json_properties(unsigned int property_flags) {
    static const unsigned int properties[] = {
        IMAGE_PROPERTY_ALPHA,
        IMAGE_PROPERTY_PALETTE,
        IMAGE_PROPERTY_INTERLACED,
        IMAGE_PROPERTY_ANIMATED,
        IMAGE_PROPERTY_PROGRESSIVE,
        IMAGE_PROPERTY_LOSSLESS,
        IMAGE_PROPERTY_EXIF,
        IMAGE_PROPERTY_ICC,
        IMAGE_PROPERTY_XMP,
        IMAGE_PROPERTY_C2PA,
        IMAGE_PROPERTY_TOP_DOWN,
        IMAGE_PROPERTY_LOOPING,
        IMAGE_PROPERTY_ORIENTATION
    };
    size_t index;
    int wrote = 0;

    rt_write_char(1, '[');
    for (index = 0U; index < sizeof(properties) / sizeof(properties[0]); ++index) {
        if ((property_flags & properties[index]) != 0U) {
            const char *name = image_property_name(properties[index]);

            if (name == 0) {
                continue;
            }
            if (wrote) {
                rt_write_char(1, ',');
            }
            write_json_string(name);
            wrote = 1;
        }
    }
    rt_write_char(1, ']');
}

static int ascii_lower(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A' + 'a';
    }
    return ch;
}

static const char *path_extension(const char *path) {
    const char *last_slash = 0;
    const char *last_dot = 0;
    const char *p;

    if (path == 0) {
        return 0;
    }
    for (p = path; *p != '\0'; ++p) {
        if (*p == '/') {
            last_slash = p;
            last_dot = 0;
        } else if (*p == '.') {
            last_dot = p;
        }
    }
    if (last_dot == 0 || last_dot[1] == '\0') {
        return 0;
    }
    if (last_slash != 0 && last_dot == last_slash + 1) {
        return 0;
    }
    if (last_slash == 0 && last_dot == path) {
        return 0;
    }
    return last_dot + 1;
}

static int extension_equals(const char *extension, const char *expected) {
    size_t i = 0U;

    if (extension == 0 || expected == 0) {
        return 0;
    }
    while (extension[i] != '\0' && expected[i] != '\0') {
        if (ascii_lower((unsigned char)extension[i]) != ascii_lower((unsigned char)expected[i])) {
            return 0;
        }
        i += 1U;
    }
    return extension[i] == '\0' && expected[i] == '\0';
}

static int extension_matches_format(const char *extension, ImageFormat format) {
    switch (format) {
        case IMAGE_FORMAT_PNG:
            return extension_equals(extension, "png");
        case IMAGE_FORMAT_JPEG:
            return extension_equals(extension, "jpg") ||
                   extension_equals(extension, "jpeg") ||
                   extension_equals(extension, "jpe");
        case IMAGE_FORMAT_GIF:
            return extension_equals(extension, "gif");
        case IMAGE_FORMAT_TIFF:
            return extension_equals(extension, "tif") ||
                   extension_equals(extension, "tiff");
        case IMAGE_FORMAT_WEBP:
            return extension_equals(extension, "webp");
        case IMAGE_FORMAT_BMP:
            return extension_equals(extension, "bmp") ||
                   extension_equals(extension, "dib");
        default:
            return 1;
    }
}

static void warn_extension_mismatch(const char *path, const ImageInfo *info) {
    const char *extension = path_extension(path);

    if (extension == 0 || extension_matches_format(extension, info->format)) {
        return;
    }
    rt_write_cstr(2, "imginfo: warning: file extension .");
    rt_write_cstr(2, extension);
    rt_write_cstr(2, " does not match detected ");
    rt_write_cstr(2, image_format_extension(info->format));
    rt_write_cstr(2, ": ");
    rt_write_line(2, path);
}

static int write_machine_line(const char *label, const ImageInfo *info) {
    rt_write_cstr(1, label);
    rt_write_char(1, '\t');
    rt_write_cstr(1, image_format_extension(info->format));
    rt_write_char(1, '\t');
    if ((info->flags & IMAGE_INFO_HAS_DIMENSIONS) != 0U) {
        write_uint_field(info->width);
        rt_write_char(1, '\t');
        write_uint_field(info->height);
    } else {
        write_unknown_field();
        rt_write_char(1, '\t');
        write_unknown_field();
    }
    rt_write_char(1, '\t');
    if ((info->flags & IMAGE_INFO_HAS_BIT_DEPTH) != 0U) {
        write_uint_field(info->bit_depth);
    } else {
        write_unknown_field();
    }
    rt_write_char(1, '\t');
    if ((info->flags & IMAGE_INFO_HAS_CHANNELS) != 0U) {
        write_uint_field(info->channel_count);
    } else {
        write_unknown_field();
    }
    rt_write_char(1, '\t');
    rt_write_line(1, image_format_mime(info->format));
    return 0;
}

static int write_human_line(const char *label, const ImageInfo *info) {
    rt_write_cstr(1, label);
    rt_write_cstr(1, ": ");
    rt_write_cstr(1, image_format_name(info->format));
    rt_write_cstr(1, " image");
    if ((info->flags & IMAGE_INFO_HAS_DIMENSIONS) != 0U) {
        rt_write_cstr(1, ", ");
        rt_write_uint(1, (unsigned long long)info->width);
        rt_write_char(1, 'x');
        rt_write_uint(1, (unsigned long long)info->height);
    }
    if ((info->flags & IMAGE_INFO_HAS_BIT_DEPTH) != 0U) {
        rt_write_cstr(1, ", ");
        rt_write_uint(1, (unsigned long long)info->bit_depth);
        rt_write_cstr(1, "-bit");
    }
    if ((info->flags & IMAGE_INFO_HAS_CHANNELS) != 0U) {
        rt_write_cstr(1, ", ");
        rt_write_cstr(1, image_channel_description(info));
    }
    if ((info->flags & IMAGE_INFO_HAS_VARIANT) != 0U) {
        rt_write_cstr(1, ", ");
        rt_write_cstr(1, info->variant);
    }
    if ((info->flags & IMAGE_INFO_HAS_COLOR) != 0U &&
        ((info->flags & IMAGE_INFO_HAS_CHANNELS) == 0U ||
         rt_strcmp(info->color_model, image_channel_description(info)) != 0)) {
        rt_write_cstr(1, ", ");
        rt_write_cstr(1, info->color_model);
    }
    if ((info->flags & IMAGE_INFO_HAS_COMPRESSION) != 0U) {
        rt_write_cstr(1, ", ");
        rt_write_cstr(1, info->compression);
    }
    if ((info->flags & IMAGE_INFO_HAS_DENSITY) != 0U) {
        rt_write_cstr(1, ", ");
        rt_write_uint(1, (unsigned long long)info->density_x);
        rt_write_char(1, 'x');
        rt_write_uint(1, (unsigned long long)info->density_y);
        rt_write_char(1, ' ');
        rt_write_cstr(1, info->density_unit);
    }
    if ((info->flags & IMAGE_INFO_HAS_ORIENTATION) != 0U && info->orientation != 1U) {
        rt_write_cstr(1, ", orientation ");
        rt_write_uint(1, (unsigned long long)info->orientation);
        rt_write_cstr(1, " (");
        rt_write_cstr(1, image_orientation_description(info->orientation));
        rt_write_char(1, ')');
    }
    if ((info->flags & IMAGE_INFO_HAS_FRAMES) != 0U) {
        rt_write_cstr(1, ", ");
        rt_write_uint(1, (unsigned long long)info->frame_count);
        rt_write_cstr(1, info->frame_count == 1U ? " frame" : " frames");
    }
    if (info->property_flags != 0U) {
        rt_write_cstr(1, ", ");
        write_property_list(info->property_flags);
    }
    rt_write_cstr(1, ", ");
    rt_write_line(1, image_format_mime(info->format));
    return 0;
}

static void write_detail_string(const char *name, const char *value, int known) {
    rt_write_cstr(1, "  ");
    rt_write_cstr(1, name);
    rt_write_cstr(1, ": ");
    if (known && value != 0) {
        rt_write_line(1, value);
    } else {
        write_unknown_value();
    }
}

static void write_detail_uint(const char *name, unsigned int value, int known) {
    rt_write_cstr(1, "  ");
    rt_write_cstr(1, name);
    rt_write_cstr(1, ": ");
    if (known) {
        rt_write_uint(1, (unsigned long long)value);
        rt_write_char(1, '\n');
    } else {
        write_unknown_value();
    }
}

static int write_detail_block(const char *label, const ImageInfo *info) {
    rt_write_cstr(1, label);
    rt_write_line(1, ":");
    write_detail_string("format", image_format_name(info->format), 1);
    write_detail_string("extension", image_format_extension(info->format), 1);
    write_detail_string("mime", image_format_mime(info->format), 1);
    write_detail_string("variant", info->variant, (info->flags & IMAGE_INFO_HAS_VARIANT) != 0U);
    rt_write_cstr(1, "  dimensions: ");
    if ((info->flags & IMAGE_INFO_HAS_DIMENSIONS) != 0U) {
        rt_write_uint(1, (unsigned long long)info->width);
        rt_write_char(1, 'x');
        rt_write_uint(1, (unsigned long long)info->height);
        rt_write_char(1, '\n');
    } else {
        write_unknown_value();
    }
    write_detail_uint("bit-depth", info->bit_depth, (info->flags & IMAGE_INFO_HAS_BIT_DEPTH) != 0U);
    rt_write_cstr(1, "  channels: ");
    if ((info->flags & IMAGE_INFO_HAS_CHANNELS) != 0U) {
        rt_write_uint(1, (unsigned long long)info->channel_count);
        rt_write_cstr(1, " (");
        rt_write_cstr(1, image_channel_description(info));
        rt_write_line(1, ")");
    } else {
        write_unknown_value();
    }
    write_detail_string("color", info->color_model, (info->flags & IMAGE_INFO_HAS_COLOR) != 0U);
    write_detail_string("compression", info->compression, (info->flags & IMAGE_INFO_HAS_COMPRESSION) != 0U);
    rt_write_cstr(1, "  density: ");
    if ((info->flags & IMAGE_INFO_HAS_DENSITY) != 0U) {
        rt_write_uint(1, (unsigned long long)info->density_x);
        rt_write_char(1, 'x');
        rt_write_uint(1, (unsigned long long)info->density_y);
        rt_write_char(1, ' ');
        rt_write_line(1, info->density_unit);
    } else {
        write_unknown_value();
    }
    rt_write_cstr(1, "  orientation: ");
    if ((info->flags & IMAGE_INFO_HAS_ORIENTATION) != 0U) {
        rt_write_uint(1, (unsigned long long)info->orientation);
        rt_write_cstr(1, " (");
        rt_write_cstr(1, image_orientation_description(info->orientation));
        rt_write_line(1, ")");
    } else {
        write_unknown_value();
    }
    write_detail_uint("frames", info->frame_count, (info->flags & IMAGE_INFO_HAS_FRAMES) != 0U);
    write_detail_uint("duration-ms", info->duration_ms, (info->flags & IMAGE_INFO_HAS_DURATION_MS) != 0U);
    write_detail_uint("loop-count", info->loop_count, (info->flags & IMAGE_INFO_HAS_LOOP_COUNT) != 0U);
    rt_write_cstr(1, "  properties: ");
    write_property_list(info->property_flags);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  c2pa: ");
    if ((info->flags & IMAGE_INFO_HAS_C2PA) != 0U) {
        rt_write_line(1, info->c2pa.status);
        write_detail_string("c2pa-carrier", info->c2pa.carrier, 1);
        write_detail_string("c2pa-signature-algorithm", info->c2pa.signature_algorithm, info->c2pa.signature_algorithm != 0);
        write_detail_uint("c2pa-carriers", info->c2pa.carrier_count, 1);
        write_detail_uint("c2pa-boxes", info->c2pa.box_count, 1);
        write_detail_uint("c2pa-invalid-boxes", info->c2pa.invalid_box_count, 1);
        write_detail_uint("c2pa-jumbf-boxes", info->c2pa.jumbf_box_count, 1);
        write_detail_uint("c2pa-cbor-boxes", info->c2pa.cbor_box_count, 1);
        write_detail_uint("c2pa-valid-cbor-boxes", info->c2pa.cbor_valid_count, 1);
        write_detail_uint("c2pa-manifests", info->c2pa.manifest_count, 1);
        write_detail_uint("c2pa-claims", info->c2pa.claim_count, 1);
        write_detail_uint("c2pa-assertion-stores", info->c2pa.assertion_store_count, 1);
        write_detail_uint("c2pa-signatures", info->c2pa.signature_count, 1);
        write_detail_uint("c2pa-cose-signatures", info->c2pa.cose_signature_count, 1);
        write_detail_uint("c2pa-x509-certificates", info->c2pa.x509_cert_count, 1);
        write_detail_uint("c2pa-parseable-x509-certificates", info->c2pa.x509_parseable_cert_count, 1);
        write_detail_string("c2pa-content-hash", info->c2pa.content_hash_matched ? "match" : (info->c2pa.content_hash_mismatched ? "mismatch" : (info->c2pa.content_hash_checked ? "checked" : "not checked")), 1);
        write_detail_string("c2pa-signature-verification", info->c2pa.signature_supported ? (info->c2pa.signature_valid ? "valid" : "invalid") : "unsupported", 1);
        write_detail_string("c2pa-trust-validation", info->c2pa.trust_supported ? (info->c2pa.trust_valid ? "trusted" : "untrusted") : "unsupported", 1);
        write_detail_uint("c2pa-ingredients", info->c2pa.ingredient_count, 1);
    } else {
        write_unknown_value();
    }
    return 0;
}

static void write_json_info(const char *label, const ImageInfo *info) {
    rt_write_cstr(1, "{\"path\":");
    write_json_string(label);
    rt_write_cstr(1, ",\"format\":");
    write_json_string(image_format_name(info->format));
    rt_write_cstr(1, ",\"extension\":");
    write_json_string(image_format_extension(info->format));
    rt_write_cstr(1, ",\"canonical_extension\":");
    write_json_string(image_format_extension(info->format));
    rt_write_cstr(1, ",\"mime\":");
    write_json_string(image_format_mime(info->format));
    rt_write_cstr(1, ",\"width\":");
    write_json_uint_or_null(info->width, (info->flags & IMAGE_INFO_HAS_DIMENSIONS) != 0U);
    rt_write_cstr(1, ",\"height\":");
    write_json_uint_or_null(info->height, (info->flags & IMAGE_INFO_HAS_DIMENSIONS) != 0U);
    rt_write_cstr(1, ",\"bit_depth\":");
    write_json_uint_or_null(info->bit_depth, (info->flags & IMAGE_INFO_HAS_BIT_DEPTH) != 0U);
    rt_write_cstr(1, ",\"channels\":");
    write_json_uint_or_null(info->channel_count, (info->flags & IMAGE_INFO_HAS_CHANNELS) != 0U);
    rt_write_cstr(1, ",\"variant\":");
    if ((info->flags & IMAGE_INFO_HAS_VARIANT) != 0U) write_json_string(info->variant); else rt_write_cstr(1, "null");
    rt_write_cstr(1, ",\"color\":");
    if ((info->flags & IMAGE_INFO_HAS_COLOR) != 0U) write_json_string(info->color_model); else rt_write_cstr(1, "null");
    rt_write_cstr(1, ",\"compression\":");
    if ((info->flags & IMAGE_INFO_HAS_COMPRESSION) != 0U) write_json_string(info->compression); else rt_write_cstr(1, "null");
    rt_write_cstr(1, ",\"orientation\":");
    write_json_uint_or_null(info->orientation, (info->flags & IMAGE_INFO_HAS_ORIENTATION) != 0U);
    rt_write_cstr(1, ",\"frames\":");
    write_json_uint_or_null(info->frame_count, (info->flags & IMAGE_INFO_HAS_FRAMES) != 0U);
    rt_write_cstr(1, ",\"duration_ms\":");
    write_json_uint_or_null(info->duration_ms, (info->flags & IMAGE_INFO_HAS_DURATION_MS) != 0U);
    rt_write_cstr(1, ",\"loop_count\":");
    write_json_uint_or_null(info->loop_count, (info->flags & IMAGE_INFO_HAS_LOOP_COUNT) != 0U);
    rt_write_cstr(1, ",\"density\":");
    if ((info->flags & IMAGE_INFO_HAS_DENSITY) != 0U) {
        rt_write_cstr(1, "{\"x\":");
        rt_write_uint(1, (unsigned long long)info->density_x);
        rt_write_cstr(1, ",\"y\":");
        rt_write_uint(1, (unsigned long long)info->density_y);
        rt_write_cstr(1, ",\"unit\":");
        write_json_string(info->density_unit);
        rt_write_char(1, '}');
    } else {
        rt_write_cstr(1, "null");
    }
    rt_write_cstr(1, ",\"properties\":");
    write_json_properties(info->property_flags);
    rt_write_cstr(1, ",\"c2pa\":");
    if ((info->flags & IMAGE_INFO_HAS_C2PA) != 0U) {
        rt_write_cstr(1, "{\"present\":true,\"status\":");
        write_json_string(info->c2pa.status);
        rt_write_cstr(1, ",\"carrier\":");
        write_json_string(info->c2pa.carrier);
        rt_write_cstr(1, ",\"signature_algorithm\":");
        if (info->c2pa.signature_algorithm != 0) write_json_string(info->c2pa.signature_algorithm); else rt_write_cstr(1, "null");
        rt_write_cstr(1, ",\"carrier_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.carrier_count);
        rt_write_cstr(1, ",\"recognized_carrier_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.recognized_carrier_count);
        rt_write_cstr(1, ",\"box_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.box_count);
        rt_write_cstr(1, ",\"invalid_box_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.invalid_box_count);
        rt_write_cstr(1, ",\"jumbf_box_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.jumbf_box_count);
        rt_write_cstr(1, ",\"cbor_box_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.cbor_box_count);
        rt_write_cstr(1, ",\"cbor_valid_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.cbor_valid_count);
        rt_write_cstr(1, ",\"manifest_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.manifest_count);
        rt_write_cstr(1, ",\"claim_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.claim_count);
        rt_write_cstr(1, ",\"assertion_store_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.assertion_store_count);
        rt_write_cstr(1, ",\"signature_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.signature_count);
        rt_write_cstr(1, ",\"cose_signature_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.cose_signature_count);
        rt_write_cstr(1, ",\"x509_certificate_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.x509_cert_count);
        rt_write_cstr(1, ",\"x509_parseable_certificate_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.x509_parseable_cert_count);
        rt_write_cstr(1, ",\"content_hash_checked\":");
        rt_write_cstr(1, info->c2pa.content_hash_checked ? "true" : "false");
        rt_write_cstr(1, ",\"content_hash_matched\":");
        rt_write_cstr(1, info->c2pa.content_hash_matched ? "true" : "false");
        rt_write_cstr(1, ",\"content_hash_mismatched\":");
        rt_write_cstr(1, info->c2pa.content_hash_mismatched ? "true" : "false");
        rt_write_cstr(1, ",\"signature_verification_supported\":");
        rt_write_cstr(1, info->c2pa.signature_supported ? "true" : "false");
        rt_write_cstr(1, ",\"trust_validation_supported\":");
        rt_write_cstr(1, info->c2pa.trust_supported ? "true" : "false");
        rt_write_cstr(1, ",\"ingredient_count\":");
        rt_write_uint(1, (unsigned long long)info->c2pa.ingredient_count);
        rt_write_char(1, '}');
    } else {
        rt_write_cstr(1, "{\"present\":false}");
    }
    rt_write_line(1, "}");
}

static int describe_path(const char *path, const ImginfoOptions *options) {
    unsigned char buffer[IMGINFO_READ_LIMIT];
    unsigned char *full_data = 0;
    size_t size = 0U;
    const unsigned char *probe_data = buffer;
    ImageInfo info;
    const char *label = path ? path : "stdin";

    if (path == 0) {
        if (read_all_input(path, &full_data, &size) != 0) {
            return -1;
        }
        probe_data = full_data;
    } else if (read_probe_data(path, buffer, sizeof(buffer), &size) != 0) {
        return -1;
    }
    if (image_probe(probe_data, size, &info) != 0) {
        if (full_data != 0) {
            rt_free(full_data);
        }
        tool_write_error("imginfo", "unsupported image format: ", label);
        return -1;
    }
    if (path != 0 && size == sizeof(buffer) &&
        (((info.format == IMAGE_FORMAT_JPEG) && (info.flags & IMAGE_INFO_HAS_DIMENSIONS) == 0U) ||
         (info.flags & IMAGE_INFO_HAS_C2PA) == 0U)) {
        unsigned char *full_data;
        size_t full_size;
        ImageInfo full_info;

        if (read_all_input(path, &full_data, &full_size) == 0) {
            if (image_probe(full_data, full_size, &full_info) == 0) {
                info = full_info;
            }
            rt_free(full_data);
        }
    }
    warn_extension_mismatch(path, &info);
    if (full_data != 0) {
        rt_free(full_data);
    }
    if (options->canonical_ext_only) {
        rt_write_line(1, image_format_extension(info.format));
        return 0;
    }
    if (options->mime_only) {
        rt_write_cstr(1, label);
        rt_write_cstr(1, ": ");
        rt_write_line(1, image_format_mime(info.format));
        return 0;
    }
    if (options->machine_output) {
        return write_machine_line(label, &info);
    }
    if (options->detailed_output) {
        return write_detail_block(label, &info);
    }
    if (options->json_output) {
        write_json_info(label, &info);
        return 0;
    }
    return write_human_line(label, &info);
}

static int describe_path_recursive(const char *path, const ImginfoOptions *options) {
    PlatformDirEntry entries[IMGINFO_ENTRY_CAPACITY];
    size_t count = 0U;
    int is_directory = 0;
    int status = 0;
    size_t index;

    if (!options->recursive || platform_collect_entries(path, 1, entries, IMGINFO_ENTRY_CAPACITY, &count, &is_directory) != 0) {
        return describe_path(path, options);
    }
    if (!is_directory) {
        platform_free_entries(entries, count);
        return describe_path(path, options);
    }
    for (index = 0U; index < count; ++index) {
        char child_path[IMGINFO_PATH_CAPACITY];

        if (rt_strcmp(entries[index].name, ".") == 0 || rt_strcmp(entries[index].name, "..") == 0) {
            continue;
        }
        if (tool_join_path(path, entries[index].name, child_path, sizeof(child_path)) != 0) {
            status = 1;
            continue;
        }
        if (entries[index].is_dir) {
            if (describe_path_recursive(child_path, options) != 0) {
                status = 1;
            }
        } else if (describe_path(child_path, options) != 0) {
            status = 1;
        }
    }
    platform_free_entries(entries, count);
    return status == 0 ? 0 : -1;
}

static int parse_options(int argc, char **argv, ImginfoOptions *options, int *arg_index_out) {
    int argi = 1;

    options->mime_only = 0;
    options->machine_output = 0;
    options->detailed_output = 0;
    options->json_output = 0;
    options->canonical_ext_only = 0;
    options->recursive = 0;
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *arg = argv[argi];

        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 1;
        }
        if (rt_strcmp(arg, "-m") == 0 || rt_strcmp(arg, "--mime") == 0) {
            options->mime_only = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-p") == 0 || rt_strcmp(arg, "--plain") == 0) {
            options->machine_output = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-d") == 0 || rt_strcmp(arg, "--details") == 0) {
            options->detailed_output = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "--json") == 0) {
            options->json_output = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "--canonical-ext") == 0) {
            options->canonical_ext_only = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "-R") == 0 || rt_strcmp(arg, "--recursive") == 0) {
            options->recursive = 1;
            argi += 1;
            continue;
        }
        tool_write_error("imginfo", "unknown option: ", arg);
        print_usage();
        return -1;
    }
    *arg_index_out = argi;
    return 0;
}

int main(int argc, char **argv) {
    ImginfoOptions options;
    int argi;
    int parse_result;
    int status = 0;

    parse_result = parse_options(argc, argv, &options, &argi);
    if (parse_result > 0) {
        return 0;
    }
    if (parse_result < 0) {
        return 1;
    }
    if (argi >= argc) {
        return describe_path(0, &options) == 0 ? 0 : 1;
    }
    while (argi < argc) {
        if (describe_path_recursive(argv[argi], &options) != 0) {
            status = 1;
        }
        argi += 1;
    }
    return status;
}
