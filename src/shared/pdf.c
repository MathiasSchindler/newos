#include "pdf.h"
#include "runtime.h"

static int pdf_is_space(unsigned char ch) {
    return ch == 0U || ch == 9U || ch == 10U || ch == 12U || ch == 13U || ch == 32U;
}

static int pdf_is_digit(unsigned char ch) {
    return ch >= (unsigned char)'0' && ch <= (unsigned char)'9';
}

static int pdf_is_delim(unsigned char ch) {
    return pdf_is_space(ch) || ch == (unsigned char)'(' || ch == (unsigned char)')' || ch == (unsigned char)'<' || ch == (unsigned char)'>' || ch == (unsigned char)'[' || ch == (unsigned char)']' || ch == (unsigned char)'{' || ch == (unsigned char)'}' || ch == (unsigned char)'/' || ch == (unsigned char)'%';
}

static size_t pdf_skip_ws(const unsigned char *data, size_t size, size_t offset) {
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

static int pdf_text_at(const unsigned char *data, size_t size, size_t offset, const char *text) {
    size_t length = rt_strlen(text);
    size_t index;

    if (offset > size || length > size - offset) return 0;
    for (index = 0U; index < length; ++index) {
        if (data[offset + index] != (unsigned char)text[index]) return 0;
    }
    return 1;
}

static int pdf_keyword_at(const unsigned char *data, size_t size, size_t offset, const char *text) {
    size_t length = rt_strlen(text);

    if (!pdf_text_at(data, size, offset, text)) return 0;
    if (offset != 0U && !pdf_is_delim(data[offset - 1U])) return 0;
    if (offset + length < size && !pdf_is_delim(data[offset + length])) return 0;
    return 1;
}

static size_t pdf_find_keyword(const unsigned char *data, size_t size, size_t start, size_t end, const char *text) {
    size_t length = rt_strlen(text);
    size_t offset;

    if (end > size) end = size;
    if (length == 0U || start >= end || length > end - start) return size;
    for (offset = start; offset + length <= end; ++offset) {
        if (pdf_keyword_at(data, size, offset, text)) return offset;
    }
    return size;
}

static int pdf_parse_u64(const unsigned char *data, size_t size, size_t *offset_io, unsigned long long *value_out) {
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

static void pdf_copy_name(const unsigned char *data, size_t size, size_t offset, char *buffer, size_t buffer_size) {
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
        if (data[offset] == (unsigned char)'/' && pdf_text_at(data, size, offset, key) && (offset + key_length >= size || pdf_is_delim(data[offset + key_length]))) {
            *offset_out = offset;
            return 1;
        }
    }
    return 0;
}

static int pdf_find_key(const unsigned char *data, size_t size, const char *key, size_t *offset_out) {
    return pdf_find_key_from(data, size, 0U, key, offset_out);
}

static int pdf_find_top_key(const unsigned char *data, size_t size, const char *key, size_t *offset_out) {
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
        } else if (depth == 1U && data[offset] == (unsigned char)'/' && offset + key_length <= size && pdf_text_at(data, size, offset, key) && (offset + key_length >= size || pdf_is_delim(data[offset + key_length]))) {
            *offset_out = offset;
            return 1;
        } else {
            offset += 1U;
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

static size_t pdf_stream_body_start(const unsigned char *data, size_t size, size_t stream_offset) {
    size_t offset = stream_offset + 6U;

    if (offset < size && data[offset] == (unsigned char)'\r') {
        offset += 1U;
        if (offset < size && data[offset] == (unsigned char)'\n') offset += 1U;
    } else if (offset < size && data[offset] == (unsigned char)'\n') {
        offset += 1U;
    }
    return offset;
}

static void pdf_trim_stream_end(const unsigned char *data, size_t start, size_t *end_io) {
    while (*end_io > start && (data[*end_io - 1U] == (unsigned char)'\n' || data[*end_io - 1U] == (unsigned char)'\r')) *end_io -= 1U;
}

static int pdf_add_object(PdfInfo *info, unsigned long long number, unsigned long long generation, size_t offset, int has_stream, const char *type, const char *subtype) {
    PdfObjectInfo *object;

    if (pdf_grow_objects(info) != 0) return -1;
    object = &info->objects[info->objects_len++];
    rt_memset(object, 0, sizeof(*object));
    object->number = (unsigned int)number;
    object->generation = (unsigned int)generation;
    object->offset = offset;
    object->has_stream = has_stream;
    rt_copy_string(object->type, sizeof(object->type), type != 0 ? type : "");
    rt_copy_string(object->subtype, sizeof(object->subtype), subtype != 0 ? subtype : "");
    return 0;
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
    int found = 0;

    found += pdf_copy_info_field(dict, dict_size, "/Title", document->title, sizeof(document->title));
    found += pdf_copy_info_field(dict, dict_size, "/Author", document->author, sizeof(document->author));
    found += pdf_copy_info_field(dict, dict_size, "/Subject", document->subject, sizeof(document->subject));
    found += pdf_copy_info_field(dict, dict_size, "/Keywords", document->keywords, sizeof(document->keywords));
    found += pdf_copy_info_field(dict, dict_size, "/Creator", document->creator, sizeof(document->creator));
    found += pdf_copy_info_field(dict, dict_size, "/Producer", document->producer, sizeof(document->producer));
    found += pdf_copy_info_field(dict, dict_size, "/CreationDate", document->creation_date, sizeof(document->creation_date));
    found += pdf_copy_info_field(dict, dict_size, "/ModDate", document->modification_date, sizeof(document->modification_date));
    if (found != 0) {
        info->info_dictionary_count += 1ULL;
        document->object_number = (unsigned int)number;
        document->generation = (unsigned int)generation;
    }
}

static int pdf_is_font_type(const char *type, const char *subtype) {
    if (type[0] != '\0' && rt_strcmp(type, "Font") == 0) return 1;
    if (subtype[0] == '\0') return 0;
    return rt_strcmp(subtype, "Type0") == 0 || rt_strcmp(subtype, "Type1") == 0 || rt_strcmp(subtype, "TrueType") == 0 || rt_strcmp(subtype, "Type3") == 0 || rt_strcmp(subtype, "CIDFontType0") == 0 || rt_strcmp(subtype, "CIDFontType2") == 0 || rt_strcmp(subtype, "MMType1") == 0;
}

static int pdf_analyze_object(PdfInfo *info, const unsigned char *data, size_t size, unsigned long long number, unsigned long long generation, size_t object_offset, size_t body_start, size_t dict_end, size_t stream_offset, size_t endstream_offset) {
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
    if (pdf_add_object(info, number, generation, object_offset, has_stream, type, subtype) != 0) return -1;
    info->object_count += 1ULL;
    if (has_stream) {
        size_t content_start = pdf_stream_body_start(data, size, stream_offset);
        size_t content_end = endstream_offset;

        info->stream_count += 1ULL;
        pdf_trim_stream_end(data, content_start, &content_end);
        if (pdf_find_top_key(dict, dict_size, "/Filter", &key_offset)) info->filtered_stream_count += 1ULL;
        else if (content_start <= content_end && content_end <= size) pdf_scan_content_stream(data, content_start, content_end, info);
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
    if (type[0] != '\0' && rt_strcmp(type, "ObjStm") == 0) info->object_stream_count += 1ULL;
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

    for (offset = 0U; offset + 5U <= size; ++offset) {
        if (!info->has_eof && pdf_text_at(data, size, offset, "%%EOF")) info->has_eof = 1;
        if (pdf_keyword_at(data, size, offset, "xref")) info->xref_table_count += 1ULL;
        if (pdf_keyword_at(data, size, offset, "trailer")) info->trailer_count += 1ULL;
    }
}

void pdf_info_init(PdfInfo *info) {
    if (info != 0) rt_memset(info, 0, sizeof(*info));
}

void pdf_info_free(PdfInfo *info) {
    if (info == 0) return;
    rt_free(info->objects);
    rt_free(info->pages);
    rt_free(info->fonts);
    rt_free(info->filters);
    rt_free(info->encodings);
    rt_free(info->font_names);
    pdf_info_init(info);
}

int pdf_analyze(const unsigned char *data, size_t size, PdfInfo *info) {
    size_t offset = 0U;

    if (data == 0 || info == 0) return -1;
    pdf_info_init(info);
    pdf_scan_header(data, size, info);
    pdf_scan_markers(data, size, info);
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
            endstream_offset = pdf_find_keyword(data, size, stream_offset + 6U, size, "endstream");
            if (endstream_offset < size) {
                object_end = pdf_find_keyword(data, size, endstream_offset + 9U, size, "endobj");
                if (object_end == size) object_end = endstream_offset + 9U;
            } else {
                stream_offset = size;
                dict_end = first_endobj;
                object_end = first_endobj;
            }
        }
        if (pdf_analyze_object(info, data, size, number, generation, object_offset, body_start, dict_end, stream_offset, endstream_offset) != 0) {
            pdf_info_free(info);
            return -1;
        }
        if (object_end >= size) break;
        offset = object_end + 6U;
    }
    if (!info->has_header) {
        pdf_info_free(info);
        return -1;
    }
    return 0;
}

static long long pdf_abs_fixed(long long value) {
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