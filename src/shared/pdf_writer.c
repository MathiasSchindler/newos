#include "pdf.h"
#include "runtime.h"

#define PDFW_OBJECT_STREAM_MAX_OBJECTS 8192U

typedef struct {
    const PdfDocument *document;
    unsigned int *map;
    size_t map_len;
} PdfRewriteMap;

typedef struct {
    unsigned int number;
    size_t offset;
    size_t end;
} PdfwObjectStreamEntry;

static int pdfw_is_space(unsigned char ch) {
    return ch == 0U || ch == 9U || ch == 10U || ch == 12U || ch == 13U || ch == 32U;
}

static int pdfw_is_digit(unsigned char ch) {
    return ch >= (unsigned char)'0' && ch <= (unsigned char)'9';
}

static int pdfw_is_delim(unsigned char ch) {
    return pdfw_is_space(ch) || ch == (unsigned char)'(' || ch == (unsigned char)')' || ch == (unsigned char)'<' || ch == (unsigned char)'>' || ch == (unsigned char)'[' || ch == (unsigned char)']' || ch == (unsigned char)'{' || ch == (unsigned char)'}' || ch == (unsigned char)'/' || ch == (unsigned char)'%';
}

static size_t pdfw_strlen_bytes(const unsigned char *data, size_t size, size_t offset, const char *text) {
    size_t length = rt_strlen(text);
    size_t index;

    if (offset > size || length > size - offset) return 0U;
    for (index = 0U; index < length; ++index) {
        if (data[offset + index] != (unsigned char)text[index]) return 0U;
    }
    return length;
}

static size_t pdfw_skip_ws(const unsigned char *data, size_t size, size_t offset) {
    while (offset < size) {
        if (pdfw_is_space(data[offset])) {
            offset += 1U;
        } else if (data[offset] == (unsigned char)'%') {
            while (offset < size && data[offset] != (unsigned char)'\n' && data[offset] != (unsigned char)'\r') offset += 1U;
        } else {
            break;
        }
    }
    return offset;
}

static int pdfw_parse_u64(const unsigned char *data, size_t size, size_t *offset_io, unsigned long long *value_out) {
    unsigned long long value = 0ULL;
    size_t offset = *offset_io;
    int saw_digit = 0;

    while (offset < size && pdfw_is_digit(data[offset])) {
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

static void pdfw_skip_literal(const unsigned char *data, size_t size, size_t *offset_io) {
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

static int pdfw_find_top_key(const unsigned char *data, size_t size, const char *key, size_t *offset_out) {
    size_t key_length = rt_strlen(key);
    size_t offset = 0U;
    unsigned int depth = 0U;

    while (offset < size) {
        if (data[offset] == (unsigned char)'%') {
            while (offset < size && data[offset] != (unsigned char)'\n' && data[offset] != (unsigned char)'\r') offset += 1U;
        } else if (data[offset] == (unsigned char)'(') {
            pdfw_skip_literal(data, size, &offset);
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
        } else if (depth == 1U && data[offset] == (unsigned char)'/' && offset + key_length <= size && pdfw_strlen_bytes(data, size, offset, key) != 0U && (offset + key_length >= size || pdfw_is_delim(data[offset + key_length]))) {
            *offset_out = offset;
            return 1;
        } else {
            offset += 1U;
        }
    }
    return 0;
}

static void pdfw_copy_name(const unsigned char *data, size_t size, size_t offset, char *buffer, size_t buffer_size) {
    size_t used = 0U;

    if (buffer_size == 0U) return;
    while (offset < size && !pdfw_is_delim(data[offset])) {
        if (used + 1U < buffer_size) buffer[used++] = (char)data[offset];
        offset += 1U;
    }
    buffer[used] = '\0';
}

static int pdfw_find_top_name_value(const unsigned char *data, size_t size, const char *key, char *buffer, size_t buffer_size) {
    size_t key_offset;
    size_t value_offset;

    if (buffer_size != 0U) buffer[0] = '\0';
    if (!pdfw_find_top_key(data, size, key, &key_offset)) return 0;
    value_offset = pdfw_skip_ws(data, size, key_offset + rt_strlen(key));
    if (value_offset >= size || data[value_offset] != (unsigned char)'/') return 0;
    pdfw_copy_name(data, size, value_offset + 1U, buffer, buffer_size);
    return buffer_size != 0U && buffer[0] != '\0';
}

static int pdfw_append_bytes(PdfBuffer *buffer, const unsigned char *data, size_t size) {
    return tool_byte_buffer_append(buffer, data, size);
}

static int pdfw_append_cstr(PdfBuffer *buffer, const char *text) {
    return pdfw_append_bytes(buffer, (const unsigned char *)text, rt_strlen(text));
}

static int pdfw_append_char(PdfBuffer *buffer, char ch) {
    unsigned char byte = (unsigned char)ch;
    return pdfw_append_bytes(buffer, &byte, 1U);
}

static int pdfw_append_uint(PdfBuffer *buffer, unsigned long long value) {
    char digits[32];
    size_t count = 0U;

    if (value == 0ULL) return pdfw_append_char(buffer, '0');
    while (value != 0ULL && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (count != 0U) {
        count -= 1U;
        if (pdfw_append_char(buffer, digits[count]) != 0) return -1;
    }
    return 0;
}

static int pdfw_append_xref_offset(PdfBuffer *buffer, size_t value) {
    unsigned long long divisor = 1000000000ULL;

    while (divisor != 0ULL) {
        unsigned long long digit = ((unsigned long long)value / divisor) % 10ULL;
        if (pdfw_append_char(buffer, (char)('0' + digit)) != 0) return -1;
        divisor /= 10ULL;
    }
    return 0;
}

void pdf_buffer_init(PdfBuffer *buffer) {
    tool_byte_buffer_init(buffer);
}

void pdf_buffer_free(PdfBuffer *buffer) {
    tool_byte_buffer_free(buffer);
}

void pdf_document_init(PdfDocument *document) {
    if (document != 0) rt_memset(document, 0, sizeof(*document));
}

void pdf_document_free(PdfDocument *document) {
    size_t index;

    if (document == 0) return;
    pdf_info_free(&document->info);
    for (index = 0U; index < document->objects_len; ++index) rt_free(document->objects[index].owned_data);
    rt_free(document->objects);
    rt_free(document->pages);
    pdf_document_init(document);
}

static int pdfw_grow_spans(PdfDocument *document) {
    size_t next_capacity;
    PdfObjectSpan *next;

    if (document->objects_len < document->objects_cap) return 0;
    next_capacity = document->objects_cap == 0U ? 32U : document->objects_cap * 2U;
    next = (PdfObjectSpan *)rt_realloc_array(document->objects, next_capacity, sizeof(PdfObjectSpan));
    if (next == 0) return -1;
    document->objects = next;
    document->objects_cap = next_capacity;
    return 0;
}

static int pdfw_grow_pages(PdfDocument *document) {
    size_t next_capacity;
    PdfPageRef *next;

    if (document->pages_len < document->pages_cap) return 0;
    next_capacity = document->pages_cap == 0U ? 16U : document->pages_cap * 2U;
    next = (PdfPageRef *)rt_realloc_array(document->pages, next_capacity, sizeof(PdfPageRef));
    if (next == 0) return -1;
    document->pages = next;
    document->pages_cap = next_capacity;
    return 0;
}

static const unsigned char *pdfw_span_data(const PdfDocument *document, const PdfObjectSpan *span, size_t *size_out) {
    if (span->data != 0) {
        if (size_out != 0) *size_out = span->data_size;
        return span->data;
    }
    if (size_out != 0) *size_out = document->size;
    return document->data;
}

static int pdfw_add_page_ref(PdfDocument *document, unsigned int number, unsigned int generation) {
    PdfPageRef *page;

    if (pdfw_grow_pages(document) != 0) return -1;
    page = &document->pages[document->pages_len++];
    page->object_number = number;
    page->generation = generation;
    return 0;
}

static int pdfw_find_object_span(const PdfDocument *document, unsigned int number, unsigned int generation, size_t *index_out) {
    size_t index;

    for (index = 0U; index < document->objects_len; ++index) {
        if (document->objects[index].number == number && document->objects[index].generation == generation) {
            if (index_out != 0) *index_out = index;
            return 1;
        }
    }
    return 0;
}

static int pdfw_find_top_u64_value(const unsigned char *data, size_t size, const char *key, unsigned long long *value_out) {
    size_t key_offset;
    size_t value_offset;

    if (!pdfw_find_top_key(data, size, key, &key_offset)) return 0;
    value_offset = pdfw_skip_ws(data, size, key_offset + rt_strlen(key));
    if (pdfw_parse_u64(data, size, &value_offset, value_out) != 0) return 0;
    if (value_offset < size && !pdfw_is_delim(data[value_offset])) return 0;
    return 1;
}

static size_t pdfw_trim_object_stream_body_end(const unsigned char *data, size_t start, size_t end) {
    while (end > start && pdfw_is_space(data[end - 1U])) end -= 1U;
    return end;
}

static int pdfw_compare_object_stream_entries(const void *left_ptr, const void *right_ptr) {
    const PdfwObjectStreamEntry *left = (const PdfwObjectStreamEntry *)left_ptr;
    const PdfwObjectStreamEntry *right = (const PdfwObjectStreamEntry *)right_ptr;

    if (left->offset < right->offset) return -1;
    if (left->offset > right->offset) return 1;
    if (left->number < right->number) return -1;
    if (left->number > right->number) return 1;
    return 0;
}

static int pdfw_add_materialized_object(PdfDocument *document, unsigned int number, const unsigned char *body, size_t body_size) {
    unsigned char *copy;
    PdfObjectSpan *span;

    if (number == 0U || body == 0 || body_size == 0U) return 0;
    if (pdfw_find_object_span(document, number, 0U, 0)) return 0;
    copy = (unsigned char *)rt_malloc(body_size);
    if (copy == 0) return -1;
    memcpy(copy, body, body_size);
    if (pdfw_grow_spans(document) != 0) {
        rt_free(copy);
        return -1;
    }
    span = &document->objects[document->objects_len++];
    rt_memset(span, 0, sizeof(*span));
    span->number = number;
    span->generation = 0U;
    span->data = copy;
    span->data_size = body_size;
    span->owned_data = copy;
    span->body_start = 0U;
    span->body_end = body_size;
    span->stream_offset = body_size;
    span->endstream_offset = body_size;
    (void)pdfw_find_top_name_value(copy, body_size, "/Type", span->type, sizeof(span->type));
    (void)pdfw_find_top_name_value(copy, body_size, "/Subtype", span->subtype, sizeof(span->subtype));
    if (span->type[0] != '\0' && rt_strcmp(span->type, "Catalog") == 0) {
        document->catalog_object_number = span->number;
        document->catalog_generation = 0U;
    }
    if (span->type[0] != '\0' && rt_strcmp(span->type, "Page") == 0) {
        if (pdfw_add_page_ref(document, span->number, 0U) != 0) return -1;
    }
    if (span->number > document->max_object_number) document->max_object_number = span->number;
    return 0;
}

static int pdfw_materialize_object_stream(PdfDocument *document, const PdfObjectSpan *object) {
    const unsigned char *source;
    size_t source_size;
    const unsigned char *dict;
    size_t dict_size;
    unsigned long long n_value;
    unsigned long long first_value;
    PdfBuffer decoded;
    PdfwObjectStreamEntry *entries = 0;
    size_t offset = 0U;
    size_t count;
    size_t index;
    int result = -1;

    source = pdfw_span_data(document, object, &source_size);
    if (object->stream_offset > source_size || object->body_start > object->stream_offset) return -1;
    dict = source + object->body_start;
    dict_size = object->stream_offset - object->body_start;
    if (!pdfw_find_top_u64_value(dict, dict_size, "/N", &n_value) || !pdfw_find_top_u64_value(dict, dict_size, "/First", &first_value)) return -1;
    if (n_value == 0ULL || n_value > PDFW_OBJECT_STREAM_MAX_OBJECTS || first_value > (unsigned long long)((size_t)-1)) return -1;
    pdf_buffer_init(&decoded);
    if (pdf_object_stream_data(document, object, 1, &decoded) != 0) goto done;
    if (first_value > (unsigned long long)decoded.size) goto done;
    count = (size_t)n_value;
    entries = (PdfwObjectStreamEntry *)rt_malloc_array(count, sizeof(entries[0]));
    if (entries == 0) goto done;
    for (index = 0U; index < count; ++index) {
        unsigned long long number;
        unsigned long long relative;

        offset = pdfw_skip_ws(decoded.data, (size_t)first_value, offset);
        if (pdfw_parse_u64(decoded.data, (size_t)first_value, &offset, &number) != 0) goto done;
        offset = pdfw_skip_ws(decoded.data, (size_t)first_value, offset);
        if (pdfw_parse_u64(decoded.data, (size_t)first_value, &offset, &relative) != 0) goto done;
        if (number > 4294967295ULL || relative > (unsigned long long)(decoded.size - (size_t)first_value)) goto done;
        entries[index].number = (unsigned int)number;
        entries[index].offset = (size_t)first_value + (size_t)relative;
        entries[index].end = decoded.size;
    }
    rt_sort(entries, count, sizeof(entries[0]), pdfw_compare_object_stream_entries);
    for (index = 0U; index < count; ++index) {
        size_t body_start = entries[index].offset;
        size_t body_end = index + 1U < count ? entries[index + 1U].offset : decoded.size;

        if (body_start >= decoded.size || body_end > decoded.size || body_end <= body_start) goto done;
        entries[index].end = body_end;
    }
    for (index = 0U; index < count; ++index) {
        size_t body_start = entries[index].offset;
        size_t body_end = entries[index].end;

        body_end = pdfw_trim_object_stream_body_end(decoded.data, body_start, body_end);
        if (body_end <= body_start) goto done;
        if (pdfw_add_materialized_object(document, entries[index].number, decoded.data + body_start, body_end - body_start) != 0) goto done;
    }
    result = 0;
done:
    rt_free(entries);
    pdf_buffer_free(&decoded);
    return result;
}

static int pdfw_materialize_object_streams(PdfDocument *document) {
    size_t initial_len = document->objects_len;
    size_t index;

    for (index = 0U; index < initial_len; ++index) {
        PdfObjectSpan *span = &document->objects[index];

        if (span->type[0] != '\0' && rt_strcmp(span->type, "ObjStm") == 0) {
            if (pdfw_materialize_object_stream(document, span) != 0) return -1;
        }
    }
    return 0;
}

static int pdfw_add_span_from_info(PdfDocument *document, const PdfObjectInfo *object) {
    PdfObjectSpan *span;

    if (!object->has_file_span) return 0;
    if (pdfw_grow_spans(document) != 0) return -1;
    span = &document->objects[document->objects_len++];
    rt_memset(span, 0, sizeof(*span));
    span->number = object->number;
    span->generation = object->generation;
    span->object_offset = object->offset;
    span->body_start = object->body_start;
    span->body_end = object->body_end;
    span->stream_offset = object->stream_offset;
    span->endstream_offset = object->endstream_offset;
    rt_copy_string(span->type, sizeof(span->type), object->type);
    rt_copy_string(span->subtype, sizeof(span->subtype), object->subtype);
    if (span->type[0] != '\0' && rt_strcmp(span->type, "Catalog") == 0) {
        document->catalog_object_number = span->number;
        document->catalog_generation = span->generation;
    }
    if (span->type[0] != '\0' && rt_strcmp(span->type, "Page") == 0) {
        if (pdfw_add_page_ref(document, span->number, span->generation) != 0) return -1;
    }
    if (span->number > document->max_object_number) document->max_object_number = span->number;
    return 0;
}

static int pdfw_build_spans_from_info(PdfDocument *document) {
    size_t index;

    for (index = 0U; index < document->info.objects_len; ++index) {
        if (pdfw_add_span_from_info(document, &document->info.objects[index]) != 0) return -1;
    }
    return 0;
}

static void pdfw_scan_startxref(PdfDocument *document) {
    size_t offset;

    document->startxref_offset = 0U;
    if (document->size < 9U) return;
    offset = document->size - 9U;
    for (;;) {
        if (pdfw_strlen_bytes(document->data, document->size, offset, "startxref") != 0U) {
            size_t parse_offset = pdfw_skip_ws(document->data, document->size, offset + 9U);
            unsigned long long value;

            if (pdfw_parse_u64(document->data, document->size, &parse_offset, &value) == 0) document->startxref_offset = (size_t)value;
            return;
        }
        if (offset == 0U) break;
        offset -= 1U;
    }
}

int pdf_document_scan(const unsigned char *data, size_t size, PdfDocument *document) {
    return pdf_document_scan_with_options(data, size, document, 0U);
}

int pdf_document_scan_with_options(const unsigned char *data, size_t size, PdfDocument *document, unsigned int flags) {
    if (data == 0 || document == 0) return -1;
    pdf_document_init(document);
    document->data = data;
    document->size = size;
    if (pdf_analyze_with_options(data, size, &document->info, flags) != 0) {
        pdf_document_free(document);
        return -1;
    }
    if (pdfw_build_spans_from_info(document) != 0) {
        pdf_document_free(document);
        return -1;
    }
    pdfw_scan_startxref(document);
    return 0;
}

int pdf_document_parse(const unsigned char *data, size_t size, PdfDocument *document) {
    if (pdf_document_scan(data, size, document) != 0) return PDF_DOCUMENT_PARSE_UNREADABLE;
    if (document->info.encrypted != 0ULL) {
        pdf_document_free(document);
        return PDF_DOCUMENT_PARSE_ENCRYPTED;
    }
    if (document->info.object_stream_count != 0ULL) {
        if (pdfw_materialize_object_streams(document) != 0 || document->info.pages_len > document->pages_len) {
            pdf_document_free(document);
            return PDF_DOCUMENT_PARSE_OBJECT_STREAM_UNSUPPORTED;
        }
    }
    if (document->catalog_object_number == 0U || document->pages_len == 0U) {
        pdf_document_free(document);
        return PDF_DOCUMENT_PARSE_UNREADABLE;
    }
    return 0;
}

int pdf_document_info_has_fields(const PdfDocumentInfo *info) {
    if (info == 0) return 0;
    return info->title[0] != '\0' || info->author[0] != '\0' || info->subject[0] != '\0' || info->keywords[0] != '\0' || info->creator[0] != '\0' || info->producer[0] != '\0' || info->creation_date[0] != '\0' || info->modification_date[0] != '\0';
}

static char *pdfw_info_field(PdfDocumentInfo *info, const char *field) {
    if (rt_strcmp(field, "title") == 0 || rt_strcmp(field, "Title") == 0) return info->title;
    if (rt_strcmp(field, "author") == 0 || rt_strcmp(field, "Author") == 0) return info->author;
    if (rt_strcmp(field, "subject") == 0 || rt_strcmp(field, "Subject") == 0) return info->subject;
    if (rt_strcmp(field, "keywords") == 0 || rt_strcmp(field, "Keywords") == 0) return info->keywords;
    if (rt_strcmp(field, "creator") == 0 || rt_strcmp(field, "Creator") == 0) return info->creator;
    if (rt_strcmp(field, "producer") == 0 || rt_strcmp(field, "Producer") == 0) return info->producer;
    if (rt_strcmp(field, "creation_date") == 0 || rt_strcmp(field, "CreationDate") == 0 || rt_strcmp(field, "creationdate") == 0) return info->creation_date;
    if (rt_strcmp(field, "modification_date") == 0 || rt_strcmp(field, "ModDate") == 0 || rt_strcmp(field, "moddate") == 0) return info->modification_date;
    return 0;
}

int pdf_document_info_set_field(PdfDocumentInfo *info, const char *field, const char *value) {
    char *target;

    if (info == 0 || field == 0 || value == 0) return -1;
    target = pdfw_info_field(info, field);
    if (target == 0) return -1;
    rt_copy_string(target, PDF_TEXT_CAPACITY, value);
    return 0;
}

int pdf_document_info_remove_field(PdfDocumentInfo *info, const char *field) {
    char *target;

    if (info == 0 || field == 0) return -1;
    target = pdfw_info_field(info, field);
    if (target == 0) return -1;
    target[0] = '\0';
    return 0;
}

static int pdfw_page_selected(const PdfDocument *document, size_t first_page, size_t page_count, unsigned int number) {
    size_t end_page = first_page + page_count;
    size_t index;

    for (index = first_page; index < end_page && index < document->pages_len; ++index) {
        if (document->pages[index].object_number == number) return 1;
    }
    return 0;
}

static int pdfw_should_copy_object(const PdfDocument *document, const PdfObjectSpan *span, size_t first_page, size_t page_count) {
    if (span->type[0] != '\0' && rt_strcmp(span->type, "Catalog") == 0) return 0;
    if (span->type[0] != '\0' && rt_strcmp(span->type, "Pages") == 0) return 0;
    if (span->type[0] != '\0' && rt_strcmp(span->type, "XRef") == 0) return 0;
    if (span->type[0] != '\0' && rt_strcmp(span->type, "ObjStm") == 0) return 0;
    if (document->info.document_info.object_number == span->number) return 0;
    if (span->type[0] != '\0' && rt_strcmp(span->type, "Page") == 0) return pdfw_page_selected(document, first_page, page_count, span->number);
    return 1;
}

static int pdfw_append_escaped_text(PdfBuffer *buffer, const char *text) {
    size_t index;

    if (pdfw_append_char(buffer, '(') != 0) return -1;
    for (index = 0U; text[index] != '\0'; ++index) {
        char ch = text[index];

        if (ch == '(' || ch == ')' || ch == '\\') {
            if (pdfw_append_char(buffer, '\\') != 0 || pdfw_append_char(buffer, ch) != 0) return -1;
        } else if (ch == '\n') {
            if (pdfw_append_cstr(buffer, "\\n") != 0) return -1;
        } else if (ch == '\r') {
            if (pdfw_append_cstr(buffer, "\\r") != 0) return -1;
        } else if (ch == '\t') {
            if (pdfw_append_cstr(buffer, "\\t") != 0) return -1;
        } else {
            if (pdfw_append_char(buffer, ch) != 0) return -1;
        }
    }
    return pdfw_append_char(buffer, ')');
}

static int pdfw_append_info_entry(PdfBuffer *buffer, const char *key, const char *value) {
    if (value == 0 || value[0] == '\0') return 0;
    if (pdfw_append_char(buffer, '/') != 0 || pdfw_append_cstr(buffer, key) != 0 || pdfw_append_char(buffer, ' ') != 0) return -1;
    if (pdfw_append_escaped_text(buffer, value) != 0) return -1;
    return pdfw_append_char(buffer, '\n');
}

static int pdfw_write_info_dict(PdfBuffer *buffer, const PdfDocumentInfo *metadata) {
    if (pdfw_append_cstr(buffer, "<<\n") != 0) return -1;
    if (metadata != 0) {
        if (pdfw_append_info_entry(buffer, "Title", metadata->title) != 0) return -1;
        if (pdfw_append_info_entry(buffer, "Author", metadata->author) != 0) return -1;
        if (pdfw_append_info_entry(buffer, "Subject", metadata->subject) != 0) return -1;
        if (pdfw_append_info_entry(buffer, "Keywords", metadata->keywords) != 0) return -1;
        if (pdfw_append_info_entry(buffer, "Creator", metadata->creator) != 0) return -1;
        if (pdfw_append_info_entry(buffer, "Producer", metadata->producer) != 0) return -1;
        if (pdfw_append_info_entry(buffer, "CreationDate", metadata->creation_date) != 0) return -1;
        if (pdfw_append_info_entry(buffer, "ModDate", metadata->modification_date) != 0) return -1;
    }
    return pdfw_append_cstr(buffer, ">>\n");
}

static int pdfw_write_ref_or_null(PdfBuffer *buffer, const PdfRewriteMap *map, unsigned int old_number) {
    if ((size_t)old_number < map->map_len && map->map[old_number] != 0U) {
        if (pdfw_append_uint(buffer, map->map[old_number]) != 0 || pdfw_append_cstr(buffer, " 0 R") != 0) return -1;
    } else {
        if (pdfw_append_cstr(buffer, "null") != 0) return -1;
    }
    return 0;
}

static int pdfw_try_parent_rewrite(PdfBuffer *buffer, const unsigned char *data, size_t size, size_t *offset_io, unsigned int pages_object_number) {
    size_t offset = *offset_io;
    size_t after_key;
    size_t value_offset;
    size_t parse_offset;
    unsigned long long number;
    unsigned long long generation;

    if (pdfw_strlen_bytes(data, size, offset, "/Parent") == 0U) return 0;
    after_key = offset + 7U;
    if (after_key < size && !pdfw_is_delim(data[after_key])) return 0;
    value_offset = pdfw_skip_ws(data, size, after_key);
    parse_offset = value_offset;
    if (pdfw_parse_u64(data, size, &parse_offset, &number) != 0) return 0;
    parse_offset = pdfw_skip_ws(data, size, parse_offset);
    if (pdfw_parse_u64(data, size, &parse_offset, &generation) != 0) return 0;
    parse_offset = pdfw_skip_ws(data, size, parse_offset);
    if (parse_offset >= size || data[parse_offset] != (unsigned char)'R' || (parse_offset + 1U < size && !pdfw_is_delim(data[parse_offset + 1U]))) return 0;
    if (pdfw_append_cstr(buffer, "/Parent ") != 0 || pdfw_append_uint(buffer, pages_object_number) != 0 || pdfw_append_cstr(buffer, " 0 R") != 0) return -1;
    *offset_io = parse_offset + 1U;
    return 1;
}

static int pdfw_write_rewritten_range(PdfBuffer *buffer, const unsigned char *data, size_t start, size_t end, const PdfRewriteMap *map, int is_page, unsigned int pages_object_number) {
    size_t offset = start;

    while (offset < end) {
        if (is_page && data[offset] == (unsigned char)'/') {
            int parent_result = pdfw_try_parent_rewrite(buffer, data, end, &offset, pages_object_number);
            if (parent_result < 0) return -1;
            if (parent_result > 0) continue;
        }
        if (data[offset] == (unsigned char)'(') {
            size_t string_start = offset;
            pdfw_skip_literal(data, end, &offset);
            if (pdfw_append_bytes(buffer, data + string_start, offset - string_start) != 0) return -1;
        } else if (data[offset] == (unsigned char)'%') {
            size_t comment_start = offset;
            while (offset < end && data[offset] != (unsigned char)'\n' && data[offset] != (unsigned char)'\r') offset += 1U;
            if (pdfw_append_bytes(buffer, data + comment_start, offset - comment_start) != 0) return -1;
        } else if (data[offset] == (unsigned char)'<' && offset + 1U < end && data[offset + 1U] != (unsigned char)'<') {
            size_t hex_start = offset++;
            while (offset < end && data[offset] != (unsigned char)'>') offset += 1U;
            if (offset < end) offset += 1U;
            if (pdfw_append_bytes(buffer, data + hex_start, offset - hex_start) != 0) return -1;
        } else if (pdfw_is_digit(data[offset])) {
            size_t number_start = offset;
            size_t parse_offset = offset;
            unsigned long long number;
            unsigned long long generation;

            if (pdfw_parse_u64(data, end, &parse_offset, &number) == 0 && parse_offset < end && pdfw_is_space(data[parse_offset])) {
                size_t second_offset = pdfw_skip_ws(data, end, parse_offset);

                if (pdfw_parse_u64(data, end, &second_offset, &generation) == 0 && second_offset < end && pdfw_is_space(data[second_offset])) {
                    size_t ref_offset = pdfw_skip_ws(data, end, second_offset);

                    if (ref_offset < end && data[ref_offset] == (unsigned char)'R' && (ref_offset + 1U >= end || pdfw_is_delim(data[ref_offset + 1U])) && number <= 4294967295ULL) {
                        if (pdfw_write_ref_or_null(buffer, map, (unsigned int)number) != 0) return -1;
                        offset = ref_offset + 1U;
                        continue;
                    }
                }
            }
            while (offset < end && !pdfw_is_delim(data[offset])) offset += 1U;
            if (pdfw_append_bytes(buffer, data + number_start, offset - number_start) != 0) return -1;
        } else {
            if (pdfw_append_char(buffer, (char)data[offset]) != 0) return -1;
            offset += 1U;
        }
    }
    return 0;
}

static int pdfw_write_copied_object(PdfBuffer *buffer, const PdfObjectSpan *span, const PdfRewriteMap *map, unsigned int pages_object_number) {
    int is_page = span->type[0] != '\0' && rt_strcmp(span->type, "Page") == 0;
    unsigned int new_number = 0U;
    const unsigned char *source;
    size_t source_size;

    if ((size_t)span->number >= map->map_len) return -1;
    new_number = map->map[span->number];
    if (new_number == 0U) return 0;
    source = pdfw_span_data(map->document, span, &source_size);
    if (span->body_start > source_size || span->body_end > source_size || span->stream_offset > source_size) return -1;
    if (pdfw_append_uint(buffer, new_number) != 0 || pdfw_append_cstr(buffer, " 0 obj\n") != 0) return -1;
    if (span->stream_offset < source_size && span->stream_offset < span->body_end) {
        if (pdfw_write_rewritten_range(buffer, source, span->body_start, span->stream_offset, map, is_page, pages_object_number) != 0) return -1;
        if (pdfw_append_bytes(buffer, source + span->stream_offset, span->body_end - span->stream_offset) != 0) return -1;
    } else {
        if (pdfw_write_rewritten_range(buffer, source, span->body_start, span->body_end, map, is_page, pages_object_number) != 0) return -1;
    }
    return pdfw_append_cstr(buffer, "\nendobj\n");
}

static int pdfw_write_object_header(PdfBuffer *buffer, unsigned int object_number, size_t *offsets) {
    offsets[object_number] = buffer->size;
    if (pdfw_append_uint(buffer, object_number) != 0 || pdfw_append_cstr(buffer, " 0 obj\n") != 0) return -1;
    return 0;
}

static int pdfw_write_new_document(const PdfRewriteMap *maps, size_t map_count, const unsigned int *page_numbers, size_t page_count, const PdfDocumentInfo *metadata, unsigned int total_objects, PdfBuffer *output) {
    size_t *offsets;
    size_t xref_offset;
    size_t index;
    unsigned int pages_object = 2U;
    unsigned int info_object = pdf_document_info_has_fields(metadata) ? 3U : 0U;

    pdf_buffer_init(output);
    offsets = (size_t *)rt_malloc_array((size_t)total_objects + 1U, sizeof(size_t));
    if (offsets == 0) return -1;
    rt_memset(offsets, 0, ((size_t)total_objects + 1U) * sizeof(size_t));
    if (pdfw_append_cstr(output, "%PDF-1.7\n%newos\n") != 0) goto fail;
    if (pdfw_write_object_header(output, 1U, offsets) != 0) goto fail;
    if (pdfw_append_cstr(output, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n") != 0) goto fail;
    if (pdfw_write_object_header(output, 2U, offsets) != 0) goto fail;
    if (pdfw_append_cstr(output, "<< /Type /Pages /Count ") != 0 || pdfw_append_uint(output, (unsigned long long)page_count) != 0 || pdfw_append_cstr(output, " /Kids [") != 0) goto fail;
    for (index = 0U; index < page_count; ++index) {
        if (index != 0U && pdfw_append_char(output, ' ') != 0) goto fail;
        if (pdfw_append_uint(output, page_numbers[index]) != 0 || pdfw_append_cstr(output, " 0 R") != 0) goto fail;
    }
    if (pdfw_append_cstr(output, "] >>\nendobj\n") != 0) goto fail;
    if (info_object != 0U) {
        if (pdfw_write_object_header(output, info_object, offsets) != 0) goto fail;
        if (pdfw_write_info_dict(output, metadata) != 0 || pdfw_append_cstr(output, "endobj\n") != 0) goto fail;
    }
    for (index = 0U; index < map_count; ++index) {
        size_t object_index;
        const PdfDocument *document = maps[index].document;

        for (object_index = 0U; object_index < document->objects_len; ++object_index) {
            const PdfObjectSpan *span = &document->objects[object_index];
            unsigned int new_number = (size_t)span->number < maps[index].map_len ? maps[index].map[span->number] : 0U;

            if (new_number != 0U) {
                offsets[new_number] = output->size;
                if (pdfw_write_copied_object(output, span, &maps[index], pages_object) != 0) goto fail;
            }
        }
    }
    xref_offset = output->size;
    if (pdfw_append_cstr(output, "xref\n0 ") != 0 || pdfw_append_uint(output, (unsigned long long)total_objects + 1ULL) != 0 || pdfw_append_cstr(output, "\n0000000000 65535 f \n") != 0) goto fail;
    for (index = 1U; index <= total_objects; ++index) {
        if (pdfw_append_xref_offset(output, offsets[index]) != 0 || pdfw_append_cstr(output, " 00000 n \n") != 0) goto fail;
    }
    if (pdfw_append_cstr(output, "trailer\n<< /Size ") != 0 || pdfw_append_uint(output, (unsigned long long)total_objects + 1ULL) != 0 || pdfw_append_cstr(output, " /Root 1 0 R") != 0) goto fail;
    if (info_object != 0U && (pdfw_append_cstr(output, " /Info ") != 0 || pdfw_append_uint(output, info_object) != 0 || pdfw_append_cstr(output, " 0 R") != 0)) goto fail;
    if (pdfw_append_cstr(output, " >>\nstartxref\n") != 0 || pdfw_append_uint(output, (unsigned long long)xref_offset) != 0 || pdfw_append_cstr(output, "\n%%EOF\n") != 0) goto fail;
    rt_free(offsets);
    return 0;
fail:
    rt_free(offsets);
    pdf_buffer_free(output);
    return -1;
}

int pdf_write_join(const PdfPageSelection *selections, size_t selection_count, const PdfDocumentInfo *metadata, PdfBuffer *output) {
    PdfRewriteMap *maps;
    unsigned int *page_numbers;
    unsigned int next_object = pdf_document_info_has_fields(metadata) ? 4U : 3U;
    size_t total_pages = 0U;
    size_t selection_index;
    size_t page_index;
    size_t map_count = 0U;
    int status;

    if (selections == 0 || selection_count == 0U || output == 0) return -1;
    for (selection_index = 0U; selection_index < selection_count; ++selection_index) total_pages += selections[selection_index].page_count;
    if (total_pages == 0U) return -1;
    maps = (PdfRewriteMap *)rt_malloc_array(selection_count, sizeof(PdfRewriteMap));
    page_numbers = (unsigned int *)rt_malloc_array(total_pages, sizeof(unsigned int));
    if (maps == 0 || page_numbers == 0) {
        rt_free(maps);
        rt_free(page_numbers);
        return -1;
    }
    rt_memset(maps, 0, selection_count * sizeof(PdfRewriteMap));
    page_index = 0U;
    for (selection_index = 0U; selection_index < selection_count; ++selection_index) {
        const PdfDocument *document = selections[selection_index].document;
        size_t first_page = selections[selection_index].first_page;
        size_t count = selections[selection_index].page_count;
        size_t object_index;

        if (document == 0 || first_page > document->pages_len || count > document->pages_len - first_page) goto fail;
        maps[selection_index].document = document;
        maps[selection_index].map_len = (size_t)document->max_object_number + 1U;
        maps[selection_index].map = (unsigned int *)rt_malloc_array(maps[selection_index].map_len, sizeof(unsigned int));
        if (maps[selection_index].map == 0) goto fail;
        rt_memset(maps[selection_index].map, 0, maps[selection_index].map_len * sizeof(unsigned int));
        map_count += 1U;
        for (object_index = 0U; object_index < document->objects_len; ++object_index) {
            const PdfObjectSpan *span = &document->objects[object_index];
            if (pdfw_should_copy_object(document, span, first_page, count)) maps[selection_index].map[span->number] = next_object++;
        }
        for (object_index = first_page; object_index < first_page + count; ++object_index) page_numbers[page_index++] = maps[selection_index].map[document->pages[object_index].object_number];
    }
    status = pdfw_write_new_document(maps, map_count, page_numbers, total_pages, metadata, next_object - 1U, output);
    for (selection_index = 0U; selection_index < map_count; ++selection_index) rt_free(maps[selection_index].map);
    rt_free(maps);
    rt_free(page_numbers);
    return status;
fail:
    for (selection_index = 0U; selection_index < map_count; ++selection_index) rt_free(maps[selection_index].map);
    rt_free(maps);
    rt_free(page_numbers);
    return -1;
}

int pdf_write_split_part(const PdfDocument *document, size_t first_page, size_t page_count, const PdfDocumentInfo *metadata, PdfBuffer *output) {
    PdfPageSelection selection;

    selection.document = document;
    selection.first_page = first_page;
    selection.page_count = page_count;
    return pdf_write_join(&selection, 1U, metadata, output);
}

int pdf_write_info_update(const PdfDocument *document, const PdfDocumentInfo *metadata, PdfBuffer *output) {
    size_t *offsets;
    size_t xref_offset;
    unsigned int new_info_object;
    unsigned int new_size;

    if (document == 0 || output == 0 || document->catalog_object_number == 0U) return -1;
    pdf_buffer_init(output);
    if (pdfw_append_bytes(output, document->data, document->size) != 0) return -1;
    if (output->size != 0U && output->data[output->size - 1U] != (unsigned char)'\n') {
        if (pdfw_append_char(output, '\n') != 0) goto fail;
    }
    new_info_object = document->max_object_number + 1U;
    new_size = new_info_object + 1U;
    offsets = (size_t *)rt_malloc_array((size_t)new_size, sizeof(size_t));
    if (offsets == 0) goto fail;
    rt_memset(offsets, 0, (size_t)new_size * sizeof(size_t));
    offsets[new_info_object] = output->size;
    if (pdfw_append_uint(output, new_info_object) != 0 || pdfw_append_cstr(output, " 0 obj\n") != 0 || pdfw_write_info_dict(output, metadata) != 0 || pdfw_append_cstr(output, "endobj\n") != 0) goto fail_with_offsets;
    xref_offset = output->size;
    if (pdfw_append_cstr(output, "xref\n") != 0 || pdfw_append_uint(output, new_info_object) != 0 || pdfw_append_cstr(output, " 1\n") != 0 || pdfw_append_xref_offset(output, offsets[new_info_object]) != 0 || pdfw_append_cstr(output, " 00000 n \n") != 0) goto fail_with_offsets;
    if (pdfw_append_cstr(output, "trailer\n<< /Size ") != 0 || pdfw_append_uint(output, new_size) != 0 || pdfw_append_cstr(output, " /Root ") != 0 || pdfw_append_uint(output, document->catalog_object_number) != 0 || pdfw_append_cstr(output, " 0 R /Info ") != 0 || pdfw_append_uint(output, new_info_object) != 0 || pdfw_append_cstr(output, " 0 R") != 0) goto fail_with_offsets;
    if (document->startxref_offset != 0U) {
        if (pdfw_append_cstr(output, " /Prev ") != 0 || pdfw_append_uint(output, (unsigned long long)document->startxref_offset) != 0) goto fail_with_offsets;
    }
    if (pdfw_append_cstr(output, " >>\nstartxref\n") != 0 || pdfw_append_uint(output, (unsigned long long)xref_offset) != 0 || pdfw_append_cstr(output, "\n%%EOF\n") != 0) goto fail_with_offsets;
    rt_free(offsets);
    return 0;
fail_with_offsets:
    rt_free(offsets);
fail:
    pdf_buffer_free(output);
    return -1;
}