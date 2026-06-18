#ifndef NEWOS_PDF_H
#define NEWOS_PDF_H

#include <stddef.h>

#define PDF_NAME_CAPACITY 64U
#define PDF_TEXT_CAPACITY 256U

typedef struct {
    char name[PDF_NAME_CAPACITY];
    unsigned long long count;
} PdfNameCount;

typedef struct {
    unsigned int number;
    unsigned int generation;
    size_t offset;
    int has_stream;
    char type[PDF_NAME_CAPACITY];
    char subtype[PDF_NAME_CAPACITY];
} PdfObjectInfo;

typedef struct {
    unsigned int object_number;
    unsigned int generation;
    int has_media_box;
    long long media_box[4];
    int has_crop_box;
    long long crop_box[4];
    int has_rotate;
    long long rotate;
} PdfPageInfo;

typedef struct {
    unsigned int object_number;
    unsigned int generation;
    char subtype[PDF_NAME_CAPACITY];
    char base_font[PDF_NAME_CAPACITY];
    char encoding[PDF_NAME_CAPACITY];
    int has_to_unicode;
    int embedded_program_in_object;
} PdfFontInfo;

typedef struct {
    unsigned int object_number;
    unsigned int generation;
    char title[PDF_TEXT_CAPACITY];
    char author[PDF_TEXT_CAPACITY];
    char subject[PDF_TEXT_CAPACITY];
    char keywords[PDF_TEXT_CAPACITY];
    char creator[PDF_TEXT_CAPACITY];
    char producer[PDF_TEXT_CAPACITY];
    char creation_date[PDF_TEXT_CAPACITY];
    char modification_date[PDF_TEXT_CAPACITY];
} PdfDocumentInfo;

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} PdfBuffer;

typedef struct {
    unsigned int number;
    unsigned int generation;
    const unsigned char *data;
    size_t data_size;
    unsigned char *owned_data;
    size_t object_offset;
    size_t body_start;
    size_t body_end;
    size_t stream_offset;
    size_t endstream_offset;
    char type[PDF_NAME_CAPACITY];
    char subtype[PDF_NAME_CAPACITY];
} PdfObjectSpan;

typedef struct {
    unsigned int object_number;
    unsigned int generation;
} PdfPageRef;

typedef struct {
    int has_header;
    int has_eof;
    unsigned int major_version;
    unsigned int minor_version;
    unsigned long long object_count;
    unsigned long long stream_count;
    unsigned long long filtered_stream_count;
    unsigned long long xref_table_count;
    unsigned long long xref_stream_count;
    unsigned long long trailer_count;
    unsigned long long info_dictionary_count;
    unsigned long long object_stream_count;
    unsigned long long catalog_count;
    unsigned long long pages_tree_count;
    unsigned long long page_count;
    unsigned long long font_count;
    unsigned long long embedded_font_program_count;
    unsigned long long image_count;
    unsigned long long form_xobject_count;
    unsigned long long annotation_count;
    unsigned long long metadata_count;
    unsigned long long encrypted;
    unsigned long long text_object_count;
    unsigned long long text_show_count;
    unsigned long long path_operator_count;
    unsigned long long xobject_paint_count;
    unsigned long long inline_image_count;
    PdfDocumentInfo document_info;
    PdfObjectInfo *objects;
    size_t objects_len;
    size_t objects_cap;
    PdfPageInfo *pages;
    size_t pages_len;
    size_t pages_cap;
    PdfFontInfo *fonts;
    size_t fonts_len;
    size_t fonts_cap;
    PdfNameCount *filters;
    size_t filters_len;
    size_t filters_cap;
    PdfNameCount *encodings;
    size_t encodings_len;
    size_t encodings_cap;
    PdfNameCount *font_names;
    size_t font_names_len;
    size_t font_names_cap;
} PdfInfo;

typedef struct {
    const unsigned char *data;
    size_t size;
    PdfInfo info;
    PdfObjectSpan *objects;
    size_t objects_len;
    size_t objects_cap;
    PdfPageRef *pages;
    size_t pages_len;
    size_t pages_cap;
    unsigned int catalog_object_number;
    unsigned int catalog_generation;
    unsigned int max_object_number;
    size_t startxref_offset;
} PdfDocument;

typedef struct {
    const PdfDocument *document;
    size_t first_page;
    size_t page_count;
} PdfPageSelection;

#define PDF_DOCUMENT_PARSE_UNREADABLE (-1)
#define PDF_DOCUMENT_PARSE_ENCRYPTED (-2)
#define PDF_DOCUMENT_PARSE_OBJECT_STREAM_UNSUPPORTED (-3)

void pdf_info_init(PdfInfo *info);
void pdf_info_free(PdfInfo *info);
int pdf_analyze(const unsigned char *data, size_t size, PdfInfo *info);
int pdf_is_space(unsigned char ch);
int pdf_is_digit(unsigned char ch);
int pdf_is_delim(unsigned char ch);
size_t pdf_skip_ws(const unsigned char *data, size_t size, size_t offset);
int pdf_text_at(const unsigned char *data, size_t size, size_t offset, const char *text);
int pdf_text_at_len(const unsigned char *data, size_t size, size_t offset, const char *text, size_t length);
int pdf_keyword_at(const unsigned char *data, size_t size, size_t offset, const char *text);
int pdf_parse_u64(const unsigned char *data, size_t size, size_t *offset_io, unsigned long long *value_out);
size_t pdf_stream_body_start(const unsigned char *data, size_t size, size_t stream_offset);
void pdf_trim_stream_end(const unsigned char *data, size_t start, size_t *end_io);
long long pdf_abs_fixed(long long value);
const char *pdf_page_format_name(long long width, long long height);
int pdf_format_date(const char *pdf_date, char *buffer, size_t buffer_size);
void pdf_buffer_init(PdfBuffer *buffer);
void pdf_buffer_free(PdfBuffer *buffer);
void pdf_document_init(PdfDocument *document);
void pdf_document_free(PdfDocument *document);
int pdf_document_scan(const unsigned char *data, size_t size, PdfDocument *document);
int pdf_document_parse(const unsigned char *data, size_t size, PdfDocument *document);
int pdf_object_stream_data(const PdfDocument *document, const PdfObjectSpan *object, int decode, PdfBuffer *output);
int pdf_document_info_has_fields(const PdfDocumentInfo *info);
int pdf_document_info_set_field(PdfDocumentInfo *info, const char *field, const char *value);
int pdf_document_info_remove_field(PdfDocumentInfo *info, const char *field);
int pdf_write_join(const PdfPageSelection *selections, size_t selection_count, const PdfDocumentInfo *metadata, PdfBuffer *output);
int pdf_write_split_part(const PdfDocument *document, size_t first_page, size_t page_count, const PdfDocumentInfo *metadata, PdfBuffer *output);
int pdf_write_info_update(const PdfDocument *document, const PdfDocumentInfo *metadata, PdfBuffer *output);

#endif