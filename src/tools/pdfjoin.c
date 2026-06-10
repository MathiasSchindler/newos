#include "pdf.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    const char *path;
    unsigned char *data;
    size_t size;
    PdfDocument document;
} PdfJoinInput;

typedef struct {
    int title;
    int author;
    int subject;
    int keywords;
    int creator;
    int producer;
    char title_value[PDF_TEXT_CAPACITY];
    char author_value[PDF_TEXT_CAPACITY];
    char subject_value[PDF_TEXT_CAPACITY];
    char keywords_value[PDF_TEXT_CAPACITY];
    char creator_value[PDF_TEXT_CAPACITY];
    char producer_value[PDF_TEXT_CAPACITY];
} PdfJoinMetadataOverrides;

static void print_usage(void) {
    tool_write_usage("pdfjoin", "-o OUTPUT [--no-metadata] [--title TEXT] [--author TEXT] [--subject TEXT] [--keywords TEXT] [--creator TEXT] [--producer TEXT] PDF...");
}

static int require_option_value(int argc, char **argv, int *index_io) {
    if (*index_io + 1 >= argc) {
        tool_write_error("pdfjoin", "option requires an argument: ", argv[*index_io]);
        print_usage();
        return -1;
    }
    *index_io += 1;
    return 0;
}

static int set_metadata_override(PdfJoinMetadataOverrides *overrides, const char *option, const char *value) {
    if (rt_strcmp(option, "--title") == 0) {
        rt_copy_string(overrides->title_value, sizeof(overrides->title_value), value);
        overrides->title = 1;
    } else if (rt_strcmp(option, "--author") == 0) {
        rt_copy_string(overrides->author_value, sizeof(overrides->author_value), value);
        overrides->author = 1;
    } else if (rt_strcmp(option, "--subject") == 0) {
        rt_copy_string(overrides->subject_value, sizeof(overrides->subject_value), value);
        overrides->subject = 1;
    } else if (rt_strcmp(option, "--keywords") == 0) {
        rt_copy_string(overrides->keywords_value, sizeof(overrides->keywords_value), value);
        overrides->keywords = 1;
    } else if (rt_strcmp(option, "--creator") == 0) {
        rt_copy_string(overrides->creator_value, sizeof(overrides->creator_value), value);
        overrides->creator = 1;
    } else if (rt_strcmp(option, "--producer") == 0) {
        rt_copy_string(overrides->producer_value, sizeof(overrides->producer_value), value);
        overrides->producer = 1;
    } else {
        return -1;
    }
    return 0;
}

static int is_metadata_option(const char *option) {
    return rt_strcmp(option, "--title") == 0 || rt_strcmp(option, "--author") == 0 || rt_strcmp(option, "--subject") == 0 || rt_strcmp(option, "--keywords") == 0 || rt_strcmp(option, "--creator") == 0 || rt_strcmp(option, "--producer") == 0;
}

static int has_metadata_overrides(const PdfJoinMetadataOverrides *overrides) {
    return overrides->title || overrides->author || overrides->subject || overrides->keywords || overrides->creator || overrides->producer;
}

static void apply_metadata_overrides(PdfDocumentInfo *metadata, const PdfJoinMetadataOverrides *overrides) {
    if (overrides->title) rt_copy_string(metadata->title, sizeof(metadata->title), overrides->title_value);
    if (overrides->author) rt_copy_string(metadata->author, sizeof(metadata->author), overrides->author_value);
    if (overrides->subject) rt_copy_string(metadata->subject, sizeof(metadata->subject), overrides->subject_value);
    if (overrides->keywords) rt_copy_string(metadata->keywords, sizeof(metadata->keywords), overrides->keywords_value);
    if (overrides->creator) rt_copy_string(metadata->creator, sizeof(metadata->creator), overrides->creator_value);
    if (overrides->producer) rt_copy_string(metadata->producer, sizeof(metadata->producer), overrides->producer_value);
}

static void write_parse_error(const char *path, int parse_status) {
    if (parse_status == PDF_DOCUMENT_PARSE_ENCRYPTED) {
        tool_write_error("pdfjoin", "encrypted PDF is not supported: ", path);
    } else if (parse_status == PDF_DOCUMENT_PARSE_OBJECT_STREAM_UNSUPPORTED) {
        tool_write_error("pdfjoin", "unsupported compressed object stream in PDF: ", path);
    } else {
        tool_write_error("pdfjoin", "unsupported or unreadable PDF: ", path);
    }
}

int main(int argc, char **argv) {
    const char *output_path = 0;
    int first_input = 1;
    int input_count;
    int index;
    PdfJoinInput *inputs;
    PdfPageSelection *selections;
    PdfBuffer output;
    PdfDocumentInfo metadata;
    PdfJoinMetadataOverrides overrides;
    PdfDocumentInfo *metadata_to_write = &metadata;
    int no_metadata = 0;
    int status = 0;

    rt_memset(&overrides, 0, sizeof(overrides));
    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];

        if (rt_strcmp(arg, "-o") == 0 || rt_strcmp(arg, "--output") == 0) {
            if (require_option_value(argc, argv, &index) != 0) return 2;
            output_path = argv[index];
            first_input = index + 1;
        } else if (rt_strcmp(arg, "--no-metadata") == 0) {
            no_metadata = 1;
            first_input = index + 1;
        } else if (is_metadata_option(arg)) {
            if (require_option_value(argc, argv, &index) != 0) return 2;
            if (set_metadata_override(&overrides, arg, argv[index]) != 0) return 2;
            first_input = index + 1;
        } else if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        } else if (arg[0] == '-' && rt_strcmp(arg, "-") != 0) {
            tool_write_error("pdfjoin", "unknown option: ", arg);
            print_usage();
            return 2;
        } else {
            first_input = index;
            break;
        }
    }
    if (no_metadata && has_metadata_overrides(&overrides)) {
        tool_write_error("pdfjoin", "--no-metadata cannot be combined with metadata overrides", 0);
        print_usage();
        return 2;
    }
    if (output_path == 0 || first_input >= argc || argc - first_input < 2) {
        print_usage();
        return 2;
    }
    if (no_metadata) metadata_to_write = 0;
    input_count = argc - first_input;
    inputs = (PdfJoinInput *)rt_malloc_array((size_t)input_count, sizeof(PdfJoinInput));
    selections = (PdfPageSelection *)rt_malloc_array((size_t)input_count, sizeof(PdfPageSelection));
    if (inputs == 0 || selections == 0) {
        rt_free(inputs);
        rt_free(selections);
        tool_write_error("pdfjoin", "out of memory", 0);
        return 1;
    }
    rt_memset(inputs, 0, (size_t)input_count * sizeof(PdfJoinInput));
    rt_memset(selections, 0, (size_t)input_count * sizeof(PdfPageSelection));
    rt_memset(&metadata, 0, sizeof(metadata));
    pdf_buffer_init(&output);
    for (index = 0; index < input_count; ++index) {
        PdfJoinInput *input = &inputs[index];

        input->path = argv[first_input + index];
        if (tool_read_all_input(input->path, &input->data, &input->size) != 0) {
            tool_write_error("pdfjoin", "read failed: ", input->path);
            status = 1;
            break;
        }
        {
            int parse_status = pdf_document_parse(input->data, input->size, &input->document);
            if (parse_status != 0) {
                write_parse_error(input->path, parse_status);
                status = 1;
                break;
            }
        }
        if (index == 0) {
            metadata = input->document.info.document_info;
            apply_metadata_overrides(&metadata, &overrides);
        }
        selections[index].document = &input->document;
        selections[index].first_page = 0U;
        selections[index].page_count = input->document.pages_len;
    }
    if (status == 0 && pdf_write_join(selections, (size_t)input_count, metadata_to_write, &output) != 0) {
        tool_write_error("pdfjoin", "could not build output", 0);
        status = 1;
    }
    if (status == 0 && tool_write_file_all(output_path, output.data, output.size) != 0) {
        tool_write_error("pdfjoin", "write failed: ", output_path);
        status = 1;
    }
    pdf_buffer_free(&output);
    for (index = 0; index < input_count; ++index) {
        pdf_document_free(&inputs[index].document);
        rt_free(inputs[index].data);
    }
    rt_free(inputs);
    rt_free(selections);
    return status;
}
