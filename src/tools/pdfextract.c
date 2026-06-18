#include "pdf.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int want_stream;
    int want_metadata;
    int want_list_streams;
    int decode;
    unsigned int object_number;
    unsigned int generation;
    int have_generation;
} PdfExtractOptions;

static void print_usage(void) {
    tool_write_usage("pdfextract", "--stream OBJ[:GEN] [--raw|--decoded] PDF | --metadata PDF | --list-streams PDF");
}

static int px_find_top_u64_direct_value(const unsigned char *data, size_t size, const char *key, unsigned long long *value_out) {
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

static void write_filter_value(const unsigned char *dict, size_t dict_size) {
    size_t key_offset;
    size_t offset;
    char name[PDF_NAME_CAPACITY];
    int first = 1;

    if (!pdf_find_top_key(dict, dict_size, "/Filter", &key_offset)) {
        rt_write_cstr(1, "none");
        return;
    }
    offset = pdf_skip_ws(dict, dict_size, key_offset + 7U);
    if (offset >= dict_size) {
        rt_write_cstr(1, "unknown");
    } else if (dict[offset] == (unsigned char)'/') {
        pdf_copy_name(dict, dict_size, offset + 1U, name, sizeof(name));
        rt_write_cstr(1, name[0] != '\0' ? name : "unknown");
    } else if (dict[offset] == (unsigned char)'[') {
        rt_write_char(1, '[');
        offset += 1U;
        while (offset < dict_size) {
            offset = pdf_skip_ws(dict, dict_size, offset);
            if (offset >= dict_size || dict[offset] == (unsigned char)']') break;
            if (dict[offset] == (unsigned char)'/') {
                pdf_copy_name(dict, dict_size, offset + 1U, name, sizeof(name));
                if (!first) rt_write_char(1, ',');
                rt_write_cstr(1, name[0] != '\0' ? name : "unknown");
                first = 0;
                offset += 1U;
                while (offset < dict_size && !pdf_is_delim(dict[offset])) offset += 1U;
            } else break;
        }
        rt_write_char(1, ']');
    } else {
        rt_write_cstr(1, "unknown");
    }
}

static int parse_object_spec(const char *text, unsigned int *number_out, unsigned int *generation_out, int *have_generation_out) {
    unsigned long long number = 0ULL;
    unsigned long long generation = 0ULL;
    size_t offset = 0U;

    if (text == 0 || text[0] < '0' || text[0] > '9') return -1;
    while (text[offset] >= '0' && text[offset] <= '9') {
        unsigned int digit = (unsigned int)(text[offset] - '0');
        if (number > (4294967295ULL - (unsigned long long)digit) / 10ULL) return -1;
        number = number * 10ULL + (unsigned long long)digit;
        offset += 1U;
    }
    *have_generation_out = 0;
    if (text[offset] == ':') {
        offset += 1U;
        if (text[offset] < '0' || text[offset] > '9') return -1;
        while (text[offset] >= '0' && text[offset] <= '9') {
            unsigned int digit = (unsigned int)(text[offset] - '0');
            if (generation > (4294967295ULL - (unsigned long long)digit) / 10ULL) return -1;
            generation = generation * 10ULL + (unsigned long long)digit;
            offset += 1U;
        }
        *have_generation_out = 1;
    }
    if (text[offset] != '\0' || number == 0ULL) return -1;
    *number_out = (unsigned int)number;
    *generation_out = (unsigned int)generation;
    return 0;
}

static const PdfObjectSpan *find_object(const PdfDocument *document, const PdfExtractOptions *options) {
    size_t index;

    for (index = 0U; index < document->objects_len; ++index) {
        const PdfObjectSpan *object = &document->objects[index];

        if (object->number == options->object_number && (!options->have_generation || object->generation == options->generation)) return object;
    }
    return 0;
}

static void write_field(const char *label, const char *value) {
    if (value == 0 || value[0] == '\0') return;
    rt_write_cstr(1, label);
    rt_write_cstr(1, ": ");
    rt_write_line(1, value);
}

static int write_metadata(const PdfDocumentInfo *info) {
    if (!pdf_document_info_has_fields(info)) {
        rt_write_line(2, "pdfextract: no document-info metadata found");
        return 1;
    }
    write_field("title", info->title);
    write_field("author", info->author);
    write_field("subject", info->subject);
    write_field("keywords", info->keywords);
    write_field("creator", info->creator);
    write_field("producer", info->producer);
    write_field("creation_date", info->creation_date);
    write_field("modification_date", info->modification_date);
    return 0;
}

static int extract_stream(const PdfDocument *document, const PdfExtractOptions *options) {
    const PdfObjectSpan *object = find_object(document, options);
    PdfBuffer output;
    int result;

    if (object == 0) {
        rt_write_cstr(2, "pdfextract: object not found: ");
        rt_write_uint(2, options->object_number);
        rt_write_char(2, '\n');
        return 1;
    }
    if (object->stream_offset >= document->size) {
        rt_write_cstr(2, "pdfextract: object has no stream: ");
        rt_write_uint(2, options->object_number);
        rt_write_char(2, '\n');
        return 1;
    }
    pdf_buffer_init(&output);
    result = pdf_object_stream_data(document, object, options->decode, &output);
    if (result != 0) {
        pdf_buffer_free(&output);
        if (result == -2) tool_write_error("pdfextract", "unsupported stream filter for object: ", "");
        else tool_write_error("pdfextract", "could not extract stream for object: ", "");
        rt_write_uint(2, options->object_number);
        rt_write_char(2, '\n');
        return 1;
    }
    result = rt_write_all(1, output.data, output.size) == 0 ? 0 : 1;
    pdf_buffer_free(&output);
    return result;
}

static int stream_raw_size(const PdfDocument *document, const PdfObjectSpan *object, size_t *size_out) {
    const unsigned char *dict;
    size_t dict_size;
    size_t content_start;
    size_t content_end;
    unsigned long long stream_length;

    if (object->stream_offset >= document->size || object->endstream_offset > document->size || object->body_start > object->stream_offset) return -1;
    dict = document->data + object->body_start;
    dict_size = object->stream_offset - object->body_start;
    content_start = pdf_stream_body_start(document->data, document->size, object->stream_offset);
    if (content_start > object->endstream_offset) return -1;
    content_end = object->endstream_offset;
    if (px_find_top_u64_direct_value(dict, dict_size, "/Length", &stream_length) && stream_length <= (unsigned long long)(document->size - content_start)) {
        content_end = content_start + (size_t)stream_length;
    } else {
        pdf_trim_stream_end(document->data, content_start, &content_end);
    }
    if (content_end < content_start || content_end > document->size) return -1;
    *size_out = content_end - content_start;
    return 0;
}

static void write_object_kind(const char *label, const char *value) {
    rt_write_char(1, ' ');
    rt_write_cstr(1, label);
    rt_write_char(1, '=');
    rt_write_cstr(1, value != 0 && value[0] != '\0' ? value : "-");
}

static int list_streams(const PdfDocument *document) {
    size_t index;

    rt_write_line(1, "object generation raw raw_size decoded decoded_size filter type subtype");
    for (index = 0U; index < document->objects_len; ++index) {
        const PdfObjectSpan *object = &document->objects[index];
        const unsigned char *dict;
        size_t dict_size;
        size_t raw_size = 0U;
        int have_raw_size;
        int filter_kind;

        if (object->stream_offset >= document->size) continue;
        dict = document->data + object->body_start;
        dict_size = object->stream_offset > object->body_start ? object->stream_offset - object->body_start : 0U;
        have_raw_size = stream_raw_size(document, object, &raw_size) == 0;
        filter_kind = pdf_stream_filter_kind(dict, dict_size);
        rt_write_uint(1, object->number);
        rt_write_char(1, ' ');
        rt_write_uint(1, object->generation);
        rt_write_cstr(1, " raw=yes raw_size=");
        if (have_raw_size) rt_write_uint(1, raw_size);
        else rt_write_cstr(1, "unknown");
        if (filter_kind == 0) {
            rt_write_cstr(1, " decoded=yes decoded_size=");
            if (have_raw_size) rt_write_uint(1, raw_size);
            else rt_write_cstr(1, "unknown");
        } else if (filter_kind == 1) {
            rt_write_cstr(1, " decoded=yes decoded_size=unknown");
        } else {
            rt_write_cstr(1, " decoded=no decoded_size=-");
        }
        rt_write_cstr(1, " filter=");
        write_filter_value(dict, dict_size);
        write_object_kind("type", object->type);
        write_object_kind("subtype", object->subtype);
        rt_write_char(1, '\n');
    }
    return 0;
}

static int process_path(const char *path, const PdfExtractOptions *options) {
    unsigned char *data;
    size_t size;
    PdfDocument document;
    int status;

    if (tool_read_all_input(path, &data, &size) != 0) return 1;
    if (pdf_document_scan(data, size, &document) != 0) {
        tool_write_error("pdfextract", "not a readable PDF: ", path ? path : "stdin");
        rt_free(data);
        return 1;
    }
    if (options->want_metadata) status = write_metadata(&document.info.document_info);
    else if (options->want_list_streams) status = list_streams(&document);
    else status = extract_stream(&document, options);
    pdf_document_free(&document);
    rt_free(data);
    return status;
}

int main(int argc, char **argv) {
    PdfExtractOptions options;
    int index;
    const char *path = 0;

    rt_memset(&options, 0, sizeof(options));
    options.decode = 1;
    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];

        if (rt_strcmp(arg, "--") == 0) {
            index += 1;
            break;
        }
        if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        }
        if (rt_strcmp(arg, "--raw") == 0) {
            options.decode = 0;
        } else if (rt_strcmp(arg, "--decoded") == 0) {
            options.decode = 1;
        } else if (rt_strcmp(arg, "--metadata") == 0) {
            options.want_metadata = 1;
        } else if (rt_strcmp(arg, "--list-streams") == 0) {
            options.want_list_streams = 1;
        } else if (rt_strcmp(arg, "--stream") == 0) {
            if (index + 1 >= argc || parse_object_spec(argv[index + 1], &options.object_number, &options.generation, &options.have_generation) != 0) {
                tool_write_error("pdfextract", "invalid object number: ", index + 1 < argc ? argv[index + 1] : "");
                print_usage();
                return 1;
            }
            options.want_stream = 1;
            index += 1;
        } else if (arg[0] == '-' && rt_strcmp(arg, "-") != 0) {
            tool_write_error("pdfextract", "unknown option: ", arg);
            print_usage();
            return 1;
        } else {
            break;
        }
    }
    if (options.want_stream + options.want_metadata + options.want_list_streams != 1) {
        tool_write_error("pdfextract", "choose exactly one extraction mode", "");
        print_usage();
        return 1;
    }
    if (index < argc) {
        path = argv[index++];
        if (index < argc) {
            tool_write_error("pdfextract", "too many input files", "");
            print_usage();
            return 1;
        }
    }
    return process_path(path, &options);
}
