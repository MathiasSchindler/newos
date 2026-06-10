#include "pdf.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(void) {
    tool_write_usage("pdfcheck", "PDF ...");
}

static int is_space(unsigned char ch) {
    return ch == 0U || ch == 9U || ch == 10U || ch == 12U || ch == 13U || ch == 32U;
}

static int is_digit(unsigned char ch) {
    return ch >= (unsigned char)'0' && ch <= (unsigned char)'9';
}

static int is_delim(unsigned char ch) {
    return is_space(ch) || ch == (unsigned char)'(' || ch == (unsigned char)')' || ch == (unsigned char)'<' || ch == (unsigned char)'>' || ch == (unsigned char)'[' || ch == (unsigned char)']' || ch == (unsigned char)'{' || ch == (unsigned char)'}' || ch == (unsigned char)'/' || ch == (unsigned char)'%';
}

static size_t skip_ws(const unsigned char *data, size_t size, size_t offset) {
    while (offset < size) {
        if (is_space(data[offset])) offset += 1U;
        else if (data[offset] == (unsigned char)'%') {
            while (offset < size && data[offset] != (unsigned char)'\n' && data[offset] != (unsigned char)'\r') offset += 1U;
        } else break;
    }
    return offset;
}

static int parse_u64(const unsigned char *data, size_t size, size_t *offset_io, unsigned long long *value_out) {
    unsigned long long value = 0ULL;
    size_t offset = *offset_io;
    int saw = 0;

    while (offset < size && is_digit(data[offset])) {
        unsigned int digit = (unsigned int)(data[offset] - (unsigned char)'0');
        if (value > (18446744073709551615ULL - (unsigned long long)digit) / 10ULL) return -1;
        value = value * 10ULL + (unsigned long long)digit;
        offset += 1U;
        saw = 1;
    }
    if (!saw) return -1;
    *offset_io = offset;
    *value_out = value;
    return 0;
}

static int text_at(const unsigned char *data, size_t size, size_t offset, const char *text) {
    size_t length = rt_strlen(text);
    size_t index;

    if (offset > size || length > size - offset) return 0;
    for (index = 0U; index < length; ++index) {
        if (data[offset + index] != (unsigned char)text[index]) return 0;
    }
    return 1;
}

static int key_at(const unsigned char *data, size_t size, size_t offset, const char *key) {
    size_t length = rt_strlen(key);

    if (!text_at(data, size, offset, key)) return 0;
    return offset + length >= size || is_delim(data[offset + length]);
}

static int object_exists(const PdfInfo *info, unsigned long long number, unsigned long long generation) {
    size_t index;

    if (number == 0ULL || number > 4294967295ULL || generation > 4294967295ULL) return 0;
    for (index = 0U; index < info->objects_len; ++index) {
        if (info->objects[index].number == (unsigned int)number && info->objects[index].generation == (unsigned int)generation) return 1;
    }
    return 0;
}

static int find_key_ref(const unsigned char *data, size_t size, const char *key, unsigned long long *number_out, unsigned long long *generation_out) {
    size_t offset;
    size_t key_length = rt_strlen(key);

    for (offset = 0U; offset + key_length < size; ++offset) {
        size_t parse_offset;
        unsigned long long number;
        unsigned long long generation;

        if (data[offset] != (unsigned char)'/' || !key_at(data, size, offset, key)) continue;
        parse_offset = skip_ws(data, size, offset + key_length);
        if (parse_u64(data, size, &parse_offset, &number) != 0) continue;
        parse_offset = skip_ws(data, size, parse_offset);
        if (parse_u64(data, size, &parse_offset, &generation) != 0) continue;
        parse_offset = skip_ws(data, size, parse_offset);
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

static int scan_dangling_refs(const unsigned char *data, size_t size, const PdfInfo *info) {
    size_t offset = 0U;
    int errors = 0;

    while (offset < size) {
        size_t parse_offset;
        unsigned long long number;
        unsigned long long generation;

        if ((offset != 0U && !is_delim(data[offset - 1U])) || !is_digit(data[offset])) {
            offset += 1U;
            continue;
        }
        parse_offset = offset;
        if (parse_u64(data, size, &parse_offset, &number) != 0) {
            offset += 1U;
            continue;
        }
        parse_offset = skip_ws(data, size, parse_offset);
        if (parse_u64(data, size, &parse_offset, &generation) != 0) {
            offset += 1U;
            continue;
        }
        parse_offset = skip_ws(data, size, parse_offset);
        if (parse_offset < size && data[parse_offset] == (unsigned char)'R') {
            if (!object_exists(info, number, generation)) {
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
    if (scan_dangling_refs(data, size, &document.info)) status = 1;
    if (document.info.object_count == 0ULL) {
        write_error_line("no indirect objects found");
        status = 1;
    }
    if (status == 0) write_ok("structure checks passed");
    pdf_document_free(&document);
    rt_free(data);
    return status;
}

int main(int argc, char **argv) {
    int index;
    int status = 0;

    if (argc < 2) {
        print_usage();
        return 1;
    }
    for (index = 1; index < argc; ++index) {
        if (rt_strcmp(argv[index], "-h") == 0 || rt_strcmp(argv[index], "--help") == 0) {
            print_usage();
            return 0;
        }
        if (argv[index][0] == '-' && rt_strcmp(argv[index], "-") != 0) {
            tool_write_error("pdfcheck", "unknown option: ", argv[index]);
            print_usage();
            return 1;
        }
        if (check_path(argv[index]) != 0) status = 1;
    }
    return status;
}
