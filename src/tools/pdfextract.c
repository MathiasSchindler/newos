#include "pdf.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int want_stream;
    int want_metadata;
    int decode;
    unsigned int object_number;
    unsigned int generation;
    int have_generation;
} PdfExtractOptions;

static void print_usage(void) {
    tool_write_usage("pdfextract", "--stream OBJ[:GEN] [--raw|--decoded] PDF | --metadata PDF");
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
    if ((options.want_stream && options.want_metadata) || (!options.want_stream && !options.want_metadata)) {
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
