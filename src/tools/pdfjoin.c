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

static void print_usage(void) {
    tool_write_usage("pdfjoin", "-o OUTPUT PDF...");
}

#define read_all_input tool_read_all_input

static int write_all_output(const char *path, const unsigned char *data, size_t size) {
    int fd = platform_open_write(path, 0644U);
    size_t written = 0U;

    if (fd < 0) return -1;
    while (written < size) {
        long chunk = platform_write(fd, data + written, size - written);

        if (chunk < 0) {
            platform_close(fd);
            return -1;
        }
        if (chunk == 0) {
            platform_close(fd);
            return -1;
        }
        written += (size_t)chunk;
    }
    return platform_close(fd) == 0 ? 0 : -1;
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
    int status = 0;

    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];

        if (rt_strcmp(arg, "-o") == 0 || rt_strcmp(arg, "--output") == 0) {
            if (index + 1 >= argc) {
                print_usage();
                return 2;
            }
            output_path = argv[++index];
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
    if (output_path == 0 || first_input >= argc || argc - first_input < 2) {
        print_usage();
        return 2;
    }
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
        if (read_all_input(input->path, &input->data, &input->size) != 0) {
            tool_write_error("pdfjoin", "read failed: ", input->path);
            status = 1;
            break;
        }
        if (pdf_document_parse(input->data, input->size, &input->document) != 0) {
            tool_write_error("pdfjoin", "unsupported or unreadable PDF: ", input->path);
            status = 1;
            break;
        }
        if (index == 0) metadata = input->document.info.document_info;
        selections[index].document = &input->document;
        selections[index].first_page = 0U;
        selections[index].page_count = input->document.pages_len;
    }
    if (status == 0 && pdf_write_join(selections, (size_t)input_count, &metadata, &output) != 0) {
        tool_write_error("pdfjoin", "could not build output", 0);
        status = 1;
    }
    if (status == 0 && write_all_output(output_path, output.data, output.size) != 0) {
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
