#include "pdf.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(void) {
    tool_write_usage("pdfcheck", "[--json] PDF ...");
}

static int key_at(const unsigned char *data, size_t size, size_t offset, const char *key) {
    size_t length = rt_strlen(key);

    if (!pdf_text_at(data, size, offset, key)) return 0;
    return offset + length >= size || pdf_is_delim(data[offset + length]);
}

static int object_exists(const PdfInfo *info, unsigned long long number, unsigned long long generation) {
    size_t index;

    if (number == 0ULL || number > 4294967295ULL || generation > 4294967295ULL) return 0;
    for (index = 0U; index < info->objects_len; ++index) {
        if (info->objects[index].number == (unsigned int)number && info->objects[index].generation == (unsigned int)generation) return 1;
    }
    return 0;
}

static int is_null_reference(unsigned long long number, unsigned long long generation) {
    return number == 0ULL && generation == 0ULL;
}

static int find_key_ref(const unsigned char *data, size_t size, const char *key, unsigned long long *number_out, unsigned long long *generation_out) {
    size_t offset;
    size_t key_length = rt_strlen(key);

    for (offset = 0U; offset + key_length < size; ++offset) {
        size_t parse_offset;
        unsigned long long number;
        unsigned long long generation;

        if (data[offset] != (unsigned char)'/' || !key_at(data, size, offset, key)) continue;
        parse_offset = pdf_skip_ws(data, size, offset + key_length);
        if (pdf_parse_u64(data, size, &parse_offset, &number) != 0) continue;
        parse_offset = pdf_skip_ws(data, size, parse_offset);
        if (pdf_parse_u64(data, size, &parse_offset, &generation) != 0) continue;
        parse_offset = pdf_skip_ws(data, size, parse_offset);
        if (parse_offset < size && data[parse_offset] == (unsigned char)'R') {
            *number_out = number;
            *generation_out = generation;
            return 1;
        }
    }
    return 0;
}

static void write_ok(const char *message) {
    rt_write_cstr(1, "ok: ");
    rt_write_line(1, message);
}

static void write_notice(const char *message) {
    rt_write_cstr(1, "notice: ");
    rt_write_line(1, message);
}

static void write_error_line(const char *message) {
    rt_write_cstr(1, "error: ");
    rt_write_line(1, message);
}

static void write_count_line(const char *label, unsigned long long value) {
    rt_write_cstr(1, label);
    rt_write_cstr(1, ": ");
    rt_write_uint(1, value);
    rt_write_char(1, '\n');
}

typedef struct {
    char text[160];
} CheckMessage;

typedef struct {
    CheckMessage *items;
    size_t len;
    size_t cap;
} CheckMessageList;

static void message_list_init(CheckMessageList *list) {
    list->items = 0;
    list->len = 0U;
    list->cap = 0U;
}

static void message_list_free(CheckMessageList *list) {
    rt_free(list->items);
    list->items = 0;
    list->len = 0U;
    list->cap = 0U;
}

static int message_list_add(CheckMessageList *list, const char *message) {
    CheckMessage *next;
    size_t next_cap;

    if (list->len == list->cap) {
        next_cap = list->cap == 0U ? 8U : list->cap * 2U;
        if (next_cap <= list->cap) return -1;
        next = (CheckMessage *)rt_realloc_array(list->items, next_cap, sizeof(CheckMessage));
        if (next == 0) return -1;
        list->items = next;
        list->cap = next_cap;
    }
    rt_copy_string(list->items[list->len].text, sizeof(list->items[list->len].text), message);
    list->len += 1U;
    return 0;
}

static int message_list_add_ref(CheckMessageList *list, const char *prefix, unsigned long long number, unsigned long long generation, const char *suffix) {
    char buffer[160];
    char number_text[32];
    char generation_text[32];
    size_t used = 0U;
    size_t index;

    rt_unsigned_to_string(number, number_text, sizeof(number_text));
    rt_unsigned_to_string(generation, generation_text, sizeof(generation_text));
    buffer[0] = '\0';
#define APPEND_TEXT(text_value) do { \
        const char *append_text_value = (text_value); \
        for (index = 0U; append_text_value[index] != '\0' && used + 1U < sizeof(buffer); ++index) buffer[used++] = append_text_value[index]; \
        buffer[used] = '\0'; \
    } while (0)
    APPEND_TEXT(prefix);
    APPEND_TEXT(number_text);
    APPEND_TEXT(" ");
    APPEND_TEXT(generation_text);
    APPEND_TEXT(suffix);
#undef APPEND_TEXT
    return message_list_add(list, buffer);
}

typedef struct {
    const char *path;
    int ok;
    int has_header;
    int has_eof;
    unsigned int major_version;
    unsigned int minor_version;
    unsigned long long object_count;
    unsigned long long stream_count;
    unsigned long long page_count;
    unsigned long long xref_stream_count;
    unsigned long long object_stream_count;
    unsigned long long encrypted;
    int have_root;
    int root_exists;
    unsigned long long root_number;
    unsigned long long root_generation;
    int have_info;
    int info_exists;
    unsigned long long info_number;
    unsigned long long info_generation;
    CheckMessageList notices;
    CheckMessageList errors;
} CheckReport;

static void check_report_init(CheckReport *report, const char *path) {
    rt_memset(report, 0, sizeof(*report));
    report->path = path;
    report->ok = 1;
    message_list_init(&report->notices);
    message_list_init(&report->errors);
}

static void check_report_free(CheckReport *report) {
    message_list_free(&report->notices);
    message_list_free(&report->errors);
}

static void check_report_error(CheckReport *report, const char *message) {
    report->ok = 0;
    (void)message_list_add(&report->errors, message);
}

static void check_report_notice(CheckReport *report, const char *message) {
    (void)message_list_add(&report->notices, message);
}

static void json_write_string(const char *text) {
    static const char hex[] = "0123456789abcdef";
    size_t index;

    rt_write_char(1, '"');
    for (index = 0U; text != 0 && text[index] != '\0'; ++index) {
        unsigned char ch = (unsigned char)text[index];

        if (ch == (unsigned char)'"' || ch == (unsigned char)'\\') {
            rt_write_char(1, '\\');
            rt_write_char(1, (char)ch);
        } else if (ch == (unsigned char)'\n') rt_write_cstr(1, "\\n");
        else if (ch == (unsigned char)'\r') rt_write_cstr(1, "\\r");
        else if (ch == (unsigned char)'\t') rt_write_cstr(1, "\\t");
        else if (ch < 32U) {
            rt_write_cstr(1, "\\u00");
            rt_write_char(1, hex[(ch >> 4) & 0xfU]);
            rt_write_char(1, hex[ch & 0xfU]);
        } else {
            rt_write_char(1, (char)ch);
        }
    }
    rt_write_char(1, '"');
}

static void json_write_bool(int value) {
    rt_write_cstr(1, value ? "true" : "false");
}

static void json_write_message_array(const CheckMessageList *list) {
    size_t index;

    rt_write_char(1, '[');
    for (index = 0U; index < list->len; ++index) {
        if (index != 0U) rt_write_char(1, ',');
        json_write_string(list->items[index].text);
    }
    rt_write_char(1, ']');
}

static void json_write_ref(const char *name, int present, int exists, unsigned long long number, unsigned long long generation) {
    rt_write_char(1, '"');
    rt_write_cstr(1, name);
    rt_write_cstr(1, "\":{");
    rt_write_cstr(1, "\"present\":");
    json_write_bool(present);
    if (present) {
        rt_write_cstr(1, ",\"number\":");
        rt_write_uint(1, number);
        rt_write_cstr(1, ",\"generation\":");
        rt_write_uint(1, generation);
        rt_write_cstr(1, ",\"exists\":");
        json_write_bool(exists);
    }
    rt_write_char(1, '}');
}

static void json_write_report(const CheckReport *report) {
    rt_write_cstr(1, "{");
    rt_write_cstr(1, "\"file\":");
    json_write_string(report->path ? report->path : "stdin");
    rt_write_cstr(1, ",\"ok\":");
    json_write_bool(report->ok);
    rt_write_cstr(1, ",\"header\":{\"present\":");
    json_write_bool(report->has_header);
    if (report->has_header) {
        rt_write_cstr(1, ",\"major\":");
        rt_write_uint(1, report->major_version);
        rt_write_cstr(1, ",\"minor\":");
        rt_write_uint(1, report->minor_version);
    }
    rt_write_cstr(1, "},\"eof\":");
    json_write_bool(report->has_eof);
    rt_write_cstr(1, ",\"counters\":{\"objects\":");
    rt_write_uint(1, report->object_count);
    rt_write_cstr(1, ",\"streams\":");
    rt_write_uint(1, report->stream_count);
    rt_write_cstr(1, ",\"pages\":");
    rt_write_uint(1, report->page_count);
    rt_write_cstr(1, ",\"xref_streams\":");
    rt_write_uint(1, report->xref_stream_count);
    rt_write_cstr(1, ",\"object_streams\":");
    rt_write_uint(1, report->object_stream_count);
    rt_write_cstr(1, ",\"encrypted\":");
    rt_write_uint(1, report->encrypted);
    rt_write_cstr(1, "},");
    json_write_ref("root", report->have_root, report->root_exists, report->root_number, report->root_generation);
    rt_write_char(1, ',');
    json_write_ref("info", report->have_info, report->info_exists, report->info_number, report->info_generation);
    rt_write_cstr(1, ",\"notices\":");
    json_write_message_array(&report->notices);
    rt_write_cstr(1, ",\"errors\":");
    json_write_message_array(&report->errors);
    rt_write_cstr(1, "}");
}

static int report_ref(const char *label, const PdfInfo *info, unsigned long long number, unsigned long long generation, int required) {
    rt_write_cstr(1, label);
    rt_write_cstr(1, ": ");
    rt_write_uint(1, number);
    rt_write_char(1, ' ');
    rt_write_uint(1, generation);
    rt_write_cstr(1, " R");
    if (!object_exists(info, number, generation)) {
        rt_write_cstr(1, " (dangling)\n");
        return 1;
    }
    rt_write_cstr(1, " (ok)\n");
    (void)required;
    return 0;
}

static size_t stream_body_start(const unsigned char *data, size_t size, size_t stream_offset) {
    size_t offset = stream_offset + 6U;

    if (offset < size && data[offset] == (unsigned char)'\r') {
        offset += 1U;
        if (offset < size && data[offset] == (unsigned char)'\n') offset += 1U;
    } else if (offset < size && data[offset] == (unsigned char)'\n') {
        offset += 1U;
    }
    return offset;
}

static int offset_is_stream_data(const PdfDocument *document, size_t offset, size_t *end_out) {
    size_t index;

    for (index = 0U; index < document->objects_len; ++index) {
        const PdfObjectSpan *object = &document->objects[index];
        size_t content_start;

        if (object->stream_offset >= document->size || object->endstream_offset > document->size) continue;
        content_start = stream_body_start(document->data, document->size, object->stream_offset);
        if (content_start <= object->endstream_offset && offset >= content_start && offset < object->endstream_offset) {
            if (end_out != 0) *end_out = object->endstream_offset;
            return 1;
        }
    }
    return 0;
}

static int scan_dangling_refs(const unsigned char *data, size_t size, const PdfDocument *document) {
    size_t offset = 0U;
    int errors = 0;

    while (offset < size) {
        size_t parse_offset;
        unsigned long long number;
        unsigned long long generation;
        size_t stream_end;

        if (offset_is_stream_data(document, offset, &stream_end)) {
            offset = stream_end > offset ? stream_end : offset + 1U;
            continue;
        }
        if ((offset != 0U && !pdf_is_delim(data[offset - 1U])) || !pdf_is_digit(data[offset])) {
            offset += 1U;
            continue;
        }
        parse_offset = offset;
        if (pdf_parse_u64(data, size, &parse_offset, &number) != 0) {
            offset += 1U;
            continue;
        }
        parse_offset = pdf_skip_ws(data, size, parse_offset);
        if (pdf_parse_u64(data, size, &parse_offset, &generation) != 0) {
            offset += 1U;
            continue;
        }
        parse_offset = pdf_skip_ws(data, size, parse_offset);
        if (parse_offset < size && data[parse_offset] == (unsigned char)'R') {
            if (!is_null_reference(number, generation) && !object_exists(&document->info, number, generation)) {
                rt_write_cstr(1, "error: dangling ref ");
                rt_write_uint(1, number);
                rt_write_char(1, ' ');
                rt_write_uint(1, generation);
                rt_write_cstr(1, " R\n");
                errors = 1;
            }
            offset = parse_offset + 1U;
        } else {
            offset += 1U;
        }
    }
    return errors;
}

static int scan_dangling_refs_report(const unsigned char *data, size_t size, const PdfDocument *document, CheckReport *report) {
    size_t offset = 0U;
    int errors = 0;

    while (offset < size) {
        size_t parse_offset;
        unsigned long long number;
        unsigned long long generation;
        size_t stream_end;

        if (offset_is_stream_data(document, offset, &stream_end)) {
            offset = stream_end > offset ? stream_end : offset + 1U;
            continue;
        }
        if ((offset != 0U && !pdf_is_delim(data[offset - 1U])) || !pdf_is_digit(data[offset])) {
            offset += 1U;
            continue;
        }
        parse_offset = offset;
        if (pdf_parse_u64(data, size, &parse_offset, &number) != 0) {
            offset += 1U;
            continue;
        }
        parse_offset = pdf_skip_ws(data, size, parse_offset);
        if (pdf_parse_u64(data, size, &parse_offset, &generation) != 0) {
            offset += 1U;
            continue;
        }
        parse_offset = pdf_skip_ws(data, size, parse_offset);
        if (parse_offset < size && data[parse_offset] == (unsigned char)'R') {
            if (!is_null_reference(number, generation) && !object_exists(&document->info, number, generation)) {
                report->ok = 0;
                (void)message_list_add_ref(&report->errors, "dangling ref ", number, generation, " R");
                errors = 1;
            }
            offset = parse_offset + 1U;
        } else {
            offset += 1U;
        }
    }
    return errors;
}

static int check_path(const char *path) {
    unsigned char *data;
    size_t size;
    PdfDocument document;
    unsigned long long root_number = 0ULL;
    unsigned long long root_generation = 0ULL;
    unsigned long long info_number = 0ULL;
    unsigned long long info_generation = 0ULL;
    int have_root;
    int have_info;
    int status = 0;

    if (tool_read_all_input(path, &data, &size) != 0) return 1;
    rt_write_cstr(1, "file: ");
    rt_write_line(1, path ? path : "stdin");
    if (pdf_document_scan(data, size, &document) != 0) {
        write_error_line("not a readable PDF");
        rt_free(data);
        return 1;
    }
    if (document.info.has_header) {
        rt_write_cstr(1, "ok: header PDF-");
        rt_write_uint(1, document.info.major_version);
        rt_write_char(1, '.');
        rt_write_uint(1, document.info.minor_version);
        rt_write_char(1, '\n');
    } else {
        write_error_line("missing PDF header");
        status = 1;
    }
    if (document.info.has_eof) write_ok("EOF marker");
    else {
        write_error_line("missing %%EOF marker");
        status = 1;
    }
    write_count_line("objects", document.info.object_count);
    write_count_line("streams", document.info.stream_count);
    write_count_line("pages", document.info.page_count);
    have_root = find_key_ref(data, size, "/Root", &root_number, &root_generation);
    have_info = find_key_ref(data, size, "/Info", &info_number, &info_generation);
    if (have_root) {
        if (report_ref("root", &document.info, root_number, root_generation, 1)) status = 1;
    } else if (document.info.catalog_count != 0ULL) {
        write_notice("no trailer /Root reference found; catalog object is visible");
    } else {
        write_error_line("missing root catalog reference");
        status = 1;
    }
    if (have_info) {
        if (report_ref("info", &document.info, info_number, info_generation, 0)) status = 1;
    } else {
        write_notice("no document-info reference");
    }
    if (document.info.xref_stream_count != 0ULL) write_count_line("notice: xref_streams", document.info.xref_stream_count);
    if (document.info.object_stream_count != 0ULL) write_count_line("notice: object_streams", document.info.object_stream_count);
    if (document.info.encrypted != 0ULL) {
        write_error_line("encrypted PDFs are unsupported");
        status = 1;
    }
    if (scan_dangling_refs(data, size, &document)) status = 1;
    if (document.info.object_count == 0ULL) {
        write_error_line("no indirect objects found");
        status = 1;
    }
    if (status == 0) write_ok("structure checks passed");
    pdf_document_free(&document);
    rt_free(data);
    return status;
}

static int check_path_json(const char *path, int first) {
    unsigned char *data;
    size_t size;
    PdfDocument document;
    CheckReport report;
    int status;

    check_report_init(&report, path);
    if (!first) rt_write_cstr(1, ",\n");
    if (tool_read_all_input(path, &data, &size) != 0) {
        check_report_error(&report, "could not read input");
        json_write_report(&report);
        check_report_free(&report);
        return 1;
    }
    if (pdf_document_scan(data, size, &document) != 0) {
        check_report_error(&report, "not a readable PDF");
        json_write_report(&report);
        check_report_free(&report);
        rt_free(data);
        return 1;
    }
    report.has_header = document.info.has_header;
    report.has_eof = document.info.has_eof;
    report.major_version = document.info.major_version;
    report.minor_version = document.info.minor_version;
    report.object_count = document.info.object_count;
    report.stream_count = document.info.stream_count;
    report.page_count = document.info.page_count;
    report.xref_stream_count = document.info.xref_stream_count;
    report.object_stream_count = document.info.object_stream_count;
    report.encrypted = document.info.encrypted;
    if (!report.has_header) check_report_error(&report, "missing PDF header");
    if (!report.has_eof) check_report_error(&report, "missing %%EOF marker");
    report.have_root = find_key_ref(data, size, "/Root", &report.root_number, &report.root_generation);
    report.have_info = find_key_ref(data, size, "/Info", &report.info_number, &report.info_generation);
    if (report.have_root) {
        report.root_exists = object_exists(&document.info, report.root_number, report.root_generation);
        if (!report.root_exists) check_report_error(&report, "root reference is dangling");
    } else if (document.info.catalog_count != 0ULL) {
        check_report_notice(&report, "no trailer /Root reference found; catalog object is visible");
    } else {
        check_report_error(&report, "missing root catalog reference");
    }
    if (report.have_info) {
        report.info_exists = object_exists(&document.info, report.info_number, report.info_generation);
        if (!report.info_exists) check_report_error(&report, "info reference is dangling");
    } else {
        check_report_notice(&report, "no document-info reference");
    }
    if (document.info.xref_stream_count != 0ULL) check_report_notice(&report, "xref streams present");
    if (document.info.object_stream_count != 0ULL) check_report_notice(&report, "object streams present");
    if (document.info.encrypted != 0ULL) check_report_error(&report, "encrypted PDFs are unsupported");
    (void)scan_dangling_refs_report(data, size, &document, &report);
    if (document.info.object_count == 0ULL) check_report_error(&report, "no indirect objects found");
    json_write_report(&report);
    status = report.ok ? 0 : 1;
    check_report_free(&report);
    pdf_document_free(&document);
    rt_free(data);
    return status;
}

int main(int argc, char **argv) {
    int index;
    int status = 0;
    int json = 0;
    int path_count = 0;
    int emitted = 0;

    if (argc < 2) {
        print_usage();
        return 1;
    }
    for (index = 1; index < argc; ++index) {
        if (rt_strcmp(argv[index], "-h") == 0 || rt_strcmp(argv[index], "--help") == 0) {
            print_usage();
            return 0;
        }
        if (rt_strcmp(argv[index], "--json") == 0) {
            json = 1;
            continue;
        }
        if (argv[index][0] == '-' && rt_strcmp(argv[index], "-") != 0) {
            tool_write_error("pdfcheck", "unknown option: ", argv[index]);
            print_usage();
            return 1;
        }
        path_count += 1;
    }
    if (path_count == 0) {
        print_usage();
        return 1;
    }
    if (json) {
        rt_write_cstr(1, "[\n");
        for (index = 1; index < argc; ++index) {
            if (rt_strcmp(argv[index], "--json") == 0) continue;
            if (check_path_json(argv[index], emitted == 0) != 0) status = 1;
            emitted += 1;
        }
        rt_write_cstr(1, "\n]\n");
    } else {
        for (index = 1; index < argc; ++index) {
            if (rt_strcmp(argv[index], "--json") == 0) continue;
            if (check_path(argv[index]) != 0) status = 1;
        }
    }
    return status;
}
