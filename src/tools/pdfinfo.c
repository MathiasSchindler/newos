#include "pdf.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PDFINFO_INITIAL_CAPACITY 65536U

typedef struct {
    int plain;
    int details;
    int list_pages;
    int list_objects;
    int list_fonts;
    int json;
} PdfInfoOptions;

static void print_usage(void) {
    tool_write_usage("pdfinfo", "[-p|--plain] [-d|--details] [--pages] [--objects] [--fonts] [--json] [file ...]");
}

static int read_all_input(const char *path, unsigned char **data_out, size_t *size_out) {
    int fd;
    int should_close;
    unsigned char *buffer;
    size_t capacity = PDFINFO_INITIAL_CAPACITY;
    size_t used = 0U;

    *data_out = 0;
    *size_out = 0U;
    if (tool_open_input(path, &fd, &should_close) != 0) return -1;
    buffer = (unsigned char *)rt_malloc(capacity);
    if (buffer == 0) {
        tool_close_input(fd, should_close);
        tool_write_error("pdfinfo", "out of memory: ", path ? path : "stdin");
        return -1;
    }
    while (1) {
        long bytes_read;

        if (used == capacity) {
            size_t next_capacity = capacity * 2U;
            unsigned char *next;

            if (next_capacity <= capacity) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                tool_write_error("pdfinfo", "input too large: ", path ? path : "stdin");
                return -1;
            }
            next = (unsigned char *)rt_realloc(buffer, next_capacity);
            if (next == 0) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                tool_write_error("pdfinfo", "out of memory: ", path ? path : "stdin");
                return -1;
            }
            buffer = next;
            capacity = next_capacity;
        }
        bytes_read = platform_read(fd, buffer + used, capacity - used);
        if (bytes_read < 0) {
            rt_free(buffer);
            tool_close_input(fd, should_close);
            tool_write_error("pdfinfo", "read failed: ", path ? path : "stdin");
            return -1;
        }
        if (bytes_read == 0) break;
        used += (size_t)bytes_read;
    }
    tool_close_input(fd, should_close);
    *data_out = buffer;
    *size_out = used;
    return 0;
}

static long long fixed_abs(long long value) {
    return value < 0LL ? -value : value;
}

static void write_fixed(long long value) {
    long long absolute = fixed_abs(value);
    unsigned long long whole = (unsigned long long)(absolute / 1000LL);
    unsigned long long fraction = (unsigned long long)(absolute % 1000LL);

    if (value < 0LL) rt_write_char(1, '-');
    rt_write_uint(1, whole);
    if (fraction != 0ULL) {
        rt_write_char(1, '.');
        rt_write_char(1, (char)('0' + (fraction / 100ULL) % 10ULL));
        rt_write_char(1, (char)('0' + (fraction / 10ULL) % 10ULL));
        rt_write_char(1, (char)('0' + fraction % 10ULL));
    }
}

static long long page_width(const PdfPageInfo *page) {
    return fixed_abs(page->media_box[2] - page->media_box[0]);
}

static long long page_height(const PdfPageInfo *page) {
    return fixed_abs(page->media_box[3] - page->media_box[1]);
}

static void write_bool_word(unsigned long long value) {
    rt_write_cstr(1, value ? "yes" : "no");
}

static void write_metric(const char *label, unsigned long long value) {
    rt_write_cstr(1, label);
    rt_write_cstr(1, ": ");
    rt_write_uint(1, value);
    rt_write_char(1, '\n');
}

static void write_text_field(const char *label, const char *value) {
    if (value == 0 || value[0] == '\0') return;
    rt_write_cstr(1, label);
    rt_write_cstr(1, ": ");
    rt_write_line(1, value);
}

static void write_date_field(const char *label, const char *value) {
    char formatted[PDF_TEXT_CAPACITY];

    if (value == 0 || value[0] == '\0') return;
    rt_write_cstr(1, label);
    rt_write_cstr(1, ": ");
    if (pdf_format_date(value, formatted, sizeof(formatted))) {
        rt_write_cstr(1, formatted);
        rt_write_cstr(1, " (raw ");
        rt_write_cstr(1, value);
        rt_write_cstr(1, ")\n");
    } else {
        rt_write_line(1, value);
    }
}

static void write_name_counts(const char *label, const PdfNameCount *items, size_t length) {
    size_t index;

    rt_write_cstr(1, label);
    rt_write_cstr(1, ": ");
    if (length == 0U) {
        rt_write_line(1, "-");
        return;
    }
    for (index = 0U; index < length; ++index) {
        if (index != 0U) rt_write_cstr(1, ", ");
        rt_write_cstr(1, items[index].name);
        rt_write_cstr(1, "(");
        rt_write_uint(1, items[index].count);
        rt_write_char(1, ')');
    }
    rt_write_char(1, '\n');
}

static void write_summary(const char *path, size_t size, const PdfInfo *info) {
    rt_write_cstr(1, "file: ");
    rt_write_line(1, path ? path : "stdin");
    rt_write_cstr(1, "pdf_version: ");
    if (info->has_header) {
        rt_write_uint(1, info->major_version);
        rt_write_char(1, '.');
        rt_write_uint(1, info->minor_version);
        rt_write_char(1, '\n');
    } else {
        rt_write_line(1, "unknown");
    }
    write_metric("bytes", (unsigned long long)size);
    write_text_field("title", info->document_info.title);
    write_text_field("author", info->document_info.author);
    write_text_field("subject", info->document_info.subject);
    write_text_field("keywords", info->document_info.keywords);
    write_text_field("creator", info->document_info.creator);
    write_text_field("producer", info->document_info.producer);
    write_date_field("creation_date", info->document_info.creation_date);
    write_date_field("modification_date", info->document_info.modification_date);
    write_metric("objects", info->object_count);
    write_metric("streams", info->stream_count);
    write_metric("pages", info->page_count);
    write_metric("fonts", info->font_count);
    write_metric("images", info->image_count);
    rt_write_cstr(1, "encrypted: ");
    write_bool_word(info->encrypted);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "has_eof: ");
    write_bool_word((unsigned long long)info->has_eof);
    rt_write_char(1, '\n');
    write_name_counts("filters", info->filters, info->filters_len);
    write_name_counts("encodings", info->encodings, info->encodings_len);
}

static void write_plain(const char *path, size_t size, const PdfInfo *info) {
    rt_write_cstr(1, path ? path : "stdin");
    rt_write_cstr(1, " version=");
    if (info->has_header) {
        rt_write_uint(1, info->major_version);
        rt_write_char(1, '.');
        rt_write_uint(1, info->minor_version);
    } else {
        rt_write_cstr(1, "unknown");
    }
    rt_write_cstr(1, " bytes=");
    rt_write_uint(1, (unsigned long long)size);
    rt_write_cstr(1, " pages=");
    rt_write_uint(1, info->page_count);
    rt_write_cstr(1, " objects=");
    rt_write_uint(1, info->object_count);
    rt_write_cstr(1, " streams=");
    rt_write_uint(1, info->stream_count);
    rt_write_cstr(1, " fonts=");
    rt_write_uint(1, info->font_count);
    rt_write_cstr(1, " images=");
    rt_write_uint(1, info->image_count);
    rt_write_char(1, '\n');
}

static void write_details(const PdfInfo *info) {
    write_metric("catalogs", info->catalog_count);
    write_metric("info_dictionaries", info->info_dictionary_count);
    write_metric("page_trees", info->pages_tree_count);
    write_metric("xref_tables", info->xref_table_count);
    write_metric("xref_streams", info->xref_stream_count);
    write_metric("trailers", info->trailer_count);
    write_metric("object_streams", info->object_stream_count);
    write_metric("filtered_streams", info->filtered_stream_count);
    write_metric("form_xobjects", info->form_xobject_count);
    write_metric("annotations", info->annotation_count);
    write_metric("metadata_objects", info->metadata_count);
    write_metric("embedded_font_programs", info->embedded_font_program_count);
    write_metric("text_objects", info->text_object_count);
    write_metric("text_show_ops", info->text_show_count);
    write_metric("path_ops", info->path_operator_count);
    write_metric("xobject_paint_ops", info->xobject_paint_count);
    write_metric("inline_images", info->inline_image_count);
    write_name_counts("font_names", info->font_names, info->font_names_len);
}

static void write_pages(const PdfInfo *info) {
    size_t index;

    rt_write_line(1, "pages:");
    for (index = 0U; index < info->pages_len; ++index) {
        const PdfPageInfo *page = &info->pages[index];

        rt_write_cstr(1, "  ");
        rt_write_uint(1, (unsigned long long)(index + 1U));
        rt_write_cstr(1, ": obj ");
        rt_write_uint(1, page->object_number);
        rt_write_cstr(1, " ");
        rt_write_uint(1, page->generation);
        if (page->has_media_box) {
            long long width = page_width(page);
            long long height = page_height(page);

            rt_write_cstr(1, " media=");
            write_fixed(width);
            rt_write_char(1, 'x');
            write_fixed(height);
            rt_write_cstr(1, "pt format=");
            rt_write_cstr(1, pdf_page_format_name(width, height));
        }
        if (page->has_rotate) {
            rt_write_cstr(1, " rotate=");
            write_fixed(page->rotate);
        }
        rt_write_char(1, '\n');
    }
}

static void write_fonts(const PdfInfo *info) {
    size_t index;

    rt_write_line(1, "fonts:");
    for (index = 0U; index < info->fonts_len; ++index) {
        const PdfFontInfo *font = &info->fonts[index];

        rt_write_cstr(1, "  obj ");
        rt_write_uint(1, font->object_number);
        rt_write_cstr(1, " ");
        rt_write_uint(1, font->generation);
        rt_write_cstr(1, " subtype=");
        rt_write_cstr(1, font->subtype[0] != '\0' ? font->subtype : "-");
        rt_write_cstr(1, " base=");
        rt_write_cstr(1, font->base_font[0] != '\0' ? font->base_font : "-");
        rt_write_cstr(1, " encoding=");
        rt_write_cstr(1, font->encoding[0] != '\0' ? font->encoding : "-");
        rt_write_cstr(1, " tounicode=");
        rt_write_cstr(1, font->has_to_unicode ? "yes" : "no");
        rt_write_cstr(1, " embedded=");
        rt_write_line(1, font->embedded_program_in_object ? "yes" : "no");
    }
}

static void write_objects(const PdfInfo *info) {
    size_t index;

    rt_write_line(1, "objects:");
    for (index = 0U; index < info->objects_len; ++index) {
        const PdfObjectInfo *object = &info->objects[index];

        rt_write_cstr(1, "  obj ");
        rt_write_uint(1, object->number);
        rt_write_cstr(1, " ");
        rt_write_uint(1, object->generation);
        rt_write_cstr(1, " offset=");
        rt_write_uint(1, (unsigned long long)object->offset);
        rt_write_cstr(1, " type=");
        rt_write_cstr(1, object->type[0] != '\0' ? object->type : "-");
        rt_write_cstr(1, " subtype=");
        rt_write_cstr(1, object->subtype[0] != '\0' ? object->subtype : "-");
        rt_write_cstr(1, " stream=");
        rt_write_line(1, object->has_stream ? "yes" : "no");
    }
}

static void json_key(const char *key) {
    tool_json_write_string(1, key);
    rt_write_char(1, ':');
}

static void json_uint_field(const char *key, unsigned long long value, int *first) {
    if (!*first) rt_write_char(1, ',');
    *first = 0;
    json_key(key);
    rt_write_uint(1, value);
}

static void json_bool_field(const char *key, int value, int *first) {
    if (!*first) rt_write_char(1, ',');
    *first = 0;
    json_key(key);
    rt_write_cstr(1, value ? "true" : "false");
}

static void json_string_field(const char *key, const char *value, int *first) {
    if (!*first) rt_write_char(1, ',');
    *first = 0;
    json_key(key);
    tool_json_write_string(1, value);
}

static void write_json_name_counts(const char *key, const PdfNameCount *items, size_t length, int *first) {
    size_t index;

    if (!*first) rt_write_char(1, ',');
    *first = 0;
    json_key(key);
    rt_write_char(1, '[');
    for (index = 0U; index < length; ++index) {
        if (index != 0U) rt_write_char(1, ',');
        rt_write_cstr(1, "{\"name\":");
        tool_json_write_string(1, items[index].name);
        rt_write_cstr(1, ",\"count\":");
        rt_write_uint(1, items[index].count);
        rt_write_char(1, '}');
    }
    rt_write_char(1, ']');
}

static void write_json_output(const char *path, size_t size, const PdfInfo *info) {
    int first = 1;
    char creation_date_formatted[PDF_TEXT_CAPACITY];
    char modification_date_formatted[PDF_TEXT_CAPACITY];

    creation_date_formatted[0] = '\0';
    modification_date_formatted[0] = '\0';
    (void)pdf_format_date(info->document_info.creation_date, creation_date_formatted, sizeof(creation_date_formatted));
    (void)pdf_format_date(info->document_info.modification_date, modification_date_formatted, sizeof(modification_date_formatted));

    rt_write_char(1, '{');
    json_string_field("file", path ? path : "stdin", &first);
    json_uint_field("bytes", (unsigned long long)size, &first);
    json_bool_field("has_header", info->has_header, &first);
    json_bool_field("has_eof", info->has_eof, &first);
    json_uint_field("major_version", info->major_version, &first);
    json_uint_field("minor_version", info->minor_version, &first);
    json_string_field("title", info->document_info.title, &first);
    json_string_field("author", info->document_info.author, &first);
    json_string_field("subject", info->document_info.subject, &first);
    json_string_field("keywords", info->document_info.keywords, &first);
    json_string_field("creator", info->document_info.creator, &first);
    json_string_field("producer", info->document_info.producer, &first);
    json_string_field("creation_date", info->document_info.creation_date, &first);
    json_string_field("creation_date_formatted", creation_date_formatted, &first);
    json_string_field("modification_date", info->document_info.modification_date, &first);
    json_string_field("modification_date_formatted", modification_date_formatted, &first);
    json_uint_field("info_dictionaries", info->info_dictionary_count, &first);
    json_uint_field("objects", info->object_count, &first);
    json_uint_field("streams", info->stream_count, &first);
    json_uint_field("filtered_streams", info->filtered_stream_count, &first);
    json_uint_field("pages", info->page_count, &first);
    json_uint_field("fonts", info->font_count, &first);
    json_uint_field("images", info->image_count, &first);
    json_uint_field("forms", info->form_xobject_count, &first);
    json_uint_field("annotations", info->annotation_count, &first);
    json_uint_field("metadata", info->metadata_count, &first);
    json_bool_field("encrypted", info->encrypted != 0ULL, &first);
    write_json_name_counts("filters", info->filters, info->filters_len, &first);
    write_json_name_counts("encodings", info->encodings, info->encodings_len, &first);
    write_json_name_counts("font_names", info->font_names, info->font_names_len, &first);
    rt_write_cstr(1, "}\n");
}

static int process_path(const char *path, const PdfInfoOptions *options, int multiple) {
    unsigned char *data;
    size_t size;
    PdfInfo info;

    if (read_all_input(path, &data, &size) != 0) return 1;
    if (pdf_analyze(data, size, &info) != 0) {
        tool_write_error("pdfinfo", "not a readable PDF: ", path ? path : "stdin");
        rt_free(data);
        return 1;
    }
    if (options->json) {
        write_json_output(path, size, &info);
    } else if (options->plain) {
        write_plain(path, size, &info);
    } else {
        if (multiple) {
            rt_write_cstr(1, "==> ");
            rt_write_cstr(1, path ? path : "stdin");
            rt_write_cstr(1, " <==\n");
        }
        write_summary(path, size, &info);
        if (options->details) write_details(&info);
        if (options->list_pages || options->details) write_pages(&info);
        if (options->list_fonts || options->details) write_fonts(&info);
        if (options->list_objects) write_objects(&info);
    }
    pdf_info_free(&info);
    rt_free(data);
    return 0;
}

int main(int argc, char **argv) {
    PdfInfoOptions options;
    int first_path = 1;
    int index;
    int path_count = 0;
    int status = 0;

    rt_memset(&options, 0, sizeof(options));
    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];

        if (rt_strcmp(arg, "--") == 0) {
            first_path = index + 1;
            break;
        }
        if (arg[0] != '-' || rt_strcmp(arg, "-") == 0) break;
        if (rt_strcmp(arg, "-p") == 0 || rt_strcmp(arg, "--plain") == 0) options.plain = 1;
        else if (rt_strcmp(arg, "-d") == 0 || rt_strcmp(arg, "--details") == 0) options.details = 1;
        else if (rt_strcmp(arg, "--pages") == 0) options.list_pages = 1;
        else if (rt_strcmp(arg, "--objects") == 0) options.list_objects = 1;
        else if (rt_strcmp(arg, "--fonts") == 0) options.list_fonts = 1;
        else if (rt_strcmp(arg, "--json") == 0) options.json = 1;
        else if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        } else {
            tool_write_error("pdfinfo", "unknown option: ", arg);
            print_usage();
            return 1;
        }
        first_path = index + 1;
    }
    tool_json_set_enabled(options.json);
    for (index = first_path; index < argc; ++index) path_count += 1;
    if (path_count == 0) return process_path(0, &options, 0);
    for (index = first_path; index < argc; ++index) {
        if (process_path(argv[index], &options, path_count > 1) != 0) status = 1;
    }
    return status;
}