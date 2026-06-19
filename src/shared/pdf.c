#include "pdf.h"
#include "compression/zlib.h"
#include "runtime.h"

#define PDF_FLATE_MAX_OUTPUT (64U * 1024U * 1024U)
#define PDF_OBJECT_STREAM_MAX_OBJECTS 8192U
#define PDF_XREF_STREAM_MAX_ENTRIES 65536ULL

/*
 * Modern PDF support is intentionally minimal: streams decode only unfiltered
 * data or a single FlateDecode/Fl filter, PNG predictors require 8-bit
 * components, Flate output is capped at 64 MiB, object streams are capped at
 * 8192 objects, and xref streams are capped at 65536 entries to avoid
 * unbounded memory use.
 */

int pdf_is_space(unsigned char ch) {
    return ch == 0U || ch == 9U || ch == 10U || ch == 12U || ch == 13U || ch == 32U;
}

int pdf_is_digit(unsigned char ch) {
    return ch >= (unsigned char)'0' && ch <= (unsigned char)'9';
}

int pdf_is_delim(unsigned char ch) {
    return pdf_is_space(ch) || ch == (unsigned char)'(' || ch == (unsigned char)')' || ch == (unsigned char)'<' || ch == (unsigned char)'>' || ch == (unsigned char)'[' || ch == (unsigned char)']' || ch == (unsigned char)'{' || ch == (unsigned char)'}' || ch == (unsigned char)'/' || ch == (unsigned char)'%';
}

typedef struct {
    const unsigned char *data;
    size_t size;
    unsigned char *owned;
} PdfDecodedStream;

typedef struct {
    unsigned long long number;
    size_t offset;
    size_t end;
} PdfObjectStreamEntry;

static int pdf_compare_object_stream_entries(const void *left_ptr, const void *right_ptr) {
    const PdfObjectStreamEntry *left = (const PdfObjectStreamEntry *)left_ptr;
    const PdfObjectStreamEntry *right = (const PdfObjectStreamEntry *)right_ptr;

    if (left->offset < right->offset) return -1;
    if (left->offset > right->offset) return 1;
    if (left->number < right->number) return -1;
    if (left->number > right->number) return 1;
    return 0;
}

size_t pdf_skip_ws(const unsigned char *data, size_t size, size_t offset) {
    while (offset < size) {
        if (pdf_is_space(data[offset])) {
            offset += 1U;
        } else if (data[offset] == (unsigned char)'%') {
            while (offset < size && data[offset] != (unsigned char)'\n' && data[offset] != (unsigned char)'\r') offset += 1U;
        } else {
            break;
        }
    }
    return offset;
}

int pdf_text_at(const unsigned char *data, size_t size, size_t offset, const char *text) {
    size_t length = rt_strlen(text);
    size_t index;

    if (offset > size || length > size - offset) return 0;
    for (index = 0U; index < length; ++index) {
        if (data[offset + index] != (unsigned char)text[index]) return 0;
    }
    return 1;
}

int pdf_text_at_len(const unsigned char *data, size_t size, size_t offset, const char *text, size_t length) {
    size_t index;

    if (offset > size || length > size - offset) return 0;
    for (index = 0U; index < length; ++index) {
        if (data[offset + index] != (unsigned char)text[index]) return 0;
    }
    return 1;
}

int pdf_keyword_at(const unsigned char *data, size_t size, size_t offset, const char *text) {
    size_t length = rt_strlen(text);

    if (!pdf_text_at_len(data, size, offset, text, length)) return 0;
    if (offset != 0U && !pdf_is_delim(data[offset - 1U])) return 0;
    if (offset + length < size && !pdf_is_delim(data[offset + length])) return 0;
    return 1;
}

static int pdf_keyword_at_len(const unsigned char *data, size_t size, size_t offset, const char *text, size_t length) {
    if (!pdf_text_at_len(data, size, offset, text, length)) return 0;
    if (offset != 0U && !pdf_is_delim(data[offset - 1U])) return 0;
    if (offset + length < size && !pdf_is_delim(data[offset + length])) return 0;
    return 1;
}

static size_t pdf_find_keyword(const unsigned char *data, size_t size, size_t start, size_t end, const char *text) {
    size_t length = rt_strlen(text);
    size_t offset;
    unsigned char first;

    if (end > size) end = size;
    if (length == 0U || start >= end || length > end - start) return size;
    first = (unsigned char)text[0];
    for (offset = start; offset + length <= end; ++offset) {
        if (data[offset] == first && pdf_keyword_at_len(data, size, offset, text, length)) return offset;
    }
    return size;
}

int pdf_parse_u64(const unsigned char *data, size_t size, size_t *offset_io, unsigned long long *value_out) {
    unsigned long long value = 0ULL;
    size_t offset = *offset_io;
    int saw_digit = 0;

    while (offset < size && pdf_is_digit(data[offset])) {
        unsigned int digit = (unsigned int)(data[offset] - (unsigned char)'0');

        if (value > (18446744073709551615ULL - (unsigned long long)digit) / 10ULL) return -1;
        value = value * 10ULL + (unsigned long long)digit;
        offset += 1U;
        saw_digit = 1;
    }
    if (!saw_digit) return -1;
    *offset_io = offset;
    *value_out = value;
    return 0;
}

static int pdf_parse_fixed(const unsigned char *data, size_t size, size_t *offset_io, long long *value_out) {
    size_t offset = pdf_skip_ws(data, size, *offset_io);
    long long sign = 1LL;
    long long whole = 0LL;
    long long fraction = 0LL;
    int fraction_digits = 0;
    int saw_digit = 0;

    if (offset < size && data[offset] == (unsigned char)'-') {
        sign = -1LL;
        offset += 1U;
    } else if (offset < size && data[offset] == (unsigned char)'+') {
        offset += 1U;
    }
    while (offset < size && pdf_is_digit(data[offset])) {
        whole = whole * 10LL + (long long)(data[offset] - (unsigned char)'0');
        offset += 1U;
        saw_digit = 1;
    }
    if (offset < size && data[offset] == (unsigned char)'.') {
        offset += 1U;
        while (offset < size && pdf_is_digit(data[offset])) {
            if (fraction_digits < 3) {
                fraction = fraction * 10LL + (long long)(data[offset] - (unsigned char)'0');
                fraction_digits += 1;
            }
            offset += 1U;
            saw_digit = 1;
        }
    }
    if (!saw_digit) return -1;
    while (fraction_digits < 3) {
        fraction *= 10LL;
        fraction_digits += 1;
    }
    *offset_io = offset;
    *value_out = sign * (whole * 1000LL + fraction);
    return 0;
}

static int pdf_append_utf8(char *buffer, size_t buffer_size, size_t *used_io, unsigned int codepoint) {
    char encoded[4];
    size_t encoded_length = 0U;
    size_t index;

    if (buffer_size == 0U || *used_io + 1U >= buffer_size) return 0;
    if (codepoint == 0U) return 0;
    if (rt_utf8_encode(codepoint, encoded, sizeof(encoded), &encoded_length) != 0) return 0;
    for (index = 0U; index < encoded_length && *used_io + 1U < buffer_size; ++index) {
        buffer[*used_io] = encoded[index];
        *used_io += 1U;
    }
    buffer[*used_io] = '\0';
    return encoded_length != 0U;
}

static int pdf_hex_value(unsigned char ch) {
    if (ch >= (unsigned char)'0' && ch <= (unsigned char)'9') return (int)(ch - (unsigned char)'0');
    if (ch >= (unsigned char)'A' && ch <= (unsigned char)'F') return (int)(ch - (unsigned char)'A') + 10;
    if (ch >= (unsigned char)'a' && ch <= (unsigned char)'f') return (int)(ch - (unsigned char)'a') + 10;
    return -1;
}

static size_t pdf_bytes_to_text(const unsigned char *bytes, size_t length, char *buffer, size_t buffer_size) {
    size_t used = 0U;
    size_t index;

    if (buffer_size == 0U) return 0U;
    buffer[0] = '\0';
    if (length >= 2U && bytes[0] == 0xfeU && bytes[1] == 0xffU) {
        for (index = 2U; index + 1U < length; index += 2U) {
            unsigned int codepoint = ((unsigned int)bytes[index] << 8U) | (unsigned int)bytes[index + 1U];
            (void)pdf_append_utf8(buffer, buffer_size, &used, codepoint);
        }
        return used;
    }
    if (length >= 2U && bytes[0] == 0xffU && bytes[1] == 0xfeU) {
        for (index = 2U; index + 1U < length; index += 2U) {
            unsigned int codepoint = ((unsigned int)bytes[index + 1U] << 8U) | (unsigned int)bytes[index];
            (void)pdf_append_utf8(buffer, buffer_size, &used, codepoint);
        }
        return used;
    }
    for (index = 0U; index < length && used + 1U < buffer_size; ++index) {
        if (bytes[index] != 0U) buffer[used++] = (char)bytes[index];
    }
    buffer[used] = '\0';
    return used;
}

static size_t pdf_literal_to_text(const unsigned char *data, size_t size, size_t offset, char *buffer, size_t buffer_size) {
    unsigned char bytes[PDF_TEXT_CAPACITY * 2U];
    size_t byte_count = 0U;
    int depth = 1;

    if (buffer_size != 0U) buffer[0] = '\0';
    if (offset >= size || data[offset] != (unsigned char)'(') return 0U;
    offset += 1U;
    while (offset < size && depth > 0) {
        unsigned char ch = data[offset++];

        if (ch == (unsigned char)'\\') {
            unsigned int octal = 0U;
            int octal_digits = 0;

            if (offset >= size) break;
            ch = data[offset++];
            if (ch >= (unsigned char)'0' && ch <= (unsigned char)'7') {
                octal = (unsigned int)(ch - (unsigned char)'0');
                octal_digits = 1;
                while (offset < size && octal_digits < 3 && data[offset] >= (unsigned char)'0' && data[offset] <= (unsigned char)'7') {
                    octal = octal * 8U + (unsigned int)(data[offset] - (unsigned char)'0');
                    offset += 1U;
                    octal_digits += 1;
                }
                ch = (unsigned char)(octal & 0xffU);
            } else if (ch == (unsigned char)'n') ch = (unsigned char)'\n';
            else if (ch == (unsigned char)'r') ch = (unsigned char)'\r';
            else if (ch == (unsigned char)'t') ch = (unsigned char)'\t';
            else if (ch == (unsigned char)'b') ch = 8U;
            else if (ch == (unsigned char)'f') ch = 12U;
            else if (ch == (unsigned char)'\r') {
                if (offset < size && data[offset] == (unsigned char)'\n') offset += 1U;
                continue;
            } else if (ch == (unsigned char)'\n') {
                continue;
            }
            if (byte_count < sizeof(bytes)) bytes[byte_count++] = ch;
        } else {
            if (ch == (unsigned char)'(') depth += 1;
            if (ch == (unsigned char)')') depth -= 1;
            if (depth > 0 && byte_count < sizeof(bytes)) bytes[byte_count++] = ch;
        }
    }
    return pdf_bytes_to_text(bytes, byte_count, buffer, buffer_size);
}

static size_t pdf_hex_to_text(const unsigned char *data, size_t size, size_t offset, char *buffer, size_t buffer_size) {
    unsigned char bytes[PDF_TEXT_CAPACITY * 2U];
    size_t byte_count = 0U;
    int high_nibble = -1;

    if (buffer_size != 0U) buffer[0] = '\0';
    if (offset >= size || data[offset] != (unsigned char)'<') return 0U;
    offset += 1U;
    while (offset < size && data[offset] != (unsigned char)'>') {
        int value;

        if (pdf_is_space(data[offset])) {
            offset += 1U;
            continue;
        }
        value = pdf_hex_value(data[offset]);
        if (value < 0) break;
        if (high_nibble < 0) {
            high_nibble = value;
        } else {
            if (byte_count < sizeof(bytes)) bytes[byte_count++] = (unsigned char)((high_nibble << 4) | value);
            high_nibble = -1;
        }
        offset += 1U;
    }
    if (high_nibble >= 0 && byte_count < sizeof(bytes)) bytes[byte_count++] = (unsigned char)(high_nibble << 4);
    return pdf_bytes_to_text(bytes, byte_count, buffer, buffer_size);
}

void pdf_copy_name(const unsigned char *data, size_t size, size_t offset, char *buffer, size_t buffer_size) {
    size_t used = 0U;

    if (buffer_size == 0U) return;
    while (offset < size && !pdf_is_delim(data[offset])) {
        if (used + 1U < buffer_size) buffer[used++] = (char)data[offset];
        offset += 1U;
    }
    buffer[used] = '\0';
}

static void pdf_skip_literal_string(const unsigned char *data, size_t size, size_t *offset_io);

static int pdf_find_key_from(const unsigned char *data, size_t size, size_t start, const char *key, size_t *offset_out) {
    size_t key_length = rt_strlen(key);
    size_t offset;

    for (offset = start; offset + key_length <= size; ++offset) {
        while (offset + key_length <= size && data[offset] != (unsigned char)'/') offset += 1U;
        if (offset + key_length > size) break;
        if (data[offset] == (unsigned char)'/' && pdf_text_at_len(data, size, offset, key, key_length) && (offset + key_length >= size || pdf_is_delim(data[offset + key_length]))) {
            *offset_out = offset;
            return 1;
        }
    }
    return 0;
}

static int pdf_find_key(const unsigned char *data, size_t size, const char *key, size_t *offset_out) {
    return pdf_find_key_from(data, size, 0U, key, offset_out);
}

int pdf_find_top_key(const unsigned char *data, size_t size, const char *key, size_t *offset_out) {
    size_t key_length = rt_strlen(key);
    size_t offset = 0U;
    unsigned int depth = 0U;

    while (offset < size) {
        if (data[offset] == (unsigned char)'%') {
            while (offset < size && data[offset] != (unsigned char)'\n' && data[offset] != (unsigned char)'\r') offset += 1U;
        } else if (data[offset] == (unsigned char)'(') {
            pdf_skip_literal_string(data, size, &offset);
        } else if (data[offset] == (unsigned char)'<' && offset + 1U < size && data[offset + 1U] == (unsigned char)'<') {
            depth += 1U;
            offset += 2U;
        } else if (data[offset] == (unsigned char)'>' && offset + 1U < size && data[offset + 1U] == (unsigned char)'>') {
            if (depth != 0U) depth -= 1U;
            offset += 2U;
        } else if (data[offset] == (unsigned char)'<' && offset + 1U < size) {
            offset += 1U;
            while (offset < size && data[offset] != (unsigned char)'>') offset += 1U;
            if (offset < size) offset += 1U;
        } else if (data[offset] == (unsigned char)'[') {
            depth += 1U;
            offset += 1U;
        } else if (data[offset] == (unsigned char)']') {
            if (depth != 0U) depth -= 1U;
            offset += 1U;
        } else if (depth == 1U && data[offset] == (unsigned char)'/' && offset + key_length <= size && pdf_text_at_len(data, size, offset, key, key_length) && (offset + key_length >= size || pdf_is_delim(data[offset + key_length]))) {
            *offset_out = offset;
            return 1;
        } else {
            size_t start_offset = offset;
            while (offset < size && data[offset] != (unsigned char)'%' && data[offset] != (unsigned char)'(' && data[offset] != (unsigned char)'<' && data[offset] != (unsigned char)'>' && data[offset] != (unsigned char)'[' && data[offset] != (unsigned char)']' && data[offset] != (unsigned char)'/') offset += 1U;
            if (offset == start_offset) offset += 1U;
        }
    }
    return 0;
}

static int pdf_find_name_value(const unsigned char *data, size_t size, const char *key, char *buffer, size_t buffer_size) {
    size_t key_offset;
    size_t value_offset;

    if (buffer_size != 0U) buffer[0] = '\0';
    if (!pdf_find_key(data, size, key, &key_offset)) return 0;
    value_offset = pdf_skip_ws(data, size, key_offset + rt_strlen(key));
    if (value_offset >= size || data[value_offset] != (unsigned char)'/') return 0;
    pdf_copy_name(data, size, value_offset + 1U, buffer, buffer_size);
    return buffer_size != 0U && buffer[0] != '\0';
}

static int pdf_find_top_name_value(const unsigned char *data, size_t size, const char *key, char *buffer, size_t buffer_size) {
    size_t key_offset;
    size_t value_offset;

    if (buffer_size != 0U) buffer[0] = '\0';
    if (!pdf_find_top_key(data, size, key, &key_offset)) return 0;
    value_offset = pdf_skip_ws(data, size, key_offset + rt_strlen(key));
    if (value_offset >= size || data[value_offset] != (unsigned char)'/') return 0;
    pdf_copy_name(data, size, value_offset + 1U, buffer, buffer_size);
    return buffer_size != 0U && buffer[0] != '\0';
}

static int pdf_find_literal_value(const unsigned char *data, size_t size, const char *key, char *buffer, size_t buffer_size) {
    size_t key_offset;
    size_t offset;
    size_t used = 0U;
    int depth = 1;

    if (buffer_size == 0U) return 0;
    buffer[0] = '\0';
    if (!pdf_find_key(data, size, key, &key_offset)) return 0;
    offset = pdf_skip_ws(data, size, key_offset + rt_strlen(key));
    if (offset >= size || data[offset] != (unsigned char)'(') return 0;
    offset += 1U;
    while (offset < size && depth > 0) {
        unsigned char ch = data[offset++];

        if (ch == (unsigned char)'\\') {
            if (offset < size && used + 1U < buffer_size) buffer[used++] = (char)data[offset];
            if (offset < size) offset += 1U;
        } else {
            if (ch == (unsigned char)'(') depth += 1;
            if (ch == (unsigned char)')') depth -= 1;
            if (depth > 0 && used + 1U < buffer_size) buffer[used++] = (char)ch;
        }
    }
    buffer[used] = '\0';
    return used != 0U;
}

static int pdf_find_top_text_value(const unsigned char *data, size_t size, const char *key, char *buffer, size_t buffer_size) {
    size_t key_offset;
    size_t value_offset;

    if (buffer_size != 0U) buffer[0] = '\0';
    if (!pdf_find_top_key(data, size, key, &key_offset)) return 0;
    value_offset = pdf_skip_ws(data, size, key_offset + rt_strlen(key));
    if (value_offset >= size) return 0;
    if (data[value_offset] == (unsigned char)'(') return pdf_literal_to_text(data, size, value_offset, buffer, buffer_size) != 0U;
    if (data[value_offset] == (unsigned char)'<' && value_offset + 1U < size && data[value_offset + 1U] != (unsigned char)'<') return pdf_hex_to_text(data, size, value_offset, buffer, buffer_size) != 0U;
    if (data[value_offset] == (unsigned char)'/') {
        pdf_copy_name(data, size, value_offset + 1U, buffer, buffer_size);
        return buffer_size != 0U && buffer[0] != '\0';
    }
    return 0;
}

static int pdf_find_top_number_value(const unsigned char *data, size_t size, const char *key, long long *value_out) {
    size_t key_offset;
    size_t offset;

    if (!pdf_find_top_key(data, size, key, &key_offset)) return 0;
    offset = key_offset + rt_strlen(key);
    return pdf_parse_fixed(data, size, &offset, value_out) == 0;
}

static int pdf_find_top_u64_direct_value(const unsigned char *data, size_t size, const char *key, unsigned long long *value_out) {
    size_t key_offset;
    size_t offset;
    size_t after_value;
    unsigned long long value;

    if (!pdf_find_top_key(data, size, key, &key_offset)) return 0;
    offset = pdf_skip_ws(data, size, key_offset + rt_strlen(key));
    if (pdf_parse_u64(data, size, &offset, &value) != 0) return 0;
    after_value = pdf_skip_ws(data, size, offset);
    if (after_value < size && pdf_is_digit(data[after_value])) {
        size_t ref_offset = after_value;
        unsigned long long generation;

        if (pdf_parse_u64(data, size, &ref_offset, &generation) == 0) {
            ref_offset = pdf_skip_ws(data, size, ref_offset);
            if (pdf_keyword_at(data, size, ref_offset, "R")) return 0;
        }
    }
    *value_out = value;
    return 1;
}

static int pdf_find_u64_direct_value(const unsigned char *data, size_t size, const char *key, unsigned long long *value_out) {
    size_t key_offset;
    size_t offset;
    size_t after_value;
    unsigned long long value;

    if (!pdf_find_key(data, size, key, &key_offset)) return 0;
    offset = pdf_skip_ws(data, size, key_offset + rt_strlen(key));
    if (pdf_parse_u64(data, size, &offset, &value) != 0) return 0;
    after_value = pdf_skip_ws(data, size, offset);
    if (after_value < size && pdf_is_digit(data[after_value])) {
        size_t ref_offset = after_value;
        unsigned long long generation;

        if (pdf_parse_u64(data, size, &ref_offset, &generation) == 0) {
            ref_offset = pdf_skip_ws(data, size, ref_offset);
            if (pdf_keyword_at(data, size, ref_offset, "R")) return 0;
        }
    }
    *value_out = value;
    return 1;
}

static int pdf_read_u64_array_value(const unsigned char *data, size_t size, size_t *offset_io, unsigned long long *value_out) {
    size_t offset = pdf_skip_ws(data, size, *offset_io);

    if (offset >= size || data[offset] == (unsigned char)']') return 0;
    if (pdf_parse_u64(data, size, &offset, value_out) != 0) return -1;
    *offset_io = offset;
    return 1;
}

static int pdf_find_top_u64_array_offset(const unsigned char *data, size_t size, const char *key, size_t *array_offset_out) {
    size_t key_offset;
    size_t offset;

    if (!pdf_find_top_key(data, size, key, &key_offset)) return 0;
    offset = pdf_skip_ws(data, size, key_offset + rt_strlen(key));
    if (offset >= size || data[offset] != (unsigned char)'[') return 0;
    *array_offset_out = offset + 1U;
    return 1;
}

static int pdf_find_top_box_value(const unsigned char *data, size_t size, const char *key, long long values[4]) {
    size_t key_offset;
    size_t offset;
    size_t index;

    if (!pdf_find_top_key(data, size, key, &key_offset)) return 0;
    offset = pdf_skip_ws(data, size, key_offset + rt_strlen(key));
    if (offset >= size || data[offset] != (unsigned char)'[') return 0;
    offset += 1U;
    for (index = 0U; index < 4U; ++index) {
        if (pdf_parse_fixed(data, size, &offset, &values[index]) != 0) return 0;
    }
    return 1;
}

static int pdf_grow_objects(PdfInfo *info) {
    size_t next_capacity;
    PdfObjectInfo *next;

    if (info->objects_len < info->objects_cap) return 0;
    next_capacity = info->objects_cap == 0U ? 32U : info->objects_cap * 2U;
    next = (PdfObjectInfo *)rt_realloc_array(info->objects, next_capacity, sizeof(PdfObjectInfo));
    if (next == 0) return -1;
    info->objects = next;
    info->objects_cap = next_capacity;
    return 0;
}

static size_t pdf_object_index_slot(unsigned int number, unsigned int generation, size_t capacity) {
    unsigned long long mixed = ((unsigned long long)number << 32U) ^ (unsigned long long)generation;

    mixed ^= mixed >> 33U;
    mixed *= 0xff51afd7ed558ccdULL;
    mixed ^= mixed >> 33U;
    return (size_t)(mixed & (unsigned long long)(capacity - 1U));
}

static void pdf_object_index_insert_slot(PdfInfo *info, size_t object_index) {
    const PdfObjectInfo *object = &info->objects[object_index];
    size_t slot = pdf_object_index_slot(object->number, object->generation, info->object_index_cap);

    while (info->object_index[slot] != 0) {
        size_t existing_index = (size_t)(info->object_index[slot] - 1);
        const PdfObjectInfo *existing = &info->objects[existing_index];

        if (existing->number == object->number && existing->generation == object->generation) return;
        slot = (slot + 1U) & (info->object_index_cap - 1U);
    }
    info->object_index[slot] = (int)object_index + 1;
}

static int pdf_rebuild_object_index(PdfInfo *info, size_t next_capacity) {
    int *old_index = info->object_index;
    size_t old_capacity = info->object_index_cap;
    int *next;
    size_t index;

    next = (int *)rt_malloc_array(next_capacity, sizeof(next[0]));
    if (next == 0) return -1;
    rt_memset(next, 0, next_capacity * sizeof(next[0]));
    info->object_index = next;
    info->object_index_cap = next_capacity;
    for (index = 0U; index < info->objects_len; ++index) pdf_object_index_insert_slot(info, index);
    rt_free(old_index);
    (void)old_capacity;
    return 0;
}

static int pdf_ensure_object_index_capacity(PdfInfo *info) {
    size_t next_capacity;

    if (info->object_index_cap != 0U && (info->objects_len + 1U) * 2U <= info->object_index_cap) return 0;
    next_capacity = info->object_index_cap == 0U ? 64U : info->object_index_cap * 2U;
    while ((info->objects_len + 1U) * 2U > next_capacity) {
        if (next_capacity > ((size_t)-1) / 2U) return -1;
        next_capacity *= 2U;
    }
    return pdf_rebuild_object_index(info, next_capacity);
}

static int pdf_grow_pages(PdfInfo *info) {
    size_t next_capacity;
    PdfPageInfo *next;

    if (info->pages_len < info->pages_cap) return 0;
    next_capacity = info->pages_cap == 0U ? 16U : info->pages_cap * 2U;
    next = (PdfPageInfo *)rt_realloc_array(info->pages, next_capacity, sizeof(PdfPageInfo));
    if (next == 0) return -1;
    info->pages = next;
    info->pages_cap = next_capacity;
    return 0;
}

static int pdf_grow_fonts(PdfInfo *info) {
    size_t next_capacity;
    PdfFontInfo *next;

    if (info->fonts_len < info->fonts_cap) return 0;
    next_capacity = info->fonts_cap == 0U ? 16U : info->fonts_cap * 2U;
    next = (PdfFontInfo *)rt_realloc_array(info->fonts, next_capacity, sizeof(PdfFontInfo));
    if (next == 0) return -1;
    info->fonts = next;
    info->fonts_cap = next_capacity;
    return 0;
}

static int pdf_add_name_count(PdfNameCount **items_io, size_t *length_io, size_t *capacity_io, const char *name) {
    PdfNameCount *items = *items_io;
    size_t index;

    if (name == 0 || name[0] == '\0') return 0;
    for (index = 0U; index < *length_io; ++index) {
        if (rt_strcmp(items[index].name, name) == 0) {
            items[index].count += 1ULL;
            return 0;
        }
    }
    if (*length_io == *capacity_io) {
        size_t next_capacity = *capacity_io == 0U ? 16U : *capacity_io * 2U;
        PdfNameCount *next = (PdfNameCount *)rt_realloc_array(items, next_capacity, sizeof(PdfNameCount));
        if (next == 0) return -1;
        items = next;
        *items_io = items;
        *capacity_io = next_capacity;
    }
    rt_memset(&items[*length_io], 0, sizeof(items[*length_io]));
    rt_copy_string(items[*length_io].name, sizeof(items[*length_io].name), name);
    items[*length_io].count = 1ULL;
    *length_io += 1U;
    return 0;
}

static int pdf_collect_filter_value(const unsigned char *data, size_t size, size_t value_offset, PdfInfo *info) {
    size_t offset = pdf_skip_ws(data, size, value_offset);
    char name[PDF_NAME_CAPACITY];

    if (offset >= size) return 0;
    if (data[offset] == (unsigned char)'/') {
        pdf_copy_name(data, size, offset + 1U, name, sizeof(name));
        return pdf_add_name_count(&info->filters, &info->filters_len, &info->filters_cap, name);
    }
    if (data[offset] == (unsigned char)'[') {
        offset += 1U;
        while (offset < size && data[offset] != (unsigned char)']') {
            offset = pdf_skip_ws(data, size, offset);
            if (offset < size && data[offset] == (unsigned char)'/') {
                pdf_copy_name(data, size, offset + 1U, name, sizeof(name));
                if (pdf_add_name_count(&info->filters, &info->filters_len, &info->filters_cap, name) != 0) return -1;
                offset += 1U;
                while (offset < size && !pdf_is_delim(data[offset])) offset += 1U;
            } else if (offset < size) {
                offset += 1U;
            }
        }
    }
    return 0;
}

static int pdf_collect_filters(const unsigned char *data, size_t size, PdfInfo *info) {
    size_t offset = 0U;
    size_t key_offset;

    while (pdf_find_key_from(data, size, offset, "/Filter", &key_offset)) {
        if (pdf_collect_filter_value(data, size, key_offset + 7U, info) != 0) return -1;
        offset = key_offset + 7U;
    }
    return 0;
}

static int pdf_collect_encodings(const unsigned char *data, size_t size, PdfInfo *info) {
    size_t offset = 0U;
    size_t key_offset;
    char name[PDF_NAME_CAPACITY];

    while (pdf_find_key_from(data, size, offset, "/Encoding", &key_offset)) {
        size_t value_offset = pdf_skip_ws(data, size, key_offset + 9U);

        if (value_offset < size && data[value_offset] == (unsigned char)'/') {
            pdf_copy_name(data, size, value_offset + 1U, name, sizeof(name));
            if (pdf_add_name_count(&info->encodings, &info->encodings_len, &info->encodings_cap, name) != 0) return -1;
        }
        offset = key_offset + 9U;
    }
    if (pdf_find_name_value(data, size, "/CMapName", name, sizeof(name))) {
        if (pdf_add_name_count(&info->encodings, &info->encodings_len, &info->encodings_cap, name) != 0) return -1;
    }
    if (pdf_find_literal_value(data, size, "/Registry", name, sizeof(name))) {
        if (pdf_add_name_count(&info->encodings, &info->encodings_len, &info->encodings_cap, name) != 0) return -1;
    }
    if (pdf_find_literal_value(data, size, "/Ordering", name, sizeof(name))) {
        if (pdf_add_name_count(&info->encodings, &info->encodings_len, &info->encodings_cap, name) != 0) return -1;
    }
    return 0;
}

static void pdf_skip_literal_string(const unsigned char *data, size_t size, size_t *offset_io) {
    size_t offset = *offset_io + 1U;
    int depth = 1;

    while (offset < size && depth > 0) {
        unsigned char ch = data[offset++];

        if (ch == (unsigned char)'\\') {
            if (offset < size) offset += 1U;
        } else if (ch == (unsigned char)'(') {
            depth += 1;
        } else if (ch == (unsigned char)')') {
            depth -= 1;
        }
    }
    *offset_io = offset;
}

static void pdf_scan_content_operator(PdfInfo *info, const unsigned char *data, size_t start, size_t end) {
    size_t length = end - start;

    if (length == 2U && data[start] == (unsigned char)'B' && data[start + 1U] == (unsigned char)'T') info->text_object_count += 1ULL;
    else if (length == 2U && data[start] == (unsigned char)'T' && data[start + 1U] == (unsigned char)'j') info->text_show_count += 1ULL;
    else if (length == 2U && data[start] == (unsigned char)'T' && data[start + 1U] == (unsigned char)'J') info->text_show_count += 1ULL;
    else if (length == 1U && data[start] == (unsigned char)'\'') info->text_show_count += 1ULL;
    else if (length == 1U && data[start] == (unsigned char)'"') info->text_show_count += 1ULL;
    else if (length == 2U && data[start] == (unsigned char)'D' && data[start + 1U] == (unsigned char)'o') info->xobject_paint_count += 1ULL;
    else if (length == 2U && data[start] == (unsigned char)'B' && data[start + 1U] == (unsigned char)'I') info->inline_image_count += 1ULL;
    else if ((length == 1U && (data[start] == (unsigned char)'m' || data[start] == (unsigned char)'l' || data[start] == (unsigned char)'c' || data[start] == (unsigned char)'v' || data[start] == (unsigned char)'y' || data[start] == (unsigned char)'h' || data[start] == (unsigned char)'S' || data[start] == (unsigned char)'s' || data[start] == (unsigned char)'f' || data[start] == (unsigned char)'F' || data[start] == (unsigned char)'n')) ||
             (length == 2U && data[start] == (unsigned char)'r' && data[start + 1U] == (unsigned char)'e') ||
             (length == 2U && data[start] == (unsigned char)'f' && data[start + 1U] == (unsigned char)'*') ||
             (length == 1U && (data[start] == (unsigned char)'B' || data[start] == (unsigned char)'b')) ||
             (length == 2U && (data[start] == (unsigned char)'B' || data[start] == (unsigned char)'b') && data[start + 1U] == (unsigned char)'*')) info->path_operator_count += 1ULL;
}

static void pdf_scan_content_stream(const unsigned char *data, size_t start, size_t end, PdfInfo *info) {
    size_t offset = start;

    while (offset < end) {
        if (pdf_is_space(data[offset])) {
            offset += 1U;
        } else if (data[offset] == (unsigned char)'%') {
            while (offset < end && data[offset] != (unsigned char)'\n' && data[offset] != (unsigned char)'\r') offset += 1U;
        } else if (data[offset] == (unsigned char)'(') {
            pdf_skip_literal_string(data, end, &offset);
        } else if (data[offset] == (unsigned char)'<' && offset + 1U < end && data[offset + 1U] != (unsigned char)'<') {
            offset += 1U;
            while (offset < end && data[offset] != (unsigned char)'>') offset += 1U;
            if (offset < end) offset += 1U;
        } else if (data[offset] == (unsigned char)'/') {
            offset += 1U;
            while (offset < end && !pdf_is_delim(data[offset])) offset += 1U;
        } else if (data[offset] == (unsigned char)'\'' || data[offset] == (unsigned char)'"') {
            pdf_scan_content_operator(info, data, offset, offset + 1U);
            offset += 1U;
        } else if (pdf_is_delim(data[offset])) {
            offset += 1U;
        } else {
            size_t token_start = offset;
            while (offset < end && !pdf_is_delim(data[offset])) offset += 1U;
            pdf_scan_content_operator(info, data, token_start, offset);
        }
    }
}

size_t pdf_stream_body_start(const unsigned char *data, size_t size, size_t stream_offset) {
    size_t offset = stream_offset + 6U;

    if (offset < size && data[offset] == (unsigned char)'\r') {
        offset += 1U;
        if (offset < size && data[offset] == (unsigned char)'\n') offset += 1U;
    } else if (offset < size && data[offset] == (unsigned char)'\n') {
        offset += 1U;
    }
    return offset;
}

static size_t pdf_find_endstream_from_length(const unsigned char *data, size_t size, size_t content_start, unsigned long long stream_length) {
    size_t expected;
    size_t probe;
    size_t near_end;

    if (content_start > size) return size;
    if (stream_length > (unsigned long long)(size - content_start)) return size;
    expected = content_start + (size_t)stream_length;
    probe = expected;
    if (probe < size && data[probe] == (unsigned char)'\r') {
        probe += 1U;
        if (probe < size && data[probe] == (unsigned char)'\n') probe += 1U;
    } else if (probe < size && data[probe] == (unsigned char)'\n') {
        probe += 1U;
    }
    probe = pdf_skip_ws(data, size, probe);
    if (pdf_keyword_at(data, size, probe, "endstream")) return probe;
    near_end = expected + 128U;
    if (near_end < expected || near_end > size) near_end = size;
    return pdf_find_keyword(data, size, expected, near_end, "endstream");
}

void pdf_trim_stream_end(const unsigned char *data, size_t start, size_t *end_io) {
    while (*end_io > start && (data[*end_io - 1U] == (unsigned char)'\n' || data[*end_io - 1U] == (unsigned char)'\r')) *end_io -= 1U;
}

static void pdf_decoded_stream_free(PdfDecodedStream *stream) {
    if (stream == 0) return;
    rt_free(stream->owned);
    stream->data = 0;
    stream->size = 0U;
    stream->owned = 0;
}

static int pdf_name_is_flate(const char *name) {
    return rt_strcmp(name, "FlateDecode") == 0 || rt_strcmp(name, "Fl") == 0;
}

int pdf_stream_filter_kind(const unsigned char *dict, size_t dict_size) {
    size_t key_offset;
    size_t offset;
    char name[PDF_NAME_CAPACITY];

    if (!pdf_find_top_key(dict, dict_size, "/Filter", &key_offset)) return 0;
    offset = pdf_skip_ws(dict, dict_size, key_offset + 7U);
    if (offset >= dict_size) return -1;
    if (dict[offset] == (unsigned char)'/') {
        pdf_copy_name(dict, dict_size, offset + 1U, name, sizeof(name));
        return pdf_name_is_flate(name) ? 1 : -1;
    }
    if (dict[offset] == (unsigned char)'[') {
        int saw_filter = 0;

        offset += 1U;
        while (offset < dict_size) {
            offset = pdf_skip_ws(dict, dict_size, offset);
            if (offset >= dict_size) break;
            if (dict[offset] == (unsigned char)']') return saw_filter == 1 ? 1 : -1;
            if (dict[offset] != (unsigned char)'/') return -1;
            if (saw_filter) return -1;
            pdf_copy_name(dict, dict_size, offset + 1U, name, sizeof(name));
            if (!pdf_name_is_flate(name)) return -1;
            saw_filter = 1;
            offset += 1U;
            while (offset < dict_size && !pdf_is_delim(dict[offset])) offset += 1U;
        }
    }
    return -1;
}

static unsigned char pdf_paeth(unsigned char a, unsigned char b, unsigned char c) {
    int ai = (int)a;
    int bi = (int)b;
    int ci = (int)c;
    int p = ai + bi - ci;
    int pa = p > ai ? p - ai : ai - p;
    int pb = p > bi ? p - bi : bi - p;
    int pc = p > ci ? p - ci : ci - p;

    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static int pdf_apply_png_predictor(const unsigned char *dict, size_t dict_size, PdfDecodedStream *stream) {
    unsigned long long predictor = 1ULL;
    unsigned long long columns = 1ULL;
    unsigned long long colors = 1ULL;
    unsigned long long bits = 8ULL;
    size_t bpp;
    size_t row_in;
    size_t rows;
    size_t out_size;
    unsigned char *out;
    size_t row;

    if (!pdf_find_u64_direct_value(dict, dict_size, "/Predictor", &predictor) || predictor <= 1ULL) return 0;
    if (predictor == 2ULL) return -1;
    if (predictor < 10ULL || predictor > 15ULL) return -1;
    (void)pdf_find_u64_direct_value(dict, dict_size, "/Columns", &columns);
    (void)pdf_find_u64_direct_value(dict, dict_size, "/Colors", &colors);
    (void)pdf_find_u64_direct_value(dict, dict_size, "/BitsPerComponent", &bits);
    if (columns == 0ULL || colors == 0ULL || bits == 0ULL || columns > (unsigned long long)((size_t)-1 - 1U) || colors > 16ULL || bits != 8ULL) return -1;
    bpp = (size_t)colors;
    row_in = (size_t)columns + 1U;
    if (row_in == 0U || stream->size % row_in != 0U) return -1;
    rows = stream->size / row_in;
    if (rows > (size_t)-1 / (size_t)columns) return -1;
    out_size = rows * (size_t)columns;
    out = (unsigned char *)rt_malloc(out_size == 0U ? 1U : out_size);
    if (out == 0) return -1;
    for (row = 0U; row < rows; ++row) {
        size_t in_base = row * row_in;
        size_t out_base = row * (size_t)columns;
        unsigned char filter = stream->data[in_base];
        size_t col;

        if (filter > 4U) {
            rt_free(out);
            return -1;
        }
        for (col = 0U; col < (size_t)columns; ++col) {
            unsigned char raw = stream->data[in_base + 1U + col];
            unsigned char left = col >= bpp ? out[out_base + col - bpp] : 0U;
            unsigned char up = row != 0U ? out[out_base + col - (size_t)columns] : 0U;
            unsigned char up_left = row != 0U && col >= bpp ? out[out_base + col - (size_t)columns - bpp] : 0U;
            unsigned int value = raw;

            if (filter == 1U) value += (unsigned int)left;
            else if (filter == 2U) value += (unsigned int)up;
            else if (filter == 3U) value += ((unsigned int)left + (unsigned int)up) / 2U;
            else if (filter == 4U) value += (unsigned int)pdf_paeth(left, up, up_left);
            out[out_base + col] = (unsigned char)(value & 0xffU);
        }
    }
    rt_free(stream->owned);
    stream->owned = out;
    stream->data = out;
    stream->size = out_size;
    return 0;
}

static int pdf_inflate_stream(const unsigned char *input, size_t input_size, const unsigned char *dict, size_t dict_size, PdfDecodedStream *stream) {
    size_t capacity;

    if (input_size > PDF_FLATE_MAX_OUTPUT) return -1;
    if (input_size < 6U || (input[0] & 0x0fU) != 8U || ((((unsigned int)input[0] << 8U) + (unsigned int)input[1]) % 31U) != 0U || (input[1] & 0x20U) != 0U) return -1;
    capacity = input_size < 1024U ? 1024U : input_size * 2U;
    if (capacity < input_size || capacity > PDF_FLATE_MAX_OUTPUT) capacity = PDF_FLATE_MAX_OUTPUT;
    while (capacity <= PDF_FLATE_MAX_OUTPUT) {
        unsigned char *out = (unsigned char *)rt_malloc(capacity == 0U ? 1U : capacity);
        size_t output_size = 0U;

        if (out == 0) return -1;
        if (compression_zlib_inflate(input, input_size, out, capacity, &output_size) == 0) {
            stream->owned = out;
            stream->data = out;
            stream->size = output_size;
            if (pdf_apply_png_predictor(dict, dict_size, stream) != 0) {
                pdf_decoded_stream_free(stream);
                return -1;
            }
            return 0;
        }
        rt_free(out);
        if (capacity == PDF_FLATE_MAX_OUTPUT) break;
        if (capacity > PDF_FLATE_MAX_OUTPUT / 2U) capacity = PDF_FLATE_MAX_OUTPUT;
        else capacity *= 2U;
    }
    return -1;
}

static int pdf_decode_stream(const unsigned char *file_data, size_t file_size, const unsigned char *dict, size_t dict_size, size_t content_start, size_t endstream_offset, PdfDecodedStream *stream) {
    unsigned long long stream_length;
    size_t content_end = endstream_offset;
    int filter_kind;

    rt_memset(stream, 0, sizeof(*stream));
    if (content_start > file_size || endstream_offset > file_size || content_start > endstream_offset) return -1;
    if (pdf_find_top_u64_direct_value(dict, dict_size, "/Length", &stream_length) && stream_length <= (unsigned long long)(file_size - content_start)) {
        content_end = content_start + (size_t)stream_length;
    } else {
        pdf_trim_stream_end(file_data, content_start, &content_end);
    }
    filter_kind = pdf_stream_filter_kind(dict, dict_size);
    if (filter_kind == 0) {
        stream->data = file_data + content_start;
        stream->size = content_end - content_start;
        return 0;
    }
    if (filter_kind != 1) return -2;
    return pdf_inflate_stream(file_data + content_start, content_end - content_start, dict, dict_size, stream);
}

static int pdf_buffer_append_local(PdfBuffer *buffer, const unsigned char *data, size_t size) {
    return tool_byte_buffer_append(buffer, data, size);
}

int pdf_object_stream_data(const PdfDocument *document, const PdfObjectSpan *object, int decode, PdfBuffer *output) {
    const unsigned char *dict;
    size_t dict_size;
    size_t content_start;
    size_t content_end;
    PdfDecodedStream decoded;

    if (document == 0 || object == 0 || output == 0 || document->data == 0) return -1;
    if (object->stream_offset >= document->size || object->endstream_offset > document->size || object->body_start > object->stream_offset) return -1;
    dict = document->data + object->body_start;
    dict_size = object->stream_offset - object->body_start;
    content_start = pdf_stream_body_start(document->data, document->size, object->stream_offset);
    if (content_start > object->endstream_offset) return -1;
    if (decode) {
        int result = pdf_decode_stream(document->data, document->size, dict, dict_size, content_start, object->endstream_offset, &decoded);
        if (result != 0) return result;
        result = pdf_buffer_append_local(output, decoded.data, decoded.size);
        pdf_decoded_stream_free(&decoded);
        return result;
    }
    content_end = object->endstream_offset;
    if (object->body_start < object->stream_offset) {
        unsigned long long stream_length;

        if (pdf_find_top_u64_direct_value(dict, dict_size, "/Length", &stream_length) && stream_length <= (unsigned long long)(document->size - content_start)) {
            content_end = content_start + (size_t)stream_length;
        } else {
            pdf_trim_stream_end(document->data, content_start, &content_end);
        }
    }
    if (content_end < content_start || content_end > document->size) return -1;
    return pdf_buffer_append_local(output, document->data + content_start, content_end - content_start);
}

static int pdf_add_object(PdfInfo *info, unsigned long long number, unsigned long long generation, size_t offset, int has_file_span, size_t body_start, size_t body_end, size_t stream_offset, size_t endstream_offset, int has_stream, const char *type, const char *subtype) {
    PdfObjectInfo *object;
    size_t object_index;

    if (pdf_grow_objects(info) != 0) return -1;
    if (pdf_ensure_object_index_capacity(info) != 0) return -1;
    object_index = info->objects_len++;
    object = &info->objects[object_index];
    rt_memset(object, 0, sizeof(*object));
    object->number = (unsigned int)number;
    object->generation = (unsigned int)generation;
    object->offset = offset;
    object->has_file_span = has_file_span;
    object->body_start = body_start;
    object->body_end = body_end;
    object->stream_offset = stream_offset;
    object->endstream_offset = endstream_offset;
    object->has_stream = has_stream;
    rt_copy_string(object->type, sizeof(object->type), type != 0 ? type : "");
    rt_copy_string(object->subtype, sizeof(object->subtype), subtype != 0 ? subtype : "");
    pdf_object_index_insert_slot(info, object_index);
    return 0;
}

static int pdf_find_object_index(const PdfInfo *info, unsigned long long number, unsigned long long generation, size_t *index_out) {
    size_t index;

    if (number > 4294967295ULL || generation > 4294967295ULL) return 0;
    if (info->object_index_cap != 0U) {
        size_t slot = pdf_object_index_slot((unsigned int)number, (unsigned int)generation, info->object_index_cap);

        while (info->object_index[slot] != 0) {
            size_t existing_index = (size_t)(info->object_index[slot] - 1);
            const PdfObjectInfo *object = &info->objects[existing_index];

            if (object->number == (unsigned int)number && object->generation == (unsigned int)generation) {
                if (index_out != 0) *index_out = existing_index;
                return 1;
            }
            slot = (slot + 1U) & (info->object_index_cap - 1U);
        }
        return 0;
    }
    for (index = 0U; index < info->objects_len; ++index) {
        if (info->objects[index].number == (unsigned int)number && info->objects[index].generation == (unsigned int)generation) {
            if (index_out != 0) *index_out = index;
            return 1;
        }
    }
    return 0;
}

static int pdf_add_xref_visible_object(PdfInfo *info, unsigned long long number, unsigned long long generation, size_t offset) {
    if (number == 0ULL || number > 4294967295ULL || generation > 4294967295ULL) return 0;
    if (pdf_find_object_index(info, number, generation, 0)) return 0;
    if (pdf_add_object(info, number, generation, offset, 0, 0U, 0U, 0U, 0U, 0, "", "") != 0) return -1;
    info->object_count += 1ULL;
    return 0;
}

static int pdf_analyze_object(PdfInfo *info, const unsigned char *data, size_t size, unsigned long long number, unsigned long long generation, size_t object_offset, int has_file_span, size_t body_start, size_t body_end, size_t dict_end, size_t stream_offset, size_t endstream_offset, unsigned int flags);

static int pdf_parse_object_stream(PdfInfo *info, const unsigned char *file_data, size_t file_size, unsigned long long stream_number, size_t object_offset, const unsigned char *dict, size_t dict_size, size_t content_start, size_t endstream_offset) {
    PdfDecodedStream decoded;
    PdfObjectStreamEntry *entries = 0;
    unsigned long long n_value;
    unsigned long long first_value;
    size_t offset = 0U;
    size_t count;
    size_t index;
    int result = 0;

    (void)stream_number;
    if (!pdf_find_top_u64_direct_value(dict, dict_size, "/N", &n_value) || !pdf_find_top_u64_direct_value(dict, dict_size, "/First", &first_value)) return 0;
    if (n_value == 0ULL || n_value > PDF_OBJECT_STREAM_MAX_OBJECTS || first_value > (unsigned long long)((size_t)-1)) return 0;
    if (pdf_decode_stream(file_data, file_size, dict, dict_size, content_start, endstream_offset, &decoded) != 0) return 0;
    if (first_value > (unsigned long long)decoded.size) {
        pdf_decoded_stream_free(&decoded);
        return 0;
    }
    count = (size_t)n_value;
    entries = (PdfObjectStreamEntry *)rt_malloc_array(count, sizeof(PdfObjectStreamEntry));
    if (entries == 0) {
        pdf_decoded_stream_free(&decoded);
        return -1;
    }
    for (index = 0U; index < count; ++index) {
        unsigned long long number;
        unsigned long long relative;
        int parsed;

        parsed = pdf_read_u64_array_value(decoded.data, (size_t)first_value, &offset, &number);
        if (parsed != 1 || pdf_read_u64_array_value(decoded.data, (size_t)first_value, &offset, &relative) != 1 || relative > (unsigned long long)(decoded.size - (size_t)first_value)) {
            result = 0;
            goto done;
        }
        entries[index].number = number;
        entries[index].offset = (size_t)first_value + (size_t)relative;
        entries[index].end = decoded.size;
    }
    rt_sort(entries, count, sizeof(entries[0]), pdf_compare_object_stream_entries);
    {
        size_t current_offset = decoded.size;
        size_t current_end = decoded.size;

        for (index = count; index > 0U; --index) {
            size_t entry_index = index - 1U;

            if (entries[entry_index].offset < current_offset) {
                current_end = current_offset;
                current_offset = entries[entry_index].offset;
            }
            entries[entry_index].end = current_end;
        }
    }
    for (index = 0U; index < count; ++index) {
        size_t body_start = entries[index].offset;
        size_t body_end = entries[index].end;

        if (entries[index].number == 0ULL || entries[index].number > 4294967295ULL || body_start >= decoded.size || body_end > decoded.size || body_end <= body_start) continue;
        if (pdf_find_object_index(info, entries[index].number, 0ULL, 0)) continue;
        if (pdf_analyze_object(info, decoded.data, decoded.size, entries[index].number, 0ULL, object_offset, 0, body_start, body_end, body_end, decoded.size, decoded.size, 0U) != 0) {
            result = -1;
            goto done;
        }
    }
done:
    rt_free(entries);
    pdf_decoded_stream_free(&decoded);
    return result;
}

static unsigned long long pdf_xref_field_value(const unsigned char *entry, unsigned long long width, unsigned long long default_value) {
    unsigned long long value = 0ULL;
    unsigned long long index;

    if (width == 0ULL) return default_value;
    if (width > 8ULL) return 0ULL;
    for (index = 0ULL; index < width; ++index) value = (value << 8U) | (unsigned long long)entry[index];
    return value;
}

static int pdf_process_xref_entries(PdfInfo *info, const unsigned char *entries, size_t entries_size, unsigned long long first_object, unsigned long long count, unsigned long long w0, unsigned long long w1, unsigned long long w2) {
    unsigned long long entry_size = w0 + w1 + w2;
    unsigned long long index;

    if (entry_size == 0ULL || entry_size > 64ULL || count > PDF_XREF_STREAM_MAX_ENTRIES || count > (unsigned long long)(entries_size / (size_t)entry_size)) return 0;
    if (count != 0ULL && first_object > 18446744073709551615ULL - (count - 1ULL)) return 0;
    for (index = 0ULL; index < count; ++index) {
        const unsigned char *entry = entries + (size_t)(index * entry_size);
        unsigned long long type = pdf_xref_field_value(entry, w0, 1ULL);
        unsigned long long field1 = pdf_xref_field_value(entry + (size_t)w0, w1, 0ULL);
        unsigned long long field2 = pdf_xref_field_value(entry + (size_t)(w0 + w1), w2, 0ULL);
        unsigned long long number = first_object + index;

        if (type == 1ULL) {
            if (pdf_add_xref_visible_object(info, number, field2, (size_t)field1) != 0) return -1;
        } else if (type == 2ULL) {
            if (pdf_add_xref_visible_object(info, number, 0ULL, (size_t)field1) != 0) return -1;
        }
    }
    return 0;
}

static int pdf_parse_xref_stream(PdfInfo *info, const unsigned char *file_data, size_t file_size, const unsigned char *dict, size_t dict_size, size_t content_start, size_t endstream_offset) {
    PdfDecodedStream decoded;
    unsigned long long w[3];
    unsigned long long size_value = 0ULL;
    size_t array_offset;
    size_t stream_offset = 0U;
    int result = 0;

    if (!pdf_find_top_u64_array_offset(dict, dict_size, "/W", &array_offset)) return 0;
    if (pdf_read_u64_array_value(dict, dict_size, &array_offset, &w[0]) != 1 || pdf_read_u64_array_value(dict, dict_size, &array_offset, &w[1]) != 1 || pdf_read_u64_array_value(dict, dict_size, &array_offset, &w[2]) != 1) return 0;
    if (w[0] > 8ULL || w[1] > 8ULL || w[2] > 8ULL || w[0] + w[1] + w[2] == 0ULL || w[0] + w[1] + w[2] > 64ULL) return 0;
    (void)pdf_find_top_u64_direct_value(dict, dict_size, "/Size", &size_value);
    if (pdf_decode_stream(file_data, file_size, dict, dict_size, content_start, endstream_offset, &decoded) != 0) return 0;
    if (pdf_find_top_u64_array_offset(dict, dict_size, "/Index", &array_offset)) {
        unsigned long long total_entries = 0ULL;

        while (array_offset < dict_size) {
            unsigned long long first_object;
            unsigned long long count;
            int parsed = pdf_read_u64_array_value(dict, dict_size, &array_offset, &first_object);

            if (parsed == 0) break;
            if (parsed < 0 || pdf_read_u64_array_value(dict, dict_size, &array_offset, &count) != 1) break;
            if (stream_offset > decoded.size) break;
            if (count > PDF_XREF_STREAM_MAX_ENTRIES || total_entries > PDF_XREF_STREAM_MAX_ENTRIES - count) break;
            if (pdf_process_xref_entries(info, decoded.data + stream_offset, decoded.size - stream_offset, first_object, count, w[0], w[1], w[2]) != 0) {
                result = -1;
                goto done;
            }
            total_entries += count;
            stream_offset += (size_t)((w[0] + w[1] + w[2]) * count);
        }
    } else if (size_value != 0ULL) {
        result = pdf_process_xref_entries(info, decoded.data, decoded.size, 0ULL, size_value, w[0], w[1], w[2]);
    }
done:
    pdf_decoded_stream_free(&decoded);
    return result;
}

static int pdf_add_page(PdfInfo *info, unsigned long long number, unsigned long long generation, const unsigned char *dict, size_t dict_size) {
    PdfPageInfo *page;

    if (pdf_grow_pages(info) != 0) return -1;
    page = &info->pages[info->pages_len++];
    rt_memset(page, 0, sizeof(*page));
    page->object_number = (unsigned int)number;
    page->generation = (unsigned int)generation;
    page->has_media_box = pdf_find_top_box_value(dict, dict_size, "/MediaBox", page->media_box);
    page->has_crop_box = pdf_find_top_box_value(dict, dict_size, "/CropBox", page->crop_box);
    page->has_rotate = pdf_find_top_number_value(dict, dict_size, "/Rotate", &page->rotate);
    return 0;
}

static int pdf_add_font(PdfInfo *info, unsigned long long number, unsigned long long generation, const unsigned char *dict, size_t dict_size, const char *subtype) {
    PdfFontInfo *font;
    char name[PDF_NAME_CAPACITY];
    size_t key_offset;

    if (pdf_grow_fonts(info) != 0) return -1;
    font = &info->fonts[info->fonts_len++];
    rt_memset(font, 0, sizeof(*font));
    font->object_number = (unsigned int)number;
    font->generation = (unsigned int)generation;
    rt_copy_string(font->subtype, sizeof(font->subtype), subtype != 0 ? subtype : "");
    if (pdf_find_top_name_value(dict, dict_size, "/BaseFont", name, sizeof(name))) {
        rt_copy_string(font->base_font, sizeof(font->base_font), name);
        if (pdf_add_name_count(&info->font_names, &info->font_names_len, &info->font_names_cap, name) != 0) return -1;
    }
    if (pdf_find_top_name_value(dict, dict_size, "/Encoding", name, sizeof(name))) rt_copy_string(font->encoding, sizeof(font->encoding), name);
    font->has_to_unicode = pdf_find_key(dict, dict_size, "/ToUnicode", &key_offset);
    font->embedded_program_in_object = pdf_find_key(dict, dict_size, "/FontFile", &key_offset) || pdf_find_key(dict, dict_size, "/FontFile2", &key_offset) || pdf_find_key(dict, dict_size, "/FontFile3", &key_offset);
    return 0;
}

static int pdf_copy_info_field(const unsigned char *dict, size_t dict_size, const char *key, char *dest, size_t dest_size) {
    char value[PDF_TEXT_CAPACITY];

    if (!pdf_find_top_text_value(dict, dict_size, key, value, sizeof(value))) return 0;
    rt_copy_string(dest, dest_size, value);
    return 1;
}

static void pdf_collect_document_info(PdfInfo *info, unsigned long long number, unsigned long long generation, const unsigned char *dict, size_t dict_size) {
    PdfDocumentInfo *document = &info->document_info;
    PdfDocumentInfo next;
    int found = 0;

    rt_memset(&next, 0, sizeof(next));
    found += pdf_copy_info_field(dict, dict_size, "/Title", next.title, sizeof(next.title));
    found += pdf_copy_info_field(dict, dict_size, "/Author", next.author, sizeof(next.author));
    found += pdf_copy_info_field(dict, dict_size, "/Subject", next.subject, sizeof(next.subject));
    found += pdf_copy_info_field(dict, dict_size, "/Keywords", next.keywords, sizeof(next.keywords));
    found += pdf_copy_info_field(dict, dict_size, "/Creator", next.creator, sizeof(next.creator));
    found += pdf_copy_info_field(dict, dict_size, "/Producer", next.producer, sizeof(next.producer));
    found += pdf_copy_info_field(dict, dict_size, "/CreationDate", next.creation_date, sizeof(next.creation_date));
    found += pdf_copy_info_field(dict, dict_size, "/ModDate", next.modification_date, sizeof(next.modification_date));
    if (found != 0) {
        info->info_dictionary_count += 1ULL;
        *document = next;
        document->object_number = (unsigned int)number;
        document->generation = (unsigned int)generation;
    }
}

static int pdf_is_font_type(const char *type, const char *subtype) {
    if (type[0] != '\0' && rt_strcmp(type, "Font") == 0) return 1;
    if (subtype[0] == '\0') return 0;
    return rt_strcmp(subtype, "Type0") == 0 || rt_strcmp(subtype, "Type1") == 0 || rt_strcmp(subtype, "TrueType") == 0 || rt_strcmp(subtype, "Type3") == 0 || rt_strcmp(subtype, "CIDFontType0") == 0 || rt_strcmp(subtype, "CIDFontType2") == 0 || rt_strcmp(subtype, "MMType1") == 0;
}

static int pdf_analyze_object(PdfInfo *info, const unsigned char *data, size_t size, unsigned long long number, unsigned long long generation, size_t object_offset, int has_file_span, size_t body_start, size_t body_end, size_t dict_end, size_t stream_offset, size_t endstream_offset, unsigned int flags) {
    const unsigned char *dict = data + body_start;
    size_t dict_size = dict_end > body_start ? dict_end - body_start : 0U;
    char type[PDF_NAME_CAPACITY];
    char subtype[PDF_NAME_CAPACITY];
    int has_stream = stream_offset < size && endstream_offset <= size;
    size_t key_offset;

    type[0] = '\0';
    subtype[0] = '\0';
    (void)pdf_find_top_name_value(dict, dict_size, "/Type", type, sizeof(type));
    (void)pdf_find_top_name_value(dict, dict_size, "/Subtype", subtype, sizeof(subtype));
    if (pdf_add_object(info, number, generation, object_offset, has_file_span, body_start, body_end, stream_offset, endstream_offset, has_stream, type, subtype) != 0) return -1;
    info->object_count += 1ULL;
    if (has_stream) {
        size_t content_start = pdf_stream_body_start(data, size, stream_offset);
        size_t content_end = endstream_offset;

        info->stream_count += 1ULL;
        if (pdf_find_top_key(dict, dict_size, "/Filter", &key_offset)) {
            info->filtered_stream_count += 1ULL;
            if ((flags & PDF_ANALYZE_CONTENT_OPS) != 0U && (type[0] == '\0' || rt_strcmp(type, "XObject") == 0) && (subtype[0] == '\0' || rt_strcmp(subtype, "Form") == 0)) {
                PdfDecodedStream decoded;

                if (pdf_decode_stream(data, size, dict, dict_size, content_start, endstream_offset, &decoded) == 0) {
                    pdf_scan_content_stream(decoded.data, 0U, decoded.size, info);
                    pdf_decoded_stream_free(&decoded);
                }
            }
        } else if ((flags & PDF_ANALYZE_CONTENT_OPS) != 0U) {
            pdf_trim_stream_end(data, content_start, &content_end);
            if (content_start <= content_end && content_end <= size) pdf_scan_content_stream(data, content_start, content_end, info);
        }
    }
    if (pdf_collect_filters(dict, dict_size, info) != 0) return -1;
    if (pdf_collect_encodings(dict, dict_size, info) != 0) return -1;
    pdf_collect_document_info(info, number, generation, dict, dict_size);
    if (pdf_find_key(dict, dict_size, "/Encrypt", &key_offset)) info->encrypted = 1ULL;
    if (pdf_find_key(dict, dict_size, "/FontFile", &key_offset) || pdf_find_key(dict, dict_size, "/FontFile2", &key_offset) || pdf_find_key(dict, dict_size, "/FontFile3", &key_offset)) info->embedded_font_program_count += 1ULL;
    if (type[0] != '\0' && rt_strcmp(type, "Catalog") == 0) info->catalog_count += 1ULL;
    if (type[0] != '\0' && rt_strcmp(type, "Pages") == 0) info->pages_tree_count += 1ULL;
    if (type[0] != '\0' && rt_strcmp(type, "Page") == 0) {
        info->page_count += 1ULL;
        if (pdf_add_page(info, number, generation, dict, dict_size) != 0) return -1;
    }
    if (pdf_is_font_type(type, subtype)) {
        info->font_count += 1ULL;
        if (pdf_add_font(info, number, generation, dict, dict_size, subtype) != 0) return -1;
    }
    if (type[0] != '\0' && rt_strcmp(type, "ObjStm") == 0) {
        info->object_stream_count += 1ULL;
        if (has_stream) {
            size_t content_start = pdf_stream_body_start(data, size, stream_offset);

            if (pdf_parse_object_stream(info, data, size, number, object_offset, dict, dict_size, content_start, endstream_offset) != 0) return -1;
        }
    }
    if (type[0] != '\0' && rt_strcmp(type, "XRef") == 0) info->xref_stream_count += 1ULL;
    if (subtype[0] != '\0' && rt_strcmp(subtype, "Image") == 0) info->image_count += 1ULL;
    if (subtype[0] != '\0' && rt_strcmp(subtype, "Form") == 0) info->form_xobject_count += 1ULL;
    if (pdf_find_key(dict, dict_size, "/Annots", &key_offset)) info->annotation_count += 1ULL;
    if ((type[0] != '\0' && rt_strcmp(type, "Metadata") == 0) || (subtype[0] != '\0' && rt_strcmp(subtype, "XML") == 0)) info->metadata_count += 1ULL;
    return 0;
}

static void pdf_scan_header(const unsigned char *data, size_t size, PdfInfo *info) {
    size_t limit = size < 1024U ? size : 1024U;
    size_t offset;

    for (offset = 0U; offset + 8U <= limit; ++offset) {
        if (pdf_text_at(data, size, offset, "%PDF-")) {
            info->has_header = 1;
            if (offset + 7U < size && pdf_is_digit(data[offset + 5U]) && data[offset + 6U] == (unsigned char)'.' && pdf_is_digit(data[offset + 7U])) {
                info->major_version = (unsigned int)(data[offset + 5U] - (unsigned char)'0');
                info->minor_version = (unsigned int)(data[offset + 7U] - (unsigned char)'0');
            }
            return;
        }
    }
}

static void pdf_scan_markers(const unsigned char *data, size_t size, PdfInfo *info) {
    size_t offset;

    for (offset = 0U; offset < size; ++offset) {
        if (!info->has_eof && data[offset] == (unsigned char)'%' && pdf_text_at_len(data, size, offset, "%%EOF", 5U)) info->has_eof = 1;
        if (data[offset] == (unsigned char)'x' && pdf_keyword_at_len(data, size, offset, "xref", 4U)) info->xref_table_count += 1ULL;
        if (data[offset] == (unsigned char)'t' && pdf_keyword_at_len(data, size, offset, "trailer", 7U)) info->trailer_count += 1ULL;
    }
}

static size_t pdf_find_dictionary_end(const unsigned char *data, size_t size, size_t dict_start) {
    size_t offset = dict_start;
    unsigned int depth = 0U;

    while (offset < size) {
        if (data[offset] == (unsigned char)'%') {
            while (offset < size && data[offset] != (unsigned char)'\n' && data[offset] != (unsigned char)'\r') offset += 1U;
        } else if (data[offset] == (unsigned char)'(') {
            pdf_skip_literal_string(data, size, &offset);
        } else if (data[offset] == (unsigned char)'<' && offset + 1U < size && data[offset + 1U] == (unsigned char)'<') {
            depth += 1U;
            offset += 2U;
        } else if (data[offset] == (unsigned char)'>' && offset + 1U < size && data[offset + 1U] == (unsigned char)'>') {
            offset += 2U;
            if (depth != 0U) {
                depth -= 1U;
                if (depth == 0U) return offset;
            }
        } else if (data[offset] == (unsigned char)'<' && offset + 1U < size) {
            offset += 1U;
            while (offset < size && data[offset] != (unsigned char)'>') offset += 1U;
            if (offset < size) offset += 1U;
        } else {
            offset += 1U;
        }
    }
    return size;
}

static void pdf_scan_trailer_encrypt(const unsigned char *data, size_t size, PdfInfo *info) {
    size_t offset = 0U;

    while (offset < size) {
        size_t trailer = pdf_find_keyword(data, size, offset, size, "trailer");
        size_t dict_start;
        size_t dict_end;
        size_t key_offset;

        if (trailer == size) break;
        dict_start = trailer + 7U;
        while (dict_start + 1U < size && !(data[dict_start] == (unsigned char)'<' && data[dict_start + 1U] == (unsigned char)'<')) dict_start += 1U;
        if (dict_start + 1U >= size) break;
        dict_end = pdf_find_dictionary_end(data, size, dict_start);
        if (dict_end > dict_start && pdf_find_top_key(data + dict_start, dict_end - dict_start, "/Encrypt", &key_offset)) info->encrypted = 1ULL;
        offset = dict_end > trailer ? dict_end : trailer + 7U;
    }
}

static int pdf_scan_xref_stream_entries(const unsigned char *data, size_t size, PdfInfo *info) {
    size_t offset = 0U;

    while (offset < size) {
        size_t parse_offset;
        unsigned long long number;
        unsigned long long generation;
        size_t body_start;
        size_t first_endobj;
        size_t stream_offset;
        size_t endstream_offset = size;
        size_t dict_end;
        char type[PDF_NAME_CAPACITY];

        if ((offset != 0U && !pdf_is_delim(data[offset - 1U])) || !pdf_is_digit(data[offset])) {
            offset += 1U;
            continue;
        }
        parse_offset = offset;
        if (pdf_parse_u64(data, size, &parse_offset, &number) != 0 || parse_offset >= size || !pdf_is_space(data[parse_offset])) {
            offset += 1U;
            continue;
        }
        parse_offset = pdf_skip_ws(data, size, parse_offset);
        if (pdf_parse_u64(data, size, &parse_offset, &generation) != 0 || parse_offset >= size || !pdf_is_space(data[parse_offset])) {
            offset += 1U;
            continue;
        }
        parse_offset = pdf_skip_ws(data, size, parse_offset);
        if (!pdf_keyword_at(data, size, parse_offset, "obj")) {
            offset += 1U;
            continue;
        }
        body_start = parse_offset + 3U;
        first_endobj = pdf_find_keyword(data, size, body_start, size, "endobj");
        if (first_endobj == size) break;
        stream_offset = pdf_find_keyword(data, size, body_start, first_endobj, "stream");
        dict_end = stream_offset < size ? stream_offset : first_endobj;
        type[0] = '\0';
        (void)pdf_find_top_name_value(data + body_start, dict_end > body_start ? dict_end - body_start : 0U, "/Type", type, sizeof(type));
        if (stream_offset < size && type[0] != '\0' && rt_strcmp(type, "XRef") == 0) {
            unsigned long long stream_length;
            size_t content_start = pdf_stream_body_start(data, size, stream_offset);

            if (dict_end > body_start && pdf_find_top_u64_direct_value(data + body_start, dict_end - body_start, "/Length", &stream_length)) {
                endstream_offset = pdf_find_endstream_from_length(data, size, content_start, stream_length);
            }
            if (endstream_offset == size) endstream_offset = pdf_find_keyword(data, size, stream_offset + 6U, size, "endstream");
            if (endstream_offset < size && pdf_parse_xref_stream(info, data, size, data + body_start, dict_end - body_start, content_start, endstream_offset) != 0) return -1;
        }
        offset = first_endobj + 6U;
    }
    return 0;
}

void pdf_info_init(PdfInfo *info) {
    if (info != 0) rt_memset(info, 0, sizeof(*info));
}

void pdf_info_free(PdfInfo *info) {
    if (info == 0) return;
    rt_free(info->objects);
    rt_free(info->object_index);
    rt_free(info->pages);
    rt_free(info->fonts);
    rt_free(info->filters);
    rt_free(info->encodings);
    rt_free(info->font_names);
    pdf_info_init(info);
}

int pdf_analyze_with_options(const unsigned char *data, size_t size, PdfInfo *info, unsigned int flags) {
    size_t offset = 0U;

    if (data == 0 || info == 0) return -1;
    pdf_info_init(info);
    pdf_scan_header(data, size, info);
    pdf_scan_markers(data, size, info);
    pdf_scan_trailer_encrypt(data, size, info);
    while (offset < size) {
        size_t object_offset = offset;
        size_t parse_offset;
        unsigned long long number;
        unsigned long long generation;
        size_t body_start;
        size_t first_endobj;
        size_t object_end;
        size_t stream_offset;
        size_t endstream_offset = size;
        size_t dict_end;

        if ((offset != 0U && !pdf_is_delim(data[offset - 1U])) || !pdf_is_digit(data[offset])) {
            offset += 1U;
            continue;
        }
        parse_offset = offset;
        if (pdf_parse_u64(data, size, &parse_offset, &number) != 0 || parse_offset >= size || !pdf_is_space(data[parse_offset])) {
            offset += 1U;
            continue;
        }
        parse_offset = pdf_skip_ws(data, size, parse_offset);
        if (pdf_parse_u64(data, size, &parse_offset, &generation) != 0 || parse_offset >= size || !pdf_is_space(data[parse_offset])) {
            offset += 1U;
            continue;
        }
        parse_offset = pdf_skip_ws(data, size, parse_offset);
        if (!pdf_keyword_at(data, size, parse_offset, "obj")) {
            offset += 1U;
            continue;
        }
        body_start = parse_offset + 3U;
        first_endobj = pdf_find_keyword(data, size, body_start, size, "endobj");
        stream_offset = pdf_find_keyword(data, size, body_start, first_endobj, "stream");
        object_end = first_endobj;
        dict_end = stream_offset < size ? stream_offset : first_endobj;
        if (stream_offset < size) {
            unsigned long long stream_length;
            size_t content_start = pdf_stream_body_start(data, size, stream_offset);

            endstream_offset = size;
            if (dict_end > body_start && pdf_find_top_u64_direct_value(data + body_start, dict_end - body_start, "/Length", &stream_length)) {
                endstream_offset = pdf_find_endstream_from_length(data, size, content_start, stream_length);
            }
            if (endstream_offset == size) endstream_offset = pdf_find_keyword(data, size, stream_offset + 6U, size, "endstream");
            if (endstream_offset < size) {
                object_end = pdf_find_keyword(data, size, endstream_offset + 9U, size, "endobj");
                if (object_end == size) object_end = endstream_offset + 9U;
            } else {
                stream_offset = size;
                dict_end = first_endobj;
                object_end = first_endobj;
            }
        }
        if (pdf_analyze_object(info, data, size, number, generation, object_offset, 1, body_start, object_end, dict_end, stream_offset, endstream_offset, flags) != 0) {
            pdf_info_free(info);
            return -1;
        }
        if (object_end >= size) break;
        offset = object_end + 6U;
    }
    if (pdf_scan_xref_stream_entries(data, size, info) != 0) {
        pdf_info_free(info);
        return -1;
    }
    if (!info->has_header) {
        pdf_info_free(info);
        return -1;
    }
    return 0;
}

int pdf_analyze(const unsigned char *data, size_t size, PdfInfo *info) {
    return pdf_analyze_with_options(data, size, info, PDF_ANALYZE_CONTENT_OPS);
}

long long pdf_abs_fixed(long long value) {
    return value < 0LL ? -value : value;
}

static int pdf_near_points(long long value, long long target) {
    long long delta = value - target * 1000LL;
    return pdf_abs_fixed(delta) <= 2000LL;
}

static int pdf_page_matches(long long width, long long height, long long portrait_width, long long portrait_height) {
    return (pdf_near_points(width, portrait_width) && pdf_near_points(height, portrait_height)) || (pdf_near_points(width, portrait_height) && pdf_near_points(height, portrait_width));
}

const char *pdf_page_format_name(long long width, long long height) {
    if (pdf_page_matches(width, height, 612LL, 792LL)) return "Letter";
    if (pdf_page_matches(width, height, 612LL, 1008LL)) return "Legal";
    if (pdf_page_matches(width, height, 792LL, 1224LL)) return "Tabloid";
    if (pdf_page_matches(width, height, 595LL, 842LL)) return "A4";
    if (pdf_page_matches(width, height, 842LL, 1191LL)) return "A3";
    if (pdf_page_matches(width, height, 420LL, 595LL)) return "A5";
    return "custom";
}

static int pdf_date_digit(char ch) {
    if (ch < '0' || ch > '9') return -1;
    return ch - '0';
}

static int pdf_parse_date_digits(const char *text, size_t offset, size_t count, unsigned int *value_out) {
    unsigned int value = 0U;
    size_t index;

    for (index = 0U; index < count; ++index) {
        int digit = pdf_date_digit(text[offset + index]);
        if (digit < 0) return 0;
        value = value * 10U + (unsigned int)digit;
    }
    *value_out = value;
    return 1;
}

static int pdf_append_char(char *buffer, size_t buffer_size, size_t *used_io, char ch) {
    if (buffer_size == 0U || *used_io + 1U >= buffer_size) return 0;
    buffer[*used_io] = ch;
    *used_io += 1U;
    buffer[*used_io] = '\0';
    return 1;
}

static int pdf_append_cstr(char *buffer, size_t buffer_size, size_t *used_io, const char *text) {
    size_t index;

    for (index = 0U; text[index] != '\0'; ++index) {
        if (!pdf_append_char(buffer, buffer_size, used_io, text[index])) return 0;
    }
    return 1;
}

static int pdf_append_two_digits(char *buffer, size_t buffer_size, size_t *used_io, unsigned int value) {
    return pdf_append_char(buffer, buffer_size, used_io, (char)('0' + ((value / 10U) % 10U))) && pdf_append_char(buffer, buffer_size, used_io, (char)('0' + (value % 10U)));
}

static int pdf_append_four_digits(char *buffer, size_t buffer_size, size_t *used_io, unsigned int value) {
    return pdf_append_char(buffer, buffer_size, used_io, (char)('0' + ((value / 1000U) % 10U))) &&
           pdf_append_char(buffer, buffer_size, used_io, (char)('0' + ((value / 100U) % 10U))) &&
           pdf_append_char(buffer, buffer_size, used_io, (char)('0' + ((value / 10U) % 10U))) &&
           pdf_append_char(buffer, buffer_size, used_io, (char)('0' + (value % 10U)));
}

int pdf_format_date(const char *pdf_date, char *buffer, size_t buffer_size) {
    size_t length;
    size_t offset = 0U;
    size_t used = 0U;
    unsigned int year;
    unsigned int month = 1U;
    unsigned int day = 1U;
    unsigned int hour = 0U;
    unsigned int minute = 0U;
    unsigned int second = 0U;

    if (buffer_size == 0U) return 0;
    buffer[0] = '\0';
    if (pdf_date == 0 || pdf_date[0] == '\0') return 0;
    length = rt_strlen(pdf_date);
    if (length >= 2U && pdf_date[0] == 'D' && pdf_date[1] == ':') offset = 2U;
    if (length < offset + 4U || !pdf_parse_date_digits(pdf_date, offset, 4U, &year)) return 0;
    offset += 4U;
    if (length >= offset + 2U && !pdf_parse_date_digits(pdf_date, offset, 2U, &month)) return 0;
    if (length >= offset + 2U) offset += 2U;
    if (length >= offset + 2U && !pdf_parse_date_digits(pdf_date, offset, 2U, &day)) return 0;
    if (length >= offset + 2U) offset += 2U;
    if (length >= offset + 2U && !pdf_parse_date_digits(pdf_date, offset, 2U, &hour)) return 0;
    if (length >= offset + 2U) offset += 2U;
    if (length >= offset + 2U && !pdf_parse_date_digits(pdf_date, offset, 2U, &minute)) return 0;
    if (length >= offset + 2U) offset += 2U;
    if (length >= offset + 2U && !pdf_parse_date_digits(pdf_date, offset, 2U, &second)) return 0;
    if (length >= offset + 2U) offset += 2U;
    if (month < 1U || month > 12U || day < 1U || day > 31U || hour > 23U || minute > 59U || second > 60U) return 0;
    if (!pdf_append_four_digits(buffer, buffer_size, &used, year) || !pdf_append_char(buffer, buffer_size, &used, '-') || !pdf_append_two_digits(buffer, buffer_size, &used, month) || !pdf_append_char(buffer, buffer_size, &used, '-') || !pdf_append_two_digits(buffer, buffer_size, &used, day)) return 0;
    if (!pdf_append_char(buffer, buffer_size, &used, ' ') || !pdf_append_two_digits(buffer, buffer_size, &used, hour) || !pdf_append_char(buffer, buffer_size, &used, ':') || !pdf_append_two_digits(buffer, buffer_size, &used, minute) || !pdf_append_char(buffer, buffer_size, &used, ':') || !pdf_append_two_digits(buffer, buffer_size, &used, second)) return 0;
    if (offset < length) {
        char zone = pdf_date[offset];
        unsigned int zone_hour = 0U;
        unsigned int zone_minute = 0U;

        if (zone == 'Z' || zone == 'z') {
            if (!pdf_append_cstr(buffer, buffer_size, &used, " UTC")) return 0;
        } else if (zone == '+' || zone == '-') {
            offset += 1U;
            if (length < offset + 2U || !pdf_parse_date_digits(pdf_date, offset, 2U, &zone_hour)) return used != 0U;
            offset += 2U;
            if (offset < length && pdf_date[offset] == '\'') offset += 1U;
            if (length >= offset + 2U) {
                if (!pdf_parse_date_digits(pdf_date, offset, 2U, &zone_minute)) return used != 0U;
            }
            if (zone_hour > 23U || zone_minute > 59U) return used != 0U;
            if (!pdf_append_cstr(buffer, buffer_size, &used, " UTC")) return 0;
            if (zone_hour == 0U && zone_minute == 0U) return 1;
            if (!pdf_append_char(buffer, buffer_size, &used, zone) || !pdf_append_two_digits(buffer, buffer_size, &used, zone_hour) || !pdf_append_char(buffer, buffer_size, &used, ':') || !pdf_append_two_digits(buffer, buffer_size, &used, zone_minute)) return 0;
        }
    }
    return used != 0U;
}