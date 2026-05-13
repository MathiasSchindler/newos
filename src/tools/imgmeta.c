#include "image/image.h"
#include "compression/crc32.h"
#include "compression/zlib.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define IMGMETA_INITIAL_CAPACITY (64U * 1024U)

static void print_usage(void) {
    tool_write_usage("imgmeta", "show [-v|--verbose] [--c2pa-trust] FILE ... | list-text FILE ... | strip -o OUTPUT FILE | copy -o OUTPUT FILE | edit [--set-text KEY=VALUE|--set-itxt KEY=VALUE|--remove-text KEY] [--language TAG] [--compressed] -o OUTPUT FILE");
}

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t pos;
    int valid;
} ImgmetaCborReader;

typedef struct {
    const unsigned char *text;
    size_t size;
} ImgmetaTextRef;

typedef struct {
    const unsigned char *bytes;
    size_t size;
} ImgmetaBytesRef;

typedef struct {
    unsigned int manifest_index;
    unsigned int assertion_store_index;
    unsigned int claim_index;
    unsigned int assertion_index;
    unsigned int signature_index;
} ImgmetaC2paVerboseState;

static unsigned int read_u16_be(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 8) | (unsigned int)bytes[1];
}

static unsigned int read_u16_le(const unsigned char *bytes) {
    return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8);
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

static unsigned int read_tiff_u16(const unsigned char *bytes, int little_endian) {
    return little_endian ? read_u16_le(bytes) : read_u16_be(bytes);
}

static unsigned int read_tiff_u32(const unsigned char *bytes, int little_endian) {
    return little_endian ? read_u32_le(bytes) : read_u32_be(bytes);
}

static void write_u16_le(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8) & 0xffU);
}

static void write_u16_be(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)((value >> 8) & 0xffU);
    bytes[1] = (unsigned char)(value & 0xffU);
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

static void write_tiff_u16(unsigned char *bytes, unsigned int value, int little_endian) {
    if (little_endian) {
        write_u16_le(bytes, value);
    } else {
        write_u16_be(bytes, value);
    }
}

static void write_tiff_u32(unsigned char *bytes, unsigned int value, int little_endian) {
    if (little_endian) {
        write_u32_le(bytes, value);
    } else {
        write_u32_be(bytes, value);
    }
}

static int tiff_u64_high_is_zero(const unsigned char *bytes, int little_endian) {
    return little_endian ? read_u32_le(bytes + 4U) == 0U : read_u32_be(bytes) == 0U;
}

static unsigned int read_tiff_u64_low(const unsigned char *bytes, int little_endian) {
    return little_endian ? read_u32_le(bytes) : read_u32_be(bytes + 4U);
}

static void write_tiff_u64_low(unsigned char *bytes, unsigned int value, int little_endian) {
    if (little_endian) {
        write_u32_le(bytes, value);
        write_u32_le(bytes + 4U, 0U);
    } else {
        write_u32_be(bytes, 0U);
        write_u32_be(bytes + 4U, value);
    }
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
        IMAGE_PROPERTY_C2PA,
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

static int bytes_start_with_text(const unsigned char *bytes, size_t size, const char *text) {
    size_t text_size = rt_strlen(text);

    return size >= text_size && bytes_equal(bytes, text, text_size);
}

static int text_ref_equals(ImgmetaTextRef ref, const char *text) {
    size_t text_size = rt_strlen(text);

    return ref.size == text_size && bytes_equal(ref.text, text, text_size);
}

static int text_ref_starts_with(ImgmetaTextRef ref, const char *text) {
    size_t text_size = rt_strlen(text);

    return ref.size >= text_size && bytes_equal(ref.text, text, text_size);
}

static void write_text_ref(int fd, ImgmetaTextRef ref) {
    (void)rt_write_all(fd, ref.text, ref.size);
}

static void write_hex_bytes(int fd, const unsigned char *bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < size; ++index) {
        rt_write_char(fd, digits[(bytes[index] >> 4U) & 0x0fU]);
        rt_write_char(fd, digits[bytes[index] & 0x0fU]);
    }
}

static void write_c2pa_assertion_label(ImgmetaTextRef url) {
    size_t index = url.size;

    while (index > 0U) {
        if (url.text[index - 1U] == '/') {
            break;
        }
        index -= 1U;
    }
    write_text_ref(1, (ImgmetaTextRef){url.text + index, url.size - index});
}

static void imgmeta_cbor_init(ImgmetaCborReader *reader, const unsigned char *data, size_t size) {
    reader->data = data;
    reader->size = size;
    reader->pos = 0U;
    reader->valid = 1;
}

static int imgmeta_cbor_read_type(ImgmetaCborReader *reader, unsigned int *major, unsigned long long *value) {
    unsigned char initial;
    unsigned int addl;
    unsigned int byte_count = 0U;
    unsigned int index;

    if (reader->pos >= reader->size) {
        reader->valid = 0;
        return 0;
    }
    initial = reader->data[reader->pos++];
    *major = (unsigned int)(initial >> 5U);
    addl = (unsigned int)(initial & 0x1fU);
    if (addl < 24U) {
        *value = (unsigned long long)addl;
        return 1;
    }
    if (addl == 24U) byte_count = 1U;
    else if (addl == 25U) byte_count = 2U;
    else if (addl == 26U) byte_count = 4U;
    else if (addl == 27U) byte_count = 8U;
    else {
        reader->valid = 0;
        return 0;
    }
    if (reader->pos + (size_t)byte_count > reader->size) {
        reader->valid = 0;
        return 0;
    }
    *value = 0ULL;
    for (index = 0U; index < byte_count; ++index) {
        *value = (*value << 8U) | (unsigned long long)reader->data[reader->pos++];
    }
    return 1;
}

static int imgmeta_cbor_skip(ImgmetaCborReader *reader) {
    unsigned int major;
    unsigned long long value;
    unsigned long long index;

    if (!imgmeta_cbor_read_type(reader, &major, &value)) return 0;
    if (major == 0U || major == 1U || major == 7U) return 1;
    if (major == 2U || major == 3U) {
        if (value > (unsigned long long)(reader->size - reader->pos)) {
            reader->valid = 0;
            return 0;
        }
        reader->pos += (size_t)value;
        return 1;
    }
    if (major == 4U) {
        for (index = 0ULL; index < value; ++index) {
            if (!imgmeta_cbor_skip(reader)) return 0;
        }
        return 1;
    }
    if (major == 5U) {
        for (index = 0ULL; index < value; ++index) {
            if (!imgmeta_cbor_skip(reader) || !imgmeta_cbor_skip(reader)) return 0;
        }
        return 1;
    }
    if (major == 6U) {
        return imgmeta_cbor_skip(reader);
    }
    reader->valid = 0;
    return 0;
}

static int imgmeta_cbor_read_text_ref(ImgmetaCborReader *reader, ImgmetaTextRef *ref) {
    unsigned int major;
    unsigned long long value;

    if (!imgmeta_cbor_read_type(reader, &major, &value) || major != 3U || value > (unsigned long long)(reader->size - reader->pos)) {
        reader->valid = 0;
        return 0;
    }
    ref->text = reader->data + reader->pos;
    ref->size = (size_t)value;
    reader->pos += (size_t)value;
    return 1;
}

static int imgmeta_cbor_read_bytes_ref(ImgmetaCborReader *reader, ImgmetaBytesRef *ref) {
    unsigned int major;
    unsigned long long value;

    if (!imgmeta_cbor_read_type(reader, &major, &value) || major != 2U || value > (unsigned long long)(reader->size - reader->pos)) {
        reader->valid = 0;
        return 0;
    }
    ref->bytes = reader->data + reader->pos;
    ref->size = (size_t)value;
    reader->pos += (size_t)value;
    return 1;
}

static int imgmeta_cbor_item_slice(ImgmetaCborReader *reader, const unsigned char **data, size_t *size) {
    size_t start = reader->pos;

    if (!imgmeta_cbor_skip(reader)) return 0;
    *data = reader->data + start;
    *size = reader->pos - start;
    return 1;
}

static const char *imgmeta_cose_alg_name(long long alg) {
    if (alg == -7LL) return "ES256";
    return "unknown";
}

static void write_hash_ref_line(const char *prefix, ImgmetaTextRef url, ImgmetaBytesRef hash) {
    if (url.text == 0 && hash.bytes == 0) return;
    rt_write_cstr(1, prefix);
    if (url.text != 0) {
        write_c2pa_assertion_label(url);
    } else {
        rt_write_char(1, '-');
    }
    if (hash.bytes != 0) {
        rt_write_cstr(1, " sha256=");
        write_hex_bytes(1, hash.bytes, hash.size);
    }
    rt_write_char(1, '\n');
}

static int imgmeta_parse_url_hash_map(const unsigned char *data, size_t size, ImgmetaTextRef *url, ImgmetaBytesRef *hash) {
    ImgmetaCborReader reader;
    unsigned int major;
    unsigned long long count;
    unsigned long long index;

    url->text = 0;
    url->size = 0U;
    hash->bytes = 0;
    hash->size = 0U;
    imgmeta_cbor_init(&reader, data, size);
    if (!imgmeta_cbor_read_type(&reader, &major, &count) || major != 5U) return 0;
    for (index = 0ULL; index < count && reader.valid; ++index) {
        ImgmetaTextRef key;
        if (!imgmeta_cbor_read_text_ref(&reader, &key)) return 0;
        if (text_ref_equals(key, "url")) {
            (void)imgmeta_cbor_read_text_ref(&reader, url);
        } else if (text_ref_equals(key, "hash")) {
            (void)imgmeta_cbor_read_bytes_ref(&reader, hash);
        } else if (!imgmeta_cbor_skip(&reader)) {
            return 0;
        }
    }
    return reader.valid;
}

static void imgmeta_print_claim_generator(const unsigned char *data, size_t size) {
    ImgmetaCborReader reader;
    unsigned int major;
    unsigned long long count;
    unsigned long long index;
    ImgmetaTextRef name = {0, 0U};
    ImgmetaTextRef version = {0, 0U};

    imgmeta_cbor_init(&reader, data, size);
    if (!imgmeta_cbor_read_type(&reader, &major, &count) || major != 5U) return;
    for (index = 0ULL; index < count && reader.valid; ++index) {
        ImgmetaTextRef key;
        size_t value_start;
        if (!imgmeta_cbor_read_text_ref(&reader, &key)) return;
        value_start = reader.pos;
        if (text_ref_equals(key, "name")) {
            (void)imgmeta_cbor_read_text_ref(&reader, &name);
        } else if (text_ref_equals(key, "version")) {
            (void)imgmeta_cbor_read_text_ref(&reader, &version);
        } else {
            reader.pos = value_start;
            if (!imgmeta_cbor_skip(&reader)) return;
        }
    }
    if (name.text != 0) {
        rt_write_cstr(1, "    generator: ");
        write_text_ref(1, name);
        if (version.text != 0) {
            rt_write_char(1, ' ');
            write_text_ref(1, version);
        }
        rt_write_char(1, '\n');
    }
}

static void imgmeta_print_claim_assertion(ImgmetaCborReader *reader) {
    unsigned int major;
    unsigned long long count;
    unsigned long long index;
    ImgmetaTextRef url = {0, 0U};
    ImgmetaBytesRef hash = {0, 0U};

    if (!imgmeta_cbor_read_type(reader, &major, &count) || major != 5U) {
        reader->valid = 0;
        return;
    }
    for (index = 0ULL; index < count && reader->valid; ++index) {
        ImgmetaTextRef key;
        if (!imgmeta_cbor_read_text_ref(reader, &key)) return;
        if (text_ref_equals(key, "url")) {
            (void)imgmeta_cbor_read_text_ref(reader, &url);
        } else if (text_ref_equals(key, "hash")) {
            (void)imgmeta_cbor_read_bytes_ref(reader, &hash);
        } else if (!imgmeta_cbor_skip(reader)) {
            return;
        }
    }
    if (url.text != 0) {
        rt_write_cstr(1, "      - ");
        write_c2pa_assertion_label(url);
        if (hash.bytes != 0) {
            rt_write_cstr(1, " sha256=");
            write_hex_bytes(1, hash.bytes, hash.size);
        }
        rt_write_char(1, '\n');
    }
}

static void imgmeta_print_claim_assertions(const unsigned char *data, size_t size, const char *heading) {
    ImgmetaCborReader reader;
    unsigned int major;
    unsigned long long count;
    unsigned long long index;

    imgmeta_cbor_init(&reader, data, size);
    if (!imgmeta_cbor_read_type(&reader, &major, &count) || major != 4U) return;
    rt_write_cstr(1, "    ");
    rt_write_cstr(1, heading);
    rt_write_line(1, ":");
    for (index = 0ULL; index < count && reader.valid; ++index) {
        imgmeta_print_claim_assertion(&reader);
    }
}

static void imgmeta_print_c2pa_claim(unsigned int claim_index, ImgmetaTextRef label, const unsigned char *data, size_t size) {
    ImgmetaCborReader reader;
    unsigned int major;
    unsigned long long count;
    unsigned long long index;

    rt_write_cstr(1, "  c2pa-claim[");
    rt_write_uint(1, (unsigned long long)claim_index);
    rt_write_line(1, "]:");
    rt_write_cstr(1, "    label: ");
    write_text_ref(1, label);
    rt_write_char(1, '\n');

    imgmeta_cbor_init(&reader, data, size);
    if (!imgmeta_cbor_read_type(&reader, &major, &count) || major != 5U) {
        rt_write_line(1, "    parse: unsupported claim shape");
        return;
    }
    for (index = 0ULL; index < count && reader.valid; ++index) {
        ImgmetaTextRef key;
        size_t value_start;
        size_t value_end;
        if (!imgmeta_cbor_read_text_ref(&reader, &key)) return;
        value_start = reader.pos;
        if (!imgmeta_cbor_skip(&reader)) return;
        value_end = reader.pos;
        reader.pos = value_start;
        if (text_ref_equals(key, "instanceID")) {
            ImgmetaTextRef value;
            if (imgmeta_cbor_read_text_ref(&reader, &value)) {
                rt_write_cstr(1, "    instance-id: ");
                write_text_ref(1, value);
                rt_write_char(1, '\n');
            }
        } else if (text_ref_equals(key, "claim_generator_info")) {
            imgmeta_print_claim_generator(reader.data + value_start, value_end - value_start);
            reader.pos = value_end;
        } else if (text_ref_equals(key, "signature")) {
            ImgmetaTextRef value;
            if (imgmeta_cbor_read_text_ref(&reader, &value)) {
                rt_write_cstr(1, "    signature: ");
                write_text_ref(1, value);
                rt_write_char(1, '\n');
            }
        } else if (text_ref_equals(key, "alg")) {
            ImgmetaTextRef value;
            if (imgmeta_cbor_read_text_ref(&reader, &value)) {
                rt_write_cstr(1, "    assertion-hash-algorithm: ");
                write_text_ref(1, value);
                rt_write_char(1, '\n');
            }
        } else if (text_ref_equals(key, "created_assertions")) {
            imgmeta_print_claim_assertions(reader.data + value_start, value_end - value_start, "created-assertions");
            reader.pos = value_end;
        } else if (text_ref_equals(key, "gathered_assertions")) {
            imgmeta_print_claim_assertions(reader.data + value_start, value_end - value_start, "gathered-assertions");
            reader.pos = value_end;
        } else {
            reader.pos = value_end;
        }
    }
}

static void imgmeta_print_c2pa_signature(unsigned int signature_index, ImgmetaTextRef label, const unsigned char *data, size_t size) {
    ImgmetaCborReader reader;
    unsigned int major;
    unsigned long long value;
    const unsigned char *protected_data = 0;
    const unsigned char *unprotected_data = 0;
    const unsigned char *payload_data = 0;
    const unsigned char *signature_data = 0;
    size_t protected_size = 0U;
    size_t protected_content_size = 0U;
    size_t unprotected_size = 0U;
    size_t payload_size = 0U;
    size_t signature_item_size = 0U;
    ImgmetaBytesRef signature = {0, 0U};
    long long cose_alg = 0LL;
    unsigned long long x509_count = 0ULL;
    unsigned long long timestamp_count = 0ULL;

    rt_write_cstr(1, "  c2pa-signature[");
    rt_write_uint(1, (unsigned long long)signature_index);
    rt_write_line(1, "]:");
    rt_write_cstr(1, "    label: ");
    write_text_ref(1, label);
    rt_write_char(1, '\n');

    imgmeta_cbor_init(&reader, data, size);
    if (!imgmeta_cbor_read_type(&reader, &major, &value)) return;
    if (major == 6U) {
        rt_write_cstr(1, "    cose-tag: ");
        rt_write_uint(1, value);
        rt_write_char(1, '\n');
        if (!imgmeta_cbor_read_type(&reader, &major, &value)) return;
    }
    if (major != 4U || value != 4ULL) {
        rt_write_line(1, "    parse: unsupported signature shape");
        return;
    }
    if (!imgmeta_cbor_item_slice(&reader, &protected_data, &protected_size)) return;
    if (!imgmeta_cbor_item_slice(&reader, &unprotected_data, &unprotected_size)) return;
    if (!imgmeta_cbor_item_slice(&reader, &payload_data, &payload_size)) return;
    signature_item_size = reader.pos;
    if (!imgmeta_cbor_read_bytes_ref(&reader, &signature)) return;
    signature_item_size = reader.pos - signature_item_size;

    if (protected_data != 0) {
        ImgmetaCborReader protected_reader;
        ImgmetaBytesRef protected_bytes;
        imgmeta_cbor_init(&protected_reader, protected_data, protected_size);
        if (imgmeta_cbor_read_bytes_ref(&protected_reader, &protected_bytes)) {
            ImgmetaCborReader map_reader;
            imgmeta_cbor_init(&map_reader, protected_bytes.bytes, protected_bytes.size);
            protected_content_size = protected_bytes.size;
            if (imgmeta_cbor_read_type(&map_reader, &major, &value) && major == 5U) {
                unsigned long long index;
                for (index = 0ULL; index < value && map_reader.valid; ++index) {
                    unsigned int key_major;
                    unsigned long long key_value;
                    if (!imgmeta_cbor_read_type(&map_reader, &key_major, &key_value)) break;
                    if (key_major == 0U && key_value == 1ULL) {
                        unsigned int value_major;
                        unsigned long long alg_value;
                        if (imgmeta_cbor_read_type(&map_reader, &value_major, &alg_value)) {
                            if (value_major == 1U) cose_alg = -1LL - (long long)alg_value;
                            else if (value_major == 0U) cose_alg = (long long)alg_value;
                        }
                    } else if (key_major == 0U && key_value == 33ULL) {
                        unsigned int value_major;
                        unsigned long long item_count;
                        if (imgmeta_cbor_read_type(&map_reader, &value_major, &item_count)) {
                            if (value_major == 2U) {
                                x509_count = 1ULL;
                                if (item_count <= (unsigned long long)(map_reader.size - map_reader.pos)) map_reader.pos += (size_t)item_count;
                                else map_reader.valid = 0;
                            } else if (value_major == 4U) {
                                unsigned long long cert_index;
                                x509_count = item_count;
                                for (cert_index = 0ULL; cert_index < item_count && map_reader.valid; ++cert_index) {
                                    if (!imgmeta_cbor_skip(&map_reader)) break;
                                }
                            } else if (!imgmeta_cbor_skip(&map_reader)) {
                                break;
                            }
                        }
                    } else if (!imgmeta_cbor_skip(&map_reader)) {
                        break;
                    }
                }
            }
        }
    }

    if (unprotected_data != 0) {
        ImgmetaCborReader unprotected_reader;
        imgmeta_cbor_init(&unprotected_reader, unprotected_data, unprotected_size);
        if (imgmeta_cbor_read_type(&unprotected_reader, &major, &value) && major == 5U) {
            unsigned long long index;
            for (index = 0ULL; index < value && unprotected_reader.valid; ++index) {
                ImgmetaTextRef key;
                size_t value_start;
                if (!imgmeta_cbor_read_text_ref(&unprotected_reader, &key)) break;
                value_start = unprotected_reader.pos;
                if (text_ref_equals(key, "sigTst2")) {
                    ImgmetaCborReader tst_reader;
                    imgmeta_cbor_init(&tst_reader, unprotected_reader.data + value_start, unprotected_reader.size - value_start);
                    if (imgmeta_cbor_read_type(&tst_reader, &major, &value) && major == 5U) {
                        unsigned long long map_index;
                        for (map_index = 0ULL; map_index < value && tst_reader.valid; ++map_index) {
                            ImgmetaTextRef tst_key;
                            if (!imgmeta_cbor_read_text_ref(&tst_reader, &tst_key)) break;
                            if (text_ref_equals(tst_key, "tstTokens")) {
                                if (imgmeta_cbor_read_type(&tst_reader, &major, &timestamp_count) && major != 4U) tst_reader.valid = 0;
                                break;
                            } else if (!imgmeta_cbor_skip(&tst_reader)) {
                                break;
                            }
                        }
                    }
                }
                unprotected_reader.pos = value_start;
                if (!imgmeta_cbor_skip(&unprotected_reader)) break;
            }
        }
    }

    rt_write_cstr(1, "    type: COSE_Sign1\n    algorithm: ");
    rt_write_line(1, imgmeta_cose_alg_name(cose_alg));
    rt_write_cstr(1, "    protected: ");
    rt_write_uint(1, (unsigned long long)(protected_content_size != 0U ? protected_content_size : protected_size));
    rt_write_line(1, " bytes");
    if (x509_count != 0ULL) {
        rt_write_cstr(1, "    x509-certificates: ");
        rt_write_uint(1, x509_count);
        rt_write_char(1, '\n');
    }
    if (timestamp_count != 0ULL) {
        rt_write_cstr(1, "    timestamp-tokens: ");
        rt_write_uint(1, timestamp_count);
        rt_write_char(1, '\n');
    }
    rt_write_cstr(1, "    unprotected: ");
    rt_write_uint(1, (unsigned long long)unprotected_size);
    rt_write_line(1, " bytes");
    rt_write_cstr(1, "    payload: ");
    if (payload_size == 1U && payload_data[0] == 0xf6U) {
        rt_write_line(1, "detached");
    } else {
        rt_write_uint(1, (unsigned long long)payload_size);
        rt_write_line(1, " encoded bytes");
    }
    rt_write_cstr(1, "    signature-bytes: ");
    rt_write_uint(1, (unsigned long long)signature.size);
    rt_write_char(1, '\n');
    (void)signature_data;
    (void)signature_item_size;
}

static void imgmeta_print_hash_assertion(unsigned int assertion_index, ImgmetaTextRef label, const unsigned char *data, size_t size) {
    ImgmetaCborReader reader;
    unsigned int major;
    unsigned long long count;
    unsigned long long index;
    ImgmetaTextRef alg = {0, 0U};
    ImgmetaBytesRef hash = {0, 0U};
    unsigned long long exclusion_count = 0ULL;

    rt_write_cstr(1, "  c2pa-assertion[");
    rt_write_uint(1, (unsigned long long)assertion_index);
    rt_write_line(1, "]:");
    rt_write_cstr(1, "    label: ");
    write_text_ref(1, label);
    rt_write_char(1, '\n');

    imgmeta_cbor_init(&reader, data, size);
    if (!imgmeta_cbor_read_type(&reader, &major, &count) || major != 5U) return;
    for (index = 0ULL; index < count && reader.valid; ++index) {
        ImgmetaTextRef key;
        size_t value_start;
        if (!imgmeta_cbor_read_text_ref(&reader, &key)) return;
        value_start = reader.pos;
        if (text_ref_equals(key, "alg")) {
            (void)imgmeta_cbor_read_text_ref(&reader, &alg);
        } else if (text_ref_equals(key, "hash")) {
            (void)imgmeta_cbor_read_bytes_ref(&reader, &hash);
        } else if (text_ref_equals(key, "exclusions")) {
            if (imgmeta_cbor_read_type(&reader, &major, &exclusion_count) && major != 4U) {
                reader.valid = 0;
            }
            reader.pos = value_start;
            if (!imgmeta_cbor_skip(&reader)) return;
        } else if (!imgmeta_cbor_skip(&reader)) {
            return;
        }
    }
    if (alg.text != 0) {
        rt_write_cstr(1, "    algorithm: ");
        write_text_ref(1, alg);
        rt_write_char(1, '\n');
    }
    if (hash.bytes != 0) {
        rt_write_cstr(1, "    hash: ");
        write_hex_bytes(1, hash.bytes, hash.size);
        rt_write_char(1, '\n');
    }
    rt_write_cstr(1, "    exclusions: ");
    rt_write_uint(1, exclusion_count);
    rt_write_char(1, '\n');
}

static void imgmeta_print_action_item(ImgmetaCborReader *reader) {
    unsigned int major;
    unsigned long long count;
    unsigned long long index;
    ImgmetaTextRef action = {0, 0U};
    ImgmetaTextRef description = {0, 0U};
    ImgmetaTextRef digital_source = {0, 0U};

    if (!imgmeta_cbor_read_type(reader, &major, &count) || major != 5U) {
        reader->valid = 0;
        return;
    }
    for (index = 0ULL; index < count && reader->valid; ++index) {
        ImgmetaTextRef key;
        if (!imgmeta_cbor_read_text_ref(reader, &key)) return;
        if (text_ref_equals(key, "action")) {
            (void)imgmeta_cbor_read_text_ref(reader, &action);
        } else if (text_ref_equals(key, "description")) {
            (void)imgmeta_cbor_read_text_ref(reader, &description);
        } else if (text_ref_equals(key, "digitalSourceType")) {
            (void)imgmeta_cbor_read_text_ref(reader, &digital_source);
        } else if (!imgmeta_cbor_skip(reader)) {
            return;
        }
    }
    if (action.text != 0) {
        rt_write_cstr(1, "      - ");
        write_text_ref(1, action);
        if (description.text != 0) {
            rt_write_cstr(1, ": ");
            write_text_ref(1, description);
        }
        if (digital_source.text != 0) {
            rt_write_cstr(1, " [");
            write_c2pa_assertion_label(digital_source);
            rt_write_char(1, ']');
        }
        rt_write_char(1, '\n');
    }
}

static void imgmeta_print_actions_assertion(unsigned int assertion_index, ImgmetaTextRef label, const unsigned char *data, size_t size) {
    ImgmetaCborReader reader;
    ImgmetaCborReader actions_reader;
    unsigned int major;
    unsigned long long count;
    unsigned long long index;
    const unsigned char *actions_data = 0;
    size_t actions_size = 0U;

    rt_write_cstr(1, "  c2pa-assertion[");
    rt_write_uint(1, (unsigned long long)assertion_index);
    rt_write_line(1, "]:");
    rt_write_cstr(1, "    label: ");
    write_text_ref(1, label);
    rt_write_char(1, '\n');

    imgmeta_cbor_init(&reader, data, size);
    if (!imgmeta_cbor_read_type(&reader, &major, &count) || major != 5U) return;
    for (index = 0ULL; index < count && reader.valid; ++index) {
        ImgmetaTextRef key;
        if (!imgmeta_cbor_read_text_ref(&reader, &key)) return;
        if (text_ref_equals(key, "actions")) {
            if (!imgmeta_cbor_item_slice(&reader, &actions_data, &actions_size)) return;
        } else if (!imgmeta_cbor_skip(&reader)) {
            return;
        }
    }
    if (actions_data == 0) return;
    imgmeta_cbor_init(&actions_reader, actions_data, actions_size);
    if (!imgmeta_cbor_read_type(&actions_reader, &major, &count) || major != 4U) return;
    rt_write_cstr(1, "    actions: ");
    rt_write_uint(1, count);
    rt_write_char(1, '\n');
    for (index = 0ULL; index < count && actions_reader.valid; ++index) {
        imgmeta_print_action_item(&actions_reader);
    }
}

static void imgmeta_print_ingredient_assertion(unsigned int assertion_index, ImgmetaTextRef label, const unsigned char *data, size_t size) {
    ImgmetaCborReader reader;
    unsigned int major;
    unsigned long long count;
    unsigned long long index;
    ImgmetaTextRef relationship = {0, 0U};
    const unsigned char *active_manifest_data = 0;
    const unsigned char *claim_signature_data = 0;
    size_t active_manifest_size = 0U;
    size_t claim_signature_size = 0U;

    rt_write_cstr(1, "  c2pa-assertion[");
    rt_write_uint(1, (unsigned long long)assertion_index);
    rt_write_line(1, "]:");
    rt_write_cstr(1, "    label: ");
    write_text_ref(1, label);
    rt_write_char(1, '\n');

    imgmeta_cbor_init(&reader, data, size);
    if (!imgmeta_cbor_read_type(&reader, &major, &count) || major != 5U) return;
    for (index = 0ULL; index < count && reader.valid; ++index) {
        ImgmetaTextRef key;
        if (!imgmeta_cbor_read_text_ref(&reader, &key)) return;
        if (text_ref_equals(key, "relationship")) {
            (void)imgmeta_cbor_read_text_ref(&reader, &relationship);
        } else if (text_ref_equals(key, "activeManifest")) {
            if (!imgmeta_cbor_item_slice(&reader, &active_manifest_data, &active_manifest_size)) return;
        } else if (text_ref_equals(key, "claimSignature")) {
            if (!imgmeta_cbor_item_slice(&reader, &claim_signature_data, &claim_signature_size)) return;
        } else if (!imgmeta_cbor_skip(&reader)) {
            return;
        }
    }
    if (relationship.text != 0) {
        rt_write_cstr(1, "    relationship: ");
        write_text_ref(1, relationship);
        rt_write_char(1, '\n');
    }
    if (active_manifest_data != 0) {
        ImgmetaTextRef url;
        ImgmetaBytesRef hash;
        if (imgmeta_parse_url_hash_map(active_manifest_data, active_manifest_size, &url, &hash)) {
            write_hash_ref_line("    active-manifest: ", url, hash);
        }
    }
    if (claim_signature_data != 0) {
        ImgmetaTextRef url;
        ImgmetaBytesRef hash;
        if (imgmeta_parse_url_hash_map(claim_signature_data, claim_signature_size, &url, &hash)) {
            write_hash_ref_line("    claim-signature: ", url, hash);
        }
    }
}

static void imgmeta_print_generic_assertion(unsigned int assertion_index, ImgmetaTextRef label, const unsigned char *data, size_t size) {
    ImgmetaCborReader reader;
    unsigned int major;
    unsigned long long count;

    rt_write_cstr(1, "  c2pa-assertion[");
    rt_write_uint(1, (unsigned long long)assertion_index);
    rt_write_line(1, "]:");
    rt_write_cstr(1, "    label: ");
    write_text_ref(1, label);
    rt_write_char(1, '\n');
    imgmeta_cbor_init(&reader, data, size);
    if (imgmeta_cbor_read_type(&reader, &major, &count)) {
        rt_write_cstr(1, "    cbor: ");
        if (major == 4U) rt_write_cstr(1, "array ");
        else if (major == 5U) rt_write_cstr(1, "map ");
        else if (major == 2U) rt_write_cstr(1, "bytes ");
        else if (major == 3U) rt_write_cstr(1, "text ");
        else rt_write_cstr(1, "type ");
        rt_write_uint(1, count);
        rt_write_char(1, '\n');
    }
}

static void imgmeta_print_c2pa_assertion(unsigned int assertion_index, ImgmetaTextRef label, const unsigned char *data, size_t size) {
    if (text_ref_starts_with(label, "c2pa.hash.")) {
        imgmeta_print_hash_assertion(assertion_index, label, data, size);
    } else if (text_ref_starts_with(label, "c2pa.actions")) {
        imgmeta_print_actions_assertion(assertion_index, label, data, size);
    } else if (text_ref_starts_with(label, "c2pa.ingredient")) {
        imgmeta_print_ingredient_assertion(assertion_index, label, data, size);
    } else {
        imgmeta_print_generic_assertion(assertion_index, label, data, size);
    }
}

static int imgmeta_find_label_with_prefix(const unsigned char *data, size_t size, const char *prefix, ImgmetaTextRef *label) {
    size_t prefix_size = rt_strlen(prefix);
    size_t offset;

    if (size < prefix_size) return 0;
    for (offset = 0U; offset + prefix_size <= size; ++offset) {
        if (bytes_equal(data + offset, prefix, prefix_size)) {
            size_t end = offset;
            while (end < size && data[end] >= 0x20U && data[end] <= 0x7eU) {
                end += 1U;
            }
            label->text = data + offset;
            label->size = end - offset;
            return label->size > 0U;
        }
    }
    return 0;
}

static int imgmeta_jumd_label(const unsigned char *data, size_t size, ImgmetaTextRef *label) {
    label->text = 0;
    label->size = 0U;
    if (imgmeta_find_label_with_prefix(data, size, "urn:c2pa:", label)) return 1;
    if (imgmeta_find_label_with_prefix(data, size, "c2pa.", label)) return 1;
    if (imgmeta_find_label_with_prefix(data, size, "c2pa", label)) return 1;
    if (imgmeta_find_label_with_prefix(data, size, "cbor", label)) return 1;
    return 0;
}

static void imgmeta_verbose_c2pa_boxes(const unsigned char *data,
                                       size_t size,
                                       unsigned int depth,
                                       ImgmetaTextRef parent_label,
                                       ImgmetaC2paVerboseState *state) {
    size_t offset = 0U;

    if (depth > 16U) return;
    while (offset + 8U <= size) {
        unsigned long long box_size = (unsigned long long)read_u32_be(data + offset);
        const unsigned char *type = data + offset + 4U;
        size_t header_size = 8U;
        size_t payload_offset;
        size_t payload_size;
        ImgmetaTextRef label = {0, 0U};

        if (box_size == 1ULL) {
            if (offset + 16U > size) return;
            box_size = ((unsigned long long)read_u32_be(data + offset + 8U) << 32U) | (unsigned long long)read_u32_be(data + offset + 12U);
            header_size = 16U;
        } else if (box_size == 0ULL) {
            box_size = (unsigned long long)(size - offset);
        }
        if (box_size < (unsigned long long)header_size || box_size > (unsigned long long)(size - offset)) return;
        payload_offset = offset + header_size;
        payload_size = (size_t)box_size - header_size;

        if (bytes_equal(type, "jumb", 4U)) {
            if (payload_size >= 8U && bytes_equal(data + payload_offset + 4U, "jumd", 4U)) {
                unsigned long long jumd_size = (unsigned long long)read_u32_be(data + payload_offset);
                if (jumd_size >= 8ULL && jumd_size <= (unsigned long long)payload_size) {
                    (void)imgmeta_jumd_label(data + payload_offset + 8U, (size_t)jumd_size - 8U, &label);
                }
            }
            if (label.text != 0 && text_ref_starts_with(label, "urn:c2pa:")) {
                rt_write_cstr(1, "  c2pa-manifest[");
                rt_write_uint(1, (unsigned long long)state->manifest_index++);
                rt_write_line(1, "]:");
                rt_write_cstr(1, "    label: ");
                write_text_ref(1, label);
                rt_write_char(1, '\n');
            } else if (label.text != 0 && text_ref_starts_with(label, "c2pa.assertions")) {
                rt_write_cstr(1, "  c2pa-assertion-store[");
                rt_write_uint(1, (unsigned long long)state->assertion_store_index++);
                rt_write_line(1, "]:");
                rt_write_cstr(1, "    label: ");
                write_text_ref(1, label);
                rt_write_char(1, '\n');
            }
            imgmeta_verbose_c2pa_boxes(data + payload_offset, payload_size, depth + 1U, label, state);
        } else if (bytes_equal(type, "cbor", 4U) && parent_label.text != 0) {
            if (text_ref_starts_with(parent_label, "c2pa.claim")) {
                imgmeta_print_c2pa_claim(state->claim_index, parent_label, data + payload_offset, payload_size);
                state->claim_index += 1U;
            } else if (text_ref_starts_with(parent_label, "c2pa.signature")) {
                imgmeta_print_c2pa_signature(state->signature_index, parent_label, data + payload_offset, payload_size);
                state->signature_index += 1U;
            } else if (text_ref_starts_with(parent_label, "c2pa.") && !text_ref_starts_with(parent_label, "c2pa.assertions")) {
                imgmeta_print_c2pa_assertion(state->assertion_index, parent_label, data + payload_offset, payload_size);
                state->assertion_index += 1U;
            }
        }
        offset += (size_t)box_size;
    }
}

static void imgmeta_verbose_c2pa_png(const unsigned char *data, size_t size, ImgmetaC2paVerboseState *state) {
    static const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    size_t offset = 8U;

    if (size < 8U || !bytes_equal(data, (const char *)signature, 8U)) return;
    while (offset + 12U <= size) {
        unsigned int chunk_size = read_u32_be(data + offset);
        const unsigned char *type = data + offset + 4U;
        size_t payload_offset = offset + 8U;
        if ((size_t)chunk_size > size - payload_offset || payload_offset + (size_t)chunk_size + 4U > size) return;
        if (bytes_equal(type, "caBX", 4U)) {
            imgmeta_verbose_c2pa_boxes(data + payload_offset, (size_t)chunk_size, 0U, (ImgmetaTextRef){0, 0U}, state);
        }
        offset = payload_offset + (size_t)chunk_size + 4U;
    }
}

static void imgmeta_verbose_c2pa_jpeg(const unsigned char *data, size_t size, ImgmetaC2paVerboseState *state) {
    size_t offset = 2U;

    if (size < 4U || data[0] != 0xffU || data[1] != 0xd8U) return;
    while (offset + 4U <= size) {
        unsigned char marker;
        unsigned int segment_size;
        const unsigned char *payload;
        size_t payload_size;
        size_t index;

        while (offset < size && data[offset] != 0xffU) offset += 1U;
        while (offset < size && data[offset] == 0xffU) offset += 1U;
        if (offset >= size) return;
        marker = data[offset++];
        if (marker == 0xd9U || marker == 0xdaU) return;
        if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd7U)) continue;
        if (offset + 2U > size) return;
        segment_size = read_u16_be(data + offset);
        if (segment_size < 2U || offset + (size_t)segment_size > size) return;
        payload = data + offset + 2U;
        payload_size = (size_t)segment_size - 2U;
        if (marker == 0xebU && payload_size >= 8U && bytes_start_with_text(payload, payload_size, "JP")) {
            for (index = 4U; index + 4U <= payload_size; ++index) {
                if (bytes_equal(payload + index, "jumb", 4U) && index >= 4U) {
                    imgmeta_verbose_c2pa_boxes(payload + index - 4U, payload_size - (index - 4U), 0U, (ImgmetaTextRef){0, 0U}, state);
                    break;
                }
            }
        }
        offset += (size_t)segment_size;
    }
}

static void imgmeta_show_c2pa_verbose(const unsigned char *data, size_t size, const ImageInfo *info) {
    ImgmetaC2paVerboseState state;

    state.manifest_index = 0U;
    state.assertion_store_index = 0U;
    state.claim_index = 0U;
    state.assertion_index = 0U;
    state.signature_index = 0U;

    if ((info->flags & IMAGE_INFO_HAS_C2PA) == 0U) return;
    if (info->format == IMAGE_FORMAT_PNG) {
        imgmeta_verbose_c2pa_png(data, size, &state);
    } else if (info->format == IMAGE_FORMAT_JPEG) {
        imgmeta_verbose_c2pa_jpeg(data, size, &state);
    }
    if (state.claim_index == 0U && info->c2pa.claim_count > 0U) {
        rt_write_line(1, "  c2pa-claims-detail: unavailable for this carrier");
    }
}

static int show_path(const char *path, int verbose, int c2pa_trust_validation) {
    unsigned char *data;
    size_t size;
    ImageInfo info;
    ImageProbeOptions probe_options;
    const char *label = path ? path : "stdin";

    probe_options.c2pa_trust_validation = c2pa_trust_validation;

    if (read_all_input(path, &data, &size) != 0) {
        return -1;
    }
    if (image_probe_ex(data, size, &probe_options, &info) != 0) {
        rt_free(data);
        tool_write_error("imgmeta", "unsupported image format: ", label);
        return -1;
    }
    rt_write_cstr(1, label);
    rt_write_line(1, ":");
    rt_write_cstr(1, "  format: ");
    rt_write_line(1, image_format_name(info.format));
    rt_write_cstr(1, "  metadata: ");
    write_property_list(info.property_flags);
    rt_write_char(1, '\n');
    if ((info.flags & IMAGE_INFO_HAS_C2PA) != 0U) {
        rt_write_cstr(1, "  c2pa: ");
        rt_write_line(1, info.c2pa.status);
        rt_write_cstr(1, "  c2pa-carrier: ");
        rt_write_line(1, info.c2pa.carrier);
        if (info.c2pa.signature_algorithm != 0) {
            rt_write_cstr(1, "  c2pa-signature-algorithm: ");
            rt_write_line(1, info.c2pa.signature_algorithm);
        }
        rt_write_cstr(1, "  c2pa-boxes: ");
        rt_write_uint(1, (unsigned long long)info.c2pa.box_count);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "  c2pa-cbor-boxes: ");
        rt_write_uint(1, (unsigned long long)info.c2pa.cbor_box_count);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "  c2pa-manifests: ");
        rt_write_uint(1, (unsigned long long)info.c2pa.manifest_count);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "  c2pa-claims: ");
        rt_write_uint(1, (unsigned long long)info.c2pa.claim_count);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "  c2pa-signatures: ");
        rt_write_uint(1, (unsigned long long)info.c2pa.signature_count);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "  c2pa-cose-signatures: ");
        rt_write_uint(1, (unsigned long long)info.c2pa.cose_signature_count);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "  c2pa-verified-signatures: ");
        rt_write_uint(1, (unsigned long long)info.c2pa.signature_verified_count);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "  c2pa-invalid-signatures: ");
        rt_write_uint(1, (unsigned long long)info.c2pa.signature_invalid_count);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "  c2pa-x509-certificates: ");
        rt_write_uint(1, (unsigned long long)info.c2pa.x509_cert_count);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "  c2pa-validation-failures: ");
        rt_write_uint(1, (unsigned long long)info.c2pa.validation_failure_count);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "  c2pa-content-hash: ");
        rt_write_line(1, info.c2pa.content_hash_matched ? "match" : (info.c2pa.content_hash_mismatched ? "mismatch" : (info.c2pa.content_hash_checked ? "checked" : "not checked")));
        rt_write_cstr(1, "  c2pa-signature-verification: ");
        rt_write_line(1, info.c2pa.signature_supported ? (info.c2pa.signature_valid ? "valid" : "invalid") : "unsupported");
        rt_write_cstr(1, "  c2pa-trust-validation: ");
        rt_write_line(1, info.c2pa.trust_supported ? (info.c2pa.trust_valid ? "trusted" : "untrusted") : "unsupported");
    }
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
    if (verbose) {
        imgmeta_show_c2pa_verbose(data, size, &info);
    }
    rt_free(data);
    return 0;
}

static int png_chunk_is_metadata(const unsigned char *type) {
    return bytes_equal(type, "eXIf", 4U) ||
           bytes_equal(type, "caBX", 4U) ||
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

static int png_metadata_chunks_size(const unsigned char *data, size_t size, size_t *metadata_size_out) {
    size_t offset = 8U;
    size_t metadata_size = 0U;

    *metadata_size_out = 0U;
    while (offset + 12U <= size) {
        unsigned int length = read_u32_be(data + offset);
        const unsigned char *type = data + offset + 4U;
        size_t chunk_size;

        if ((size_t)length > size - offset - 12U) {
            return -1;
        }
        chunk_size = (size_t)length + 12U;
        if (png_chunk_is_metadata(type)) {
            if (metadata_size > ((size_t)-1) - chunk_size) {
                return -1;
            }
            metadata_size += chunk_size;
        }
        offset += chunk_size;
        if (bytes_equal(type, "IEND", 4U)) {
            break;
        }
    }
    *metadata_size_out = metadata_size;
    return 0;
}

static int copy_png_metadata(const unsigned char *metadata_data,
                             size_t metadata_size,
                             const unsigned char *image_data,
                             size_t image_size,
                             unsigned char **out_data,
                             size_t *out_size) {
    static const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    unsigned char *output;
    size_t metadata_total = 0U;
    size_t source_offset = 8U;
    size_t input_offset = 8U;
    size_t output_size = 8U;
    int inserted = 0;

    if (metadata_size < 8U || image_size < 8U || !bytes_equal(metadata_data, (const char *)signature, sizeof(signature)) ||
        !bytes_equal(image_data, (const char *)signature, sizeof(signature))) {
        return -1;
    }
    if (png_metadata_chunks_size(metadata_data, metadata_size, &metadata_total) != 0 || image_size > ((size_t)-1) - metadata_total) {
        return -1;
    }
    output = (unsigned char *)rt_malloc(image_size + metadata_total);
    if (output == 0) {
        return -1;
    }
    memcpy(output, image_data, 8U);
    while (input_offset + 12U <= image_size) {
        unsigned int length = read_u32_be(image_data + input_offset);
        const unsigned char *type = image_data + input_offset + 4U;
        size_t chunk_size;

        if ((size_t)length > image_size - input_offset - 12U) {
            rt_free(output);
            return -1;
        }
        chunk_size = (size_t)length + 12U;
        if (!inserted && (bytes_equal(type, "IDAT", 4U) || bytes_equal(type, "IEND", 4U))) {
            source_offset = 8U;
            while (source_offset + 12U <= metadata_size) {
                unsigned int source_length = read_u32_be(metadata_data + source_offset);
                const unsigned char *source_type = metadata_data + source_offset + 4U;
                size_t source_chunk_size;

                if ((size_t)source_length > metadata_size - source_offset - 12U) {
                    rt_free(output);
                    return -1;
                }
                source_chunk_size = (size_t)source_length + 12U;
                if (png_chunk_is_metadata(source_type)) {
                    memcpy(output + output_size, metadata_data + source_offset, source_chunk_size);
                    output_size += source_chunk_size;
                }
                source_offset += source_chunk_size;
                if (bytes_equal(source_type, "IEND", 4U)) {
                    break;
                }
            }
            inserted = 1;
        }
        if (!png_chunk_is_metadata(type)) {
            memcpy(output + output_size, image_data + input_offset, chunk_size);
            output_size += chunk_size;
        }
        input_offset += chunk_size;
        if (bytes_equal(type, "IEND", 4U)) {
            break;
        }
    }
    if (!inserted) {
        rt_free(output);
        return -1;
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

static void write_png_itxt_chunk(unsigned char *output,
                                 size_t offset,
                                 const char *key,
                                 size_t key_length,
                                 const char *value,
                                 size_t value_length,
                                 const char *language,
                                 size_t language_length,
                                 int compressed) {
    unsigned int crc;
    unsigned int payload_length = (unsigned int)(key_length + 5U + language_length + value_length);
    size_t cursor;

    write_u32_be(output + offset, payload_length);
    memcpy(output + offset + 4U, "iTXt", 4U);
    cursor = offset + 8U;
    memcpy(output + cursor, key, key_length);
    cursor += key_length;
    output[cursor++] = compressed ? 1U : 0U;
    output[cursor++] = 0U;
    output[cursor++] = 0U;
    if (language_length != 0U) {
        memcpy(output + cursor, language, language_length);
        cursor += language_length;
    }
    output[cursor++] = 0U;
    output[cursor++] = 0U;
    memcpy(output + cursor, value, value_length);
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

static int edit_png_itxt(const unsigned char *data,
                         size_t size,
                         const char *key,
                         size_t key_length,
                         const char *value,
                         size_t value_length,
                         const char *language,
                         size_t language_length,
                         int compressed,
                         unsigned char **out_data,
                         size_t *out_size) {
    static const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    unsigned char *output;
    size_t input_offset = 8U;
    size_t output_size = 8U;
    unsigned char *compressed_value = 0;
    size_t compressed_value_size = 0U;
    const char *chunk_value = value;
    size_t chunk_value_length = value_length;
    size_t text_chunk_size;
    int wrote_text = 0;

    if (size < 8U || !bytes_equal(data, (const char *)signature, sizeof(signature)) || !png_text_key_is_valid(key, key_length)) {
        return -1;
    }
    if (compressed) {
        size_t bound = compression_zlib_store_bound(value_length);

        if (bound == 0U) {
            return -1;
        }
        compressed_value = (unsigned char *)rt_malloc(bound);
        if (compressed_value == 0) {
            return -1;
        }
        if (compression_zlib_store((const unsigned char *)value, value_length, compressed_value, bound, &compressed_value_size) != 0) {
            rt_free(compressed_value);
            return -1;
        }
        chunk_value = (const char *)compressed_value;
        chunk_value_length = compressed_value_size;
    }
    text_chunk_size = key_length + 5U + language_length + chunk_value_length + 12U;
    if (text_chunk_size > 0xffffffffU || size > ((size_t)-1) - text_chunk_size) {
        if (compressed_value != 0) {
            rt_free(compressed_value);
        }
        return -1;
    }
    output = (unsigned char *)rt_malloc(size + text_chunk_size);
    if (output == 0) {
        if (compressed_value != 0) {
            rt_free(compressed_value);
        }
        return -1;
    }
    memcpy(output, data, 8U);
    while (input_offset + 12U <= size) {
        unsigned int length = read_u32_be(data + input_offset);
        const unsigned char *type = data + input_offset + 4U;
        const unsigned char *payload = data + input_offset + 8U;
        size_t chunk_size;

        if ((size_t)length > size - input_offset - 12U) {
            if (compressed_value != 0) {
                rt_free(compressed_value);
            }
            rt_free(output);
            return -1;
        }
        chunk_size = (size_t)length + 12U;
        if (!wrote_text && (bytes_equal(type, "IDAT", 4U) || bytes_equal(type, "IEND", 4U))) {
            write_png_itxt_chunk(output, output_size, key, key_length, chunk_value, chunk_value_length, language, language_length, compressed);
            output_size += text_chunk_size;
            wrote_text = 1;
        }
        if ((bytes_equal(type, "iTXt", 4U) || bytes_equal(type, "tEXt", 4U) || bytes_equal(type, "zTXt", 4U)) &&
            png_text_key_matches(payload, length, key, key_length)) {
            if (!wrote_text) {
                write_png_itxt_chunk(output, output_size, key, key_length, chunk_value, chunk_value_length, language, language_length, compressed);
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
        if (compressed_value != 0) {
            rt_free(compressed_value);
        }
        rt_free(output);
        return -1;
    }
    if (compressed_value != 0) {
        rt_free(compressed_value);
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
        if ((bytes_equal(type, "tEXt", 4U) || bytes_equal(type, "iTXt", 4U) || bytes_equal(type, "zTXt", 4U)) &&
            png_text_key_matches(payload, length, key, key_length)) {
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
        } else if (bytes_equal(type, "iTXt", 4U)) {
            size_t cursor;
            int compressed = 1;

            key_length = png_text_key_length(payload, length);
            rt_write_cstr(1, label);
            rt_write_char(1, '\t');
            rt_write_all(1, type, 4U);
            rt_write_char(1, '\t');
            rt_write_all(1, payload, key_length);
            rt_write_char(1, '\t');
            cursor = key_length + 1U;
            if (cursor + 2U < (size_t)length) {
                compressed = payload[cursor] != 0U;
                cursor += 2U;
                while (cursor < (size_t)length && payload[cursor] != 0U) {
                    cursor += 1U;
                }
                if (cursor < (size_t)length) {
                    cursor += 1U;
                }
                while (cursor < (size_t)length && payload[cursor] != 0U) {
                    cursor += 1U;
                }
                if (cursor < (size_t)length) {
                    cursor += 1U;
                }
                if (!compressed && cursor <= (size_t)length) {
                    rt_write_all(1, payload + cursor, (size_t)length - cursor);
                } else {
                    rt_write_char(1, '-');
                }
            } else {
                rt_write_char(1, '-');
            }
            rt_write_char(1, '\n');
        } else if (bytes_equal(type, "zTXt", 4U)) {
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
    if (marker == 0xebU && segment_size >= 14U &&
        bytes_equal(segment + 2U, "JP", 2U) &&
        segment[4U] == 0U && segment[5U] == 1U &&
        bytes_equal(segment + 10U, "jumb", 4U)) {
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

static int tiff_tag_is_metadata(unsigned int tag) {
    return tag == 270U ||
           tag == 271U ||
           tag == 272U ||
           tag == 274U ||
           tag == 305U ||
           tag == 306U ||
           tag == 315U ||
           tag == 700U ||
           tag == 33723U ||
           tag == 34377U ||
           tag == 34665U ||
           tag == 34675U ||
           tag == 34853U;
}

static size_t tiff_type_size(unsigned int type) {
    switch (type) {
        case 1U:
        case 2U:
        case 6U:
        case 7U:
            return 1U;
        case 3U:
        case 8U:
            return 2U;
        case 4U:
        case 9U:
        case 11U:
            return 4U;
        case 5U:
        case 10U:
        case 12U:
            return 8U;
        default:
            return 0U;
    }
}

static int strip_tiff(const unsigned char *data, size_t size, unsigned char **out_data, size_t *out_size) {
    unsigned char *output;
    int little_endian;
    unsigned int ifd_offset;
    unsigned int entry_count;
    unsigned int kept_count = 0U;
    unsigned int index;
    size_t entries_start;
    size_t old_next_offset;
    size_t new_next_offset;

    if (size < 8U || !((data[0] == 'I' && data[1] == 'I') || (data[0] == 'M' && data[1] == 'M'))) {
        return -1;
    }
    little_endian = data[0] == 'I';
    if (read_tiff_u16(data + 2U, little_endian) == 43U) {
        unsigned int kept_count = 0U;
        size_t ifd_offset_size;

        if (size < 16U || read_tiff_u16(data + 4U, little_endian) != 8U || read_tiff_u16(data + 6U, little_endian) != 0U ||
            !tiff_u64_high_is_zero(data + 8U, little_endian)) {
            return -1;
        }
        ifd_offset = read_tiff_u64_low(data + 8U, little_endian);
        if ((size_t)ifd_offset + 8U > size || !tiff_u64_high_is_zero(data + ifd_offset, little_endian)) {
            return -1;
        }
        entry_count = read_tiff_u64_low(data + ifd_offset, little_endian);
        entries_start = (size_t)ifd_offset + 8U;
        if ((size_t)entry_count > (size - entries_start) / 20U || entries_start + (size_t)entry_count * 20U + 8U > size) {
            return -1;
        }
        output = (unsigned char *)rt_malloc(size == 0U ? 1U : size);
        if (output == 0) {
            return -1;
        }
        memcpy(output, data, size);
        old_next_offset = entries_start + (size_t)entry_count * 20U;
        for (index = 0U; index < entry_count; ++index) {
            const unsigned char *entry = data + entries_start + (size_t)index * 20U;
            unsigned int tag = read_tiff_u16(entry, little_endian);

            if (tiff_tag_is_metadata(tag)) {
                unsigned int type = read_tiff_u16(entry + 2U, little_endian);
                size_t unit_size = tiff_type_size(type);

                if (unit_size != 0U && tiff_u64_high_is_zero(entry + 4U, little_endian)) {
                    unsigned int count = read_tiff_u64_low(entry + 4U, little_endian);

                    if (count != 0U && (size_t)count <= ((size_t)-1) / unit_size) {
                        size_t value_size = unit_size * (size_t)count;

                        if (value_size > 8U && tiff_u64_high_is_zero(entry + 12U, little_endian)) {
                            unsigned int value_offset = read_tiff_u64_low(entry + 12U, little_endian);

                            if ((size_t)value_offset <= size && value_size <= size - (size_t)value_offset) {
                                rt_memset(output + value_offset, 0, value_size);
                            }
                        }
                    }
                }
                continue;
            }
            if (kept_count != index) {
                memcpy(output + entries_start + (size_t)kept_count * 20U, entry, 20U);
            }
            kept_count += 1U;
        }
        new_next_offset = entries_start + (size_t)kept_count * 20U;
        write_tiff_u64_low(output + ifd_offset, kept_count, little_endian);
        ifd_offset_size = read_tiff_u64_low(data + old_next_offset, little_endian);
        write_tiff_u64_low(output + new_next_offset, (unsigned int)ifd_offset_size, little_endian);
        if (old_next_offset + 8U > new_next_offset + 8U) {
            rt_memset(output + new_next_offset + 8U, 0, old_next_offset - new_next_offset);
        }
        *out_data = output;
        *out_size = size;
        return 0;
    }
    if (read_tiff_u16(data + 2U, little_endian) != 42U) {
        return -1;
    }
    ifd_offset = read_tiff_u32(data + 4U, little_endian);
    if ((size_t)ifd_offset + 2U > size) {
        return -1;
    }
    entry_count = read_tiff_u16(data + ifd_offset, little_endian);
    entries_start = (size_t)ifd_offset + 2U;
    if ((size_t)entry_count > (size - entries_start) / 12U || entries_start + (size_t)entry_count * 12U + 4U > size) {
        return -1;
    }
    output = (unsigned char *)rt_malloc(size == 0U ? 1U : size);
    if (output == 0) {
        return -1;
    }
    memcpy(output, data, size);
    old_next_offset = entries_start + (size_t)entry_count * 12U;
    for (index = 0U; index < entry_count; ++index) {
        const unsigned char *entry = data + entries_start + (size_t)index * 12U;
        unsigned int tag = read_tiff_u16(entry, little_endian);

        if (tiff_tag_is_metadata(tag)) {
            unsigned int type = read_tiff_u16(entry + 2U, little_endian);
            unsigned int count = read_tiff_u32(entry + 4U, little_endian);
            size_t unit_size = tiff_type_size(type);

            if (unit_size != 0U && count != 0U && (size_t)count <= ((size_t)-1) / unit_size) {
                size_t value_size = unit_size * (size_t)count;

                if (value_size > 4U) {
                    unsigned int value_offset = read_tiff_u32(entry + 8U, little_endian);

                    if ((size_t)value_offset <= size && value_size <= size - (size_t)value_offset) {
                        rt_memset(output + value_offset, 0, value_size);
                    }
                }
            }
            continue;
        }
        if (kept_count != index) {
            memcpy(output + entries_start + (size_t)kept_count * 12U, entry, 12U);
        }
        kept_count += 1U;
    }
    new_next_offset = entries_start + (size_t)kept_count * 12U;
    write_tiff_u16(output + ifd_offset, kept_count, little_endian);
    write_tiff_u32(output + new_next_offset, read_tiff_u32(data + old_next_offset, little_endian), little_endian);
    if (old_next_offset + 4U > new_next_offset + 4U) {
        rt_memset(output + new_next_offset + 4U, 0, old_next_offset - new_next_offset);
    }
    *out_data = output;
    *out_size = size;
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
    } else if (info.format == IMAGE_FORMAT_TIFF) {
        result = strip_tiff(data, size, &stripped, &stripped_size);
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

static int copy_metadata_path(const char *metadata_path, const char *input_path, const char *output_path) {
    unsigned char *metadata_data;
    unsigned char *image_data;
    unsigned char *copied = 0;
    size_t metadata_size;
    size_t image_size;
    size_t copied_size = 0U;
    ImageInfo metadata_info;
    ImageInfo image_info;
    int result;

    if (read_all_input(metadata_path, &metadata_data, &metadata_size) != 0) {
        return -1;
    }
    if (read_all_input(input_path, &image_data, &image_size) != 0) {
        rt_free(metadata_data);
        return -1;
    }
    if (image_probe(metadata_data, metadata_size, &metadata_info) != 0 || image_probe(image_data, image_size, &image_info) != 0) {
        rt_free(metadata_data);
        rt_free(image_data);
        tool_write_error("imgmeta", "unsupported image format for metadata copy", 0);
        return -1;
    }
    if (metadata_info.format != IMAGE_FORMAT_PNG || image_info.format != IMAGE_FORMAT_PNG) {
        rt_free(metadata_data);
        rt_free(image_data);
        tool_write_error("imgmeta", "selective metadata copy is implemented for PNG only", 0);
        return -1;
    }
    result = copy_png_metadata(metadata_data, metadata_size, image_data, image_size, &copied, &copied_size);
    rt_free(metadata_data);
    rt_free(image_data);
    if (result != 0 || copied == 0) {
        tool_write_error("imgmeta", "could not copy PNG metadata", 0);
        return -1;
    }
    result = write_file(output_path, copied, copied_size);
    rt_free(copied);
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

static int edit_itxt_path(const char *input_path, const char *output_path, const char *assignment, const char *language, int compressed) {
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
        tool_write_error("imgmeta", "iTXt metadata must be KEY=VALUE", 0);
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
        tool_write_error("imgmeta", "iTXt editing is not implemented for: ", image_format_extension(info.format));
        rt_free(data);
        return -1;
    }
    result = edit_png_itxt(data, size, assignment, (size_t)(equals - assignment), equals + 1, rt_strlen(equals + 1),
                           language == 0 ? "" : language, language == 0 ? 0U : rt_strlen(language), compressed, &edited, &edited_size);
    rt_free(data);
    if (result != 0 || edited == 0) {
        tool_write_error("imgmeta", "could not edit PNG iTXt metadata: ", input_path);
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
    int verbose = 0;
    int c2pa_trust_validation = 0;

    while (arg_index < argc) {
        const char *arg = argv[arg_index];
        if (rt_strcmp(arg, "-v") == 0 || rt_strcmp(arg, "--verbose") == 0) {
            verbose = 1;
            arg_index += 1;
            continue;
        }
        if (rt_strcmp(arg, "--c2pa-trust") == 0 || rt_strcmp(arg, "--trust") == 0) {
            c2pa_trust_validation = 1;
            arg_index += 1;
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
        break;
    }
    if (arg_index >= argc) {
        return show_path(0, verbose, c2pa_trust_validation) == 0 ? 0 : 1;
    }
    while (arg_index < argc) {
        if (show_path(argv[arg_index], verbose, c2pa_trust_validation) != 0) {
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
    const char *metadata_path = 0;

    while (arg_index < argc) {
        const char *arg = argv[arg_index];

        if ((rt_strcmp(arg, "-o") == 0 || rt_strcmp(arg, "--output") == 0) && arg_index + 1 < argc) {
            output_path = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if (rt_strcmp(arg, "--from") == 0 && arg_index + 1 < argc) {
            metadata_path = argv[arg_index + 1];
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
    if (metadata_path != 0) {
        return copy_metadata_path(metadata_path, input_path, output_path) == 0 ? 0 : 1;
    }
    return copy_path(input_path, output_path) == 0 ? 0 : 1;
}

static int run_edit(int argc, char **argv, int arg_index) {
    const char *output_path = 0;
    const char *input_path = 0;
    const char *text_assignment = 0;
    const char *itxt_assignment = 0;
    const char *remove_text_key = 0;
    const char *language = 0;
    int compressed = 0;

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
        if (rt_strcmp(arg, "--set-itxt") == 0 && arg_index + 1 < argc) {
            itxt_assignment = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if (rt_strcmp(arg, "--remove-text") == 0 && arg_index + 1 < argc) {
            remove_text_key = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if ((rt_strcmp(arg, "--language") == 0 || rt_strcmp(arg, "--lang") == 0) && arg_index + 1 < argc) {
            language = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if (rt_strcmp(arg, "--compressed") == 0) {
            compressed = 1;
            arg_index += 1;
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
    if (input_path == 0 || output_path == 0 ||
        ((text_assignment != 0) + (itxt_assignment != 0) + (remove_text_key != 0)) != 1) {
        tool_write_error("imgmeta", "edit requires one of --set-text KEY=VALUE, --set-itxt KEY=VALUE, or --remove-text KEY, -o OUTPUT, and one input file", 0);
        print_usage();
        return 1;
    }
    if (remove_text_key != 0) {
        return remove_text_path(input_path, output_path, remove_text_key) == 0 ? 0 : 1;
    }
    if (itxt_assignment != 0) {
        return edit_itxt_path(input_path, output_path, itxt_assignment, language, compressed) == 0 ? 0 : 1;
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
