#include "pdf.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int title;
    int author;
    int subject;
    int keywords;
    int creator;
    int producer;
    int creation_date;
    int modification_date;
} PdfInfoEditRemoveFlags;

static void print_usage(void) {
    tool_write_usage("pdfinfoedit", "--set FIELD=VALUE [--remove FIELD] -o OUTPUT PDF");
}

#define read_all_input tool_read_all_input

static int write_all_output(const char *path, const unsigned char *data, size_t size) {
    int fd = platform_open_write(path, 0644U);
    size_t written = 0U;

    if (fd < 0) return -1;
    while (written < size) {
        long chunk = platform_write(fd, data + written, size - written);

        if (chunk <= 0) {
            platform_close(fd);
            return -1;
        }
        written += (size_t)chunk;
    }
    return platform_close(fd) == 0 ? 0 : -1;
}

static int split_assignment(char *text, char **field_out, char **value_out) {
    size_t index;

    for (index = 0U; text[index] != '\0'; ++index) {
        if (text[index] == '=') {
            text[index] = '\0';
            *field_out = text;
            *value_out = text + index + 1U;
            return text[0] != '\0' ? 0 : -1;
        }
    }
    return -1;
}

static int mark_remove_field(PdfInfoEditRemoveFlags *flags, const char *field) {
    if (rt_strcmp(field, "title") == 0 || rt_strcmp(field, "Title") == 0) flags->title = 1;
    else if (rt_strcmp(field, "author") == 0 || rt_strcmp(field, "Author") == 0) flags->author = 1;
    else if (rt_strcmp(field, "subject") == 0 || rt_strcmp(field, "Subject") == 0) flags->subject = 1;
    else if (rt_strcmp(field, "keywords") == 0 || rt_strcmp(field, "Keywords") == 0) flags->keywords = 1;
    else if (rt_strcmp(field, "creator") == 0 || rt_strcmp(field, "Creator") == 0) flags->creator = 1;
    else if (rt_strcmp(field, "producer") == 0 || rt_strcmp(field, "Producer") == 0) flags->producer = 1;
    else if (rt_strcmp(field, "creation_date") == 0 || rt_strcmp(field, "CreationDate") == 0 || rt_strcmp(field, "creationdate") == 0) flags->creation_date = 1;
    else if (rt_strcmp(field, "modification_date") == 0 || rt_strcmp(field, "ModDate") == 0 || rt_strcmp(field, "moddate") == 0) flags->modification_date = 1;
    else return -1;
    return 0;
}

static void apply_remove_flags(PdfDocumentInfo *metadata, const PdfInfoEditRemoveFlags *flags) {
    if (flags->title) metadata->title[0] = '\0';
    if (flags->author) metadata->author[0] = '\0';
    if (flags->subject) metadata->subject[0] = '\0';
    if (flags->keywords) metadata->keywords[0] = '\0';
    if (flags->creator) metadata->creator[0] = '\0';
    if (flags->producer) metadata->producer[0] = '\0';
    if (flags->creation_date) metadata->creation_date[0] = '\0';
    if (flags->modification_date) metadata->modification_date[0] = '\0';
}

int main(int argc, char **argv) {
    const char *output_path = 0;
    const char *input_path = 0;
    unsigned char *data = 0;
    size_t size = 0U;
    PdfDocument document;
    PdfDocumentInfo metadata;
    PdfInfoEditRemoveFlags remove_flags;
    PdfBuffer output;
    int changed = 0;
    int index;
    int status = 0;

    rt_memset(&metadata, 0, sizeof(metadata));
    rt_memset(&remove_flags, 0, sizeof(remove_flags));
    pdf_buffer_init(&output);
    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];

        if (rt_strcmp(arg, "-o") == 0 || rt_strcmp(arg, "--output") == 0) {
            if (index + 1 >= argc) {
                print_usage();
                return 2;
            }
            output_path = argv[++index];
        } else if (rt_strcmp(arg, "--set") == 0) {
            char scratch[PDF_TEXT_CAPACITY * 2U];
            char *field;
            char *value;

            if (index + 1 >= argc) {
                print_usage();
                return 2;
            }
            rt_copy_string(scratch, sizeof(scratch), argv[++index]);
            if (split_assignment(scratch, &field, &value) != 0 || pdf_document_info_set_field(&metadata, field, value) != 0) {
                tool_write_error("pdfinfoedit", "invalid metadata assignment: ", argv[index]);
                return 2;
            }
            changed = 1;
        } else if (rt_strcmp(arg, "--remove") == 0) {
            if (index + 1 >= argc) {
                print_usage();
                return 2;
            }
            if (mark_remove_field(&remove_flags, argv[++index]) != 0) {
                tool_write_error("pdfinfoedit", "unknown metadata field: ", argv[index]);
                return 2;
            }
            changed = 1;
        } else if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        } else if (arg[0] == '-' && rt_strcmp(arg, "-") != 0) {
            tool_write_error("pdfinfoedit", "unknown option: ", arg);
            return 2;
        } else {
            input_path = arg;
        }
    }
    if (output_path == 0 || input_path == 0 || !changed) {
        print_usage();
        return 2;
    }
    if (read_all_input(input_path, &data, &size) != 0) {
        tool_write_error("pdfinfoedit", "read failed: ", input_path);
        return 1;
    }
    if (pdf_document_parse(data, size, &document) != 0) {
        tool_write_error("pdfinfoedit", "unsupported or unreadable PDF: ", input_path);
        rt_free(data);
        return 1;
    }
    if (metadata.title[0] == '\0') rt_copy_string(metadata.title, sizeof(metadata.title), document.info.document_info.title);
    if (metadata.author[0] == '\0') rt_copy_string(metadata.author, sizeof(metadata.author), document.info.document_info.author);
    if (metadata.subject[0] == '\0') rt_copy_string(metadata.subject, sizeof(metadata.subject), document.info.document_info.subject);
    if (metadata.keywords[0] == '\0') rt_copy_string(metadata.keywords, sizeof(metadata.keywords), document.info.document_info.keywords);
    if (metadata.creator[0] == '\0') rt_copy_string(metadata.creator, sizeof(metadata.creator), document.info.document_info.creator);
    if (metadata.producer[0] == '\0') rt_copy_string(metadata.producer, sizeof(metadata.producer), document.info.document_info.producer);
    if (metadata.creation_date[0] == '\0') rt_copy_string(metadata.creation_date, sizeof(metadata.creation_date), document.info.document_info.creation_date);
    if (metadata.modification_date[0] == '\0') rt_copy_string(metadata.modification_date, sizeof(metadata.modification_date), document.info.document_info.modification_date);
    apply_remove_flags(&metadata, &remove_flags);
    if (pdf_write_info_update(&document, &metadata, &output) != 0) {
        tool_write_error("pdfinfoedit", "could not update metadata", 0);
        status = 1;
    } else if (write_all_output(output_path, output.data, output.size) != 0) {
        tool_write_error("pdfinfoedit", "write failed: ", output_path);
        status = 1;
    }
    pdf_buffer_free(&output);
    pdf_document_free(&document);
    rt_free(data);
    return status;
}
