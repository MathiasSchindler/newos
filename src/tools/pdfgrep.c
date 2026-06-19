#include "pdf.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    const char *pattern;
    int ignore_case;
    int list_files;
    int quiet;
    int object_number;
    int have_context;
    unsigned int context_chars;
    int multiple;
    const char *path;
    int matched;
} PdfGrepContext;

static void print_usage(void) {
    tool_write_usage("pdfgrep", "[-i] [-l] [-q] [-n] [-C NUM] PATTERN [PDF ...]");
}

static char lower_ascii(char ch) {
    return ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
}

static int find_text(const unsigned char *text, size_t text_size, const char *pattern, int ignore_case, size_t *offset_out) {
    size_t pattern_size = rt_strlen(pattern);
    size_t offset;
    size_t index;

    if (pattern_size == 0U) {
        *offset_out = 0U;
        return 1;
    }
    if (pattern_size > text_size) return 0;
    for (offset = 0U; offset + pattern_size <= text_size; ++offset) {
        int same = 1;

        for (index = 0U; index < pattern_size; ++index) {
            char left = (char)text[offset + index];
            char right = pattern[index];

            if (ignore_case) {
                left = lower_ascii(left);
                right = lower_ascii(right);
            }
            if (left != right) {
                same = 0;
                break;
            }
        }
        if (same) {
            *offset_out = offset;
            return 1;
        }
    }
    return 0;
}

static int parse_context_count(const char *text, unsigned int *value_out) {
    unsigned long long value;

    if (text == 0 || rt_parse_uint(text, &value) != 0 || value > 4294967295ULL) return -1;
    *value_out = (unsigned int)value;
    return 0;
}

static int starts_with(const char *text, const char *prefix) {
    size_t length = rt_strlen(prefix);

    return rt_strncmp(text, prefix, length) == 0;
}

static int append_pending_byte(PdfBuffer *pending, unsigned int value) {
    if (pending->size + 1U < pending->size) return -1;
    if (pending->size + 1U > pending->capacity && tool_byte_buffer_reserve(pending, pending->size + 1U) != 0) return -1;
    pending->data[pending->size++] = (unsigned char)(value & 0xffU);
    return 0;
}

static size_t append_literal(const unsigned char *data, size_t size, size_t offset, PdfBuffer *pending) {
    int depth = 1;

    offset += 1U;
    while (offset < size && depth > 0) {
        unsigned char ch = data[offset++];

        if (ch == (unsigned char)'\\') {
            unsigned int octal = 0U;
            int octal_digits = 0;

            if (offset >= size) break;
            ch = data[offset++];
            if (ch >= (unsigned char)'0' && ch <= (unsigned char)'7') {
                octal = (unsigned int)(ch - (unsigned char)'0');
                octal_digits = 1;
                while (offset < size && octal_digits < 3 && data[offset] >= (unsigned char)'0' && data[offset] <= (unsigned char)'7') {
                    octal = octal * 8U + (unsigned int)(data[offset] - (unsigned char)'0');
                    offset += 1U;
                    octal_digits += 1;
                }
                ch = (unsigned char)(octal & 0xffU);
            } else if (ch == (unsigned char)'n') ch = (unsigned char)'\n';
            else if (ch == (unsigned char)'r') ch = (unsigned char)'\r';
            else if (ch == (unsigned char)'t') ch = (unsigned char)'\t';
            else if (ch == (unsigned char)'b') ch = 8U;
            else if (ch == (unsigned char)'f') ch = 12U;
            else if (ch == (unsigned char)'\r') {
                if (offset < size && data[offset] == (unsigned char)'\n') offset += 1U;
                continue;
            } else if (ch == (unsigned char)'\n') {
                continue;
            }
            (void)append_pending_byte(pending, ch);
        } else {
            if (ch == (unsigned char)'(') depth += 1;
            if (ch == (unsigned char)')') depth -= 1;
            if (depth > 0) (void)append_pending_byte(pending, ch);
        }
    }
    return offset;
}

static size_t append_hex_string(const unsigned char *data, size_t size, size_t offset, PdfBuffer *pending) {
    int high = -1;

    offset += 1U;
    while (offset < size && data[offset] != (unsigned char)'>') {
        int value;

        if (pdf_is_space(data[offset])) {
            offset += 1U;
            continue;
        }
        value = tool_hex_value(data[offset]);
        if (value < 0) break;
        if (high < 0) high = value;
        else {
            unsigned char ch = (unsigned char)((high << 4) | value);
            (void)append_pending_byte(pending, ch);
            high = -1;
        }
        offset += 1U;
    }
    if (high >= 0) {
        unsigned char ch = (unsigned char)(high << 4);
        (void)append_pending_byte(pending, ch);
    }
    if (offset < size && data[offset] == (unsigned char)'>') offset += 1U;
    return offset;
}

static int token_is_text_show(const unsigned char *data, size_t start, size_t end) {
    size_t length = end - start;

    return (length == 2U && data[start] == (unsigned char)'T' && (data[start + 1U] == (unsigned char)'j' || data[start + 1U] == (unsigned char)'J'));
}

static int token_is_text_scope(const unsigned char *data, size_t start, size_t end) {
    size_t length = end - start;

    return (length == 2U && (data[start] == (unsigned char)'B' || data[start] == (unsigned char)'E') && data[start + 1U] == (unsigned char)'T');
}

static int emit_pending(PdfGrepContext *context, const PdfObjectSpan *object, PdfBuffer *pending) {
    size_t match_offset = 0U;
    size_t pattern_size = rt_strlen(context->pattern);

    if (pending->size == 0U) return 0;
    if (!find_text(pending->data, pending->size, context->pattern, context->ignore_case, &match_offset)) {
        pending->size = 0U;
        if (pending->data != 0) pending->data[0] = 0U;
        return 0;
    }
    context->matched = 1;
    if (!context->quiet && !context->list_files) {
        if (context->multiple) {
            rt_write_cstr(1, context->path ? context->path : "stdin");
            rt_write_char(1, ':');
        }
        if (context->object_number) {
            rt_write_uint(1, object->number);
            rt_write_char(1, ':');
        }
        if (context->have_context) {
            size_t start = match_offset > (size_t)context->context_chars ? match_offset - (size_t)context->context_chars : 0U;
            size_t end = match_offset + pattern_size;

            if (end < match_offset) end = pending->size;
            if (pending->size - end > (size_t)context->context_chars) end += (size_t)context->context_chars;
            else end = pending->size;
            if (start != 0U) rt_write_cstr(1, "...");
            (void)rt_write_all(1, pending->data + start, end - start);
            if (end != pending->size) rt_write_cstr(1, "...");
        } else {
            (void)rt_write_all(1, pending->data, pending->size);
        }
        rt_write_char(1, '\n');
    }
    pending->size = 0U;
    if (pending->data != 0) pending->data[0] = 0U;
    return context->quiet || context->list_files ? 1 : 0;
}

static int scan_text_stream(PdfGrepContext *context, const PdfObjectSpan *object, const unsigned char *data, size_t size) {
    PdfBuffer pending;
    size_t offset = 0U;
    int stop = 0;

    pdf_buffer_init(&pending);
    while (offset < size && !stop) {
        if (pdf_is_space(data[offset])) {
            offset += 1U;
        } else if (data[offset] == (unsigned char)'%') {
            while (offset < size && data[offset] != (unsigned char)'\n' && data[offset] != (unsigned char)'\r') offset += 1U;
        } else if (data[offset] == (unsigned char)'(') {
            offset = append_literal(data, size, offset, &pending);
        } else if (data[offset] == (unsigned char)'<' && offset + 1U < size && data[offset + 1U] != (unsigned char)'<') {
            offset = append_hex_string(data, size, offset, &pending);
        } else if (data[offset] == (unsigned char)'/') {
            offset += 1U;
            while (offset < size && !pdf_is_delim(data[offset])) offset += 1U;
        } else if (data[offset] == (unsigned char)'\'' || data[offset] == (unsigned char)'"') {
            stop = emit_pending(context, object, &pending);
            offset += 1U;
        } else if (pdf_is_delim(data[offset])) {
            offset += 1U;
        } else {
            size_t start = offset;

            while (offset < size && !pdf_is_delim(data[offset])) offset += 1U;
            if (token_is_text_show(data, start, offset)) {
                stop = emit_pending(context, object, &pending);
            } else if (token_is_text_scope(data, start, offset)) {
                pending.size = 0U;
                if (pending.data != 0) pending.data[0] = 0U;
            }
        }
    }
    pdf_buffer_free(&pending);
    return stop;
}

static int process_path(const char *path, PdfGrepContext *context) {
    unsigned char *data;
    size_t size;
    PdfDocument document;
    size_t index;

    if (tool_read_all_input(path, &data, &size) != 0) return 2;
    if (pdf_document_scan_with_options(data, size, &document, 0U) != 0) {
        tool_write_error("pdfgrep", "not a readable PDF: ", path ? path : "stdin");
        rt_free(data);
        return 2;
    }
    context->path = path;
    context->matched = 0;
    for (index = 0U; index < document.objects_len; ++index) {
        PdfObjectSpan *object = &document.objects[index];
        PdfBuffer stream;
        int result;

        if (object->stream_offset >= document.size) continue;
        pdf_buffer_init(&stream);
        result = pdf_object_stream_data(&document, object, 1, &stream);
        if (result == 0) {
            if (scan_text_stream(context, object, stream.data, stream.size)) {
                pdf_buffer_free(&stream);
                break;
            }
        }
        pdf_buffer_free(&stream);
    }
    if (context->matched && context->list_files && !context->quiet) rt_write_line(1, path ? path : "stdin");
    pdf_document_free(&document);
    rt_free(data);
    return context->matched ? 0 : 1;
}

int main(int argc, char **argv) {
    PdfGrepContext context;
    int index;
    int first_path;
    int status = 1;
    int had_error = 0;
    int path_count;

    rt_memset(&context, 0, sizeof(context));
    context.object_number = 1;
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
        if (rt_strcmp(arg, "-i") == 0 || rt_strcmp(arg, "--ignore-case") == 0) context.ignore_case = 1;
        else if (rt_strcmp(arg, "-l") == 0 || rt_strcmp(arg, "--files-with-matches") == 0) context.list_files = 1;
        else if (rt_strcmp(arg, "-q") == 0 || rt_strcmp(arg, "--quiet") == 0) context.quiet = 1;
        else if (rt_strcmp(arg, "-n") == 0 || rt_strcmp(arg, "--object-number") == 0) context.object_number = 1;
        else if (rt_strcmp(arg, "-C") == 0 || rt_strcmp(arg, "--context") == 0) {
            if (index + 1 >= argc || parse_context_count(argv[index + 1], &context.context_chars) != 0) {
                tool_write_error("pdfgrep", "invalid context count: ", index + 1 < argc ? argv[index + 1] : "");
                print_usage();
                return 2;
            }
            context.have_context = 1;
            index += 1;
        } else if (starts_with(arg, "--context=")) {
            if (parse_context_count(arg + 10, &context.context_chars) != 0) {
                tool_write_error("pdfgrep", "invalid context count: ", arg + 10);
                print_usage();
                return 2;
            }
            context.have_context = 1;
        }
        else if (arg[0] == '-' && rt_strcmp(arg, "-") != 0) {
            tool_write_error("pdfgrep", "unknown option: ", arg);
            print_usage();
            return 2;
        } else {
            break;
        }
    }
    if (index >= argc) {
        print_usage();
        return 2;
    }
    context.pattern = argv[index++];
    first_path = index;
    path_count = argc - first_path;
    context.multiple = path_count > 1;
    if (path_count == 0) return process_path(0, &context);
    for (index = first_path; index < argc; ++index) {
        int result = process_path(argv[index], &context);

        if (result == 0) status = 0;
        else if (result == 2) had_error = 1;
    }
    return had_error ? 2 : status;
}
